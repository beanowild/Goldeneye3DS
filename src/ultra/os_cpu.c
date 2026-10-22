/*
 * os_cpu.c - libultra's MIPS CPU-level calls, mapped to the ARM11.
 *
 * Most of these replace hand-written MIPS assembly in the decomp
 * (invaldcache.s, setintmask.s, getsr.s, ...), so they have to exist in C.
 *
 * Caches
 * ------
 * On N64, the CPU cache isn't coherent with RSP/PI DMA, so the game
 * flushes and invalidates around every transfer. Here every "DMA" is a CPU
 * copy, and the RSP stand-in reads memory through the CPU too, so
 * there's nothing to do. (Data handed to the PICA GPU is a different
 * matter, and the renderer flushes it itself with GSPGPU_FlushDataCache.)
 *
 * Addresses
 * ---------
 * osVirtualToPhysical is the identity: the game's pointers are real ARM
 * pointers. The display-list interpreter must then tell a pointer from an
 * N64 segmented address (0x0SOOOOOO, segment S in bits 24-27). That works
 * only while game memory never sits at 0x01000000-0x0FFFFFFF, where a real
 * pointer would read as "segment 1-15". The 3DSX code/data/bss segment
 * starts at 0x00100000 (segment 0 = physical, so those resolve to
 * themselves), but the default heap is at 0x08000000. **The renderer
 * milestone has to put the game's memory arenas in linear memory
 * (0x14000000 / 0x30000000) or remap them.** This is noted here because
 * this function is where the assumption starts.
 *
 * Interrupt mask
 * --------------
 * See pal_sys.h: "interrupts off" means holding the (re-entrant) OS lock.
 * Every stand-in interrupt source (VI pump, timer thread, RSP thread)
 * posts under that lock, so a masked region can't be interrupted. Other
 * game threads can only become runnable through a message, so they can't
 * get in either. Each thread keeps its own mask, as in its N64 context.
 */
#include <ultra64.h>

#include "pal/pal_sys.h"
#include "ultra/os_cpu.h"

/* ---- Caches ----------------------------------------------------------- */

void osInvalDCache(void *vaddr, s32 nbytes)     { (void)vaddr; (void)nbytes; }
void osInvalICache(void *vaddr, s32 nbytes)     { (void)vaddr; (void)nbytes; }
void osWritebackDCache(void *vaddr, s32 nbytes) { (void)vaddr; (void)nbytes; }
void osWritebackDCacheAll(void)                 { }

/* ---- Address translation ------------------------------------------------ */

u32 osVirtualToPhysical(void *vaddr)
{
    return (u32)(uintptr_t)vaddr;
}

void *osPhysicalToVirtual(u32 paddr)
{
    return (void *)(uintptr_t)paddr;
}

/* ---- Game heap ------------------------------------------------------------ */

/* On N64 the game heap is every byte of RDRAM between the end of .bss and
 * the TLB paging block, about 3 MB of the 4 MB console. 4 MB here leaves
 * room to spare without changing how the game's allocator divides it.
 *
 * It lives in linear memory so that no heap pointer falls in
 * 0x01000000-0x0FFFFFFF, where the display-list interpreter would mistake
 * it for an N64 segmented address (see the top of this file). */
#define GAME_HEAP_SIZE (4u * 1024 * 1024)

u8 *osPortGameHeap(u32 *size)
{
    static u8 *heap;
    if (heap == NULL) {
        heap = pal_linear_alloc(GAME_HEAP_SIZE, 64);
        if (heap == NULL)
            pal_fatal("Not enough linear memory for the game heap (%u KB)",
                      GAME_HEAP_SIZE / 1024);
    }
    *size = GAME_HEAP_SIZE;
    return heap;
}

/* ---- Interrupt mask ------------------------------------------------------ */

static __thread OSIntMask t_int_mask = OS_IM_ALL;

OSIntMask osSetIntMask(OSIntMask im)
{
    OSIntMask old = t_int_mask;
    int was_off = old != OS_IM_ALL;
    int now_off = im != OS_IM_ALL;

    /* Any mask short of "everything on" counts as off: the stand-in
     * interrupt sources aren't split by line, and games mask to make a
     * section atomic, not to filter particular lines. */
    if (!was_off && now_off)
        pal_os_lock();
    else if (was_off && !now_off)
        pal_os_unlock();

    t_int_mask = im;
    return old;
}

/* ---- Status / FP control registers --------------------------------------- */

/* Status register: report "interrupts enabled, kernel mode", the state
 * game code expects to see. Writes are ignored. */
#define SR_DEFAULT 0x0000FF01u   /* SR_IMASK | SR_IE (R4300.h) */

u32  __osGetSR(void)        { return SR_DEFAULT; }
void __osSetSR(u32 value)   { (void)value; }

/* FPU control/status. Games set the rounding mode and which FP exceptions
 * trap. ARM VFP already rounds to nearest and doesn't trap by default,
 * which is what N64 games ask for, so the value is just stored and returned. */
static u32 s_fpc_csr = 0;   /* FPCSR_RM_RN: round to nearest, no traps */

u32 __osGetFpcCsr(void) { return s_fpc_csr; }

u32 __osSetFpcCsr(u32 value)
{
    u32 old = s_fpc_csr;
    s_fpc_csr = value;
    return old;
}

/* ---- TLB ---------------------------------------------------------------- */

/* GoldenEye maps part of its code through the TLB on N64. Natively
 * compiled, all code sits at its link address, so there's nothing to map.
 * The decomp's own TLB-miss handler (tlb_manage.c) needs a TARGET_3DS
 * guard; these just keep its callers linking. */
void osMapTLB(s32 index, OSPageMask pm, void *vaddr, u32 evenpaddr,
              u32 oddpaddr, s32 asid)
{
    (void)index; (void)pm; (void)vaddr; (void)evenpaddr; (void)oddpaddr; (void)asid;
}

void osUnmapTLB(s32 index) { (void)index; }
void osUnmapTLBAll(void)   { }

u32 __osProbeTLB(void *vaddr)
{
    return osVirtualToPhysical(vaddr);
}

/* ---- libm constant -------------------------------------------------------- */

/* libultra's gu/sinf.c and cosf.c return this for invalid input. On N64 it
 * comes from libm_vals.s, which can't be built here. ARM's quiet-NaN bit
 * pattern is used, not MIPS's (0x7F810000), which would be a signalling
 * NaN on ARM. */
const u32 __libm_qnan_f = 0x7FC00000;
