/*
 * f3d_tex.c - the RDP's texture memory (TMEM), tiles, and decoding tiles
 * into GPU textures.
 *
 * TMEM is emulated as the 4 KB it is, with the RDP's load rules, because
 * Rare's code relies on them: textures marked "preswapped" are stored in
 * RAM with their odd rows already word-swapped and loaded with dxt = 0, so
 * only the RDP's un-swap on sampling puts them right.
 *
 *  - Loads (LOADBLOCK, LOADTILE) copy RAM bytes into TMEM 64-bit words.
 *    On odd lines (LOADBLOCK: by the dxt counter; LOADTILE: odd rows) the
 *    two 32-bit halves of each word are swapped.
 *  - Sampling a row t reads with the halves swapped back when t is odd.
 *  - 32-bit texels are split: red/green in the low 2 KB, blue/alpha at
 *    the same offset in the high 2 KB.
 *  - LOADTLUT puts palette entry i at TMEM word (tile tmem + i); the
 *    palette always starts at word 256 (byte 0x800).
 *
 * Texture data in RAM is N64 byte order (the port never swaps it), so
 * everything here works on bytes, big-endian.
 *
 * A decoded tile becomes an RGBA8 texture of power-of-two size: wrapped
 * and mirrored axes use the mask period (small periods baked into an
 * 8-texel repeat), clamped axes are padded with their edge texels.
 */
#include <ultra64.h>
#include <PR/gbi.h>

#include "f3d_int.h"
#include "pal/pal_sys.h"

/* #define F3D_TEX_DUMP 1 */

#define TMEM_SIZE 4096
#define MAX_DIM   1024              /* a side; TMEM bounds the area */
#define MAX_TEXELS (128 * 1024)

static u8 s_tmem[TMEM_SIZE] __attribute__((aligned(8)));
static Tile s_tile[8];
/* The G_SETTILE / G_SETTILESIZE words each tile was last set from (all
 * ones: unknown), so that setting a tile as it is costs a compare: the
 * game does so ~2000 times a frame. */
static u32 s_tile_set[8][2], s_tile_size[8][2];

static struct {
    u8 fmt, siz;
    u16 width;
    u32 addr;
} s_timg;

/* Memo: a tile's texture key stays valid until TMEM or that tile changes
 * (it saves hashing; the id is looked up again, as it may be evicted). */
static u32 s_gen;                       /* bumped by every TMEM write */
static u32 s_hi_gen;                    /* bumped by writes to the high 2 KB */
static struct {
    u32 gen;
    int valid;
    u64 hash;
} s_pal[16];                            /* 4-bit palettes' hashes */
static struct {
    u32 gen, om;
    int valid;
    u64 key;
    u16 id;                             /* its texture this task, 0 = not looked up */
    TexMap map;
} s_memo[8];

extern void *malloc(unsigned int size);   /* the decomp's <stdlib.h> has none */
static u32 *s_texels;    /* MAX_TEXELS, on the heap: statics must end below 16 MB */

/* Texture keys hash where TMEM's bytes came from rather than the bytes
 * (up to 4 KB a lookup): each load is recorded with its TMEM range and a
 * hash of its source (address, format, geometry and a few sampled words,
 * so a texture rewritten in place still changes key). A region's key is
 * the records overlapping it, newest first. Records are trimmed to the
 * bytes they still own, so the list stays short. */
#define MAX_LOADS 32
static struct { u16 start, end; u64 hash; } s_loads[MAX_LOADS];   /* oldest first */
static int s_nloads;

static u64 hash_words(u64 h, const u8 *p, u32 bytes);
static void tex_materialize(void);

/* Define F3D_TEX_DUMP to write the first decoded textures (RGBA8, with the
 * TMEM they came from) to sdmc:/3ds/ge007/texdump/, for
 * tools/texdump_to_png.py. */
#ifdef F3D_TEX_DUMP
static struct { u32 kind, siz, dxt, tile, bytes; } s_last_load;
static void dump(const Tile *t, u32 w, u32 h, u32 tlut_type)
{
    static int count;
    char name[160];
    if (count >= 120)
        return;
    __builtin_snprintf(name, sizeof name,
        "sdmc:/3ds/ge007/texdump/%03d_%lux%lu_fmt%u_siz%u_line%u_tmem%u_pal%u_tt%lu_ms%u-%u_cm%u-%u_ld%lu-%lu-%lu-%lu.rgba",
        count, (unsigned long)w, (unsigned long)h, t->fmt, t->siz, t->line, t->tmem, t->palette,
        (unsigned long)tlut_type, t->masks, t->maskt, t->cms, t->cmt,
        (unsigned long)s_last_load.kind, (unsigned long)s_last_load.siz,
        (unsigned long)s_last_load.dxt, (unsigned long)s_last_load.bytes);
    pal_debug_write(name, s_texels, w * h * 4);
    __builtin_snprintf(name, sizeof name, "sdmc:/3ds/ge007/texdump/%03d.tmem", count);
    pal_debug_write(name, s_tmem, TMEM_SIZE);
    count++;
}
#define NOTE_LOAD(k, sz, d, b) (s_last_load.kind = (k), s_last_load.siz = (sz), s_last_load.dxt = (d), s_last_load.bytes = (b))
#else
#define NOTE_LOAD(k, sz, d, b) ((void)0)
#endif

void tex_reset(void)
{
    static int s_first = 1;
    if (s_first) {   /* tiles start as nothing a command sets */
        s_texels = malloc(MAX_TEXELS * sizeof *s_texels);
        if (!s_texels)
            pal_fatal("no memory for the texture decoder");
        __builtin_memset(s_tile_set, 0xFF, sizeof s_tile_set);
        __builtin_memset(s_tile_size, 0xFF, sizeof s_tile_size);
        s_first = 0;
    }
    for (int i = 0; i < 8; i++)
        s_memo[i].valid = 0;
    for (int i = 0; i < 16; i++)
        s_pal[i].valid = 0;
}

/* A load wrote TMEM bytes [start, end) (end may pass the top: it wraps). */
static void note_write(u32 start, u32 end)
{
    s_gen++;
    if (end > 0x800 || start >= 0x800)
        s_hi_gen++;
}

static void record_range(u32 start, u32 end, u64 hash)
{
    /* older records keep only the bytes the new one leaves them (a record
     * cut in two by it becomes two) */
    int n = 0;
    struct { u16 start, end; u64 hash; } out[MAX_LOADS * 2];
    for (int i = 0; i < s_nloads; i++) {
        u32 a = s_loads[i].start, b = s_loads[i].end;
        if (b <= start || a >= end) {
            out[n].start = (u16)a; out[n].end = (u16)b; out[n].hash = s_loads[i].hash; n++;
            continue;
        }
        if (a < start) {
            out[n].start = (u16)a; out[n].end = (u16)start; out[n].hash = s_loads[i].hash; n++;
        }
        if (b > end) {
            out[n].start = (u16)end; out[n].end = (u16)b; out[n].hash = s_loads[i].hash; n++;
        }
    }
    /* still too many: the oldest becomes a hash of its bytes */
    if (n >= MAX_LOADS)
        tex_materialize();
    while (n >= MAX_LOADS) {
        out[1].hash = hash_words(out[1].hash ^ out[0].hash, s_tmem + out[1].start, out[1].end - out[1].start);
        for (int i = 0; i < n - 1; i++)
            out[i] = out[i + 1];
        n--;
    }
    for (int i = 0; i < n; i++) {
        s_loads[i].start = out[i].start;
        s_loads[i].end = out[i].end;
        s_loads[i].hash = out[i].hash;
    }
    s_loads[n].start = (u16)start;
    s_loads[n].end = (u16)end;
    s_loads[n].hash = hash;
    s_nloads = n + 1;
}

/* TMEM [start, end) (not wrapping) holds exactly what this load would
 * put there: its newest overlapping record is the same load. */
static int load_is_current(u32 start, u32 end, u64 hash)
{
    if (end > TMEM_SIZE)
        return 0;
    for (int i = s_nloads - 1; i >= 0; i--)
        if (s_loads[i].start < end && s_loads[i].end > start)
            return s_loads[i].start == start && s_loads[i].end == end && s_loads[i].hash == hash;
    return 0;
}

/* A load of TMEM [start, end) (end may pass the top: it wraps). */
static void record_load(u32 start, u32 end, u64 hash)
{
    u32 len = end - start;
    start &= TMEM_SIZE - 1;
    end = start + len;
    if (len >= TMEM_SIZE) {
        record_range(0, TMEM_SIZE, hash);
    } else if (end > TMEM_SIZE) {
        record_range(start, TMEM_SIZE, hash);
        record_range(0, end - TMEM_SIZE, hash);
    } else {
        record_range(start, end, hash);
    }
}

/* What a load reads: the source and how, plus 8 words spread over its
 * bytes [src, src + span). */
static u64 load_hash(const u8 *src, u32 span, const u32 *params, u32 nparams)
{
    u32 w[8];
    if (span < 4)
        span = 4;
    for (int i = 0; i < 8; i++)   /* any byte order will do for a hash */
        __builtin_memcpy(&w[i], src + ((span - 4) * (u32)i / 7), 4);
    u64 h = hash_words(0xCBF29CE484222325ull ^ (uintptr_t)src, (const u8 *)params, nparams * 4);
    return hash_words(h, (const u8 *)w, sizeof w);
}

/* Key for TMEM bytes [start, start + len), which may wrap. */
static u64 region_key(u64 h, u32 start, u32 len)
{
    if (g_f3d_rec)
        f3d_rec_tmem_read(start, len);
    u32 s0 = start & (TMEM_SIZE - 1), e0 = s0 + len;
    for (int i = s_nloads - 1; i >= 0; i--) {
        u32 a = s_loads[i].start, b = s_loads[i].end;
        int hit = (a < e0 && b > s0) || (e0 > TMEM_SIZE && a < e0 - TMEM_SIZE);
        if (hit) {
            u32 v[4] = { (u32)s_loads[i].hash, (u32)(s_loads[i].hash >> 32), a, b };
            h = hash_words(h, (const u8 *)v, sizeof v);
        }
    }
    return h;
}

/* ---- Commands ---------------------------------------------------------- */

void tex_settimg(u32 w0, u32 w1)
{
    s_timg.fmt = (w0 >> 21) & 7;
    s_timg.siz = (w0 >> 19) & 3;
    s_timg.width = (w0 & 0xFFF) + 1;
    s_timg.addr = (u32)(uintptr_t)f3d_resolve(w1);
}

int tex_settile(u32 w0, u32 w1)
{
    int i = (w1 >> 24) & 7;
    w0 &= 0x00FFFFFF;
    if (s_tile_set[i][0] == w0 && s_tile_set[i][1] == w1)
        return 0;
    s_tile_set[i][0] = w0;
    s_tile_set[i][1] = w1;
    Tile *t = &s_tile[i];
    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1FF;
    t->tmem = w0 & 0x1FF;
    t->palette = (w1 >> 20) & 0xF;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 0xF;
    t->shiftt = (w1 >> 10) & 0xF;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 0xF;
    t->shifts = w1 & 0xF;
    s_memo[i].valid = 0;
    return 1;
}

int tex_settilesize(u32 w0, u32 w1)
{
    int i = (w1 >> 24) & 7;
    w0 &= 0x00FFFFFF;
    w1 &= 0x00FFFFFF;
    if (s_tile_size[i][0] == w0 && s_tile_size[i][1] == w1)
        return 0;
    s_tile_size[i][0] = w0;
    s_tile_size[i][1] = w1;
    Tile *t = &s_tile[i];
    t->uls = (w0 >> 12) & 0xFFF;
    t->ult = w0 & 0xFFF;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
    s_memo[i].valid = 0;
    return 1;
}

static void tmem_put(u32 off, u8 v, int odd)
{
    s_tmem[(off ^ (odd ? 4 : 0)) & (TMEM_SIZE - 1)] = v;
}

/* One 64-bit word to TMEM offset off (a multiple of 8), halves swapped on
 * odd lines. Bytes stay in N64 order, so a swap moves whole 32-bit halves. */
static void tmem_put_word(u32 off, const u8 *src, int odd)
{
    u32 a, b;
    u32 *d = (u32 *)(s_tmem + (off & (TMEM_SIZE - 8)));
    __builtin_memcpy(&a, src, 4);
    __builtin_memcpy(&b, src + 4, 4);
    d[odd] = a;
    d[!odd] = b;
}

/* Deferred loads. With keys made from load records (above), TMEM's bytes
 * are only read to decode a texture that isn't cached, so a load is
 * queued and its copy done when TMEM is next read (tex_materialize) or at
 * the end of the task. A queued load that a later one overwrites entirely
 * is dropped. Within a task the game's RAM doesn't change under the RSP,
 * so copying late reads the same bytes. */
typedef struct {
    u8 kind;            /* 1 LOADBLOCK, 2 LOADTILE, 3 LOADTLUT */
    u8 siz;
    u16 width;          /* source image width (LOADTILE) */
    const u8 *src;      /* first source byte (LOADTILE: the image) */
    u16 start, end;     /* TMEM bytes written, for dropping covered loads;
                           0, 0 when the load wraps */
    u32 base, stride;   /* TMEM byte offset, row stride */
    u32 a, b, c, d;     /* LOADBLOCK: n, dxt; LOADTILE: uls, ult, lrs, lrt;
                           LOADTLUT: count */
} PendingLoad;

#define MAX_PENDING 64
static PendingLoad s_pend[MAX_PENDING];
static int s_npend;

static void copy_block(const PendingLoad *p)
{
    const u8 *src = p->src;
    u32 n = p->a, dxt = p->b, base = p->base;
    if (p->siz == G_IM_SIZ_32b) {
        for (u32 k = 0; k < n && k < 1024; k++) {
            u32 off = base + k * 2;
            /* dxt counts 64-bit source words: two 32-bit texels each */
            int odd = (((k / 2) * dxt) >> 11) & 1;
            tmem_put(off, src[k * 4 + 0], odd);
            tmem_put(off + 1, src[k * 4 + 1], odd);
            tmem_put((off & 0x7FF) + 0x800, src[k * 4 + 2], odd);
            tmem_put(((off + 1) & 0x7FF) + 0x800, src[k * 4 + 3], odd);
        }
        return;
    }
    u32 words = ((((n << p->siz) >> 1) + 7) & ~7u) / 8;
    if (words > TMEM_SIZE / 8)
        words = TMEM_SIZE / 8;
    for (u32 i = 0; i < words; i++)
        tmem_put_word(base + i * 8, src + i * 8, ((i * dxt) >> 11) & 1);
}

static void copy_tile(const PendingLoad *p)
{
    u32 uls = p->a, ult = p->b, lrs = p->c, lrt = p->d;
    for (u32 r = 0; r <= lrt - ult; r++) {
        const u8 *src = p->src + ((((ult + r) * p->width + uls) << p->siz) >> 1);
        u32 row = p->base + r * p->stride;
        int odd = r & 1;
        if (p->siz == G_IM_SIZ_32b) {
            for (u32 c = 0; c <= lrs - uls; c++) {
                u32 off = row + c * 2;
                tmem_put(off, src[c * 4 + 0], odd);
                tmem_put(off + 1, src[c * 4 + 1], odd);
                tmem_put((off & 0x7FF) + 0x800, src[c * 4 + 2], odd);
                tmem_put(((off + 1) & 0x7FF) + 0x800, src[c * 4 + 3], odd);
            }
        } else {
            u32 bytes = ((lrs - uls + 1) << p->siz) >> 1, i = 0;
            if (!(row & 7))
                for (; i + 8 <= bytes; i += 8)
                    tmem_put_word(row + i, src + i, odd);
            for (; i < bytes; i++)
                tmem_put(row + i, src[i], odd);
        }
    }
}

static void copy_tlut(const PendingLoad *p)
{
    for (u32 i = 0; i < p->a; i++) {   /* each entry fills a doubleword, four times */
        u32 hi = p->src[i * 2], lo = p->src[i * 2 + 1];
        u32 w = hi | (lo << 8) | (hi << 16) | (lo << 24);   /* bytes hi lo hi lo */
        u32 off = (p->base + i * 8) & (TMEM_SIZE - 1);
        *(u32 *)(s_tmem + off) = w;
        *(u32 *)(s_tmem + off + 4) = w;
    }
}

/* Do the queued copies: before anything reads TMEM's bytes. */
static void tex_materialize(void)
{
    for (int i = 0; i < s_npend; i++) {
        const PendingLoad *p = &s_pend[i];
        if (p->kind == 1)
            copy_block(p);
        else if (p->kind == 2)
            copy_tile(p);
        else
            copy_tlut(p);
    }
    s_npend = 0;
}

static void push_pending(const PendingLoad *p)
{
    if (p->end > p->start) {
        int j = 0;
        for (int i = 0; i < s_npend; i++)
            if (!(s_pend[i].end > s_pend[i].start && s_pend[i].start >= p->start && s_pend[i].end <= p->end))
                s_pend[j++] = s_pend[i];
        s_npend = j;
    }
    if (s_npend == MAX_PENDING)
        tex_materialize();
    s_pend[s_npend++] = *p;
}

void tex_task_end(void)
{
    tex_materialize();
}

/* The TMEM range of a load as kept in PendingLoad: 0, 0 if it wraps. */
static void pending_range(PendingLoad *p, u32 s0, u32 e0)
{
    if (e0 > TMEM_SIZE) {
        p->start = p->end = 0;
    } else {
        p->start = (u16)s0;
        p->end = (u16)e0;
    }
}

int tex_loadblock(u32 w0, u32 w1)
{
    Tile *t = &s_tile[(w1 >> 24) & 7];
    u32 uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    u32 lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
    u32 n = lrs - uls + 1, base = t->tmem * 8;

    if (g_f3d_rec) {
        f3d_rec_timg_read();
        f3d_rec_tile_read((w1 >> 24) & 7, 0);
    }
    if (!s_timg.addr || lrs < uls)
        return 0;
    const u8 *src = (const u8 *)(uintptr_t)s_timg.addr + (((ult * s_timg.width + uls) << s_timg.siz) >> 1);
    NOTE_LOAD(1, s_timg.siz, dxt, n);
    u32 params[4] = { 1, s_timg.siz | ((u32)s_timg.fmt << 4), dxt | (base << 12), n };
    u32 span = s_timg.siz == G_IM_SIZ_32b ? n * 4 : (n << s_timg.siz) >> 1;
    u64 h = load_hash(src, span, params, 4);
    u32 s0 = s_timg.siz == G_IM_SIZ_32b ? 0 : base;
    u32 e0 = s_timg.siz == G_IM_SIZ_32b ? TMEM_SIZE : base + ((span + 7) & ~7u);
    if (e0 > s0 + TMEM_SIZE)
        e0 = s0 + TMEM_SIZE;
    if (g_f3d_rec)
        f3d_rec_tmem_load(s0, e0);
    if (load_is_current(s0, e0, h))
        return 0;                   /* reloaded as it is: TMEM unchanged */
    record_load(s0, e0, h);
    note_write(s0, e0);
    PendingLoad p = { 1, s_timg.siz, 0, src, 0, 0, base, 0, n, dxt, 0, 0 };
    pending_range(&p, s0, e0);
    push_pending(&p);
    return 1;
}

int tex_loadtile(u32 w0, u32 w1)
{
    Tile *t = &s_tile[(w1 >> 24) & 7];
    u32 uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    u32 lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    u32 base = t->tmem * 8, stride = t->line * 8;

    if (g_f3d_rec) {
        f3d_rec_timg_read();
        f3d_rec_tile_read((w1 >> 24) & 7, 0);
    }
    if (!s_timg.addr || lrs < uls || lrt < ult)
        return 0;
    int changed = t->uls != uls << 2 || t->ult != ult << 2 || t->lrs != lrs << 2 || t->lrt != lrt << 2;
    if (changed) {
        t->uls = uls << 2; t->ult = ult << 2;
        t->lrs = lrs << 2; t->lrt = lrt << 2;
        s_memo[(w1 >> 24) & 7].valid = 0;
        s_tile_size[(w1 >> 24) & 7][0] = ~0u;   /* not as G_SETTILESIZE set it now */
    }
    NOTE_LOAD(2, s_timg.siz, s_timg.width, (lrs - uls + 1) | ((lrt - ult + 1) << 16));

    const u8 *img = (const u8 *)(uintptr_t)s_timg.addr;
    const u8 *src0 = img + (((ult * s_timg.width + uls) << s_timg.siz) >> 1);
    u32 params[5] = { 2, s_timg.siz | ((u32)s_timg.fmt << 4) | ((u32)s_timg.width << 8),
                      base | (stride << 16), (lrs - uls) | ((lrt - ult) << 16), 0 };
    u32 span = (((lrt - ult) * s_timg.width + (lrs - uls) + 1) << s_timg.siz) >> 1;
    u64 h = load_hash(src0, span, params, 5);
    u32 s0 = s_timg.siz == G_IM_SIZ_32b ? 0 : base;
    u32 e0 = s_timg.siz == G_IM_SIZ_32b ? TMEM_SIZE
           : base + (lrt - ult) * stride + ((((lrs - uls + 1) << s_timg.siz) >> 1) + 7) / 8 * 8;
    if (e0 > s0 + TMEM_SIZE)
        e0 = s0 + TMEM_SIZE;
    if (g_f3d_rec)
        f3d_rec_tmem_load(s0, e0);
    if (load_is_current(s0, e0, h))
        return changed;             /* reloaded as it is: TMEM unchanged */
    record_load(s0, e0, h);
    note_write(s0, e0);
    PendingLoad p = { 2, s_timg.siz, s_timg.width, img, 0, 0, base, stride, uls, ult, lrs, lrt };
    pending_range(&p, s0, e0);
    push_pending(&p);
    return 1;
}

/* Entries uls..lrs of row ult, like a LOADTILE: Rare keeps a texture's
 * palette after its texels and starts the load past them (gDPLoadTLUT07). */
int tex_loadtlut(u32 w0, u32 w1)
{
    Tile *t = &s_tile[(w1 >> 24) & 7];
    u32 uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    u32 lrs = ((w1 >> 12) & 0xFFF) >> 2;
    u32 count = lrs - uls + 1, base = t->tmem * 8;
    const u8 *src = (const u8 *)(uintptr_t)s_timg.addr;

    if (g_f3d_rec) {
        f3d_rec_timg_read();
        f3d_rec_tile_read((w1 >> 24) & 7, 0);
    }
    if (!src || lrs < uls)
        return 0;
    src += (((ult * s_timg.width + uls) << s_timg.siz) >> 1);
    if (count > 256)
        count = 256;
    u32 params[3] = { 3, base, count };
    u64 h = load_hash(src, count * 2, params, 3);
    u32 e0 = base + count * 8;
    if (g_f3d_rec)
        f3d_rec_tmem_load(base, e0);
    if (load_is_current(base, e0, h))
        return 0;                   /* the same palette again */
    note_write(base, e0);
    record_load(base, e0, h);
    PendingLoad p = { 3, 0, 0, src, 0, 0, base, 0, count, 0, 0, 0 };
    pending_range(&p, base, e0);
    push_pending(&p);
    return 1;
}

/* ---- Decoding ------------------------------------------------------------ */

static u32 rgba5551(u16 c)
{
    u32 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return (r << 24) | (g << 16) | (b << 8) | ((c & 1) ? 0xFF : 0);
}

static u32 ia(u32 i, u32 a)
{
    return (i << 24) | (i << 16) | (i << 8) | a;
}

static u32 tlut(u32 index, u32 tlut_type)
{
    u32 off = 0x800 + index * 8;
    u16 c = (u16)((s_tmem[off & 0xFFF] << 8) | s_tmem[(off + 1) & 0xFFF]);
    if (tlut_type == (G_TT_IA16 >> G_MDSFT_TEXTLUT))
        return ia(c >> 8, c & 0xFF);
    return rgba5551(c);
}

/* Row y of tile t (relative to the tile's origin), texels 0 .. n - 1. pal
 * holds the TLUT's colours when one is on (CI, and any 16-bit texel, which
 * the RDP looks up by its high byte: sky.c loads the CI8 water image and
 * retypes the tiles RGBA16). */
static void decode_row(const Tile *t, u32 y, u32 n, u32 tlut_type, const u32 *pal, u32 *out)
{
    u32 row = t->tmem * 8 + y * t->line * 8;
    u32 sw = (y & 1) ? 4 : 0;           /* odd rows: 32-bit halves swapped */
#define TM(off) s_tmem[((off) ^ sw) & (TMEM_SIZE - 1)]
    switch (t->siz) {
    case G_IM_SIZ_4b:
        if (t->fmt == G_IM_FMT_CI && tlut_type) {
            const u32 *p = pal + t->palette * 16;
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x / 2);
                out[x] = p[(x & 1) ? (v & 0xF) : (v >> 4)];
            }
        } else if (t->fmt == G_IM_FMT_CI) {     /* no TLUT: the index as I */
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x / 2);
                v = ((x & 1) ? (v & 0xF) : (v >> 4)) | t->palette << 4;
                out[x] = ia(v, v);
            }
        } else if (t->fmt == G_IM_FMT_IA) {
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x / 2);
                v = (x & 1) ? (v & 0xF) : (v >> 4);
                out[x] = ia((v >> 1) * 255 / 7, (v & 1) ? 0xFF : 0);
            }
        } else {                                /* I4 */
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x / 2);
                v = (x & 1) ? (v & 0xF) : (v >> 4);
                out[x] = ia(v * 17, v * 17);
            }
        }
        break;
    case G_IM_SIZ_8b:
        if (t->fmt == G_IM_FMT_CI && tlut_type) {
            for (u32 x = 0; x < n; x++)
                out[x] = pal[TM(row + x)];
        } else if (t->fmt == G_IM_FMT_IA) {
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x);
                out[x] = ia((v >> 4) * 17, (v & 0xF) * 17);
            }
        } else {                                /* I8, or CI8 without a TLUT */
            for (u32 x = 0; x < n; x++) {
                u32 v = TM(row + x);
                out[x] = ia(v, v);
            }
        }
        break;
    case G_IM_SIZ_16b:
        if (tlut_type) {
            for (u32 x = 0; x < n; x++)
                out[x] = pal[TM(row + x * 2)];
        } else if (t->fmt == G_IM_FMT_IA) {
            for (u32 x = 0; x < n; x++)
                out[x] = ia(TM(row + x * 2), TM(row + x * 2 + 1));
        } else {
            for (u32 x = 0; x < n; x++)
                out[x] = rgba5551((u16)(TM(row + x * 2) << 8 | TM(row + x * 2 + 1)));
        }
        break;
    default:                                    /* 32b: RG low, BA high */
        for (u32 x = 0; x < n; x++) {
            u32 off = (row + x * 2) & 0x7FF;
            out[x] = (u32)TM(off) << 24 | (u32)TM(off + 1) << 16 | (u32)TM(off + 0x800) << 8 |
                     TM(off + 0x801);
        }
        break;
    }
#undef TM
}

typedef struct {
    u32 size;       /* GPU texture size */
    u32 period;     /* wrap period in texels, or the clamp extent */
    int mirror;     /* period is mirrored (baked) */
    int wrap;       /* GFX_WRAP_* */
} Axis;

static u32 pow2_at_least(u32 v)
{
    u32 p = 8;
    while (p < v && p < MAX_DIM)
        p <<= 1;
    return p;
}

static void axis_setup(Axis *a, u32 extent, u32 cm, u32 mask)
{
    if (extent == 0)
        extent = 1;
    if (mask && !(cm & G_TX_CLAMP)) {
        a->period = 1u << mask;
        a->mirror = (cm & G_TX_MIRROR) != 0;
        if (a->period > MAX_DIM)
            a->period = MAX_DIM;
        if (a->period >= 8) {
            a->size = a->period;
            a->wrap = a->mirror ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT;
            a->mirror = 0;                  /* the GPU mirrors */
        } else {
            a->size = a->mirror ? pow2_at_least(a->period * 2) : 8;
            a->wrap = GFX_WRAP_REPEAT;      /* pattern baked in */
        }
    } else {
        a->period = extent > MAX_DIM ? MAX_DIM : extent;
        a->mirror = 0;
        a->size = pow2_at_least(a->period);
        a->wrap = GFX_WRAP_CLAMP;
    }
}

/* Wrapped periods are powers of two (1 << mask), so no divides: the ARM11
 * has no divide instruction, and this runs per texel. */
static u32 axis_index(const Axis *a, u32 x)
{
    if (a->wrap == GFX_WRAP_CLAMP)
        return x < a->period ? x : a->period - 1;
    if (a->mirror) {
        u32 m = x & (a->period * 2 - 1);
        return m < a->period ? m : a->period * 2 - 1 - m;
    }
    return x & (a->period - 1);
}

static float shift_scale(u32 shift)
{
    if (shift == 0)
        return 1.0f;
    if (shift <= 10)
        return 1.0f / (float)(1u << shift);
    return (float)(1u << (16 - shift));
}

/* 64-bit FNV-1a over 32-bit words. */
/* Two 32-bit FNV-style lanes over alternate words: 32-bit multiplies only,
 * which the ARM11 does far faster than a 64-bit FNV step. p is 4-aligned. */
static u64 hash_words(u64 h, const u8 *p, u32 bytes)
{
    const u32 *w = (const u32 *)p;
    u32 a = (u32)h, b = (u32)(h >> 32), n = bytes / 8;
    for (u32 i = 0; i < n; i++) {
        a = (a ^ w[2 * i]) * 0x01000193u;
        b = (b ^ w[2 * i + 1]) * 0x5BD1E995u;
    }
    if (bytes & 4)
        a = (a ^ w[2 * n]) * 0x01000193u;
    a ^= b >> 15;
    b ^= a >> 13;
    return ((u64)b << 32) | a;
}

/* The largest byte in TMEM [start, start + len) (8-aligned, may wrap): an
 * upper bound on the palette indices of the texels there (for 16-bit
 * texels, whose high byte is the index, it counts the low bytes too). By
 * the texels' key, so TMEM is only read the first time. */
static u32 max_index(u64 texkey, u32 start, u32 len)
{
    static struct {
        u64 key;
        u8 max, valid;
    } cache[8192];
    u32 slot = (u32)(texkey ^ (texkey >> 32) ^ (texkey >> 13)) & 8191;
    if (cache[slot].valid && cache[slot].key == texkey)
        return cache[slot].max;
    tex_materialize();
    u32 m = 0;
    for (u32 i = 0; i < len; i += 4) {
        u32 w = *(const u32 *)(s_tmem + ((start + i) & (TMEM_SIZE - 4))), d;
        /* bytewise max: usub8 sets a GE flag per byte of w >= m, sel picks */
        __asm__("usub8 %0, %1, %2\n\tsel %0, %1, %2" : "=&r"(d) : "r"(w), "r"(m) : "cc");
        m = d;
    }
    u32 hi = m >> 24, b;
    b = (m >> 16) & 0xFF; hi = b > hi ? b : hi;
    b = (m >> 8) & 0xFF; hi = b > hi ? b : hi;
    b = m & 0xFF; hi = b > hi ? b : hi;
    cache[slot].key = texkey;
    cache[slot].max = (u8)hi;
    cache[slot].valid = 1;
    return hi;
}

/* A tile's GPU texture size, coordinate mapping and key (what the decode
 * reads and how). */
typedef struct {
    const Tile *t;
    Axis as, at;
    u32 tlut_type;
    int linear;
    u64 key;
} TilePrep;

static void tile_prep(int tile, u32 om_h, TexMap *map, TilePrep *pp)
{
    tile &= 7;
    const Tile *t = &s_tile[tile];
    if (g_f3d_rec) {
        f3d_rec_tile_read(tile, 0);
        f3d_rec_tile_read(tile, 1);
    }
    u32 tlut_type = (om_h >> G_MDSFT_TEXTLUT) & 3;
    u32 filt = (om_h >> G_MDSFT_TEXTFILT) & 3;
    int linear = filt != (G_TF_POINT >> G_MDSFT_TEXTFILT);
    u32 om = tlut_type | (filt << 2);

    u32 w = ((t->lrs - t->uls) >> 2) + 1, h = ((t->lrt - t->ult) >> 2) + 1;
    if (t->lrs < t->uls) w = 1;
    if (t->lrt < t->ult) h = 1;
    Axis as, at;
    axis_setup(&as, w, t->cms, t->masks);
    axis_setup(&at, h, t->cmt, t->maskt);

    if (map) {
        map->scale_s = shift_scale(t->shifts);
        map->scale_t = shift_scale(t->shiftt);
        map->off_s = t->uls / 4.0f - (linear ? 0.5f : 0.0f);
        map->off_t = t->ult / 4.0f - (linear ? 0.5f : 0.0f);
        map->inv_w = 1.0f / as.size;
        map->inv_h = 1.0f / at.size;
    }

    pp->t = t;
    pp->as = as;
    pp->at = at;
    pp->tlut_type = tlut_type;
    pp->linear = linear;

    /* The key: what the decode reads (the tile's TMEM rows, the palette)
     * and how it reads it. */
    u64 key;
    if (s_memo[tile].valid && s_memo[tile].gen == s_gen && s_memo[tile].om == om) {
        key = s_memo[tile].key;
        pp->key = key;
        return;
    }
    u32 rows_bytes = t->line * 8 * at.period;
    u32 start = t->tmem * 8;
    key = 0xCBF29CE484222325ull;
    u32 params[6] = { t->fmt | (t->siz << 4) | (t->palette << 8) | (tlut_type << 12) | ((u32)linear << 14),
                      t->line | ((u32)t->tmem << 16), as.size | (at.size << 16),
                      as.period | (at.period << 16), (u32)as.wrap | ((u32)at.wrap << 4) |
                      ((u32)as.mirror << 8) | ((u32)at.mirror << 9), 0 };
    key = hash_words(key, (const u8 *)params, sizeof params);
    if (rows_bytes > TMEM_SIZE)
        rows_bytes = TMEM_SIZE;
    key = region_key(key, start, rows_bytes);
    if (t->siz == G_IM_SIZ_32b)
        key = region_key(key, 0x800, 0x800);
    /* The palette, where the decode looks texels up in one (CI with a TLUT
     * on, and any 16-bit texel: see decode_row). For 8 and 16 bits only the
     * entries the texels can index count: what lies beyond them in TMEM
     * (other textures' palettes) made a new key for the same image over and
     * over (Statue: 171 textures made for 31 images in 5 s). */
    if (tlut_type && (t->fmt == G_IM_FMT_CI || t->siz == G_IM_SIZ_16b) && t->siz != G_IM_SIZ_32b) {
        u64 ph;
        if (t->siz == G_IM_SIZ_4b) {
            int p = t->palette;
            if (!s_pal[p].valid || s_pal[p].gen != s_hi_gen) {
                s_pal[p].hash = region_key(0xCBF29CE484222325ull, 0x800 + p * 0x80, 0x80);
                s_pal[p].gen = s_hi_gen;
                s_pal[p].valid = 1;
            }
            ph = s_pal[p].hash;
        } else {
            /* per entry count, while the high TMEM is unchanged */
            static struct { u32 gen; u64 hash; } s_pal8[256];
            u32 n = max_index(key, start, rows_bytes) + 1;
            if (s_pal8[n - 1].gen != s_hi_gen + 1) {
                s_pal8[n - 1].hash = region_key(0xCBF29CE484222325ull ^ n, 0x800, n * 8);
                s_pal8[n - 1].gen = s_hi_gen + 1;
            }
            ph = s_pal8[n - 1].hash;
        }
        key = (key ^ ph) * 0x100000001B3ull;
    }

    s_memo[tile].valid = 1;
    s_memo[tile].gen = s_gen;
    s_memo[tile].om = om;
    s_memo[tile].key = key;
    s_memo[tile].id = 0;

    pp->key = key;
}

/* Decodes a prepared tile into out (as.size x at.size texels). Each source
 * row is decoded once; padding and repeats are copies. */
static void tile_decode(const TilePrep *pp, u32 *out_texels)
{
    const Tile *t = pp->t;
    const Axis *as = &pp->as, *at = &pp->at;
    static u32 row[MAX_DIM];
    static u16 first[MAX_DIM];
    /* The palette's colours, decoded a block of 16 at a time and kept while
     * the high TMEM and the TLUT type stay the same (a mip chain's levels
     * share one; a CI8 palette is 256 entries). */
    static u32 pal[256], pal_gen, pal_type;
    static u16 pal_blocks;
    u32 cols = as->period < as->size ? as->period : as->size;
    __builtin_memset(first, 0xFF, at->period * sizeof first[0]);   /* sy < period */
    if (pp->tlut_type) {
        if (pal_gen != s_hi_gen + 1 || pal_type != pp->tlut_type) {
            pal_gen = s_hi_gen + 1;
            pal_type = pp->tlut_type;
            pal_blocks = 0;
        }
        u32 lo = 0, hi = 16;
        if (t->siz == G_IM_SIZ_4b) {
            lo = t->palette;
            hi = lo + 1;
        }
        for (u32 b = lo; b < hi; b++) {
            if (pal_blocks & (1u << b))
                continue;
            for (u32 i = b * 16; i < b * 16 + 16; i++)
                pal[i] = tlut(i, pp->tlut_type);
            pal_blocks |= 1u << b;
        }
    }
    for (u32 y = 0; y < at->size; y++) {
        u32 sy = axis_index(at, y);
        u32 *out = out_texels + y * as->size;
        if (first[sy] != 0xFFFF) {
            __builtin_memcpy(out, out_texels + first[sy] * as->size, as->size * 4);
            continue;
        }
        first[sy] = (u16)y;
        if (as->wrap == GFX_WRAP_CLAMP || (!as->mirror && cols == as->period)) {
            /* straight into place, then padding or whole repeats */
            decode_row(t, sy, cols, pp->tlut_type, pal, out);
            if (as->wrap == GFX_WRAP_CLAMP) {
                for (u32 x = cols; x < as->size; x++)
                    out[x] = out[cols - 1];
            } else {
                for (u32 x = cols; x < as->size; x += cols)
                    __builtin_memcpy(out + x, out, cols * 4);
            }
        } else {
            decode_row(t, sy, cols, pp->tlut_type, pal, row);
            for (u32 x = 0; x < as->size; x++)
                out[x] = row[axis_index(as, x)];
        }
    }
}

u16 tex_get(int tile, u32 om_h, TexMap *map)
{
    /* The same tile, TMEM and modes as last time this task: the same
     * texture (one used this frame isn't evicted). */
    tile &= 7;
    u32 om = ((om_h >> G_MDSFT_TEXTLUT) & 3) | (((om_h >> G_MDSFT_TEXTFILT) & 3) << 2);
    if (s_memo[tile].valid && s_memo[tile].id && s_memo[tile].gen == s_gen && s_memo[tile].om == om) {
        *map = s_memo[tile].map;
        return s_memo[tile].id;
    }
    TilePrep pp;
    tile_prep(tile, om_h, map, &pp);
    u16 id = gfxb_tex_find(pp.key);
    if (!id && pp.as.size * pp.at.size > MAX_TEXELS)
        return 0;
    if (!id) {
        tex_materialize();
        tile_decode(&pp, s_texels);
#ifdef F3D_TEX_DUMP
        if (pp.t->fmt == G_IM_FMT_CI)   /* DEBUG(temp) */
            dump(pp.t, pp.as.size, pp.at.size, pp.tlut_type);
#endif
        id = gfxb_tex_create(pp.key, (const uint32_t *)s_texels, pp.as.size, pp.at.size,
                             pp.as.wrap, pp.at.wrap, pp.linear);
    }
    if (s_memo[tile].valid && s_memo[tile].gen == s_gen && s_memo[tile].om == om) {
        s_memo[tile].id = id;
        s_memo[tile].map = *map;
    }
    return id;
}

/* A mipmapped texture from tiles tile .. tile + levels, each half the size
 * of the one before (G_TL_LOD in clamp mode: the RDP picks the tile pair
 * by LOD and blends them by LOD_FRACTION; the GPU's trilinear filter does
 * the same). Levels stop at 8 texels a side or where the chain doesn't
 * halve. 0 if there is no usable chain. */
u16 tex_get_mip(int tile, int levels, u32 om_h, TexMap *map)
{
    static u32 s_mip[MAX_TEXELS / 2];
    TilePrep pp[8];
    tile_prep(tile, om_h, map, &pp[0]);
    u32 w = pp[0].as.size, h = pp[0].at.size;
    int n = 1;
    u64 key = pp[0].key ^ 0x4D49504D41500000ull;   /* "MIPMAP" */
    for (int k = 1; k <= levels && k < 8; k++) {
        if ((w >> k) < 8 || (h >> k) < 8)
            break;
        tile_prep(tile + k, om_h, NULL, &pp[k]);
        if (pp[k].as.size != (w >> k) || pp[k].at.size != (h >> k))
            break;
        key = (key ^ pp[k].key) * 0x100000001B3ull;
        n++;
    }
    if (n < 2)
        return 0;
    key = (key ^ (u64)n) * 0x100000001B3ull;
    u16 id = gfxb_tex_find(key);
    if (id || w * h > MAX_TEXELS)
        return id;

    tex_materialize();
    const uint32_t *lv[8];
    tile_decode(&pp[0], s_texels);
    lv[0] = (const uint32_t *)s_texels;
    u32 used = 0;
    for (int k = 1; k < n; k++) {
        tile_decode(&pp[k], s_mip + used);
        lv[k] = (const uint32_t *)(s_mip + used);
        used += (w >> k) * (h >> k);
    }
    return gfxb_tex_create_mip(key, lv, n, w, h, pp[0].as.wrap, pp[0].at.wrap, pp[0].linear);
}

/* ---- Room replay (f3d.c) ---------------------------------------------- */

void tex_regs_get(TexRegs *r)
{
    __builtin_memcpy(r->tile, s_tile, sizeof s_tile);
    __builtin_memcpy(r->set, s_tile_set, sizeof s_tile_set);
    __builtin_memcpy(r->size, s_tile_size, sizeof s_tile_size);
    r->timg_fmt = s_timg.fmt;
    r->timg_siz = s_timg.siz;
    r->timg_width = s_timg.width;
    r->timg_addr = s_timg.addr;
}

int tex_regs_tile_same(const TexRegs *r, int i, int size)
{
    const Tile *a = &s_tile[i], *b = &r->tile[i];
    if (size)
        return a->uls == b->uls && a->ult == b->ult && a->lrs == b->lrs && a->lrt == b->lrt;
    return s_tile_set[i][0] == r->set[i][0] && s_tile_set[i][1] == r->set[i][1];
}

int tex_regs_timg_same(const TexRegs *r)
{
    return s_timg.fmt == r->timg_fmt && s_timg.siz == r->timg_siz && s_timg.width == r->timg_width &&
           s_timg.addr == r->timg_addr;
}

/* The load records as they are (a room's on entry), and the key of TMEM
 * bytes [start, start + len) by them (no recording hook). */
void tex_loads_get(TexLoads *l)
{
    l->n = s_nloads;
    for (int i = 0; i < s_nloads; i++) {
        l->start[i] = s_loads[i].start;
        l->end[i] = s_loads[i].end;
        l->hash[i] = s_loads[i].hash;
    }
}

u64 tex_loads_key(const TexLoads *l, u32 start, u32 len)
{
    TexLoads live;
    if (!l) {
        tex_loads_get(&live);
        l = &live;
    }
    u64 h = 0xCBF29CE484222325ull;
    u32 s0 = start & (TMEM_SIZE - 1), e0 = s0 + len;
    for (int i = l->n - 1; i >= 0; i--) {
        u32 a = l->start[i], b = l->end[i];
        if ((a < e0 && b > s0) || (e0 > TMEM_SIZE && a < e0 - TMEM_SIZE)) {
            u32 v[4] = { (u32)l->hash[i], (u32)(l->hash[i] >> 32), a, b };
            h = hash_words(h, (const u8 *)v, sizeof v);
        }
    }
    return h;
}

void tex_rec_begin(void)
{
    s_gen++;
    s_hi_gen++;
}

void tex_debug_tile(int i)   /* DEBUG(temp) */
{
    const Tile *t = &s_tile[i & 7];
    pal_log("  tile %d: fmt %u siz %u line %u tmem %u pal %u cm %u/%u mask %u/%u shift %u/%u size %u,%u-%u,%u timg %08lX",
            i, t->fmt, t->siz, t->line, t->tmem, t->palette, t->cms, t->cmt, t->masks, t->maskt,
            t->shifts, t->shiftt, t->uls, t->ult, t->lrs, t->lrt, (unsigned long)s_timg.addr);
}
