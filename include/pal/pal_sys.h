/*
 * pal_sys.h - threads, the OS lock, and fatal errors.
 *
 * The libultra shim (src/ultra/) is built on these instead of calling
 * libctru directly, so it never has to include <3ds.h> alongside the
 * decomp's <ultra64.h> (both define u32, s32, ... and other names).
 */
#ifndef PAL_SYS_H
#define PAL_SYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Priorities ---------------------------------------------------------
 * 3DS priorities run 0x18 (highest an app may use) to 0x3F (lowest).
 * Lower number = more important, the opposite of libultra.
 *
 *   SERVICE  port threads standing in for N64 interrupts (VI retrace,
 *            audio). They must preempt every game thread.
 *   RSP      the thread running RSP tasks (os_sp.c). Above every game
 *            thread: the RSP is separate hardware on N64, and game code
 *            busy-waits on it (e.g. for the music player to stop), which
 *            would starve it forever at a lower priority.
 *   GAME_TOP the highest-priority game thread. Other game threads take the
 *            numbers below it in order (see os_thread.c).
 *   LOWEST   libultra's idle thread, which spins forever. It sits below
 *            the 3DS main thread (0x30) so the spin never starves APT.
 */
enum {
    PAL_PRIO_SERVICE  = 0x19,
    PAL_PRIO_RSP      = 0x1A,
    PAL_PRIO_GAME_TOP = 0x1B,
    PAL_PRIO_GAME_MIN = 0x2F,   /* stay above the 3DS main thread at 0x30 */
    PAL_PRIO_LOWEST   = 0x3F,
};

typedef struct PalThread PalThread;

/* Start `fn(arg)` on a new thread with its own stack. Every thread goes on
 * the same core: libultra assumes one CPU with strict priority preemption,
 * and game code relies on that instead of locking. Horizon schedules a
 * single core the same way, so keeping one core keeps those assumptions true.
 * Returns NULL on failure. */
PalThread *pal_thread_start(void (*fn)(void *), void *arg, size_t stack_size, int prio);

/* The same, but on a spare core when there is one (the New 3DS's core 2),
 * else like pal_thread_start. Only for threads that stand in for separate
 * N64 hardware (the RSP), which ran in parallel with the CPU anyway. */
PalThread *pal_thread_start_spare_core(void (*fn)(void *), void *arg, size_t stack_size, int prio);

/* The same on the system core (core 1), which the app may use for up to
 * PAL_SYSCORE_PERCENT of its time; else like pal_thread_start. */
#define PAL_SYSCORE_PERCENT 50
#define PAL_SYSCORE_PERCENT_OLD 80   /* Old 3DS: audio underran at 50 */

/* Old 3DS: the graphics lane (standing in for the RSP) shares core 0 with
 * the game, so it needs a place among the game's threads. Above every
 * game thread it locked out the audio thread (sound underran); below all
 * of them the game, which waits for it, hung. It sits just below the
 * threads at this N64 priority and up (GoldenEye: audio 20, scheduler 30,
 * crash 40, monitor 250) and above the rest (main 10): os_thread.c
 * leaves it that slot. */
#define PAL_RSP_N64_PRIORITY 15
#define PAL_PRIO_RSP_OLD3DS (PAL_PRIO_GAME_TOP + 4)
int pal_is_new3ds(void);
/* Gives core 1 back to the system (the time limit set for the audio and
 * graphics lanes outlives the program); called on the way out. */
void pal_sys_restore_cpu_limit(void);
PalThread *pal_thread_start_sys_core(void (*fn)(void *), void *arg, size_t stack_size, int prio);

void pal_thread_set_priority(PalThread *t, int prio);

/* pal_log, and also appended to sdmc:/3ds/ge007/perf.log without waiting
 * on the SD card: for numbers wanted from runs on real hardware. */
void pal_perf_log(const char *fmt, ...);
void pal_thread_yield(void);

/* End the calling thread. Never returns. */
void pal_thread_exit(void) __attribute__((noreturn));

/* One global lock plus one condition variable, standing in for libultra's
 * "disable interrupts" critical sections. Contention is low (a handful of
 * threads on one core), so one lock is simpler than per-queue locks and
 * costs little.
 *
 * The lock is re-entrant per thread: nested lock/unlock pairs just count.
 * That lets osSetIntMask(OS_IM_NONE) hold it across a game critical
 * section in which the game still calls osSendMesg etc.
 *
 * pal_os_wait() releases the lock completely while sleeping, whatever the
 * nesting depth, and restores it on return. A thread that blocks with
 * "interrupts off" therefore doesn't stall everyone else, which matches
 * N64, where each thread's interrupt mask is part of its saved context. */
void pal_os_lock(void);
void pal_os_unlock(void);
void pal_os_wait(void);
void pal_os_wake_all(void);

/* Like pal_os_wait, but returns after at most `ns` nanoseconds. Callers
 * re-check their condition either way, so it doesn't say which happened. */
void pal_os_wait_timeout(int64_t ns);

/* ---- Memory --------------------------------------------------------- */

/* Allocate from the 3DS linear heap (physically contiguous, GPU-visible,
 * at 0x14000000+ on Old 3DS and 0x30000000+ on New 3DS). Returns NULL on
 * failure. Never freed: used for whole-session arenas. */
void *pal_linear_alloc(size_t size, size_t align);
/* Write back the data cache over a range (before the DSP reads it). */
void pal_flush_dcache(const void *p, size_t size);

/* ---- Time ----------------------------------------------------------- */

/* Free-running ARM11 system tick (svcGetSystemTick). */
#define PAL_TICKS_PER_SEC 268111856ull

uint64_t pal_ticks(void);

/* Developer timers (-gfxdbg): ticks summed per id, read and cleared by the
 * 5 s heartbeat (os_vi.c). */
enum { PAL_PROF_LVLRENDER, PAL_PROF_CHRPROPS, PAL_PROF_ROOM_DL, PAL_PROF_ROOMLOAD,
       PAL_PROF_BGLEVEL, PAL_PROF_SKY, PAL_PROF_PORTALS, PAL_PROF_PROPSTICK,
       PAL_PROF_MARK1, PAL_PROF_MARK2, PAL_PROF_MARK3, PAL_PROF_MARK4, PAL_PROF_MARK5,
       PAL_PROF_CHRTICK, PAL_PROF_OBJTICK, PAL_PROF_PLAYERTICK,
       PAL_PROF_CHRAI, PAL_PROF_CHRMTX, PAL_PROF_NODE01, PAL_PROF_NODE02, PAL_PROF_NODE15,
       PAL_PROF_F3D_VTX, PAL_PROF_F3D_STATE, PAL_PROF_F3D_TEXLOAD,
       PAL_PROF_F3D_STATETEX, PAL_PROF_COUNT };
void pal_prof_add(int id, uint64_t ticks);
void pal_prof_take(uint64_t out[PAL_PROF_COUNT]);

/* Debug log to stderr (visible through 3dslink / Luma logs). The libultra
 * shim uses this instead of <stdio.h>, which the decomp may shadow with
 * its own libc headers. */
void pal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Debugging: write a file (e.g. "sdmc:/3ds/ge007/dump/x.bin"), creating
 * its directory. Returns 0 on success. */
int pal_debug_write(const char *path, const void *data, size_t size);

/* The integer in a small text file, or fallback if it is missing or holds
 * no number. For developer switches on the SD card. */
int pal_debug_read_int(const char *path, int fallback);
/* A text file's first size - 1 bytes, NUL-terminated: their count, or -1. */
int pal_debug_read_text(const char *path, char *buf, int size);

/* Show an error and exit. Never returns. */
void pal_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Must run before any other pal_sys / libultra-shim call. */
void pal_sys_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_SYS_H */
