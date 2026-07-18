#!/usr/bin/env python3
"""Generate a native Nintendo DS static-recompilation project."""

from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import shutil
import struct
import sys
import tempfile
from dataclasses import asdict, dataclass, replace
from pathlib import Path


TEMPLATE_DIR = Path(__file__).with_name("template")
GENERATOR_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


@dataclass(frozen=True)
class Program:
    rom_offset: int
    entry_address: int
    ram_address: int
    size: int


@dataclass(frozen=True)
class RomInfo:
    title: str
    game_code: str
    maker_code: str
    unit_code: int
    device_capacity: int
    rom_size: int
    sha256: str
    arm9: Program
    arm7: Program
    arm9_overlay_offset: int
    arm9_overlay_size: int
    arm7_overlay_offset: int
    arm7_overlay_size: int
    arm9_build_info_offset: int
    arm7_build_info_offset: int


@dataclass(frozen=True)
class Overlay:
    ram_address: int
    ram_size: int
    static_init_start: int
    static_init_end: int
    rom_offset: int
    rom_size: int


def read_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def read_program(header: bytes, offset: int) -> Program:
    return Program(*struct.unpack_from("<4I", header, offset))


def valid_autoload_params(image: bytes, base: int, offset: int) -> bool:
    if not 0 <= offset <= len(image) - 12:
        return False
    table_start, table_end, source = struct.unpack_from("<3I", image, offset)
    if not (base <= table_start < table_end <= base + len(image)):
        return False
    if (table_end - table_start) % 12 != 0 or table_end - table_start > 0x1000:
        return False
    if not base <= source <= base + len(image):
        return False
    for descriptor in range(table_start - base, table_end - base, 12):
        size = read_u32(image, descriptor + 4)
        if source + size > base + len(image):
            return False
        source += size
    return True


def find_module_params_offset(
    image: bytes, base: int, hook_address: int, direct_offset: int
) -> int:
    magic = struct.pack("<II", 0xDEC00621, 0x2106C0DE)
    candidates = [direct_offset]
    pointer_offset = hook_address - 4 - base
    if 0 <= pointer_offset <= len(image) - 4:
        candidates.append(read_u32(image, pointer_offset) - base)
    signature = image.find(magic)
    if signature >= 28:
        candidates.append(signature - 28)
    for offset in candidates:
        if 0 <= offset <= len(image) - 36 and image[offset + 28 : offset + 36] == magic:
            return offset
    # Older ARM7 SDK startup records omit the magic words. Their startup code
    # still contains a pointer to the 12-byte-descriptor autoload parameters.
    for pointer in range(0, min(len(image), 0x400), 4):
        offset = read_u32(image, pointer) - base
        if valid_autoload_params(image, base, offset):
            return offset
    return 0


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_rom(path: Path) -> RomInfo:
    size = path.stat().st_size
    if size < 0x200:
        raise ValueError("ROM is smaller than the 0x200-byte Nintendo DS header")

    with path.open("rb") as rom:
        header = rom.read(0x200)

    arm9 = read_program(header, 0x20)
    arm7 = read_program(header, 0x30)
    for name, program in (("ARM9", arm9), ("ARM7", arm7)):
        if program.size == 0:
            raise ValueError(f"{name} program is empty")
        if program.rom_offset + program.size > size:
            raise ValueError(f"{name} program extends beyond the ROM")
        if not program.ram_address <= program.entry_address < program.ram_address + program.size:
            raise ValueError(f"{name} entry point is outside its loaded program")

    def text(start: int, length: int) -> str:
        raw = header[start : start + length].split(b"\0", 1)[0]
        return raw.decode("ascii", "replace").strip()

    game_code = text(0x0C, 4)
    if len(game_code) != 4:
        raise ValueError("ROM has no four-character game code")

    info = RomInfo(
        title=text(0, 12) or "UNTITLED",
        game_code=game_code,
        maker_code=text(0x10, 2),
        unit_code=header[0x12],
        device_capacity=header[0x14],
        rom_size=size,
        sha256=digest_file(path),
        arm9=arm9,
        arm7=arm7,
        arm9_overlay_offset=read_u32(header, 0x50),
        arm9_overlay_size=read_u32(header, 0x54),
        arm7_overlay_offset=read_u32(header, 0x58),
        arm7_overlay_size=read_u32(header, 0x5C),
        arm9_build_info_offset=read_u32(header, 0x88),
        arm7_build_info_offset=read_u32(header, 0x8C),
    )
    with path.open("rb") as rom:
        rom.seek(arm9.rom_offset)
        arm9_image = rom.read(arm9.size)
        rom.seek(arm7.rom_offset)
        arm7_image = rom.read(arm7.size)
    return replace(
        info,
        arm9_build_info_offset=find_module_params_offset(
            arm9_image, arm9.ram_address, read_u32(header, 0x70),
            info.arm9_build_info_offset,
        ),
        arm7_build_info_offset=find_module_params_offset(
            arm7_image, arm7.ram_address, read_u32(header, 0x74),
            info.arm7_build_info_offset,
        ),
    )


def read_segment(path: Path, program: Program) -> bytes:
    with path.open("rb") as rom:
        rom.seek(program.rom_offset)
        data = rom.read(program.size)
    if len(data) != program.size:
        raise ValueError("ROM ended while reading a CPU program")
    return data


def read_overlay_table(path: Path, table_offset: int, table_size: int) -> list[Overlay]:
    if table_size == 0:
        return []
    if table_size % 32 != 0:
        raise ValueError("overlay table size is not a multiple of 32 bytes")
    rom_size = path.stat().st_size
    with path.open("rb") as rom:
        header = rom.read(0x50)
        fat_offset, fat_size = struct.unpack_from("<II", header, 0x48)
        if table_offset + table_size > rom_size:
            raise ValueError("overlay table extends beyond the ROM")
        rom.seek(table_offset)
        table = rom.read(table_size)
        overlays: list[Overlay] = []
        for offset in range(0, table_size, 32):
            _, ram_address, ram_size, _, init_start, init_end, file_id, _ = struct.unpack_from(
                "<8I", table, offset
            )
            if file_id * 8 + 8 > fat_size:
                raise ValueError("overlay file ID is outside the FAT")
            rom.seek(fat_offset + file_id * 8)
            file_start, file_end = struct.unpack("<II", rom.read(8))
            if not file_start <= file_end <= rom_size:
                raise ValueError("overlay file extends beyond the ROM")
            overlays.append(Overlay(
                ram_address, ram_size, init_start, init_end,
                file_start, file_end - file_start,
            ))
    return overlays


def read_overlay_image(path: Path, overlay: Overlay) -> bytes:
    with path.open("rb") as rom:
        rom.seek(overlay.rom_offset)
        data = rom.read(overlay.rom_size)
    image, _ = decompress_arm9(data)
    if len(image) > overlay.ram_size:
        raise ValueError("expanded overlay is larger than its declared RAM size")
    return image


def write_text_if_changed(output: Path, content: str) -> None:
    if output.exists() and output.read_text(encoding="utf-8") == content:
        return
    output.write_text(content, encoding="utf-8", newline="\n")


def write_bytes_if_changed(output: Path, content: bytes) -> None:
    if output.exists() and output.read_bytes() == content:
        return
    output.write_bytes(content)


def sync_template(output: Path) -> None:
    """Copy runtime sources without touching unchanged mtimes.

    A normal copytree refresh makes every dependent object look newer after
    each `create`, even when the template bytes did not change.  Generated
    projects are large enough that preserving mtimes is material to rebuild
    time, so use the same content-aware write path as generated files.
    """
    for source in TEMPLATE_DIR.rglob("*"):
        target = output / source.relative_to(TEMPLATE_DIR)
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif source.is_file():
            write_bytes_if_changed(target, source.read_bytes())
            source_mode = source.stat().st_mode & 0o777
            if target.stat().st_mode & 0o777 != source_mode:
                target.chmod(source_mode)


def write_byte_array(output: Path, name: str, data: bytes) -> None:
    lines = ['#include "rom_data.h"', "", f"const uint8_t {name}[] = {{"]
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{value:02x}" for value in data[offset : offset + 16])
        lines.append(f"    {values},")
    lines.extend(("};", f"const size_t {name}_size = sizeof({name});", ""))
    write_text_if_changed(output, "\n".join(lines))


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def rotate_right(value: int, amount: int) -> int:
    amount &= 31
    return value if amount == 0 else (value >> amount) | (value << (32 - amount) & 0xFFFFFFFF)


def arm_immediate(word: int) -> int:
    return rotate_right(word & 0xFF, ((word >> 8) & 0xF) * 2)


def decompress_arm9(data: bytes) -> tuple[bytes, bool]:
    """Expand the backwards LZ stream produced by Nintendo's ARM9 linker."""
    if len(data) < 8:
        return data, False
    footer, extra = struct.unpack_from("<II", data, len(data) - 8)
    header_size = footer >> 24
    compressed_size = footer & 0xFFFFFF
    if not (8 <= header_size <= 11 and header_size <= compressed_size <= len(data)):
        return data, False
    if extra == 0 or extra > 64 * 1024 * 1024:
        return data, False

    source = len(data) - header_size
    source_bottom = len(data) - compressed_size
    destination = len(data) + extra
    expanded = bytearray(data)
    expanded.extend(b"\0" * extra)

    try:
        while source > source_bottom:
            source -= 1
            flags = expanded[source]
            for _ in range(8):
                if source <= source_bottom or destination <= len(data) - compressed_size:
                    break
                if flags & 0x80:
                    source -= 1
                    first = expanded[source]
                    source -= 1
                    second = expanded[source]
                    distance = (((first & 0x0F) << 8) | second) + 2
                    length = (first >> 4) + 3
                    for _ in range(length):
                        if destination == 0 or destination + distance >= len(expanded):
                            raise ValueError("ARM9 compressed stream has an invalid back-reference")
                        value = expanded[destination + distance]
                        destination -= 1
                        expanded[destination] = value
                else:
                    source -= 1
                    destination -= 1
                    expanded[destination] = expanded[source]
                flags = (flags << 1) & 0xFF
    except IndexError as error:
        raise ValueError("ARM9 compressed stream is truncated") from error

    if source != source_bottom or destination != source_bottom:
        raise ValueError("ARM9 compressed stream did not expand to its declared boundary")
    return bytes(expanded), True


def translated_body(
    pc: int,
    word: int,
    indent: str = "            ",
    terminal: str = "return NDS_RUN_BUDGET_EXHAUSTED;",
) -> list[str]:
    next_pc = pc + 4
    condition = word >> 28
    lines: list[str] = []
    if word & 0xFE000000 == 0xFA000000:  # BLX immediate
        offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
        target = (pc + 8 + offset) & 0xFFFFFFFF
        lines.append(f"{indent}nds_branch_link_exchange_immediate(cpu, 0x{target:08x}u, 0x{next_pc:08x}u);")
        lines.append(f"{indent}{terminal}")
        return lines
    if condition != 0xE:
        if condition == 0xF:
            lines.append(f"{indent}return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
            return lines
        lines.extend(
            (
                f"{indent}if (!nds_condition(cpu, 0x{condition:x}u)) {{",
                f"{indent}    cpu->r[15] = 0x{next_pc:08x}u;",
                f"{indent}    {terminal}",
                f"{indent}}}",
            )
        )

    if word & 0x0FFFFFF0 == 0x012FFF10:  # BX
        rm = word & 0xF
        lines.append(f"{indent}if (!nds_branch_exchange(cpu, {rm}u, false, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0FFFFFF0 == 0x012FFF30:  # BLX register
        rm = word & 0xF
        lines.append(f"{indent}if (!nds_branch_exchange(cpu, {rm}u, true, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0E000000 == 0x0A000000:  # B / BL
        target = (pc + 8 + sign_extend(word & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
        if word & (1 << 24):
            lines.append(f"{indent}cpu->r[14] = 0x{next_pc:08x}u;")
        lines.append(f"{indent}cpu->r[15] = 0x{target:08x}u;")
    elif word & 0x0FC000F0 == 0x00000090:  # MUL / MLA
        lines.append(f"{indent}if (!nds_exec_multiply(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0F8000F0 == 0x00800090:  # UMULL/UMLAL/SMULL/SMLAL
        lines.append(f"{indent}if (!nds_exec_long_multiply(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0E000090 == 0x00000090:  # Halfword/signed transfer
        lines.append(f"{indent}if (!nds_exec_half_transfer(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0FFF0FF0 == 0x016F0F10:  # CLZ
        lines.append(f"{indent}nds_exec_clz(cpu, 0x{word:08x}u, pc);")
    elif (
        word & 0x0FBF0FFF == 0x010F0000
        or word & 0x0FB0FFF0 == 0x0120F000
        or word & 0x0FB0F000 == 0x0320F000
    ):
        lines.append(f"{indent}if (!nds_exec_status(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0C000000 == 0x00000000:  # Data processing
        lines.append(f"{indent}if (!nds_exec_data_processing(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0C000000 == 0x04000000:  # Single transfer
        lines.append(f"{indent}if (!nds_exec_single_transfer(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0E000000 == 0x08000000:  # Block transfer
        lines.append(f"{indent}if (!nds_exec_block_transfer(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0F000010 == 0x0E000010:  # MRC / MCR
        lines.append(f"{indent}if (!nds_exec_coprocessor(cpu, 0x{word:08x}u, pc))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    elif word & 0x0F000000 == 0x0F000000:  # SWI
        lines.append(f"{indent}if (!nds_exec_swi(cpu, 0x{word & 0xFFFFFF:06x}u, pc, pc + 4u))")
        lines.append(f"{indent}    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    else:
        lines.append(f"{indent}return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);")
    lines.append(f"{indent}{terminal}")
    return lines


def translated_case(pc: int, word: int) -> list[str]:
    lines = [f"        case 0x{pc:08x}u:"]
    lines.extend(
        (
            f"            if (word != 0x{word:08x}u) {{",
            "                if (!nds_exec_arm(cpu, word, pc))",
            "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);",
            "                return NDS_RUN_BUDGET_EXHAUSTED;",
            "            }",
        )
    )
    lines.extend(translated_body(pc, word))
    return lines


def image_word(image: bytes, base: int, address: int) -> int | None:
    offset = address - base
    if offset < 0 or offset + 4 > len(image) or offset & 3:
        return None
    return read_u32(image, offset)


def image_half(image: bytes, base: int, address: int) -> int | None:
    offset = address - base
    if offset < 0 or offset + 2 > len(image) or offset & 1:
        return None
    return struct.unpack_from("<H", image, offset)[0]


def literal_value(image: bytes, base: int, pc: int, word: int, register: int) -> int | None:
    if word >> 28 != 0xE or word & 0x0F7F0000 != 0x051F0000:
        return None
    if (word >> 12) & 0xF != register:
        return None
    offset = word & 0xFFF
    address = pc + 8 + offset if word & (1 << 23) else pc + 8 - offset
    return image_word(image, base, address)


def find_static_copies(
    image: bytes, base: int, start: int, destination_start: int, destination_end: int
) -> list[dict[str, int]]:
    """Find an SDK startup's small word-copy loops into local CPU memory."""
    stop = min(base + len(image), start + 0x800)
    copies: list[dict[str, int]] = []
    for pc in range(start, stop - 24, 4):
        words = [image_word(image, base, pc + index * 4) for index in range(7)]
        if any(word is None for word in words):
            continue
        source = literal_value(image, base, pc, words[0], 1)
        destination = literal_value(image, base, pc + 4, words[1], 2)
        if source is None or destination is None:
            continue
        if words[2] & 0x0FFFF000 != 0x02823000:
            continue
        size = arm_immediate(words[2])
        if (
            words[3] & 0x0FFFFFFF != 0x04910004
            or words[4] & 0x0FFFFFFF != 0x04820004
            or words[5] & 0x0FFFFFFF != 0x01520003
            or words[6] >> 28 != 0xB
        ):
            continue
        if not (0 < size <= 32 * 1024 and size & 3 == 0):
            continue
        if not (base <= source and source + size <= base + len(image)):
            continue
        if not (destination_start <= destination and destination + size <= destination_end):
            continue
        copies.append({"source": source, "destination": destination, "size": size})
    return copies


def find_stack_trampoline_copies(
    image: bytes, base: int, start: int, destination_start: int, destination_end: int
) -> list[dict[str, int]]:
    """Find the SDK's two-word ARM trampoline copied onto the startup stack."""
    stop = min(base + len(image), start + 0x800)
    for pc in range(start + 4, stop - 36, 4):
        source_word = image_word(image, base, pc - 4)
        source = (
            literal_value(image, base, pc - 4, source_word, 1)
            if source_word is not None else None
        )
        words = [image_word(image, base, pc + index * 4) for index in range(10)]
        if source is None or words[:4] != [0xE4910004, 0xE50D0004, 0xE5910000, 0xE58D0000]:
            continue
        if words[8:] != [0xE24D3004, 0xE12FFF13]:
            continue
        for setup in range(max(start, pc - 0x100), pc - 8, 4):
            stack_top_word = image_word(image, base, setup)
            stack_size_word = image_word(image, base, setup + 4)
            stack_top = literal_value(image, base, setup, stack_top_word, 13)
            stack_size = literal_value(image, base, setup + 4, stack_size_word, 1)
            if stack_top is None or stack_size is None:
                continue
            if image_word(image, base, setup + 8) != 0xE04D1001:
                continue
            for finish in range(setup + 12, pc, 4):
                word = image_word(image, base, finish)
                if word is None or word & 0x0FFFF000 != 0x0241D000:
                    continue
                destination = stack_top - stack_size - arm_immediate(word) - 4
                if (
                    base <= source and source + 8 <= base + len(image)
                    and destination_start <= destination
                    and destination + 8 <= destination_end
                ):
                    return [{"source": source, "destination": destination, "size": 8}]
    return []


def find_autoload_copies(
    image: bytes, base: int, build_info_offset: int, descriptor_size: int
) -> list[dict[str, int]]:
    build_info = base + build_info_offset
    table_start = image_word(image, base, build_info)
    table_end = image_word(image, base, build_info + 4)
    source = image_word(image, base, build_info + 8)
    if table_start is None or table_end is None or source is None:
        return []
    if not (base <= table_start <= table_end <= base + len(image)):
        return []
    if (table_end - table_start) % descriptor_size != 0:
        return []

    copies: list[dict[str, int]] = []
    for descriptor in range(table_start, table_end, descriptor_size):
        destination = image_word(image, base, descriptor)
        size = image_word(image, base, descriptor + 4)
        if destination is None or size is None or size > 16 * 1024 * 1024:
            return []
        if source + size > base + len(image):
            return []
        if size != 0:
            copies.append({"source": source, "destination": destination, "size": size & ~3})
        source += size
    return copies


def arm_successors(pc: int, word: int) -> tuple[int, ...]:
    condition = word >> 28
    next_pc = pc + 4
    conditional = condition not in (0xE, 0xF)
    if word & 0x0FFFFFF0 == 0x012FFF10:  # BX
        return (next_pc,) if conditional else ()
    if word & 0x0FFFFFF0 == 0x012FFF30:  # BLX register
        return (next_pc,)
    if word & 0x0E000000 == 0x0A000000:  # B / BL
        target = (pc + 8 + sign_extend(word & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
        if word & (1 << 24) or conditional:
            return next_pc, target
        return (target,)

    if word & 0x0C000000 == 0x00000000:
        opcode = (word >> 21) & 0xF
        rd = (word >> 12) & 0xF
        writes_result = opcode not in (8, 9, 10, 11)
        if writes_result and rd == 15:
            return (next_pc,) if conditional else ()
    elif word & 0x0C000000 == 0x04000000:
        if word & (1 << 20) and (word >> 12) & 0xF == 15:
            return (next_pc,) if conditional else ()
    elif word & 0x0E000000 == 0x08000000:
        if word & (1 << 20) and word & 0x8000:
            return (next_pc,) if conditional else ()
    return (next_pc,)


def arm_jump_table_targets(image: bytes, base: int, pc: int, word: int) -> tuple[int, ...]:
    if word & 0x0FFFFFF0 != 0x008FF100:  # ADDcc pc, pc, Rm, LSL #2
        return ()
    register = word & 15
    count = 0
    while count < 256:
        entry = image_word(image, base, pc + 8 + count * 4)
        if entry is None or entry & 0xFF000000 != 0xEA000000:
            break
        count += 1
    if count == 0:
        previous = image_word(image, base, pc - 4)
        scaled_index = (
            0xE0800080 | (register << 16) | (register << 12) | register
        )
        if previous == scaled_index:  # Compiler division helper enters an unrolled loop.
            return (pc + 8,)
        if (previous is None or previous & 0x0FF00000 != 0x03500000
                or (previous >> 16) & 15 != register):
            return ()
        count = arm_immediate(previous) + 1
        if not 1 <= count <= 256:
            return ()
    return tuple(pc + 8 + index * 4 for index in range(count))


def arm_function_table_targets(image: bytes, base: int, pc: int, word: int) -> tuple[tuple[int, bool], ...]:
    if word & 0x0F7F0000 != 0x051F0000:
        return ()
    register = (word >> 12) & 15
    table = literal_value(image, base, pc, word, register)
    indexed_load = image_word(image, base, pc + 4)
    if table is None or indexed_load is None:
        return ()
    required = (1 << 25) | (1 << 24) | (1 << 23) | (1 << 20)
    transfer_mask = (1 << 25) | (1 << 24) | (1 << 23) | (1 << 22) | (1 << 21) | (1 << 20)
    if indexed_load & transfer_mask != required:
        return ()
    if (indexed_load >> 16) & 15 != register or (indexed_load >> 12) & 15 != register:
        return ()
    if indexed_load & 0xFF0 != 0x100:  # index register, LSL #2
        return ()
    if not any(
        (candidate := image_word(image, base, pc + distance * 4)) is not None
        and candidate & 0x0FFFFFF0 == 0x012FFF30
        and candidate & 15 == register
        for distance in range(2, 6)
    ):
        return ()

    targets: list[tuple[int, bool]] = []
    for index in range(256):
        target = image_word(image, base, table + index * 4)
        if target is None or not (base <= (target & ~1) < base + len(image)):
            break
        targets.append((target & ~1, bool(target & 1)))
    return tuple(targets)


def plausible_function(image: bytes, base: int, target: int, tail: int = 32) -> bool:
    address = target & ~1
    if target & 1:
        half = image_half(image, base, address)
        if half is not None and (
            half & 0xFE00 == 0xB400 or half & 0xF800 == 0x4800 or half == 0x4770
        ):
            return True
        return any(image_half(image, base, address + offset) == 0x4770 for offset in range(0, tail, 2))
    word = image_word(image, base, address)
    if word is not None and (
        word & 0xFFFF0000 == 0xE92D0000
        or word >> 28 == 0xE and word & 0x0F7F0000 == 0x051F0000
        or word & 0xFFFFFFF0 == 0xE12FFF10
    ):
        return True
    return any(
        (image_word(image, base, address + offset) or 0) & 0xFFFFFFF0 ==
        0xE12FFF10
        for offset in range(0, tail, 4)
    )


def find_function_roots(image: bytes, base: int, tail: int = 32) -> list[tuple[int, bool]]:
    roots: set[tuple[int, bool]] = set()
    run: list[tuple[int, bool]] = []
    anchored = False
    for offset in range(0, len(image) - 3, 4):
        value = read_u32(image, offset)
        if base <= (value & ~1) < base + len(image):
            if plausible_function(image, base, value, tail):
                roots.add((value & ~1, bool(value & 1)))
            run.append((value & ~1, bool(value & 1)))
            anchored = anchored or plausible_function(image, base, value, tail)
        else:
            if len(run) >= 2 and anchored:
                roots.update(run)
            run = []
            anchored = False
    if len(run) >= 2 and anchored:
        roots.update(run)
    return sorted(roots)


def short_linear_arm_function(
    image: bytes, base: int, entry: int, max_instructions: int = 16
) -> dict[int, int] | None:
    """Capture tiny overlay functions missed by the normal root heuristic."""
    translated: dict[int, int] = {}
    for index in range(max_instructions):
        pc = entry + index * 4
        word = image_word(image, base, pc)
        if word is None:
            return None
        translated[pc] = word
        if word & 0xFFFFFFF0 == 0xE12FFF10:  # Unconditional BX tail call.
            return translated
        if word & 0x0FFFFFFF == 0x012FFF1E:  # Conditional BX LR falls through.
            continue
        if (word & 0x0E000000 == 0x0A000000 or
                word & 0x0FFFFFF0 in (0x012FFF10, 0x012FFF30)):
            return None
    return None


def add_adjacent_short_arm_functions(
    arm: dict[int, int], thumb: dict[int, int], image: bytes, base: int,
    limit: int = 0,
) -> int:
    """Capture unreferenced compiler leaf functions between translated siblings."""
    before = len(arm)
    addresses = sorted(arm)
    for lower, upper in zip(addresses, addresses[1:]):
        if not 8 <= upper - lower <= 64:
            continue
        for pc in range(lower + 4, upper, 4):
            word = image_word(image, base, pc)
            if (word is None or word >> 28 != 0xE or
                    word & 0x0F7F0000 != 0x051F0000):
                continue
            function = short_linear_arm_function(image, base, pc)
            if function is None or max(function) >= upper:
                continue
            if limit and len(arm) + len(thumb) + len(function) > limit:
                continue
            arm.update(function)
    return len(arm) - before


def add_address_taken_functions(
    arm: dict[int, int], thumb: dict[int, int], image: bytes, base: int,
    limit: int = 0,
) -> int:
    before = len(arm) + len(thumb)
    roots = deque(sorted(
        set(find_function_roots(image, base)) |
        {
            (base + offset, False)
            for offset in range(0, len(image) - 3, 4)
            if read_u32(image, offset) & 0xFFFF0000 == 0xE92D0000
        }
    ))
    while roots and (limit == 0 or len(arm) + len(thumb) < limit):
        root = roots.popleft()
        pending = deque([root])
        while pending and (limit == 0 or len(arm) + len(thumb) < limit):
            pc, is_thumb = pending.popleft()
            selected = thumb if is_thumb else arm
            if pc in selected:
                continue
            if is_thumb:
                half = image_half(image, base, pc)
                if half is None:
                    continue
                thumb[pc] = half
                pending.extend(thumb_successors(image, base, pc, half))
            else:
                word = image_word(image, base, pc)
                if word is None:
                    continue
                arm[pc] = word
                if word & 0xFE000000 == 0xFA000000:
                    offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
                    target = (pc + 8 + offset) & 0xFFFFFFFF
                    pending.append((pc + 4, False))
                    pending.append((target, True))
                    roots.append((target, True))
                else:
                    pending.extend(arm_function_table_targets(image, base, pc, word))
                    successors = arm_successors(pc, word) + arm_jump_table_targets(
                        image, base, pc, word
                    )
                    if word & 0x0F000000 == 0x0B000000:
                        roots.append((successors[-1], False))
                    if (word & 0x0FFFFFF0 == 0x012FFF10 and
                            image_word(image, base, pc - 4) == 0xE1A0E00F):
                        successors += (pc + 4,)
                    pending.extend((successor, False) for successor in successors)
    return len(arm) + len(thumb) - before


def thumb_successors(image: bytes, base: int, pc: int, half: int) -> tuple[tuple[int, bool], ...]:
    next_pc = pc + 2
    if half & 0xFF00 == 0x4700:  # BX / BLX register
        return ((next_pc, True),) if half & 0x0080 else ()
    if half & 0xFF00 == 0xBD00 and half & 0x0100:  # POP {..., pc}
        return ()
    if half & 0xF000 == 0xD000:
        condition = (half >> 8) & 0xF
        if condition < 0xE:
            target = pc + 4 + sign_extend(half & 0xFF, 8) * 2
            return (next_pc, True), (target, True)
        return ((next_pc, True),) if condition == 0xF and (half & 0xFF) in (3, 0x0B, 0x0C) else ()
    if half & 0xF800 == 0xE000:
        return ((pc + 4 + sign_extend(half & 0x7FF, 11) * 2, True),)
    if half & 0xF800 == 0xF000:  # BL prefix
        return ((next_pc, True),)
    if half & 0xF800 in (0xF800, 0xE800):  # BL / BLX suffix
        prefix = image_half(image, base, pc - 2)
        if prefix is not None and prefix & 0xF800 == 0xF000:
            high = sign_extend(prefix & 0x7FF, 11) << 12
            target = pc + 2 + high + ((half & 0x7FF) << 1)
            thumb = half & 0xF800 == 0xF800
            return (next_pc, True), (target & (~1 if thumb else ~3), thumb)
        return ((next_pc, True),)
    if half & 0xFC00 == 0x4400 and (half & 0x0300) != 0x0300:
        operation = (half >> 8) & 3
        source = (half >> 3) & 15
        rd = (half & 7) | ((half >> 4) & 8)
        if rd == 15:
            if operation == 0:
                for distance in range(1, 9):
                    compare = image_half(image, base, pc - distance * 2)
                    if compare is None:
                        break
                    if compare & 0xF800 == 0x2800 and (compare >> 8) & 7 == source:
                        count = (compare & 0xFF) + 1
                        if count <= 64:
                            targets = []
                            for index in range(count):
                                offset = image_half(image, base, pc + 2 + index * 2)
                                if offset is None:
                                    return ()
                                targets.append((pc + 4 + sign_extend(offset, 16), True))
                            return tuple(targets)
            return ()
    return ((next_pc, True),)


def reachable_instructions(
    image: bytes, base: int, entry: int, limit: int, entry_thumb: bool = False,
    extra_entries: tuple[tuple[int, bool], ...] = (),
) -> tuple[dict[int, int], dict[int, int]]:
    pending = deque(((entry, entry_thumb), *extra_entries))
    arm: dict[int, int] = {}
    thumb: dict[int, int] = {}
    while pending and (limit == 0 or len(arm) + len(thumb) < limit):
        pc, is_thumb = pending.popleft()
        selected = thumb if is_thumb else arm
        if pc in selected:
            continue
        if is_thumb:
            half = image_half(image, base, pc)
            if half is None:
                continue
            thumb[pc] = half
            if half & 0xF800 == 0x4800:  # LDR literal feeding a nearby BX/BLX.
                register = (half >> 8) & 7
                literal = ((pc + 4) & ~3) + (half & 0xFF) * 4
                value = image_word(image, base, literal)
                for distance in range(1, 4):
                    exchange = image_half(image, base, pc + distance * 2)
                    if exchange is None:
                        break
                    if exchange & 0xFF87 in (0x4700, 0x4780) and (exchange >> 3) & 15 == register:
                        if value is not None and base <= (value & ~1) < base + len(image):
                            pending.appendleft((value & ~1, bool(value & 1)))
                        break
            for successor in reversed(thumb_successors(image, base, pc, half)):
                pending.appendleft(successor)
            continue

        word = image_word(image, base, pc)
        if word is None:
            continue
        arm[pc] = word
        if word & 0xFE000000 == 0xFA000000:  # BLX immediate enters Thumb state.
            offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
            target = (pc + 8 + offset) & 0xFFFFFFFF
            pending.appendleft((target, True))
            pending.appendleft((pc + 4, False))
            continue
        if word & 0x0F7F0000 == 0x051F0000:  # Literal code pointers commonly feed BX.
            register = (word >> 12) & 15
            value = literal_value(image, base, pc, word, register)
            exchanged = False
            for distance in range(1, 9):
                exchange = image_word(image, base, pc + distance * 4)
                if exchange is None:
                    break
                if exchange & 0x0FFFFFF0 in (0x012FFF10, 0x012FFF30) and exchange & 15 == register:
                    if value is not None and base <= (value & ~1) < base + len(image):
                        pending.appendleft((value & ~1, bool(value & 1)))
                    exchanged = True
                    break
            if not exchanged and value is not None and plausible_function(image, base, value):
                for distance in range(1, 9):
                    candidate = image_word(image, base, pc + distance * 4)
                    if candidate is None:
                        break
                    if candidate & 0x0F000000 == 0x0B000000:
                        pending.appendleft((value & ~1, bool(value & 1)))
                        break
        for target in reversed(arm_function_table_targets(image, base, pc, word)):
            pending.appendleft(target)
        successors = arm_successors(pc, word) + arm_jump_table_targets(image, base, pc, word)
        if (word & 0x0FFFFFF0 == 0x012FFF10 and
                image_word(image, base, pc - 4) == 0xE1A0E00F):
            successors += (pc + 4,)
        for successor in reversed(successors):
            pending.appendleft((successor, False))
    add_address_taken_functions(arm, thumb, image, base, limit)
    add_adjacent_short_arm_functions(arm, thumb, image, base, limit)
    return arm, thumb


def external_targets(
    image: bytes, base: int, arm: dict[int, int], thumb: dict[int, int]
) -> set[tuple[int, bool]]:
    end = base + len(image)
    targets: set[tuple[int, bool]] = set()
    for pc, word in arm.items():
        if word & 0x0F7F0000 == 0x051F0000:
            register = (word >> 12) & 15
            value = literal_value(image, base, pc, word, register)
            for distance in range(1, 9):
                exchange = image_word(image, base, pc + distance * 4)
                if exchange is None:
                    break
                if (exchange & 0x0FFFFFF0 in (0x012FFF10, 0x012FFF30)
                        and exchange & 15 == register):
                    if value is not None and not base <= (value & ~1) < end:
                        targets.add((value & ~1, bool(value & 1)))
                    break
        if word & 0xFE000000 == 0xFA000000:
            offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
            target = ((pc + 8 + offset) & 0xFFFFFFFF, True)
            if not base <= target[0] < end:
                targets.add(target)
            continue
        for target in arm_successors(pc, word):
            if not base <= target < end:
                targets.add((target, False))
    for pc, half in thumb.items():
        if half & 0xF800 == 0x4800:
            register = (half >> 8) & 7
            literal = ((pc + 4) & ~3) + (half & 0xFF) * 4
            value = image_word(image, base, literal)
            for distance in range(1, 4):
                exchange = image_half(image, base, pc + distance * 2)
                if exchange is None:
                    break
                if (exchange & 0xFF87 in (0x4700, 0x4780)
                        and (exchange >> 3) & 15 == register):
                    if value is not None and not base <= (value & ~1) < end:
                        targets.add((value & ~1, bool(value & 1)))
                    break
        for target in thumb_successors(image, base, pc, half):
            if not base <= target[0] < end:
                targets.add(target)
    return targets


def copied_external_targets(
    image: bytes, base: int, copies: list[dict[str, int]]
) -> set[tuple[int, bool]]:
    targets: set[tuple[int, bool]] = set()
    for copy in copies:
        for offset in range(0, copy["size"], 4):
            pc = copy["destination"] + offset
            word = image_word(image, base, copy["source"] + offset)
            if word is None:
                continue
            if word & 0xFE000000 == 0xFA000000:
                displacement = sign_extend(
                    ((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26
                )
                targets.add(((pc + 8 + displacement) & 0xFFFFFFFF, True))
            elif word & 0x0E000000 == 0x0A000000:
                displacement = sign_extend(word & 0xFFFFFF, 24) * 4
                targets.add(((pc + 8 + displacement) & 0xFFFFFFFF, False))
        for offset in range(2, copy["size"], 2):
            pc = copy["destination"] + offset
            prefix = image_half(image, base, copy["source"] + offset - 2)
            half = image_half(image, base, copy["source"] + offset)
            if (prefix is None or half is None or prefix & 0xF800 != 0xF000
                    or half & 0xF800 not in (0xF800, 0xE800)):
                continue
            high = sign_extend(prefix & 0x7FF, 11) << 12
            target = pc + 2 + high + ((half & 0x7FF) << 1)
            is_thumb = half & 0xF800 == 0xF800
            targets.add((target & (~1 if is_thumb else ~3), is_thumb))
    return targets


def referenced_overlay_variants(
    rom_path: Path,
    overlays: list[Overlay],
    image: bytes,
    base: int,
    entry: int,
    instruction_limit: int,
    copies: list[dict[str, int]],
) -> list[tuple[dict[int, int], dict[int, int]]]:
    arm, thumb = reachable_instructions(image, base, entry, instruction_limit)
    overlay_images = [read_overlay_image(rom_path, overlay) for overlay in overlays]
    discovered = [({}, {}) for _ in overlays]
    pending = deque(sorted(
        external_targets(image, base, arm, thumb) |
        copied_external_targets(image, base, copies)
    ))
    main_arm: dict[int, int] = {}
    main_thumb: dict[int, int] = {}
    main_roots: set[tuple[int, bool]] = set()
    seen: set[tuple[int, bool]] = set()
    while pending or main_roots:
        if not pending:
            roots = [
                root for root in sorted(main_roots)
                if root[0] not in (main_thumb if root[1] else main_arm)
                and root[0] not in (thumb if root[1] else arm)
            ]
            main_roots.clear()
            if not roots:
                continue
            first, *rest = roots
            found_arm, found_thumb = reachable_instructions(
                image, base, first[0], 0, first[1], tuple(rest)
            )
            main_arm.update(found_arm)
            main_thumb.update(found_thumb)
            for exit_target in sorted(external_targets(
                image, base, found_arm, found_thumb
            )):
                if exit_target not in seen:
                    pending.append(exit_target)
            continue
        root = pending.popleft()
        if root in seen:
            continue
        seen.add(root)
        target, is_thumb = root
        if base <= target < base + len(image):
            if (target not in (main_thumb if is_thumb else main_arm)
                    and target not in (thumb if is_thumb else arm)):
                main_roots.add(root)
            continue
        for index, (overlay, overlay_image) in enumerate(zip(overlays, overlay_images, strict=True)):
            if not overlay.ram_address <= target < overlay.ram_address + len(overlay_image):
                continue
            overlay_arm, overlay_thumb = discovered[index]
            if target in (overlay_thumb if is_thumb else overlay_arm):
                continue
            roots = {root}
            if not overlay_arm and not overlay_thumb:
                for address in range(overlay.static_init_start, overlay.static_init_end, 4):
                    initializer = image_word(overlay_image, overlay.ram_address, address)
                    if (initializer is not None and
                            overlay.ram_address <= (initializer & ~1) <
                            overlay.ram_address + len(overlay_image)):
                        roots.add((initializer & ~1, bool(initializer & 1)))
            first, *rest = sorted(roots)
            found_arm, found_thumb = reachable_instructions(
                overlay_image, overlay.ram_address, first[0], 0, first[1], tuple(rest)
            )
            overlay_arm.update(found_arm)
            overlay_thumb.update(found_thumb)
            for exit_target in sorted(external_targets(
                overlay_image, overlay.ram_address, found_arm, found_thumb
            )):
                if exit_target not in seen:
                    pending.append(exit_target)

    variants = [(main_arm, main_thumb)] if main_arm or main_thumb else []
    variants.extend(variant for variant in discovered if variant[0] or variant[1])

    # Some games dispatch through function pointers in overlays loaded later,
    # so no main-image branch can identify their overlay. Keep the fallback
    # deliberately tiny: full root expansion for every overlay is enormous.
    for overlay, overlay_image in zip(overlays, overlay_images, strict=True):
        for target, is_thumb in find_function_roots(
            overlay_image, overlay.ram_address, 256
        ):
            if is_thumb:
                continue
            function = short_linear_arm_function(
                overlay_image, overlay.ram_address, target, 64
            )
            if function is not None:
                variants.append((function, {}))
    return variants


def write_translator(
    output: Path,
    name: str,
    image: bytes,
    base: int,
    entry: int,
    instruction_limit: int,
    copies: list[dict[str, int]],
    extra_variants: list[tuple[dict[int, int], dict[int, int]]],
) -> tuple[int, int, int]:
    arm, thumb = reachable_instructions(image, base, entry, instruction_limit)
    overlay_exits: set[tuple[int, bool]] = set()
    for extra_arm, extra_thumb in extra_variants:
        for pc, word in extra_arm.items():
            if word & 0xFE000000 == 0xFA000000:  # BLX immediate enters the main image in Thumb state.
                offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
                target = (pc + 8 + offset) & 0xFFFFFFFF
                if target not in extra_thumb:
                    overlay_exits.add((target, True))
            elif word & 0x0E000000 == 0x0A000000:
                target = (pc + 8 + sign_extend(word & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
                if target not in extra_arm:
                    overlay_exits.add((target, False))
    overlay_exits = {
        root for root in overlay_exits if base <= root[0] < base + len(image)
    }
    remaining = (
        instruction_limit - len(arm) - len(thumb) if instruction_limit else 0
    )
    if overlay_exits and (instruction_limit == 0 or remaining > 0):
        first, *rest = overlay_exits
        found_arm, found_thumb = reachable_instructions(
            image, base, first[0], remaining, first[1], tuple(rest)
        )
        arm.update(found_arm)
        thumb.update(found_thumb)
    aliases: dict[int, dict[int, int]] = {}
    thumb_aliases: dict[int, set[int]] = {}
    for copy in copies:
        for offset in range(0, copy["size"], 4):
            source = copy["source"] + offset
            word = image_word(image, base, source)
            if word is not None:
                aliases.setdefault(copy["destination"] + offset, {})[word] = source
        for offset in range(0, copy["size"], 2):
            half = image_half(image, base, copy["source"] + offset)
            if half is not None:
                thumb_aliases.setdefault(copy["destination"] + offset, set()).add(half)
    for extra_arm, extra_thumb in extra_variants:
        for pc, word in extra_arm.items():
            aliases.setdefault(pc, {})[word] = pc
        for pc, half in extra_thumb.items():
            thumb_aliases.setdefault(pc, set()).add(half)
    alias_exits: set[tuple[int, bool]] = set()
    for pc, variants in aliases.items():
        for word in variants:
            if word & 0xFE000000 == 0xFA000000:
                offset = sign_extend(((word & 0xFFFFFF) << 2) | ((word >> 23) & 2), 26)
                target = (pc + 8 + offset) & 0xFFFFFFFF
                if target not in thumb_aliases:
                    alias_exits.add((target, True))
            elif word & 0x0E000000 == 0x0A000000:
                target = (pc + 8 + sign_extend(word & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
                if target not in aliases:
                    alias_exits.add((target, False))
    alias_exits = {
        root for root in alias_exits if base <= root[0] < base + len(image)
    }
    remaining = (
        instruction_limit - len(arm) - len(thumb) if instruction_limit else 0
    )
    if alias_exits and (instruction_limit == 0 or remaining > 0):
        first, *rest = alias_exits
        found_arm, found_thumb = reachable_instructions(
            image, base, first[0], remaining, first[1], tuple(rest)
        )
        arm.update(found_arm)
        thumb.update(found_thumb)
    for pc in set(arm) & set(aliases):
        aliases[pc][arm.pop(pc)] = pc
    for pc in set(thumb) & set(thumb_aliases):
        thumb_aliases[pc].add(thumb.pop(pc))
    variant_count = (
        sum(len(variants) for variants in aliases.values())
        + sum(len(variants) for variants in thumb_aliases.values())
    )

    thumb_entries: list[tuple[int, list[str]]] = []
    for pc, half in sorted(thumb.items()):
        thumb_entries.append(
            (
                pc,
                [
                    f"        case 0x{pc:08x}u:",
                    f"            if (half != 0x{half:04x}u) {{",
                    "                if (!nds_exec_thumb(cpu, half, pc))",
                    "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, half);",
                    "                return NDS_RUN_BUDGET_EXHAUSTED;",
                    "            }",
                    f"            if (!nds_exec_thumb(cpu, 0x{half:04x}u, pc))",
                    "                return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, half);",
                    "            return NDS_RUN_BUDGET_EXHAUSTED;",
                ],
            )
        )
    for pc, variants in sorted(thumb_aliases.items()):
        case_lines = [f"        case 0x{pc:08x}u:", "            switch (half) {"]
        for half in sorted(variants):
            case_lines.extend(
                (
                    f"            case 0x{half:04x}u:",
                    f"                if (!nds_exec_thumb(cpu, 0x{half:04x}u, pc))",
                    "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, half);",
                    "                return NDS_RUN_BUDGET_EXHAUSTED;",
                )
            )
        case_lines.extend(
            (
                "            default:",
                "                if (!nds_exec_thumb(cpu, half, pc))",
                "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, half);",
                "                return NDS_RUN_BUDGET_EXHAUSTED;",
                "            }",
            )
        )
        thumb_entries.append((pc, case_lines))
    thumb_entries.sort()

    arm_entries = [(pc, translated_case(pc, word)) for pc, word in sorted(arm.items())]
    for pc, variants in sorted(aliases.items()):
        case_lines = [f"        case 0x{pc:08x}u:", "            switch (word) {"]
        for word in sorted(variants):
            case_lines.append(f"            case 0x{word:08x}u:")
            case_lines.extend(translated_body(pc, word, "                "))
        case_lines.extend(
            (
                "            default:",
                "                if (!nds_exec_arm(cpu, word, pc))",
                "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);",
                "                return NDS_RUN_BUDGET_EXHAUSTED;",
                "            }",
            )
        )
        arm_entries.append((pc, case_lines))
    arm_entries.sort()

    ranges: dict[str, list[tuple[str, int, int]]] = {"arm": [], "thumb": []}
    shard_paths: set[Path] = set()
    for mode, entries, value_type, value_name in (
        ("arm", arm_entries, "uint32_t", "word"),
        ("thumb", thumb_entries, "uint16_t", "half"),
    ):
        shards: dict[int, list[tuple[int, list[str]]]] = {}
        for address, case_lines in entries:
            shards.setdefault(address >> 12, []).append((address, case_lines))
        for page, shard in sorted(shards.items()):
            function = f"nds_dispatch_{name}_{mode}_{page:05x}"
            shard_lines = [
                '#include "arm9_recomp.h"',
                "",
                f"NdsRunResult {function}(NdsCpu *cpu, uint32_t pc, {value_type} {value_name}) {{",
                "    switch (pc) {",
            ]
            for _, case_lines in shard:
                shard_lines.extend(case_lines)
            shard_lines.extend(
                (
                    "    default:",
                    f"        return nds_cpu_trap(cpu, NDS_RUN_OUTSIDE_TRANSLATION, pc, {value_name});",
                    "    }",
                    "}",
                    "",
                )
            )
            shard_path = output.parent / f"{name}_recomp_{mode}_{page:05x}.c"
            shard_paths.add(shard_path)
            write_text_if_changed(shard_path, "\n".join(shard_lines))
            ranges[mode].append((function, shard[0][0], shard[-1][0]))

    for stale in output.parent.glob(f"{name}_recomp_*.c"):
        if stale not in shard_paths:
            stale.unlink()

    lines = ['#include "arm9_recomp.h"', ""]
    for mode, value_type in (("arm", "uint32_t"), ("thumb", "uint16_t")):
        for function, _, _ in ranges[mode]:
            lines.append(f"NdsRunResult {function}(NdsCpu *, uint32_t, {value_type});")
    lines.extend(
        (
            "",
            f"const uint32_t nds_{name}_translated_instruction_count = {len(arm) + len(thumb) + variant_count}u;",
            "",
            f"NdsRunResult nds_run_{name}(NdsCpu *cpu, uint32_t budget) {{",
            "    while (budget-- != 0) {",
            "        if (nds_finish_interrupt(cpu))",
            "            continue;",
            "        const uint32_t pc = cpu->r[15];",
            "        cpu->previous_pc = cpu->last_pc;",
            "        cpu->last_pc = pc;",
            "        cpu->history[cpu->history_index++ & 15u] = pc;",
            "        cpu->instructions_executed++;",
            "        NdsRunResult result = NDS_RUN_OUTSIDE_TRANSLATION;",
            "        if (nds_cpu_is_thumb(cpu)) {",
            "            const uint16_t half = nds_read16(cpu, pc);",
        )
    )
    if not ranges["thumb"]:
        lines.append("            (void)half;")
    lines.append("            switch (pc >> 12) {")
    for function, first, _ in ranges["thumb"]:
        lines.append(f"            case 0x{first >> 12:05x}u: result = {function}(cpu, pc, half); break;")
    lines.extend(
        (
            "            }",
            "            if (result == NDS_RUN_OUTSIDE_TRANSLATION) {",
            "                if (!nds_exec_thumb(cpu, half, pc))",
            "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, half);",
            "                result = NDS_RUN_BUDGET_EXHAUSTED;",
            "            }",
            "        } else {",
            "            const uint32_t word = nds_read32(cpu, pc);",
            "            switch (pc >> 12) {",
        )
    )
    for function, first, _ in ranges["arm"]:
        lines.append(f"            case 0x{first >> 12:05x}u: result = {function}(cpu, pc, word); break;")
    lines.extend(
        (
            "            }",
            "            if (result == NDS_RUN_OUTSIDE_TRANSLATION) {",
            "                if (!nds_exec_arm(cpu, word, pc))",
            "                    return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word);",
            "                result = NDS_RUN_BUDGET_EXHAUSTED;",
            "            }",
            "        }",
            "        if (result != NDS_RUN_BUDGET_EXHAUSTED)",
            "            return result;",
            "        if (cpu->reschedule)",
            "            return NDS_RUN_BUDGET_EXHAUSTED;",
            "    }",
            "    return nds_cpu_trap(cpu, NDS_RUN_BUDGET_EXHAUSTED, cpu->r[15], nds_read32(cpu, cpu->r[15]));",
            "}",
            "",
        )
    )
    write_text_if_changed(output, "\n".join(lines))
    return len(arm), len(thumb), variant_count


def write_config_header(output: Path, info: RomInfo, compressed: bool, expanded_size: int) -> None:
    safe_title = info.title.replace("\\", "\\\\").replace('"', '\\"')
    write_text_if_changed(
        output,
        "\n".join(
            (
                "#pragma once",
                "",
                f'#define NDS_ROM_TITLE "{safe_title}"',
                f'#define NDS_GAME_CODE "{info.game_code}"',
                f"#define NDS_UNIT_CODE {info.unit_code}u",
                f"#define NDS_DEVICE_CAPACITY {info.device_capacity}u",
                f"#define NDS_ROM_SIZE 0x{info.rom_size:08x}u",
                f"#define NDS_ARM9_ROM_OFFSET 0x{info.arm9.rom_offset:08x}u",
                f"#define NDS_ARM9_ENTRY 0x{info.arm9.entry_address:08x}u",
                f"#define NDS_ARM9_RAM_ADDRESS 0x{info.arm9.ram_address:08x}u",
                f"#define NDS_ARM9_SIZE 0x{info.arm9.size:08x}u",
                f"#define NDS_ARM9_EXPANDED_SIZE 0x{expanded_size:08x}u",
                f"#define NDS_ARM9_BUILD_INFO_OFFSET 0x{info.arm9_build_info_offset:08x}u",
                f"#define NDS_ARM9_WAS_COMPRESSED {1 if compressed else 0}",
                f"#define NDS_ARM7_ENTRY 0x{info.arm7.entry_address:08x}u",
                f"#define NDS_ARM7_RAM_ADDRESS 0x{info.arm7.ram_address:08x}u",
                f"#define NDS_ARM7_SIZE 0x{info.arm7.size:08x}u",
                "",
            )
        ),
    )


def create_project(rom_path: Path, output: Path, instruction_limit: int, force: bool) -> RomInfo:
    info = inspect_rom(rom_path)
    manifest_path = output / "project.json"
    previous_sha256: str | None = None
    previous_manifest: dict[str, object] | None = None
    if manifest_path.exists() and not force:
        previous_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous_sha256 = previous_manifest.get("rom", {}).get("sha256")
        if previous_sha256 != info.sha256:
            raise ValueError(f"{output} belongs to another ROM; pass --force to replace generated files")
    elif manifest_path.exists():
        previous_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        previous_sha256 = previous_manifest.get("rom", {}).get("sha256")

    output.mkdir(parents=True, exist_ok=True)
    sync_template(output)
    generated = output / "generated"
    rom_dir = output / "rom"
    generated.mkdir(exist_ok=True)
    rom_dir.mkdir(exist_ok=True)

    # Keep create idempotent in the useful sense: after the ROM and generator
    # are unchanged, avoid rescanning hundreds of thousands of instructions,
    # extracting the ARM images, and rewriting hundreds of megabytes of
    # generated C just to configure or rebuild the same project.  Template
    # files were copied above, so runtime changes still propagate without
    # invalidating the translation cache.
    required_generated = (
        "arm7_data.c",
        "arm7_recomp.c",
        "arm9_recomp.c",
        "recomp_sources.cmake",
        "rom_config.h",
        "rom_data.c",
    )
    source_manifest = generated / "recomp_sources.cmake"
    cached_sources = []
    if source_manifest.is_file():
        cached_sources = [
            Path(line.strip())
            for line in source_manifest.read_text(encoding="utf-8").splitlines()
            if line.strip().startswith("generated/")
        ]
    cached_translation = (
        not force
        and previous_manifest is not None
        and previous_manifest.get("generator_sha256") == GENERATOR_SHA256
        and previous_manifest.get("translation", {}).get("instruction_limit") == instruction_limit
        and all((generated / name).is_file() for name in required_generated)
        and cached_sources
        and all((output / source).is_file() for source in cached_sources)
        and (rom_dir / "game.nds").is_file()
    )
    if cached_translation:
        return info

    arm9 = read_segment(rom_path, info.arm9)
    arm7 = read_segment(rom_path, info.arm7)
    translation_image, compressed = decompress_arm9(arm9)
    packaged_rom = rom_dir / "game.nds"
    if not packaged_rom.exists() or previous_sha256 != info.sha256:
        shutil.copy2(rom_path, packaged_rom)
    write_bytes_if_changed(rom_dir / "arm9.bin", arm9)
    write_bytes_if_changed(rom_dir / "arm7.bin", arm7)
    if compressed:
        write_bytes_if_changed(rom_dir / "arm9-expanded.bin", translation_image)
    elif (rom_dir / "arm9-expanded.bin").exists():
        (rom_dir / "arm9-expanded.bin").unlink()

    write_config_header(generated / "rom_config.h", info, compressed, len(translation_image))
    write_byte_array(generated / "rom_data.c", "nds_arm9_image", translation_image)
    write_byte_array(generated / "arm7_data.c", "nds_arm7_image", arm7)
    copies = find_static_copies(
        translation_image, info.arm9.ram_address, info.arm9.entry_address,
        0, 0x02000000
    )
    copies.extend(find_autoload_copies(
        translation_image, info.arm9.ram_address, info.arm9_build_info_offset,
        16 if info.unit_code else 12
    ))
    arm7_copies = find_static_copies(
        arm7, info.arm7.ram_address, info.arm7.entry_address,
        0x03800000, 0x03810000
    )
    arm7_copies.extend(find_stack_trampoline_copies(
        arm7, info.arm7.ram_address, info.arm7.entry_address,
        0x03800000, 0x03810000
    ))
    arm7_copies.extend(find_autoload_copies(
        arm7, info.arm7.ram_address, info.arm7_build_info_offset,
        16 if info.unit_code else 12
    ))
    arm9_overlay_variants = referenced_overlay_variants(
        rom_path,
        read_overlay_table(rom_path, info.arm9_overlay_offset, info.arm9_overlay_size),
        translation_image, info.arm9.ram_address, info.arm9.entry_address,
        instruction_limit, copies,
    )
    arm7_overlay_variants = referenced_overlay_variants(
        rom_path,
        read_overlay_table(rom_path, info.arm7_overlay_offset, info.arm7_overlay_size),
        arm7, info.arm7.ram_address, info.arm7.entry_address,
        instruction_limit, arm7_copies,
    )
    arm_translated, thumb_translated, aliases = write_translator(
        generated / "arm9_recomp.c", "arm9", translation_image,
        info.arm9.ram_address, info.arm9.entry_address, instruction_limit, copies,
        arm9_overlay_variants,
    )
    arm7_arm, arm7_thumb, arm7_aliases = write_translator(
        generated / "arm7_recomp.c", "arm7", arm7,
        info.arm7.ram_address, info.arm7.entry_address, instruction_limit, arm7_copies,
        arm7_overlay_variants,
    )
    recomp_sources = sorted(
        path.relative_to(output).as_posix()
        for path in generated.glob("*_recomp*.c")
    )
    write_text_if_changed(
        generated / "recomp_sources.cmake",
        "set(NDSRECOMP_SOURCES\n"
        + "\n".join(f"    {source}" for source in recomp_sources)
        + "\n)\n",
    )

    manifest = {
        "format_version": 1,
        "generator_sha256": GENERATOR_SHA256,
        "rom": asdict(info),
        "translation": {
            "instruction_limit": instruction_limit,
            "arm9_reachable_instructions": arm_translated,
            "thumb_reachable_instructions": thumb_translated,
            "arm9_relocated_variants": aliases,
            "arm9_overlay_images": len(arm9_overlay_variants),
            "arm9_compressed": compressed,
            "arm9_expanded_size": len(translation_image),
            "static_copies": copies,
            "thumb_supported": True,
            "arm7_reachable_instructions": arm7_arm,
            "arm7_thumb_reachable_instructions": arm7_thumb,
            "arm7_relocated_variants": arm7_aliases,
            "arm7_overlay_images": len(arm7_overlay_variants),
            "arm7_static_copies": arm7_copies,
        },
    }
    write_text_if_changed(manifest_path, json.dumps(manifest, indent=2) + "\n")
    write_text_if_changed(
        output / "README.md",
        f"""# {info.game_code} native recompilation project

Generated from `{info.title}`. The source ROM is packaged as `rom/game.nds` so
the runtime can service cartridge reads for overlays and game assets.

```sh
./compile.sh
./start.sh --self-test
```

Reuse the same build directory for incremental rebuilds. Set
`NDSRECOMP_BUILD_DIR` to use another build location and
`NDSRECOMP_BUILD_JOBS` to limit parallel compiler jobs. New projects group
generated translation units in small unity files for clean builds. CMake also
uses ccache or sccache automatically when installed; disable grouping with
`-DNDSRECOMP_UNITY_BUILD=OFF` or tune `NDSRECOMP_UNITY_BATCH_SIZE` and
`NDSRECOMP_UNITY_BATCH_KIB`.

For Windows, configure with `cmake/mingw-w64.cmake`. The runtime presents its
two-screen framebuffer through the easyGL2D-compatible host layer. The
translator handles reachable ARM/Thumb code for both CPUs plus SDK relocations.
Dynamically written ARM/Thumb code falls back to the same runtime instruction
helpers, so ITCM handlers and overlay entry points remain executable. Audio and
the complete DS hardware models remain active implementation work.
""",
    )
    return info


def create_projects(
    roms: list[Path], output: Path | None, instruction_limit: int, force: bool,
    output_root: Path = Path("output"),
) -> list[tuple[Path, RomInfo]]:
    if output is not None and len(roms) != 1:
        raise ValueError("--output can only be used with one ROM")
    outputs = (
        [output]
        if output is not None
        else [output_root / rom.stem / "native" for rom in roms]
    )
    if len({path.absolute() for path in outputs}) != len(outputs):
        raise ValueError("input ROM filenames must have unique stems")
    return [
        (project, create_project(rom, project, instruction_limit, force))
        for rom, project in zip(roms, outputs, strict=True)
    ]


def self_test() -> None:
    limited_image = bytearray(0x20)
    struct.pack_into("<2I", limited_image, 0, 0x1010, 0x1010)
    struct.pack_into("<3I", limited_image, 8, 0xE12FFF1E, 0, 0xE92D4000)
    limited_arm, limited_thumb = reachable_instructions(
        bytes(limited_image), 0x1000, 0x1008, 1
    )
    assert len(limited_arm) + len(limited_thumb) == 1

    with tempfile.TemporaryDirectory(prefix="ndsrecomp-test-") as temp:
        root = Path(temp)
        rom = root / "test.nds"
        data = bytearray(0x1000)
        data[0:12] = b"RECOMP TEST\0"
        data[0x0C:0x10] = b"TST0"
        data[0x10:0x12] = b"00"
        data[0x14] = 12
        struct.pack_into("<4I", data, 0x20, 0x200, 0x02000000, 0x02000000, 0x200)
        struct.pack_into("<4I", data, 0x30, 0x400, 0x03800000, 0x03800000, 0x100)
        struct.pack_into("<I", data, 0x70, 0x02000184)
        struct.pack_into("<I", data, 0x380, 0x02000100)
        struct.pack_into("<II", data, 0x31C, 0xDEC00621, 0x2106C0DE)
        # mov r0, #1; add r0, r0, #2; bx lr
        struct.pack_into("<3I", data, 0x200, 0xE3A00001, 0xE2800002, 0xE12FFF1E)
        # Old ARM7 SDK startup: pointer to magic-less autoload parameters,
        # followed by one 12-byte destination/initialized/BSS descriptor.
        struct.pack_into("<I", data, 0x400, 0xE12FFF1E)
        struct.pack_into("<I", data, 0x43C, 0x03800040)
        struct.pack_into("<3I", data, 0x440, 0x03800080, 0x0380008C, 0x038000C0)
        struct.pack_into("<3I", data, 0x480, 0x038000E0, 8, 0)
        struct.pack_into("<2I", data, 0x4C0, 0xE3A00001, 0xE12FFF1E)
        rom.write_bytes(data)
        second_rom = root / "second.nds"
        shutil.copy2(rom, second_rom)
        projects = create_projects([rom, second_rom], None, 3, False, root / "output")
        project, info = projects[0]
        assert project == root / "output" / "test" / "native"
        assert projects[1][0] == root / "output" / "second" / "native"
        manifest = json.loads((project / "project.json").read_text(encoding="utf-8"))
        assert info.game_code == "TST0"
        assert info.device_capacity == 12
        assert info.arm9_build_info_offset == 0x100
        assert info.arm7_build_info_offset == 0x40
        assert manifest["generator_sha256"] == GENERATOR_SHA256
        assert manifest["translation"]["instruction_limit"] == 3
        assert manifest["translation"]["arm9_reachable_instructions"] == 3
        assert manifest["translation"]["thumb_reachable_instructions"] == 0
        assert manifest["translation"]["arm7_static_copies"] == [
            {"source": 0x038000C0, "destination": 0x038000E0, "size": 8}
        ]
        generated = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (project / "generated").glob("arm9_recomp_*.c")
        )
        assert "nds_exec_data_processing" in generated
        assert "nds_branch_exchange" in generated
        runner = (project / "generated" / "arm9_recomp.c").read_text(encoding="utf-8")
        assert "nds_exec_arm" in runner
        assert "nds_exec_thumb" in runner
        assert "return nds_cpu_trap(cpu, NDS_RUN_UNSUPPORTED, pc, word)" in runner
        cached_projects = create_projects([rom], None, 3, False, root / "output")
        assert cached_projects[0][0] == project

        literal_leaf = bytearray(0x80)
        struct.pack_into("<I", literal_leaf, 0, 0xE59F1058)
        struct.pack_into("<I", literal_leaf, 0x5C, 0xE12FFF1E)
        struct.pack_into("<I", literal_leaf, 0x70, 0x02000000)
        assert (0x02000000, False) in find_function_roots(
            bytes(literal_leaf), 0x02000000
        )

        indirect_leaf = bytearray(0x40)
        struct.pack_into("<I", indirect_leaf, 0, 0xE12FFF1E)
        struct.pack_into("<2I", indirect_leaf, 0x20, 0xE92D4010, 0xE8BD8010)
        indirect_arm, _ = reachable_instructions(
            bytes(indirect_leaf), 0x02000000, 0x02000000, 0
        )
        assert indirect_arm[0x02000020] == 0xE92D4010

        long_indirect_leaf = bytearray(0x100)
        long_indirect_target = 0x02000020
        struct.pack_into("<I", long_indirect_leaf, 0, long_indirect_target)
        for offset in range(0x20, 0xA8, 4):
            struct.pack_into("<I", long_indirect_leaf, offset, 0xE1A00000)
        struct.pack_into("<I", long_indirect_leaf, 0xA8, 0xE12FFF1E)
        assert (long_indirect_target, False) in find_function_roots(
            bytes(long_indirect_leaf), 0x02000000, 256
        )
        assert short_linear_arm_function(
            bytes(long_indirect_leaf), 0x02000000,
            long_indirect_target, 64,
        ) is not None

        conditional_leaf = bytearray(long_indirect_leaf)
        struct.pack_into("<I", conditional_leaf, 0x28, 0x112FFF1E)
        assert short_linear_arm_function(
            bytes(conditional_leaf), 0x02000000,
            long_indirect_target, 64,
        ) is not None

        adjacent_leaf = bytearray(0x20)
        struct.pack_into("<I", adjacent_leaf, 0, 0xE12FFF1E)
        struct.pack_into("<4I", adjacent_leaf, 4,
                         0, 0xE59F1004, 0xE5801000, 0xE12FFF1E)
        struct.pack_into("<2I", adjacent_leaf, 0x14, 0, 0xE12FFF1E)
        adjacent_arm = {
            0x02000000: 0xE12FFF1E,
            0x02000018: 0xE12FFF1E,
        }
        assert add_adjacent_short_arm_functions(
            adjacent_arm, {}, bytes(adjacent_leaf), 0x02000000
        ) == 3
        assert adjacent_arm[0x02000008] == 0xE59F1004

        tail_call = bytearray(0x40)
        tail_call_target = 0x02000020
        struct.pack_into("<I", tail_call, 0, tail_call_target)
        struct.pack_into(
            "<6I", tail_call, 0x20,
            0xE1A01000, 0xE5910004, 0xE59FC004,
            0xE590002C, 0xE12FFF1C, 0x02000038,
        )
        assert tail_call_target in {
            target for target, is_thumb in find_function_roots(
                bytes(tail_call), 0x02000000
            ) if not is_thumb
        }
        assert len(short_linear_arm_function(
            bytes(tail_call), 0x02000000, tail_call_target
        ) or {}) == 5

        chain_rom = root / "overlay-chain.nds"
        chain_data = bytearray(0x300)
        main_base = 0x02000000
        first_base = 0x02001000
        second_base = 0x02002000
        main_target = main_base + 0x20
        copy_base = 0x01FFF000
        main_branch = 0xEA000000 | (((first_base - (copy_base + 8)) // 4) & 0xFFFFFF)
        struct.pack_into("<2I", chain_data, 0, 0xE12FFF1E, main_branch)
        struct.pack_into("<2I", chain_data, 0x20, 0xE3A00001, 0xE12FFF1E)
        struct.pack_into(
            "<4I", chain_data, 0x100,
            0xE59FC000, 0xE12FFF1C, second_base, 0xE12FFF1E,
        )
        struct.pack_into(
            "<4I", chain_data, 0x200,
            0xE59FC000, 0xE12FFF1C, main_target, 0xE12FFF1E,
        )
        chain_rom.write_bytes(chain_data)
        overlay_variants = referenced_overlay_variants(
            chain_rom,
            [
                Overlay(first_base, 16, 0, 0, 0x100, 16),
                Overlay(second_base, 16, 0, 0, 0x200, 16),
            ],
            bytes(chain_data[:0x28]), main_base, main_base, 0,
            [{"source": main_base + 4, "destination": copy_base, "size": 4}],
        )
        assert any(arm.get(main_target) == 0xE3A00001 for arm, _ in overlay_variants)
    print("ndsrecomp generator self-test: OK")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="inspect and validate a ROM")
    inspect_parser.add_argument("rom", type=Path)

    create_parser = subparsers.add_parser("create", help="create a native recompilation project")
    create_parser.add_argument("rom", type=Path, nargs="+", metavar="ROM")
    create_parser.add_argument(
        "--output", "-o", type=Path, help="project directory (single ROM only)"
    )
    create_parser.add_argument(
        "--instructions", type=int, default=0,
        help="maximum discovered instructions per CPU; 0 translates the complete graph",
    )
    create_parser.add_argument("--force", action="store_true")

    subparsers.add_parser("self-test", help="run the generator's built-in check")
    args = parser.parse_args()

    try:
        if args.command == "inspect":
            print(json.dumps(asdict(inspect_rom(args.rom)), indent=2))
        elif args.command == "create":
            if args.instructions < 0:
                raise ValueError("--instructions must be non-negative")
            for output, info in create_projects(
                args.rom, args.output, args.instructions, args.force
            ):
                print(f"created {output} for {info.game_code} ({info.title})")
        else:
            self_test()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
