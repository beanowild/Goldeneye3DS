/*
 * os_message.h - libultra message queues (src/ultra/os_message.c).
 *
 * The prototypes repeat the decomp's libultra declarations. They are not
 * needed to compile, but if this header and <ultra64.h> ever disagree the
 * compiler stops with an error, which shows the shim signature drifted
 * from what the game expects.
 *
 * OSMesgQueue keeps libultra's layout. The game allocates queues itself
 * and reads `validCount` / `msgCount` through the MQ_IS_EMPTY / MQ_IS_FULL
 * macros. `mtqueue` and `fullqueue` (libultra's lists of blocked threads)
 * are unused; blocking goes through pal_os_wait() instead.
 */
#ifndef ULTRA_OS_MESSAGE_H
#define ULTRA_OS_MESSAGE_H

#include <ultra64.h>

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count);
s32  osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag);
s32  osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag);
s32  osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag);

/* ---- Port-only ------------------------------------------------------ */

/* osSendMesg(mq, msg, OS_MESG_NOBLOCK) for callers that already hold the
 * OS lock (pal_os_lock is not recursive). This is how the "interrupt"
 * sources (VI retrace, timers, events) post, the way libultra posts from
 * its interrupt handler. Returns -1 if the queue is full; the message is
 * dropped, as on N64. */
s32 osPortSendMesgLocked(OSMesgQueue *mq, OSMesg msg);

#endif /* ULTRA_OS_MESSAGE_H */
