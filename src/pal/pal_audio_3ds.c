/*
 * pal_audio_3ds.c - pal_audio on ndsp, with a clock-driven fallback.
 */
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal_audio.h"
#include "pal/pal_sys.h"

#define CHANNEL     0
#define FIFO_DEPTH  2               /* N64 AI: playing + one queued (simulated path) */
#define DSP_DEPTH   6               /* buffers the DSP path accepts before "busy" */
#define NUM_BUFS    (DSP_DEPTH + 1) /* + one the DSP may still be retiring */
/*
 * The game sizes each audio frame as (frame size - osAiGetLength() + a
 * margin): on N64 that feedback keeps the AI's two-deep FIFO just full.
 * The DSP's buffer boundaries don't line up with the game's frames the
 * same way, and reporting only the playing buffer's remainder left the
 * game at its minimum frame size, ~2% short, so the DSP ran dry every
 * second or two (crackling). Instead the DSP path reports how much is
 * queued beyond TARGET_FRAMES, so the same formula holds the queue near
 * that level (~70 ms) and never underruns.
 */
#define TARGET_FRAMES 1500u
#define BUF_BYTES   16384u          /* 4096 stereo frames; GE uses ~2.5 KB */
#define FRAME_BYTES 4u              /* stereo s16 */

static bool     s_dsp;
static uint32_t s_rate = 32000;

/* ---- DSP path ------------------------------------------------------------ */

static ndspWaveBuf s_wb[NUM_BUFS];
static uint32_t    s_wb_time[NUM_BUFS];   /* synthesizer time of each buffer's first sample */
static bool        s_wb_tagged[NUM_BUFS];
static uint8_t    *s_mem;   /* NUM_BUFS * BUF_BYTES, linear memory for DMA */

static bool wb_pending(const ndspWaveBuf *wb)
{
    return wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING;
}

static int dsp_pending_count(void)
{
    int n = 0;
    for (int i = 0; i < NUM_BUFS; i++)
        n += wb_pending(&s_wb[i]);
    return n;
}

/* ---- Simulated path ------------------------------------------------------- */

/* Start/end tick of each pending buffer, oldest first. */
static uint64_t s_sim_start[FIFO_DEPTH], s_sim_end[FIFO_DEPTH];
static int      s_sim_count;

static void sim_expire(uint64_t now)
{
    while (s_sim_count > 0 && s_sim_end[0] <= now) {
        s_sim_start[0] = s_sim_start[1];
        s_sim_end[0] = s_sim_end[1];
        s_sim_count--;
    }
}

/* ---- Hardware voices -------------------------------------------------------
 *
 * Events go through a single-producer, single-consumer ring: the game's
 * audio thread writes, the ndsp thread's frame callback (once per 160 DSP
 * samples, before ndsp sends channel state to the DSP) reads. An event
 * fires once the stream's playback position reaches its time, so voices
 * stay in step with the stream (the reverb return) and with the game's
 * own audio clock, to within a DSP frame (~5 ms).
 */
#define RING_SIZE   1024u           /* power of two */
#define VOICE_BUFS  8               /* wave buffers per voice: attack, loop passes, tail */

typedef struct {
    float    cl, cr;                /* gains now */
    float    sl, sr, tl, tr;        /* ramp: from, to */
    uint32_t t0, dur;               /* ramp start time and length; dur 0 = idle */
    float    ml, mr;                /* gains last sent to ndsp */
    ndspWaveBuf wb[VOICE_BUFS];
} Voice;

static bool           s_polyphase;
static uint32_t       s_untimed, s_ring_peak;   /* stats (pal_audio_stats) */
static PalVoiceEvent  *s_ring;
static volatile uint32_t s_ring_head, s_ring_tail;   /* head: written, tail: read */
static uint32_t        s_ring_dropped;
static Voice          *s_voices;

static void voice_set_mix(int v, float l, float r)
{
    Voice *vo = &s_voices[v];
    if (l == vo->ml && r == vo->mr)
        return;
    float mix[12] = { 0 };
    mix[0] = l;
    mix[1] = r;
    ndspChnSetMix(v + 1, mix);
    vo->ml = l;
    vo->mr = r;
}

static void voice_start(int v, const PalVoiceEvent *ev)
{
    Voice *vo = &s_voices[v];
    int ch = v + 1, n = 0;

    ndspChnWaveBufClear(ch);
    if (!ev->pcm || ev->len == 0)
        return;
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
    /* linear: polyphase on many channels is more than the DSP can do in
     * a frame on hardware (Azahar doesn't model its cost); it drops frames
     * and the sound slows and loses events. -polyphase to compare. */
    ndspChnSetInterp(ch, s_polyphase ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, ev->rate);
    vo->cl = vo->tl = ev->l;
    vo->cr = vo->tr = ev->r;
    vo->dur = 0;
    voice_set_mix(v, ev->l, ev->r);

    memset(vo->wb, 0, sizeof vo->wb);
    uint32_t ls = ev->loop_start, le = ev->loop_end;
    if (ev->loop_count != 0 && le > ls && le <= ev->len) {
        /* the attack up to the loop end, then the loop: forever as one
         * looping buffer, or n more passes and then the tail */
        vo->wb[n].data_pcm16 = (s16 *)ev->pcm;
        vo->wb[n++].nsamples = le;
        if (ev->loop_count < 0 || ev->loop_count > VOICE_BUFS - 3) {
            vo->wb[n].data_pcm16 = (s16 *)ev->pcm + ls;
            vo->wb[n].nsamples = le - ls;
            vo->wb[n++].looping = true;
        } else {
            for (int k = 0; k < ev->loop_count; k++) {
                vo->wb[n].data_pcm16 = (s16 *)ev->pcm + ls;
                vo->wb[n++].nsamples = le - ls;
            }
            if (ev->len > le) {
                vo->wb[n].data_pcm16 = (s16 *)ev->pcm + le;
                vo->wb[n++].nsamples = ev->len - le;
            }
        }
    } else {
        vo->wb[n].data_pcm16 = (s16 *)ev->pcm;
        vo->wb[n++].nsamples = ev->len;
    }
    for (int k = 0; k < n; k++)
        ndspChnWaveBufAdd(ch, &vo->wb[k]);
}

static void voice_apply(const PalVoiceEvent *ev)
{
    int v = ev->voice;
    Voice *vo = &s_voices[v];

    switch (ev->type) {
    case PAL_VOICE_START:
        voice_start(v, ev);
        break;
    case PAL_VOICE_STOP:
        ndspChnWaveBufClear(v + 1);
        break;
    case PAL_VOICE_RATE:
        ndspChnSetRate(v + 1, ev->rate);
        break;
    case PAL_VOICE_GAIN:
        vo->sl = vo->cl;
        vo->sr = vo->cr;
        vo->tl = ev->l;
        vo->tr = ev->r;
        vo->t0 = ev->time;
        vo->dur = ev->ramp;
        if (ev->ramp == 0) {
            vo->cl = ev->l;
            vo->cr = ev->r;
            voice_set_mix(v, ev->l, ev->r);
        }
        break;
    }
}

/* The stream's playback position in synthesizer time; false when nothing
 * tagged is queued (events then fire at once). */
static bool stream_time(uint32_t *now)
{
    bool have = false;
    uint32_t first = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        const ndspWaveBuf *wb = &s_wb[i];
        if (!s_wb_tagged[i])
            continue;
        if (wb->status == NDSP_WBUF_PLAYING) {
            *now = s_wb_time[i] + ndspChnGetSamplePos(CHANNEL);
            return true;
        }
        if (wb->status == NDSP_WBUF_QUEUED &&
            (!have || (int32_t)(s_wb_time[i] - first) < 0)) {
            first = s_wb_time[i];
            have = true;
        }
    }
    *now = first;
    return have;
}

static void frame_callback(void *arg)
{
    (void)arg;
    uint32_t now;
    bool timed = stream_time(&now);

    uint32_t tail = s_ring_tail;
    while (tail != s_ring_head) {
        const PalVoiceEvent *ev = &s_ring[tail & (RING_SIZE - 1)];
        if (timed && (int32_t)(ev->time - now) > 0)
            break;
        if (!timed)
            s_untimed++;
        voice_apply(ev);
        tail++;
    }
    __atomic_store_n(&s_ring_tail, tail, __ATOMIC_RELEASE);

    for (int v = 0; v < PAL_VOICE_COUNT; v++) {
        Voice *vo = &s_voices[v];
        if (vo->dur == 0)
            continue;
        int32_t el = timed ? (int32_t)(now - vo->t0) : (int32_t)vo->dur;
        if (el < 0)
            continue;
        if ((uint32_t)el >= vo->dur) {
            vo->cl = vo->tl;
            vo->cr = vo->tr;
            vo->dur = 0;
        } else {
            float f = (float)el / (float)vo->dur;
            vo->cl = vo->sl + (vo->tl - vo->sl) * f;
            vo->cr = vo->sr + (vo->tr - vo->sr) * f;
        }
        voice_set_mix(v, vo->cl, vo->cr);
    }
}

static void voices_init(void)
{
    s_ring = malloc(RING_SIZE * sizeof *s_ring);
    s_voices = calloc(PAL_VOICE_COUNT, sizeof *s_voices);
    if (!s_ring || !s_voices) {
        free(s_ring);
        free(s_voices);
        s_ring = NULL;
        s_voices = NULL;
        return;
    }
    s_ring_head = s_ring_tail = 0;
    s_polyphase = pal_debug_has_token("-polyphase");
    for (int v = 0; v < PAL_VOICE_COUNT; v++) {
        ndspChnReset(v + 1);
        s_voices[v].ml = s_voices[v].mr = -1.0f;
        voice_set_mix(v, 0, 0);
    }
    ndspSetCallback(frame_callback, NULL);
}

static void voices_exit(void)
{
    if (!s_voices)
        return;
    ndspSetCallback(NULL, NULL);
    for (int v = 0; v < PAL_VOICE_COUNT; v++)
        ndspChnReset(v + 1);
    free(s_ring);
    free(s_voices);
    s_ring = NULL;
    s_voices = NULL;
}

void pal_audio_stats(uint32_t out[4])
{
    static uint32_t s_dropped0;
    uint32_t d = s_dsp ? ndspGetDroppedFrames() : 0;
    out[0] = d - s_dropped0;   /* DSP frames dropped since the last call */
    s_dropped0 = d;
    out[1] = s_untimed;        /* voice events applied with no stream to time them */
    out[2] = s_ring_peak;      /* most voice events waiting at once */
    out[3] = s_ring_dropped;   /* voice events lost to a full queue (total) */
    s_untimed = s_ring_peak = 0;
}

bool pal_audio_voices_available(void)
{
    return s_dsp && s_voices != NULL;
}

void pal_audio_voice_event(const PalVoiceEvent *ev)
{
    if (!s_voices || ev->voice >= PAL_VOICE_COUNT)
        return;
    uint32_t head = s_ring_head;
    if (head - __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
        if (s_ring_dropped++ == 0)
            pal_log("audio: voice event ring full; dropping events");
        return;
    }
    s_ring[head & (RING_SIZE - 1)] = *ev;
    __atomic_store_n(&s_ring_head, head + 1, __ATOMIC_RELEASE);
    uint32_t fill = head + 1 - s_ring_tail;
    if (fill > s_ring_peak)
        s_ring_peak = fill;
}

/* ---- API ------------------------------------------------------------------ */

bool pal_audio_init(void)
{
    if (R_FAILED(ndspInit())) {
        pal_log("audio: DSP unavailable (is sdmc:/3ds/dspfirm.cdc missing?); "
                "running muted");
        s_dsp = false;
        return false;
    }

    s_mem = linearAlloc(NUM_BUFS * BUF_BYTES);
    if (!s_mem) {
        ndspExit();
        pal_log("audio: out of linear memory; running muted");
        s_dsp = false;
        return false;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CHANNEL);
    ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(CHANNEL, (float)s_rate);
    ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = { 0 };
    mix[0] = 1.0f;   /* front left  */
    mix[1] = 1.0f;   /* front right */
    ndspChnSetMix(CHANNEL, mix);

    memset(s_wb, 0, sizeof(s_wb));
    s_dsp = true;
    voices_init();
    return true;
}

void pal_audio_exit(void)
{
    if (!s_dsp)
        return;
    /* Must run before the app exits, or the DSP can hang the return to the
     * Homebrew Launcher. */
    voices_exit();
    ndspChnReset(CHANNEL);
    ndspExit();
    linearFree(s_mem);
    s_mem = NULL;
    s_dsp = false;
}

void pal_audio_set_rate(uint32_t hz)
{
    if (hz == 0)
        return;
    s_rate = hz;
    if (s_dsp)
        ndspChnSetRate(CHANNEL, (float)hz);
}

bool pal_audio_busy(void)
{
    if (s_dsp)
        return dsp_pending_count() >= DSP_DEPTH;
    sim_expire(pal_ticks());
    return s_sim_count >= FIFO_DEPTH;
}

bool pal_audio_playing(void)
{
    if (s_dsp)
        return dsp_pending_count() > 0;
    sim_expire(pal_ticks());
    return s_sim_count > 0;
}

static uint32_t s_underruns;   /* submits that found the DSP with nothing queued */
static bool     s_started;

uint32_t pal_audio_underruns(void)
{
    return s_underruns;
}

static bool submit(const void *pcm, uint32_t bytes, uint32_t time, bool tagged)
{
    if (pal_audio_busy())
        return false;

    bytes &= ~(FRAME_BYTES - 1);
    if (bytes > BUF_BYTES) {
        pal_log("audio: %lu-byte buffer truncated to %u",
                (unsigned long)bytes, BUF_BYTES);
        bytes = BUF_BYTES;
    }
    if (bytes == 0)
        return true;

    if (!s_dsp) {
        uint64_t now = pal_ticks();
        uint64_t start = s_sim_count ? s_sim_end[s_sim_count - 1] : now;
        uint64_t dur = (uint64_t)(bytes / FRAME_BYTES) * PAL_TICKS_PER_SEC / s_rate;
        s_sim_start[s_sim_count] = start;
        s_sim_end[s_sim_count] = start + dur;
        s_sim_count++;
        return true;
    }

    if (s_started && dsp_pending_count() == 0)
        s_underruns++;
    s_started = true;
    for (int i = 0; i < NUM_BUFS; i++) {
        ndspWaveBuf *wb = &s_wb[i];
        if (wb_pending(wb))
            continue;

        uint8_t *dst = s_mem + (size_t)i * BUF_BYTES;
        memcpy(dst, pcm, bytes);
        DSP_FlushDataCache(dst, bytes);

        memset(wb, 0, sizeof(*wb));
        s_wb_time[i] = time;
        s_wb_tagged[i] = tagged;
        wb->data_vaddr = dst;
        wb->nsamples = bytes / FRAME_BYTES;
        ndspChnWaveBufAdd(CHANNEL, wb);
        return true;
    }
    return false;   /* unreachable: busy() guarantees a free buffer */
}

bool pal_audio_submit(const void *pcm, uint32_t bytes)
{
    return submit(pcm, bytes, 0, false);
}

bool pal_audio_submit_tagged(const void *pcm, uint32_t bytes, uint32_t time)
{
    return submit(pcm, bytes, time, true);
}

uint32_t pal_audio_remaining(void)
{
    if (!s_dsp) {
        uint64_t now = pal_ticks();
        sim_expire(now);
        if (s_sim_count == 0 || s_sim_start[0] > now)
            return 0;
        uint64_t frames = (s_sim_end[0] - now) * s_rate / PAL_TICKS_PER_SEC;
        return (uint32_t)(frames * FRAME_BYTES);
    }

    uint32_t queued = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        const ndspWaveBuf *wb = &s_wb[i];
        if (wb->status == NDSP_WBUF_PLAYING) {
            u32 pos = ndspChnGetSamplePos(CHANNEL);
            queued += pos < wb->nsamples ? wb->nsamples - pos : 0;
        } else if (wb->status == NDSP_WBUF_QUEUED) {
            queued += wb->nsamples;
        }
    }
    return queued > TARGET_FRAMES ? (queued - TARGET_FRAMES) * FRAME_BYTES : 0;
}
