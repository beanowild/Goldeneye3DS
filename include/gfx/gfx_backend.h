/*
 * gfx_backend.h - what the F3D interpreter (src/gfx/f3d*.c, game side) asks
 * of the GPU backend (src/gfx/gfx_c3d.c, citro3d side).
 *
 * Neutral types only: f3d.c includes <ultra64.h>, gfx_c3d.c includes
 * <3ds.h>, and the two can't share a translation unit.
 *
 * The interpreter does the N64's geometry work on the CPU (matrix
 * transform, lighting, culling, viewport) and hands over triangles whose
 * positions are already in the backend's clip space, so the GPU only
 * clips, rasterises and shades. Rooms are the exception: their geometry is
 * kept on the GPU in model space (gfxb_static_*) and drawn through a
 * transform (gfxb_set_xform), culled by the GPU. The colour combiner arrives already
 * compiled to texture-environment stages (f3d_cc.c), and textures already
 * decoded to RGBA8 (f3d_tex.c).
 */
#ifndef GFX_BACKEND_H
#define GFX_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One vertex, as uploaded to the GPU. Through the current transform
 * (identity but for rooms), x, y in [-w, w] cover the screen left to right
 * and bottom to top; z in [-w, 0] is near to far.
 * s, t are texel coordinates as the RSP hands them to the RDP; the state
 * maps them onto each texture unit (GfxState.tex_map). r, g, b, a is the
 * shade colour (GfxState.shade). Nothing in a vertex depends on the
 * state, so the interpreter writes each one once. */
typedef struct {
    float x, y, z, w;
    float s, t;
    uint8_t r, g, b, a;
} GfxVtx;

/* Texture-environment values: the PICA's own encodings (GPU_TEVSRC,
 * GPU_TEVOP_RGB / GPU_TEVOP_A, GPU_COMBINEFUNC), so the backend passes them
 * straight through. */
enum {
    GFX_SRC_PRIMARY = 0x00,      /* vertex colour (N64 shade) */
    GFX_SRC_TEX0 = 0x03,
    GFX_SRC_TEX1 = 0x04,
    GFX_SRC_PREV_BUFFER = 0x0D,  /* saved stage output (N64 combined, cycle 2) */
    GFX_SRC_CONSTANT = 0x0E,
    GFX_SRC_PREVIOUS = 0x0F,

    GFX_OP_COLOR = 0x00,          /* rgb operands */
    GFX_OP_ONE_MINUS_COLOR = 0x01,
    GFX_OP_ALPHA = 0x02,
    GFX_OP_ONE_MINUS_ALPHA = 0x03,
    GFX_OP_A_ALPHA = 0x00,        /* alpha operands */
    GFX_OP_A_ONE_MINUS_ALPHA = 0x01,

    GFX_FN_REPLACE = 0,
    GFX_FN_MODULATE = 1,
    GFX_FN_ADD = 2,
    GFX_FN_INTERPOLATE = 4,       /* s0 * s2 + s1 * (1 - s2) */
    GFX_FN_SUBTRACT = 5,
    GFX_FN_MULTIPLY_ADD = 8,      /* s0 * s1 + s2 */
};

#define GFX_MAX_STAGES 6

typedef struct {
    uint8_t func, src[3], op[3];
} GfxTevArgs;

typedef struct {
    GfxTevArgs rgb, a;
    uint32_t constant;           /* 0xRRGGBBAA */
} GfxTevStage;

/* Pipeline state for the triangles that follow. Compared with memcmp to
 * batch draws, so always build it from a zeroed struct. */
typedef struct {
    bool depth_test;
    bool depth_write;
    bool depth_equal;            /* decals: pass on equal depth too */
    bool blend;                  /* alpha blend against the framebuffer */
    bool alpha_test;
    uint8_t alpha_ref;
    uint8_t num_stages;          /* 0: vertex colour as is */
    uint8_t buf_update;          /* stage i's output saved to the buffer, bit i */
    uint16_t tex[2];             /* texture ids per unit, 0 = none */
    uint16_t scissor[4];         /* x0, y0, x1, y1 in top-screen pixels (x1, y1
                                    exclusive); all 0: none */
    bool shade;                  /* vertex colours; else white */
    bool noise;                  /* unit 1 also adds noise_scale * (x/w, y/w) */
    uint8_t stereo;              /* GFX_STEREO_*: how the two eyes see it */
    uint8_t cull;                /* GFX_CULL_*: faces the GPU drops (rooms) */
    GfxTevStage stages[GFX_MAX_STAGES];
    /* per unit: u = s * tex_map[0] + tex_map[2], v = t * tex_map[1] + tex_map[3]
     * (v = 0 is the first row of the image) */
    float tex_map[2][4];
    float noise_scale[2];
    float stereo_c;              /* GFX_STEREO_WORLD: the w drawn at the screen's depth */
} GfxState;

/* Stereo 3D: each eye's x is shifted by e * k * max(w - c, -POP * w) for
 * GFX_STEREO_WORLD (c = stereo_c: at the screen's depth; farther recedes,
 * nearer comes out by at most POP times the depth at infinity, gfx_c3d.c),
 * by e * k * w for GFX_STEREO_SKY (at infinity), e * k * stereo_c * w for
 * GFX_STEREO_LAYER (menus), and not at all for GFX_STEREO_FLAT (HUD,
 * rectangles). The shift is affine in clip space (but for the POP cap,
 * right by the eye), so T-junctions between pieces of level geometry stay
 * closed. e is -1 for the left eye and +1 for the right (so beyond c the
 * left eye's picture sits left of the right eye's: behind the screen); k
 * follows the 3D slider. */
enum { GFX_STEREO_FLAT, GFX_STEREO_WORLD, GFX_STEREO_SKY,
       GFX_STEREO_LAYER };   /* at stereo_c of the way to infinity, all of it (menus) */

/* Front faces wind counter-clockwise in clip space (y up), as the N64's. */
enum { GFX_CULL_NONE, GFX_CULL_BACK, GFX_CULL_FRONT };

/* Model space to clip space for the draws that follow (f3d.v.pica xf): a
 * palette of n transforms (at most GFX_XFORM_MAX), each GFX_XFORM_ROWS
 * rows: clip x, y, z, w as rows . (x, y, z, 1), then the game's own clip
 * z. A vertex's w is GFX_XFORM_ROWS * the index of its transform. fog =
 * (scale, offset, on, 0): the RSP's fog, replacing vertex alpha with 255 *
 * clamp(z / w * scale + offset), 0 behind the eye. rows NULL: vertices are
 * in clip space already (every frame starts so). */
#define GFX_XFORM_ROWS 5
#define GFX_XFORM_MAX 15
void gfxb_set_xform(const float (*rows)[4], int n, const float fog[4]);

/* Static geometry: vertices and indices kept on the GPU from frame to
 * frame. gfxb_static_alloc gives pool memory (8-byte aligned, NULL when
 * the pool is full) for the caller to fill and then commit. Reset frees it
 * all; call it only before the frame's first static draw (nothing drawn
 * from the pool is in flight then). */
void *gfxb_static_alloc(uint32_t bytes);
void gfxb_static_commit(const void *p, uint32_t bytes);
void gfxb_static_reset(void);
/* count indices idx[] into vtx[] (both from the pool), under the current
 * state and transform. */
void gfxb_draw_static(const GfxVtx *vtx, const uint16_t *idx, int count);

void gfxb_init(void);

/* A graphics task: begin before its first draw, end after its last. */
void gfxb_frame_begin(void);
void gfxb_frame_end(void);

/* st must stay as it is until the next gfxb_set_state (the backend keeps
 * the pointer, to send only what the next state changes). */
void gfxb_set_state(const GfxState *st);

/* A triangle list (count is a multiple of 3). */
void gfxb_draw(const GfxVtx *v, int count);

/* Space for n vertices in this task's GPU vertex buffer, for the caller
 * to fill in place (*out): the index of the first, or -1 if the buffer is
 * full. Indices stay valid until the task ends. */
int gfxb_vtx_alloc(int n, GfxVtx **out);
/* A triangle list of gfxb_vtx_alloc indices (count a multiple of 3). */
void gfxb_draw_indexed(const uint16_t *idx, int count);

void gfxb_clear_depth(void);
void gfxb_clear_color(uint8_t r, uint8_t g, uint8_t b);

/* ---- Textures ------------------------------------------------------------
 * Textures are cached by a 64-bit key the interpreter computes from what
 * they were decoded from. Ids are small integers, never 0. */
enum { GFX_WRAP_REPEAT, GFX_WRAP_MIRROR, GFX_WRAP_CLAMP };

/* Forgets every cached texture (they are made again when next needed): a
 * benchmark of decoding. */
void gfxb_tex_forget_all(void);

/* The id of a cached texture, or 0. Marks it used this frame. */
uint16_t gfxb_tex_find(uint64_t key);

/* The key a cached texture's id holds, or 0. */
uint64_t gfxb_tex_key(uint16_t id);
/* The id still holds that key; marks it used this frame. */
bool gfxb_tex_check(uint16_t id, uint64_t key);

/* Uploads w x h RGBA8 texels (0xRRGGBBAA, row-major, first row first) and
 * returns its id. w and h are powers of two from 8 to 1024. */
uint16_t gfxb_tex_create(uint64_t key, const uint32_t *texels, int w, int h,
                         int wrap_s, int wrap_t, bool linear);

/* The same with a mip chain: levels[k] is (w >> k) x (h >> k), n levels,
 * sampled trilinearly. */
uint16_t gfxb_tex_create_mip(uint64_t key, const uint32_t *const *levels, int n, int w, int h,
                             int wrap_s, int wrap_t, bool linear);

#ifdef __cplusplus
}
#endif

#endif /* GFX_BACKEND_H */
