/*
 * asp.c - runs the game's audio tasks: the libultra audio microcode
 * (aspMain, "ABI 1") in C.
 *
 * An audio task is a list of Acmd commands (include/PR/abi.h) that the
 * synthesizer (src/libultra/audio) builds each frame. The microcode works
 * on a few KB of DMEM: it loads ADPCM data into it, decodes, resamples,
 * applies envelopes and pans into dry/wet buffers, mixes, interleaves, and
 * saves the result to the RAM buffer that then goes to osAiSetNextBuffer.
 * Here DMEM is an array and each command is a function, following the
 * behaviour documented by the N64 HLE audio implementations.
 *
 * Data: the command list and every buffer it touches are native order.
 * Compressed ADPCM is read a byte at a time, so it stays as the ROM has
 * it; the codebooks and loop states are swapped when the bank is loaded
 * (bnkf.c). RAW16 samples are big-endian: load.c flags their loads and
 * A_LOADBUFF swaps them. Addresses are real pointers (osVirtualToPhysical is the
 * identity); the segment table is kept for form's sake.
 */
#include <ultra64.h>
#include <PR/abi.h>

#include "pal/pal_sys.h"
#include "ultra/os_sp.h"

#define DMEM_SIZE 0x1000

static union {
    u8  b[DMEM_SIZE];
    s16 h[DMEM_SIZE / 2];
} s_dmem;

static struct {
    u32 seg[16];
    u16 in, out, count;                 /* A_SETBUFF, main */
    u16 dry_right, wet_left, wet_right; /* A_SETBUFF with A_AUX */
    s16 vol[2], target[2];
    s32 rate[2];
    s16 vol_dry, vol_wet;
    s16 table[8][16];                   /* A_LOADADPCM codebook */
    u32 loop;                           /* A_SETLOOP: ADPCM loop state */
} A;

static s16 s_resample_table[64][4];

#define DMEM_U8(a)   (&s_dmem.b[(a) & (DMEM_SIZE - 1)])
#define DMEM_S16(a)  (&s_dmem.h[((a) & (DMEM_SIZE - 1)) >> 1])

static inline s16 clamp16(s32 v)
{
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : (s16)v;
}

static inline s32 clamp32(s64 v)
{
    return v < -0x7FFFFFFFLL - 1 ? (s32)(-0x7FFFFFFFLL - 1) : v > 0x7FFFFFFF ? 0x7FFFFFFF : (s32)v;
}

static void *ram(u32 addr)
{
    u32 seg = addr >> 24;
    if (seg < 16)
        return (void *)(uintptr_t)(A.seg[seg] + (addr & 0xFFFFFF));
    return (void *)(uintptr_t)addr;
}

static u32 round_up(u32 v, u32 a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ---- Commands ------------------------------------------------------------ */

static void adpcm(u32 flags, u32 state_addr)
{
    const u8 *in = DMEM_U8(A.in);
    s16 *out = DMEM_S16(A.out);
    s16 *state = ram(state_addr);
    int nbytes = (int)round_up(A.count, 32);

    /* the previous frame's last 16 samples come first in the output */
    if (flags & A_INIT)
        __builtin_memset(out, 0, 16 * sizeof(s16));
    else if (flags & A_LOOP)
        __builtin_memcpy(out, ram(A.loop), 16 * sizeof(s16));
    else
        __builtin_memcpy(out, state, 16 * sizeof(s16));
    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;
        const s16 *tbl = A.table[*in++ & 0xF];   /* [0..7]: book 1, [8..15]: book 2 */
        for (int half = 0; half < 2; half++) {
            s16 ins[8];
            s16 prev1 = out[-1], prev2 = out[-2];
            for (int j = 0; j < 4; j++) {
                ins[j * 2] = (s16)((s32)((u32)(*in >> 4) << 28) >> 28 << shift);
                ins[j * 2 + 1] = (s16)((s32)((u32)(*in++ & 0xF) << 28) >> 28 << shift);
            }
            for (int j = 0; j < 8; j++) {
                s32 acc = tbl[j] * prev2 + tbl[8 + j] * prev1 + (ins[j] << 11);
                for (int k = 0; k < j; k++)
                    acc += tbl[8 + (j - k) - 1] * ins[k];
                *out++ = clamp16(acc >> 11);
            }
        }
        nbytes -= 16 * sizeof(s16);
    }
    __builtin_memcpy(state, out - 16, 16 * sizeof(s16));
}

static void resample(u32 flags, u16 pitch, u32 state_addr)
{
    s16 tmp[16];
    s16 *in_initial = DMEM_S16(A.in), *in = in_initial;
    s16 *out = DMEM_S16(A.out);
    s16 *state = ram(state_addr);
    int nbytes = (int)round_up(A.count, 16);
    u32 acc;

    if (flags & A_INIT)
        __builtin_memset(tmp, 0, sizeof tmp);
    else
        __builtin_memcpy(tmp, state, sizeof tmp);
    if (flags & 2) {
        __builtin_memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / (int)sizeof(s16);
    }
    in -= 4;
    acc = (u16)tmp[4];
    __builtin_memcpy(in, tmp, 4 * sizeof(s16));

    do {
        for (int i = 0; i < 8; i++) {
            const s16 *t = s_resample_table[acc * 64 >> 16];
            s32 sample = ((in[0] * t[0] + 0x4000) >> 15) + ((in[1] * t[1] + 0x4000) >> 15) +
                         ((in[2] * t[2] + 0x4000) >> 15) + ((in[3] * t[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);
            acc += (u32)pitch << 1;
            in += acc >> 16;
            acc &= 0xFFFF;
        }
        nbytes -= 8 * sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16)acc;
    __builtin_memcpy(state, in, 4 * sizeof(s16));
    int i = (int)((in - in_initial + 4) & 7);
    in -= i;
    if (i != 0)
        i = -8 - i;
    state[5] = (s16)i;
    __builtin_memcpy(state + 8, in, 8 * sizeof(s16));
}

/*
 * Envelope mixer. Rare's synthesizer (libultrare/audio/env.c, _getRate)
 * sends a linear slope: the volume change per 8 samples in 16.16, rather
 * than stock libultra's per-8-sample multiplier, so the ramps here add.
 * ENVMIX_STATE (40 shorts) holds s32 vols[2][8], then target, rate, dry/wet.
 */
typedef struct {
    s32 vols[2][8];
    s16 target[2];
    s32 rate[2];
    s16 vol_dry, vol_wet;
} EnvState;
_Static_assert(sizeof(EnvState) <= sizeof(ENVMIX_STATE), "ENVMIX_STATE too small");

static void envmixer(u32 flags, u32 state_addr)
{
    s16 *in = DMEM_S16(A.in);
    s16 *dry[2] = { DMEM_S16(A.out), DMEM_S16(A.dry_right) };
    s16 *wet[2] = { DMEM_S16(A.wet_left), DMEM_S16(A.wet_right) };
    EnvState st, *saved = ram(state_addr);
    int nbytes = (int)round_up(A.count, 16);

    if (flags & A_INIT) {
        st.target[0] = A.target[0];
        st.target[1] = A.target[1];
        st.rate[0] = A.rate[0];
        st.rate[1] = A.rate[1];
        st.vol_dry = A.vol_dry;
        st.vol_wet = A.vol_wet;
        /* lane i starts (i + 1) / 8 of the way through the first step */
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 8; i++)
                st.vols[c][i] = clamp32(((s64)A.vol[c] << 16) + (s64)st.rate[c] * (i + 1) / 8);
    } else {
        __builtin_memcpy(&st, saved, sizeof st);
    }

    do {
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) {
                s32 v = st.vols[c][i];
                if (st.rate[c] >= 0) {               /* rising */
                    if ((v >> 16) > st.target[c])
                        v = st.target[c] << 16;
                } else if ((v >> 16) < st.target[c]) {
                    v = st.target[c] << 16;
                }
                st.vols[c][i] = v;
                dry[c][i] = clamp16((dry[c][i] * 0x7FFF +
                                     in[i] * (((v >> 16) * st.vol_dry + 0x4000) >> 15) + 0x4000) >> 15);
                if (flags & A_AUX)
                    wet[c][i] = clamp16((wet[c][i] * 0x7FFF +
                                         in[i] * (((v >> 16) * st.vol_wet + 0x4000) >> 15) + 0x4000) >> 15);
                st.vols[c][i] = clamp32((s64)v + st.rate[c]);
            }
            dry[c] += 8;
            if (flags & A_AUX)
                wet[c] += 8;
        }
        in += 8;
        nbytes -= 16;
    } while (nbytes > 0);

    __builtin_memcpy(saved, &st, sizeof st);
}

static void set_volume(u32 flags, s16 v, s16 t, s16 r)
{
    if (flags & A_AUX) {
        A.vol_dry = v;
        A.vol_wet = r;
    } else if (flags & A_VOL) {
        A.vol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        int c = (flags & A_LEFT) ? 0 : 1;
        A.target[c] = v;
        A.rate[c] = (s32)(((u32)(u16)t << 16) | (u16)r);
    }
}

static void mix(s16 gain, u16 in_addr, u16 out_addr)
{
    s16 *in = DMEM_S16(in_addr), *out = DMEM_S16(out_addr);
    int n = (int)round_up(A.count, 32) / 2;

    for (int i = 0; i < n; i++)
        out[i] = clamp16((out[i] * 0x7FFF + in[i] * gain) >> 15);
}

static void interleave(u16 left, u16 right)
{
    s16 *l = DMEM_S16(left), *r = DMEM_S16(right), *d = DMEM_S16(A.out);
    int n = (int)round_up(A.count, 16) / 2;
    s16 tmp[2 * DMEM_SIZE / 2];

    /* the output may overlap the inputs */
    if (n * 2 > (int)(sizeof tmp / sizeof tmp[0]))
        n = sizeof tmp / sizeof tmp[0] / 2;
    for (int i = 0; i < n; i++) {
        tmp[2 * i] = l[i];
        tmp[2 * i + 1] = r[i];
    }
    __builtin_memcpy(d, tmp, n * 2 * sizeof(s16));
}

static void pole_filter(u32 flags, s16 gain, u32 state_addr)
{
    s16 *src = DMEM_S16(A.in), *dst = DMEM_S16(A.out);
    s16 *state = ram(state_addr);
    const s16 *h1 = A.table[0];
    s16 h2[8], h2_before[8];
    int count = (int)round_up(A.count, 16);
    s16 l1, l2;

    if (flags & A_INIT) {
        l1 = l2 = 0;
    } else {
        l1 = state[2];
        l2 = state[3];
    }
    for (int i = 0; i < 8; i++) {
        h2_before[i] = A.table[0][8 + i];
        h2[i] = (s16)((A.table[0][8 + i] * gain) >> 14);
    }
    do {
        s16 frame[8];
        for (int i = 0; i < 8; i++)
            frame[i] = src[i];
        for (int i = 0; i < 8; i++) {
            s32 acc = frame[i] * gain + h1[i] * l1 + h2_before[i] * l2;
            for (int k = 0; k < i; k++)
                acc += h2[k] * frame[i - 1 - k];
            dst[i] = clamp16(acc >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        src += 8;
        dst += 8;
        count -= 16;
    } while (count > 0);
    __builtin_memcpy(state, dst - 4, 4 * sizeof(s16));
}

/* ---- Task ---------------------------------------------------------------- */

static void asp_task(OSTask *task)
{
    const u32 *cmd = (const u32 *)task->t.data_ptr;
    u32 n = task->t.data_size / 8;

    for (u32 i = 0; i < n; i++, cmd += 2) {
        u32 w0 = cmd[0], w1 = cmd[1];
        u32 flags = (w0 >> 16) & 0xFF;

        switch (w0 >> 24) {
        case A_SPNOOP:
            break;
        case A_ADPCM:
            adpcm(flags, w1);
            break;
        case A_CLEARBUFF:
            __builtin_memset(DMEM_U8(w0 & 0xFFFF), 0, round_up(w1 & 0xFFFF, 16));
            break;
        case A_ENVMIXER:
            envmixer(flags, w1);
            break;
        case A_LOADBUFF: {
            u32 bytes = round_up(A.count, 8);
            __builtin_memcpy(DMEM_U8(A.in), ram(w1), bytes);
            if (flags & 1) {        /* port flag: big-endian RAW16 samples (load.c) */
                u16 *h = (u16 *)DMEM_U8(A.in);
                for (u32 k = 0; k < bytes / 2; k++)
                    h[k] = (u16)((h[k] >> 8) | (h[k] << 8));
            }
            break;
        }
        case A_RESAMPLE:
            resample(flags, (u16)w0, w1);
            break;
        case A_SAVEBUFF:
            __builtin_memcpy(ram(w1), DMEM_U8(A.out), round_up(A.count, 8));
            break;
        case A_SEGMENT:
            A.seg[(w1 >> 24) & 0xF] = w1 & 0xFFFFFF;
            break;
        case A_SETBUFF:
            if (flags & A_AUX) {
                A.dry_right = (u16)w0;
                A.wet_left = (u16)(w1 >> 16);
                A.wet_right = (u16)w1;
            } else {
                A.in = (u16)w0;
                A.out = (u16)(w1 >> 16);
                A.count = (u16)w1;
            }
            break;
        case A_SETVOL:
            set_volume(flags, (s16)w0, (s16)(w1 >> 16), (s16)w1);
            break;
        case A_DMEMMOVE:
            /* exact count: load.c merges RAW16 sections with odd-sized moves */
            __builtin_memmove(DMEM_U8(w1 >> 16), DMEM_U8(w0 & 0xFFFF), w1 & 0xFFFF);
            break;
        case A_LOADADPCM: {
            u32 bytes = w0 & 0xFFFF;
            if (bytes > sizeof A.table)
                bytes = sizeof A.table;
            __builtin_memcpy(A.table, ram(w1), bytes);
            break;
        }
        case A_MIXER:
            mix((s16)w0, (u16)(w1 >> 16), (u16)w1);
            break;
        case A_INTERLEAVE:
            interleave((u16)(w1 >> 16), (u16)w1);
            break;
        case A_POLEF:
            pole_filter(flags, (s16)w0, w1);
            break;
        case A_SETLOOP:
            A.loop = w1;
            break;
        default: {
            static u32 seen;
            u32 op = w0 >> 24;
            if (op < 32 && !(seen & (1u << op))) {
                seen |= 1u << op;
                pal_log("asp: unhandled audio command %02lX", (unsigned long)op);
            }
            break;
        }
        }
    }
}

/* An audio task run on the calling thread (amPortRenderFrame, audi.c). */
void asp_run(OSTask *task)
{
    asp_task(task);
}

void asp_init(void)
{
    /* 4-tap interpolation weights (Catmull-Rom) between taps 1 and 2, for
     * 64 fractional positions, in Q15 like the microcode's table. */
    for (int i = 0; i < 64; i++) {
        float t = i / 64.0f, t2 = t * t, t3 = t2 * t;
        float w[4] = { (-t3 + 2 * t2 - t) * 0.5f, (3 * t3 - 5 * t2 + 2) * 0.5f,
                       (-3 * t3 + 4 * t2 + t) * 0.5f, (t3 - t2) * 0.5f };
        for (int k = 0; k < 4; k++) {
            float v = w[k] * 32767.0f;
            s_resample_table[i][k] = (s16)(v < 0 ? v - 0.5f : v + 0.5f);
        }
    }
    osPortSetTaskHandler(M_AUDTASK, asp_task);
}
