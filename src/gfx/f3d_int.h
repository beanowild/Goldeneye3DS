/*
 * f3d_int.h - shared between the F3D interpreter's files (f3d.c, the
 * display-list walker; f3d_tex.c, TMEM and textures; f3d_cc.c, the colour
 * combiner). Game side: <ultra64.h> types.
 */
#ifndef F3D_INT_H
#define F3D_INT_H

#include <ultra64.h>

#include "gfx/gfx_backend.h"

/* ---- f3d.c ---- */
void *f3d_resolve(u32 addr);

/* Room replay (f3d.c): while a room's display list is being recorded
 * (g_f3d_rec), f3d_tex.c reports which tiles and TMEM bytes it reads and
 * which TMEM bytes the room loads, so that a recording that depends on
 * what came before the room is noticed. Ranges may wrap past the top. */
extern int g_f3d_rec;
void f3d_rec_tile_read(int tile, int size);   /* a tile's descriptor (0) or size (1) */
void f3d_rec_timg_read(void);
void f3d_rec_tmem_load(u32 start, u32 end);
void f3d_rec_tmem_read(u32 start, u32 len);

/* ---- f3d_tex.c ---- */

typedef struct {
    u8 fmt, siz, palette;
    u8 cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;            /* in 64-bit words */
    u16 uls, ult, lrs, lrt;    /* 10.2 */
} Tile;

/* The tiles and texture image as commands left them: a room's on entry
 * and exit (room replay). */
typedef struct {
    Tile tile[8];
    u32 set[8][2], size[8][2];
    u8 timg_fmt, timg_siz;
    u16 timg_width;
    u32 timg_addr;
} TexRegs;

void tex_regs_get(TexRegs *r);
int tex_regs_tile_same(const TexRegs *r, int tile, int size);   /* live == r's */
int tex_regs_timg_same(const TexRegs *r);
/* TMEM's load records (see f3d_tex.c), and a key for bytes [start,
 * start + len) by them: l's, or the live ones if l is NULL. */
typedef struct {
    int n;
    u16 start[32], end[32];
    u64 hash[32];
} TexLoads;
void tex_loads_get(TexLoads *l);
u64 tex_loads_key(const TexLoads *l, u32 start, u32 len);
/* A recording starts: memos made from earlier TMEM must be made again, so
 * that their reads are seen. */
void tex_rec_begin(void);

/* How a tile's texture coordinates map onto its GPU texture:
 * u = (s * scale_s - off_s) * inv_w, likewise v. s, t in texels, as the
 * RSP hands them to the RDP (after G_TEXTURE's scale). */
typedef struct {
    float scale_s, scale_t;
    float off_s, off_t;
    float inv_w, inv_h;
} TexMap;

/* Room exits waiting to be run (f3d.c): f3d_tex.c's entry points run
 * them first, through TEX_SYNC. */
extern int g_tex_deferred;
void f3d_exits_flush(void);
#define TEX_SYNC() do { if (__builtin_expect(g_tex_deferred, 0)) f3d_exits_flush(); } while (0)

void tex_reset(void);
void tex_task_end(void);   /* finish deferred TMEM loads */
void tex_settimg(u32 w0, u32 w1);
/* These return whether a tile or TMEM changed. */
int tex_settile(u32 w0, u32 w1);
int tex_settilesize(u32 w0, u32 w1);
int tex_loadblock(u32 w0, u32 w1);
int tex_loadtile(u32 w0, u32 w1);
int tex_loadtlut(u32 w0, u32 w1);

/* The GPU texture for a tile in the current TMEM/tile state (0 if it
 * can't be made), and its coordinate mapping. om_h supplies the TLUT
 * type and filter. */
u16 tex_get(int tile, u32 om_h, TexMap *map);
/* The same for a mip chain in tiles tile .. tile + levels (G_TL_LOD), or 0. */
u16 tex_get_mip(int tile, int levels, u32 om_h, TexMap *map);
void tex_debug_tile(int i);   /* DEBUG(temp) */

/* ---- f3d_cc.c ---- */

typedef struct {
    u32 cc0, cc1;           /* G_SETCOMBINE words (w0 without the opcode) */
    int two_cycle;
    u32 prim, env;          /* 0xRRGGBBAA */
    u8 prim_lod;            /* primitive LOD fraction, 0-255 */
    u8 lod_frac;            /* LOD_FRACTION, 0-255: per pixel on the RDP,
                               one value per draw here (f3d.c) */
} CcInput;

/* Fills st->stages, num_stages and buf_update; returns the texture units
 * the result reads (bit 0: unit 0, i.e. the G_TEXTURE tile; bit 1: tile + 1). */
int cc_compile(const CcInput *in, GfxState *st);

#endif /* F3D_INT_H */
