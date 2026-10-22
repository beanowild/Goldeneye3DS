/*
 * os_time.c - libultra clock and timers on the 3DS system tick.
 *
 * Clock
 * -----
 * N64 time is the CPU count register, ticking at OS_CPU_COUNTER
 * (46,875,000 Hz). The 3DS system tick runs at 268,111,856 Hz. osGetTime
 * scales one to the other, counting from osPortTimeInit (at boot, as on
 * N64). osGetCount is the low 32 bits of the same value, so it wraps every
 * ~92 s, like the real count register.
 *
 * Timers
 * ------
 * libultra runs timers off the count/compare interrupt. Here a service
 * thread at PAL_PRIO_SERVICE (above every game thread, like an interrupt)
 * sleeps on the OS condition variable until the earliest deadline, posts
 * the expired timers' messages, and re-arms periodic ones.
 *
 * Active timers are kept on a circular list through the game-owned
 * OSTimer's own next/prev fields, as libultra does. A timer is active
 * exactly when `next` is non-NULL. `value` holds the absolute deadline,
 * not libultra's remaining countdown; only libultra code reads it.
 */
#include <ultra64.h>

#include "pal/pal_sys.h"
#include "ultra/os_message.h"
#include "ultra/os_port.h"
#include "ultra/os_time.h"

#define N64_COUNT_HZ  46875000ull   /* OS_CPU_COUNTER */
#define NS_PER_SEC    1000000000ull

#define TIMER_THREAD_STACK (16 * 1024)

static u64     s_base_ticks;   /* pal_ticks() when the clock read s_base_time */
static OSTime  s_base_time;
static OSTimer s_timers;       /* list head; sentinel, never a real timer */

/* a * num / den without overflowing 64 bits: split a into whole multiples
 * of den plus a remainder. Exact as long as den * num fits in 64 bits,
 * which holds for every rate used here. */
static u64 scale(u64 a, u64 num, u64 den)
{
    return (a / den) * num + (a % den) * num / den;
}

u64 osPortCyclesToNs(OSTime cycles)
{
    return scale(cycles, NS_PER_SEC, N64_COUNT_HZ);
}

/* ---- Clock ------------------------------------------------------------ */

OSTime osGetTime(void)
{
    return s_base_time + scale(pal_ticks() - s_base_ticks, N64_COUNT_HZ, PAL_TICKS_PER_SEC);
}

void osSetTime(OSTime time)
{
    pal_os_lock();
    s_base_ticks = pal_ticks();
    s_base_time = time;
    pal_os_wake_all();   /* timer deadlines just moved; let the thread recheck */
    pal_os_unlock();
}

u32 osGetCount(void)
{
    return (u32)osGetTime();
}

/* ---- Timer list (lock held) ------------------------------------------- */

static void timer_unlink(OSTimer *t)
{
    t->prev->next = t->next;
    t->next->prev = t->prev;
    t->next = NULL;
    t->prev = NULL;
}

static void timer_link(OSTimer *t)
{
    t->next = s_timers.next;
    t->prev = &s_timers;
    s_timers.next->prev = t;
    s_timers.next = t;
}

/* Post every expired timer and return the earliest remaining deadline,
 * or 0 if no timers are active. */
static OSTime fire_expired(OSTime now)
{
    OSTime earliest = 0;

    for (OSTimer *t = s_timers.next, *next; t != &s_timers; t = next) {
        next = t->next;

        if (t->value <= now) {
            if (t->mq != NULL)
                osPortSendMesgLocked(t->mq, t->msg);

            if (t->interval == 0) {
                timer_unlink(t);
                continue;
            }
            /* Periodic: stay on the original grid so there's no drift. If
             * far behind (after the app was suspended), skip the missed
             * periods rather than firing a burst of them. */
            t->value += t->interval;
            if (t->value <= now)
                t->value = now + t->interval;
        }

        if (earliest == 0 || t->value < earliest)
            earliest = t->value;
    }
    return earliest;
}

static void timer_thread(void *arg)
{
    (void)arg;

    pal_os_lock();
    for (;;) {
        OSTime now = osGetTime();
        OSTime next = fire_expired(now);

        /* Every osSendMesg broadcast wakes this thread too, so it also
         * wakes when timers are added or stopped. Each wake just rescans
         * the list (a handful of timers). */
        if (next == 0)
            pal_os_wait();
        else
            pal_os_wait_timeout((int64_t)osPortCyclesToNs(next - now));
    }
}

void osPortTimeInit(void)
{
    s_base_ticks = pal_ticks();
    s_base_time = 0;
    s_timers.next = &s_timers;
    s_timers.prev = &s_timers;

    if (!pal_thread_start(timer_thread, NULL, TIMER_THREAD_STACK, PAL_PRIO_SERVICE))
        pal_fatal("could not start the timer thread");
}

/* ---- Timers ------------------------------------------------------------ */

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval,
               OSMesgQueue *mq, OSMesg msg)
{
    pal_os_lock();

    /* Re-setting an active timer would corrupt libultra's list; here it
     * just restarts the timer. */
    if (t->next != NULL)
        timer_unlink(t);

    /* libultra: a zero countdown means "first fire after one interval". */
    t->value = osGetTime() + (countdown != 0 ? countdown : interval);
    t->interval = interval;
    t->mq = mq;
    t->msg = msg;
    timer_link(t);

    pal_os_wake_all();   /* the new deadline may be the earliest */
    pal_os_unlock();
    return 0;
}

int osStopTimer(OSTimer *t)
{
    pal_os_lock();
    if (t->next == NULL) {
        pal_os_unlock();
        return -1;   /* not running, as libultra reports */
    }
    timer_unlink(t);
    pal_os_unlock();
    return 0;
}
