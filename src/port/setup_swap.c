/*
 * setup_swap.c - stage setup files (U*setup*Z) and briefing files
 * (Ubrief*Z) from N64 byte order to native order, in place.
 *
 * A setup file starts with ten u32 file offsets (struct stagesetup):
 *
 *   waypoints     {s32 pad, s32 *neighbours, s32 group, s32 dist}, until
 *                 pad < 0; neighbours is an s32 list ending in -1
 *   waygroups     {s32 *neighbours, s32 *waypoints, s32 dist}, until
 *                 neighbours is 0; both are -1 terminated s32 lists
 *   intro         commands {s32 type, ...}, sized by type, until
 *                 INTROTYPE_END
 *   propdefs      records {u16 scale, u8 state, u8 type, ...}, sized by
 *                 sizepropdef(), until PROPDEF_END
 *   patrol paths  {s32 *waypoints, u8 id, u8 loop, u16 len}, until
 *                 waypoints is 0
 *   ai lists      {u8 *bytecode, s32 id}, until bytecode is 0. The
 *                 bytecode is read a byte at a time: left alone.
 *   pads          {f32 pos[3], up[3], look[3]; char *name; u32 stan},
 *                 until name is 0
 *   bound pads    pads plus f32 bbox[6], likewise
 *   pad names,    char * lists ending in 0; the strings are bytes
 *   bound names
 *
 * Words are 32-bit swapped unless a record's struct (as the game reads it:
 * see tools/dwarf_layout.py) has narrower fields there: 16-bit fields are
 * swapped as halves, bytes are left where they are.
 *
 * A briefing file is struct BriefStruct: 24 u16 text ids.
 */
#include <stdint.h>

#include "pal/pal_sys.h"
#include "port/asset_swap.h"

#define MAX_SETUP_SIZE (512 * 1024)

enum {                      /* PROPDEF_TYPE (bondconstants.h) */
    PD_DOOR = 0x01, PD_KEY = 0x04, PD_MAGAZINE = 0x07, PD_COLLECTABLE = 0x08,
    PD_GUARD = 0x09, PD_MONITOR = 0x0A, PD_MULTI_MONITOR = 0x0B,
    PD_GUARD_ATTRIBUTE = 0x12, PD_AMMO = 0x14, PD_TAG = 0x16,
    PD_OBJECTIVE_START = 0x17, PD_WATCH_MENU_TEXT = 0x23, PD_VEHICLE = 0x27,
    PD_AIRCRAFT = 0x28, PD_END = 0x30
};

enum {                      /* INTROTYPE (bondconstants.h) */
    IT_SPAWN, IT_ITEM, IT_AMMO, IT_SWIRL, IT_ANIM, IT_CUFF, IT_CAMERA,
    IT_WATCH, IT_CREDITS, IT_END
};

/* Records that inherit ObjectRecord (header, s16 obj, s16 pad, ...,
 * u8 colours at 0x78-0x7F). */
static const uint8_t s_objrecord[0x31] = {
    [0x01] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1, [0x06] = 1, [0x07] = 1,
    [0x08] = 1, [0x0A] = 1, [0x0B] = 1, [0x0C] = 1, [0x0D] = 1, [0x0F] = 1,
    [0x11] = 1, [0x14] = 1, [0x15] = 1, [0x24] = 1, [0x27] = 1, [0x28] = 1,
    [0x29] = 1, [0x2A] = 1, [0x2B] = 1, [0x2D] = 1, [0x2F] = 1,
};

extern int32_t sizepropdef(void *pdef);   /* loadobjectmodel.c: words */

static uint8_t *s_data;
static uint32_t s_size;
static uint8_t  s_done[MAX_SETUP_SIZE / 4 / 8];   /* one bit per word */

static int fits(uint32_t off, uint32_t len)
{
    return off + len <= s_size && off + len >= off;
}

/* Claims the word at off (a multiple of 4); 0 if it's outside the file or
 * done already. */
static int claim(uint32_t off)
{
    if (!fits(off, 4) || (off & 3))
        return 0;
    uint32_t w = off >> 2;
    if (s_done[w >> 3] & (1 << (w & 7)))
        return 0;
    s_done[w >> 3] |= 1 << (w & 7);
    return 1;
}

static void sw32(uint32_t off)
{
    if (claim(off))
        assetSwap32(s_data + off);
}

static void sw16x2(uint32_t off)     /* a word of two 16-bit fields */
{
    if (claim(off))
        assetSwap16Array(s_data + off, 2);
}

static void keep(uint32_t off)       /* a word of bytes */
{
    claim(off);
}

static int32_t rd32(uint32_t off) { return fits(off, 4) ? *(int32_t *)(s_data + off) : 0; }

/* An s32 list ending in -1, at a file offset. */
static void sw_list(uint32_t off)
{
    if (off == 0)
        return;
    for (; fits(off, 4); off += 4) {
        sw32(off);
        if (rd32(off) == -1)
            break;
    }
}

/* ---- Propdefs ------------------------------------------------------------- */

/* How word `w` (a byte offset in the record) of a record of `type` is
 * read: 32 = a whole word, 16 = two halves (or a half and bytes), 8 =
 * bytes only. */
static int word_kind(int type, uint32_t w)
{
    if (w == 0)
        return 0;                                 /* the header: special */
    if (type < (int)sizeof s_objrecord && s_objrecord[type]) {
        if (w == 0x04)
            return 16;                            /* s16 obj, s16 pad */
        if (w == 0x78 || w == 0x7C)
            return 8;                             /* shadecol, nextcol */
    }
    switch (type) {
    case PD_DOOR:
        if (w == 0x98 || w == 0xBC || w == 0xC4)  /* doorFlags/Type; open state,
                                                     opacity; cull, sound, fade */
            return 16;
        break;
    case PD_COLLECTABLE:
        if (w == 0x80)
            return 16;                            /* u8 weapon, u8 link, s16 timer */
        break;
    case PD_GUARD:
        if (w >= 0x04 && w <= 0x14)
            return 16;                            /* u16 chrnum ... HeadID */
        break;
    case PD_MONITOR:
    case PD_MULTI_MONITOR: {
        int n = type == PD_MONITOR ? 1 : 4;
        for (int i = 0; i < n; i++) {
            uint32_t b = 0x84 + 0x74 * i;         /* struct MonitorRecord */
            if (w == b)
                return 16;                        /* offset, pause60 */
            if (w >= b + 0x5C && w <= b + 0x64)
                return 8;                         /* colours */
        }
        if (type == PD_MULTI_MONITOR && w == 0x250)
            return 8;                             /* ImageNums[4] */
        break;
    }
    case PD_GUARD_ATTRIBUTE:
        if (w == 0x08)
            return 16;                            /* s16, s8, s8 */
        break;
    case PD_AMMO:
        if (w >= 0x80 && w <= 0xB0)
            return 16;                            /* 13 {u16 model, u16 count} */
        break;
    case PD_TAG:
        if (w == 0x04)
            return 16;                            /* u16 ID, s16 OffsetToObj */
        break;
    /* struct watchMenuObjectiveText / objective_entry: u32 id, enum menu
     * (int-sized, as IDO), u16 reserved, u16 text, then for objectives
     * u16, u8, s8 difficulty (the low byte of the s32 MinDificulty). */
    case PD_WATCH_MENU_TEXT:
        if (w == 0x08)
            return 16;
        break;
    case PD_OBJECTIVE_START:
        if (w == 0x08)
            return 16;
        if (w == 0x0C)
            return 8;
        break;
    case PD_VEHICLE:
    case PD_AIRCRAFT:
        if (w == 0x84)
            return 16;                            /* aioffset, aireturnlist */
        break;
    }
    return 32;
}

static void sw_propdefs(uint32_t off)
{
    if (off == 0)
        return;
    while (fits(off, 4)) {
        uint8_t *rec = s_data + off;
        int type = rec[3];
        if (claim(off))
            assetSwap16(rec);                     /* u16 scale; u8 state, type */
        if (type == PD_END)
            return;
        int32_t words = sizepropdef(rec);
        if (words <= 0)
            words = 1;
        for (int32_t i = 1; i < words; i++) {
            uint32_t w = 4 * (uint32_t)i;
            switch (word_kind(type, w)) {
            case 16: {
                /* swap the 16-bit fields; a byte pair stays as bytes */
                if (!claim(off + w))
                    break;
                uint8_t *p = rec + w;
                int half_bytes = 0;
                if (type == PD_COLLECTABLE && w == 0x80)
                    half_bytes = 1;                       /* which half is bytes */
                else if (type == PD_GUARD_ATTRIBUTE && w == 0x08)
                    half_bytes = 2;
                else if (type == PD_DOOR && (w == 0xBC || w == 0xC4))
                    half_bytes = w == 0xBC ? 1 : 2;
                if (half_bytes != 1)
                    assetSwap16(p);
                if (half_bytes != 2)
                    assetSwap16(p + 2);
                break;
            }
            case 8:
                keep(off + w);
                break;
            default:
                sw32(off + w);
                break;
            }
        }
        off += 4 * (uint32_t)words;
    }
}

/* ---- Intro ------------------------------------------------------------------ */

static void sw_intro(uint32_t off)
{
    static const uint8_t words[IT_END] = { 3, 4, 4, 8, 2, 2, 10, 3, 2 };

    if (off == 0)
        return;
    while (fits(off, 4)) {
        sw32(off);
        int32_t type = rd32(off);
        if (type < 0 || type >= IT_END)
            return;                               /* IT_END, or not a command */
        for (int i = 1; i < words[type]; i++) {
            if (type == IT_CAMERA && i == 7)
                sw16x2(off + 4 * i);              /* u16 lang_index[2] */
            else
                sw32(off + 4 * i);
        }
        if (type == IT_CREDITS) {
            /* {u16 text1, text2, pos1, align1, pos2, align2} until both
             * texts are 0 */
            uint32_t c = (uint32_t)rd32(off + 4);
            for (; c && fits(c, 12); c += 12) {
                for (int k = 0; k < 12; k += 4)
                    sw16x2(c + k);
                if (*(uint16_t *)(s_data + c) == 0 && *(uint16_t *)(s_data + c + 2) == 0)
                    break;
            }
        }
        off += 4 * words[type];
    }
}

/* ---- The file ------------------------------------------------------------- */

void assetSwapSetupFile(void *file, uint32_t size)
{
    uint32_t hdr[10];

    if (!file || size < 40)
        return;
    if (size > MAX_SETUP_SIZE) {
        pal_log("setup_swap: %lu bytes, more than %u: only the first part is swapped",
                (unsigned long)size, MAX_SETUP_SIZE);
        size = MAX_SETUP_SIZE;
    }
    s_data = file;
    s_size = size;
    __builtin_memset(s_done, 0, sizeof s_done);

    for (int i = 0; i < 10; i++) {
        sw32(4 * i);
        hdr[i] = (uint32_t)rd32(4 * i);
    }

    /* waypoints */
    for (uint32_t o = hdr[0]; o && fits(o, 16); o += 16) {
        sw32(o);
        if (rd32(o) < 0) {
            sw32(o + 4); sw32(o + 8); sw32(o + 12);
            break;
        }
        sw32(o + 4); sw32(o + 8); sw32(o + 12);
        sw_list((uint32_t)rd32(o + 4));
    }
    /* waygroups */
    for (uint32_t o = hdr[1]; o && fits(o, 12); o += 12) {
        sw32(o); sw32(o + 4); sw32(o + 8);
        if (rd32(o) == 0)
            break;
        sw_list((uint32_t)rd32(o));
        sw_list((uint32_t)rd32(o + 4));
    }
    sw_intro(hdr[2]);
    sw_propdefs(hdr[3]);
    /* patrol paths */
    for (uint32_t o = hdr[4]; o && fits(o, 8); o += 8) {
        sw32(o);
        if (claim(o + 4))
            assetSwap16(s_data + o + 6);          /* u8 id, u8 loop, u16 len */
        if (rd32(o) == 0)
            break;
        sw_list((uint32_t)rd32(o));
    }
    /* ai lists: the table only */
    for (uint32_t o = hdr[5]; o && fits(o, 8); o += 8) {
        sw32(o); sw32(o + 4);
        if (rd32(o) == 0)
            break;
    }
    /* pads, bound pads */
    for (int b = 0; b < 2; b++) {
        uint32_t stride = b ? 0x44 : 0x2C;
        for (uint32_t o = hdr[6 + b]; o && fits(o, stride); o += stride) {
            for (uint32_t k = 0; k < stride; k += 4)
                sw32(o + k);
            if (rd32(o + 0x24) == 0)
                break;
        }
    }
    /* pad names */
    for (int b = 0; b < 2; b++)
        for (uint32_t o = hdr[8 + b]; o && fits(o, 4); o += 4) {
            sw32(o);
            if (rd32(o) == 0)
                break;
        }
}

void assetSwapBriefFile(void *file)
{
    if (file)
        assetSwap16Array(file, 4 + 2 * 10);       /* brief[4], objective[10] */
}
