/*
 * os_sp.h - libultra RSP/RDP task API (src/ultra/os_sp.c).
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h). osSpTaskStart is a
 * macro (osSpTaskLoad + osSpTaskStartGo) in libultra's headers.
 */
#ifndef ULTRA_OS_SP_H
#define ULTRA_OS_SP_H

#include <ultra64.h>

void          osSpTaskLoad(OSTask *tp);
void          osSpTaskStartGo(OSTask *tp);
void          osSpTaskYield(void);
OSYieldResult osSpTaskYielded(OSTask *tp);

u32  __osSpGetStatus(void);
void __osSpSetStatus(u32 value);
s32  __osSpSetPc(u32 pc);

void osDpSetStatus(u32 data);
s32  osDpSetNextBuffer(void *bufPtr, u64 size);
void osDpGetCounters(u32 *array);

/* ---- Port-only ------------------------------------------------------ */

/* What runs a task in place of the RSP microcode. M_GFXTASK goes to the
 * F3D display-list interpreter, M_AUDTASK to the audio-command executor.
 * Handlers run on the task thread with no locks held. */
typedef void (*OSPortTaskHandler)(OSTask *task);

void osPortSetTaskHandler(u32 type, OSPortTaskHandler handler);

/* Tasks of `type` completed so far (for the heartbeat log). */
u32 osPortTaskCount(u32 type);

/* Timing since the last call, for the heartbeat log: per task type, the
 * tasks run, their total run time and their total wait in the queue (all
 * in pal ticks). */
typedef struct { u32 n; u64 run, wait, max; } OSPortTaskTimes;
void osPortTakeTaskTimes(OSPortTaskTimes out[4]);

/* Waits until every audio task started so far has run. */
void osPortWaitAudioDone(void);

#endif /* ULTRA_OS_SP_H */
