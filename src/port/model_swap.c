/*
 * model_swap.c - model files (chr/prop/gun "...Z") from N64 byte order to
 * native order, in place, right after they are loaded and before the game
 * turns their offsets into pointers (load_object_fill_header()).
 *
 * File layout (checked against all 512 models with the extracted assets):
 *
 *   u32 switches[numSwitches]                 node offsets
 *   ModelFileTextures textures[numTextures]   12 bytes, u32 TextureID first
 *   ModelNode nodes...                        the tree, from here
 *   records, vertex arrays, collision data    reached from the nodes
 *   embedded texel data                       left alone: texels stay in
 *                                             N64 byte order everywhere
 *   display lists                             from the first DL to the end
 *                                             of the file, nothing else
 *
 * Pointers in the file are segment 5 addresses (0x05000000 + file offset).
 * Field layouts follow the ModelRoData_* structs in bondtypes.h; they're
 * written out by size here so this file doesn't need the game's headers.
 *
 * A bitmap with one bit per halfword records what has been swapped, so
 * data reached twice (shared records, vertex arrays loaded by several
 * G_VTX commands) is swapped exactly once.
 */
#include <stdint.h>

#include "pal/pal_sys.h"
#include "port/asset_swap.h"

#define SEG5          0x05000000u
#define NODE_SIZE     0x18
#define VTX_SIZE      16
#define MAX_FILE_SIZE (512 * 1024)

static uint8_t s_done[MAX_FILE_SIZE / 2 / 8];
static uint8_t *s_file;
static uint32_t s_size;

/* Mark [off, off+len) as swapped; returns 0 if it was already (or is out
 * of range, which is logged once). */
static int claim(uint32_t off, uint32_t len)
{
    uint32_t h = off >> 1;
    if (off + len > s_size || (off & 1)) {
        static int warned;
        if (!warned++)
            pal_log("model_swap: bad range %lx+%lx (size %lx)",
                    (unsigned long)off, (unsigned long)len, (unsigned long)s_size);
        return 0;
    }
    if (s_done[h >> 3] & (1 << (h & 7)))
        return 0;
    for (uint32_t i = h; i < ((off + len + 1) >> 1); i++)
        s_done[i >> 3] |= 1 << (i & 7);
    return 1;
}

static void sw16(uint32_t off)
{
    if (claim(off, 2))
        assetSwap16(s_file + off);
}

static void sw32(uint32_t off)
{
    if (claim(off, 4))
        assetSwap32(s_file + off);
}

static uint32_t rd32(uint32_t off) { return *(uint32_t *)(s_file + off); }
static uint16_t rd16(uint32_t off) { return *(uint16_t *)(s_file + off); }

/* A swapped segment-5 pointer as a file offset, or ~0 for NULL/foreign. */
static uint32_t segoff(uint32_t p)
{
    if ((p >> 24) != 5 || p - SEG5 >= s_size)
        return ~0u;
    return p - SEG5;
}

/* Swap fields laid out by `spec`: '4' = 32-bit, '2' = 16-bit, '1' = byte. */
static void swap_spec(uint32_t off, const char *spec)
{
    for (; *spec; spec++) {
        if (*spec == '4') { sw32(off); off += 4; }
        else if (*spec == '2') { sw16(off); off += 2; }
        else off += 1;
    }
}

static const char *const s_recSpec[] = {
    [1]  = "224422",         /* Header: anim part, matrix, first group, groups (read as f32), rw, pad */
    [2]  = "444222244",      /* Group: origin, joint, matrix ids[3], child group, radius */
    [3]  = "444222244",      /* Op03: as Group */
    [4]  = "444421",         /* DL: primary, secondary, base, vertices, count, model type */
    [8]  = "44422",          /* LOD: min, max, affects, rw, pad */
    [9]  = "4444444422",     /* BSP: point, vector, left, right, pad, rw */
    [10] = "4444444",        /* BBox: model number, bbox */
    [11] = "44444444444444444224", /* Op11: unk0c[16], radius, rw, unk46, base */
    [12] = "44444444224",    /* Gunfire: offset, size, image, scale, rw, pad, base */
    [13] = "44444444",       /* Shadow */
    [14] = "4444",           /* Op14 */
    [15] = "4444444",        /* Interlink */
    [16] = "44422224",       /* Op16 */
    [17] = "44444444",       /* Op17 */
    [18] = "422",            /* Switch: controls, rw, pad */
    [20] = "224422",         /* Op20: as Header */
    [21] = "444224",         /* GroupSimple */
    [22] = "4444",           /* DLPrimary: quad count, vertices, primary, base */
    [23] = "2",              /* Head placeholder */
    [24] = "4442244224",     /* DLCollision */
};

static void swap_vertices(uint32_t off, uint32_t count)
{
    for (; count--; off += VTX_SIZE)
        swap_spec(off, "222222");          /* x y z, flag, s t; colour bytes */
}

static void swap_collision_vertices(uint32_t off, uint32_t count)
{
    for (; count--; off += VTX_SIZE)
        swap_spec(off, "2222422");         /* x y z, index, linked-to, related index, pad */
}

static uint32_t s_firstDL;

static void note_dl(uint32_t p)
{
    uint32_t o = segoff(p);
    if (o != ~0u && o < s_firstDL)
        s_firstDL = o;
}

static void swap_record(uint32_t type, uint32_t rec)
{
    uint32_t v, n;

    if (type >= sizeof s_recSpec / sizeof s_recSpec[0] || !s_recSpec[type]) {
        static int warned;
        if (!warned++)
            pal_log("model_swap: unhandled node type %lu", (unsigned long)type);
        return;
    }
    swap_spec(rec, s_recSpec[type]);

    switch (type) {
    case 4:
        note_dl(rd32(rec + 0x0));
        note_dl(rd32(rec + 0x4));
        if ((v = segoff(rd32(rec + 0xC))) != ~0u)
            swap_vertices(v, rd16(rec + 0x10));
        break;
    case 22:
        /* The count is of quads: model.c dorottex copies four vertices
         * per count (the AR33's flash is 6 quads, 24 vertices; swapping 6
         * left the rest big-endian, a screen-filling yellow sheet). */
        note_dl(rd32(rec + 0x8));
        if ((v = segoff(rd32(rec + 0x4))) != ~0u)
            swap_vertices(v, rd32(rec + 0x0) * 4);
        break;
    case 24:
        note_dl(rd32(rec + 0x0));
        note_dl(rd32(rec + 0x4));
        n = (int16_t)rd16(rec + 0xC);
        if ((v = segoff(rd32(rec + 0x8))) != ~0u)
            swap_vertices(v, n);
        if ((v = segoff(rd32(rec + 0x10))) != ~0u)
            swap_collision_vertices(v, (int16_t)rd16(rec + 0xE));
        if ((v = segoff(rd32(rec + 0x14))) != ~0u)
            for (uint32_t i = 0; i < n; i++)
                sw16(v + 2 * i);           /* PointUsage: one s16 per vertex */
        break;
    }
}

static void swap_nodes(uint32_t node)
{
    while (node != ~0u) {
        uint32_t type, rec, child;

        if (!claim(node, 2))
            return;                        /* cycle or bad offset */
        assetSwap16(s_file + node);
        for (int i = 4; i < NODE_SIZE; i += 4)
            sw32(node + i);

        type = rd16(node) & 0xff;
        rec = segoff(rd32(node + 0x4));
        child = segoff(rd32(node + 0x14));
        if (rec != ~0u) {
            swap_record(type, rec);
            if (type == 8)                 /* LOD: the walk follows "affects" */
                child = segoff(rd32(rec + 0x8));
        }

        if (child != ~0u) {
            node = child;
            continue;
        }
        /* Up and across, the way modelPromoteNodeOffsetsToPointers walks.
         * Nodes passed on the way up were swapped when first visited. */
        while (node != ~0u) {
            uint32_t next = segoff(rd32(node + 0xC));
            if (next != ~0u) {
                node = next;
                break;
            }
            node = segoff(rd32(node + 0x8));
        }
    }
}

void assetSwapModelFile(void *file, uint32_t size, int numSwitches, int numTextures)
{
    uint32_t i, root;

    if (size > MAX_FILE_SIZE) {
        pal_log("model_swap: file of %lu bytes too big", (unsigned long)size);
        return;
    }
    s_file = file;
    s_size = size;
    s_firstDL = size;
    __builtin_memset(s_done, 0, (size / 2 + 7) / 8);

    for (i = 0; i < (uint32_t)numSwitches; i++)
        sw32(4 * i);
    for (i = 0; i < (uint32_t)numTextures; i++)
        sw32(4 * numSwitches + 12 * i);

    root = 4 * numSwitches + 12 * numTextures;
    swap_nodes(root);

    /* Display lists: whole words from the first one to the end of the file.
     * Then the vertices they load that no record lists (e.g. explosionbit). */
    for (i = s_firstDL & ~7u; i + 8 <= size; i += 8) {
        sw32(i);
        sw32(i + 4);
    }
    for (i = s_firstDL & ~7u; i + 8 <= size; i += 8) {
        uint32_t w0 = rd32(i), v;
        if ((w0 >> 24) == 0x04 && (v = segoff(rd32(i + 4))) != ~0u)   /* G_VTX */
            swap_vertices(v, ((w0 >> 20) & 0xF) + 1);
    }
}
