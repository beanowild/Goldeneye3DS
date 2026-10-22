/*
 * os_thread.c - libultra threads as libctru threads.
 *
 * Model
 * -----
 * The game allocates its OSThread structs itself, so we can't add fields.
 * Each one gets a slot in a side table that records its entry point and,
 * once started, its PalThread. The slot table and the OSThread fields we
 * write (priority, state, id) are protected by the global OS lock.
 *
 * libultra creates threads stopped and runs them on osStartThread. libctru
 * can't create a suspended thread, so osCreateThread only records the
 * request and osStartThread creates the native thread.
 *
 * Priorities
 * ----------
 * libultra: 0..255, higher runs first. Game threads use 1..127, libultra's
 * own managers 128..255, and the idle thread 0.
 * 3DS: 0x18..0x3F, lower runs first, and only about 22 levels are free
 * between PAL_PRIO_GAME_TOP and the 3DS main thread.
 *
 * A linear scale would squeeze 255 levels into 22 and give threads that
 * were distinct on N64 the same priority. Game code relies on "the higher
 * thread always runs first" instead of locking, so that would cause races.
 * Instead priorities are mapped by *rank*: the highest distinct priority
 * in use gets PAL_PRIO_GAME_TOP, the next PAL_PRIO_GAME_TOP+1, and so on.
 * Order is preserved exactly as long as no more than 22 distinct levels are
 * in use; GoldenEye uses far fewer. The ranks are recomputed whenever a
 * thread is created or changes priority.
 *
 * The idle thread (OS_PRIORITY_IDLE) spins forever on N64. It always gets
 * PAL_PRIO_LOWEST, below the 3DS main thread, so its spin can't starve the
 * home menu or any system work.
 */
#include <ultra64.h>

#include "pal/pal_sys.h"
#include "ultra/os_thread.h"

typedef struct {
    OSThread  *os;
    void     (*entry)(void *);
    void      *arg;
    PalThread *pal;       /* NULL until osStartThread */
    int        prio_3ds;  /* last priority applied to `pal` */
    int        parked;    /* stopped itself with osStopThread */
    const char *wait_what; /* diagnostics: what it's blocked on, or NULL */
    const void *wait_obj;
    void       *wait_ra;
} ThreadSlot;

static ThreadSlot s_slots[OS_PORT_MAX_THREADS];
static int        s_num_slots;

/* Each native thread knows which OSThread it is running. */
static __thread OSThread *t_self;

OSThread *osPortCurrentThread(void)
{
    return t_self;
}

/* ---- Slot table (lock held) ------------------------------------------ */

static ThreadSlot *find_slot(OSThread *t);

void osPortNoteWait(const char *what, const void *obj, void *ra)
{
    ThreadSlot *s = t_self ? find_slot(t_self) : NULL;
    if (s) {
        s->wait_what = what;
        s->wait_obj = obj;
        s->wait_ra = ra;
    }
}

void osPortNoteRunning(void)
{
    ThreadSlot *s = t_self ? find_slot(t_self) : NULL;
    if (s)
        s->wait_what = NULL;
}

void osPortDumpThreads(void)
{
    pal_os_lock();
    for (int i = 0; i < s_num_slots; i++) {
        ThreadSlot *s = &s_slots[i];
        if (!s->pal)
            continue;
        if (s->parked)
            pal_log("  thread %d (pri %d): stopped", (int)s->os->id, (int)s->os->priority);
        else if (s->wait_what)
            pal_log("  thread %d (pri %d): waiting in %s %p, called from %p", (int)s->os->id,
                    (int)s->os->priority, s->wait_what, s->wait_obj, s->wait_ra);
        else
            pal_log("  thread %d (pri %d): not blocked (running or ready)",
                    (int)s->os->id, (int)s->os->priority);
    }
    pal_os_unlock();
}

static ThreadSlot *find_slot(OSThread *t)
{
    for (int i = 0; i < s_num_slots; i++)
        if (s_slots[i].os == t)
            return &s_slots[i];
    return NULL;
}

static ThreadSlot *find_or_add_slot(OSThread *t)
{
    ThreadSlot *s = find_slot(t);
    if (s)
        return s;
    if (s_num_slots == OS_PORT_MAX_THREADS)
        pal_fatal("osCreateThread: more than %d threads", OS_PORT_MAX_THREADS);
    s = &s_slots[s_num_slots++];
    s->os = t;
    s->pal = NULL;
    s->parked = 0;
    return s;
}

/* ---- Priority mapping (lock held) ------------------------------------ */

static int map_priority(OSPri pri)
{
    if (pri == OS_PRIORITY_IDLE)
        return PAL_PRIO_LOWEST;

    /* rank = number of distinct non-idle priorities above `pri` */
    int rank = 0;
    for (int i = 0; i < s_num_slots; i++) {
        OSPri p = s_slots[i].os->priority;
        if (p <= pri)
            continue;
        int seen = 0;
        for (int j = 0; j < i; j++) {
            if (s_slots[j].os->priority == p) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            rank++;
    }

    /* Old 3DS: the graphics lane shares core 0 and takes the slot below
     * the threads at PAL_RSP_N64_PRIORITY and up (pal_thread_start_spare_core),
     * so the ones under it move down a rank. */
    if (pri < PAL_RSP_N64_PRIORITY && !pal_is_new3ds())
        rank++;

    int prio = PAL_PRIO_GAME_TOP + rank;
    if (prio > PAL_PRIO_GAME_MIN) {
        /* Too many distinct levels: the lowest ones share a priority. */
        prio = PAL_PRIO_GAME_MIN;
    }
    return prio;
}

/* Reapply priorities to every running thread. A new or changed priority
 * can shift the rank of the others. */
static void apply_priorities(void)
{
    for (int i = 0; i < s_num_slots; i++) {
        ThreadSlot *s = &s_slots[i];
        if (!s->pal)
            continue;
        int prio = map_priority(s->os->priority);
        if (prio != s->prio_3ds) {
            s->prio_3ds = prio;
            pal_thread_set_priority(s->pal, prio);
        }
    }
}

/* ---- Thread entry ----------------------------------------------------- */

static void trampoline(void *arg)
{
    ThreadSlot *s = arg;   /* slots never move; the table is static */

    t_self = s->os;
    s->os->state = OS_STATE_RUNNING;

    s->entry(s->arg);

    /* On N64 a returning entry function lands in __osCleanupThread,
     * which destroys the thread. Here the native thread just ends. */
    pal_os_lock();
    s->os->state = OS_STATE_STOPPED;
    pal_os_unlock();
}

/* ---- libultra API ------------------------------------------------------ */

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                    void *sp, OSPri pri)
{
    (void)sp;   /* see OS_PORT_THREAD_STACK */

    pal_os_lock();

    ThreadSlot *s = find_or_add_slot(t);
    if (s->pal && t->state != OS_STATE_STOPPED)
        pal_fatal("osCreateThread: thread %d is still running", (int)t->id);
    s->pal = NULL;
    s->entry = entry;
    s->arg = arg;

    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;

    apply_priorities();   /* a new level may shift running threads */
    pal_os_unlock();
}

void osStartThread(OSThread *t)
{
    pal_os_lock();

    ThreadSlot *s = find_slot(t);
    if (!s)
        pal_fatal("osStartThread: thread was never created");

    /* Already started: resume it if it parked itself with osStopThread,
     * otherwise there's nothing to do. */
    if (s->pal) {
        if (s->parked) {
            s->parked = 0;
            t->state = OS_STATE_RUNNABLE;
            pal_os_wake_all();
        }
        pal_os_unlock();
        return;
    }

    t->state = OS_STATE_RUNNABLE;
    s->prio_3ds = map_priority(t->priority);
    s->pal = pal_thread_start(trampoline, s, OS_PORT_THREAD_STACK, s->prio_3ds);
    if (!s->pal)
        pal_fatal("osStartThread: could not create thread %d", (int)t->id);
    pal_log("ge007: thread %d started (N64 pri %d -> 3DS 0x%02X)",
            (int)t->id, (int)t->priority, s->prio_3ds);

    /* If the new thread outranks us, the kernel has already switched to it,
     * as N64 would. Its first OS call waits on the lock until we release
     * it here. */
    pal_os_unlock();
}

void osSetThreadPri(OSThread *t, OSPri pri)
{
    if (t == NULL)
        t = t_self;
    if (t == NULL)
        return;   /* called from a non-game thread; nothing to change */

    pal_os_lock();
    t->priority = pri;
    apply_priorities();
    pal_os_unlock();
}

OSPri osGetThreadPri(OSThread *t)
{
    if (t == NULL)
        t = t_self;
    return t ? t->priority : 0;
}

OSId osGetThreadId(OSThread *t)
{
    if (t == NULL)
        t = t_self;
    return t ? t->id : 0;
}

void osYieldThread(void)
{
    pal_thread_yield();
}

/* Stopping yourself works as on N64: the thread parks until someone calls
 * osStartThread on it. Stopping another thread would mean suspending it
 * from outside, which Horizon doesn't offer to applications, so that's
 * logged and ignored. libultra uses that mainly for its debugger and
 * crash screen. */
void osStopThread(OSThread *t)
{
    if (t == NULL)
        t = t_self;
    if (t == NULL)
        return;

    if (t != t_self) {
        pal_log("osStopThread: can't stop thread %d from outside; ignored", (int)t->id);
        return;
    }

    pal_os_lock();
    ThreadSlot *s = find_slot(t);
    if (s) {
        s->parked = 1;
        t->state = OS_STATE_STOPPED;
        while (s->parked)
            pal_os_wait();
        t->state = OS_STATE_RUNNING;
    }
    pal_os_unlock();
}

/* Destroying yourself ends the native thread. Destroying another thread
 * has the same problem as stopping one, so it's logged and ignored. */
void osDestroyThread(OSThread *t)
{
    if (t == NULL)
        t = t_self;
    if (t == NULL)
        return;

    if (t != t_self) {
        pal_log("osDestroyThread: can't destroy thread %d from outside; ignored",
                (int)t->id);
        return;
    }

    pal_os_lock();
    t->state = OS_STATE_STOPPED;
    pal_os_unlock();
    pal_thread_exit();
}

/* No CPU exceptions reach the game: a crash goes to Luma3DS's exception
 * screen, so there is never a "faulted thread" for the crash handler. */
OSThread *__osGetCurrFaultedThread(void)
{
    return NULL;
}


/* Idle time: the spin notes each short step between tick reads; a long
 * gap means it was preempted (the game was running) and isn't counted.
 * Only the idle thread writes the counter (a u32, so reads aren't torn);
 * the reader keeps its own last value. Resetting it from the reader raced
 * the spin's add, and idle time came out above 100%. */
static volatile u32 s_idle_ticks;

void osPortIdleLoop(void)
{
    u64 last = pal_ticks();
    for (;;) {
        u64 now = pal_ticks();
        if (now - last < PAL_TICKS_PER_SEC / 10000)   /* under 0.1 ms */
            s_idle_ticks += now - last;
        last = now;
    }
}

u64 osPortTakeIdleTicks(void)
{
    static u32 s_taken;
    u32 now = s_idle_ticks;
    u32 v = now - s_taken;   /* wraps every 16 s of idle; read every 5 s */
    s_taken = now;
    return v;
}
