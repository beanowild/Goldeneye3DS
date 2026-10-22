/*
 * pal_rom_3ds.c - ROM file access through stdio on the sdmc: device.
 *
 * libctru's default startup mounts the SD card as "sdmc:", so plain
 * fopen/fread work. The only 3DS-specific part is the LightLock that keeps
 * seek+read atomic in streaming mode.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal_rom.h"

#define ROM_MIN_SIZE 0x101000u     /* header + IPL3 + some code */
#define ROM_MAX_SIZE 0x4000000u    /* 64 MB, the largest cartridge */

typedef enum { ORDER_Z64, ORDER_V64, ORDER_N64, ORDER_UNKNOWN } ByteOrder;

static FILE       *s_file;     /* stream mode only */
static uint8_t    *s_data;     /* preload mode only, z64 order */
static bool        s_owns_data; /* s_data was malloc'd here, not the caller's */
static uint32_t    s_size;
static const char *s_path;
static LightLock   s_lock;
static char        s_err[256];

static ByteOrder detect_order(const uint8_t h[4])
{
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) return ORDER_Z64;
    if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) return ORDER_V64;
    if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) return ORDER_N64;
    return ORDER_UNKNOWN;
}

/* Convert the whole image to z64 order in place. */
static void normalize(uint8_t *p, uint32_t size, ByteOrder order)
{
    if (order == ORDER_V64) {           /* 16-bit halves swapped */
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
        }
    } else if (order == ORDER_N64) {    /* 32-bit words reversed */
        for (uint32_t i = 0; i + 3 < size; i += 4) {
            uint8_t a = p[i], b = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2];
            p[i + 2] = b;    p[i + 3] = a;
        }
    }
}

static const char *fail(const char *fmt, const char *path)
{
    snprintf(s_err, sizeof(s_err), fmt, path);
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    if (s_owns_data)
        free(s_data);
    s_data = NULL;
    s_owns_data = false;
    s_size = 0;
    s_path = NULL;
    return s_err;
}

const char *pal_rom_open(const char *const *paths, PalRomMode mode,
                         void *image, uint32_t cap)
{
    LightLock_Init(&s_lock);

    FILE *f = NULL;
    const char *path = NULL;
    for (const char *const *p = paths; *p; p++) {
        if ((f = fopen(*p, "rb")) != NULL) {
            path = *p;
            break;
        }
    }
    if (!f) {
        /* name the SD card path: nobody can put a file into romfs: */
        const char *const *p = paths;
        while (*p && strncmp(*p, "sdmc:", 5) != 0)
            p++;
        snprintf(s_err, sizeof(s_err),
                 "GoldenEye ROM not found.\n\nCopy your own dump to:\n%s",
                 *p ? *p : "(no path configured)");
        return s_err;
    }
    s_file = f;
    s_path = path;

    if (fseek(f, 0, SEEK_END) != 0)
        return fail("Could not read %s", path);
    long len = ftell(f);
    if (len < (long)ROM_MIN_SIZE || len > (long)ROM_MAX_SIZE)
        return fail("%s is not an N64 ROM (bad size).", path);
    s_size = (uint32_t)len;
    rewind(f);

    uint8_t hdr[0x40];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        return fail("Could not read %s", path);

    ByteOrder order = detect_order(hdr);
    if (order == ORDER_UNKNOWN)
        return fail("%s is not an N64 ROM (bad header).", path);
    if (order != ORDER_Z64 && (s_size % 4) != 0)
        return fail("%s has an odd size for a byte-swapped ROM.", path);

    /* The internal name at 0x20 should be "GOLDENEYE". Only warn on a
     * mismatch, so translation patches and hacks still load. */
    normalize(hdr, sizeof(hdr), order);
    if (memcmp(hdr + 0x20, "GOLDENEYE", 9) != 0)
        fprintf(stderr, "warning: %s: internal name is not GOLDENEYE\n", path);

    if (mode == PAL_ROM_STREAM) {
        if (order != ORDER_Z64)
            return fail("%s is byte-swapped. Convert it to .z64 to use "
                        "streaming mode, or use preload mode.", path);
        /* Larger stdio buffer: most game DMAs are a few KB and close together. */
        setvbuf(f, NULL, _IOFBF, 64 * 1024);
        return NULL;
    }

    /* PAL_ROM_PRELOAD */
    if (image) {
        if (s_size > cap)
            return fail("%s is larger than a GoldenEye ROM.", path);
        s_data = image;
    } else {
        s_data = malloc(s_size);
        s_owns_data = true;
    }
    if (!s_data)
        return fail("Not enough memory to load %s", path);
    rewind(f);
    if (fread(s_data, 1, s_size, f) != s_size)
        return fail("Could not read %s", path);
    normalize(s_data, s_size, order);

    fclose(f);
    s_file = NULL;
    return NULL;
}

uint32_t    pal_rom_size(void) { return s_size; }
const char *pal_rom_path(void) { return s_path; }

const uint8_t *pal_rom_ptr(uint32_t offset, uint32_t size)
{
    if (!s_data || offset > s_size || size > s_size - offset)
        return NULL;
    return s_data + offset;
}

bool pal_rom_read(void *dst, uint32_t offset, uint32_t size)
{
    uint32_t avail = offset < s_size ? s_size - offset : 0;
    uint32_t n = size < avail ? size : avail;

    if (n) {
        if (s_data) {
            memcpy(dst, s_data + offset, n);
        } else if (s_file) {
            LightLock_Lock(&s_lock);
            size_t got = 0;
            if (fseek(s_file, (long)offset, SEEK_SET) == 0)
                got = fread(dst, 1, n, s_file);
            LightLock_Unlock(&s_lock);
            n = (uint32_t)got;
        } else {
            n = 0;   /* not open */
        }
    }

    if (n < size)
        memset((uint8_t *)dst + n, 0, size - n);
    return n == size;
}
