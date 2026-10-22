/*
 * os_ai.h - libultra Audio Interface (src/ultra/os_ai.c).
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_AI_H
#define ULTRA_OS_AI_H

#include <ultra64.h>

s32 osAiSetFrequency(u32 frequency);
s32 osAiSetNextBuffer(void *bufPtr, u32 size);
u32 osAiGetLength(void);
u32 osAiGetStatus(void);

#endif /* ULTRA_OS_AI_H */
