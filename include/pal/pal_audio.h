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

#ifdef __cplusplus
}
#endif

#endif /* PAL_AUDIO_H */
