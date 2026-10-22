/*
 * pal_video.c - present-hook storage. Neutral code (no <3ds.h>,
 * no <ultra64.h>) so both the renderer and the VI shim can link to it.
 */
#include <stddef.h>

#include "pal/pal_video.h"

static PalPresentHook s_hook;

void pal_video_set_present_hook(PalPresentHook hook)
{
    s_hook = hook;
}

void pal_video_present(const void *fb, bool black)
{
    if (s_hook)
        s_hook(fb, black);
}
