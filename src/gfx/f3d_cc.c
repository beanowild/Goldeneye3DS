/*
 * f3d_cc.c - the RDP colour combiner, compiled to PICA texture-environment
 * stages.
 *
 * Each N64 cycle computes (A - B) * C + D, for colour and alpha
 * separately, from a fixed menu of inputs. A PICA stage computes one
 * function of up to three sources, and has one constant colour. Inputs
 * map to sources as:
 *
 *   SHADE           primary (vertex) colour
 *   TEXEL0/1        texture units 0/1 (swapped in cycle 2, as the RDP does)
 *   COMBINED        cycle 2 only: cycle 1's output, kept in the PICA's
 *                   combiner buffer (stage update bits)
 *   PRIM, ENV, 1, 0, PRIM_LOD_FRAC, *_ALPHA of those
 *                   constants: folded on the CPU into the stage constant
 *
 * The expression is simplified first (C = 0, B = 0, B = D lerps, constant
 * folding), then emitted as 1-3 stages per cycle, never with two
 * different constants in one stage. Colour and alpha are emitted
 * separately and zipped into the same stages, padded with pass-throughs.
 *
 * Inexact: (A - B) goes through a stage that clamps at 0 when neither side
 * is constant, LOD_FRACTION is one value per draw, NOISE is 0.5, and the keying
 * inputs (CENTER, SCALE, K4, K5) as 0.
 */
#include <ultra64.h>
#include <PR/gbi.h>

#include "f3d_int.h"

enum { T_CONST, T_SHADE, T_TEX0, T_TEX1, T_COMB, T_PREV };

typedef struct {
    u8 kind;
    u8 alpha;          /* colour group: the source's alpha, broadcast */
    float k[3];        /* T_CONST value (alpha group: k[0]) */
} Term;

typedef struct {
    u8 func, n;
    u8 src[3], op[3];
    u8 kused;
    float k[3];
} Op;

typedef struct {
    int alpha;         /* emitting the alpha group */
    int n;
    Op ops[3];
    int ok;
} Emit;

static const CcInput *s_in;

static float ch(u32 c, int i) { return ((c >> (24 - 8 * i)) & 0xFF) / 255.0f; }

static Term tconst3(float r, float g, float b) { Term t = { T_CONST, 0, { r, g, b } }; return t; }
static Term tconst(float v) { return tconst3(v, v, v); }
static Term tkind(int kind, int alpha) { Term t = { (u8)kind, (u8)alpha, { 0, 0, 0 } }; return t; }

static Term color_const(u32 c, int alpha)
{
    return alpha ? tconst(ch(c, 3)) : tconst3(ch(c, 0), ch(c, 1), ch(c, 2));
}

static Term texel(int which, int cycle, int alpha)
{
    if (cycle == 1)
        which ^= 1;
    return tkind(which ? T_TEX1 : T_TEX0, alpha);
}

static Term combined(int cycle, int alpha)
{
    return cycle == 1 ? tkind(T_COMB, alpha) : tconst(0);
}

/* Colour input for slot 0-3 (a, b, c, d). */
static Term color_input(int slot, u32 code, int cycle)
{
    switch (code) {
    case 0: return combined(cycle, 0);
    case 1: return texel(0, cycle, 0);
    case 2: return texel(1, cycle, 0);
    case 3: return color_const(s_in->prim, 0);
    case 4: return tkind(T_SHADE, 0);
    case 5: return color_const(s_in->env, 0);
    }
    if (slot == 0)
        return tconst(code == 6 ? 1.0f : code == 7 ? 0.5f : 0.0f);
    if (slot == 3)
        return tconst(code == 6 ? 1.0f : 0.0f);
    if (slot == 2) {
        switch (code) {
        case 7: return combined(cycle, 1);
        case 8: return texel(0, cycle, 1);
        case 9: return texel(1, cycle, 1);
        case 10: return color_const(s_in->prim, 1);
        case 11: return tkind(T_SHADE, 1);
        case 12: return color_const(s_in->env, 1);
        case 13: return tconst(s_in->lod_frac / 255.0f);
        case 14: return tconst(s_in->prim_lod / 255.0f);
        }
    }
    return tconst(0.0f);
}

/* Alpha input for slot 0-3. */
static Term alpha_input(int slot, u32 code, int cycle)
{
    switch (code) {
    case 0: return slot == 2 ? tconst(s_in->lod_frac / 255.0f) : combined(cycle, 1);   /* c: LOD_FRACTION */
    case 1: return texel(0, cycle, 1);
    case 2: return texel(1, cycle, 1);
    case 3: return color_const(s_in->prim, 1);
    case 4: return tkind(T_SHADE, 1);
    case 5: return color_const(s_in->env, 1);
    case 6: return tconst(slot == 2 ? s_in->prim_lod / 255.0f : 1.0f);
    default: return tconst(0.0f);
    }
}

/* ---- Term arithmetic ---------------------------------------------------- */

static int nch(const Emit *e) { return e->alpha ? 1 : 3; }

static int is_k(const Term *t) { return t->kind == T_CONST; }

static int k_all(const Emit *e, const Term *t, float v)
{
    if (!is_k(t))
        return 0;
    for (int i = 0; i < nch(e); i++)
        if (__builtin_fabsf(t->k[i] - v) > 1e-4f)
            return 0;
    return 1;
}

static int same(const Emit *e, const Term *a, const Term *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->kind != T_CONST)
        return e->alpha || a->alpha == b->alpha;
    for (int i = 0; i < nch(e); i++)
        if (__builtin_fabsf(a->k[i] - b->k[i]) > 1e-4f)
            return 0;
    return 1;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static Term k_fn(const Term *a, const Term *b, const Term *c, int fn)
{
    Term r = tconst(0);
    for (int i = 0; i < 3; i++) {
        float x = a->k[i], y = b ? b->k[i] : 0, z = c ? c->k[i] : 0;
        switch (fn) {
        case 0: r.k[i] = x - y; break;          /* a - b (unclamped) */
        case 1: r.k[i] = x * y; break;          /* a * b */
        case 2: r.k[i] = x * y + z; break;      /* a * b + c */
        case 3: r.k[i] = 1.0f - x; break;
        }
    }
    return r;
}

/* ---- Emitting ---------------------------------------------------------- */

static int arg(Emit *e, Op *op, int i, const Term *t, int invert)
{
    if (t->kind == T_CONST) {
        float v[3];
        for (int c = 0; c < 3; c++)
            v[c] = clamp01(invert ? 1.0f - t->k[c] : t->k[c]);
        if (op->kused) {
            for (int c = 0; c < nch(e); c++)
                if (__builtin_fabsf(op->k[c] - v[c]) > 1e-4f)
                    return 0;
        }
        op->kused = 1;
        op->k[0] = v[0]; op->k[1] = v[1]; op->k[2] = v[2];
        op->src[i] = GFX_SRC_CONSTANT;
        op->op[i] = e->alpha ? GFX_OP_A_ALPHA : GFX_OP_COLOR;
        return 1;
    }
    switch (t->kind) {
    case T_SHADE: op->src[i] = GFX_SRC_PRIMARY; break;
    case T_TEX0: op->src[i] = GFX_SRC_TEX0; break;
    case T_TEX1: op->src[i] = GFX_SRC_TEX1; break;
    case T_COMB: op->src[i] = GFX_SRC_PREV_BUFFER; break;
    default: op->src[i] = GFX_SRC_PREVIOUS; break;
    }
    if (e->alpha)
        op->op[i] = invert ? GFX_OP_A_ONE_MINUS_ALPHA : GFX_OP_A_ALPHA;
    else if (t->alpha)
        op->op[i] = invert ? GFX_OP_ONE_MINUS_ALPHA : GFX_OP_ALPHA;
    else
        op->op[i] = invert ? GFX_OP_ONE_MINUS_COLOR : GFX_OP_COLOR;
    return 1;
}

/* One stage: fn of terms (invert: bit i makes term i one-minus). */
static void stage(Emit *e, int fn, int n, const Term *t0, const Term *t1, const Term *t2, int invert)
{
    const Term *t[3] = { t0, t1, t2 };
    if (e->n >= 3) {
        e->ok = 0;
        return;
    }
    Op *op = &e->ops[e->n++];
    *op = (Op){ 0 };
    op->func = (u8)fn;
    op->n = (u8)n;
    for (int i = 0; i < 3; i++) {
        op->src[i] = GFX_SRC_PREVIOUS;
        op->op[i] = 0;
    }
    for (int i = 0; i < n; i++)
        if (!arg(e, op, i, t[i], (invert >> i) & 1))
            e->ok = 0;
}

static const Term s_prev = { T_PREV, 0, { 0, 0, 0 } };

static void e_single(Emit *e, const Term *x)
{
    stage(e, GFX_FN_REPLACE, 1, x, NULL, NULL, 0);
}

static void e_add(Emit *e, const Term *x, const Term *d)
{
    if (is_k(x) && is_k(d)) {
        Term k = k_fn(x, d, NULL, 0);
        for (int i = 0; i < 3; i++) k.k[i] = x->k[i] + d->k[i];
        e_single(e, &k);
    } else if (k_all(e, d, 0)) {
        e_single(e, x);
    } else if (k_all(e, x, 0)) {
        e_single(e, d);
    } else {
        stage(e, GFX_FN_ADD, 2, x, d, NULL, 0);
    }
}

static void e_mul(Emit *e, const Term *x, const Term *c)
{
    if (is_k(x) && is_k(c)) {
        Term k = k_fn(x, c, NULL, 1);
        e_single(e, &k);
    } else if (k_all(e, c, 1)) {
        e_single(e, x);
    } else if (k_all(e, x, 1)) {
        e_single(e, c);
    } else {
        stage(e, GFX_FN_MODULATE, 2, x, c, NULL, 0);
    }
}

/* x * c + d */
static void e_mul_add(Emit *e, const Term *x, const Term *c, const Term *d)
{
    if (k_all(e, d, 0) || k_all(e, x, 0) || k_all(e, c, 0)) {
        if (k_all(e, x, 0) || k_all(e, c, 0))
            e_single(e, d);
        else
            e_mul(e, x, c);
        return;
    }
    if (is_k(x) && is_k(c)) {
        Term k = k_fn(x, c, NULL, 1);
        e_add(e, &k, d);
        return;
    }
    int nk = is_k(x) + is_k(c) + is_k(d);
    if (nk <= 1 || (is_k(d) && ((is_k(x) && same(e, x, d)) || (is_k(c) && same(e, c, d))))) {
        if (k_all(e, c, 1))
            e_add(e, x, d);
        else if (k_all(e, x, 1))
            e_add(e, c, d);
        else
            stage(e, GFX_FN_MULTIPLY_ADD, 3, x, c, d, 0);
        return;
    }
    /* Two different constants: multiply first, then add. */
    e_mul(e, x, c);
    e_add(e, &s_prev, d);
}

/* a * c + b * (1 - c) */
static void e_lerp(Emit *e, const Term *a, const Term *b, const Term *c)
{
    if (is_k(c)) {
        Term inv = k_fn(c, NULL, NULL, 3);
        if (is_k(a)) {                          /* b * (1 - c) + a * c */
            Term k = k_fn(a, c, NULL, 1);
            e_mul_add(e, b, &inv, &k);
        } else if (is_k(b)) {                   /* a * c + b * (1 - c) */
            Term k = k_fn(b, &inv, NULL, 1);
            e_mul_add(e, a, c, &k);
        } else {
            stage(e, GFX_FN_INTERPOLATE, 3, a, b, c, 0);
        }
        return;
    }
    if (is_k(a) && is_k(b) && !same(e, a, b)) {
        e_mul(e, c, a);                         /* c * a */
        stage(e, GFX_FN_MULTIPLY_ADD, 3, c, b, &s_prev, 1);   /* (1 - c) * b + prev */
        return;
    }
    stage(e, GFX_FN_INTERPOLATE, 3, a, b, c, 0);
}

/* (a - b) * c + d */
static void e_expr(Emit *e, Term a, Term b, Term c, Term d)
{
    if (k_all(e, &c, 0) || same(e, &a, &b)) {
        e_single(e, &d);
        return;
    }
    if (is_k(&a) && is_k(&b) && is_k(&c) && is_k(&d)) {
        Term ab = k_fn(&a, &b, NULL, 0);
        Term k = k_fn(&ab, &c, &d, 2);
        e_single(e, &k);
        return;
    }
    if (k_all(e, &b, 0)) {
        e_mul_add(e, &a, &c, &d);
        return;
    }
    if (same(e, &b, &d)) {
        e_lerp(e, &a, &b, &c);
        return;
    }
    if (is_k(&a) && is_k(&b)) {
        Term k = k_fn(&a, &b, NULL, 0);
        int neg = 1, pos = 1;
        for (int i = 0; i < nch(e); i++) {
            if (k.k[i] < 0) pos = 0;
            if (k.k[i] > 0) neg = 0;
        }
        if (neg && !pos) {                      /* d - |k| * c */
            for (int i = 0; i < 3; i++) k.k[i] = -k.k[i];
            e_mul(e, &c, &k);
            stage(e, GFX_FN_SUBTRACT, 2, &d, &s_prev, NULL, 0);
            return;
        }
        e_mul_add(e, &k, &c, &d);               /* (mixed signs clamp) */
        return;
    }
    stage(e, GFX_FN_SUBTRACT, 2, &a, &b, NULL, 0);
    e_mul_add(e, &s_prev, &c, &d);
}

static void compile_cycle(int cycle, Emit *rgb, Emit *alpha)
{
    u32 w0 = s_in->cc0, w1 = s_in->cc1;
    u32 a, b, c, d, aa, ab, ac, ad;
    if (cycle == 0) {
        a = (w0 >> 20) & 0xF; c = (w0 >> 15) & 0x1F; aa = (w0 >> 12) & 7; ac = (w0 >> 9) & 7;
        b = (w1 >> 28) & 0xF; d = (w1 >> 15) & 7; ab = (w1 >> 12) & 7; ad = (w1 >> 9) & 7;
    } else {
        a = (w0 >> 5) & 0xF; c = w0 & 0x1F; aa = (w1 >> 21) & 7; ac = (w1 >> 18) & 7;
        b = (w1 >> 24) & 0xF; d = (w1 >> 6) & 7; ab = (w1 >> 3) & 7; ad = w1 & 7;
    }
    *rgb = (Emit){ .alpha = 0, .ok = 1 };
    *alpha = (Emit){ .alpha = 1, .ok = 1 };
    e_expr(rgb, color_input(0, a, cycle), color_input(1, b, cycle), color_input(2, c, cycle),
           color_input(3, d, cycle));
    e_expr(alpha, alpha_input(0, aa, cycle), alpha_input(1, ab, cycle), alpha_input(2, ac, cycle),
           alpha_input(3, ad, cycle));
}

static u8 to_u8(float v) { return (u8)(clamp01(v) * 255.0f + 0.5f); }

static int uses(const Emit *e, int src)
{
    for (int i = 0; i < e->n; i++)
        for (int k = 0; k < e->ops[i].n; k++)
            if (e->ops[i].src[k] == src)
                return 1;
    return 0;
}

static void put_args(GfxTevArgs *out, const Op *op)
{
    out->func = op->func;
    for (int i = 0; i < 3; i++) {
        out->src[i] = op->src[i];
        out->op[i] = op->op[i];
    }
}

static int cc_compile_uncached(const CcInput *in, GfxState *st);

/* The same combiner and colours come back for every rectangle of a row
 * strip image; remember the last result. */
int cc_compile(const CcInput *in, GfxState *st)
{
    /* A few recent results: draws often alternate between two modes. */
    enum { MEMO = 8 };
    static struct {
        CcInput in;
        GfxTevStage stages[GFX_MAX_STAGES];
        u8 n, buf;
        int mask;                   /* -1: empty */
    } memo[MEMO] = { [0 ... MEMO - 1] = { .mask = -1 } };
    static int next;

    for (int i = 0; i < MEMO; i++) {
        const CcInput *m = &memo[i].in;
        if (memo[i].mask >= 0 && m->cc0 == in->cc0 && m->cc1 == in->cc1 &&
            m->two_cycle == in->two_cycle && m->prim == in->prim && m->env == in->env &&
            m->prim_lod == in->prim_lod && m->lod_frac == in->lod_frac) {
            __builtin_memcpy(st->stages, memo[i].stages, sizeof memo[i].stages);
            st->num_stages = memo[i].n;
            st->buf_update = memo[i].buf;
            return memo[i].mask;
        }
    }
    int i = next;
    next = (next + 1) % MEMO;
    memo[i].mask = cc_compile_uncached(in, st);
    memo[i].in = *in;
    __builtin_memcpy(memo[i].stages, st->stages, sizeof memo[i].stages);
    memo[i].n = st->num_stages;
    memo[i].buf = st->buf_update;
    return memo[i].mask;
}

static int cc_compile_uncached(const CcInput *in, GfxState *st)
{
    static const Op pass = { GFX_FN_REPLACE, 1, { GFX_SRC_PREVIOUS, GFX_SRC_PREVIOUS, GFX_SRC_PREVIOUS }, { 0, 0, 0 }, 0, { 0, 0, 0 } };
    int cycles = in->two_cycle ? 2 : 1, n = 0, texmask = 0, first2 = 0;

    s_in = in;
    st->num_stages = 0;
    st->buf_update = 0;
    for (int cy = 0; cy < cycles; cy++) {
        Emit rgb, alpha;
        compile_cycle(cy, &rgb, &alpha);
        if (!rgb.ok || !alpha.ok) {
            /* Shouldn't happen; fall back to texture * shade. */
            Term t0 = tkind(T_TEX0, 0), sh = tkind(T_SHADE, 0);
            rgb = (Emit){ .alpha = 0, .ok = 1 };
            alpha = (Emit){ .alpha = 1, .ok = 1 };
            e_mul(&rgb, &t0, &sh);
            e_mul(&alpha, &t0, &sh);
        }
        int len = rgb.n > alpha.n ? rgb.n : alpha.n;
        for (int i = 0; i < len && n < GFX_MAX_STAGES; i++, n++) {
            const Op *r = i < rgb.n ? &rgb.ops[i] : &pass;
            const Op *a = i < alpha.n ? &alpha.ops[i] : &pass;
            GfxTevStage *s = &st->stages[n];
            put_args(&s->rgb, r);
            put_args(&s->a, a);
            s->constant = ((u32)to_u8(r->k[0]) << 24) | ((u32)to_u8(r->k[1]) << 16) |
                          ((u32)to_u8(r->k[2]) << 8) | to_u8(a->k[0]);
        }
        if (cy == 0 && cycles == 2 && n > 0 && n <= 4) {
            st->buf_update = (u8)(1u << (n - 1));
            first2 = n;
        }
        texmask |= (uses(&rgb, GFX_SRC_TEX0) || uses(&alpha, GFX_SRC_TEX0)) ? 1 : 0;
        texmask |= (uses(&rgb, GFX_SRC_TEX1) || uses(&alpha, GFX_SRC_TEX1)) ? 2 : 0;
    }
    /* The PICA's buffer lags a stage: stage k's saved output reaches
     * PREVIOUS_BUFFER from stage k + 2 on. The stage right after cycle 1
     * gets the combined colour as PREVIOUS instead. */
    if (first2 > 0 && first2 < n) {
        GfxTevArgs *args[2] = { &st->stages[first2].rgb, &st->stages[first2].a };
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < 3; j++)
                if (args[k]->src[j] == GFX_SRC_PREV_BUFFER)
                    args[k]->src[j] = GFX_SRC_PREVIOUS;
    }
    st->num_stages = (u8)n;
    return texmask;
}
