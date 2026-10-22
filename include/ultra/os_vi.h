/*
 * os_vi.h - libultra Video Interface (src/ultra/os_vi.c).
 *
 * The retrace "interrupt" is osPortVblank() (declared in os_port.h so
 * main.c can call it), run once per 3DS vblank from the main thread.
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_VI_H
#define ULTRA_OS_VI_H

#include <ultra64.h>

void  osCreateViManager(OSPri pri);
void  osViSetMode(OSViMode *modep);
void  osViSetSpecialFeatures(u32 func);
void  osViSetXScale(f32 value);
void  osViSetYScale(f32 value);
void  osViBlack(u8 active);
void  osViSwapBuffer(void *frameBufPtr);
void  osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount);
void *osViGetCurrentFramebuffer(void);
void *osViGetNextFramebuffer(void);
u32   osViGetCurrentLine(void);
u32   osViGetCurrentMode(void);
void  osViRepeatLine(u8 active);

#endif /* ULTRA_OS_VI_H */
