/*
 * os_thread.h - libultra threads (src/ultra/os_thread.c).
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_THREAD_H
#define ULTRA_OS_THREAD_H

#include <ultra64.h>

void  osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                     void *sp, OSPri pri);
void  osStartThread(OSThread *t);
void  osSetThreadPri(OSThread *t, OSPri pri);
OSPri osGetThreadPri(OSThread *t);
OSId  osGetThreadId(OSThread *t);
void  osYieldThread(void);
void  osStopThread(OSThread *t);
void  osDestroyThread(OSThread *t);
OSThread *__osGetCurrFaultedThread(void);

/* ---- Port-only ------------------------------------------------------ */

/* Maximum number of OSThreads the game may create. GoldenEye uses fewer
 * than ten. */
#define OS_PORT_MAX_THREADS 32

/* Native stack for each game thread. The N64 stack passed to
 * osCreateThread is ignored: it was sized for IDO-compiled MIPS code and
 * is too small for GCC-compiled ARM code that inlines heavily. */
#define OS_PORT_THREAD_STACK (128 * 1024)

/* The OSThread running on the calling thread, or NULL on threads the game
 * didn't create (e.g. the 3DS main thread). */
OSThread *osPortCurrentThread(void);

/* Diagnostics for hangs. A game thread about to block notes what on (OS
 * lock held; `ra` is the game code that called in), and clears it when it
 * wakes. osPortDumpThreads logs every thread's state (lock not held). */
void osPortNoteWait(const char *what, const void *obj, void *ra);
void osPortNoteRunning(void);
void osPortDumpThreads(void);

/* The game's idle thread (init.c idleproc) spins here at the lowest
 * priority; the time it gets is the game core's idle time.
 * osPortTakeIdleTicks returns the ticks spent idle since the last call. */
void osPortIdleLoop(void);
u64 osPortTakeIdleTicks(void);

#endif /* ULTRA_OS_THREAD_H */
