/*
 * pal_input_profiles.c - built-in control layouts.
 *
 * GoldenEye's default "1.1 Honey" scheme: stick = move/turn,
 * C-buttons = strafe and look up/down, Z = fire, R = aim, A = change weapon,
 * B = action (doors, switches), Start = watch menu.
 *
 * Deadzones and thresholds are starting points; tune them on hardware.
 */
#include "pal/pal_input.h"

/* Bottom-screen buttons, shared by both profiles. A column down the right
 * edge for the right thumb, Start in the top-left corner where it can't be
 * hit by accident. Everything else on the screen is the look pad. */
#define BOTTOM_ZONES                                                   \
    .zones = {                                                         \
        { "WATCH",  0,   0,   64, 40, N64_BTN_START },                 \
        { "A",      248, 0,   72, 80, N64_BTN_A },                     \
        { "B",      248, 80,  72, 80, N64_BTN_B },                     \
        { "Z FIRE", 248, 160, 72, 80, N64_BTN_Z },                     \
    },                                                                 \
    .num_zones = 4

#define TOUCH_LOOK_DEFAULTS                                            \
    .touch_look = true,                                                \
    .touch_look_on = 3,                                                \
    .touch_look_hold = 4,                                              \
    .touch_look_invert_y = false

/* New 3DS / New 2DS XL: C-stick is the C-buttons, triggers do aim/fire. */
const PalInputProfile pal_profile_new3ds = {
    .name = "New 3DS",
    .binds = {
        { PAL_KEY_A,                      N64_BTN_A },
        { PAL_KEY_B,                      N64_BTN_B },
        { PAL_KEY_X,                      N64_BTN_Z },   /* fire without triggers */
        /* Y is left free for a future quick-action (e.g. toggle bottom-screen map). */
        { PAL_KEY_ZR,                     N64_BTN_Z },
        { PAL_KEY_ZL,                     N64_BTN_R },
        { PAL_KEY_L,                      N64_BTN_L },
        { PAL_KEY_R,                      N64_BTN_R },
        { PAL_KEY_START | PAL_KEY_SELECT, N64_BTN_START },
        { PAL_KEY_DUP,                    N64_BTN_DUP },
        { PAL_KEY_DDOWN,                  N64_BTN_DDOWN },
        { PAL_KEY_DLEFT,                  N64_BTN_DLEFT },
        { PAL_KEY_DRIGHT,                 N64_BTN_DRIGHT },
    },
    .num_binds = 12,

    .move = { .deadzone = 15, .raw_max = 150, .exponent = 1.0f },

    .cstick_to_cbuttons = true,
    .cstick_on = 40,
    .cstick_off = 25,

    BOTTOM_ZONES,
    TOUCH_LOOK_DEFAULTS,
};

/* Old 3DS / 2DS: no C-stick and no ZL/ZR.
 *   ABXY  -> C-button diamond (X up, B down, Y left, A right)
 *   R     -> Z (fire)   L -> R (aim)
 * Z has to live on a shoulder or you can't fire while looking, so L/R don't
 * map 1:1 here. A/B (weapon/action) are on the D-pad and the touch column. */
const PalInputProfile pal_profile_old3ds = {
    .name = "Old 3DS",
    .binds = {
        { PAL_KEY_X,                      N64_BTN_CUP },
        { PAL_KEY_B,                      N64_BTN_CDOWN },
        { PAL_KEY_Y,                      N64_BTN_CLEFT },
        { PAL_KEY_A,                      N64_BTN_CRIGHT },
        { PAL_KEY_R,                      N64_BTN_Z },
        { PAL_KEY_L,                      N64_BTN_R },
        { PAL_KEY_DUP,                    N64_BTN_A },
        { PAL_KEY_DDOWN,                  N64_BTN_B },
        { PAL_KEY_DLEFT,                  N64_BTN_L },
        { PAL_KEY_START | PAL_KEY_SELECT, N64_BTN_START },
    },
    .num_binds = 10,

    .move = { .deadzone = 15, .raw_max = 150, .exponent = 1.0f },

    .cstick_to_cbuttons = false,

    BOTTOM_ZONES,
    TOUCH_LOOK_DEFAULTS,
};
