/*
 * os_sp.c - RSP/RDP tasks, run by C handlers instead of microcode.
 *
 * On N64 the scheduler loads a task (osSpTaskLoad) and starts it
 * (osSpTaskStartGo). The RSP then runs it in parallel with the CPU and
 * raises the SP interrupt when done, plus the DP interrupt once the RDP
 * has drawn a graphics task's output. The scheduler waits for those
 * events, which arrive through osSetEventMesg.
 *
 * Here task threads play the RSP: osSpTaskStartGo queues the task and
 * returns at once, a thread runs the handler registered for the task's
 * type, and then posts OS_EVENT_SP (OS_EVENT_DP for graphics tasks, whose
 * SP event comes as they start; see task_thread). Graphics tasks run on
 * one thread and audio tasks on another, so audio mixing overlaps the
 * drawing, as the N64's RSP and RDP overlapped.
 *
 * Task thread priority: PAL_PRIO_RSP, above every game thread. The RSP is
 * separate hardware on N64, and game code busy-waits on its results (the
 * music code spins until the audio task has stopped a sequence), so the
 * task thread must be able to preempt any game thread; below them it
 * starves and the game deadlocks. On a New 3DS the graphics thread runs
 * on the spare core (core 2), in parallel with the game, as the RSP did;
 * the audio thread runs on the system core (core 1) everywhere.
 *
 * Yielding: libultra lets the scheduler pause a long graphics task to slip
 * in an audio task. Tasks here run to completion, so osSpTaskYielded
 * always says "not yielded" and the scheduler treats the graphics task as
 * finished normally. It then waits for that task's SP event before
 * starting the audio task, as it would on N64.
 */
#include <ultra64.h>

#include "pal/pal_audio.h"
#include "pal/pal_sys.h"
#include "ultra/os_event.h"
#include "ultra/os_port.h"
#include "ultra/os_sp.h"

#define TASK_QUEUE_LEN     4
#define TASK_THREAD_STACK  (64 * 1024)
#define NUM_TASK_TYPES     4      /* M_GFXTASK=1, M_AUDTASK=2, M_VIDTASK=3 */

#define SP_STATUS_HALT_BIT 0x0001u

/* Two lanes, each a worker thread with its own queue: graphics, and
 * everything else (audio). All guarded by the OS lock. */
enum { LANE_GFX, LANE_AUDIO, NUM_LANES };

typedef struct {
    OSTask *queue[TASK_QUEUE_LEN];
    u64     queued_at[TASK_QUEUE_LEN];
    int     head, count;              /* queue[head] is running or next */
} Lane;

static Lane s_lanes[NUM_LANES];
static int  s_rsp_busy;               /* tasks started whose SP event is still due */

static OSPortTaskHandler s_handlers[NUM_TASK_TYPES];
static u32               s_done[NUM_TASK_TYPES];
static OSPortTaskTimes   s_times[NUM_TASK_TYPES];

static void task_thread(void *arg)
{
    Lane *lane = arg;

    pal_os_lock();
    for (;;) {
        while (lane->count == 0)
            pal_os_wait();

        OSTask *task = lane->queue[lane->head];
        u32 type = task->t.type;
        static u32 s_seen_types;
        if (type < 32 && !(s_seen_types & (1u << type))) {
            s_seen_types |= 1u << type;
            pal_log("ge007: first RSP task of type %lu", (unsigned long)type);
        }
        OSPortTaskHandler handler = type < NUM_TASK_TYPES ? s_handlers[type] : NULL;
        u64 queued = lane->queued_at[lane->head];

        /* Every task's SP event comes as it starts, so one lane's work
         * never holds up the other's next task.
         * - Graphics: on N64 the RSP hands its output to the RDP and is free
         *   for the audio task while the RDP draws. The scheduler reports
         *   the frame done (and starts the next graphics task) only at the
         *   DP event, which comes when the task has run.
         * - Audio: the game hands the mixed buffer to the AI next, and
         *   osAiSetNextBuffer waits for this lane to finish
         *   (osPortWaitAudioDone). __osSpGetStatus stays busy until then. */
        if (type == M_GFXTASK)
            s_rsp_busy--;
        osPortPostEventLocked(OS_EVENT_SP);
        pal_os_unlock();

        /* With no handler (renderer/audio not written yet) the task
         * completes empty, so the game's scheduler keeps running. */
        u64 start = pal_ticks();
        if (handler)
            handler(task);
        u64 end = pal_ticks();

        pal_os_lock();
        if (type < NUM_TASK_TYPES) {
            s_done[type]++;
            s_times[type].n++;
            s_times[type].run += end - start;
            s_times[type].wait += start - queued;
            if (end - start > s_times[type].max)
                s_times[type].max = end - start;
        }
        lane->head = (lane->head + 1) % TASK_QUEUE_LEN;
        lane->count--;

        if (type == M_GFXTASK)
            osPortPostEventLocked(OS_EVENT_DP);
        else
            s_rsp_busy--;
        pal_os_wake_all();   /* a full queue may now have room; the AI may wait */
    }
}

void osPortSpInit(void)
{
    /* Old 3DS with the synthesizer on the DSP (audio/dsp_synth.c): the
     * audio tasks are empty, so core 1 is free for the graphics tasks and
     * core 0 is left to the game, as the N64 split CPU and RSP. */
    if (pal_swap_cores()) {
        pal_log("ge007: graphics tasks on core 0, the game on core 1 (-swapcores)");
        if (!pal_thread_start(task_thread, &s_lanes[LANE_GFX], TASK_THREAD_STACK, PAL_PRIO_RSP))
            pal_fatal("could not start the graphics task thread");
    } else if (!pal_is_new3ds() && pal_audio_voices_available() && !pal_debug_has_token("-cpuaudio")) {
        pal_log("ge007: graphics tasks on core 1");
        if (!pal_thread_start_sys_core(task_thread, &s_lanes[LANE_GFX], TASK_THREAD_STACK, PAL_PRIO_RSP))
            pal_fatal("could not start the graphics task thread");
    } else if (!pal_thread_start_spare_core(task_thread, &s_lanes[LANE_GFX], TASK_THREAD_STACK, PAL_PRIO_RSP))
        pal_fatal("could not start the graphics task thread");
    if (!pal_thread_start_sys_core(task_thread, &s_lanes[LANE_AUDIO], TASK_THREAD_STACK, PAL_PRIO_RSP))
        pal_fatal("could not start the audio task thread");
}

void osPortSetTaskHandler(u32 type, OSPortTaskHandler handler)
{
    if (type >= NUM_TASK_TYPES)
        pal_fatal("osPortSetTaskHandler: bad task type %lu", (unsigned long)type);
    pal_os_lock();
    s_handlers[type] = handler;
    pal_os_unlock();
}

void osPortWaitAudioDone(void)
{
    pal_os_lock();
    while (s_lanes[LANE_AUDIO].count != 0)
        pal_os_wait();
    pal_os_unlock();
}

void osPortTakeTaskTimes(OSPortTaskTimes out[4])
{
    pal_os_lock();
    for (int i = 0; i < NUM_TASK_TYPES; i++) {
        out[i] = s_times[i];
        s_times[i] = (OSPortTaskTimes){ 0, 0, 0, 0 };
    }
    pal_os_unlock();
}

u32 osPortTaskCount(u32 type)
{
    pal_os_lock();
    u32 n = type < NUM_TASK_TYPES ? s_done[type] : 0;
    pal_os_unlock();
    return n;
}

/* ---- libultra API ------------------------------------------------------- */

void osSpTaskLoad(OSTask *tp)
{
    /* On N64 this DMAs the ucode and task header into the RSP. Nothing to
     * load here: the handler reads the OSTask directly when it runs. */
    (void)tp;
}

void osSpTaskStartGo(OSTask *tp)
{
    pal_os_lock();
    /* The N64 RSP holds a single task. libultra's scheduler never starts
     * a second before the first's SP event, so the queue rarely holds more
     * than one; waiting here only covers misuse. */
    Lane *lane = &s_lanes[tp->t.type == M_GFXTASK ? LANE_GFX : LANE_AUDIO];
    while (lane->count == TASK_QUEUE_LEN)
        pal_os_wait();
    lane->queue[(lane->head + lane->count) % TASK_QUEUE_LEN] = tp;
    lane->queued_at[(lane->head + lane->count) % TASK_QUEUE_LEN] = pal_ticks();
    lane->count++;
    s_rsp_busy++;
    pal_os_wake_all();
    pal_os_unlock();
}

void osSpTaskYield(void)
{
    /* Tasks always run to completion; see the top of the file. */
}

OSYieldResult osSpTaskYielded(OSTask *tp)
{
    (void)tp;
    return 0;
}

u32 __osSpGetStatus(void)
{
    pal_os_lock();
    int busy = s_rsp_busy != 0;
    pal_os_unlock();
    return busy ? 0 : SP_STATUS_HALT_BIT;
}

void __osSpSetStatus(u32 value) { (void)value; }
s32  __osSpSetPc(u32 pc)        { (void)pc; return 0; }

/* RDP control: the RDP's command FIFO is fed by the renderer, not by
 * these registers, so they are accepted and ignored. */
void osDpSetStatus(u32 data) { (void)data; }

s32 osDpSetNextBuffer(void *bufPtr, u64 size)
{
    (void)bufPtr;
    (void)size;
    return 0;
}

void osDpGetCounters(u32 *array)
{
    /* clock, cmd busy, pipe busy, tmem load: all zero ("idle"). */
    for (int i = 0; i < 4; i++)
        array[i] = 0;
}
