/*
 * pal_sys_3ds.c - pal_sys on libctru.
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pal/pal_sys.h"

struct PalThread {
    Thread th;
};

static LightLock s_os_lock;
static CondVar   s_os_cv;

int pal_is_new3ds(void);

void pal_sys_init(void)
{
    osSetSpeedupEnable(true);   /* New 3DS: 804 MHz and the L2 cache */
    pal_log("ge007: %s 3DS", pal_is_new3ds() ? "New" : "Old");
    LightLock_Init(&s_os_lock);
    CondVar_Init(&s_os_cv);
}

PalThread *pal_thread_start(void (*fn)(void *), void *arg, size_t stack_size, int prio)
{
    PalThread *t = malloc(sizeof(*t));
    if (!t)
        return NULL;

    /* Core -2 = this process's default core (the app core). Not detached:
     * a detached thread frees its handle when it exits, and we may still
     * call pal_thread_set_priority on it afterwards. libultra threads
     * normally never exit, so not joining them costs nothing. */
    t->th = threadCreate(fn, arg, stack_size, prio, -2, false);
    if (!t->th) {
        free(t);
        return NULL;
    }
    return t;
}

int pal_is_new3ds(void)
{
    static int s_known = -1;
    if (s_known < 0) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        s_known = n3ds;
        /* developer token -old3ds (sdmc:/3ds/ge007/tokens.txt): take the
         * Old 3DS paths here; Azahar reports a New 3DS whatever its model
         * setting. Read directly: threads start before the game parses its
         * tokens. */
        char tok[128];
        if (s_known && pal_debug_read_text("sdmc:/3ds/ge007/tokens.txt", tok, sizeof tok) > 0 &&
            strstr(tok, "-old3ds") != NULL)
            s_known = 0;
    }
    return s_known;
}

PalThread *pal_thread_start_spare_core(void (*fn)(void *), void *arg, size_t stack_size, int prio)
{
    bool n3ds = pal_is_new3ds();
    if (!n3ds) {
        /* Old 3DS: no core 2, so core 0 with the game (core 1 was tried
         * in 1.0.15: the game alone keeps core 0 saturated, so no frames
         * were gained, and audio sharing core 1 underran more), at its
         * own slot among the game's threads (pal_sys.h). */
        (void)prio;
        return pal_thread_start(fn, arg, stack_size, PAL_PRIO_RSP_OLD3DS);
    }

    PalThread *t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    t->th = threadCreate(fn, arg, stack_size, prio, 2, false);
    if (!t->th) {
        free(t);
        return pal_thread_start(fn, arg, stack_size, prio);
    }
    return t;
}

static u32 s_prev_limit;

void pal_sys_restore_cpu_limit(void)
{
    if (s_prev_limit)
        APT_SetAppCpuTimeLimit(s_prev_limit);
}

PalThread *pal_thread_start_sys_core(void (*fn)(void *), void *arg, size_t stack_size, int prio)
{
    bool n3ds = pal_is_new3ds();
    /* The limit outlives the program (a .3dsx runs inside the Homebrew
     * Launcher): with 80% left set, the system crawled after the game
     * quit. Put back what was there on exit. */
    static bool s_restore_set;
    if (!s_restore_set && R_SUCCEEDED(APT_GetAppCpuTimeLimit(&s_prev_limit))) {
        s_restore_set = true;
        atexit(pal_sys_restore_cpu_limit);
    }
    /* Old 3DS: audio tasks (~8 ms, ~60 a second there) underran at 50%;
     * as much of core 1 as the system gives an application */
    if (R_FAILED(APT_SetAppCpuTimeLimit(n3ds ? PAL_SYSCORE_PERCENT : PAL_SYSCORE_PERCENT_OLD)))
        return pal_thread_start(fn, arg, stack_size, prio);

    PalThread *t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    t->th = threadCreate(fn, arg, stack_size, prio, 1, false);
    if (!t->th) {
        free(t);
        return pal_thread_start(fn, arg, stack_size, prio);
    }
    return t;
}

void pal_thread_set_priority(PalThread *t, int prio)
{
    Handle h = t ? threadGetHandle(t->th) : CUR_THREAD_HANDLE;
    svcSetThreadPriority(h, prio);
}

void pal_thread_yield(void)
{
    /* Sleeping 0 ns hands the core to another ready thread of the same
     * priority, like osYieldThread does. */
    svcSleepThread(0);
}

void pal_thread_exit(void)
{
    threadExit(0);
    __builtin_unreachable();
}

/* Nesting depth of s_os_lock held by this thread. CondVar_Wait drops and
 * re-takes the underlying LightLock without touching the count, which is
 * exactly the "release completely while blocked" behaviour we want. */
static __thread unsigned t_os_depth;

void pal_os_lock(void)
{
    if (t_os_depth++ == 0)
        LightLock_Lock(&s_os_lock);
}

void pal_os_unlock(void)
{
    if (--t_os_depth == 0)
        LightLock_Unlock(&s_os_lock);
}

void pal_os_wait(void)     { CondVar_Wait(&s_os_cv, &s_os_lock); }
void pal_os_wake_all(void) { CondVar_Broadcast(&s_os_cv); }

void pal_os_wait_timeout(int64_t ns)
{
    CondVar_WaitTimeout(&s_os_cv, &s_os_lock, ns);
}

void *pal_linear_alloc(size_t size, size_t align)
{
    return linearMemAlign(size, align);
}

_Static_assert(PAL_TICKS_PER_SEC == SYSCLOCK_ARM11, "tick rate mismatch");

uint64_t pal_ticks(void)
{
    return svcGetSystemTick();
}

int pal_debug_write(const char *path, const void *data, size_t size)
{
    char dir[256];
    const char *slash = strrchr(path, '/');
    if (slash && (size_t)(slash - path) < sizeof dir) {
        memcpy(dir, path, slash - path);
        dir[slash - path] = 0;
        mkdir(dir, 0777);
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, size, f);
    fclose(f);
    return n == size ? 0 : -1;
}

/* A developer text file into buf (NUL-terminated); its length, or -1. */
int pal_debug_read_text(const char *path, char *buf, int size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

int pal_debug_read_int(const char *path, int fallback)
{
    /* by hand: scanf would pull in libc's strtol, which the game defines too */
    char buf[16];
    FILE *f = fopen(path, "r");
    size_t n;
    int v = 0, sign = 1, digits = 0;
    if (!f)
        return fallback;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = buf;
    while (*p == ' ' || *p == '	')
        p++;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    for (; *p >= '0' && *p <= '9'; p++, digits++)
        v = v * 10 + (*p - '0');
    return digits ? sign * v : fallback;
}

void pal_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

}

/* ---- Perf log on the SD card -------------------------------------------
 * Lines go to stderr and to sdmc:/3ds/ge007/perf.log (emptied at boot), so
 * a run on real hardware can be read back. A thread at the 3DS main
 * thread's priority does the writes, so the caller never waits on the SD
 * card; if it falls behind, lines are dropped. */
#define PERF_LOG_PATH "sdmc:/3ds/ge007/perf.log"

static LightLock s_perf_lock;
static LightEvent s_perf_event;
static char s_perf_buf[4096];
static int s_perf_len;

static void perf_writer(void *arg)
{
    static char buf[sizeof s_perf_buf];
    FILE *f = fopen(PERF_LOG_PATH, "w");
    (void)arg;
    for (;;) {
        LightEvent_Wait(&s_perf_event);
        LightLock_Lock(&s_perf_lock);
        int n = s_perf_len;
        memcpy(buf, s_perf_buf, n);
        s_perf_len = 0;
        LightLock_Unlock(&s_perf_lock);
        if (f && n) {
            fwrite(buf, 1, n, f);
            fflush(f);
        }
    }
}

void pal_perf_log(const char *fmt, ...)
{
    static bool started;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = sizeof line - 2;
    line[n++] = '\n';
    line[n] = 0;
    fputs(line, stderr);

    if (!started) {
        started = true;
        LightLock_Init(&s_perf_lock);
        LightEvent_Init(&s_perf_event, RESET_ONESHOT);
        threadCreate(perf_writer, NULL, 8 * 1024, 0x30, -2, true);
    }
    LightLock_Lock(&s_perf_lock);
    if (s_perf_len + n <= (int)sizeof s_perf_buf) {
        memcpy(s_perf_buf + s_perf_len, line, n);
        s_perf_len += n;
    }
    LightLock_Unlock(&s_perf_lock);
    LightEvent_Signal(&s_perf_event);
}

void pal_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Also to stderr, which shows up in Luma/3dslink logs. */
    fprintf(stderr, "FATAL: %s\n", msg);


    errorConf err;
    errorInit(&err, ERROR_TEXT_WORD_WRAP, CFG_LANGUAGE_EN);
    errorText(&err, msg);
    errorDisp(&err);

    exit(1);
}

static uint64_t s_prof[PAL_PROF_COUNT];

void pal_prof_add(int id, uint64_t ticks)
{
    __atomic_fetch_add(&s_prof[id], ticks, __ATOMIC_RELAXED);
}

void pal_prof_take(uint64_t out[PAL_PROF_COUNT])
{
    for (int i = 0; i < PAL_PROF_COUNT; i++)
        out[i] = __atomic_exchange_n(&s_prof[i], 0, __ATOMIC_RELAXED);
}
