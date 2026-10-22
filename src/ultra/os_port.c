/*
 * os_port.c - one-time setup for the libultra shim.
 */
#include <stddef.h>
#include <stdint.h>

#include "pal/pal_audio.h"
#include "pal/pal_rom.h"
#include "pal/pal_sys.h"
#include "ultra/os_port.h"

/* PAL_ROM_PRELOAD by default: the 12 MB ROM fits in RAM with room to
 * spare, and it removes SD latency from level loads. Build with
 * -DOS_PORT_ROM_MODE=PAL_ROM_STREAM to read from the card on every DMA. */
#ifndef OS_PORT_ROM_MODE
#define OS_PORT_ROM_MODE PAL_ROM_PRELOAD
#endif

/* The ROM image buffer and its bounds, defined by the generated
 * rom_symbols.s (tools/gen_rom_symbols.py). The game's asset symbols point
 * into it. */
extern unsigned char ge_rom_image[];
extern unsigned char ge_rom_image_end[];

/* src/gfx/f3d.c: the graphics task handler */
void f3d_init(void);
/* src/audio/asp.c: the audio task handler */
void asp_init(void);

const char *osPortInit(void)
{
    static const char *const rom_paths[] = { OS_PORT_ROM_PATHS, NULL };

    pal_sys_init();
    osPortTimeInit();
    osPortSpInit();
    f3d_init();
    asp_init();

    /* Not fatal: without the DSP the game runs muted (see pal_audio.h). */
    pal_audio_init();

    const char *err = pal_rom_open(rom_paths, OS_PORT_ROM_MODE, ge_rom_image,
                                   (uint32_t)(ge_rom_image_end - ge_rom_image));
    if (!err)
        pal_log("ge007: ROM %s, %lu bytes", pal_rom_path(),
                (unsigned long)pal_rom_size());
    return err;
}

void osPortExit(void)
{
    pal_audio_exit();
}

/* libultra's hardware bring-up (TLB, caches, PIF, clocks). osPortInit has
 * already done everything the port needs; the decomp's init() still calls
 * this, so it stays as a no-op. */
void osInitialize(void)
{
}
