/*
 * pal_rom.h - read-only access to the user's GoldenEye ROM on the SD card.
 *
 * Offsets are plain ROM offsets (0 = first byte of the .z64), and data
 * comes back in .z64 (big-endian) byte order whatever the file's format,
 * matching what the N64 PI would DMA into RDRAM.
 */
#ifndef PAL_ROM_H
#define PAL_ROM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Read the whole ROM into RAM at open. 12 MB fits easily, and DMAs
     * become a memcpy with no SD latency. Accepts .z64, .v64 and .n64. */
    PAL_ROM_PRELOAD,
    /* Read from the file on every request. Saves 12 MB of RAM but each
     * DMA waits on the SD card. Only .z64 byte order is accepted. */
    PAL_ROM_STREAM,
} PalRomMode;

/* Tries each path in the NULL-terminated list; the first that exists is
 * used. In preload mode the ROM is read into `image` (at least `cap`
 * bytes), or into a new allocation when `image` is NULL. Returns NULL on
 * success, or a message to show the user. */
const char *pal_rom_open(const char *const *paths, PalRomMode mode,
                         void *image, uint32_t cap);

uint32_t    pal_rom_size(void);
const char *pal_rom_path(void);

/* Copy `size` bytes from ROM `offset` into `dst`. Bytes beyond the end of
 * the ROM (or lost to a read error) are zeroed, and the call returns false.
 * Safe to call from several threads. */
bool pal_rom_read(void *dst, uint32_t offset, uint32_t size);

/* The ROM's bytes at offset, in place (preload mode), or NULL when the ROM
 * is streamed or the range runs past its end. Read-only. */
const uint8_t *pal_rom_ptr(uint32_t offset, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* PAL_ROM_H */
