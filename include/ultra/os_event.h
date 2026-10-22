/*
 * os_event.h - libultra system events (src/ultra/os_event.c).
 *
 * On N64, osSetEventMesg tells the interrupt handler which queue gets a
 * message when a hardware event fires (VI retrace, SP/DP task done, PI DMA
 * done, ...). Here the shim modules that stand in for that hardware call
 * osPortPostEvent at the matching moment:
 *
 *   OS_EVENT_VI       os_vi.c, every retrace
 *   OS_EVENT_PI       os_pi.c, after each DMA
 *   OS_EVENT_SP / DP  the gfx/audio task shim (osSpTaskStart), not yet written
 *   OS_EVENT_AI       the audio shim, not yet written
 *
 * The libultra prototype repeats the decomp's declaration so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_EVENT_H
#define ULTRA_OS_EVENT_H

#include <ultra64.h>

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg);

/* ---- Port-only ------------------------------------------------------ */

/* Send the message registered for `event`, if any. Never blocks; a full
 * queue drops the message, as on N64. */
void osPortPostEvent(OSEvent event);
void osPortPostEventLocked(OSEvent event);   /* OS lock already held */

#endif /* ULTRA_OS_EVENT_H */
