/*
 * asset_swap.c - the "swapped already?" set behind assetSwapClaim().
 *
 * Open addressing over addresses; shared asset data (sound bank envelopes,
 * ADPCM books and loops) is a few thousand entries at most.
 */
#include "port/asset_swap.h"

#include "pal/pal_sys.h"

#define CLAIM_SLOTS 16384   /* power of two */

static uintptr_t s_claimed[CLAIM_SLOTS];
static int s_claimedCount;

int assetSwapClaim(const void *p)
{
    uintptr_t key = (uintptr_t)p;
    uint32_t i = (uint32_t)(key * 2654435761u) & (CLAIM_SLOTS - 1);

    if (key == 0)
        return 0;
    while (s_claimed[i] != 0) {
        if (s_claimed[i] == key)
            return 0;
        i = (i + 1) & (CLAIM_SLOTS - 1);
    }
    if (s_claimedCount >= CLAIM_SLOTS * 3 / 4) {
        pal_log("asset_swap: claim set full");
        return 1;   /* no room to remember it; swap anyway */
    }
    s_claimed[i] = key;
    s_claimedCount++;
    return 1;
}
