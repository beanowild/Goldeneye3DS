/*
 * os_vi.c - libultra Video Interface on the 3DS vblank.
 *
 * Retrace
 * -------
 * The 3DS main thread calls osPortVblank() once per LCD vblank (~59.83 Hz,
 * close enough to NTSC's 60 Hz that the game's timing is unchanged). It
 * does what the N64 VI interrupt and VI manager thread do between them:
 *
 *   1. latch the buffer from the last osViSwapBuffer as "current"
 *   2. every `retraceCount` retraces, post the osViSetEvent message (the
 *      game's scheduler counts these to pace itself to 30 FPS)
 *   3. post OS_EVENT_VI to anyone registered with osSetEventMesg
 *   4. if the shown buffer or the black state changed, call the present
 *      hook (pal_video.h) so the renderer puts that frame on screen
 *
 * Steps 1-3 run under the OS lock, like an interrupt handler. Step 4 runs
 * after the lock is released, so GPU work never blocks game threads.
 *
 * Mode / features / scale
 * -----------------------
 * The N64 VI's scan-out settings (resolution, gamma, dither, AA filter,
 * scaling) have no 3DS equivalent; the renderer decides how the image is
 * scaled to 400x240. They are recorded so that osViGetCurrentMode and a
 * future renderer can read them, and otherwise ignored.
 */
#include <ultra64.h>

#include "pal/pal_audio.h"
#include "pal/pal_sys.h"
#include "pal/pal_video.h"
#include "ultra/os_event.h"
#include "ultra/os_message.h"
#include "ultra/os_port.h"
#include "ultra/os_sp.h"
#include "ultra/os_thread.h"
#include "ultra/os_vi.h"

/* NTSC: 525 half-lines per frame at ~60 Hz. VI_CURRENT_REG, and so
 * osViGetCurrentLine, counts half-lines. */
#define HALF_LINES_PER_FRAME 525u
#define TICKS_PER_FRAME      (PAL_TICKS_PER_SEC / 60u)

/* All guarded by the OS lock. */
static OSMesgQueue *s_event_mq;
static OSMesg       s_event_msg;
static u32          s_event_every;      /* retraceCount; 0 = off */
static u32          s_event_counter;

static void *s_fb_current;
static void *s_fb_next;
static int   s_black;

static OSViMode *s_mode;
static u32       s_features;

/* Last values passed to the present hook (main thread only). */
static void *s_presented_fb;
static int   s_presented_black;

static u64 s_last_vblank_ticks;

/* ---- Retrace (main thread) --------------------------------------------- */

static volatile u32 s_vblanks_total;

/* LCD vblanks since boot (frametiming.c paces Old 3DS frames on them). */
u32 osPortVblankCount(void)
{
    return s_vblanks_total;
}

void osPortVblank(void)
{
    pal_os_lock();

    s_last_vblank_ticks = pal_ticks();
    s_vblanks_total++;

    /* osViSwapBuffer takes effect at the retrace after it is called. */
    s_fb_current = s_fb_next;

    if (s_event_mq != NULL && s_event_every != 0 && ++s_event_counter >= s_event_every) {
        s_event_counter = 0;
        osPortSendMesgLocked(s_event_mq, s_event_msg);
    }
    osPortPostEventLocked(OS_EVENT_VI);

    void *fb = s_fb_current;
    int black = s_black;
    pal_os_unlock();

    /* How long each new picture stayed up, in retraces: 1 is a 60 FPS
     * frame, 2 a 33 ms one, 3 or more a hitch (for the perf line). */
    static void *s_shown;
    static u32 s_held, s_held2, s_held3;
    s_held++;
    if (fb != s_shown) {
        if (s_held == 2)
            s_held2++;
        else if (s_held >= 3)
            s_held3++;
        s_held = 0;
        s_shown = fb;
    }

    /* Heartbeat every 5 s, so a run's log shows whether frames keep coming. */
    static u32 s_vblanks, s_last_gfx, s_stalled;
    if (++s_vblanks % 300 == 0) {
        extern void f3d_stats(u32 out[9]);
        u32 st[9];
        f3d_stats(st);
        pal_log("ge007: %lus: %lu gfx, %lu audio tasks; last frame %lu cmds %lu vtx %lu tris "
                "(%lu culled) %lu fills", (unsigned long)(s_vblanks / 60),
                (unsigned long)osPortTaskCount(M_GFXTASK), (unsigned long)osPortTaskCount(M_AUDTASK),
                (unsigned long)st[0], (unsigned long)st[1], (unsigned long)st[2],
                (unsigned long)st[3], (unsigned long)st[4]);
        pal_log("ge007: last frame %lu state rebuilds, %lu changes, %lu draws, %lu texture-only refreshes",
                (unsigned long)st[5], (unsigned long)st[6], (unsigned long)st[7], (unsigned long)st[8]);
        /* Per frame over the last 5 s: graphics/audio task run time and
         * queue wait (ms), and frames per second. */
        OSPortTaskTimes tt[4];
        osPortTakeTaskTimes(tt);
        float ms = 1000.0f / PAL_TICKS_PER_SEC;
        u32 g = tt[M_GFXTASK].n ? tt[M_GFXTASK].n : 1, a = tt[M_AUDTASK].n ? tt[M_AUDTASK].n : 1;
        float busy = 100.0f - 100.0f * (float)osPortTakeIdleTicks() / (5.0f * PAL_TICKS_PER_SEC);
        extern u32 gfxb_debug_tex_created;
        extern s32 g_CurrentStageToLoad;
        static u32 s_tex0;
        u32 tex_made = gfxb_debug_tex_created - s_tex0;
        s_tex0 = gfxb_debug_tex_created;
        extern void f3d_take_times(u64 out[4]);
        u64 ft[4];
        f3d_take_times(ft);
        pal_perf_log("ge007: %lus perf: stage %ld, %.1f fps (held 2:%lu 3+:%lu); game core %.0f%% busy; gfx task %.2f ms "
                "(max %.2f, waited %.2f; gpu wait %.2f, interpret %.2f (min %.2f), submit %.2f), %lu textures made; "
                "audio task %.2f ms (waited %.2f); %lu audio underruns",
                (unsigned long)(s_vblanks / 60), (long)g_CurrentStageToLoad, tt[M_GFXTASK].n / 5.0f, (unsigned long)s_held2, (unsigned long)s_held3,
                (double)busy, (double)(tt[M_GFXTASK].run * ms / g), (double)(tt[M_GFXTASK].max * ms),
                (double)(tt[M_GFXTASK].wait * ms / g), (double)(ft[0] * ms / g), (double)(ft[1] * ms / g), (double)(ft[3] * ms),
                (double)(ft[2] * ms / g), (unsigned long)tex_made,
                (double)(tt[M_AUDTASK].run * ms / a), (double)(tt[M_AUDTASK].wait * ms / a),
                (unsigned long)pal_audio_underruns());
        s_held2 = s_held3 = 0;
        {
            /* the game's own work per frame (frametiming.c: up to its wait
             * for the next frame), in osGetCount ticks */
            extern u32 g_PortWorkSum, g_PortWorkMax, g_PortWorkN;
            float cms = 1000.0f / (OS_CPU_COUNTER / 1.0f);
            if (g_PortWorkN)
                pal_perf_log("ge007: game work %.2f ms a frame (max %.2f)",
                             (double)(g_PortWorkSum / (float)g_PortWorkN * cms), (double)(g_PortWorkMax * cms));
            g_PortWorkSum = g_PortWorkMax = g_PortWorkN = 0;
        }
        {
            extern int g_gfx_debug_enabled;
            u64 pr[PAL_PROF_COUNT];
            pal_prof_take(pr);
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: prof/frame: lvlRender %.2f (bg %.2f: chr %.2f, loads %.2f; sky %.2f; portals %.2f; "
                             "propsTick %.2f), room DLs interp %.2f ms",
                             (double)(pr[PAL_PROF_LVLRENDER] * ms / g), (double)(pr[PAL_PROF_BGLEVEL] * ms / g),
                             (double)(pr[PAL_PROF_CHRPROPS] * ms / g), (double)(pr[PAL_PROF_ROOMLOAD] * ms / g),
                             (double)(pr[PAL_PROF_SKY] * ms / g), (double)(pr[PAL_PROF_PORTALS] * ms / g),
                             (double)(pr[PAL_PROF_PROPSTICK] * ms / g), (double)(pr[PAL_PROF_ROOM_DL] * ms / g));
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: core 0/frame: main thread %.2f ms (lvlRender in it), audio frame %.2f ms (DSP synth part %.2f)",
                             (double)(pr[PAL_PROF_MAINTICK] * ms / g), (double)(pr[PAL_PROF_AUDIOFRAME] * ms / g), (double)(pr[PAL_PROF_DSPSYN] * ms / g));
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: propsTick/frame: chr %.2f (AI %.2f, bone mtx %.2f), obj %.2f, player %.2f ms",
                             (double)(pr[PAL_PROF_CHRTICK] * ms / g), (double)(pr[PAL_PROF_CHRAI] * ms / g),
                             (double)(pr[PAL_PROF_CHRMTX] * ms / g), (double)(pr[PAL_PROF_OBJTICK] * ms / g),
                             (double)(pr[PAL_PROF_PLAYERTICK] * ms / g));
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: interp/frame: vertices %.2f, state %.2f (textures %.2f), tex loads %.2f ms",
                             (double)(pr[PAL_PROF_F3D_VTX] * ms / g), (double)(pr[PAL_PROF_F3D_STATE] * ms / g),
                             (double)(pr[PAL_PROF_F3D_STATETEX] * ms / g),
                             (double)(pr[PAL_PROF_F3D_TEXLOAD] * ms / g));
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: model nodes/frame: 01 heading %.2f, 02 position %.2f, 15 subposition %.2f ms",
                             (double)(pr[PAL_PROF_NODE01] * ms / g), (double)(pr[PAL_PROF_NODE02] * ms / g),
                             (double)(pr[PAL_PROF_NODE15] * ms / g));
            if (g_gfx_debug_enabled)
                pal_perf_log("ge007: lvlRender stretches/frame: setup %.2f, to bg %.2f, bg+debug %.2f, fx %.2f, rest %.2f ms",
                             (double)(pr[PAL_PROF_MARK1] * ms / g), (double)(pr[PAL_PROF_MARK2] * ms / g),
                             (double)(pr[PAL_PROF_MARK3] * ms / g), (double)(pr[PAL_PROF_MARK4] * ms / g),
                             (double)(pr[PAL_PROF_MARK5] * ms / g));
        }
    }
    /* Watchdog: no graphics task for 3 s after the first means a hang;
     * say once what every game thread is doing. */
    if (s_vblanks % 60 == 0) {
        u32 gfx = osPortTaskCount(M_GFXTASK);
        s_stalled = (gfx != 0 && gfx == s_last_gfx) ? s_stalled + 1 : 0;
        s_last_gfx = gfx;
        if (s_stalled == 3) {
            pal_log("ge007: no frames for 3 s; game threads:");
            osPortDumpThreads();
        }
    }

    if (fb != s_presented_fb || black != s_presented_black) {
        s_presented_fb = fb;
        s_presented_black = black;
        pal_video_present(fb, black != 0);
    }
}

/* ---- libultra API ------------------------------------------------------- */

void osCreateViManager(OSPri pri)
{
    /* The main thread's retrace pump does the manager's job. */
    (void)pri;
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount)
{
    pal_log("ge007: osViSetEvent every %lu retrace(s)", (unsigned long)retraceCount);
    pal_os_lock();
    s_event_mq = mq;
    s_event_msg = msg;
    s_event_every = retraceCount;
    s_event_counter = 0;
    pal_os_unlock();
}

void osViSwapBuffer(void *frameBufPtr)
{
    pal_os_lock();
    s_fb_next = frameBufPtr;
    pal_os_unlock();
}

void *osViGetCurrentFramebuffer(void)
{
    pal_os_lock();
    void *fb = s_fb_current;
    pal_os_unlock();
    return fb;
}

void *osViGetNextFramebuffer(void)
{
    pal_os_lock();
    void *fb = s_fb_next;
    pal_os_unlock();
    return fb;
}

void osViBlack(u8 active)
{
    /* Like a swap, this takes effect at the next retrace. */
    pal_os_lock();
    s_black = active != 0;
    pal_os_unlock();
}

void osViSetMode(OSViMode *modep)
{
    pal_os_lock();
    s_mode = modep;
    pal_os_unlock();
}

u32 osViGetCurrentMode(void)
{
    pal_os_lock();
    u32 type = s_mode ? s_mode->type : 0;
    pal_os_unlock();
    return type;
}

void osViSetSpecialFeatures(u32 func)
{
    /* OS_VI_GAMMA_ON/OFF, DITHER_FILTER_ON/OFF, ... come in on/off pairs.
     * Record the last call for the renderer; nothing else uses it. */
    pal_os_lock();
    s_features = func;
    pal_os_unlock();
}

void osViSetXScale(f32 value) { (void)value; }
void osViSetYScale(f32 value) { (void)value; }

/* Where the "beam" would be, estimated from time since the last vblank.
 * Games use this for raster timing and speed meters; an estimate is all
 * they need. */
u32 osViGetCurrentLine(void)
{
    u64 elapsed = pal_ticks() - s_last_vblank_ticks;
    u64 line = elapsed * HALF_LINES_PER_FRAME / TICKS_PER_FRAME;
    return line < HALF_LINES_PER_FRAME ? (u32)line : HALF_LINES_PER_FRAME - 1;
}

/* Line doubling for low-res modes: a scan-out setting, like the others. */
void osViRepeatLine(u8 active) { (void)active; }
