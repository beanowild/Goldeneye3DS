/*
 * random.c - C versions of the game's RNG assembly (src/random.s,
 * src/game/chrObjRandom.s, src/tlb_random.s).
 *
 * The three .s files are the same generator with separate seeds: a 64-bit
 * shift/xor generator, also known from Perfect Dark's random(). This is a
 * line-by-line translation of the MIPS, so sequences are bit-identical,
 * which matters for ramrom (attract-mode demo) playback: it records seeds
 * and expects the same rolls.
 *
 *   dsll32 a2, s, 0x1f ; dsrl a2, a2, 0x1f    ->  (s << 63) >> 31
 *   dsll   a1, s, 0x1f ; dsrl32 a1, a1, 0      ->  (s << 31) >> 32
 *   dsll32 a0, s, 0xc  ; dsrl32 a0, a0, 0      ->  (s << 44) >> 32
 *   s = ((a2 | a1) ^ a0);  s ^= (s >> 20) & 0xfff;  return low 32 bits
 */
#include <ultra64.h>

/* .data: .word 0xAB8D9F77, 0x81280783, big-endian, read as one doubleword. */
#define INITIAL_SEED 0xAB8D9F7781280783ULL

u64 g_randomSeed        = INITIAL_SEED;
u64 g_chrObjRandomSeed  = INITIAL_SEED;
u64 g_tlbRandomSeed     = INITIAL_SEED;

static u32 step(u64 *seed)
{
    u64 s = *seed;
    s = (((s << 63) >> 31) | ((s << 31) >> 32)) ^ ((s << 44) >> 32);
    s ^= (s >> 20) & 0xfff;
    *seed = s;
    return (u32)s;
}

/* `daddiu a0, a0, 1; sd a0`: the u32 argument arrives sign-extended in a
 * 64-bit register (o32 ABI on a MIPS III CPU), and the +1 is 64-bit. */
static u64 seed_from(u32 x)
{
    return (u64)(s64)(s32)x + 1;
}

u32  randomGetNext(void)              { return step(&g_randomSeed); }
void randomSetSeed(u32 x)             { g_randomSeed = seed_from(x); }
u32  randomGetNextFrom(u64 *seed)     { return step(seed); }

u32  chrObjRandomGetNext(void)        { return step(&g_chrObjRandomSeed); }
void chrObjRandomSetSeed(u32 x)       { g_chrObjRandomSeed = seed_from(x); }

u32  tlbRandomGetNext(void)           { return step(&g_tlbRandomSeed); }
