/*
 * pal_input_map.c - pure 3DS -> N64 input translation.
 *
 * No libctru here: everything works on PalRawInput so it can be unit-tested
 * on a PC (see tests/test_input_map.c).
 */
#include "pal/pal_input.h"

#include <math.h>
#include <string.h>

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

static const uint16_t dir_to_cbutton[4] = {
    N64_BTN_CUP, N64_BTN_CDOWN, N64_BTN_CLEFT, N64_BTN_CRIGHT,
};

void pal_map_reset(PalMapState *st)
{
    memset(st, 0, sizeof(*st));
    st->touch_mode = PAL_TOUCH_NONE;
    st->touch_zone = -1;
}

static int8_t round_clamp_stick(float v)
{
    int i = (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
    if (i > N64_STICK_MAX)  i = N64_STICK_MAX;
    if (i < -N64_STICK_MAX) i = -N64_STICK_MAX;
    return (int8_t)i;
}

/* Radial deadzone: rescales magnitude so the edge of the deadzone maps to 0
 * and raw_max maps to N64_STICK_MAX, preserving direction. A per-axis
 * deadzone would snap diagonals to the cardinal axes, which feels bad when
 * turning and walking at the same time. */
void pal_map_axis_pair(const PalStickTuning *t, int16_t x, int16_t y,
                       int8_t *out_x, int8_t *out_y)
{
    float fx = (float)x, fy = (float)y;
    float mag = sqrtf(fx * fx + fy * fy);

    if (mag <= (float)t->deadzone || t->raw_max <= t->deadzone) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    float n = (mag - (float)t->deadzone) / (float)(t->raw_max - t->deadzone);
    if (n > 1.0f)
        n = 1.0f;
    if (t->exponent > 0.0f && t->exponent != 1.0f)
        n = powf(n, t->exponent);

    float scale = n * (float)N64_STICK_MAX / mag;
    *out_x = round_clamp_stick(fx * scale);
    *out_y = round_clamp_stick(fy * scale);
}

/* Per-direction latch with hysteresis: press at `on`, release below `off`. */
static bool latch(bool was, int16_t v, int16_t on, int16_t off)
{
    return was ? (v > off) : (v >= on);
}

static uint16_t map_cstick(const PalInputProfile *p, PalMapState *st,
                           const PalRawInput *raw)
{
    if (!p->cstick_to_cbuttons)
        return 0;

    bool *l = st->cstick_latched;
    l[DIR_UP]    = latch(l[DIR_UP],     raw->cstick_y, p->cstick_on, p->cstick_off);
    l[DIR_DOWN]  = latch(l[DIR_DOWN],  -raw->cstick_y, p->cstick_on, p->cstick_off);
    l[DIR_LEFT]  = latch(l[DIR_LEFT],  -raw->cstick_x, p->cstick_on, p->cstick_off);
    l[DIR_RIGHT] = latch(l[DIR_RIGHT],  raw->cstick_x, p->cstick_on, p->cstick_off);

    uint16_t out = 0;
    for (int d = 0; d < 4; d++)
        if (l[d])
            out |= dir_to_cbutton[d];
    return out;
}

static int hit_zone(const PalInputProfile *p, uint16_t x, uint16_t y)
{
    for (int i = 0; i < p->num_zones; i++) {
        const PalTouchZone *z = &p->zones[i];
        if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h)
            return i;
    }
    return -1;
}

static void press_look(PalMapState *st, int dir, int opposite, uint8_t hold)
{
    st->look_hold[dir] = hold;
    st->look_hold[opposite] = 0;
}

static uint16_t map_touch(const PalInputProfile *p, PalMapState *st,
                          const PalRawInput *raw)
{
    st->look_dx = 0;
    st->look_dy = 0;

    if (!raw->touching) {
        st->touch_mode = PAL_TOUCH_NONE;
        st->touch_zone = -1;
        memset(st->look_hold, 0, sizeof(st->look_hold));
        return 0;
    }

    if (st->touch_mode == PAL_TOUCH_NONE) {
        /* New touch: what it started on decides what it does until release,
         * so dragging off a button doesn't turn into a camera swipe. */
        int z = hit_zone(p, raw->touch_x, raw->touch_y);
        if (z >= 0) {
            st->touch_mode = PAL_TOUCH_ZONE;
            st->touch_zone = (int8_t)z;
        } else if (p->touch_look) {
            st->touch_mode = PAL_TOUCH_LOOK;
        } else {
            /* Dead area: remember the touch so it can't become anything
             * else mid-press. */
            st->touch_mode = PAL_TOUCH_ZONE;
            st->touch_zone = -1;
        }
        st->last_tx = raw->touch_x;
        st->last_ty = raw->touch_y;
    }

    if (st->touch_mode == PAL_TOUCH_ZONE)
        return st->touch_zone >= 0 ? p->zones[st->touch_zone].n64 : 0;

    /* PAL_TOUCH_LOOK */
    int16_t dx = (int16_t)(raw->touch_x - st->last_tx);
    int16_t dy = (int16_t)(raw->touch_y - st->last_ty);
    st->last_tx = raw->touch_x;
    st->last_ty = raw->touch_y;
    st->look_dx = dx;
    st->look_dy = dy;

    /* Screen y grows downward; dragging up should look up. */
    int16_t up_amount = p->touch_look_invert_y ? dy : (int16_t)-dy;

    if (dx >= p->touch_look_on)        press_look(st, DIR_RIGHT, DIR_LEFT, p->touch_look_hold);
    else if (dx <= -p->touch_look_on)  press_look(st, DIR_LEFT, DIR_RIGHT, p->touch_look_hold);
    if (up_amount >= p->touch_look_on)       press_look(st, DIR_UP, DIR_DOWN, p->touch_look_hold);
    else if (up_amount <= -p->touch_look_on) press_look(st, DIR_DOWN, DIR_UP, p->touch_look_hold);

    uint16_t out = 0;
    for (int d = 0; d < 4; d++) {
        if (st->look_hold[d]) {
            out |= dir_to_cbutton[d];
            st->look_hold[d]--;
        }
    }
    return out;
}

PalN64Pad pal_map_frame(const PalInputProfile *p, PalMapState *st,
                        const PalRawInput *raw)
{
    PalN64Pad pad = {0, 0, 0};

    for (int i = 0; i < p->num_binds; i++)
        if (raw->held & p->binds[i].keys)
            pad.button |= p->binds[i].n64;

    pal_map_axis_pair(&p->move, raw->cpad_x, raw->cpad_y, &pad.stick_x, &pad.stick_y);

    pad.button |= map_cstick(p, st, raw);
    pad.button |= map_touch(p, st, raw);

    return pad;
}
