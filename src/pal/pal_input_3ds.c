/*
 * pal_input_3ds.c - libctru HID polling feeding pal_input_map.
 */
#include <3ds.h>

#include "pal/pal_input.h"

_Static_assert((unsigned)PAL_KEY_A == (unsigned)KEY_A, "KEY_A mismatch");
_Static_assert((unsigned)PAL_KEY_B == (unsigned)KEY_B, "KEY_B mismatch");
_Static_assert((unsigned)PAL_KEY_SELECT == (unsigned)KEY_SELECT, "KEY_SELECT mismatch");
_Static_assert((unsigned)PAL_KEY_START == (unsigned)KEY_START, "KEY_START mismatch");
_Static_assert((unsigned)PAL_KEY_DRIGHT == (unsigned)KEY_DRIGHT, "KEY_DRIGHT mismatch");
_Static_assert((unsigned)PAL_KEY_DLEFT == (unsigned)KEY_DLEFT, "KEY_DLEFT mismatch");
_Static_assert((unsigned)PAL_KEY_DUP == (unsigned)KEY_DUP, "KEY_DUP mismatch");
_Static_assert((unsigned)PAL_KEY_DDOWN == (unsigned)KEY_DDOWN, "KEY_DDOWN mismatch");
_Static_assert((unsigned)PAL_KEY_R == (unsigned)KEY_R, "KEY_R mismatch");
_Static_assert((unsigned)PAL_KEY_L == (unsigned)KEY_L, "KEY_L mismatch");
_Static_assert((unsigned)PAL_KEY_X == (unsigned)KEY_X, "KEY_X mismatch");
_Static_assert((unsigned)PAL_KEY_Y == (unsigned)KEY_Y, "KEY_Y mismatch");
_Static_assert((unsigned)PAL_KEY_ZL == (unsigned)KEY_ZL, "KEY_ZL mismatch");
_Static_assert((unsigned)PAL_KEY_ZR == (unsigned)KEY_ZR, "KEY_ZR mismatch");
_Static_assert((unsigned)PAL_KEY_TOUCH == (unsigned)KEY_TOUCH, "KEY_TOUCH mismatch");

/* Only the physical buttons go through bindings; the CPAD_ and CSTICK_
 * pseudo-keys libctru synthesises from the analog sticks are handled as
 * analog input instead. */
#define PAL_DIGITAL_KEYS                                                   \
    (KEY_A | KEY_B | KEY_SELECT | KEY_START | KEY_DRIGHT | KEY_DLEFT |     \
     KEY_DUP | KEY_DDOWN | KEY_R | KEY_L | KEY_X | KEY_Y | KEY_ZL |        \
     KEY_ZR | KEY_TOUCH)

static const PalInputProfile *s_profile;
static PalMapState s_state;
static PalRawInput s_raw;
static PalN64Pad   s_pad;

void pal_input_init(void)
{
    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    pal_input_set_profile(is_new ? &pal_profile_new3ds : &pal_profile_old3ds);
}

void pal_input_set_profile(const PalInputProfile *prof)
{
    s_profile = prof;
    pal_map_reset(&s_state);
    s_pad = (PalN64Pad){0, 0, 0};
}

const PalInputProfile *pal_input_profile(void) { return s_profile; }

void pal_input_update(void)
{
    hidScanInput();

    u32 held = hidKeysHeld();
    {
        /* developer: Y cycles the renderer's debug modes (f3d.c) */
        extern int g_gfx_debug_enabled, g_gfx_debug_mode;
        extern void pal_log(const char *fmt, ...);
        if (g_gfx_debug_enabled && (hidKeysDown() & KEY_Y)) {
            g_gfx_debug_mode = (g_gfx_debug_mode + 1) % 9;
            pal_log("gfx debug mode %d", g_gfx_debug_mode);
        }
    }
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);   /* reads zero on Old 3DS */

    s_raw.held     = held & PAL_DIGITAL_KEYS;
    s_raw.cpad_x   = cp.dx;
    s_raw.cpad_y   = cp.dy;
    s_raw.cstick_x = cs.dx;
    s_raw.cstick_y = cs.dy;
    s_raw.touching = (held & KEY_TOUCH) != 0;
    if (s_raw.touching) {
        touchPosition tp;
        hidTouchRead(&tp);
        s_raw.touch_x = tp.px;
        s_raw.touch_y = tp.py;
    }

    s_pad = pal_map_frame(s_profile, &s_state, &s_raw);
}

const PalN64Pad   *pal_input_pad(void)   { return &s_pad; }
const PalRawInput *pal_input_raw(void)   { return &s_raw; }
const PalMapState *pal_input_state(void) { return &s_state; }
