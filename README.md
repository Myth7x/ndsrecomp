# ndsrecomp

`ndsrecomp` converts a legally dumped Nintendo DS ROM into a standalone C/CMake
static-recompilation project. The generated project contains native C for the
discovered ARM9 and ARM7 code, a small DS runtime, a software video renderer,
and an SDL3 diagnostic front end.

This is an experimental runtime, not a complete Nintendo DS emulator. The
current milestone produces Linux x86-64 ELF and Windows x86-64 EXE diagnostic
runtimes. Some ROMs boot far enough to exercise CPU, memory, video, and input
paths, but arbitrary retail games do not yet reach gameplay.

## ROMs tested

| ROM | BOOT | GAMEPLAY | GRAPHICS | AUDIO | INPUT | NOTES |
| --- | --- | --- | --- | --- | --- | --- |
| 0102 - Nintendogs - Labrador & Friends (Europe) (En,Fr,De,Es,It) | ✅ | ❌ | ❌ | ❌ | ❌ | Wrong EEPROM implementation causes being stuck |
| 5585 - Pokemon - Black Version (USA, Europe) (NDSi Enhanced) [b] | ❌ | ❌ | ❌ | ❌ | ❌ | - |
| Advance Wars - Dual Strike (USA, Australia) | ❌ | ❌ | ❌ | ❌ | ❌ | - |
| Chrono Trigger (USA) (En,Fr) | ✅ | ❌ | ❌ | ❌ | ❌ | - |
| Mario & Luigi - Bowser's Inside Story (Europe) (En,Fr,De,Es,It)_apfix | ❌ | ❌ | ❌ | ❌ | ❌ | - |
| Mario Kart DS (USA, Australia) (En,Fr,De,Es,It) | ❌ | ❌ | ❌ | ❌ | ❌ | - |
| New Super Mario Bros. (USA, Australia) | ✅ | ❌ | ❌ | ❌ | ❌ | - |
| Pokémon - Black Version 2 (USA, Europe) (NDSi Enhanced) | ❌ | ❌ | ❌ | ❌ | ❌ | - |
| Super Mario 64 DS (USA, Australia) (Rev 1) | ✅ | ❌ | ❌ | ❌ | ❌ | - |

## Quick start

Requirements:

- Python 3.10 or newer
- CMake 3.24 or newer
- Ninja and a C17 compiler
- OpenGL development files
- SDL3 development files, or internet access so CMake can fetch the pinned
  SDL3 source

Inspect a ROM before generating a project:

```sh
python3 ndsrecomp.py inspect path/to/game.nds
```

Generate a project. By default, a ROM named `game.nds` becomes
`output/game/native/`:

```sh
python3 ndsrecomp.py create path/to/game.nds
```

Generated projects include `compile.sh` and `start.sh`, so after creation the
usual workflow is:

```sh
cd output/game/native
./compile.sh
./start.sh --self-test
```

Set `NDSRECOMP_BUILD_DIR` to choose another build directory or
`NDSRECOMP_BUILD_JOBS` to limit parallel compiler jobs.

Build and run its deterministic headless self-test:

```sh
cmake -S output/game/native -B output/game/native/build-linux -G Ninja
cmake --build output/game/native/build-linux
output/game/native/build-linux/ndsrecomp --self-test
```

Generate several ROMs in one pass:

```sh
python3 ndsrecomp.py create ROMS/*.nds
```

Use `--output` for one custom project directory. Use `--instructions N` to cap
discovery per CPU for a faster diagnostic build; `0` (the default) translates
the complete graph discovered by the static analysis. Use `--force` when an
existing output directory belongs to a different ROM or must be regenerated.

```sh
python3 ndsrecomp.py create game.nds \
  --output output/my-game/native \
  --instructions 100000
```

The generator also has a self-test that does not require a ROM:

```sh
python3 ndsrecomp.py self-test
```

## Static recompilation workflow

Static recompilation happens once, before the native project is compiled. It
does not run a JIT compiler while the game is executing.

1. `ndsrecomp.py` validates the 512-byte DS header, extracts ARM9 and ARM7
   program descriptors, reads the entry points and overlay tables, and records
   the ROM SHA-256 in `project.json`.
2. The ARM9 image is expanded when it uses Nintendo's backwards LZ format.
   ARM9 and ARM7 images are embedded into generated C arrays and also copied
   into `rom/` for cartridge, overlay, asset, and save-file access at runtime.
3. The generator recognizes SDK startup behavior: autoload tables, static
   copies into RAM, ARM7 stack trampolines, and other relocated code. These
   copies are emitted as address aliases so code executing from its relocated
   address still reaches the correct native translation.
4. Starting at each CPU entry point, the analyzer follows ARM and Thumb
   control flow. It handles direct branches, mixed ARM/Thumb calls, jump
   tables, function-pointer tables, address-taken functions, and referenced
   overlay entry points. Overlay images are read through the ROM FAT and
   analyzed when reachable from the main image.
5. Each discovered instruction becomes a generated C dispatch case keyed by
   its original Nintendo DS address. Generated files are split into 4 KiB
   address-page shards so large ROMs remain manageable. `arm9_recomp.c` and
   `arm7_recomp.c` provide the CPU run loops and include the shard declarations.
6. The generated dispatcher executes known instructions through the native DS
   runtime helpers. If code changes after translation, or execution reaches an
   address outside the static set, the runtime reads the live opcode and uses
   its generic ARM/Thumb helper. Unsupported instructions stop with the CPU,
   program counter, and opcode recorded for diagnosis.
7. CMake compiles the generated translation units together with the shared
   runtime. SDL3 presents the two 256x192 DS screens, while the host runtime
   models CPU memory, interrupts, timers, IPC, cartridge access, saves, and
   the currently implemented video/GPU behavior.

The result is native code with the original ROM addresses preserved as the
runtime’s address space. It is therefore useful for fast, inspectable
experiments and incremental hardware/runtime development, while still needing
more hardware coverage and title-specific analysis before it can replace an
emulator or a full decompilation.

## Generated project layout

For `output/game/native/`:

```text
output/game/native/
├── CMakeLists.txt                 generated native build definition
├── cmake/mingw-w64.cmake         MinGW-w64 toolchain file
├── src/                           copied host runtime and renderer
├── generated/
│   ├── rom_config.h               ROM metadata and memory layout constants
│   ├── rom_data.c                 expanded ARM9 image as a C array
│   ├── arm7_data.c                ARM7 image as a C array
│   ├── arm9_recomp.c              ARM9 dispatcher
│   ├── arm7_recomp.c              ARM7 dispatcher
│   ├── *_recomp_*.c               ARM/Thumb address-page shards
│   └── recomp_sources.cmake       generated source list for CMake
├── rom/
│   ├── game.nds                   packaged source ROM
│   ├── arm9.bin                   original ARM9 segment
│   ├── arm9-expanded.bin          expanded ARM9 segment, when compressed
│   └── arm7.bin                   original ARM7 segment
├── project.json                   ROM identity and translation statistics
└── build-linux/                   optional CMake build tree
```

The generated project includes its own README with the basic build command and
runtime notes. After creation, generated source and runtime files are copied
into the project, so the project can be built independently of the repository.
Build directories are intentionally separate from generated source, making
incremental rebuilds safe and allowing generated projects to be inspected or
archived as standalone trees.

## Running and diagnostics

Without arguments, the executable loads `../rom/game.nds` relative to its own
location. A different ROM path can be supplied explicitly:

```sh
output/game/native/build-linux/ndsrecomp --frames 600
output/game/native/build-linux/ndsrecomp --rom /path/to/game.nds --once
output/game/native/build-linux/ndsrecomp --steps 1000000 --dump-frame frame.bmp
```

Useful runtime options are:

- `--self-test` runs one headless deterministic frame and reports its
  framebuffer hash.
- `--once` runs one frame and exits.
- `--frames N` runs a fixed number of frames.
- `--steps N` sets the instruction budget for the initial run.
- `--dump-frame PATH` writes the current 256x384 stacked framebuffer.
- `--tap X Y` holds the touch input at a coordinate.
- `--tap-frame FRAME X Y` presses the touch screen for a scripted frame range.

The interactive window stacks the DS screens vertically and includes a live
input, CPU, frame-timing, and render HUD. Keyboard controls are X/Z for A/B,
S/A for X/Y, Q/W for L/R, Enter/Backspace for Start/Select, and the arrow keys
for the D-pad. The mouse controls the lower-screen touch input.

Generated translation units are grouped into small unity files by default to
reduce clean-build overhead. CMake enables `ccache` or `sccache` when one is
installed. The behavior can be tuned with:

```sh
cmake -S output/game/native -B output/game/native/build-linux -G Ninja \
  -DNDSRECOMP_UNITY_BUILD=OFF \
  -DNDSRECOMP_UNITY_BATCH_SIZE=4 \
  -DNDSRECOMP_UNITY_BATCH_KIB=4096
```

For a Windows x86-64 build, install MinGW-w64 and configure with the generated
toolchain file:

```sh
cmake -S output/game/native -B output/game/native/build-windows -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build output/game/native/build-windows
```

## Current runtime boundary

Implemented areas include DS header validation and CPU extraction, ARM/Thumb
translation for both CPUs, SDK relocation handling, referenced overlays,
concurrent ARM9/ARM7 execution, banked CPU state, exceptions, BIOS services,
interrupts, timers, VBlank, IPC, cartridge reads, persistent EEPROM/flash save
files, main RAM, ITCM/DTCM, ARM7 WRAM, palette/VRAM/OAM, basic I/O, DMA, two
software-rendered screens, keyboard/mouse input, frame pacing, and diagnostic
HUD output.

Still active work includes complete overlay and filesystem behavior, full DS
2D/3D hardware coverage, audio, remaining BIOS and peripheral services, and
title-specific edge cases. A trap with the exact PC and opcode is preferred to
silently continuing with incorrect behavior.

## Related research workflows

This repository’s native generator is separate from traditional source-level
decompilation. DSD, objdiff, dsd-ghidra, and libnds remain useful for reverse
engineering, symbol recovery, matching, and rebuilding original game code,
but they are not required to generate the native recompilation project.

The native workflow accepts DS and DSi-enhanced headers. Header support does
not imply that DSi hardware or all game peripherals are emulated.
