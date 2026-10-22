/*
 * dsp_synth.c - the game's synthesizer voices on DSP channels.
 *
 * On the N64 every voice is decoded, resampled, enveloped and mixed by the
 * RSP (src/audio/asp.c does that on the CPU here, 7-14 ms of core time per
 * frame on an Old 3DS). The 3DS DSP has 24 channels that resample and mix
 * PCM in hardware, so here alAudioFrame (synthesizer.c) skips building the
 * command list and instead:
 *
 *  - walks each voice's parameter updates (the events the envmixer would
 *    have consumed: start, stop, pitch, volume/pan/fx ramps, free) exactly
 *    as env.c orders them, and turns them into timed pal_audio voice events
 *    (src/pal/pal_audio_3ds.c plays them on the DSP at their sample time);
 *  - decodes each wavetable (VADPCM or big-endian RAW16) to PCM16 in linear
 *    memory the first time it plays, kept for the session (the banks are
 *    ~1.2 MB of ROM, ~4 MB decoded at most);
 *  - mixes only the reverb send (the aux bus) on the CPU, runs the N64
 *    reverb (reverb.c, the game's custom 6-section setting) on it, and
 *    writes the return into the frame's output buffer. That buffer still
 *    goes out through osAiSetNextBuffer as the PCM stream, which paces the
 *    audio thread as before and is the clock the voice events follow.
 *
 * Voices 0..22 are DSP channels 1..23 (0 is the stream). The synthesizer
 * has 24; the last one is mixed dry on the CPU into the stream as well.
 *
 * Differences from the RSP: envelopes ramp per DSP frame (~5 ms) rather
 * than per 8 samples; resampling is the DSP's polyphase filter rather than
 * the microcode's 4-tap table; ADPCM loops decode from the running state
 * rather than the stored loop state (they are the same for sane banks).
 */
#include <ultra64.h>
#include <PR/abi.h>
#include "audio/synthInternals.h"

#include "pal/pal_audio.h"
#include "pal/pal_sys.h"

extern const void *osPortRomPointer(u32 devAddr, u32 size);
extern void *calloc(unsigned int n, unsigned int size);

#define MAX_PVOICES 32
#define CPU_VOICE   PAL_VOICE_COUNT     /* the one voice the DSP has no channel for */
#define CHUNK       AL_MAX_RSP_SAMPLES  /* reverb block, as the N64 */

s32 g_PortDspSynth;     /* alAudioFrame takes the DSP path */

/* ---- Decoded wavetables ---------------------------------------------------- */

typedef struct {
    u32  base, len;
    s16 *pcm;
    u32  n;
} Wave;

#define WAVE_SLOTS 2048     /* power of two; GE's banks have well under 1000 */
static Wave *s_waves;
static u32   s_wave_bytes;

static void decode_adpcm(const u8 *in, u32 frames, const ALADPCMBook *book, s16 *out)
{
    s32 prev1 = 0, prev2 = 0;
    s32 npred = book->npredictors;

    for (u32 f = 0; f < frames; f++) {
        s32 shift = *in >> 4, pred = *in++ & 0xF;
        const s16 *tbl = book->book + (pred < npred ? pred : 0) * 16;
        for (int half = 0; half < 2; half++) {
            s32 ins[8];
            for (int j = 0; j < 4; j++) {
                u8 b = *in++;
                ins[j * 2] = (s32)((u32)(b >> 4) << 28) >> 28 << shift;
                ins[j * 2 + 1] = (s32)((u32)(b & 0xF) << 28) >> 28 << shift;
            }
            for (int j = 0; j < 8; j++) {
                s32 acc = tbl[j] * prev2 + tbl[8 + j] * prev1 + (ins[j] << 11);
                for (int k = 0; k < j; k++)
                    acc += tbl[8 + (j - k) - 1] * ins[k];
                acc >>= 11;
                out[j] = (s16)(acc < -0x8000 ? -0x8000 : acc > 0x7FFF ? 0x7FFF : acc);
            }
            prev2 = out[6];
            prev1 = out[7];
            out += 8;
        }
    }
}

static const Wave *wave_get(ALWaveTable *w)
{
    u32 base = (u32)(uintptr_t)w->base, len = (u32)w->len;

    if (!s_waves) {
        s_waves = calloc(WAVE_SLOTS, sizeof *s_waves);
        if (!s_waves)
            return NULL;
    }
    u32 h = (base * 2654435761u ^ len) & (WAVE_SLOTS - 1);
    for (u32 probe = 0; probe < WAVE_SLOTS; probe++, h = (h + 1) & (WAVE_SLOTS - 1)) {
        Wave *e = &s_waves[h];
        if (e->pcm && e->base == base && e->len == len)
            return e;
        if (e->pcm)
            continue;

        const u8 *src = osPortRomPointer(base, len);
        u32 n;
        if (!src)
            return NULL;
        if (w->type == AL_ADPCM_WAVE)
            n = len / 9 * 16;
        else
            n = len / 2;
        if (n == 0)
            return NULL;
        s16 *pcm = pal_linear_alloc(n * sizeof(s16), 16);
        if (!pcm) {
            pal_log("dsp_synth: no linear memory for a %lu-sample wave (%lu KB decoded)",
                    (unsigned long)n, (unsigned long)(s_wave_bytes / 1024));
            return NULL;
        }
        if (w->type == AL_ADPCM_WAVE) {
            decode_adpcm(src, len / 9, w->waveInfo.adpcmWave.book, pcm);
        } else {
            for (u32 i = 0; i < n; i++)
                pcm[i] = (s16)((src[i * 2] << 8) | src[i * 2 + 1]);
        }
        pal_flush_dcache(pcm, n * sizeof(s16));
        s_wave_bytes += n * sizeof(s16);
        e->base = base;
        e->len = len;
        e->n = n;
        e->pcm = pcm;
        return e;
    }
    return NULL;
}

/* ---- Voices ---------------------------------------------------------------- */

#define EQPOWER_LENGTH 128
static const s16 eqpower[EQPOWER_LENGTH] = {   /* env.c */
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644,
    32607, 32564, 32517, 32464, 32407, 32344, 32277, 32205,
    32127, 32045, 31958, 31866, 31770, 31668, 31561, 31450,
    31334, 31213, 31087, 30957, 30822, 30682, 30537, 30388,
    30234, 30075, 29912, 29744, 29572, 29395, 29214, 29028,
    28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385,
    27160, 26931, 26698, 26461, 26220, 25975, 25726, 25473,
    25216, 24956, 24691, 24423, 24151, 23875, 23596, 23313,
    23026, 22736, 22442, 22145, 21845, 21541, 21234, 20924,
    20610, 20294, 19974, 19651, 19325, 18997, 18665, 18331,
    17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558,
    15200, 14840, 14477, 14113, 13746, 13377, 13006, 12633,
    12258, 11881, 11503, 11122, 10740, 10357,  9971,  9584,
     9196,  8806,  8415,  8023,  7630,  7235,  6839,  6442,
     6044,  5646,  5246,  4845,  4444,  4042,  3640,  3237,
     2833,  2429,  2025,  1620,  1216,   810,   405,     0,
};

typedef struct {
    /* envelope (env.c): cvol per side in N64 units, a linear ramp */
    f32 cv[2], sv[2], tv[2];
    u32 t0, dur;
    s16 volume, pan;
    s32 segEnd;
    f32 dry, wet;               /* 0..1 */
    /* sample */
    const Wave *wave;
    u32 ls, le;
    s32 count;                  /* loop passes left; -1 forever, 0 no loop */
    u32 pos, frac, step;        /* 16.16 */
    f32 ratio;
    u8  unity, playing;
} SVoice;

static PVoice *s_pvoices;
static s32     s_npvoices;
#define FRAME_MAX (CHUNK * 8)
/* on the heap: the executable's statics must end below 16 MB */
static SVoice *s_sv;
static s32    *s_mono;              /* reverb send for one frame */
static s32    *s_dry[2];            /* the CPU voice's dry output */

#define EVQ_SIZE 256
static PalVoiceEvent *s_evq;
static s32            s_evn;

typedef struct { s16 *buf; u32 time; } Tag;
static Tag s_tags[8];
static u32 s_tag_next;

void dspsyn_register(PVoice *pvoices, s32 n)
{
    s_npvoices = n < MAX_PVOICES ? n : MAX_PVOICES;
    s_sv = calloc(MAX_PVOICES, sizeof *s_sv);
    s_mono = calloc(3 * FRAME_MAX, sizeof *s_mono);
    s_evq = calloc(EVQ_SIZE, sizeof *s_evq);
    if (!s_sv || !s_mono || !s_evq)
        return;
    s_dry[0] = s_mono + FRAME_MAX;
    s_dry[1] = s_mono + 2 * FRAME_MAX;
    s_pvoices = pvoices;
}

/* The synthesizer time of a frame buffer's first sample (os_ai.c). */
s32 dspsyn_buffer_time(const void *buf, u32 *time)
{
    for (int i = 0; i < 8; i++) {
        if (s_tags[i].buf == buf) {
            *time = s_tags[i].time;
            return 1;
        }
    }
    return 0;
}

static f32 env_at(const SVoice *v, int c, u32 t)
{
    s32 el = (s32)(t - v->t0);
    if (v->dur == 0 || el >= (s32)v->dur)
        return v->tv[c];
    if (el <= 0)
        return v->sv[c];
    return v->sv[c] + (v->tv[c] - v->sv[c]) * ((f32)el / (f32)v->dur);
}

static f32 target_side(const SVoice *v, int c)
{
    s32 p = c == 0 ? v->pan : EQPOWER_LENGTH - v->pan - 1;
    if (p < 0) p = 0;
    if (p >= EQPOWER_LENGTH) p = EQPOWER_LENGTH - 1;
    return (f32)((v->volume * eqpower[p]) >> 15);
}

/* ramp cvol from its value at t to the current target over n samples */
static void env_ramp(SVoice *v, u32 t, u32 n)
{
    for (int c = 0; c < 2; c++) {
        v->sv[c] = env_at(v, c, t);
        v->tv[c] = target_side(v, c);
        v->cv[c] = v->sv[c];
    }
    v->t0 = t;
    v->dur = n;
}

static f32 rate_of(const SVoice *v, const ALSynth *drvr)
{
    if (v->unity)
        return (f32)drvr->outputRate;
    return v->ratio * (f32)drvr->outputRate;
}

static void set_ratio(SVoice *v, f32 ratio)
{
    if (ratio > MAX_RATIO)
        ratio = MAX_RATIO;
    if (ratio < 0)
        ratio = 0;
    ratio = (f32)(s32)(ratio * UNITY_PITCH) / UNITY_PITCH;
    v->ratio = ratio;
    v->step = (u32)(ratio * 65536.0f);
}

#define GAIN_SCALE (1.0f / (32768.0f))

static void send(PalVoiceEvent *ev);

static void send_gain(int i, SVoice *v, u32 t)
{
    PalVoiceEvent ev = { 0 };
    u32 end = v->t0 + v->dur;
    ev.time = t;
    ev.voice = (u8)i;
    ev.type = PAL_VOICE_GAIN;
    ev.l = v->tv[0] * GAIN_SCALE * v->dry;
    ev.r = v->tv[1] * GAIN_SCALE * v->dry;
    ev.ramp = (v->dur && (s32)(end - t) > 0) ? end - t : 0;
    send(&ev);
}

static void voice_start(int i, SVoice *v, ALWaveTable *w, const ALSynth *drvr, u32 t)
{
    PalVoiceEvent ev = { 0 };

    v->wave = w ? wave_get(w) : NULL;
    v->pos = v->frac = 0;
    v->count = 0;
    v->ls = v->le = 0;
    if (v->wave) {
        if (w->type == AL_ADPCM_WAVE && w->waveInfo.adpcmWave.loop) {
            v->ls = w->waveInfo.adpcmWave.loop->start;
            v->le = w->waveInfo.adpcmWave.loop->end;
            v->count = (s32)w->waveInfo.adpcmWave.loop->count;
        } else if (w->type == AL_RAW16_WAVE && w->waveInfo.rawWave.loop) {
            v->ls = w->waveInfo.rawWave.loop->start;
            v->le = w->waveInfo.rawWave.loop->end;
            v->count = (s32)w->waveInfo.rawWave.loop->count;
        }
        if (v->le > v->wave->n || v->le <= v->ls)
            v->count = 0;
    }
    v->playing = v->wave != NULL;

    ev.time = t;
    ev.voice = (u8)i;
    ev.type = PAL_VOICE_START;
    ev.rate = rate_of(v, drvr);
    ev.l = env_at(v, 0, t) * GAIN_SCALE * v->dry;
    ev.r = env_at(v, 1, t) * GAIN_SCALE * v->dry;
    if (v->wave) {
        ev.pcm = v->wave->pcm;
        ev.len = v->wave->n;
        ev.loop_start = v->ls;
        ev.loop_end = v->le;
        ev.loop_count = (s16)(v->count < 0 ? -1 : v->count > 0x7FFF ? 0x7FFF : v->count);
    }
    send(&ev);
    if (v->dur)
        send_gain(i, v, t);
}

static void voice_event(ALSynth *drvr, int i, SVoice *v, ALParam *p, u32 t)
{
    PalVoiceEvent ev = { 0 };

    switch (p->type) {
    case AL_FILTER_START_VOICE_ALT: {
        ALStartParamAlt *sp = (ALStartParamAlt *)p;
        if (sp->unity)
            v->unity = 1;
        v->volume = (s16)(((s32)sp->volume * (s32)sp->volume) >> 15);
        v->pan = sp->pan;
        v->dry = eqpower[sp->fxMix & 0x7F] / 32768.0f;
        v->wet = eqpower[EQPOWER_LENGTH - (sp->fxMix & 0x7F) - 1] / 32768.0f;
        v->segEnd = sp->samples;
        set_ratio(v, sp->pitch);
        if (sp->samples) {
            v->sv[0] = v->sv[1] = 1;
            v->dur = 0;
            v->tv[0] = v->tv[1] = 1;
            env_ramp(v, t, (u32)sp->samples);
        } else {
            v->tv[0] = target_side(v, 0);
            v->tv[1] = target_side(v, 1);
            v->dur = 0;
        }
        voice_start(i, v, sp->wave, drvr, t);
        break;
    }
    case AL_FILTER_START_VOICE: {
        ALStartParam *sp = (ALStartParam *)p;
        if (sp->unity)
            v->unity = 1;
        voice_start(i, v, sp->wave, drvr, t);
        break;
    }
    case AL_FILTER_SET_VOLUME:
        v->volume = (s16)((p->data.i * p->data.i) >> 15);
        v->segEnd = p->moredata.i;
        env_ramp(v, t, (u32)v->segEnd);
        send_gain(i, v, t);
        break;
    case AL_FILTER_SET_PAN:
        v->pan = (s16)p->data.i;
        env_ramp(v, t, (u32)v->segEnd);
        send_gain(i, v, t);
        break;
    case AL_FILTER_SET_FXAMT:
        v->dry = eqpower[p->data.i & 0x7F] / 32768.0f;
        v->wet = eqpower[EQPOWER_LENGTH - (p->data.i & 0x7F) - 1] / 32768.0f;
        send_gain(i, v, t);
        break;
    case AL_FILTER_STOP_VOICE:
        v->playing = 0;
        v->unity = 0;
        v->volume = 1;
        ev.time = t;
        ev.voice = (u8)i;
        ev.type = PAL_VOICE_STOP;
        send(&ev);
        break;
    case AL_FILTER_FREE_VOICE: {
        ALFreeParam *fp = (ALFreeParam *)p;
        fp->pvoice->offset = 0;
        _freePVoice(drvr, (PVoice *)fp->pvoice);
        break;
    }
    case AL_FILTER_SET_PITCH: {
        union { s32 i; f32 f; } d;
        d.i = p->data.i;
        set_ratio(v, d.f);
        ev.time = t;
        ev.voice = (u8)i;
        ev.type = PAL_VOICE_RATE;
        ev.rate = rate_of(v, drvr);
        send(&ev);
        break;
    }
    case AL_FILTER_SET_UNITY_PITCH:
        v->unity = 1;
        break;
    default:
        break;
    }
}

/* Advance a voice over [t, t + n) output samples, adding its reverb send
 * (and, for the CPU voice, its dry output) at offset off of the frame. */
static void voice_run(int i, SVoice *v, u32 t, s32 off, s32 n)
{
    if (!v->playing || n <= 0)
        return;
    const s16 *pcm = v->wave->pcm;
    u32 len = v->wave->n;
    u32 step = v->unity ? 0x10000 : v->step;
    int dry = i == CPU_VOICE;
    int wet = v->wet > 0.0f;

    for (s32 k = 0; k < n; k += 16) {
        s32 m = n - k < 16 ? n - k : 16;
        u32 tm = t + k + m / 2;
        f32 el = env_at(v, 0, tm) * GAIN_SCALE, er = env_at(v, 1, tm) * GAIN_SCALE;
        /* the N64 sums the aux pair at 0.707 each into the reverb input */
        s32 gw = wet ? (s32)((el + er) * v->wet * 0.70710678f * 4096.0f) : 0;
        s32 gl = dry ? (s32)(el * v->dry * 4096.0f) : 0;
        s32 gr = dry ? (s32)(er * v->dry * 4096.0f) : 0;
        for (s32 j = 0; j < m; j++) {
            u32 p1 = v->pos + 1;
            if (v->count != 0 && p1 >= v->le)
                p1 = v->ls;
            s32 a = pcm[v->pos], b = p1 < len ? pcm[p1] : 0;
            s32 s = a + (((b - a) * (s32)(v->frac >> 1)) >> 15);
            s32 o = off + k + j;
            s_mono[o] += (s * gw) >> 12;
            if (dry) {
                s_dry[0][o] += (s * gl) >> 12;
                s_dry[1][o] += (s * gr) >> 12;
            }
            v->frac += step;
            v->pos += v->frac >> 16;
            v->frac &= 0xFFFF;
            while (v->count != 0 && v->pos >= v->le) {
                v->pos -= v->le - v->ls;
                if (v->count > 0)
                    v->count--;
            }
            if (v->pos >= len) {
                v->playing = 0;
                return;
            }
        }
    }
}

/* ---- Event queue (sorted per frame) ---------------------------------------- */


static void send(PalVoiceEvent *ev)
{
    if (ev->voice >= PAL_VOICE_COUNT)
        return;     /* the CPU voice */
    if (s_evn == EVQ_SIZE) {
        pal_audio_voice_event(&s_evq[0]);   /* full: flush the oldest */
        __builtin_memmove(s_evq, s_evq + 1, (s_evn - 1) * sizeof s_evq[0]);
        s_evn--;
    }
    s_evq[s_evn++] = *ev;
}

static void flush_events(void)
{
    /* stable insertion sort by time: voices are walked one at a time */
    for (s32 i = 1; i < s_evn; i++) {
        PalVoiceEvent e = s_evq[i];
        s32 j = i - 1;
        while (j >= 0 && (s32)(s_evq[j].time - e.time) > 0) {
            s_evq[j + 1] = s_evq[j];
            j--;
        }
        s_evq[j + 1] = e;
    }
    for (s32 i = 0; i < s_evn; i++)
        pal_audio_voice_event(&s_evq[i]);
    s_evn = 0;
}

/* ---- Reverb (reverb.c alFxPull, on arrays) --------------------------------- */

static inline s16 clamp16(s32 v)
{
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : (s16)v;
}

static inline s16 mixs(s16 out, s16 in, s16 gain)
{
    return clamp16((out * 0x7FFF + in * gain) >> 15);
}

static s32 wrap(const ALFx *r, s32 idx)
{
    while (idx < 0)
        idx += (s32)r->length;
    while (idx >= (s32)r->length)
        idx -= (s32)r->length;
    return idx;
}

static void load(const ALFx *r, s32 idx, s16 *dst, s32 n)
{
    idx = wrap(r, idx);
    for (s32 i = 0; i < n; i++) {
        dst[i] = r->base[idx];
        if (++idx == (s32)r->length)
            idx = 0;
    }
}

static void save(ALFx *r, s32 idx, const s16 *src, s32 n)
{
    idx = wrap(r, idx);
    for (s32 i = 0; i < n; i++) {
        r->base[idx] = src[i];
        if (++idx == (s32)r->length)
            idx = 0;
    }
}

static f32 mod_func(ALDelay *d, s32 count)
{
    f32 val;
    d->rsval += d->rsinc * count;
    d->rsval = (d->rsval > 2.0f) ? d->rsval - 4.0f : d->rsval;
    val = d->rsval;
    val = (val < 0) ? -val : val;
    val -= 1.0f;
    return d->rsgain * val;
}

static void fx_chunk(ALFx *r, const s16 *in, s16 *out, s32 n)
{
    static s16 b1[CHUNK + 4], b2[CHUNK + 4], tmp[2 * CHUNK + 8];
    s16 *buff1 = b1, *buff2 = b2;
    s32 cur = (s32)(r->input - r->base);
    s32 prev_out = -0x7FFFFFFF;

    save(r, cur, in, n);
    for (s32 i = 0; i < n; i++)
        out[i] = 0;

    for (s32 s = 0; s < r->section_count; s++) {
        ALDelay *d = &r->delay[s];
        s32 in_idx = cur - (s32)d->input;
        s32 out_idx = cur - (s32)d->output;

        if (in_idx == prev_out) {
            s16 *t = buff1; buff1 = buff2; buff2 = t;
        } else {
            load(r, in_idx, buff1, n);
        }
        if (d->rs) {
            s32 length = (s32)d->output - (s32)d->input;
            f32 delta = mod_func(d, n) / length;
            delta = (f32)(s32)(delta * UNITY_PITCH) / UNITY_PITCH;
            f32 fratio = 1.0f - delta;
            f32 fincount = d->rs->delta + fratio * (f32)n;
            s32 count = (s32)fincount;
            d->rs->delta = fincount - (f32)count;
            s32 src = cur - ((s32)d->output - d->rsdelta);
            if (count + 2 > (s32)(sizeof tmp / sizeof tmp[0]))
                count = sizeof tmp / sizeof tmp[0] - 2;
            load(r, src, tmp, count + 2);
            u32 acc = 0, inc = (u32)(fratio * 65536.0f);
            for (s32 i = 0; i < n; i++) {
                u32 p = acc >> 16, f = acc & 0xFFFF;
                buff2[i] = (s16)(tmp[p] + (((tmp[p + 1] - tmp[p]) * (s32)(f >> 1)) >> 15));
                acc += inc;
            }
            d->rsdelta += count - n;
        } else {
            load(r, out_idx, buff2, n);
        }
        if (d->ffcoef) {
            for (s32 i = 0; i < n; i++)
                buff2[i] = mixs(buff2[i], buff1[i], d->ffcoef);
            if (!d->rs && !d->lp)
                save(r, out_idx, buff2, n);
        }
        if (d->fbcoef) {
            for (s32 i = 0; i < n; i++)
                buff1[i] = mixs(buff1[i], buff2[i], d->fbcoef);
            save(r, in_idx, buff1, n);
        }
        if (d->lp) {
            ALLowPass *lp = d->lp;
            s32 y = lp->first ? 0 : lp->fstate[0][0];
            s32 fc = lp->fcvec.fccoef[8];
            for (s32 i = 0; i < n; i++) {
                y = clamp16((buff2[i] * lp->fgain + fc * y) >> 14);
                buff2[i] = (s16)y;
            }
            lp->fstate[0][0] = (s16)y;
            lp->first = 0;
        }
        if (!d->rs)
            save(r, out_idx, buff2, n);
        if (d->gain)
            for (s32 i = 0; i < n; i++)
                out[i] = mixs(out[i], buff2[i], d->gain);
        prev_out = cur + (s32)d->output;    /* as reverb.c: never matches */
    }
    cur += n;
    if (cur >= (s32)r->length)
        cur -= (s32)r->length;
    r->input = r->base + cur;
}

/* ---- Frame ----------------------------------------------------------------- */

void dspsyn_frame(ALSynth *drvr, s16 *outBuf, s32 outLen)
{
    u32 start = (u32)drvr->curSamples, end = start + (u32)outLen;
    ALFx *fx = drvr->auxBus && drvr->auxBus->fx[0].base ? &drvr->auxBus->fx[0] : NULL;

    if (outLen > FRAME_MAX)
        outLen = FRAME_MAX;
    __builtin_memset(s_mono, 0, 3 * FRAME_MAX * sizeof s_mono[0]);

    for (s32 i = 0; i < s_npvoices; i++) {
        ALEnvMixer *e = &s_pvoices[i].envmixer;
        SVoice *v = &s_sv[i];
        u32 t = start;

        while (e->ctrlList != 0 && (s32)(e->ctrlList->delta - end) <= 0) {
            ALParam *p = e->ctrlList;
            u32 te = (u32)p->delta;
            if ((s32)(te - t) < 0)
                te = t;
            voice_run(i, v, t, (s32)(t - start), (s32)(te - t));
            t = te;
            voice_event(drvr, i, v, p, te);
            e->ctrlList = p->next;
            if (e->ctrlList == 0)
                e->ctrlTail = 0;
            __freeParam(p);
        }
        voice_run(i, v, t, (s32)(t - start), (s32)(end - t));
    }
    flush_events();

    /* reverb return to both sides, plus the CPU voice */
    for (s32 o = 0; o < outLen; o += CHUNK) {
        s32 n = outLen - o < CHUNK ? outLen - o : CHUNK;
        static s16 in[CHUNK], ret[CHUNK];
        for (s32 i = 0; i < n; i++)
            in[i] = clamp16(s_mono[o + i]);
        if (fx)
            fx_chunk(fx, in, ret, n);
        else
            __builtin_memset(ret, 0, n * sizeof ret[0]);
        for (s32 i = 0; i < n; i++) {
            outBuf[(o + i) * 2] = clamp16(ret[i] + s_dry[0][o + i]);
            outBuf[(o + i) * 2 + 1] = clamp16(ret[i] + s_dry[1][o + i]);
        }
    }

    s_tags[s_tag_next & 7].buf = outBuf;
    s_tags[s_tag_next & 7].time = start;
    s_tag_next++;
    drvr->curSamples += outLen;
}

s32 dspsyn_enable(void)
{
    char tok[128];
    /* developer token -cpuaudio: the RSP path (asp.c), for comparisons */
    if (pal_debug_read_text("sdmc:/3ds/ge007/tokens.txt", tok, sizeof tok) > 0 &&
        __builtin_strstr(tok, "-cpuaudio") != NULL) {
        pal_log("dsp_synth: off (-cpuaudio)");
        return 0;
    }
    if (s_pvoices && pal_audio_voices_available()) {
        g_PortDspSynth = 1;
        pal_log("dsp_synth: %ld voices on the DSP", (long)s_npvoices);
    }
    return g_PortDspSynth;
}
