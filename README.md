# GoldenEye 007 — Nintendo 3DS port

A native 3DS port of the [GoldenEye 007 N64 decompilation](https://github.com/n64decomp/007).
**No game data is included.** You need your own ROM. The port reads it from
`sdmc:/3ds/ge007/rom.z64` (or `baserom.us.z64`) at runtime.

## Making the CIA (no building needed)

1. Get `romtocia.exe` (Windows).
2. Drag your **GoldenEye 007 (USA)** ROM (`.z64`, `.v64` or `.n64`) onto it.
3. It writes `GoldenEye 007 (3DS).cia` next to the ROM in about 15 seconds.
   Install that with FBI. The ROM is inside the CIA, so nothing goes on the SD card.

The exe holds the compiled port and the CIA tools, but nothing from the ROM. It
checks the ROM's SHA-1 and builds the 3D HOME Menu banner from it. The CIA it
makes contains your ROM, so don't share it. To rebuild the exe:
`.tools/venv/Scripts/python.exe tools/romtocia/build_exe.py` (after `builder.py`).

## Status

**The game boots in a 3DS emulator.** The full decomp compiles for ARM,
links with the port layer into a 1.2 MB `ge007.3dsx`, loads your ROM, and
runs the N64 boot sequence: threads, scheduler, VI events, ROM DMA. It
stops at the first big-endian asset it parses (the animation tables). The
asset byte-order layer is the next milestone.

| Area | State |
|---|---|
| Toolchain fetch (`tools/fetch_toolchain.py`) | working: devkitARM r68 / GCC 16.1, libctru 2.7, citro3d, CMake, Ninja, w64devkit, Azahar |
| `builder.py` (verify → extract → patch → build → package) | working end to end from a pristine decomp checkout |
| Decomp compiled with GCC for ARM (`patches/goldeneye-3ds.patch`) | all ~250 files compile; links clean |
| Input PAL + controller shim | working; host tests pass |
| libultra: threads, messages, PI, VI, events, time, timers, SP tasks, AI, EEPROM, CPU | working as far as boot exercises them |
| ROM asset symbols (`tools/gen_rom_symbols.py`) | 1,526 symbols, offsets checked against the ROM |
| Game-side assembly in C (RNG; boot/TLB guarded out) | done |
| **Asset byte order (BE ROM data → ARM)** | **next**: first crash is in `initResolveAnimGroupTable` |
| F3D display-list → citro3d renderer (gfx task handler) | not started |
| Audio command executor (audio task handler) | not started |

## Building

### Toolchain

On Windows, nothing needs installing system-wide:

```sh
pip install zstandard                       # devkitPro packages are .zst
python tools/fetch_toolchain.py             # ~170 MB into .tools/
python tools/fetch_toolchain.py --with-emulator   # + Azahar, for testing
```

This pulls devkitPro's packages straight from its pacman repository,
checking each SHA-256 against the repo database, and pins CMake, Ninja and
w64devkit by hash. On Linux/macOS install devkitPro the normal way
(`3ds-dev` group).

Two native-Windows quirks are handled automatically:
- devkitPro's `3DS.cmake` refuses non-MSYS2 CMake, so on Windows the build
  uses `cmake/3DS-native-windows.cmake`. It does what devkitPro's entry
  files do, minus that guard, and reuses their platform files unchanged.
- devkitPro splices paths into flags unquoted, so a devkitPro path with a
  space breaks linking. `builder.py` then makes a directory junction at
  `<drive>:\ge007-devkitpro` (it copies nothing and needs no admin rights).

### One command: `builder.py`

```sh
python builder.py --rom path/to/goldeneye.z64 --fetch-decomp
# Windows:  builder.cmd --rom C:\path\to\goldeneye.z64 --fetch-decomp
```

1. **verify**: checks the ROM against the NTSC-U SHA-1
   (`abe01e4a…cfde83`, from the decomp's `ge007.u.sha1`). `.v64` and
   `.n64` dumps are converted first. A wrong region or modified ROM is
   rejected with an explanation.
2. **extract**: runs the decomp's own `scripts/extract_baserom.u.sh`.
   Assets go to `external/goldeneye/assets/`, and the step is skipped next
   time if the ROM is unchanged.
3. **patch**: applies `patches/goldeneye-3ds.patch` to the decomp, once.
   `--fetch-decomp` clones the commit in `patches/goldeneye-3ds.base`.
4. **build**: CMake + Ninja, then `tools/check_3dsx.py` validates the
   relocations (3dsxtool accepts some that every loader rejects).
5. **package**: `dist/3ds/ge007/` with `ge007.3dsx`, `rom.z64` and
   `README.txt`. Copy `dist/3ds` to the SD card root.

The `.3dsx` has its name, description and icon built in (SMDH); no
separate metadata file is needed. Game data is read from `rom.z64` at
runtime, so that file *is* the asset package. **`dist/` contains your ROM.
Don't share it.**

Useful flags: `--only verify|extract|build|package`, `--skip-extract`,
`--clean`, `--config Debug`, `--stream-rom`, `--no-rom`, `--harness`
(input test only; needs no ROM or decomp).

### Testing in an emulator

```sh
python tools/run_azahar.py --seconds 45
```

This copies `dist/3ds/ge007/` onto Azahar's virtual SD card, runs the game,
and prints the port's log (`pal_log` goes to `svcOutputDebugString`). It
then prints the first faults, each mapped to a function and source line.

### Host unit tests

```sh
cc -std=c11 -Iinclude tests/test_input_map.c src/pal/pal_input_map.c \
   src/pal/pal_input_profiles.c -lm -o test_input_map && ./test_input_map
```

### The decomp patch

The decomp targets IDO, not GCC. `patches/goldeneye-3ds.patch` (29 files)
makes it build with GCC for ARM. Each change keeps the N64/IDO build intact
(`#ifdef __sgi` / `TARGET_3DS`), so the patch could go upstream:
- **Preprocessor:** IDO tolerates token pastes that produce invalid tokens
  (`_##0.1##_`, `( ## X ## )`) and calls with missing macro arguments. Fixed
  in `CPPLib.h` (`IS_EMPTY` via `__VA_OPT__`, `DEFINED`, `EXPAND_ARGS_STACK`),
  `bondconstants.h` (a GCC `BITFLAG`), and by padding `SWITCH`/`New_Vector`
  calls.
- **C:** arrays of incomplete structs, array-to-array initialisers, a
  flexible array member inside an array, implicit declarations clashing
  with enum prototypes, and a member-name clash under `-fms-extensions`.
- **Endianness:** game code read display-list commands through big-endian
  bitfield views. It now uses `GFX_CMD()`/`GFX_DMA_ADDR()`-style word
  accessors in `gbi.h`, which give the same results on both endiannesses.
- **N64-only:** boot-time data inflation, TLB setup and the TLB-miss handler
  are guarded out. KSEG0/KSEG1 conversions are identity functions. The heap
  comes from the port, in linear memory.
- **Layout:** one loop relied on the N64 linker placing a variable right
  after an array.

## Layout

```
builder.py / builder.cmd    ROM → .3dsx → SD-card folder, in one command
CMakeLists.txt              3DS target, ge_pal library, optional decomp
res/icon.png                48×48 Homebrew Launcher icon
cmake/GoldenEyeDecomp.cmake compiles the decomp (game + portable libultra)
cmake/3DS-native-windows.cmake devkitPro toolchain entry for non-MSYS2 CMake
compat/decomp/              replaces the decomp's empty stddef.h
patches/                    GCC/ARM port patch for the decomp + its base commit
src/port/                   C for the game's own assembly (RNG), RSP symbols
tools/                      toolchain fetch, ROM symbols, 3dsx check, emulator run
include/pal/                platform abstraction headers (no N64 types)
src/pal/                    3DS implementations of the PAL
src/ultra/                  libultra API reimplemented on the PAL
src/gfx/      (planned)     F3D GBI interpreter + citro3d backend
src/audio/    (planned)     software RSP audio + ndsp output
src/main.c                  entry: boots the decomp, or the input test harness
tests/                      host-side unit tests
external/goldeneye          decomp submodule (not committed here)
```

Rule: **game code only ever talks to libultra.** We do not edit the decomp to
call 3DS APIs. Everything 3DS-specific sits behind `src/ultra/` → `src/pal/`.
That keeps decomp updates easy to merge.

---

## Architecture

### 1. How the decomp is integrated

The decomp is a matching decompilation built with IDO for MIPS. Here it is
recompiled with arm-none-eabi-gcc as an ordinary C library (`ge_decomp`):

- **Excluded:** libultra's hardware layers (`io/`, `os/`), RSP microcode,
  and every `.s` file. Each hand-written MIPS asm function needs a C
  replacement. libultra's portable C (audio synthesizer, `gu` maths,
  printf core, VI mode tables) *is* compiled.
- **libultra shim** (`src/ultra/`): reimplements the `os*` API the game calls
  (`osContGetReadData`, `osSendMesg`, `osPiStartDma`, `osViSwapBuffer`,
  `osAiSetNextBuffer`, …) on 3DS services.
- **Portability hazards** to fix in the decomp (ideally upstream, behind
  `NON_MATCHING`):
  - *Endianness.* The ARM11 is little-endian and the ROM is big-endian.
    Byte-swap asset data as it is loaded, per type. A blind 32-bit swap
    breaks `u8`/`u16` arrays, so each asset type needs its own swap routine.
  - *Segmented addresses.* `0x0Axxxxxx` pointers in display lists and
    models get resolved through a segment table in the GBI interpreter, as
    the RSP does.
  - *KSEG0/physical casts* (`K0_TO_PHYS`, `osVirtualToPhysical`) become
    identity functions. The 3DS gives us a flat 32-bit address space, so
    pointer size matches the N64 (no 64-bit pointer problems as on PC ports).
  - *Struct layout* differences from IDO packing and bitfields. Add
    `_Static_assert(sizeof(...))` for every struct that is read straight
    from ROM.

### 1b. libultra OS shim (threads, messages, PI, VI, time, tasks, audio, saves)

`src/ultra/` files include the decomp's `<ultra64.h>` and only call the PAL
(`pal_sys.h`, `pal_rom.h`). They never include `<3ds.h>`, since it defines
`u32`, `s32` and other names that clash with libultra's. Each
`include/ultra/os_*.h` repeats the libultra prototypes, so the compiler
catches any drift from the signatures the game expects.

- **Threads** (`os_thread.c`). Game-owned `OSThread`s get a slot in a side
  table, and `osStartThread` creates the libctru thread then. Every game
  thread runs on **one core**: libultra assumes a single CPU with strict
  priority preemption, and Horizon schedules a single core the same way.
  Priorities are mapped **by rank**, not by a linear scale, so threads with
  different N64 priorities never share a 3DS priority. The idle thread
  (priority 0) goes to 0x3F, below the 3DS main thread, so its endless
  spin can't starve APT. The N64 stack passed in is ignored; each thread
  gets a 128 KB native stack.
- **Message queues** (`os_message.c`). One global lock and condition
  variable stand in for libultra's interrupts-off critical sections.
  `OSMesgQueue` keeps its original layout because the game reads
  `validCount` directly.
- **PI / ROM** (`os_pi.c`). `osPiStartDma` copies from the ROM, then posts
  the `OSIoMesg` to the caller's queue before returning (N64 only promises
  the data is ready when the message arrives). The ROM is preloaded into
  RAM by default (`.z64`, `.v64` and `.n64` byte orders all work). Build
  with `-DOS_PORT_ROM_MODE=PAL_ROM_STREAM` to read from the SD card on
  every DMA instead (`.z64` only). Data stays big-endian, exactly as the N64
  PI would deliver it.
- **Boot.** On N64, boot never returns once the first thread starts. Here
  `GE_BOOT_ENTRY` returns, and the 3DS main thread then runs at
  `PAL_PRIO_SERVICE`, pumping APT. It will also post the VI retrace events
  that stand in for interrupts.

- **VI** (`os_vi.c`). The 3DS main thread calls `osPortVblank()` after
  each `gspWaitForVBlank`. That is the retrace interrupt: it latches the
  buffer passed to the last `osViSwapBuffer`, posts the `osViSetEvent`
  message every *n* retraces (which paces the game's scheduler to 30 FPS),
  posts `OS_EVENT_VI`, and calls the `pal_video` present hook when the
  shown buffer changes. The renderer will install that hook and call
  `C3D_FrameEnd` in it. Mode, gamma, dither and scale settings are
  recorded and otherwise ignored.
- **Events** (`os_event.c`). `osSetEventMesg` registrations get posted by
  whichever shim stands in for that hardware: VI retrace and PI DMA so
  far; SP/DP/AI once the gfx/audio task shims exist.
- **Time** (`os_time.c`). `osGetTime`/`osGetCount` convert the 3DS system
  tick (268 MHz) to the N64 count rate (46.875 MHz), so the game's cycle
  arithmetic is unchanged. `osSetTimer`/`osStopTimer` run on a service
  thread that sleeps until the next deadline. Periodic timers skip
  missed periods after a suspend instead of firing them all at once.

- **RSP/RDP tasks** (`os_sp.c`). `osSpTaskStartGo` queues the task for a
  task thread that stands in for the RSP. It runs at the lowest game
  priority, so it only uses CPU time while the game is waiting, the
  nearest single-core match to a parallel coprocessor. The thread calls
  the handler registered for the task type (`osPortSetTaskHandler`), then
  posts `OS_EVENT_SP`, plus `OS_EVENT_DP` for graphics tasks. Until the
  renderer and audio executor register handlers, tasks complete empty, so
  the scheduler keeps running. Tasks are never yielded mid-way.
- **Audio out** (`os_ai.c` → `pal_audio_3ds.c`). The AI's two-buffer FIFO
  becomes an ndsp channel with the same depth, and `osAiGetLength` /
  `osAiGetStatus` report real DSP progress, so the game's audio pacing
  works unchanged. If `dspfirm.cdc` is missing, playback is simulated
  against the clock: muted, but timing stays correct.
- **Saves** (`os_eeprom.c` → `pal_save_3ds.c`). The 4 Kbit EEPROM is a
  512-byte `sdmc:/3ds/ge007/eeprom.bin` in the same layout emulators use
  for `.eep` files, written through to the SD card on each save. Writes go
  to a temporary file first, so power loss can't corrupt the save.
- **CPU** (`os_cpu.c`). Cache operations do nothing: every "DMA" is a CPU
  copy, so the cache stays coherent. Virtual and physical addresses are
  the same. `osSetIntMask(OS_IM_NONE)` holds the OS lock, which is
  re-entrant per thread. Every stand-in interrupt posts under that lock,
  so masked sections are atomic, as on N64. TLB calls are no-ops, since
  native code doesn't need mapping.
- **Controller accessories**: every port reports no pak, so Rumble Pak
  support turns itself off.

Not supported: stopping or destroying *another* thread (Horizon can't
suspend a thread from outside; stopping or destroying yourself works), the
newer `osEPi*` handle API, which GoldenEye's libultra predates, and the
64DD.

**Renderer constraint.** Display lists mix real pointers with N64
segmented addresses (`0x0S______`). With identity address translation,
game memory must stay out of `0x01000000`–`0x0FFFFFFF`, but the default
3DS heap is at `0x08000000`. The renderer milestone must put the game's
arenas in linear memory (`0x14000000` / `0x30000000`). See `os_cpu.c`.

**Remaining backlog before the game can link:** C replacements for the
game's own assembly (`random.s`, `getra.s`, `tlb_*.s`, `osMapTLB.s`),
plus `TARGET_3DS` guards in `init()` (TLB setup, data-segment inflate)
and `tlb_manage.c`.

### 2. Input (implemented)

`pal_input_map.c` is pure logic: it takes a `PalRawInput` snapshot and
returns a `PalN64Pad`, which has the same bits and layout as `OSContPad`.
`pal_input_3ds.c` polls HID. `os_cont.c` serves the pad through
`osContGetReadData`, sampling once per game frame in `osContStartReadData`.

| N64 | New 3DS profile | Old 3DS profile |
|---|---|---|
| Stick (move/turn) | Circle Pad | Circle Pad |
| C-buttons (strafe/look) | C-stick, touch drag | ABXY diamond, touch drag |
| Z (fire) | ZR, X, touch | R, touch |
| R (aim) | R, ZL | L |
| L | L | D-pad ← |
| A (weapon) / B (action) | A / B, touch | D-pad ↑ / ↓, touch |
| Start (watch) | Start, Select, touch | Start, Select, touch |

Profile is chosen by `APT_CheckNew3DS`. The Old 3DS profile does not map L/R
straight to L/R: fire has to be on a shoulder button, since your right thumb
is busy on ABXY for looking.

Details:
- **Radial deadzone** with an optional response exponent, so diagonals don't
  snap to the axes.
- **C-stick hysteresis** (press at 40, release below 25) stops chatter at the
  threshold.
- **Touch:** what a touch starts on decides what it does until you lift.
  Starting on a button zone presses that button. Starting anywhere else
  makes it a look-drag, which presses C-buttons and holds them a few frames
  so slow drags don't flicker. The raw drag delta (`look_dx/dy`) is also
  kept for a later analog-aim mode.

**Next:** load profiles from `sdmc:/3ds/ge007/controls.ini`. Add an
optional analog touch-aim that writes straight into the player's
look/aim angles (a small, clearly marked patch to the decomp's player
code). C-button look in GoldenEye is digital and slow, so this is the
largest control-feel improvement available.

### 3. Graphics: F3D interpreter → PICA200

**Approach: interpret display lists on the CPU and draw with citro3d.**
No RDP emulation. This is the same model as the SM64/OoT PC ports (`gfx_pc`
/ Fast3D), with a PICA200 backend.

- **Vertex path.** The game's matrices and vertices go to a PICA vertex
  shader (`.v.pica`) that does the transform and lighting. Do not transform
  on the CPU: the ARM11 is the bottleneck, and the PICA has a real vertex
  unit. Batch triangles between state changes into one `C3D_DrawArrays`.
- **Color combiner → TexEnv.** The N64's 1/2-cycle combiner
  `(A−B)·C+D` maps onto PICA's 6 TexEnv stages. Each stage does
  `src0·src1 + src2` style ops, so `(A−B)·C+D` takes 2 stages. Generate
  TexEnv configs from combiner modes and cache them by the 64-bit combine
  word.
- **Textures.** Decode N64 formats (RGBA16, CI4/CI8 + TLUT, IA, I) into
  PICA formats (RGBA5551, RGBA4, LA4, L8). Morton-swizzle them, pad to
  powers of two (min 8×8), and cache them keyed on (address, format, TLUT
  hash). N64 clamp/mirror/mask become PICA wrap modes. Masks that aren't
  powers of two need a shader-free workaround (pre-expand the texture).
- **Framebuffer.** GoldenEye renders 320×240 (some modes 440×330). Render
  to a 400×240 render target with a 4:3 viewport and pillarbox. Optional
  stereo 3D: two `C3D_RenderTarget`s with an eye-offset projection.
- **Fog / Z.** N64 fog depends on vertex shade alpha. Compute the fog factor
  in the vertex shader and use PICA's fog LUT or a TexEnv blend. Remap
  depth to the PICA's [−1,0] clip-space convention.
- **Framebuffer effects** (sniper zoom overlay, screen fades) can mostly be
  drawn as full-screen quads. Avoid reading the framebuffer back from VRAM;
  it is very slow.
- **Budget for 30 FPS (33 ms):** GoldenEye's original frame rate is often
  15–20 FPS, so stable 30 is a stretch goal on Old 3DS. On New 3DS enable
  the 804 MHz clock and L2 cache (`osSetSpeedupEnable(true)`). Run game
  logic on core 0 and display-list interpretation on a second core
  (`threadCreate` with core 2 on New 3DS; on Old 3DS core 1 is the system core, which the app can only
  time-share via `APT_SetAppCpuTimeLimit`).

### 4. Audio and memory

**Audio.** GoldenEye drives libultra's synthesizer (`alSynth*` /
`n_alSynth`), which builds RSP audio command lists (`Acmd`). There's no RSP
here, so execute those commands in C, the way the SM64 PC port's `mixer.c`
does: ADPCM decode, resample, envelope, mix, reverb. Then:

- Hook `osAiSetNextBuffer` to queue the finished PCM into a ring of
  `ndspWaveBuf`s (stereo 16-bit, ndsp at 32 kHz, matching the game's
  output rate so there's no extra resample).
- Run the audio frame on its own thread at higher priority than the game
  thread, so frame-rate dips don't cause crackle.
- `ndspInit` needs `sdmc:/3ds/dspfirm.cdc`. Tell the user how to dump it if
  it's missing, rather than failing silently.

**Memory.**
- RAM is not the problem: a 3DSX app gets ~64 MB on Old 3DS (more with
  extended memory modes, ~124 MB on New 3DS). GoldenEye's whole N64 address
  space is 4 MB, or 8 MB with the Expansion Pak.
- **Keep GoldenEye's own heap.** Give the game one fixed `malloc`'d arena
  the size it expects, so its allocator and fragmentation behave the same
  as on N64. Don't route its allocations to newlib `malloc`.
- **ROM access.** `osPiStartDma` becomes a `memcpy` from the ROM image.
  Load the whole ROM (12 MB) into the regular heap at boot; it fits easily,
  and SD reads during gameplay would stall. Rare's compressed assets are still inflated by the game's own code,
  unchanged.
- **GPU memory.** Textures and vertex buffers must be in `linearAlloc`
  memory (a separate, smaller pool). Size the texture cache from linear
  memory, not total RAM, and evict LRU per level.
- **Saves.** Implement the EEPROM API (`osEepromProbe/Read/Write`) on a
  file in `sdmc:/3ds/ge007/`.

`mbedtls` isn't needed. Nothing in the game uses crypto or networking.

## Legal

This repository contains no Nintendo or Rare code or assets. The decomp is a
separate project with its own terms. You must own the game.
