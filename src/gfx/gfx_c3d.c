/*
 * gfx_c3d.c - the GPU backend for the F3D interpreter, on citro3d.
 *
 * One graphics task is one citro3d frame on the top screen. Triangles come
 * in already transformed (gfx_backend.h); this file batches them into a
 * linear-memory vertex buffer and draws runs that share state.
 *
 * Depth: the shader passes z through, and citro3d's default depth map
 * turns z/w in [-1, 0] into a depth of 1 (near) to 0 (far), so "nearer"
 * is GREATER and a cleared depth buffer holds 0. Clears are drawn as
 * full-screen quads so they stay in order with the draws around them.
 *
 * Textures: a small cache of RGBA8 textures keyed by what the interpreter
 * decoded them from. An entry is only evicted if no draw in the current
 * frame uses it (the GPU reads textures when the frame is submitted).
 *
 * State and draws: citro3d sets the GPU up at the start of each frame (one
 * empty draw sends what it has), and from then on this file writes the
 * registers itself, straight into the command buffer: only those whose
 * values changed, and each draw as one block. citro3d's own path (setters
 * marking state dirty, then a function call per register at the next draw)
 * cost about 10 us a draw in Azahar, and frames have ~300 draws. The
 * register sequences are citro3d's (C3Di_UpdateContext, C3D_DrawElements,
 * C3Di_EffectBind, C3Di_SetTex), so nothing differs from what it would
 * send. citro3d's state setters must not be called during a frame after
 * that, or its next draw would send their state over this file's.
 *
 * Stereo 3D: the left eye's commands are recorded as they are written, and
 * at the end of the frame they are sent again to the right eye's target.
 * The only difference between the two is the eye-shift uniform (f3d.v.pica
 * "ster"), whose E word the copy negates; every place it was written is
 * remembered. Vertices, indices and textures are shared.
 *
 * All of this runs on the RSP task thread; nothing else touches citro3d.
 */
#include <3ds.h>
#include <citro3d.h>
#include <string.h>

#include "f3d_shbin.h"
#include "gfx/gfx_backend.h"
#include "pal/pal_sys.h"

#define VBUF_VERTS 65536          /* indices are 16-bit */
#define IBUF_INDICES (192 * 1024)
#define MAX_TEX    1024
#define TEX_BUCKETS 2048          /* hash buckets, a power of two */
#define DECAL_BIAS  (1.0f / 16384)   /* depth units (1 = near plane) */
/* Stereo: each eye's shift at infinity with the slider all the way up, in
 * clip x (2 = the screen's width, 400 pixels): 3 pixels, 6 apart (0.03
 * was too strong for the user). Things nearer than the screen's depth
 * come out by up to STEREO_POP times that. */
#define STEREO_K_MAX 0.015f
#define STEREO_POP   1.0f
#define MAX_EYE_WORDS 2048

#define DISPLAY_TRANSFER_FLAGS                                                   \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                              \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static DVLB_s *s_dvlb;
static shaderProgram_s s_prog;
static C3D_RenderTarget *s_target;
static C3D_RenderTarget *s_right;     /* the right eye's */
static GfxVtx *s_vbuf;
static int s_vused;
static u16 *s_ibuf;
static int s_iused;
static u32 s_ibuf_pa;                 /* s_ibuf's physical address less the buffer base */
/* What the GPU currently has, so only changed parts are sent again (a
 * texture switch per row-strip rectangle is common). */
static GfxState s_applied;
static GPU_WRITEMASK s_applied_mask;
static bool s_applied_valid;
static u32 s_depth_scale;             /* the depth map (f24): scale, offsets plain/decal */
static u32 s_depth_offset[2];
static int s_unif;                    /* the shader's first uniform (tA0); the rest follow */
static int s_unif_ster;               /* its eye shift */

static float s_stereo_k;              /* this frame's shift at infinity (0: one eye) */
static u32 s_rec_start;               /* the left eye's commands start here */
static u32 s_eye_words[MAX_EYE_WORDS];   /* where "ster"'s E was written */
static int s_neye;

static bool s_overflow_logged;
static u32 s_frame;

typedef struct {
    u64 key;
    C3D_Tex tex;
    u32 regs[6];                      /* as bound: border, dim, param, lod, address; type */
    u32 used_frame;
    bool live;                        /* tex allocated */
    bool keyed;                       /* holds key, linked in its bucket */
    u16 next;                         /* next id in the bucket, 0 = end */
} TexEntry;

static TexEntry s_tex[MAX_TEX + 1];   /* [0]: an 8x8 white texture for "none" */
static u16 s_bucket[TEX_BUCKETS];
u32 gfxb_debug_tex_created;   /* textures made so far (perf log) */
static int s_tex_count;               /* ids in use: 1..s_tex_count */

/* For tools/azahar_threads.py --screenshot, which finds this variable in
 * the ELF and reads the picture through the emulator's GDB stub. */
void *gfx_debug_colorbuf;

static void tex_upload(u32 *dst, const u32 *texels, int w, int h)
{
    /* RGBA8 in 8x8 tiles, texels in Morton order inside a tile. The PICA
     * samples v = 0 from the end of the data, so the image's last row goes
     * first; then v = 0 is the image's first row, as gfx_backend.h says
     * (seen on the legal screen: its text was upside down before). */
    static const u8 mx[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const u8 my[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };
    if (w < 8 || h < 8) {   /* not a whole tile: texel by texel */
        for (int y = 0; y < h; y++) {
            const u32 *row = texels + (h - 1 - y) * w;
            for (int x = 0; x < w; x++)
                dst[((y >> 3) * (w >> 3) + (x >> 3)) * 64 + (mx[x & 7] | my[y & 7])] = row[x];
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        const u32 *row = texels + (h - 1 - y) * w;
        u32 *tile = dst + (y >> 3) * (w >> 3) * 64 + my[y & 7];
        for (int x = 0; x < w; x += 8, tile += 64) {
            const u32 *s = row + x;
            tile[0x00] = s[0]; tile[0x01] = s[1]; tile[0x04] = s[2]; tile[0x05] = s[3];
            tile[0x10] = s[4]; tile[0x11] = s[5]; tile[0x14] = s[6]; tile[0x15] = s[7];
        }
    }
}

/* What binding a texture writes to its unit (C3Di_SetTex). */
static void tex_regs(TexEntry *e)
{
    e->regs[0] = e->tex.border;
    e->regs[1] = e->tex.dim;
    e->regs[2] = e->tex.param;
    e->regs[3] = e->tex.lodParam;
    e->regs[4] = osConvertVirtToPhys(e->tex.data) >> 3;
    e->regs[5] = e->tex.fmt & 0xF;
}

/* Writes the CPU's cached data in [p, p + size) back to memory, for the GPU.
 * A system call: GSPGPU_FlushDataCache asks the GSP service to do the same
 * over IPC, which cost ~60 us a texture in Azahar. */
static void flush_dcache(const void *p, u32 size)
{
    if (size)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, size);
}

/* Texture memory written this frame, flushed together at its end (the GPU
 * reads it only once the frame is submitted): ranges close together go
 * as one call. */
#define MAX_DIRTY 64
#define DIRTY_GAP (32 * 1024)
static struct { u32 a, b; } s_dirty[MAX_DIRTY];
static int s_ndirty;

static void flush_dirty(void)
{
    for (int i = 1; i < s_ndirty; i++) {   /* by address (insertion sort) */
        for (int j = i; j > 0 && s_dirty[j - 1].a > s_dirty[j].a; j--) {
            u32 a = s_dirty[j].a, b = s_dirty[j].b;
            s_dirty[j] = s_dirty[j - 1];
            s_dirty[j - 1].a = a;
            s_dirty[j - 1].b = b;
        }
    }
    for (int i = 0; i < s_ndirty;) {
        u32 a = s_dirty[i].a, b = s_dirty[i].b;
        for (i++; i < s_ndirty && s_dirty[i].a <= b + DIRTY_GAP; i++)
            if (s_dirty[i].b > b)
                b = s_dirty[i].b;
        flush_dcache((const void *)a, b - a);
    }
    s_ndirty = 0;
}

static void mark_dirty(const void *p, u32 size)
{
    if (s_ndirty == MAX_DIRTY)
        flush_dirty();
    s_dirty[s_ndirty].a = (u32)p;
    s_dirty[s_ndirty].b = (u32)p + size;
    s_ndirty++;
}

/* ---- Raw GPU commands ----------------------------------------------------- */

/* Room for this many words in the command buffer, or NULL. */
static u32 *cmd_begin(u32 words)
{
    if (!gpuCmdBuf || gpuCmdBufOffset + words > gpuCmdBufSize) {
        static bool logged;
        if (!logged) {
            logged = true;
            pal_log("gfx: GPU command buffer full, dropping draws");
        }
        return NULL;
    }
    return gpuCmdBuf + gpuCmdBufOffset;
}

static void cmd_end(u32 *p)
{
    gpuCmdBufOffset = p - gpuCmdBuf;
}

/* One register write, with a byte mask. */
static inline u32 *wr(u32 *p, u32 reg, u32 mask, u32 v)
{
    p[0] = v;
    p[1] = GPUCMD_HEADER(0, mask, reg);
    return p + 2;
}

/* n values to consecutive registers from reg (inc), or all to reg, padded
 * to 64 bits as GPUCMD_Add does. */
static inline u32 *wr_n(u32 *p, u32 reg, int inc, const u32 *v, int n)
{
    p[0] = v[0];
    p[1] = GPUCMD_HEADER(inc, 0xF, reg) | ((u32)(n - 1) << 20);
    for (int i = 1; i < n; i++)
        p[1 + i] = v[i];
    p += 1 + n;
    if (!(n & 1))
        *p++ = 0;
    return p;
}

static inline u32 *wr_inc(u32 *p, u32 reg, const u32 *v, int n)
{
    return wr_n(p, reg, 1, v, n);
}

/* The shader's uniforms for st (f3d.v.pica): tA0, tB0, tA1, tB1, tN1,
 * cmul, cadd, each as citro3d sends a vector: w, z, y, x. */
#define NUM_UNIF 7
static void unif_values(const GfxState *st, u32 out[NUM_UNIF * 4])
{
    float v[NUM_UNIF][4] = { { 0 } };
    for (int u = 0; u < 2; u++) {
        v[u * 2][0] = st->tex_map[u][0];
        v[u * 2][1] = st->tex_map[u][1];
        v[u * 2 + 1][0] = st->tex_map[u][2];
        v[u * 2 + 1][1] = st->tex_map[u][3];
    }
    if (st->noise) {
        v[4][0] = st->noise_scale[0];
        v[4][1] = st->noise_scale[1];
    }
    for (int k = 0; k < 4; k++) {
        v[5][k] = st->shade ? 1.0f / 255 : 0.0f;
        v[6][k] = st->shade ? 0.0f : 1.0f;
    }
    for (int i = 0; i < NUM_UNIF; i++)
        for (int k = 0; k < 4; k++)
            __builtin_memcpy(&out[i * 4 + k], &v[i][3 - k], 4);
}

/* "ster" for st, as sent: E, M, B, A (gfx_backend.h GFX_STEREO_*). */
static void ster_values(const GfxState *st, u32 out[4])
{
    float v[4] = { 0 };   /* A, B, M, E */
    if (s_stereo_k > 0 && st->stereo != GFX_STEREO_FLAT) {
        v[0] = st->stereo == GFX_STEREO_LAYER ? s_stereo_k * st->stereo_c : s_stereo_k;
        v[1] = st->stereo == GFX_STEREO_WORLD ? s_stereo_k * st->stereo_c : 0.0f;
        v[2] = st->stereo == GFX_STEREO_WORLD ? -STEREO_POP * s_stereo_k : 0.0f;
        v[3] = -1.0f;   /* the left eye's image moves left, so far things recede */
    }
    for (int k = 0; k < 4; k++)
        __builtin_memcpy(&out[k], &v[3 - k], 4);
}

static bool unif_differs(const GfxState *old, const GfxState *st)
{
    return !old || old->shade != st->shade || old->noise != st->noise ||
           memcmp(old->tex_map, st->tex_map, sizeof st->tex_map) != 0 ||
           (st->noise && memcmp(old->noise_scale, st->noise_scale, sizeof st->noise_scale) != 0);
}

/* A stage's five TexEnv registers: sources, operands, functions, constant,
 * scale (C3D_TexEnv's layout). Unused stages pass the previous one on
 * (C3D_TexEnvInit); stage 0 of a state without stages gives the vertex
 * colour. */
static void tev_regs(const GfxState *st, int i, u32 out[5])
{
    if (i < st->num_stages) {
        const GfxTevStage *s = &st->stages[i];
        out[0] = (s->rgb.src[0] | s->rgb.src[1] << 4 | s->rgb.src[2] << 8) |
                 (u32)(s->a.src[0] | s->a.src[1] << 4 | s->a.src[2] << 8) << 16;
        out[1] = (s->rgb.op[0] | s->rgb.op[1] << 4 | s->rgb.op[2] << 8) |
                 (u32)(s->a.op[0] | s->a.op[1] << 4 | s->a.op[2] << 8) << 12;
        out[2] = s->rgb.func | (u32)s->a.func << 16;
        out[3] = __builtin_bswap32(s->constant);   /* 0xRRGGBBAA -> 0xAABBGGRR */
    } else {
        out[0] = i == 0 ? 0 : (GPU_PREVIOUS | GPU_PREVIOUS << 16);
        out[1] = 0;
        out[2] = GPU_REPLACE | GPU_REPLACE << 16;
        out[3] = 0xFFFFFFFF;
    }
    out[4] = 0;
}

void gfxb_init(void)
{
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4))
        pal_fatal("citro3d init failed");

    s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!s_target)
        pal_fatal("could not create the top screen render target");
    C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    gfx_debug_colorbuf = s_target->frameBuf.colorBuf;
    /* The right eye: drawn only while the slider is up, and the top screen
     * shows stereo only in frames that drew it (C3D_FrameEnd). */
    s_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (s_right) {
        C3D_RenderTargetSetOutput(s_right, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
        gfxSet3D(true);
    } else {
        pal_log("gfx: no VRAM for the right eye, stereo 3D off");
    }

    s_dvlb = DVLB_ParseFile((u32 *)f3d_shbin, f3d_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);            /* x y z w */
    AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);            /* s t */
    AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);    /* r g b a */

    static const char *const names[NUM_UNIF] = { "tA0", "tB0", "tA1", "tB1", "tN1", "cmul", "cadd" };
    s_unif = shaderInstanceGetUniformLocation(s_prog.vertexShader, names[0]);
    for (int i = 1; i < NUM_UNIF; i++)
        if (shaderInstanceGetUniformLocation(s_prog.vertexShader, names[i]) != s_unif + i)
            pal_fatal("f3d.v.pica: uniform %s is not where expected", names[i]);
    s_unif_ster = shaderInstanceGetUniformLocation(s_prog.vertexShader, "ster");

    s_vbuf = linearAlloc(VBUF_VERTS * sizeof(GfxVtx));
    s_ibuf = linearAlloc(IBUF_INDICES * sizeof(u16));
    if (!s_vbuf || !s_ibuf)
        pal_fatal("no linear memory for the vertex buffer");
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_vbuf, sizeof(GfxVtx), 3, 0x210);
    s_ibuf_pa = osConvertVirtToPhys(s_ibuf) - bi->base_paddr;
    s_depth_scale = f32tof24(-1.0f);
    s_depth_offset[0] = f32tof24(0.0f);
    s_depth_offset[1] = f32tof24(DECAL_BIAS);

    static u32 white[64];
    memset(white, 0xFF, sizeof white);
    if (!C3D_TexInit(&s_tex[0].tex, 8, 8, GPU_RGBA8))
        pal_fatal("no linear memory for textures");
    tex_upload(s_tex[0].tex.data, white, 8, 8);
    GSPGPU_FlushDataCache(s_tex[0].tex.data, sizeof white);
    s_tex[0].live = true;
    tex_regs(&s_tex[0]);

    C3D_CullFace(GPU_CULL_NONE);   /* the interpreter culls, in N64 screen space */
    pal_log("gfx: citro3d up, %lu KB linear memory free", (unsigned long)(linearSpaceFree() / 1024));
}

static void apply_state(const GfxState *st, GPU_WRITEMASK mask);

/* citro3d's part of a frame on target: texture units 0 and 1 on, blending
 * (not logic ops), the depth map, then a draw of nothing (one point three
 * times) so that it sends all it has now. Everything after is sent from
 * here. */
static void c3d_setup(C3D_RenderTarget *target)
{
    C3D_FrameDrawOn(target);
    C3D_TexBind(0, &s_tex[0].tex);
    C3D_TexBind(1, &s_tex[0].tex);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    GfxVtx *v;
    int first = gfxb_vtx_alloc(3, &v);
    if (first >= 0) {
        memset(v, 0, 3 * sizeof *v);
        C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    }
}

void gfxb_frame_begin(void)
{
    /* No SYNCDRAW: the game paces itself on the (emulated) retrace, and
     * waiting for a vblank slot here only held up every task. */
    C3D_FrameBegin(0);
    C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0x000000FF, 0);
    s_vused = 0;
    s_iused = 0;
    s_frame++;

    s_stereo_k = s_right ? osGet3DSliderState() * STEREO_K_MAX : 0.0f;
    if (s_stereo_k > 0)
        C3D_RenderTargetClear(s_right, C3D_CLEAR_ALL, 0x000000FF, 0);
    c3d_setup(s_target);
    s_rec_start = gpuCmdBufOffset;
    s_neye = 0;

    s_applied_valid = false;
    GfxState none;
    memset(&none, 0, sizeof none);
    apply_state(&none, GPU_WRITE_COLOR);
}

/* The left eye's commands again, to the right eye's target, with each
 * "ster" E negated. */
static void right_eye(void)
{
    u32 n = gpuCmdBufOffset - s_rec_start;
    if (s_neye > MAX_EYE_WORDS) {
        static bool logged;
        if (!logged) {
            logged = true;
            pal_log("gfx: %d eye-shift writes in a frame, right eye skipped", s_neye);
        }
        return;
    }
    /* checked first: once drawn on, the right eye is shown */
    if (gpuCmdBufOffset + n + 256 > gpuCmdBufSize) {
        static bool logged;
        if (!logged) {
            logged = true;
            pal_log("gfx: no room in the command buffer for the right eye (%lu words)", (unsigned long)n);
        }
        return;
    }
    c3d_setup(s_right);
    u32 *src = gpuCmdBuf + s_rec_start;
    u32 *dst = cmd_begin(n);
    if (!dst)
        return;
    memcpy(dst, src, n * 4);
    for (int i = 0; i < s_neye; i++)
        dst[s_eye_words[i] - s_rec_start] ^= 0x80000000u;   /* E = -E */
    cmd_end(dst + n);
}

void gfxb_frame_end(void)
{
    if (s_stereo_k > 0)
        right_eye();
    flush_dcache(s_vbuf, s_vused * sizeof(GfxVtx));
    flush_dcache(s_ibuf, s_iused * sizeof(u16));
    flush_dirty();
    C3D_FrameEnd(0);
}

/* Stage i as the GPU has it (old) differs from what st wants. */
static bool stage_differs(const GfxState *old, const GfxState *st, int i)
{
    if (!old)
        return true;
    bool was = i < old->num_stages, now = i < st->num_stages;
    if (was != now)
        return true;
    if (!now)
        return false;   /* unused in both (pass-through, or stage 0's vertex colour) */
    return memcmp(&old->stages[i], &st->stages[i], sizeof st->stages[i]) != 0;
}

/* Sends what differs between the GPU's state and st (everything at the
 * start of a frame). */
static void apply_state(const GfxState *st, GPU_WRITEMASK mask)
{
    const GfxState *old = s_applied_valid ? &s_applied : NULL;
    /* at most: two textures (8 + 2 words each), the cache clear (2), six
     * stages (6 each), the buffer bits (2), scissor (4), effect (12),
     * uniforms (32) */
    u32 *p = cmd_begin(120);
    if (!p)
        return;

    bool bound = false;
    for (int u = 0; u < 2; u++) {
        if (old && old->tex[u] == st->tex[u])
            continue;
        int id = st->tex[u];
        if (id > MAX_TEX || !s_tex[id].live)
            id = 0;
        const u32 *r = s_tex[id].regs;
        p = wr_inc(p, u ? GPUREG_TEXUNIT1_BORDER_COLOR : GPUREG_TEXUNIT0_BORDER_COLOR, r, 5);
        p = wr(p, u ? GPUREG_TEXUNIT1_TYPE : GPUREG_TEXUNIT0_TYPE, 0xF, r[5]);
        bound = true;
    }
    if (bound) {
        /* The texture cache is cleared at every bind, as citro3d does.
         * Clearing only after a texture was rewritten (session 11) left
         * stale texels on hardware: Cradle's walkway showed the noise
         * texture in the first eye drawn and not the second (Azahar has
         * no texture cache). */
        p = wr(p, GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
    }

    bool tev_same = old && old->num_stages == st->num_stages &&
                    memcmp(old->stages, st->stages, sizeof st->stages[0] * st->num_stages) == 0;
    if (!tev_same) {
        for (int i = 0; i < GFX_MAX_STAGES; i++) {
            if (!stage_differs(old, st, i))
                continue;
            u32 r[5];
            tev_regs(st, i, r);
            p = wr_inc(p, i < 4 ? GPUREG_TEXENV0_SOURCE + i * 8 : GPUREG_TEXENV4_SOURCE + (i - 4) * 8, r, 5);
        }
    }
    if (!old || old->buf_update != st->buf_update) {
        u32 b = st->buf_update & 0xF;
        p = wr(p, GPUREG_TEXENV_UPDATE_BUFFER, 0x2, b << 8 | b << 12);
    }

    if (!old || memcmp(old->scissor, st->scissor, sizeof st->scissor) != 0 ||
        (s_stereo_k > 0 && old->stereo != st->stereo)) {   /* widened for world draws */
        /* The framebuffer is the screen turned a quarter: its x runs up
         * the picture, its y right to left (see f3d.v.pica). */
        const uint16_t *sc = st->scissor;
        u32 r[3] = { GPU_SCISSOR_DISABLE, 0, 0 };
        if (sc[2] > sc[0] && sc[3] > sc[1]) {
            /* With 3D on, a room seen through a portal moves sideways in
             * each eye but the game's scissor round the portal does not:
             * a strip of clear colour stood at its edge (the lines down
             * Dam's tunnel). Widened by the shift at infinity; what spills
             * over lies behind the portal's wall and fails the depth test. */
            u32 x0 = sc[0], x1 = sc[2];
            if (s_stereo_k > 0 && st->stereo == GFX_STEREO_WORLD) {
                u32 m = (u32)(s_stereo_k * 200.0f) + 2;
                x0 = x0 > m ? x0 - m : 0;
                x1 = x1 + m < 400 ? x1 + m : 400;
            }
            u32 left = 240 - sc[3], top = 400 - x1, right = 240 - sc[1], bottom = 400 - x0;
            r[0] = GPU_SCISSOR_NORMAL;
            r[1] = left | top << 16;
            r[2] = (right - 1) | (bottom - 1) << 16;
        }
        p = wr_inc(p, GPUREG_SCISSORTEST_MODE, r, 3);
    }

    if (unif_differs(old, st)) {
        u32 v[NUM_UNIF * 4];
        unif_values(st, v);
        p = wr(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, 0x80000000u | s_unif);   /* f32 values */
        p = wr_n(p, GPUREG_VSH_FLOATUNIFORM_DATA, 0, v, NUM_UNIF * 4);
    }
    if (!old || old->stereo != st->stereo ||
        ((st->stereo == GFX_STEREO_WORLD || st->stereo == GFX_STEREO_LAYER) && old->stereo_c != st->stereo_c)) {
        u32 v[4];
        ster_values(st, v);
        p = wr(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, 0x80000000u | s_unif_ster);
        if (s_neye < MAX_EYE_WORDS)
            s_eye_words[s_neye] = p - gpuCmdBuf;   /* wr_n's first word is v[0], E */
        s_neye++;
        p = wr_n(p, GPUREG_VSH_FLOATUNIFORM_DATA, 0, v, 4);
    }

    bool fixed_same = old && s_applied_mask == mask &&
                      old->depth_test == st->depth_test && old->depth_write == st->depth_write &&
                      old->depth_equal == st->depth_equal && old->blend == st->blend &&
                      old->alpha_test == st->alpha_test && old->alpha_ref == st->alpha_ref;
    if (!fixed_same) {
        /* Decals: the N64 accepts a depth within the surface's own slope (dz);
         * here they are pulled a little towards the eye instead, or a decal
         * whose vertices differ from the surface's fights with it. */
        if (!old || old->depth_equal != st->depth_equal) {
            u32 r[2] = { s_depth_scale, s_depth_offset[st->depth_equal] };
            p = wr_inc(p, GPUREG_DEPTHMAP_SCALE, r, 2);
        }
        u32 zf = !st->depth_test ? GPU_ALWAYS : st->depth_equal ? GPU_GEQUAL : GPU_GREATER;
        u32 wmask = (mask & GPU_WRITE_COLOR) | (st->depth_write ? GPU_WRITE_DEPTH : 0);
        u32 depth = (st->depth_test || st->depth_write) | zf << 4 | wmask << 8;
        u32 alpha = st->alpha_test | GPU_GEQUAL << 4 | (u32)st->alpha_ref << 8;
        u32 blend = st->blend
            ? GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_SRC_ALPHA << 16 | GPU_ONE_MINUS_SRC_ALPHA << 20 |
              GPU_SRC_ALPHA << 24 | (u32)GPU_ONE_MINUS_SRC_ALPHA << 28
            : GPU_BLEND_ADD | GPU_BLEND_ADD << 8 | GPU_ONE << 16 | GPU_ZERO << 20 |
              GPU_ONE << 24 | (u32)GPU_ZERO << 28;
        p = wr(p, GPUREG_FRAGOP_ALPHA_TEST, 0xF, alpha);
        p = wr(p, GPUREG_DEPTH_COLOR_MASK, 0xF, depth);
        /* as C3Di_EffectBind derives it from the depth function */
        p = wr(p, GPUREG_GAS_DELTAZ_DEPTH, 0x8, ((0xAF02u >> (zf * 2)) & 3) << 24);
        p = wr(p, GPUREG_BLEND_FUNC, 0xF, blend);
    }
    cmd_end(p);

    s_applied = *st;
    s_applied_mask = mask;
    s_applied_valid = true;
}

void gfxb_set_state(const GfxState *st)
{
    apply_state(st, GPU_WRITE_COLOR);
}

static void overflow(void)
{
    if (!s_overflow_logged) {
        s_overflow_logged = true;
        pal_log("gfx: vertex or index buffer full, dropping triangles");
    }
}

int gfxb_vtx_alloc(int n, GfxVtx **out)
{
    if (s_vused + n > VBUF_VERTS) {
        overflow();
        return -1;
    }
    *out = &s_vbuf[s_vused];
    s_vused += n;
    return s_vused - n;
}

/* Draws count indices from s_ibuf[first]: C3D_DrawElements' commands. */
static void draw_elements(int first, int count)
{
    u32 *p = cmd_begin(30);
    if (!p)
        return;
    p = wr(p, GPUREG_PRIMITIVE_CONFIG, 0x2, GPU_GEOMETRY_PRIM);
    p = wr(p, GPUREG_RESTART_PRIMITIVE, 0xF, 1);
    p = wr(p, GPUREG_INDEXBUFFER_CONFIG, 0xF, (s_ibuf_pa + first * 2) | 1u << 31);   /* u16 */
    p = wr(p, GPUREG_NUMVERTICES, 0xF, count);
    p = wr(p, GPUREG_VERTEX_OFFSET, 0xF, 0);
    p = wr(p, GPUREG_GEOSTAGE_CONFIG, 0x2, 0x100);
    p = wr(p, GPUREG_GEOSTAGE_CONFIG2, 0x2, 0x100);
    p = wr(p, GPUREG_START_DRAW_FUNC0, 0x1, 0);
    p = wr(p, GPUREG_DRAWELEMENTS, 0xF, 1);
    p = wr(p, GPUREG_START_DRAW_FUNC0, 0x1, 1);
    p = wr(p, GPUREG_GEOSTAGE_CONFIG, 0x2, 0);
    p = wr(p, GPUREG_GEOSTAGE_CONFIG2, 0x2, 0);
    p = wr(p, GPUREG_VTX_FUNC, 0xF, 1);
    p = wr(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    p = wr(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    cmd_end(p);
}

void gfxb_draw(const GfxVtx *v, int count)
{
    GfxVtx *o;
    if (s_iused + count > IBUF_INDICES) {
        overflow();
        return;
    }
    int first = gfxb_vtx_alloc(count, &o);
    if (first < 0)
        return;
    memcpy(o, v, count * sizeof(GfxVtx));
    u16 *idx = &s_ibuf[s_iused];
    for (int i = 0; i < count; i++)
        idx[i] = (u16)(first + i);
    draw_elements(s_iused, count);
    s_iused += count;
}

void gfxb_draw_indexed(const uint16_t *idx, int count)
{
    if (s_iused + count > IBUF_INDICES) {
        overflow();
        return;
    }
    memcpy(&s_ibuf[s_iused], idx, count * sizeof(u16));
    draw_elements(s_iused, count);
    s_iused += count;
}

/* A quad over the whole screen at the far plane, in the vertex colour. */
static void full_quad(uint8_t r, uint8_t g, uint8_t b)
{
    static const float xy[6][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1} };
    GfxVtx q[6];
    for (int i = 0; i < 6; i++)
        q[i] = (GfxVtx){ xy[i][0], xy[i][1], 0.0f, 1.0f, 0, 0, r, g, b, 255 };
    gfxb_draw(q, 6);
}

void gfxb_clear_depth(void)
{
    GfxState st = { .depth_test = false, .depth_write = true, .shade = true }, keep = s_applied;
    apply_state(&st, 0);
    full_quad(0, 0, 0);
    apply_state(&keep, GPU_WRITE_COLOR);
}

void gfxb_clear_color(uint8_t r, uint8_t g, uint8_t b)
{
    GfxState st = { .shade = true }, keep = s_applied;
    apply_state(&st, GPU_WRITE_COLOR);
    full_quad(r, g, b);
    apply_state(&keep, GPU_WRITE_COLOR);
}

/* ---- Texture cache --------------------------------------------------------- */

static u32 bucket_of(u64 key)
{
    return (u32)(key ^ (key >> 29)) & (TEX_BUCKETS - 1);
}

static void unlink_entry(int id)
{
    u16 *p = &s_bucket[bucket_of(s_tex[id].key)];
    while (*p && *p != id)
        p = &s_tex[*p].next;
    if (*p)
        *p = s_tex[id].next;
    s_tex[id].keyed = false;
}

void gfxb_tex_forget_all(void)
{
    for (int id = 1; id <= s_tex_count; id++)
        if (s_tex[id].keyed)
            unlink_entry(id);
}

uint16_t gfxb_tex_find(uint64_t key)
{
    for (u16 id = s_bucket[bucket_of(key)]; id; id = s_tex[id].next) {
        if (s_tex[id].key == key) {
            s_tex[id].used_frame = s_frame;
            return id;
        }
    }
    return 0;
}

uint64_t gfxb_tex_key(uint16_t id)
{
    return id >= 1 && id <= s_tex_count && s_tex[id].keyed ? s_tex[id].key : 0;
}

bool gfxb_tex_check(uint16_t id, uint64_t key)
{
    if (id < 1 || id > s_tex_count || !s_tex[id].keyed || s_tex[id].key != key)
        return false;
    s_tex[id].used_frame = s_frame;
    return true;
}

static GPU_TEXTURE_WRAP_PARAM wrap_param(int w)
{
    return w == GFX_WRAP_MIRROR ? GPU_MIRRORED_REPEAT : w == GFX_WRAP_CLAMP ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
}

static uint16_t tex_create(uint64_t key, const uint32_t *const *levels, int n, int w, int h,
                           int wrap_s, int wrap_t, bool linear)
{
    /* A new id while there are any, else the least recently used one not
     * needed this frame. */
    int pick = 0;
    if (s_tex_count < MAX_TEX) {
        pick = ++s_tex_count;
    } else {
        /* A clock sweep: the next one not used for a few frames, else the
         * next one not used this frame (a full scan for the least recently
         * used one cost every new texture 1024 compares). */
        static int s_hand = 1;
        for (int pass = 0; pass < 2 && !pick; pass++) {
            u32 age = pass == 0 ? 8 : 1;
            for (int k = 0; k < MAX_TEX; k++) {
                int i = s_hand;
                s_hand = s_hand == MAX_TEX ? 1 : s_hand + 1;
                if (s_frame - s_tex[i].used_frame >= age) {
                    pick = i;
                    break;
                }
            }
        }
    }
    if (pick == 0) {
        static bool logged;
        if (!logged) {
            logged = true;
            pal_log("gfx: texture cache full within one frame");
        }
        return 0;
    }

    TexEntry *e = &s_tex[pick];
    if (e->keyed)
        unlink_entry(pick);
    if (e->live && (e->tex.width != w || e->tex.height != h || e->tex.maxLevel != n - 1)) {
        C3D_TexDelete(&e->tex);
        e->live = false;
    }
    if (!e->live) {
        if (!C3D_TexInitWithParams(&e->tex, NULL,
                (C3D_TexInitParams){ w, h, (u8)(n - 1), GPU_RGBA8, GPU_TEX_2D, false })) {
            static bool logged;
            if (!logged) {
                logged = true;
                pal_log("gfx: out of linear memory for a %dx%d texture (%lu KB free)", w, h,
                        (unsigned long)(linearSpaceFree() / 1024));
            }
            return 0;
        }
        e->live = true;
    }
    for (int k = 0; k < n; k++)
        tex_upload(C3D_Tex2DGetImagePtr(&e->tex, k, NULL), levels[k], w >> k, h >> k);
    u32 bytes;
    C3D_Tex2DGetImagePtr(&e->tex, -1, &bytes);
    mark_dirty(e->tex.data, bytes);
    gfxb_debug_tex_created++;
    C3D_TexSetWrap(&e->tex, wrap_param(wrap_s), wrap_param(wrap_t));
    C3D_TexSetFilter(&e->tex, linear ? GPU_LINEAR : GPU_NEAREST, linear ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetFilterMipmap(&e->tex, GPU_LINEAR);
    tex_regs(e);
    e->key = key;
    e->keyed = true;
    e->used_frame = s_frame;
    u32 b = bucket_of(key);
    e->next = s_bucket[b];
    s_bucket[b] = (u16)pick;
    return pick;
}

uint16_t gfxb_tex_create(uint64_t key, const uint32_t *texels, int w, int h,
                         int wrap_s, int wrap_t, bool linear)
{
    return tex_create(key, &texels, 1, w, h, wrap_s, wrap_t, linear);
}

uint16_t gfxb_tex_create_mip(uint64_t key, const uint32_t *const *levels, int n, int w, int h,
                             int wrap_s, int wrap_t, bool linear)
{
    return tex_create(key, levels, n, w, h, wrap_s, wrap_t, linear);
}
