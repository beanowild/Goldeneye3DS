/*
 * os_internal.c - libultra globals and internals that game code reaches
 * directly, outside the public API.
 *
 * Signatures follow the decomp's own declarations (PR/os.h,
 * PR/os_internal.h, libultra io/controller.h, io/siint.h, os/osint.h).
 */
#include <ultra64.h>
#include <PR/os_internal.h>
#include "io/controller.h"
#include "io/siint.h"
#include "os/osint.h"

/* ---- Globals libultra's boot code would set ------------------------------ */

u32 osTvType    = OS_TV_NTSC;   /* NTSC-U build; drives the game's 60 Hz timing */
u64 osClockRate = 62500000LL;   /* OS_CLOCK_RATE: the RCP clock, 62.5 MHz */

/* ---- SI bus (used by motor.c, the Rumble Pak driver) ---------------------
 *
 * The pak calls in os_cont.c already report "no pak" before motor.c gets
 * as far as raw SI transfers, so these never do real work. They fail
 * cleanly in case it ever does reach them. */

u8       __osContLastCmd;
OSPifRam __osPfsPifRam;

void __osSiGetAccess(void) { }
void __osSiRelAccess(void) { }

s32 __osSiRawStartDma(s32 direction, void *dramAddr)
{
    (void)direction;
    (void)dramAddr;
    return -1;
}

u8 __osContAddressCrc(u16 addr)
{
    (void)addr;
    return 0;
}

/* ---- Scheduler internals (read by crash.c's thread dump) -----------------
 *
 * Threads here are libctru threads (os_thread.c), so libultra's run queue
 * is always empty. The crash screen never runs anyway: CPU exceptions go
 * to Luma3DS. */

OSThread *__osRunQueue;

void __osEnqueueThread(OSThread **queue, OSThread *t)
{
    (void)queue;
    (void)t;
}

/* ---- TLB (tlb_manage.c) --------------------------------------------------- */

u32 __osGetTLBHi(s32 index)
{
    (void)index;
    return 0;   /* no entries: native code is never TLB-mapped */
}
