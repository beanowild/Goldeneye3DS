/*
 * pal_audio_3ds.c - pal_audio on ndsp, with a clock-driven fallback.
 */
#include <3ds.h>
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
    return true;
}

void pal_audio_exit(void)
{
    if (!s_dsp)
        return;
    /* Must run before the app exits, or the DSP can hang the return to the
     * Homebrew Launcher. */
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

bool pal_audio_submit(const void *pcm, uint32_t bytes)
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
        wb->data_vaddr = dst;
        wb->nsamples = bytes / FRAME_BYTES;
        ndspChnWaveBufAdd(CHANNEL, wb);
        return true;
    }
    return false;   /* unreachable: busy() guarantees a free buffer */
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
