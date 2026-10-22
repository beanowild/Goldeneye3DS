/*
 * os_message.c - libultra message queues on the PAL's global OS lock.
 *
 * On N64 these functions run with interrupts disabled and park the caller
 * on the queue's mtqueue/fullqueue thread lists. Here one global lock
 * stands in for "interrupts off", and one condition variable is broadcast
 * whenever any queue changes; each woken thread re-checks its own queue.
 * With a handful of threads on one core, waking everyone costs less than
 * keeping a condition variable per queue (the game owns OSMesgQueue, so
 * there is nowhere to store one without a side table).
 *
 * Preemption still matches N64: if a send wakes a higher-priority thread,
 * the kernel switches to it as soon as the sender releases the lock.
 */
#include <ultra64.h>

#include "pal/pal_sys.h"
#include "ultra/os_message.h"
#include "ultra/os_thread.h"

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

static int queue_full(const OSMesgQueue *mq)
{
    /* A zero-length queue is always full. On N64 that hangs the sender,
     * which is also what happens here with OS_MESG_BLOCK. */
    return mq->validCount >= mq->msgCount;
}

/* Wait until `mq` has room. Called with the lock held. Returns 0 if there
 * is room, -1 if full and the caller asked not to block. */
static s32 wait_for_space(OSMesgQueue *mq, s32 flag, void *ra)
{
    while (queue_full(mq)) {
        if (flag == OS_MESG_NOBLOCK)
            return -1;
        osPortNoteWait("osSendMesg", mq, ra);
        pal_os_wait();
        osPortNoteRunning();
    }
    return 0;
}

/* Append to a queue known to have room. Lock held. */
static void enqueue(OSMesgQueue *mq, OSMesg msg)
{
    s32 last = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[last] = msg;
    mq->validCount++;
    pal_os_wake_all();
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    pal_os_lock();
    if (wait_for_space(mq, flag, __builtin_return_address(0)) < 0) {
        pal_os_unlock();
        return -1;
    }
    enqueue(mq, msg);
    pal_os_unlock();
    return 0;
}

s32 osPortSendMesgLocked(OSMesgQueue *mq, OSMesg msg)
{
    if (queue_full(mq))
        return -1;
    enqueue(mq, msg);
    return 0;
}

/* Like osSendMesg, but puts the message at the front so it's received
 * next. The scheduler uses this for urgent events. */
s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    pal_os_lock();
    if (wait_for_space(mq, flag, __builtin_return_address(0)) < 0) {
        pal_os_unlock();
        return -1;
    }

    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;

    pal_os_wake_all();
    pal_os_unlock();
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    pal_os_lock();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            pal_os_unlock();
            return -1;
        }
        osPortNoteWait("osRecvMesg", mq, __builtin_return_address(0));
        pal_os_wait();
        osPortNoteRunning();
    }

    /* `msg` may be NULL when the caller only wants to wait for an event. */
    if (msg != NULL)
        *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    pal_os_wake_all();   /* a sender may be waiting for space */
    pal_os_unlock();
    return 0;
}
