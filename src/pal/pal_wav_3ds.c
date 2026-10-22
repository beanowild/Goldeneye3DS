/*
 * pal_wav_3ds.c - WAV files on the SD card, for music_stems.c (which
 * includes the game's <ultra64.h> and so can't have <stdio.h>).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void stems_mkdir(const char *dir)
{
    mkdir(dir, 0777);
}

/* A 16-bit stereo WAV named "<dir>/<name>.wav"; the header is written on
 * close, once the length is known. */
void *stems_wav_open(const char *dir, const char *name)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t zero[44];
        fwrite(zero, 1, sizeof zero, f);
    }
    return f;
}

void stems_wav_write(void *f, const int16_t *pairs, uint32_t count)
{
    fwrite(pairs, 4, count, (FILE *)f);
}

void stems_wav_close(void *fp, uint32_t rate, uint32_t pairs)
{
    FILE *f = fp;
    uint8_t h[44];
    uint32_t data = pairs * 4, v;
    memcpy(h, "RIFF", 4);
    v = 36 + data;          memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;                 memcpy(h + 16, &v, 4);
    v = 1 | (2u << 16);     memcpy(h + 20, &v, 4);    /* PCM, stereo */
    v = rate;               memcpy(h + 24, &v, 4);
    v = rate * 4;           memcpy(h + 28, &v, 4);
    v = 4 | (16u << 16);    memcpy(h + 32, &v, 4);    /* 4-byte frames, 16 bits */
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fclose(f);
}
