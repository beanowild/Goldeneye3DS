/*
 * os_port.h - setup for the libultra shim, callable from main.c.
 *
 * Deliberately does not include <ultra64.h>: main.c includes <3ds.h>, and
 * the two can't share a translation unit.
 */
#ifndef ULTRA_OS_PORT_H
#define ULTRA_OS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ROM locations tried in order. */
#define OS_PORT_ROM_PATHS             \
    "sdmc:/3ds/ge007/rom.z64",        \
    "sdmc:/3ds/ge007/baserom.us.z64"

/* Set up the shim: OS lock, then the ROM. Call once on the main thread
 * before entering game code. Returns NULL on success, or a message for
 * the user (e.g. ROM missing). */
const char *osPortInit(void);

/* The VI retrace "interrupt": call once per 3DS vblank from the main
 * thread (src/ultra/os_vi.c). */
void osPortVblank(void);

/* Shut down what needs it before the app exits (the DSP, which otherwise
 * can hang the return to the Homebrew Launcher). */
void osPortExit(void);

/* Internal to the shim, called by osPortInit: start the clock and timer
 * thread, and the RSP task thread. */
void osPortTimeInit(void);
void osPortSpInit(void);

#ifdef __cplusplus
}
#endif

#endif /* ULTRA_OS_PORT_H */
