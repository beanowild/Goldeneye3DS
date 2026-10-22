/*
 * pal_save_3ds.c - save files on the SD card through stdio.
 *
 * The SD card's FAT filesystem can't rename over an existing file, so a
 * store is: write <name>.tmp, delete <name>, rename .tmp to <name>. If
 * power is lost between the delete and the rename, only <name>.tmp
 * exists, and pal_save_load picks it up.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pal/pal_save.h"
#include "pal/pal_sys.h"

static void make_path(char *out, size_t cap, const char *name, const char *suffix)
{
    snprintf(out, cap, "%s/%s%s", PAL_SAVE_DIR, name, suffix);
}

static size_t read_file(const char *path, void *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return n;
}

size_t pal_save_load(const char *name, void *buf, size_t size)
{
    char path[256];

    make_path(path, sizeof(path), name, "");
    size_t n = read_file(path, buf, size);
    if (n == 0) {
        make_path(path, sizeof(path), name, ".tmp");
        n = read_file(path, buf, size);
    }
    return n;
}

static void ensure_dir(void)
{
    /* Both levels, in case this is the first homebrew on the card. */
    if (mkdir("sdmc:/3ds", 0777) != 0 && errno != EEXIST)
        pal_log("save: mkdir sdmc:/3ds failed (errno %d)", errno);
    if (mkdir(PAL_SAVE_DIR, 0777) != 0 && errno != EEXIST)
        pal_log("save: mkdir %s failed (errno %d)", PAL_SAVE_DIR, errno);
}

bool pal_save_store(const char *name, const void *buf, size_t size)
{
    char path[256], tmp[256];
    make_path(path, sizeof(path), name, "");
    make_path(tmp, sizeof(tmp), name, ".tmp");

    ensure_dir();

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        pal_log("save: cannot create %s", tmp);
        return false;
    }
    bool ok = fwrite(buf, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        pal_log("save: writing %s failed (SD card full?)", tmp);
        remove(tmp);
        return false;
    }

    remove(path);   /* may not exist yet */
    if (rename(tmp, path) != 0) {
        /* The data is safe in .tmp, and pal_save_load will find it. */
        pal_log("save: rename %s -> %s failed", tmp, path);
        return false;
    }
    return true;
}
