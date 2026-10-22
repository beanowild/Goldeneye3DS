/*
 * os_ai.c - libultra Audio Interface on pal_audio.
 *
 * The game's audio thread builds a frame of samples (through an audio
 * task) and hands it over with osAiSetNextBuffer. It uses osAiGetLength /
 * osAiGetStatus to judge how far ahead it is. pal_audio reports real DSP
 * progress, so that pacing works unchanged.
 *
 * OS_EVENT_AI isn't posted yet: ndsp gives no "buffer started" callback.
 * Games that pace with AI interrupts rather than by polling would need a
 * small watcher here.
 */
#include <ultra64.h>

#include "pal/pal_audio.h"
#include "ultra/os_ai.h"
#include "ultra/os_sp.h"

extern s32 dspsyn_buffer_time(const void *buf, u32 *time);   /* audio/dsp_synth.c */

/* The AI derives its DAC rate from the VI clock, so the real sample rate
 * is VI_NTSC_CLOCK / round(VI_NTSC_CLOCK / requested). Return that exact
 * value as libultra does; games size their buffers from it. */
#define VI_NTSC_CLOCK   48681812u
#define AI_MIN_DACRATE  132u

#define AI_STATUS_FIFO_FULL_BIT 0x80000000u
#define AI_STATUS_DMA_BUSY_BIT  0x40000000u

s32 osAiSetFrequency(u32 frequency)
{
    if (frequency == 0)
        return -1;
    u32 dacrate = (VI_NTSC_CLOCK + frequency / 2) / frequency;
    if (dacrate < AI_MIN_DACRATE)
        return -1;

    u32 actual = VI_NTSC_CLOCK / dacrate;
    pal_audio_set_rate(actual);
    return (s32)actual;
}

s32 osAiSetNextBuffer(void *bufPtr, u32 size)
{
    /* audio tasks report done as they start (os_sp.c): the buffer may
     * still be being mixed */
    osPortWaitAudioDone();
    /* -1 while both FIFO slots are full, exactly as on hardware; the game
     * retries next frame. */
    u32 time;
    if (dspsyn_buffer_time(bufPtr, &time))
        return pal_audio_submit_tagged(bufPtr, size, time) ? 0 : -1;
    return pal_audio_submit(bufPtr, size) ? 0 : -1;
}

u32 osAiGetLength(void)
{
    return pal_audio_remaining();
}

u32 osAiGetStatus(void)
{
    u32 status = 0;
    if (pal_audio_busy())
        status |= AI_STATUS_FIFO_FULL_BIT;
    if (pal_audio_playing())
        status |= AI_STATUS_DMA_BUSY_BIT;
    return status;
}
