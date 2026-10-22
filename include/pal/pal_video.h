/*
 * pal_video.h - where the VI shim hands finished frames to the renderer.
 *
 * On N64, osViSwapBuffer only takes effect at the next vertical retrace,
 * when the VI starts scanning out the new framebuffer. The shim keeps that
 * timing and calls the present hook from the retrace pump (the 3DS main
 * thread) whenever the displayed buffer or the black state changes.
 *
 * The F3D renderer (src/gfx, not written yet) installs the hook. In it, it
 * finishes the citro3d frame that drew `fb` (C3D_FrameEnd) so it reaches
 * the screen. Until then no hook is set and frames are simply not shown.
 */
#ifndef PAL_VIDEO_H
#define PAL_VIDEO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `fb` is the game's N64 framebuffer address, used to identify which
 * rendered frame to show. `black` is osViBlack's state: show black
 * instead of the frame. Runs on the main thread with no locks held. */
typedef void (*PalPresentHook)(const void *fb, bool black);

void pal_video_set_present_hook(PalPresentHook hook);
void pal_video_present(const void *fb, bool black);

#ifdef __cplusplus
}
#endif

#endif /* PAL_VIDEO_H */
