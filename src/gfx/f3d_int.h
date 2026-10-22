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

/* ---- f3d_tex.c ---- */

typedef struct {
    u8 fmt, siz, palette;
    u8 cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;            /* in 64-bit words */
    u16 uls, ult, lrs, lrt;    /* 10.2 */
} Tile;

/* How a tile's texture coordinates map onto its GPU texture:
 * u = (s * scale_s - off_s) * inv_w, likewise v. s, t in texels, as the
 * RSP hands them to the RDP (after G_TEXTURE's scale). */
typedef struct {
    float scale_s, scale_t;
    float off_s, off_t;
    float inv_w, inv_h;
} TexMap;

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
