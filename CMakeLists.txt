cmake_minimum_required(VERSION 3.13)

# Configure with the devkitPro toolchain:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#   cmake --build build
# (or use $DEVKITPRO/portlibs/3ds/bin/arm-none-eabi-cmake, which presets it)
project(ge007_3ds C ASM)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR
        "This project targets the 3DS. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(GE_INTEGRATE "Compile and link the GoldenEye decomp (see cmake/GoldenEyeDecomp.cmake)" OFF)
set(GE_VERSION "us" CACHE STRING "Decomp build version: us, jp or eu")

# ---------------------------------------------------------------------------
# Port layer: platform abstraction + libultra shim. No game code.
# ---------------------------------------------------------------------------
add_library(ge_pal STATIC
    src/pal/pal_input_map.c
    src/pal/pal_input_profiles.c
    src/pal/pal_input_3ds.c
    src/pal/pal_sys_3ds.c
    src/pal/pal_rom_3ds.c
    src/pal/pal_video.c
    src/pal/pal_audio_3ds.c
    src/pal/pal_save_3ds.c
    src/pal/pal_wav_3ds.c
    src/gfx/gfx_c3d.c
)
target_include_directories(ge_pal PUBLIC include)
target_compile_options(ge_pal PRIVATE -Wall -Wextra -O2)

# The F3D interpreter's vertex shader, embedded as f3d_shbin.h.
ctr_add_shader_library(f3d src/gfx/f3d.v.pica)
dkp_add_embedded_binary_library(ge_shader_bin f3d)
target_link_libraries(ge_pal PUBLIC ge_shader_bin citro3d)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(ge007 src/main.c)
target_link_libraries(ge007 PRIVATE ge_pal citro2d citro3d m)
target_compile_options(ge007 PRIVATE -Wall -Wextra -O2)

# ARM11 MPCore: hard-float VFPv2. The toolchain sets -march/-mtune; these
# just make sure dead code from the (large) decomp is stripped at link.
target_compile_options(ge007 PRIVATE -ffunction-sections -fdata-sections)
target_link_options(ge007 PRIVATE -Wl,--gc-sections)
# The game code uses int-sized enums (see GoldenEyeDecomp.cmake); no enum
# value crosses between it and libctru, so the mismatch notes are noise.
target_link_options(ge007 PRIVATE -Wl,--no-enum-size-warning)

if(GE_INTEGRATE)
    # init() in the decomp's src/init.c calls osInitialize, then creates and
    # starts the main thread. Its TLB setup and data-segment inflate are
    # N64-only and need TARGET_3DS guards in the decomp.
    set(GE_BOOT_ENTRY "init" CACHE STRING
        "Decomp function that starts the first libultra thread (called from main.c)")
    set(OS_PORT_ROM_MODE "PAL_ROM_PRELOAD" CACHE STRING
        "PAL_ROM_PRELOAD (load ROM into RAM) or PAL_ROM_STREAM (read from SD per DMA)")

    include(cmake/GoldenEyeDecomp.cmake)

    # Asset symbols (PcctvZ, _sfxctlSegmentRomStart, ...) as ROM offsets,
    # generated from the decomp's own file list. See the script.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GE_ROM_SYMBOLS ${CMAKE_BINARY_DIR}/generated/rom_symbols.s)
    add_custom_command(
        OUTPUT  ${GE_ROM_SYMBOLS}
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_rom_symbols.py
                ${GE_DECOMP_DIR} ${GE_ROM_SYMBOLS}
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_rom_symbols.py
                ${GE_DECOMP_DIR}/scripts/filelist.u.csv
                ${GE_DECOMP_DIR}/assets/obseg/ob_seg.s
        COMMENT "Generating ROM asset symbols"
        VERBATIM)

    # libultra shim: compiled against the decomp's <ultra64.h>, never <3ds.h>.
    add_library(ge_ultra STATIC
        src/ultra/os_ai.c
        src/ultra/os_cont.c
        src/ultra/os_cpu.c
        src/ultra/os_eeprom.c
        src/ultra/os_event.c
        src/ultra/os_internal.c
        src/ultra/os_message.c
        src/ultra/os_pi.c
        src/ultra/os_port.c
        src/ultra/os_sp.c
        src/ultra/os_thread.c
        src/ultra/os_time.c
        src/ultra/os_vi.c
        # C replacements for the game's own assembly and linker symbols
        src/port/asset_swap.c
        src/gfx/f3d.c
        src/gfx/f3d_tex.c
        src/gfx/f3d_cc.c
        src/audio/asp.c
        src/port/bg_swap.c
        src/port/model_swap.c
        src/port/music_stems.c
        src/port/random.c
        src/port/setup_swap.c
        src/port/rsp_ucode.c
        ${GE_ROM_SYMBOLS}
    )
    target_include_directories(ge_ultra PRIVATE ${GE_DECOMP_INCLUDE_DIRS})
    target_compile_definitions(ge_ultra PRIVATE ${GE_DECOMP_DEFINES}
                                                OS_PORT_ROM_MODE=${OS_PORT_ROM_MODE})
    target_compile_options(ge_ultra PRIVATE
        -fms-extensions
        -fno-short-enums       # as ge_decomp: it shares the game's structs
        -Wall -Wextra -O2)
    target_link_libraries(ge_ultra PUBLIC ge_pal)
    # The renderer's hot loops (-O3: 4.5% less interpreting on Streets), and
    # no errno checks around sqrtf.
    set_source_files_properties(src/gfx/f3d.c src/gfx/f3d_tex.c src/gfx/f3d_cc.c
                                PROPERTIES COMPILE_OPTIONS "-O3;-fno-math-errno")

    target_compile_definitions(ge007 PRIVATE GE_INTEGRATE GE_BOOT_ENTRY=${GE_BOOT_ENTRY})
    # ge_decomp first: static libs resolve left to right, and the game is
    # what pulls in the shim.
    target_link_libraries(ge007 PRIVATE ge_decomp ge_ultra)
endif()

ctr_generate_smdh(ge007.smdh
    NAME        "Goldeneye 64"
    DESCRIPTION "N64 decompilation port"
    AUTHOR      "Rare"
    ICON        ${CMAKE_SOURCE_DIR}/res/icon.png
)

# No ROMFS: game data is read at runtime from the user's own ROM on the SD
# card, so nothing copyrighted ends up inside the .3dsx.
ctr_create_3dsx(ge007 SMDH ge007.smdh)
