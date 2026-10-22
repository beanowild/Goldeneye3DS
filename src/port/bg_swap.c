/*
 * bg_swap.c - level geometry files (bg_*_all_p_seg) from N64 byte order to
 * native order, in place.
 *
 * The file's header part (what load_bg_file() reads into RAM) is, checked
 * against every level in the ROM:
 *
 *   u32 header[5]           0, room table, portal table, visibility
 *                           commands, unused (0 in every level)
 *   room table              {u32 point table, primary DL, secondary DL;
 *                            f32 x, y, z}, entry 0 empty, ends with an
 *                            entry with no point table and no primary DL
 *   visibility commands     8-byte units {u8 type, u8 length, u8 pad[2],
 *                           s32 arg}, stepping by `length` units, ending at
 *                           type 0. Only `arg` is multi-byte.
 *   portal table            {u32 portal, u8 room1, u8 room2, u8 flags,
 *                           u8 thickness}, ends with portal 0. The game
 *                           reads the last two as separate bytes
 *                           (controlbytes1/2), so they are not swapped.
 *   portals                 {u8 count, u8 pad[3], f32 points[count][3]}
 *
 * Pointers are segment 0x0F addresses (0x0F000000 + file offset). A few
 * levels have portal records no table entry points at; they are never
 * read, so they are left alone.
 *
 * The per-room data after the header is compressed and loaded room by
 * room; bg.c swaps it after decompressing (vertices as Vtx, display lists
 * as words).
 */
#include <stdint.h>

#include "pal/pal_sys.h"
#include "port/asset_swap.h"

#define SEGF          0x0F000000u
#define MAX_HDR_SIZE  (64 * 1024)

static uint8_t *s_data;
static uint32_t s_size;
static uint8_t  s_done[MAX_HDR_SIZE / 4 / 8];   /* one bit per word */

static int fits(uint32_t off, uint32_t len)
{
    return off != ~0u && off + len <= s_size && off + len >= off;
}

static void sw32(uint32_t off)
{
    if (!fits(off, 4) || (off & 3))
        return;
    uint32_t w = off >> 2;
    if (s_done[w >> 3] & (1 << (w & 7)))
        return;
    s_done[w >> 3] |= 1 << (w & 7);
    assetSwap32(s_data + off);
}

static uint32_t rd32(uint32_t off) { return fits(off, 4) ? *(uint32_t *)(s_data + off) : 0; }

static uint32_t segoff(uint32_t p)
{
    return (p >> 24) == 0x0F ? p - SEGF : ~0u;
}

void assetSwapBgHeader(void *data, uint32_t size)
{
    uint32_t i, rooms, vis, ptab;

    if (size > MAX_HDR_SIZE) {
        pal_log("bg_swap: header of %lu bytes too big", (unsigned long)size);
        return;
    }
    s_data = data;
    s_size = size;
    __builtin_memset(s_done, 0, (size / 4 + 7) / 8);

    for (i = 0; i < 5; i++)
        sw32(4 * i);

    rooms = segoff(rd32(4));
    for (i = 0; fits(rooms + 0x18 * i, 4); i++) {   /* the 0x40-byte first read can end mid-entry */
        uint32_t e = rooms + 0x18 * i;
        for (uint32_t k = 0; k < 6; k++)
            sw32(e + 4 * k);
        if (i > 0 && rd32(e) == 0 && rd32(e + 4) == 0)
            break;
    }

    vis = segoff(rd32(12));
    while (fits(vis, 8)) {
        uint32_t type = s_data[vis], len = s_data[vis + 1] ? s_data[vis + 1] : 1;
        for (uint32_t k = 0; k < len; k++)
            sw32(vis + 8 * k + 4);
        if (type == 0)
            break;
        vis += 8 * len;
    }

    ptab = segoff(rd32(8));
    for (i = 0; fits(ptab + 8 * i, 8); i++) {
        uint32_t e = ptab + 8 * i, p;
        sw32(e);
        if ((p = rd32(e)) == 0)
            break;
        p = segoff(p);
        if (fits(p, 4))                     /* shared portals: sw32 skips done words */
            for (uint32_t k = 0; k < 3u * s_data[p]; k++)
                sw32(p + 4 + 4 * k);
    }
}

void assetSwapVtxArray(void *vtx, uint32_t count)
{
    uint16_t *v = vtx;
    for (; count--; v += 8)                 /* s16 ob[3], u16 flag, s16 tc[2]; colour bytes */
        assetSwap16Array(v, 6);
}

/*
 * Stan (collision tile) files, before stanDetermineEOF(): a word, then
 * room offsets up to a 0 word, then tiles up to a 0 word:
 *
 *   u32 id : 24, u8 room    the game never reads the id bitfield: it
 *                           reads a u16 at byte 0 (bit 15 is the type) and
 *                           the byte at 2 (stanMatchTileName, stan.c), so
 *                           bytes 0-1 are swapped as a u16, 2 and 3 stay
 *   s16 mid, s16 tail       tail >> 12 is the point count
 *   points[max(count, 3)]   s16 x, y, z, u16 link
 */
void assetSwapStanFile(void *file, uint32_t size)
{
    uint8_t *d = file;
    uint32_t off = 4;

    while (off + 4 <= size && *(uint32_t *)(d + off) != 0) {
        assetSwap32(d + off);
        off += 4;
    }
    off += 4;

    while (off + 8 <= size && *(uint32_t *)(d + off) != 0) {
        uint32_t n;
        assetSwap16(d + off);
        assetSwap16Array(d + off + 4, 2);
        n = (*(uint16_t *)(d + off + 6) >> 12) & 0xF;
        if (n < 3)
            n = 3;
        if (off + 8 + 8 * n > size)
            break;
        assetSwap16Array(d + off + 8, 4 * n);
        off += 8 + 8 * n;
    }
}
