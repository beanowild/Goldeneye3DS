/*
 * asset_swap.h - helpers for turning N64 (big-endian) ROM data into native
 * (little-endian) order in place, once, right after it is loaded.
 *
 * Usable from the decomp (no <3ds.h>, no <ultra64.h>).
 */
#ifndef PORT_ASSET_SWAP_H
#define PORT_ASSET_SWAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void assetSwap16(void *p)
{
    uint16_t *v = (uint16_t *)p;
    *v = __builtin_bswap16(*v);
}

static inline void assetSwap32(void *p)
{
    uint32_t *v = (uint32_t *)p;
    *v = __builtin_bswap32(*v);
}

static inline void assetSwap16Array(void *p, uint32_t count)
{
    uint16_t *v = (uint16_t *)p;
    while (count--) {
        *v = __builtin_bswap16(*v);
        v++;
    }
}

static inline void assetSwap32Array(void *p, uint32_t count)
{
    uint32_t *v = (uint32_t *)p;
    while (count--) {
        *v = __builtin_bswap32(*v);
        v++;
    }
}

/* Returns 1 the first time it sees an address, 0 after that. For data that
 * several structures can point at (envelopes, ADPCM books, ...) so it is
 * swapped exactly once. */
int assetSwapClaim(const void *p);

/* A model file (chr/prop/gun) just loaded, still in N64 order, before
 * load_object_fill_header() promotes its offsets. See model_swap.c. */
void assetSwapModelFile(void *file, uint32_t size, int numSwitches, int numTextures);

/* A level's header part (load_bg_file()), and a room's decompressed vertex
 * table. See bg_swap.c. */
void assetSwapBgHeader(void *data, uint32_t size);
void assetSwapVtxArray(void *vtx, uint32_t count);
void assetSwapStanFile(void *file, uint32_t size);

/* A stage setup file (U*setup*Z) as loaded, before proplvreset2() rebases
 * its offsets; a briefing file (Ubrief*Z). See setup_swap.c. */
void assetSwapSetupFile(void *file, uint32_t size);
void assetSwapBriefFile(void *file);

/* A text bank (L*E / L*J): u32 string offsets, then the strings. The
 * table ends where the first string starts. */
static inline void assetSwapTextBank(void *bank)
{
    uint32_t *t = (uint32_t *)bank;
    uint32_t end = ~0u;

    if (t == 0)
        return;
    for (uint32_t i = 0; 4 * i < end; i++) {
        t[i] = __builtin_bswap32(t[i]);
        if (t[i] != 0 && t[i] < end)
            end = t[i];
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PORT_ASSET_SWAP_H */
