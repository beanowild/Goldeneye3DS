/*
 * os_cpu.h - libultra CPU-level calls (src/ultra/os_cpu.c): caches,
 * address translation, interrupt mask, status registers, TLB.
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h). The double-underscore
 * functions are libultra internals that game code in this decomp also
 * calls.
 */
#ifndef ULTRA_OS_CPU_H
#define ULTRA_OS_CPU_H

#include <ultra64.h>

void      osInvalDCache(void *vaddr, s32 nbytes);
void      osInvalICache(void *vaddr, s32 nbytes);
void      osWritebackDCache(void *vaddr, s32 nbytes);
void      osWritebackDCacheAll(void);

u32       osVirtualToPhysical(void *vaddr);
void     *osPhysicalToVirtual(u32 paddr);

OSIntMask osSetIntMask(OSIntMask im);

u32       __osGetSR(void);
void      __osSetSR(u32 value);
u32       __osGetFpcCsr(void);
u32       __osSetFpcCsr(u32 value);

void      osMapTLB(s32 index, OSPageMask pm, void *vaddr, u32 evenpaddr,
                   u32 oddpaddr, s32 asid);
void      osUnmapTLB(s32 index);
void      osUnmapTLBAll(void);
u32       __osProbeTLB(void *vaddr);

#endif /* ULTRA_OS_CPU_H */
