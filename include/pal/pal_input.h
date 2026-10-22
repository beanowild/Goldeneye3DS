/*
 * pal_input.h - 3DS -> N64 controller translation layer.
 *
 * Split into two halves:
 *   - pal_input_map.c : pure mapping logic (no libctru). Takes a PalRawInput
 *                       snapshot and produces an N64 pad. Host-testable.
 *   - pal_input_3ds.c : polls HID via libctru, fills PalRawInput, owns the
 *                       active profile and per-frame state.
 *
 * The libultra shim (src/ultra/os_cont.c) is the only consumer that knows
 * about OSContPad; everything here uses PalN64Pad, which has the same layout.
 */
#ifndef PAL_INPUT_H
#define PAL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- N64 side ---------------------------------------------------------- */

/* Bit values match libultra's os_cont.h so the shim can copy them verbatim. */
enum {
    N64_BTN_CRIGHT = 0x0001,
    N64_BTN_CLEFT  = 0x0002,
    N64_BTN_CDOWN  = 0x0004,
    N64_BTN_CUP    = 0x0008,
    N64_BTN_R      = 0x0010,
    N64_BTN_L      = 0x0020,
    N64_BTN_DRIGHT = 0x0100,
    N64_BTN_DLEFT  = 0x0200,
    N64_BTN_DDOWN  = 0x0400,
    N64_BTN_DUP    = 0x0800,
    N64_BTN_START  = 0x1000,
    N64_BTN_Z      = 0x2000,
    N64_BTN_B      = 0x4000,
    N64_BTN_A      = 0x8000,
};

/* An N64 stick reports about +/-80 at full deflection; GoldenEye clamps
 * internally, so we never exceed this range. */
#define N64_STICK_MAX 80

typedef struct {
    uint16_t button;
    int8_t   stick_x;   /* +right */
    int8_t   stick_y;   /* +up    */
} PalN64Pad;

/* ---- 3DS side ---------------------------------------------------------- */

/* These mirror libctru's KEY_* bits so the mapping code can run without
 * libctru. pal_input_3ds.c static-asserts that they match. */
enum {
    PAL_KEY_A      = 1u << 0,
    PAL_KEY_B      = 1u << 1,
    PAL_KEY_SELECT = 1u << 2,
    PAL_KEY_START  = 1u << 3,
    PAL_KEY_DRIGHT = 1u << 4,
    PAL_KEY_DLEFT  = 1u << 5,
    PAL_KEY_DUP    = 1u << 6,
    PAL_KEY_DDOWN  = 1u << 7,
    PAL_KEY_R      = 1u << 8,
    PAL_KEY_L      = 1u << 9,
    PAL_KEY_X      = 1u << 10,
    PAL_KEY_Y      = 1u << 11,
    PAL_KEY_ZL     = 1u << 14,  /* New 3DS only */
    PAL_KEY_ZR     = 1u << 15,  /* New 3DS only */
    PAL_KEY_TOUCH  = 1u << 20,
};

/* Bottom screen resolution, for touch zones. */
#define PAL_BOTTOM_W 320
#define PAL_BOTTOM_H 240

/* One frame of raw hardware state. */
typedef struct {
    uint32_t held;              /* PAL_KEY_* bitmask */
    int16_t  cpad_x, cpad_y;    /* circle pad, roughly +/-156, +up */
    int16_t  cstick_x, cstick_y;/* New 3DS C-stick, roughly +/-146, +up; 0 on Old 3DS */
    bool     touching;
    uint16_t touch_x, touch_y;  /* 0..319, 0..239, +down */
} PalRawInput;

/* ---- Profile ----------------------------------------------------------- */

/* "Any of these 3DS keys held -> set these N64 buttons". */
typedef struct {
    uint32_t keys;
    uint16_t n64;
} PalButtonBind;

/* Radial deadzone + response curve for an analog source. */
typedef struct {
    int16_t deadzone;   /* raw units */
    int16_t raw_max;    /* raw magnitude treated as full deflection */
    float   exponent;   /* 1.0 = linear, >1 = finer control near centre */
} PalStickTuning;

/* Rectangular bottom-screen button. Touches that *begin* inside a zone
 * press its buttons until released; touches that begin elsewhere become
 * look drags (see touch_look_*). */
typedef struct {
    const char *label;
    uint16_t x, y, w, h;
    uint16_t n64;
} PalTouchZone;

#define PAL_MAX_BINDS 24
#define PAL_MAX_ZONES 8

typedef struct {
    const char    *name;
    PalButtonBind  binds[PAL_MAX_BINDS];
    uint8_t        num_binds;

    PalStickTuning move;            /* circle pad -> N64 stick */

    /* C-stick -> C-buttons. Hysteresis stops chatter around the threshold. */
    bool           cstick_to_cbuttons;
    int16_t        cstick_on, cstick_off;

    PalTouchZone   zones[PAL_MAX_ZONES];
    uint8_t        num_zones;

    /* Touch drag -> C-buttons. A drag of at least touch_look_on pixels in
     * one frame presses the matching C-button; it stays pressed for
     * touch_look_hold frames so slow drags don't flicker. */
    bool           touch_look;
    int16_t        touch_look_on;
    uint8_t        touch_look_hold;
    bool           touch_look_invert_y;
} PalInputProfile;

extern const PalInputProfile pal_profile_new3ds;
extern const PalInputProfile pal_profile_old3ds;

/* ---- Mapping state (pure; used by pal_input_map.c) --------------------- */

typedef enum {
    PAL_TOUCH_NONE,
    PAL_TOUCH_ZONE,
    PAL_TOUCH_LOOK,
} PalTouchMode;

typedef struct {
    bool         cstick_latched[4];   /* up, down, left, right */
    PalTouchMode touch_mode;
    int8_t       touch_zone;          /* valid when touch_mode == ZONE */
    uint16_t     last_tx, last_ty;
    uint8_t      look_hold[4];        /* frames remaining, up/down/left/right */
    int16_t      look_dx, look_dy;    /* this frame's drag, for future analog aim */
} PalMapState;

void pal_map_reset(PalMapState *st);

/* Translate one frame. `st` carries hysteresis and touch tracking. */
PalN64Pad pal_map_frame(const PalInputProfile *prof, PalMapState *st,
                        const PalRawInput *raw);

/* Exposed for tests. */
void pal_map_axis_pair(const PalStickTuning *t, int16_t x, int16_t y,
                       int8_t *out_x, int8_t *out_y);

/* ---- Runtime (pal_input_3ds.c) ----------------------------------------- */

void pal_input_init(void);                 /* picks profile by model */
void pal_input_set_profile(const PalInputProfile *prof);
const PalInputProfile *pal_input_profile(void);

/* Scan HID and recompute the pad. Call exactly once per game frame. */
void pal_input_update(void);

const PalN64Pad   *pal_input_pad(void);
const PalRawInput *pal_input_raw(void);
const PalMapState *pal_input_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_INPUT_H */
