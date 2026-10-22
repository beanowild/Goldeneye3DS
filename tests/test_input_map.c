/*
 * Host-side tests for pal_input_map.c. No 3DS hardware or libctru needed:
 *
 *   cc -std=c11 -Iinclude tests/test_input_map.c src/pal/pal_input_map.c \
 *      src/pal/pal_input_profiles.c -lm -o test_input_map && ./test_input_map
 */
#include <stdio.h>
#include <stdlib.h>

#include "pal/pal_input.h"

static int failures;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                               \
        }                                                             \
    } while (0)

static PalN64Pad run(const PalInputProfile *p, PalMapState *st, PalRawInput raw)
{
    return pal_map_frame(p, st, &raw);
}

static void test_stick(void)
{
    const PalStickTuning *t = &pal_profile_new3ds.move;
    int8_t x, y;

    pal_map_axis_pair(t, 10, -10, &x, &y);     /* inside deadzone */
    CHECK(x == 0 && y == 0);

    pal_map_axis_pair(t, 156, 0, &x, &y);      /* past full deflection */
    CHECK(x == N64_STICK_MAX && y == 0);

    pal_map_axis_pair(t, 0, -150, &x, &y);
    CHECK(x == 0 && y == -N64_STICK_MAX);

    pal_map_axis_pair(t, 110, 110, &x, &y);    /* diagonal keeps direction */
    CHECK(x == y && x > 50 && x <= N64_STICK_MAX);

    pal_map_axis_pair(t, 16, 0, &x, &y);       /* just past deadzone: small */
    CHECK(x >= 0 && x <= 2);
}

static void test_bindings(void)
{
    PalMapState st;
    pal_map_reset(&st);

    PalN64Pad pad = run(&pal_profile_new3ds, &st,
                        (PalRawInput){ .held = PAL_KEY_ZR | PAL_KEY_SELECT });
    CHECK(pad.button == (N64_BTN_Z | N64_BTN_START));

    pal_map_reset(&st);
    pad = run(&pal_profile_old3ds, &st,
              (PalRawInput){ .held = PAL_KEY_X | PAL_KEY_A | PAL_KEY_R });
    CHECK(pad.button == (N64_BTN_CUP | N64_BTN_CRIGHT | N64_BTN_Z));
}

static void test_cstick_hysteresis(void)
{
    PalMapState st;
    pal_map_reset(&st);
    const PalInputProfile *p = &pal_profile_new3ds;

    CHECK(run(p, &st, (PalRawInput){ .cstick_y = 35 }).button == 0);            /* below on */
    CHECK(run(p, &st, (PalRawInput){ .cstick_y = 45 }).button == N64_BTN_CUP);  /* press */
    CHECK(run(p, &st, (PalRawInput){ .cstick_y = 30 }).button == N64_BTN_CUP);  /* held above off */
    CHECK(run(p, &st, (PalRawInput){ .cstick_y = 20 }).button == 0);            /* release */
    CHECK(run(p, &st, (PalRawInput){ .cstick_x = -60 }).button == N64_BTN_CLEFT);

    /* Old 3DS profile ignores the C-stick entirely. */
    pal_map_reset(&st);
    CHECK(run(&pal_profile_old3ds, &st, (PalRawInput){ .cstick_y = 100 }).button == 0);
}


static void test_touch_zone(void)
{
    PalMapState st;
    pal_map_reset(&st);
    const PalInputProfile *p = &pal_profile_new3ds;

    /* Press the Z FIRE column, then drag out of it: still Z, never look. */
    PalN64Pad pad = run(p, &st, (PalRawInput){ .touching = true, .touch_x = 280, .touch_y = 200 });
    CHECK(pad.button == N64_BTN_Z);
    pad = run(p, &st, (PalRawInput){ .touching = true, .touch_x = 150, .touch_y = 120 });
    CHECK(pad.button == N64_BTN_Z);

    pad = run(p, &st, (PalRawInput){ .touching = false });
    CHECK(pad.button == 0 && st.touch_mode == PAL_TOUCH_NONE);
}

static void test_touch_look(void)
{
    PalMapState st;
    pal_map_reset(&st);
    const PalInputProfile *p = &pal_profile_new3ds;

    /* First contact establishes the anchor and presses nothing. */
    PalN64Pad pad = run(p, &st, (PalRawInput){ .touching = true, .touch_x = 120, .touch_y = 120 });
    CHECK(pad.button == 0 && st.touch_mode == PAL_TOUCH_LOOK);

    /* Drag right and up (screen y decreases). */
    pad = run(p, &st, (PalRawInput){ .touching = true, .touch_x = 130, .touch_y = 110 });
    CHECK(pad.button == (N64_BTN_CRIGHT | N64_BTN_CUP));

    /* Holding still keeps the press for touch_look_hold frames total. */
    int held_frames = 1;
    while (run(p, &st, (PalRawInput){ .touching = true, .touch_x = 130, .touch_y = 110 }).button)
        held_frames++;
    CHECK(held_frames == p->touch_look_hold);

    /* Reversing direction swaps instantly instead of pressing both. */
    run(p, &st, (PalRawInput){ .touching = true, .touch_x = 140, .touch_y = 110 });
    pad = run(p, &st, (PalRawInput){ .touching = true, .touch_x = 130, .touch_y = 110 });
    CHECK(pad.button == N64_BTN_CLEFT);

    /* Release clears everything. */
    pad = run(p, &st, (PalRawInput){ .touching = false });
    CHECK(pad.button == 0);
}

int main(void)
{
    test_stick();
    test_bindings();
    test_cstick_hysteresis();
    test_touch_zone();
    test_touch_look();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("all input mapping tests passed\n");
    return EXIT_SUCCESS;
}
