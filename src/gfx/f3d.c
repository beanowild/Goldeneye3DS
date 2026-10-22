/*
 * f3d.c - runs the game's graphics tasks: a Fast3D (+ Rare's TRI4)
 * display-list interpreter standing in for the RSP microcode and the RDP.
 *
 * Geometry is done on the CPU, as the RSP did: vertices are transformed by
 * the modelview and projection matrices, lit, mapped through the N64
 * viewport onto the 3DS screen, and culled; the backend (gfx_backend.h)
 * gets clip-space triangles.
 *
 * Display lists are native-order 32-bit words (data from ROM is swapped
 * when loaded), so every field is read with shifts on w0/w1.
 *
 * Addresses: a word whose top byte is 0x00-0x0F is an N64 segmented
 * address (segment 0 is physical, i.e. the address itself); anything
 * higher is a real pointer. Game memory lives in linear memory
 * (0x14000000+ / 0x30000000+) and the executable at 0x00100000+, so the
 * two never collide (see os_cpu.c).
 *
 * Textures and TMEM are in f3d_tex.c, the colour combiner (compiled to GPU
 * texture-environment stages) in f3d_cc.c. The pipeline state for the
 * GPU is rebuilt only when a command has changed something it depends on
 * (R.dirty). Fill rectangles still evaluate the combiner on the CPU.
 *
 * Fog: the RSP's per-vertex factor goes in shade alpha (G_FOG), and the
 * blender's FOG-by-shade-alpha mix is one more TEV stage (current_state).
 *
 * Not done yet: LOD/mipmaps, the rest of the blender beyond alpha blending.
 */
#include <ultra64.h>
#include <PR/gbi.h>
#include <gbi_extension.h>

#include "f3d_int.h"
#include "pal/pal_sys.h"
#include "ultra/os_sp.h"

#define OP(c)            ((u8)(c))
#define MAX_DL_DEPTH     18
#define MAX_COMMANDS     (2 * 1024 * 1024)   /* per task: a runaway list is a bug */
#define MTX_STACK        16
#define NUM_VTX          16
#define BATCH_INDICES    (3 * 1024)

/* 3DS top screen */
#define SCREEN_W 400.0f
#define SCREEN_H 240.0f
extern int g_gfx_debug_enabled, g_gfx_debug_mode;   /* developer switches, below */

#define NOISE_SIZE 64      /* G_AC_DITHER's noise texture, texels a side */
/* The GPU's near plane at most this far, its far plane this much further
 * than the game's (see set_depth_map). */
#define FAR_SCALE 8.0f
/* Stereo 3D (gfx_backend.h GFX_STEREO_WORLD): the eye distance drawn at the
 * screen's depth, in game units, or this many near planes if more. The
 * pop-out cap's kink (half of it, gfx_c3d.c STEREO_POP 1) is where
 * T-junctions can crack: 10 units out in play, inside the near plane for
 * cutscenes (30) and the logos (100).
 * Projections starting at STEREO_FLAT_NEAR or beyond are drawn flat: the
 * watch (1000), which must stay under its text. */
#define STEREO_CONV      20.0f
#define STEREO_CONV_NEAR 1.5f
#define STEREO_FLAT_NEAR 500.0f

/* A loaded vertex: written to the GPU's buffer as it loads (o, idx), and
 * its clip x, y, w kept here for culling. */
typedef struct {
    float x, y, w;
    int idx;                /* index in the GPU's buffer, or -1 */
    const GfxVtx *o;
} Vert;

typedef struct { float m[4][4]; } Mat;

static struct {
    u32 seg[16];

    Mat proj;
    int zmap;               /* depth from w: z = za * w + zb (set_depth_map) */
    float za, zb;
    float stereo_c;         /* with zmap: GfxState.stereo_c */
    Mat mv[MTX_STACK];
    int mv_top;
    Mat mvp;
    int mvp_dirty;

    Vert vtx[NUM_VTX];

    u32 geom;               /* geometry mode */
    u32 om_h, om_l;         /* other modes */
    u32 cc0, cc1;           /* combine mode words */
    float prim[4], env[4], fill[4];
    float tex_s, tex_t;     /* G_TEXTURE scale */
    int tex_tile;           /* G_TEXTURE tile */
    int tex_levels;         /* G_TEXTURE mip levels beyond the first */
    float wide;             /* stretch to the full screen: 0 = fit, 1 = full */
    float anchor;           /* HUD anchor column, or < 0 (see f3d_gfx_anchor) */
    float near_scale;       /* see f3d_gfx_nearclip */
    int menu;               /* see f3d_gfx_menu */
    u32 menu_pieces;        /* this frame's so far */
    float menu_depth;       /* the last piece's, fraction of infinity */
    int menu_last_piece;    /* the last draw was a piece, not a rectangle */
    u32 prim_c, env_c;      /* as 0xRRGGBBAA, for the combiner */
    u8 prim_lod;            /* primitive LOD fraction */
    u8 blend_a;             /* blend colour alpha: the alpha-compare threshold */
    u32 fog_c;              /* G_SETFOGCOLOR, 0xRRGGBBAA */
    float fog_m, fog_o;     /* G_MW_FOG: shade alpha = z/w * fog_m + fog_o */

    int dirty;              /* state needs rebuilding from the above */
    int tex_dirty;          /* only its textures do (tiles or TMEM changed) */
    int texmask, mip;       /* from the last rebuild: units read, mipmapped */
    int state_live;         /* R.state is the backend's current state */
    int noise;              /* state compares alpha with noise on unit 1 */
    float noise_off[2];     /* noise texture offset, new every task */
    GfxState state;
    u32 state_ver;          /* bumped whenever state is remade */
    TexMap tmap[2];         /* tex_tile and tex_tile + 1 */

    int num_lights;
    float light_col[8][3];
    float light_dir[8][3];  /* as given (world/eye space) */
    float lookat[2][3];     /* G_TEXTURE_GEN's x and y directions, likewise */

    /* viewport, N64 pixels */
    float vp_sx, vp_sy, vp_tx, vp_ty;
    /* N64 pixels -> 3DS clip space: x' = ax*x + bx, y' = ay*y + by */
    float fb_w, fb_h;

    u32 cimg, zimg;
    u16 scissor[4];         /* GfxState's: top-screen pixels, all 0 = none */

    GfxState batch_state;
    u16 batch[BATCH_INDICES];   /* triangles, as GPU vertex indices */
    int batch_n;
} R;

static int s_inited;

/* Per-task counts, for the heartbeat log (f3d_stats). */
static struct { u32 cmds, vtx, tris, culled, fills, dls, rebuilds, changes, draws, texchanges; } s_stats, s_last;
static u64 s_times[4] = { 0, 0, 0, ~0ull };   /* see f3d_take_times */

/* ---- Helpers ---------------------------------------------------------- */

static const Gfx *s_cur_cmd;   /* command being run, for bad-address reports */

/* Game data lives in the executable (0x00100000+) or linear memory. */
static int addr_ok(u32 a)
{
    return (a >= 0x00100000u && a < 0x04000000u) || (a >= 0x14000000u && a < 0x1C000000u) ||
           (a >= 0x30000000u && a < 0x38000000u);
}

void *f3d_resolve(u32 addr)
{
    u32 s = addr >> 24, a = s < 16 ? R.seg[s] + (addr & 0x00FFFFFF) : addr;
    if (!addr_ok(a)) {
        static int reported;
        if (reported++ < 8)
            pal_log("f3d: bad address %08lX (seg %lu = %08lX) in cmd %08lX %08lX at %p",
                    (unsigned long)addr, (unsigned long)s, (unsigned long)(s < 16 ? R.seg[s] : 0),
                    (unsigned long)(s_cur_cmd ? s_cur_cmd->words.w0 : 0),
                    (unsigned long)(s_cur_cmd ? s_cur_cmd->words.w1 : 0), (const void *)s_cur_cmd);
        return NULL;
    }
    return (void *)(uintptr_t)a;
}

#define resolve f3d_resolve

static void mat_mul(Mat *out, const Mat *a, const Mat *b)   /* out = a * b */
{
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] +
                        a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *out = r;
}

/* An N64 Mtx: 16 s16 integer parts then 16 u16 fractions, two per word,
 * the first of each pair in the high half. */
static void mat_from_n64(Mat *out, const u32 *w)
{
    float *o = &out->m[0][0];
    for (int k = 0; k < 8; k++) {   /* a word holds two elements' halves */
        u32 wi = w[k], wf = w[8 + k];
        /* each element as one s15.16 value, converted once */
        o[k * 2] = (float)(s32)((wi & 0xFFFF0000u) | (wf >> 16)) * (1.0f / 65536);
        o[k * 2 + 1] = (float)(s32)((wi << 16) | (wf & 0xFFFF)) * (1.0f / 65536);
    }
}

static void unpack_rgba(u32 c, float out[4])
{
    out[0] = ((c >> 24) & 0xFF) / 255.0f;
    out[1] = ((c >> 16) & 0xFF) / 255.0f;
    out[2] = ((c >> 8) & 0xFF) / 255.0f;
    out[3] = (c & 0xFF) / 255.0f;
}

static u8 to_u8(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* to_u8 without branches, for the vertex loop: v * 255 rounded, saturated. */
static inline u32 sat_u8(float v)
{
    s32 i = (s32)(v * 255.0f + 0.5f);
    u32 r;
    __asm__("usat %0, #8, %1" : "=r"(r) : "r"(i));
    return r;
}

/* Widescreen: the game renders gameplay anamorphically (a wider aspect
 * into its 320 x 240 framebuffer, as its own 16:9 option does for a TV),
 * and the frame is stretched over the whole top screen. Otherwise (menus)
 * it is scaled to fit, with bars at the sides. The amount goes from 0
 * (fit) to 1 (full screen); the game eases it while zooming into the
 * watch. It travels in the display list (a tagged G_NOOP that lv.c
 * writes at the start of the frame), so each frame carries its own value:
 * a global set by the game thread raced the task running on another core,
 * and frames flickered between the two sizes. Tasks without it fit. */
#define WIDESCREEN_TAG 0x3D5754u   /* G_NOOP's low 24 bits: "=WT" */

Gfx *f3d_gfx_widescreen(Gfx *g, float amount)
{
    union { float f; u32 u; } v = { amount };
    g->words.w0 = ((u32)G_NOOP << 24) | WIDESCREEN_TAG;
    g->words.w1 = v.u;
    return g + 1;
}

/* HUD anchor: while set, draws keep the N64's proportions (x scaled like
 * y) around framebuffer column x, which itself lands where the stretched
 * frame puts it. The game sets it around HUD elements (text, the ammo
 * counter, the health bar) so they aren't stretched but keep their place
 * relative to the screen edges. A negative x clears it. */
#define ANCHOR_TAG 0x3D414Eu       /* "=AN" */

Gfx *f3d_gfx_anchor(Gfx *g, float x)
{
    union { float f; u32 u; } v = { x };
    g->words.w0 = ((u32)G_NOOP << 24) | ANCHOR_TAG;
    g->words.w1 = v.u;
    return g + 1;
}

/* Near plane: the GPU's is the game's times this (from the tag on), at
 * least NEAR_MIN units, or the game's if that is nearer. lv.c sets it for
 * the first-person camera: walls beside Bond were cut away by the near
 * plane, more often than on N64 as the 5:3 view is wider. Cameras that
 * pass through walls on purpose (intro, swirl, cutscenes) keep the game's. */
#define NEARCLIP_TAG 0x3D4E43u     /* "=NC" */
#define NEAR_MIN 1.0f

Gfx *f3d_gfx_nearclip(Gfx *g, float scale)
{
    union { float f; u32 u; } v = { scale };
    g->words.w0 = ((u32)G_NOOP << 24) | NEARCLIP_TAG;
    g->words.w1 = v.u;
    return g + 1;
}

/* N64 framebuffer pixels -> top screen pixels: sx = px * kx + ox. */
static void fb_map(float *kx, float *ky, float *ox, float *oy)
{
    float fit = __builtin_fminf(SCREEN_W / R.fb_w, SCREEN_H / R.fb_h);
    *kx = fit + (SCREEN_W / R.fb_w - fit) * R.wide;
    *ky = fit + (SCREEN_H / R.fb_h - fit) * R.wide;
    *ox = (SCREEN_W - R.fb_w * *kx) * 0.5f;
    *oy = (SCREEN_H - R.fb_h * *ky) * 0.5f;
    if (R.anchor >= 0.0f) {
        /* Whole pixels: HUD text is bilinear filtered, and a glyph placed
         * a fraction off blends its edge column with the transparent
         * texel beside it (the timer's leading 0 lost its left stroke). */
        *ox = __builtin_roundf(*ox + R.anchor * (*kx - *ky));
        *kx = *ky;
    }
}

static void fb_to_screen(float px, float py, float *sx, float *sy)
{
    float kx, ky, ox, oy;
    fb_map(&kx, &ky, &ox, &oy);
    *sx = ox + px * kx;
    *sy = oy + py * ky;
}

/* ---- Batching ---------------------------------------------------------- */

static void flush(void)
{
    if (R.batch_n) {
        s_stats.draws++;
        gfxb_draw_indexed(R.batch, R.batch_n);
        R.batch_n = 0;
    }
}

/* A vertex into the task's GPU buffer: its index, or -1 if it's full. */
static int put_vertex(const GfxVtx *v)
{
    GfxVtx *o;
    int i = gfxb_vtx_alloc(1, &o);
    if (i >= 0)
        *o = *v;
    return i;
}

/* States are built from zeroed memory, padding included, and word aligned. */
static int state_differs(const GfxState *a, const GfxState *b)
{
    _Static_assert(sizeof(GfxState) % 4 == 0, "GfxState is compared by words");
    const u32 *x = (const u32 *)a, *y = (const u32 *)b;
    for (unsigned i = 0; i < sizeof(GfxState) / 4; i++)
        if (x[i] != y[i])
            return 1;
    return 0;
}

/* Menu frames (f3d_gfx_menu): the folder screens are flat cards at one
 * distance, painted back to front, so in stereo each new piece of
 * document (a perspective draw) sits a step nearer than the one before,
 * from MENU_BACK of the way to infinity to MENU_FRONT, the steps sized by
 * the last menu frame's count; rectangles (text, icons) lie just above
 * the piece drawn before them. */
#define MENU_TAG 0x3D4D4Cu         /* "=ML" */
#define MENU_BACK  0.9f
#define MENU_FRONT 0.25f
#define MENU_LIFT  0.08f
#define MENU_STRENGTH 0.5f   /* of the levels' depth: the user found more nauseating */

Gfx *f3d_gfx_menu(Gfx *g)
{
    g->words.w0 = ((u32)G_NOOP << 24) | MENU_TAG;
    g->words.w1 = 0;
    return g + 1;
}

/* Developer timer (-gfxdbg): bg.c brackets each room's display list with
 * this tag; the interpreting time between goes to PAL_PROF_ROOM_DL. */
#define PROF_TAG 0x3D5052u         /* "=PR" */

Gfx *f3d_gfx_prof(Gfx *g, int on)
{
    g->words.w0 = ((u32)G_NOOP << 24) | PROF_TAG;
    g->words.w1 = (u32)on;
    return g + 1;
}

static u32 s_menu_pieces;   /* pieces in the last menu frame */

static void menu_layer(GfxState *t)
{
    int piece = t->stereo == GFX_STEREO_WORLD;
    t->stereo = GFX_STEREO_LAYER;
    t->stereo_c = R.batch_state.stereo_c;   /* to compare the rest */
    if (!state_differs(t, &R.batch_state))
        return;
    if (piece && R.menu_last_piece) {
        /* the same piece again with another texture: images drawn a strip
         * at a time (Bond's photo) would otherwise come apart in steps,
         * black showing between the strips at some slider positions */
        GfxState same = *t;
        __builtin_memcpy(same.tex, R.batch_state.tex, sizeof same.tex);
        __builtin_memcpy(same.tex_map, R.batch_state.tex_map, sizeof same.tex_map);
        if (!state_differs(&same, &R.batch_state))
            return;
    }
    R.menu_last_piece = piece;
    if (piece) {
        R.menu_pieces++;
        float step = (MENU_BACK - MENU_FRONT) / (float)(s_menu_pieces > 1 ? s_menu_pieces - 1 : 1);
        R.menu_depth = __builtin_fmaxf(MENU_BACK - step * (float)(R.menu_pieces - 1), MENU_FRONT);
        t->stereo_c = R.menu_depth * MENU_STRENGTH;
    } else {
        t->stereo_c = __builtin_fmaxf(R.menu_depth - MENU_LIFT, 0.0f) * MENU_STRENGTH;
    }
}

static void use_state(const GfxState *st)
{
    R.state_live = 0;
    GfxState layered;
    if (R.menu) {
        layered = *st;
        menu_layer(&layered);
        st = &layered;
    }
    if (state_differs(st, &R.batch_state)) {
        flush();
        s_stats.changes++;
        R.batch_state = *st;
        gfxb_set_state(st);
    }
}

/* With no combiner stages the vertex colour is drawn as is; stages added
 * after the combiner need it as a stage of its own. */
static void vertex_colour_stage(GfxState *n)
{
    if (n->num_stages == 0) {
        GfxTevStage *t = &n->stages[n->num_stages++];
        t->rgb.func = t->a.func = GFX_FN_REPLACE;
        t->rgb.src[0] = t->a.src[0] = GFX_SRC_PRIMARY;
        t->rgb.src[1] = t->rgb.src[2] = t->a.src[1] = t->a.src[2] = GFX_SRC_PREVIOUS;
    }
}

static u32 noise_rand(void)
{
    static u32 x = 0x2545F491;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* White texels with random alpha below 255, so alpha 255 always passes. */
static u16 noise_tex(void)
{
    static const u64 key = 0x4E4F495345000000ull;   /* "NOISE" */
    u16 id = gfxb_tex_find(key);
    if (!id) {
        static uint32_t texels[NOISE_SIZE * NOISE_SIZE];
        for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++)
            texels[i] = 0xFFFFFF00u | (noise_rand() % 255);
        id = gfxb_tex_create(key, texels, NOISE_SIZE, NOISE_SIZE, GFX_WRAP_REPEAT, GFX_WRAP_REPEAT, false);
    }
    return id;
}

/* The state's textures and their coordinate mappings, for the tiles and
 * TMEM as they are now. */
static void state_textures_impl(GfxState *n);
static void state_textures(GfxState *n)
{
    u64 t0 = g_gfx_debug_enabled ? pal_ticks() : 0;   /* developer timer */
    state_textures_impl(n);
    if (g_gfx_debug_enabled)
        pal_prof_add(PAL_PROF_F3D_STATETEX, pal_ticks() - t0);
}

static void state_textures_impl(GfxState *n)
{
    int texmask = R.texmask;
    n->tex[0] = n->tex[1] = 0;
    __builtin_memset(n->tex_map, 0, sizeof n->tex_map);
    u16 mip_id = R.mip && (texmask & 1) ? tex_get_mip(R.tex_tile, R.tex_levels, R.om_h, &R.tmap[0]) : 0;
    if (mip_id) {
        n->tex[0] = mip_id;
        if (texmask & 2) {
            n->tex[1] = mip_id;
            R.tmap[1] = R.tmap[0];
        }
    } else {
        for (int u = 0; u < 2; u++)
            if (texmask & (1 << u))
                n->tex[u] = tex_get((R.tex_tile + u) & 7, R.om_h, &R.tmap[u]);
    }
    /* u = (s * scale_s - off_s) * inv_w, as a scale and an offset */
    for (int u = 0; u < 2; u++) {
        if (texmask & (1 << u)) {
            const TexMap *m = &R.tmap[u];
            n->tex_map[u][0] = m->scale_s * m->inv_w;
            n->tex_map[u][1] = m->scale_t * m->inv_h;
            n->tex_map[u][2] = -m->off_s * m->inv_w;
            n->tex_map[u][3] = -m->off_t * m->inv_h;
        }
    }
    if (R.noise) {
        /* a noise texel per top-screen pixel, from a new offset each task */
        n->tex[1] = noise_tex();
        n->tex_map[1][0] = n->tex_map[1][1] = 0;
        n->tex_map[1][2] = R.noise_off[0];
        n->tex_map[1][3] = R.noise_off[1];
        n->noise = true;
        n->noise_scale[0] = SCREEN_W / 2 / NOISE_SIZE;
        n->noise_scale[1] = SCREEN_H / 2 / NOISE_SIZE;
    }
}

/* The state for the current modes, rebuilt if they changed. */
static const GfxState *current_state(void)
{
    if (R.dirty || R.tex_dirty) {
        R.state_ver++;
        if (!R.dirty) {
            s_stats.texchanges++;
            state_textures(&R.state);
            R.tex_dirty = 0;
            return &R.state;
        }
        s_stats.rebuilds++;
        GfxState *n = &R.state;
        u32 rm = R.om_l, cycle = (R.om_h >> G_MDSFT_CYCLETYPE) & 3;
        int texmask, mip = 0;

        __builtin_memset(n, 0, sizeof *n);
        __builtin_memcpy(n->scissor, R.scissor, sizeof R.scissor);
        n->shade = (R.geom & G_SHADE) != 0;
        if (R.zmap && R.stereo_c > 0) {   /* perspective */
            n->stereo = GFX_STEREO_WORLD;
            n->stereo_c = R.stereo_c;
        }
        n->depth_test = (R.geom & G_ZBUFFER) && (rm & Z_CMP);
        n->depth_write = (R.geom & G_ZBUFFER) && (rm & Z_UPD);
        /* Decals, and translucent surfaces (ZMODE_XLU) too: the RDP passes
         * a depth within the surface's own dz, so a translucent layer over
         * a floor (Cradle's walkway) always shows; tested as strictly
         * nearer it fought the floor on hardware, pixel by pixel. */
        n->depth_equal = (rm & ZMODE_DEC) == ZMODE_DEC || (rm & ZMODE_DEC) == ZMODE_XLU;
        n->blend = (rm & FORCE_BL) != 0 && cycle < 2;
        if ((rm & 3) == G_AC_THRESHOLD) {
            n->alpha_test = true;
            n->alpha_ref = R.blend_a ? R.blend_a : 1;
        } else if (rm & CVG_X_ALPHA) {
            n->alpha_test = true;
            n->alpha_ref = 0x80;
        }

        if (cycle == (G_CYC_COPY >> G_MDSFT_CYCLETYPE)) {   /* texels as they are */
            GfxTevStage *t = &n->stages[0];
            t->rgb.func = t->a.func = GFX_FN_REPLACE;
            t->rgb.src[0] = t->a.src[0] = GFX_SRC_TEX0;
            t->rgb.src[1] = t->rgb.src[2] = t->a.src[1] = t->a.src[2] = GFX_SRC_PREVIOUS;
            n->num_stages = 1;
            texmask = 1;
        } else {
            /* LOD_FRACTION: with G_TD_DETAIL, tile 0 is a magnified
             * detail texture over the base in tile 1, and the fraction is
             * near 1 except right up close (Surface's tree line drew as
             * the detail speckle alone). Otherwise 0: the largest mip. */
            u8 lod_frac = ((R.om_h & G_TL_LOD) && (R.om_h & (3 << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL) ? 255 : 0;
            /* Mipmaps (G_TL_LOD, clamp): tiles tex_tile.. are the levels
             * and the combiner lerps a pair by LOD_FRACTION. The GPU
             * filters a mip chain trilinearly instead, so here the
             * combiner sees a fraction of 0 (the first tile) and that
             * tile becomes the whole chain (below). */
            mip = (R.om_h & G_TL_LOD) && (R.om_h & (3 << G_MDSFT_TEXTDETAIL)) == G_TD_CLAMP &&
                  R.tex_levels > 0 && cycle == (G_CYC_2CYCLE >> G_MDSFT_CYCLETYPE);
            if (g_gfx_debug_mode == 6)   /* developer: no mipmaps */
                mip = 0;
            CcInput in = { R.cc0, R.cc1, cycle == (G_CYC_2CYCLE >> G_MDSFT_CYCLETYPE),
                           R.prim_c, R.env_c, R.prim_lod, lod_frac };
            texmask = cc_compile(&in, n);
        }
        R.noise = 0;
        /* blender cycle 1 = FOG * alpha + IN * (1 - alpha), alpha being the
         * shade alpha (the RSP's per-vertex fog: level geometry) or the fog
         * colour's own (G_RM_FOG_PRIM_A: models, an amount the game works
         * out per object) */
        int fog_a = (rm >> 26) & 3;
        if ((rm >> 30) == G_BL_CLR_FOG && cycle < 2 && n->num_stages < GFX_MAX_STAGES
            && ((fog_a == G_BL_A_SHADE && (R.geom & G_FOG)) || fog_a == G_BL_A_FOG)) {
            vertex_colour_stage(n);
            if (n->num_stages < GFX_MAX_STAGES) {
                GfxTevStage *t = &n->stages[n->num_stages++];
                t->rgb.func = GFX_FN_INTERPOLATE;
                t->rgb.src[0] = GFX_SRC_CONSTANT;
                t->rgb.src[1] = GFX_SRC_PREVIOUS;
                t->rgb.src[2] = fog_a == G_BL_A_FOG ? GFX_SRC_CONSTANT : GFX_SRC_PRIMARY;
                t->rgb.op[2] = GFX_OP_ALPHA;
                t->a.func = GFX_FN_REPLACE;
                t->a.src[0] = t->a.src[1] = t->a.src[2] = GFX_SRC_PREVIOUS;
                t->constant = R.fog_c;
            }
        }
        /* G_RM_AA_PCL_SURF (AA without forced blending, IN * A + MEM * (1 - A),
         * coverage not from alpha): the RDP keeps a pixel's alpha only as
         * noisy coverage, which is how the watch draws its static. Here a
         * pixel is drawn if its alpha beats a random value: the noise is a
         * texture on unit 1 mapped to screen pixels, and alpha - noise goes
         * to the alpha test. */
        int pcl = (rm & AA_EN) && !(rm & (FORCE_BL | ALPHA_CVG_SEL | CVG_X_ALPHA)) &&
                  (rm >> 30) == G_BL_CLR_IN && ((rm >> 26) & 3) == G_BL_A_IN &&
                  ((rm >> 22) & 3) == G_BL_CLR_MEM && ((rm >> 18) & 3) == G_BL_1MA;
        if (pcl && !(texmask & 2) && cycle < 2) {
            vertex_colour_stage(n);
            if (n->num_stages < GFX_MAX_STAGES) {
                GfxTevStage *t = &n->stages[n->num_stages++];
                t->rgb.func = GFX_FN_REPLACE;
                t->rgb.src[0] = t->rgb.src[1] = t->rgb.src[2] = GFX_SRC_PREVIOUS;
                t->a.func = GFX_FN_SUBTRACT;
                t->a.src[0] = GFX_SRC_PREVIOUS;
                t->a.src[1] = GFX_SRC_TEX1;
                t->a.src[2] = GFX_SRC_PREVIOUS;
                n->alpha_test = true;
                n->alpha_ref = 1;
                R.noise = 1;
            }
        }
        R.texmask = texmask;
        R.mip = mip;
        state_textures(n);
        R.dirty = 0;
        R.tex_dirty = 0;
    }
    return &R.state;
}

/* ---- Colour combiner (CPU, per vertex, texels as white) ---------------- */

static void cc_color_input(int code, int kind, const float shade[4], const float comb[4], float out[3])
{
    /* kind: 0 = a, 1 = b, 2 = c, 3 = d (the codes differ per slot) */
    const float *src = NULL;
    static const float one[4] = { 1, 1, 1, 1 }, zero[4] = { 0, 0, 0, 0 };
    switch (code) {
    case 0: src = comb; break;
    case 1: case 2: src = one; break;      /* TEXEL0/1 */
    case 3: src = R.prim; break;
    case 4: src = shade; break;
    case 5: src = R.env; break;
    case 6:
        src = (kind == 0 || kind == 3) ? one : zero;   /* ONE (a, d); CENTER/SCALE ~ 0 */
        break;
    default:
        if (kind == 2) {
            float a;
            switch (code) {
            case 7: a = comb[3]; break;
            case 8: case 9: a = 1.0f; break;
            case 10: a = R.prim[3]; break;
            case 11: a = shade[3]; break;
            case 12: a = R.env[3]; break;
            default: a = 0.0f; break;
            }
            out[0] = out[1] = out[2] = a;
            return;
        }
        src = (kind == 3 && code == 6) ? one : zero;
        break;
    }
    out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
}

static float cc_alpha_input(int code, int kind, const float shade[4], const float comb[4])
{
    switch (code) {
    case 0: return kind == 2 ? 0.0f : comb[3];   /* c: LOD_FRAC */
    case 1: case 2: return 1.0f;
    case 3: return R.prim[3];
    case 4: return shade[3];
    case 5: return R.env[3];
    case 6: return kind == 2 ? 0.0f : 1.0f;      /* c: PRIM_LOD_FRAC */
    default: return 0.0f;
    }
}

static void cc_cycle(int cycle, const float shade[4], float comb[4])
{
    u32 w0 = R.cc0, w1 = R.cc1;
    int a, b, c, d, aa, ab, ac, ad;
    if (cycle == 0) {
        a = (w0 >> 20) & 0xF; c = (w0 >> 15) & 0x1F; aa = (w0 >> 12) & 7; ac = (w0 >> 9) & 7;
        b = (w1 >> 28) & 0xF; d = (w1 >> 15) & 7; ab = (w1 >> 12) & 7; ad = (w1 >> 9) & 7;
    } else {
        a = (w0 >> 5) & 0xF; c = w0 & 0x1F; aa = (w1 >> 21) & 7; ac = (w1 >> 18) & 7;
        b = (w1 >> 24) & 0xF; d = (w1 >> 6) & 7; ab = (w1 >> 3) & 7; ad = w1 & 7;
    }
    float A[3], B[3], C[3], D[3], res[4];
    cc_color_input(a, 0, shade, comb, A);
    cc_color_input(b, 1, shade, comb, B);
    cc_color_input(c, 2, shade, comb, C);
    cc_color_input(d, 3, shade, comb, D);
    for (int i = 0; i < 3; i++)
        res[i] = (A[i] - B[i]) * C[i] + D[i];
    res[3] = (cc_alpha_input(aa, 0, shade, comb) - cc_alpha_input(ab, 1, shade, comb)) *
                 cc_alpha_input(ac, 2, shade, comb) + cc_alpha_input(ad, 3, shade, comb);
    for (int i = 0; i < 4; i++)
        comb[i] = res[i];
}

static void combine(const float shade[4], float out[4])
{
    float comb[4] = { 0, 0, 0, 0 };
    u32 cycle = (R.om_h >> G_MDSFT_CYCLETYPE) & 3;
    cc_cycle(0, shade, comb);
    if (cycle == 1)                         /* G_CYC_2CYCLE */
        cc_cycle(1, shade, comb);
    for (int i = 0; i < 4; i++)
        out[i] = comb[i];
}

/* ---- Vertices and triangles -------------------------------------------- */

/* The PICA clips at its near plane, z = -w, as the N64's microcode does,
 * and the game's own near plane is kept: intro cameras rely on it (the
 * cinema environments use 30, and Frigate's first camera sits inside a
 * wall it cuts away; capping it at 10 showed the wall, though it kept
 * Dam's intro truck from being cut open as on N64). For a perspective
 * projection depth is remapped from w so that the far plane goes
 * FAR_SCALE times further out: the game culls whole rooms beyond it, but
 * a room only partly beyond was
 * cut off by the GPU (Statue's tree line lost its tops against the sky),
 * and fog is at full strength there anyway.
 *
 * m is the whole modelview * projection (some lists put the view in the
 * projection). With a perspective projection P and an affine view, clip
 * z and w both follow eye z, so cz = alpha * cw + beta: column 2 of m is
 * alpha times column 3, plus beta in the translation row. */
static void set_depth_map_from(const Mat *p)
{
    R.zmap = 0;
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (__builtin_fabsf(p->m[i][3]) > __builtin_fabsf(p->m[k][3]))
            k = i;
    if (__builtin_fabsf(p->m[k][3]) < 1e-9f)
        return;                                  /* orthographic */
    float alpha = p->m[k][2] / p->m[k][3];
    float beta = p->m[3][2] - alpha * p->m[3][3];
    float wn = beta / (-1.0f - alpha), wf = beta / (1.0f - alpha);   /* z/w = -1, 1 */
    if (!(wn > 0 && wf > wn))
        return;
    float n2 = wn;
    if (R.near_scale < 1.0f)
        n2 = __builtin_fmaxf(wn * R.near_scale, __builtin_fminf(wn, NEAR_MIN));
    float f2 = wf * FAR_SCALE;
    R.zb = -1.0f / (1.0f / n2 - 1.0f / f2);   /* z/w = -1 at w = n2, 0 at w = f2 */
    R.za = -R.zb / f2;
    R.zmap = 1;
    R.stereo_c = wn >= STEREO_FLAT_NEAR ? -1.0f : __builtin_fmaxf(STEREO_CONV, wn * STEREO_CONV_NEAR);
}

static void set_depth_map(const Mat *p)
{
    int zmap = R.zmap;
    float stereo_c = R.stereo_c;
    set_depth_map_from(p);
    if (R.zmap != zmap || (R.zmap && R.stereo_c != stereo_c))
        R.dirty = 1;   /* the state's stereo mode */
}

static void update_mvp(void)
{
    if (R.mvp_dirty) {
        mat_mul(&R.mvp, &R.mv[R.mv_top], &R.proj);
        set_depth_map(&R.mvp);
        R.mvp_dirty = 0;
    }
}

/* Model space -> the backend's clip space (rows px, py, pz, pw), the game's
 * own clip z (mz, for fog) and the fog factor's scale and offset: the
 * current matrices, viewport and screen mapping. */
typedef struct {
    float px[4], py[4], pz[4], pw[4], mz[4];
    float fog_m, fog_o;
} Xform;

static void xform_setup(Xform *xf)
{
    const Mat *m = &R.mvp;
    float kx, ky, ox, oy;
    fb_map(&kx, &ky, &ox, &oy);
    /* NDC -> N64 pixels -> 3DS pixels -> 3DS NDC, folded into one affine map */
    float ax = R.vp_sx * kx / (SCREEN_W / 2), bx = (ox + R.vp_tx * kx) / (SCREEN_W / 2) - 1.0f;
    float ay = R.vp_sy * ky / (SCREEN_H / 2), by = 1.0f - (oy + R.vp_ty * ky) / (SCREEN_H / 2);
    /* Folding the viewport into the matrix: x = ax * cx + bx * cw etc. */
    for (int k = 0; k < 4; k++) {
        float mx = m->m[k][0], my = m->m[k][1], mz = m->m[k][2], mw = m->m[k][3];
        xf->px[k] = ax * mx + bx * mw;
        xf->py[k] = ay * my + by * mw;
        xf->pz[k] = R.zmap ? R.za * mw : 0.5f * mz - 0.5f * mw;   /* see set_depth_map */
        xf->pw[k] = mw;
        xf->mz[k] = mz;
    }
    if (R.zmap)
        xf->pz[3] += R.zb;
    xf->fog_m = R.fog_m * (1.0f / 255);
    xf->fog_o = R.fog_o * (1.0f / 255);
}

static void load_vertices(const Vtx *src, int v0, int n)
{
    update_mvp();
    const Mat *mv = &R.mv[R.mv_top];

    /* Light directions into model space (transpose of the modelview's
     * rotation), normalised. */
    float ldir[8 + 2][3];   /* lights, then the two lookat directions */
    int texgen = (R.geom & (G_LIGHTING | G_TEXTURE_GEN)) == (G_LIGHTING | G_TEXTURE_GEN);
    if (R.geom & G_LIGHTING) {
        for (int l = 0; l < R.num_lights + 2; l++) {
            const float *src = l < R.num_lights ? R.light_dir[l] : R.lookat[l - R.num_lights];
            float d[3];
            for (int k = 0; k < 3; k++)
                d[k] = mv->m[k][0] * src[0] + mv->m[k][1] * src[1] + mv->m[k][2] * src[2];
            float len = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 0) { d[0] /= len; d[1] /= len; d[2] /= len; }
            float *o = l < R.num_lights ? ldir[l] : ldir[8 + l - R.num_lights];
            o[0] = d[0]; o[1] = d[1]; o[2] = d[2];
        }
    }

    /* Per-load constants, so the loop has no divides but one per vertex. */
    Xform xf;
    xform_setup(&xf);
    const float *px = xf.px, *py = xf.py, *pz = xf.pz, *pw = xf.pw;
    const float ks = R.tex_s * (1.0f / 32), kt = R.tex_t * (1.0f / 32);
    const int lit = (R.geom & G_LIGHTING) != 0, fog = (R.geom & G_FOG) != 0;
    const float fog_m = xf.fog_m, fog_o = xf.fog_o;
    const float mz0 = xf.mz[0], mz1 = xf.mz[1], mz2 = xf.mz[2], mz3 = xf.mz[3];
    const int nl = R.num_lights;
    float lcol[8][3], amb[3];
    if (lit) {
        __builtin_memcpy(lcol, R.light_col, sizeof lcol);
        __builtin_memcpy(amb, R.light_col[nl], sizeof amb);
    }
    const float env_s = R.tex_s * 512.0f, env_t = R.tex_t * 512.0f;
    if (v0 + n > NUM_VTX)
        n = NUM_VTX - v0;
    if (n <= 0)
        return;
    GfxVtx *out;
    int base = gfxb_vtx_alloc(n, &out);
    if (base < 0) {
        for (int i = 0; i < n; i++)
            R.vtx[v0 + i].idx = -1;
        return;
    }

    /* Culling needs no screen positions (triangle() uses the sign of the
     * homogeneous determinant), so there is no divide here but for fog. */
    for (int i = 0; i < n; i++) {
        const Vtx_t *s = &src[i].v;
        Vert *d = &R.vtx[v0 + i];
        GfxVtx *o = &out[i];
        d->idx = base + i;
        d->o = o;
        float x = s->ob[0], y = s->ob[1], z = s->ob[2];
        float cx = x * px[0] + y * px[1] + z * px[2] + px[3];
        float cy = x * py[0] + y * py[1] + z * py[2] + py[3];
        float cw = x * pw[0] + y * pw[1] + z * pw[2] + pw[3];
        o->x = d->x = cx;
        o->y = d->y = cy;
        o->z = x * pz[0] + y * pz[1] + z * pz[2] + pz[3];   /* N64 [-w, w] -> PICA [-w, 0] */
        o->w = d->w = cw;
        o->s = s->tc[0] * ks;
        o->t = s->tc[1] * kt;

        if (lit) {
            const Vtx_tn *nv = &src[i].n;
            float nx = nv->n[0], ny = nv->n[1], nz = nv->n[2];
            float len2 = nx * nx + ny * ny + nz * nz;
            if (len2 > 0) {
                float il = 1.0f / __builtin_sqrtf(len2);
                nx *= il; ny *= il; nz *= il;
            }
            float c0 = amb[0], c1 = amb[1], c2 = amb[2];
            for (int l = 0; l < nl; l++) {
                float dot = nx * ldir[l][0] + ny * ldir[l][1] + nz * ldir[l][2];
                if (dot > 0) {
                    c0 += dot * lcol[l][0];
                    c1 += dot * lcol[l][1];
                    c2 += dot * lcol[l][2];
                }
            }
            o->r = sat_u8(c0); o->g = sat_u8(c1); o->b = sat_u8(c2);
            o->a = nv->a;
            if (texgen) {
                /* environment map: the normal's facing, from 0 to the
                 * G_TEXTURE scale (the RSP's (dot + 1) / 4 in s10.5) */
                float dx = nx * ldir[8][0] + ny * ldir[8][1] + nz * ldir[8][2];
                float dy = nx * ldir[9][0] + ny * ldir[9][1] + nz * ldir[9][2];
                o->s = (dx + 1.0f) * env_s;
                o->t = (dy + 1.0f) * env_t;
            }
        } else {
            __builtin_memcpy(&o->r, s->cn, 4);   /* r, g, b, a */
        }
        if (fog) {
            /* clip z, from the matrix as the game set it */
            float cz = x * mz0 + y * mz1 + z * mz2 + mz3;
            o->a = cw > 0 ? sat_u8(cz / cw * fog_m + fog_o) : 0;
        }
        if (g_gfx_debug_mode == 8) {   /* developer: a colour per matrix */
            u32 h = 2166136261u;
            const float *m = &R.mvp.m[0][0];
            for (int k = 0; k < 16; k++) {
                union { float f; u32 u; } v = { m[k] };
                h = (h ^ v.u) * 16777619u;
            }
            o->r = (u8)(h | 0x40); o->g = (u8)((h >> 8) | 0x40); o->b = (u8)((h >> 16) | 0x40);
        }
    }
}

/* Decals (ZMODE_DEC): the RDP accepts a pixel within the surface's own
 * depth slope (dz) of what is there, so a decal drawn over a surface at a
 * grazing angle still wins. The GPU only has a constant offset
 * (DECAL_BIAS, gfx_c3d.c), so each decal triangle is also pulled towards
 * the eye by its depth change per screen pixel. z/w is -1 at the near
 * plane and 0 at the far plane. */
static void decal_slope_bias(GfxVtx *v)
{
    if (v[0].w <= 0 || v[1].w <= 0 || v[2].w <= 0)
        return;
    float x[3], y[3], d[3];
    for (int k = 0; k < 3; k++) {
        float iw = 1.0f / v[k].w;
        x[k] = v[k].x * iw * (SCREEN_W / 2);
        y[k] = v[k].y * iw * (SCREEN_H / 2);
        d[k] = v[k].z * iw;
    }
    float ax = x[1] - x[0], ay = y[1] - y[0], ad = d[1] - d[0];
    float bx = x[2] - x[0], by = y[2] - y[0], bd = d[2] - d[0];
    float det = ax * by - ay * bx;
    if (det == 0)
        return;
    float ddx = (ad * by - bd * ay) / det, ddy = (bd * ax - ad * bx) / det;
    float bias = __builtin_fabsf(ddx) + __builtin_fabsf(ddy);
    if (bias > 1.0f / 256)
        bias = 1.0f / 256;
    for (int k = 0; k < 3; k++)
        v[k].z -= bias * v[k].w;
}

/* triangle()'s rare paths, out of line so that it stays small. */
static __attribute__((noinline)) void triangle_state(void)
{
    u64 t0 = g_gfx_debug_enabled ? pal_ticks() : 0;   /* developer timer */
    use_state(current_state());
    if (g_gfx_debug_enabled)
        pal_prof_add(PAL_PROF_F3D_STATE, pal_ticks() - t0);
    R.state_live = 1;
}

/* Decals get their own copies of their vertices, biased per triangle. */
static __attribute__((noinline)) int decal_copies(const GfxVtx *a, const GfxVtx *b, const GfxVtx *c, int j[3])
{
    GfxVtx t[3] = { *a, *b, *c };
    decal_slope_bias(t);
    for (int k = 0; k < 3; k++)
        j[k] = put_vertex(&t[k]);
    return j[0] >= 0 && j[1] >= 0 && j[2] >= 0;
}

int g_gfx_debug_enabled, g_gfx_debug_mode;   /* see debug_hidden */

/* Developer switch (-gfxdbg token, Y cycles): hide one kind of draw to
 * find which layer misbehaves. 1 decals and XLU (depth_equal), 2 blended,
 * 3 alpha-tested, 4 two-texture, 5 plain opaque; 6 draws everything but
 * without mipmaps, 7 without back-face culling, 8 a colour per matrix
 * (shaded draws). -gfxmodeN starts in mode N. */

static __attribute__((noinline)) int debug_hidden(const GfxState *st)
{
    switch (g_gfx_debug_mode) {
    case 1: return st->depth_equal;
    case 2: return st->blend;
    case 3: return st->alpha_test;
    case 4: return st->tex[1] != 0;
    case 5: return st->depth_test && !st->depth_equal && !st->blend && !st->alpha_test;
    }
    return 0;
}

static inline __attribute__((always_inline)) void triangle(int i0, int i1, int i2)
{
    const Vert *a = &R.vtx[i0], *b = &R.vtx[i1], *c = &R.vtx[i2];

    u32 cull = R.geom & (G_CULL_FRONT | G_CULL_BACK);
    if (cull && g_gfx_debug_mode != 7) {   /* developer mode 7: no culling */
        /* The determinant of the rows (x, y, w) is the triangle's winding
         * as seen from the eye, whatever the signs of w (with every w > 0
         * it is the screen-space area times wa wb wc). Triangles reaching
         * behind the eye were once left unculled: the underside of
         * Cradle's long walkway, under Bond's feet, fought its top. */
        float area = a->x * (b->y * c->w - b->w * c->y) - a->y * (b->x * c->w - b->w * c->x) +
                     a->w * (b->x * c->y - b->y * c->x);
        if (cull == (G_CULL_FRONT | G_CULL_BACK))
            return;
        /* y up: counter-clockwise (area > 0) is the front */
        if (((cull & G_CULL_BACK) && area < 0) || ((cull & G_CULL_FRONT) && area > 0)) {
            s_stats.culled++;
            return;
        }
    }

    s_stats.tris++;
    if (R.dirty || R.tex_dirty || !R.state_live)
        triangle_state();
    if (__builtin_expect(g_gfx_debug_mode != 0, 0) && debug_hidden(&R.state))
        return;
    if (R.batch_n + 3 > BATCH_INDICES)
        flush();
    int j[3] = { a->idx, b->idx, c->idx };
    if ((j[0] | j[1] | j[2]) < 0)
        return;
    if (R.state.depth_equal && !decal_copies(a->o, b->o, c->o, j))
        return;
    u16 *o = &R.batch[R.batch_n];
    o[0] = (u16)j[0];
    o[1] = (u16)j[1];
    o[2] = (u16)j[2];
    R.batch_n += 3;
}

/* ---- RDP rectangles ------------------------------------------------------ */

/* G_SETSCISSOR: upper left and lower right corners in 10.2 N64 pixels. */
static void set_scissor(u32 w0, u32 w1)
{
    float sx0, sy0, sx1, sy1;
    fb_to_screen(((w0 >> 12) & 0xFFF) / 4.0f, (w0 & 0xFFF) / 4.0f, &sx0, &sy0);
    fb_to_screen(((w1 >> 12) & 0xFFF) / 4.0f, (w1 & 0xFFF) / 4.0f, &sx1, &sy1);
    sx0 = __builtin_floorf(sx0); sy0 = __builtin_floorf(sy0);
    sx1 = __builtin_ceilf(sx1); sy1 = __builtin_ceilf(sy1);
    R.scissor[0] = (u16)__builtin_fmaxf(sx0, 0.0f);
    R.scissor[1] = (u16)__builtin_fmaxf(sy0, 0.0f);
    R.scissor[2] = (u16)__builtin_fminf(__builtin_fmaxf(sx1, 0.0f), SCREEN_W);
    R.scissor[3] = (u16)__builtin_fminf(__builtin_fmaxf(sy1, 0.0f), SCREEN_H);
    if (R.scissor[2] <= R.scissor[0] || R.scissor[3] <= R.scissor[1]) {
        R.scissor[0] = R.scissor[1] = 0;   /* nothing drawn passes; keep it */
        R.scissor[2] = R.scissor[3] = 1;   /* non-empty so it isn't "none" */
    }
    if (__builtin_memcmp(R.scissor, R.state.scissor, sizeof R.scissor) != 0)
        R.dirty = 1;
}

static void screen_quad(float x0, float y0, float x1, float y1, const float col[4], int depth)
{
    float sx0, sy0, sx1, sy1;
    fb_to_screen(x0, y0, &sx0, &sy0);
    fb_to_screen(x1, y1, &sx1, &sy1);
    float nx0 = sx0 / (SCREEN_W / 2) - 1, nx1 = sx1 / (SCREEN_W / 2) - 1;
    float ny0 = 1 - sy0 / (SCREEN_H / 2), ny1 = 1 - sy1 / (SCREEN_H / 2);
    GfxVtx q[6];
    const float xy[6][2] = { {nx0, ny0}, {nx0, ny1}, {nx1, ny1}, {nx0, ny0}, {nx1, ny1}, {nx1, ny0} };
    for (int i = 0; i < 6; i++)
        q[i] = (GfxVtx){ xy[i][0], xy[i][1], depth ? -1.0f : 0.0f, 1.0f, 0, 0,
                         to_u8(col[0]), to_u8(col[1]), to_u8(col[2]), to_u8(col[3]) };
    GfxState st = { .shade = true };
    st.blend = (R.om_l & FORCE_BL) != 0 && ((R.om_h >> G_MDSFT_CYCLETYPE) & 3) < 2;
    __builtin_memcpy(st.scissor, R.scissor, sizeof R.scissor);
    use_state(&st);
    flush();
    gfxb_draw(q, 6);
}

/* G_TEXRECT(FLIP) with its two RDPHALF words: s, t (s10.5) at the upper
 * left, then dsdx, dtdy (s5.10). */
static void tex_rect(u32 w0, u32 w1, u32 st, u32 d, int flip)
{
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    float s = (s16)(st >> 16) / 32.0f, t = (s16)(st & 0xFFFF) / 32.0f;
    float dsdx = (s16)(d >> 16) / 1024.0f, dtdy = (s16)(d & 0xFFFF) / 1024.0f;
    u32 cycle = (R.om_h >> G_MDSFT_CYCLETYPE) & 3;
    int saved_tile = R.tex_tile;

    if (cycle == (G_CYC_COPY >> G_MDSFT_CYCLETYPE)) {
        dsdx /= 4.0f;                        /* copy mode steps 4 texels a pixel */
        lrx += 1.0f;
        lry += 1.0f;
    } else {
        /* 1/2-cycle: a fractional lower-right edge still covers that pixel
         * (images drawn a row at a time use lr = ul + 0.75). */
        lrx = __builtin_ceilf(lrx);
        lry = __builtin_ceilf(lry);
    }
    if (lrx <= ulx || lry <= uly)
        return;
    R.tex_tile = (w1 >> 24) & 7;
    if (R.tex_tile != saved_tile)
        R.dirty = 1;
    GfxState rs = *current_state();
    rs.depth_test = rs.depth_write = false;
    rs.stereo = GFX_STEREO_FLAT;
    rs.stereo_c = 0;
    use_state(&rs);

    float sx0, sy0, sx1, sy1;
    fb_to_screen(ulx, uly, &sx0, &sy0);
    fb_to_screen(lrx, lry, &sx1, &sy1);
    float nx0 = sx0 / (SCREEN_W / 2) - 1, nx1 = sx1 / (SCREEN_W / 2) - 1;
    float ny0 = 1 - sy0 / (SCREEN_H / 2), ny1 = 1 - sy1 / (SCREEN_H / 2);
    float ds = dsdx * (flip ? lry - uly : lrx - ulx), dt = dtdy * (flip ? lrx - ulx : lry - uly);

    /* corners: upper left, lower left, lower right, upper right */
    const float xy[4][2] = { {nx0, ny0}, {nx0, ny1}, {nx1, ny1}, {nx1, ny0} };
    float ts[4], tt[4];
    if (!flip) {
        ts[0] = s;      tt[0] = t;
        ts[1] = s;      tt[1] = t + dt;
        ts[2] = s + ds; tt[2] = t + dt;
        ts[3] = s + ds; tt[3] = t;
    } else {                                 /* s runs down, t across */
        ts[0] = s;      tt[0] = t;
        ts[1] = s + ds; tt[1] = t;
        ts[2] = s + ds; tt[2] = t + dt;
        ts[3] = s;      tt[3] = t + dt;
    }
    GfxVtx q[4];
    for (int i = 0; i < 4; i++)
        q[i] = (GfxVtx){ xy[i][0], xy[i][1], -0.5f, 1.0f, ts[i], tt[i], 255, 255, 255, 255 };
    if (R.batch_n + 6 > BATCH_INDICES)
        flush();
    GfxVtx *o;
    int first = gfxb_vtx_alloc(4, &o);
    if (first >= 0) {
        __builtin_memcpy(o, q, sizeof q);
        static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++)
            R.batch[R.batch_n++] = (u16)(first + idx[i]);
    }

    if (R.tex_tile != saved_tile)
        R.dirty = 1;
    R.tex_tile = saved_tile;
}

static void fill_rect(u32 w0, u32 w1)
{
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    u32 cycle = (R.om_h >> G_MDSFT_CYCLETYPE) & 3;

    if (cycle >= 2) {                        /* fill/copy: lower right inclusive */
        lrx = __builtin_floorf(lrx) + 1;
        lry = __builtin_floorf(lry) + 1;
    } else {                                 /* 1/2-cycle: see tex_rect */
        lrx = __builtin_ceilf(lrx);
        lry = __builtin_ceilf(lry);
    }
    if (R.cimg == R.zimg && R.zimg != 0) {
            flush();
        gfxb_clear_depth();
        return;
    }
    float col[4];
    if (cycle == 3) {
        col[0] = R.fill[0]; col[1] = R.fill[1]; col[2] = R.fill[2]; col[3] = 1.0f;
        if (ulx <= 0 && uly <= 0 && lrx >= R.fb_w && lry >= R.fb_h) {
            float kx, ky, ox, oy;
            fb_map(&kx, &ky, &ox, &oy);
                    flush();
            if (ox <= 0 && oy <= 0) {
                gfxb_clear_color(to_u8(col[0]), to_u8(col[1]), to_u8(col[2]));
                return;
            }
            gfxb_clear_color(0, 0, 0);       /* the bars beside the frame */
        }
    } else {
        static const float white[4] = { 1, 1, 1, 1 };
        combine(white, col);
    }
    screen_quad(ulx, uly, lrx, lry, col, 0);
}

/* ---- The interpreter ------------------------------------------------------ */

/* ---- RDP triangles ------------------------------------------------------ */

/*
 * The sky (sky.c) sends whole RDP triangle commands through the microcode:
 * G_RDPHALF_1 holds a command word's high half, G_RDPHALF_CONT/_2 its low
 * half. They gather here; once a triangle (commands 0xC8-0xCF) is complete
 * it is rebuilt from its edge and attribute coefficients and drawn in
 * screen space.
 */
static struct {
    u32 half;               /* the pending G_RDPHALF_1 word */
    u32 w[2 * 22];          /* edges 4, shade 8, texture 8, z 2 (64-bit words) */
    int n, len;             /* 32-bit words gathered, words this command needs */
} s_rdp;

/* An s15.16 value split across two 16-bit halves (integer, fraction). */
static float rdp_fix(u32 i, u32 f, int hi)
{
    u32 ip = hi ? i >> 16 : i & 0xFFFF, fp = hi ? f >> 16 : f & 0xFFFF;
    return (s32)((ip << 16) | fp) / 65536.0f;
}

/* Attribute k of a shade (rgba) or texture (s, t, w) block: the value at
 * the top scanline, d/dx, d/de (down the major edge) and d/dy. */
static void rdp_attr(const u32 *b, int k, float out[3])
{
    int word = k >> 1, hi = !(k & 1);
    out[0] = rdp_fix(b[word], b[4 + word], hi);
    out[1] = rdp_fix(b[2 + word], b[6 + word], hi);
    out[2] = rdp_fix(b[8 + word], b[12 + word], hi);
}

#define SKY_EDGE_MARGIN 8.0f   /* N64 pixels; the stereo shift is 3 screen pixels */

static float rdp_s11_2(u32 v)
{
    return (float)((s32)(v << 18) >> 18) / 4.0f;
}

static void rdp_triangle(const u32 *w)
{
    u32 cmd = (w[0] >> 24) & 0x3F;
    int shade = (cmd & 4) != 0, tex = (cmd & 2) != 0;
    float yl = rdp_s11_2(w[0]), ym = rdp_s11_2(w[1] >> 16), yh = rdp_s11_2(w[1]);
    float xl = (s32)w[2] / 65536.0f, dxl = (s32)w[3] / 65536.0f;
    float xh = (s32)w[4] / 65536.0f, dxh = (s32)w[5] / 65536.0f;
    float xm = (s32)w[6] / 65536.0f, dxm = (s32)w[7] / 65536.0f;
    float y0 = __builtin_floorf(yh);

    if (!(yh <= ym && ym <= yl) || yh == yl)
        return;

    /* corners: down the minor edges (M, then L), back up the major (H) */
    float px[6], py[6];
    px[0] = xm + dxm * (yh - y0); py[0] = yh;
    px[1] = xm + dxm * (ym - y0); py[1] = ym;
    px[2] = xl;                   py[2] = ym;
    px[3] = xl + dxl * (yl - ym); py[3] = yl;
    px[4] = xh + dxh * (yl - y0); py[4] = yl;
    px[5] = xh + dxh * (yh - y0); py[5] = yh;

    const u32 *sb = w + 8, *tb = w + 8 + (shade ? 16 : 0);
    float col[4][3], st[3][3];
    for (int k = 0; k < 4; k++)
        if (shade) rdp_attr(sb, k, col[k]);
    for (int k = 0; k < 3; k++)
        if (tex) rdp_attr(tb, k, st[k]);
    int persp = (R.om_h >> G_MDSFT_TEXTPERSP) & 1;

    /* state first: it sets R.tmap for this tile */
    int saved_tile = R.tex_tile;
    R.tex_tile = (w[0] >> 16) & 7;
    if (R.tex_tile != saved_tile)
        R.dirty = 1;
    GfxState rs = *current_state();
    rs.depth_test = rs.depth_write = false;
    rs.shade = true;   /* the vertex colours below are the RDP's shade, or white */
    rs.stereo = GFX_STEREO_SKY;   /* sky.c's sky and sea: as far as can be */
    rs.stereo_c = 0;
    use_state(&rs);

    /* The RDP interpolates shade affinely across the screen, but with a
     * perspective texture the GPU interpolates every attribute
     * perspective-correctly (clip w = 1 / w below). Sky.c's water fades to
     * the fog colour by shade towards the horizon, and drawn as one polygon
     * the near colour covered almost all of it. So the polygon is cut into
     * horizontal bands, each vertex computed exactly from the RDP's
     * coefficients; within a band the difference is small. */
    /* One w scale for the whole polygon (w is affine, so its largest value
     * is at a corner): band edges must come out bit-identical in both
     * bands, or the sky shows hairline cracks between them. */
    float wmax = 1.0f;
    if (tex && persp) {
        wmax = 0;
        for (int i = 0; i < 6; i++) {
            float dy = py[i] - y0, dx = px[i] - (xh + dxh * dy);
            float ww = st[2][0] + st[2][1] * dx + st[2][2] * dy;
            if (ww > wmax)
                wmax = ww;
        }
        if (wmax <= 0)
            wmax = 1.0f / 65536.0f;
    }
    /* Band edges: the triangle is split at ym (where the minor edge turns
     * from M to L), and each half into bands about 8 lines high when the
     * shade needs it. x on each edge comes from that edge's equation
     * alone, so a band's bottom corners are its neighbour's top ones. */
    float ys[50];
    int ny = 0;
    int banded = tex && persp && shade;
    for (int half = 0; half < 2; half++) {
        float a = half ? ym : yh, z = half ? yl : ym;
        if (z <= a)
            continue;
        int nb = banded ? (int)((z - a) / 8.0f) + 1 : 1;
        if (nb > 24)
            nb = 24;
        if (ny == 0)
            ys[ny++] = a;
        for (int k = 1; k <= nb; k++)
            ys[ny++] = k == nb ? z : a + (z - a) * k / nb;
    }
    for (int b = 0; b + 1 < ny; b++) {
        float ya = ys[b], yb = ys[b + 1];
        int lower = ya >= ym;          /* below ym the minor edge is L */
        float qx[4], qy[4];
        qx[0] = lower ? xl + dxl * (ya - ym) : xm + dxm * (ya - y0); qy[0] = ya;
        qx[1] = lower ? xl + dxl * (yb - ym) : xm + dxm * (yb - y0); qy[1] = yb;
        qx[2] = xh + dxh * (yb - y0);                                qy[2] = yb;
        qx[3] = xh + dxh * (ya - y0);                                qy[3] = ya;
        int n = 4;
        /* With 3D on, each eye moves the sky sideways, which bared a strip
         * of the clear colour at the screen's edge; corners on the frame's
         * side edges are pushed past it (the attributes below come from the
         * moved corner, so the sky just carries on). */
        if (R.wide >= 1.0f)
            for (int i = 0; i < n; i++) {
                if (qx[i] <= 0.5f)
                    qx[i] -= SKY_EDGE_MARGIN;
                else if (qx[i] >= R.fb_w - 0.5f)
                    qx[i] += SKY_EDGE_MARGIN;
            }

        GfxVtx v[12];
        float wv[12];
        for (int i = 0; i < n; i++) {
            float dy = qy[i] - y0, dx = qx[i] - (xh + dxh * dy);
            float sx, sy;
            GfxVtx *o = &v[i];
            fb_to_screen(qx[i], qy[i], &sx, &sy);
            *o = (GfxVtx){ sx / (SCREEN_W / 2) - 1, 1 - sy / (SCREEN_H / 2), -0.5f, 1.0f,
                           0, 0, 255, 255, 255, 255 };
            if (shade) {
                u8 c[4];
                for (int k = 0; k < 4; k++) {
                    float a = col[k][0] + col[k][1] * dx + col[k][2] * dy;
                    c[k] = a <= 0 ? 0 : a >= 255 ? 255 : (u8)a;
                }
                o->r = c[0]; o->g = c[1]; o->b = c[2]; o->a = c[3];
            }
            wv[i] = 1.0f;
            if (tex) {
                float s = st[0][0] + st[0][1] * dx + st[0][2] * dy;
                float t = st[1][0] + st[1][1] * dx + st[1][2] * dy;
                float ww = st[2][0] + st[2][1] * dx + st[2][2] * dy;
                if (persp) {
                    /* the RDP divides by w (0x7FFF = 1); s, t are 10.5 */
                    if (ww <= 0)
                        ww = 1.0f / 65536.0f;
                    s = s / ww * 1024.0f;
                    t = t / ww * 1024.0f;
                    wv[i] = ww;
                } else {
                    s /= 32.0f;
                    t /= 32.0f;
                }
                o->s = s;
                o->t = t;
            }
        }
        if (tex && persp) {
            /* clip w = 1 / w, so the GPU's perspective-correct interpolation
             * of s/w, t/w matches the RDP's */
            for (int i = 0; i < n; i++) {
                float cw = wmax / wv[i];
                v[i].x *= cw; v[i].y *= cw; v[i].z *= cw; v[i].w = cw;
            }
        }

        s_stats.tris++;
        if (R.batch_n + 3 * (n - 2) > BATCH_INDICES)
            flush();
        GfxVtx *o;
        int first = gfxb_vtx_alloc(n, &o);
        if (first >= 0) {
            __builtin_memcpy(o, v, n * sizeof(GfxVtx));
            for (int i = 1; i < n - 1; i++) {
                R.batch[R.batch_n++] = (u16)first;
                R.batch[R.batch_n++] = (u16)(first + i);
                R.batch[R.batch_n++] = (u16)(first + i + 1);
            }
        }
    }
    if (R.tex_tile != saved_tile)
        R.dirty = 1;
    R.tex_tile = saved_tile;
}

/* G_RDPHALF_CONT / G_RDPHALF_2: one more 64-bit RDP word. */
static void rdp_word(u32 lo, int last)
{
    if (s_rdp.n == 0) {
        u32 cmd = (s_rdp.half >> 24) & 0x3F;
        if (cmd < 0x08 || cmd > 0x0F) {
            static int warned;
            if (!warned++)
                pal_log("f3d: RDP command %02lX through RDPHALF not handled",
                        (unsigned long)cmd);
            return;
        }
        s_rdp.len = 2 * (4 + ((cmd & 4) ? 8 : 0) + ((cmd & 2) ? 8 : 0) + ((cmd & 1) ? 2 : 0));
    }
    s_rdp.w[s_rdp.n++] = s_rdp.half;
    s_rdp.w[s_rdp.n++] = lo;
    if (s_rdp.n >= s_rdp.len) {
        rdp_triangle(s_rdp.w);
        s_rdp.n = 0;
    } else if (last) {
        s_rdp.n = 0;                    /* cut short */
    }
}

static void run_dl(Gfx *start)
{
    Gfx *stack[MAX_DL_DEPTH];
    int sp = 0;
    Gfx *g = start;
    u32 count = 0;

    while (g != NULL) {
        u32 w0 = g->words.w0, w1 = g->words.w1;
        u8 op = w0 >> 24;
        s_cur_cmd = g;
        g++;

        s_stats.cmds++;
        if (++count > MAX_COMMANDS) {
            pal_log("f3d: runaway display list, stopping task");
            return;
        }

        switch (op) {
        case OP(G_NOOP):
            if ((w0 & 0xFFFFFF) == WIDESCREEN_TAG || (w0 & 0xFFFFFF) == ANCHOR_TAG) {
                /* applies to vertices loaded, rectangles and scissors set
                 * from here on */
                union { u32 u; float f; } v = { w1 };
                if ((w0 & 0xFFFFFF) == WIDESCREEN_TAG)
                    R.wide = v.f;
                else
                    R.anchor = v.f;
            } else if ((w0 & 0xFFFFFF) == PROF_TAG) {
                /* a room's list, then the end of it (developer timer) */
                static u64 s_room_t0;
                if (g_gfx_debug_enabled) {
                    if (w1)
                        s_room_t0 = pal_ticks();
                    else if (s_room_t0)
                        pal_prof_add(PAL_PROF_ROOM_DL, pal_ticks() - s_room_t0);
                }
            } else if ((w0 & 0xFFFFFF) == MENU_TAG) {
                R.menu = 1;
            } else if ((w0 & 0xFFFFFF) == NEARCLIP_TAG) {
                union { u32 u; float f; } v = { w1 };
                if (v.f != R.near_scale) {
                    R.near_scale = v.f;
                    R.mvp_dirty = 1;   /* the depth map */
                }
            }
            break;
        case OP(G_SPNOOP):
        case OP(G_RDPLOADSYNC):
        case OP(G_RDPPIPESYNC):
        case OP(G_RDPTILESYNC):
        case OP(G_RDPFULLSYNC):
        case OP(G_CULLDL):
            break;

        case OP(G_RDPHALF_1):
            s_rdp.half = w1;
            break;
        case OP(G_RDPHALF_CONT):
        case OP(G_RDPHALF_2):
            rdp_word(w1, op == OP(G_RDPHALF_2));
            break;

        case OP(G_DL):
            if (!resolve(w1))
                break;              /* skip a call through a bad pointer */
            if (((w0 >> 16) & 0xFF) == G_DL_PUSH) {
                if (sp >= MAX_DL_DEPTH) {
                    pal_log("f3d: display list stack overflow");
                    return;
                }
                stack[sp++] = g;
            }
            g = resolve(w1);
            break;

        case OP(G_ENDDL):
            g = sp > 0 ? stack[--sp] : NULL;
            break;

        case OP(G_MTX): {
            u32 p = (w0 >> 16) & 0xFF;
            Mat m;
            const u32 *src = resolve(w1);
            if (!src)
                break;
            mat_from_n64(&m, src);
            if (p & G_MTX_PROJECTION) {
                if (p & G_MTX_LOAD)
                    R.proj = m;
                else
                    mat_mul(&R.proj, &m, &R.proj);
            } else {
                if ((p & G_MTX_PUSH) && R.mv_top < MTX_STACK - 1) {
                    R.mv[R.mv_top + 1] = R.mv[R.mv_top];
                    R.mv_top++;
                }
                if (p & G_MTX_LOAD)
                    R.mv[R.mv_top] = m;
                else
                    mat_mul(&R.mv[R.mv_top], &m, &R.mv[R.mv_top]);
            }
            R.mvp_dirty = 1;
            break;
        }

        case OP(G_POPMTX):
            if (R.mv_top > 0)
                R.mv_top--;
            R.mvp_dirty = 1;
            break;

        case OP(G_MOVEWORD): {
            u32 index = w0 & 0xFF, offset = (w0 >> 8) & 0xFFFF;
            switch (index) {
            case G_MW_SEGMENT:
                R.seg[(offset >> 2) & 0xF] = w1;
                break;
            case G_MW_FOG:
                R.fog_m = (s16)(w1 >> 16);
                R.fog_o = (s16)(w1 & 0xFFFF);
                break;
            case G_MW_NUMLIGHT:
                R.num_lights = (int)((w1 - 0x80000000u) / 32) - 1;
                if (R.num_lights < 0) R.num_lights = 0;
                if (R.num_lights > 7) R.num_lights = 7;
                break;
            default:
                break;
            }
            break;
        }

        case OP(G_MOVEMEM): {
            u32 index = (w0 >> 16) & 0xFF;
            void *p = resolve(w1);
            if (!p)
                break;
            if (index == G_MV_VIEWPORT) {
                const Vp_t *vp = &((Vp *)p)->vp;
                R.vp_sx = vp->vscale[0] / 4.0f;
                R.vp_sy = vp->vscale[1] / 4.0f;
                R.vp_tx = vp->vtrans[0] / 4.0f;
                R.vp_ty = vp->vtrans[1] / 4.0f;
            } else if (index == G_MV_LOOKATX || index == G_MV_LOOKATY) {
                const Light_t *lt = &((Light *)p)->l;
                for (int k = 0; k < 3; k++)
                    R.lookat[index == G_MV_LOOKATY][k] = lt->dir[k] / 127.0f;
            } else if (index >= G_MV_L0 && index <= G_MV_L7) {
                int l = (index - G_MV_L0) / 2;
                const Light_t *lt = &((Light *)p)->l;
                for (int k = 0; k < 3; k++) {
                    R.light_col[l][k] = lt->col[k] / 255.0f;
                    R.light_dir[l][k] = lt->dir[k] / 127.0f;
                }
            }
            break;
        }

        case OP(G_VTX): {
            u32 par = (w0 >> 16) & 0xFF;
            const Vtx *src = resolve(w1);
            if (!src)
                break;
            if (g_gfx_debug_enabled) {   /* developer timer */
                u64 t0 = pal_ticks();
                load_vertices(src, par & 0xF, (par >> 4) + 1);
                pal_prof_add(PAL_PROF_F3D_VTX, pal_ticks() - t0);
            } else {
                load_vertices(src, par & 0xF, (par >> 4) + 1);
            }
            s_stats.vtx += (par >> 4) + 1;
            break;
        }

        case OP(G_TRI1): {
            int i0 = ((w1 >> 16) & 0xFF) / 10, i1 = ((w1 >> 8) & 0xFF) / 10, i2 = (w1 & 0xFF) / 10;
            if (i0 < NUM_VTX && i1 < NUM_VTX && i2 < NUM_VTX)
                triangle(i0, i1, i2);
            break;
        }

        case OP(G_TRI4):
            for (int i = 0; i < 4; i++) {
                int x = (w1 >> (8 * i)) & 0xF, y = (w1 >> (8 * i + 4)) & 0xF;
                int z = (w0 >> (4 * i)) & 0xF;
                if (x || y || z)
                    triangle(x, y, z);
            }
            break;

        /* State changes mark the state for a rebuild only when a value the
         * state is built from changes; the game sets many again as they are. */
        case OP(G_TEXTURE): {
            /* the scale is applied when vertices load */
            R.tex_s = ((w1 >> 16) & 0xFFFF) / 65536.0f;
            R.tex_t = (w1 & 0xFFFF) / 65536.0f;
            int tile = (w0 >> 8) & 7, levels = (w0 >> 11) & 7;
            if (tile != R.tex_tile || levels != R.tex_levels) {
                R.tex_tile = tile;
                R.tex_levels = levels;
                R.dirty = 1;
            }
            break;
        }

        case OP(G_SETTIMG):
            tex_settimg(w0, w1);
            break;
        case OP(G_SETTILE):
            R.tex_dirty |= tex_settile(w0, w1);
            break;
        case OP(G_SETTILESIZE):
            R.tex_dirty |= tex_settilesize(w0, w1);
            break;
        case OP(G_LOADBLOCK):
            { u64 t0 = g_gfx_debug_enabled ? pal_ticks() : 0;   /* developer timer */
              R.tex_dirty |= tex_loadblock(w0, w1);
              if (g_gfx_debug_enabled) pal_prof_add(PAL_PROF_F3D_TEXLOAD, pal_ticks() - t0); }
            break;
        case OP(G_LOADTILE):
            { u64 t0 = g_gfx_debug_enabled ? pal_ticks() : 0;
              R.tex_dirty |= tex_loadtile(w0, w1);
              if (g_gfx_debug_enabled) pal_prof_add(PAL_PROF_F3D_TEXLOAD, pal_ticks() - t0); }
            break;
        case OP(G_LOADTLUT):
            R.tex_dirty |= tex_loadtlut(w0, w1);
            break;

        case OP(G_SETGEOMETRYMODE):
        case OP(G_CLEARGEOMETRYMODE): {
            u32 geom = op == OP(G_SETGEOMETRYMODE) ? R.geom | w1 : R.geom & ~w1;
            /* lighting, texgen and culling are applied per vertex/triangle */
            if ((geom ^ R.geom) & (G_ZBUFFER | G_FOG | G_SHADE))
                R.dirty = 1;
            R.geom = geom;
            break;
        }

        case OP(G_SETOTHERMODE_H):
        case OP(G_SETOTHERMODE_L): {
            u32 shift = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            u32 mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            u32 *mode = op == OP(G_SETOTHERMODE_H) ? &R.om_h : &R.om_l;
            u32 v = (*mode & ~mask) | (w1 & mask);
            if (v != *mode) {
                *mode = v;
                R.dirty = 1;
            }
            break;
        }
        case OP(G_RDPSETOTHERMODE):
            if (R.om_h != (w0 & 0x00FFFFFF) || R.om_l != w1) {
                R.om_h = w0 & 0x00FFFFFF;
                R.om_l = w1;
                R.dirty = 1;
            }
            break;

        case OP(G_SETCOMBINE):
            if (R.cc0 != (w0 & 0x00FFFFFF) || R.cc1 != w1) {
                R.cc0 = w0 & 0x00FFFFFF;
                R.cc1 = w1;
                R.dirty = 1;
            }
            break;
        case OP(G_SETPRIMCOLOR):
            if (R.prim_c != w1 || R.prim_lod != (w0 & 0xFF)) {
                unpack_rgba(w1, R.prim);
                R.prim_c = w1;
                R.prim_lod = w0 & 0xFF;
                R.dirty = 1;
            }
            break;
        case OP(G_SETENVCOLOR):
            if (R.env_c != w1) {
                unpack_rgba(w1, R.env);
                R.env_c = w1;
                R.dirty = 1;
            }
            break;
        case OP(G_SETFOGCOLOR):
            if (R.fog_c != w1) {
                R.fog_c = w1;
                R.dirty = 1;
            }
            break;
        case OP(G_SETBLENDCOLOR):
            if (R.blend_a != (w1 & 0xFF)) {
                R.blend_a = w1 & 0xFF;
                R.dirty = 1;
            }
            break;
        case OP(G_SETFILLCOLOR): {
            u32 c = w1 >> 16;   /* RGBA5551, repeated */
            R.fill[0] = ((c >> 11) & 0x1F) / 31.0f;
            R.fill[1] = ((c >> 6) & 0x1F) / 31.0f;
            R.fill[2] = ((c >> 1) & 0x1F) / 31.0f;
            R.fill[3] = 1.0f;
            break;
        }

        case OP(G_SETCIMG):
            R.cimg = (u32)(uintptr_t)resolve(w1);
            R.fb_w = (float)((w0 & 0xFFF) + 1);
            R.fb_h = R.fb_w * 0.75f;
            break;
        case OP(G_SETZIMG):
            R.zimg = (u32)(uintptr_t)resolve(w1);
            break;

        case OP(G_SETSCISSOR):
            set_scissor(w0, w1);
            break;

        case OP(G_FILLRECT):
            s_stats.fills++;
            fill_rect(w0, w1);
            break;

        case OP(G_TEXRECT):
        case OP(G_TEXRECTFLIP):
            tex_rect(w0, w1, g[0].words.w1, g[1].words.w1, op == OP(G_TEXRECTFLIP));
            g += 2;   /* its two RDPHALF words */
            break;

        default: {
            static u8 seen[32];
            if (!(seen[op >> 3] & (1 << (op & 7)))) {
                seen[op >> 3] |= 1 << (op & 7);
                pal_log("f3d: unhandled opcode %02X (%08lX %08lX) at %p", op, (unsigned long)w0,
                        (unsigned long)w1, (const void *)s_cur_cmd);
            }
            break;
        }
        }
    }
}

/* Runs one graphics task; returns its interpreting time in ticks. */
static u64 f3d_task_run(OSTask *task)
{
    __builtin_memset(&R.seg, 0, sizeof R.seg);
    R.mv_top = 0;
    R.mvp_dirty = 1;
    R.geom = 0;
    R.fb_w = 320.0f;
    R.wide = 0.0f;
    R.anchor = -1.0f;
    R.near_scale = 1.0f;
    R.menu = 0;
    R.menu_pieces = 0;
    R.menu_depth = MENU_BACK;
    R.menu_last_piece = 0;
    R.fb_h = 240.0f;
    R.vp_sx = 160.0f; R.vp_sy = 120.0f; R.vp_tx = 160.0f; R.vp_ty = 120.0f;
    /* the microcode's default lookat: eye-space x and y */
    __builtin_memset(R.lookat, 0, sizeof R.lookat);
    R.lookat[0][0] = R.lookat[1][1] = 1.0f;
    R.batch_n = 0;
    R.noise_off[0] = (noise_rand() % NOISE_SIZE) / (float)NOISE_SIZE;
    R.noise_off[1] = (noise_rand() % NOISE_SIZE) / (float)NOISE_SIZE;
    s_rdp.n = 0;
    __builtin_memset(R.scissor, 0, sizeof R.scissor);
    __builtin_memset(&R.batch_state, 0xFF, sizeof R.batch_state);   /* force the first set */
    R.dirty = 1;
    R.state_live = 0;
    for (int i = 0; i < NUM_VTX; i++)   /* GPU indices from the last task are stale */
        R.vtx[i].idx = -1;
    tex_reset();

    __builtin_memset(&s_stats, 0, sizeof s_stats);
    u64 t0 = pal_ticks();
    gfxb_frame_begin();
    u64 t1 = pal_ticks();
    run_dl((Gfx *)task->t.data_ptr);
    flush();
    tex_task_end();
    u64 t2 = pal_ticks();
    gfxb_frame_end();
    u64 t3 = pal_ticks();
    if (R.menu)
        s_menu_pieces = R.menu_pieces;
    s_last = s_stats;

    /* Where the task's time went, for the perf log: begin (waiting for
     * the GPU to finish the last frame), interpreting, end (submitting). */
    pal_os_lock();
    s_times[0] += t1 - t0;
    s_times[1] += t2 - t1;
    s_times[2] += t3 - t2;
    if (t2 - t1 < s_times[3])
        s_times[3] = t2 - t1;
    pal_os_unlock();
    return t2 - t1;
}

/* Benchmark: sdmc:/3ds/ge007/autobench.txt holding a task number N
 * (run_azahar.py --bench) makes the Nth graphics task run BENCH_RUNS more
 * times, with the game held: one frozen scene, timed without the texture
 * decodes of a moving view. The game steps one tick a frame in these runs
 * (frametiming.c), so task N is the same scene in every build. */
#define BENCH_RUNS 120

static void bench(OSTask *task)
{
    static int s_at = -1, s_n, s_cold, s_runs;
    if (s_at < 0) {
        s_at = pal_debug_read_int("sdmc:/3ds/ge007/autobench.txt", 0);
        /* more runs, to sample with tools/azahar_threads.py --profile */
        s_runs = pal_debug_read_int("sdmc:/3ds/ge007/autobench_runs.txt", BENCH_RUNS);
        /* negative: "cold", every texture decoded again each run */
        s_cold = s_at < -1;
        if (s_cold)
            s_at = -s_at;
    }
    if (s_at <= 0 || ++s_n != s_at)
        return;
    extern u32 gfxb_debug_tex_created;
    u32 made0 = gfxb_debug_tex_created;
    u64 sum = 0, lo = ~0ull, hi = 0;
    u64 w0 = pal_ticks();
    for (int i = 0; i < s_runs; i++) {
        if (s_cold)
            gfxb_tex_forget_all();
        u64 t = f3d_task_run(task);
        sum += t;
        lo = t < lo ? t : lo;
        hi = t > hi ? t : hi;
    }
    u64 wall = pal_ticks() - w0;
    float ms = 1000.0f / PAL_TICKS_PER_SEC;
    pal_log("bench%s: %d runs: interpret %.3f ms (min %.3f, max %.3f), whole task %.3f ms; "
            "%lu cmds %lu vtx %lu tris %lu culled %lu draws, %lu textures made",
            s_cold ? " (cold)" : "", s_runs, (double)(sum * ms / s_runs), (double)(lo * ms), (double)(hi * ms),
            (double)(wall * ms / s_runs), (unsigned long)s_last.cmds, (unsigned long)s_last.vtx,
            (unsigned long)s_last.tris, (unsigned long)s_last.culled, (unsigned long)s_last.draws,
            (unsigned long)(gfxb_debug_tex_created - made0));
}

static void f3d_task(OSTask *task)
{
    /* developer token: -freezeN draws graphics
     * task N again and again from then on, so that screenshots of two
     * builds or modes can be compared pixel for pixel (with run_azahar.py
     * --bench 100000, which steps the game one tick a frame) */
    static int s_freeze, s_n;
    if (!s_inited) {
        gfxb_init();
        s_inited = 1;
        char tok[128];
        if (pal_debug_read_text("sdmc:/3ds/ge007/tokens.txt", tok, sizeof tok) > 0) {
            const char *f = __builtin_strstr(tok, "-freeze");
            for (f = f ? f + 7 : NULL; f && *f >= '0' && *f <= '9'; f++)
                s_freeze = s_freeze * 10 + (*f - '0');
        }
    }
    f3d_task_run(task);
    bench(task);
    if (s_freeze && ++s_n == s_freeze) {
        pal_log("f3d: frozen at graphics task %d", s_freeze);
        for (;;) {
            pal_os_lock();
            pal_os_wait_timeout(33000000);
            pal_os_unlock();
            f3d_task_run(task);
        }
    }
}

/* Ticks spent in task begin, interpreting and end since the last call,
 * and the shortest interpreting time of one task (steadier to compare
 * builds by: texture decodes come and go with the view). */
void f3d_take_times(u64 out[4])
{
    pal_os_lock();
    for (int i = 0; i < 3; i++) {
        out[i] = s_times[i];
        s_times[i] = 0;
    }
    out[3] = s_times[3];
    s_times[3] = ~0ull;
    pal_os_unlock();
}

/* Last task's commands, vertices, triangles drawn, triangles culled, fills,
 * state rebuilds, state changes, draw calls, texture-only state refreshes. */
void f3d_stats(u32 out[9])
{
    out[5] = s_last.rebuilds;
    out[6] = s_last.changes;
    out[7] = s_last.draws;
    out[8] = s_last.texchanges;
    out[0] = s_last.cmds;
    out[1] = s_last.vtx;
    out[2] = s_last.tris;
    out[3] = s_last.culled;
    out[4] = s_last.fills;
}

void f3d_init(void)
{
    osPortSetTaskHandler(M_GFXTASK, f3d_task);
}
