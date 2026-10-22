/*
 * os_time.h - libultra time and timers (src/ultra/os_time.c).
 *
 * Time is reported in N64 CPU count units (OS_CPU_COUNTER = 46.875 MHz),
 * converted from the 3DS system tick, so the game's OS_CYCLES_TO_USEC /
 * OS_USEC_TO_CYCLES macros and hard-coded cycle constants stay correct.
 *
 * libultra has no osCancelTimer: a timer is cancelled with osStopTimer,
 * and that is what the decomp calls.
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_TIME_H
#define ULTRA_OS_TIME_H

#include <ultra64.h>

OSTime osGetTime(void);
void   osSetTime(OSTime time);
u32    osGetCount(void);
int    osSetTimer(OSTimer *t, OSTime countdown, OSTime interval,
                  OSMesgQueue *mq, OSMesg msg);
int    osStopTimer(OSTimer *t);

/* ---- Port-only ------------------------------------------------------ */

/* N64 count units <-> nanoseconds, exact for the full 64-bit range. */
u64 osPortCyclesToNs(OSTime cycles);

#endif /* ULTRA_OS_TIME_H */
