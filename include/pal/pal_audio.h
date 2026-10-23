/*
 * pal_audio.h - PCM output for the libultra AI shim (src/ultra/os_ai.c).
 *
 * Models the N64 Audio Interface: a FIFO of two DMA buffers, one playing
 * and one queued. pal_audio_busy() is the AI's "FIFO full" bit. GoldenEye
 * paces its audio thread on it and on pal_audio_remaining(), so both
 * report real playback progress.
 *
 * Samples are interleaved stereo signed 16-bit in *native* (little-endian)
 * order. The N64 AI read big-endian, but the buffers are filled by the
 * port's own audio-command executor, which writes native order.
 *
 * Not thread-safe: call from one thread (the game's audio thread).
 */
#ifndef PAL_AUDIO_H
#define PAL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start output. If the DSP can't start (most often a missing
 * sdmc:/3ds/dspfirm.cdc), playback is simulated against the system clock
 * so the game keeps correct timing, and this returns false. */
bool pal_audio_init(void);
void pal_audio_exit(void);

void pal_audio_set_rate(uint32_t hz);

/* Queue one buffer. Returns false, and queues nothing, if two buffers are
 * already pending (the AI would reject the DMA too). */
bool pal_audio_submit(const void *pcm, uint32_t bytes);

bool     pal_audio_busy(void);       /* two buffers pending */
bool     pal_audio_playing(void);    /* at least one buffer pending */
uint32_t pal_audio_remaining(void);  /* bytes left in the playing buffer */
uint32_t pal_audio_underruns(void);  /* times the DSP ran dry, since boot */

/*
 * Hardware voices (src/audio/dsp_synth.c): the game's synthesizer voices
 * played by DSP channels instead of being mixed on the CPU. The PCM stream
 * above keeps running alongside them (it carries the reverb return) and
 * is the clock: each buffer is submitted with the synthesizer sample time
 * of its first sample (pal_audio_submit_tagged), and voice events are
 * applied by the DSP frame callback when the stream reaches their time.
 *
 * Events are queued from one thread (the game's audio thread).
 */
#define PAL_VOICE_COUNT 23      /* DSP channels 1..23; 0 is the stream */

enum {
    PAL_VOICE_START,    /* pcm/len/loop, rate, gains (no ramp) */
    PAL_VOICE_STOP,
    PAL_VOICE_RATE,     /* rate */
    PAL_VOICE_GAIN,     /* ramp from the current gains to l/r over ramp samples */
};

typedef struct {
    uint32_t       time;        /* synthesizer sample time */
    uint8_t        voice, type;
    int16_t        loop_count;  /* 0: no loop, -1: forever, n: n extra passes */
    float          rate;        /* source samples per second */
    float          l, r;        /* dry gains, 1.0 = unity */
    uint32_t       ramp;        /* samples */
    const int16_t *pcm;         /* linear memory (pal_linear_alloc) */
    uint32_t       len, loop_start, loop_end;
} PalVoiceEvent;

bool pal_audio_voices_available(void);  /* DSP running */
/* Since the last call: DSP frames dropped, voice events applied untimed,
 * peak voice events queued, voice events lost (total), stream buffers whose
 * synthesizer time doesn't follow on from the last one. */
void pal_audio_stats(uint32_t out[5]);
void pal_audio_voice_event(const PalVoiceEvent *ev);
bool pal_audio_submit_tagged(const void *pcm, uint32_t bytes, uint32_t time);

#ifdef __cplusplus
}
#endif

#endif /* PAL_AUDIO_H */
