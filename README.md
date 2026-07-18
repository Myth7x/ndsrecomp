# ndsrecomp

`ndsrecomp` turns a legally dumped Nintendo DS ROM into a standalone C/CMake
static-recompilation project. Generated source, extracted CPU programs, SDL,
and native build trees all stay below `output/`.

The current milestone emits working Linux x86-64 ELF and Windows x86-64 EXE
diagnostic runtimes. It translates reachable ARM/Thumb code for both DS CPUs,
includes statically discovered overlay variants, and renders through a small host
easyGL2D-compatible layer backed by SDL3 with a live debug HUD. It is not yet a
complete DS hardware runtime, so arbitrary retail games do not reach gameplay
yet.

## Native quick start

Requirements: Python 3.10+, CMake 3.24+, Ninja, a C compiler, and internet
access for CMake to fetch the pinned SDL3 source when SDL3 is not installed.

Generated projects keep their CMake/Ninja object files, so rerunning the same
build is incremental. New projects group generated translation units in small
unity files for faster clean builds. If `ccache` or `sccache` is installed,
CMake enables it automatically. Disable grouping with
`-DNDSRECOMP_UNITY_BUILD=OFF`, or tune its file and size limits with
`NDSRECOMP_UNITY_BATCH_SIZE` and `NDSRECOMP_UNITY_BATCH_KIB`.

```sh
# Inspect any structurally valid .nds ROM.
python3 ndsrecomp.py inspect game.nds

# Generate and compile a Linux ELF under output/game/native/.
make ROM=game.nds native-linux
output/game/native/build-linux/ndsrecomp --self-test

# Generate isolated projects for every ROM matched by the shell.
python3 ndsrecomp.py create ROMS/*.nds

# Cross-compile a Windows EXE (requires MinGW-w64).
make ROM=game.nds native-windows
```

The convenience defaults use `baserom.nds`, so `make` is equivalent to
`make ROM=baserom.nds native-linux`. Normal builds translate the complete
discovered control-flow graph; set `INSTRUCTIONS=...` to cap it for diagnostics.
Every ROM gets an isolated tree named from its
filename: `other.nds` uses `output/other/`. `ROM_OUTPUT=...` can override it.
The generator accepts several input paths at once, so a ROM collection can be
generated in one pass. To compile each generated Linux project as well:

```sh
for rom in ROMS/*.nds; do make ROM="$rom" native-linux; done
```

The native window shows the DS button-to-key mapping, touch state, frame timing,
CPU throughput/PCs, and display-register state in a two-column debug sidebar.
Use the mouse on the lower screen for touch. Keyboard controls are
X/Z for A/B, S/A for X/Y, Q/W for L/R, Enter/Backspace for Start/Select, and
the arrow keys for the D-pad.

Useful targets:

- `make check` — generator test, Linux build, and deterministic runtime test.
- `make check-windows` — Windows cross-build and Wine runtime test.
- `make clean` — remove only native build trees; preserve generated source.
- `make distclean` — remove the generated native project.

The generated project is independent of this repository after creation. Its
`project.json` records the ROM header, SHA-256, and translation coverage. The
source ROM is copied to `rom/game.nds` inside that ROM's output tree so the
runtime can service cartridge, overlay, and asset reads.

## Current runtime boundary

Implemented today:

- generic NDS header validation and ARM9/ARM7 extraction;
- static ARM and Thumb instruction cases, direct control-flow discovery,
  jump/function tables, SDK autoload relocation, and mixed-mode calls;
- concurrent ARM9/ARM7 execution, ARM banked SP/LR, exception returns, BIOS
  services, interrupts, timers, VBlank, IPC, and cartridge reads;
- persistent EEPROM/flash save files stored beside each packaged ROM;
- 16 MiB main RAM, ITCM/DTCM, ARM7 WRAM, palette, VRAM, OAM, basic I/O, and
  synchronous DMA;
- two stacked 256x192 software screens presented through SDL3, with keyboard,
  mouse touch input, frame pacing, and a live input/performance/render HUD;
- reproducible Linux ELF and MinGW-w64 Windows EXE builds.

Still required before the “any ROM runs” goal is reached: complete overlay and
filesystem behavior, full 2D/3D GPU register emulation, audio, remaining
BIOS and peripheral services, and title-specific edge cases. Unsupported
execution stops with the exact PC and opcode rather than silently producing
incorrect behavior.

## DSD, objdiff, dsd-ghidra, and libnds

The original decompilation/research tools remain available as secondary
workflows:

```sh
make setup       # pinned DSD, objdiff, dsd-ghidra, devkitPro image
make ds          # libnds replacement under output/baserom/ds/
make decomp-init # DSD data under output/baserom/decomp/
make objdiff
make ghidra
```

Pinned versions are DSD 0.11.0, objdiff 3.7.3, dsd-ghidra 0.6.0, and
`devkitpro/devkitarm:20260610`. DSD currently rejects DSi-enhanced ROMs such as
the configured `IRBD` image; `make decomp-init` reports that limitation before
writing invalid data. The native generator accepts both DS and DSi-enhanced
headers, but supporting a header is distinct from fully emulating its hardware.
