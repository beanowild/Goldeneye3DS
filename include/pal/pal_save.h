/*
 * pal_save.h - small save files in the game's SD card folder.
 */
#ifndef PAL_SAVE_H
#define PAL_SAVE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_SAVE_DIR "sdmc:/3ds/ge007"

/* Read up to `size` bytes of PAL_SAVE_DIR/<name> into `buf`. Returns the
 * number of bytes read; 0 if the file doesn't exist yet. */
size_t pal_save_load(const char *name, void *buf, size_t size);

/* Replace PAL_SAVE_DIR/<name> with `buf`. The data goes to a temporary
 * file first, and pal_save_load falls back to that file, so a crash or
 * power loss mid-save leaves either the old save or the new one, never a
 * mix. */
bool pal_save_store(const char *name, const void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PAL_SAVE_H */
