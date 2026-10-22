/*
 * os_event.c - routes stand-in "hardware" events to the game's queues.
 */
#include <ultra64.h>

#include "pal/pal_sys.h"
#include "ultra/os_event.h"
#include "ultra/os_message.h"

typedef struct {
    OSMesgQueue *mq;
    OSMesg       msg;
} EventSlot;

static EventSlot s_events[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg)
{
    if ((u32)event >= OS_NUM_EVENTS) {
        pal_log("osSetEventMesg: unknown event %d", (int)event);
        return;
    }
    pal_os_lock();
    s_events[event].mq = mq;
    s_events[event].msg = msg;
    pal_os_unlock();
}

void osPortPostEventLocked(OSEvent event)
{
    if ((u32)event >= OS_NUM_EVENTS)
        return;
    EventSlot *e = &s_events[event];
    if (e->mq != NULL)
        osPortSendMesgLocked(e->mq, e->msg);
}

void osPortPostEvent(OSEvent event)
{
    pal_os_lock();
    osPortPostEventLocked(event);
    pal_os_unlock();
}
