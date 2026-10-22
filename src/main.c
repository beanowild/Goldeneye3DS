/*
 * main.c - entry point.
 *
 * With GE_INTEGRATE, sets up the libultra shim and enters the decomp (see
 * run_game). Otherwise it runs an input test: the top screen
 * shows the N64 pad the game would see, the bottom screen shows touch state
 * and the touch-zone layout. Use it to tune deadzones and thresholds on
 * real hardware before any game code exists.
 *
 *   L + R + SELECT        switch profile
 *   START + SELECT (1s)   exit
 */
#include <3ds.h>
#include <stdio.h>

#include "pal/pal_input.h"
#include "pal/pal_sys.h"

#ifdef GE_INTEGRATE
#include "ultra/os_port.h"

/* The decomp function that, on N64, runs after hardware init and starts
 * the first thread with osCreateThread/osStartThread. Set with
 * -DGE_BOOT_ENTRY=<name> at configure time. */
extern void GE_BOOT_ENTRY(void);

static void run_game(void)
{
    /* stderr -> svcOutputDebugString: pal_log output shows up in
     * emulator logs (Azahar/Citra) and in Luma3DS's debugger. */
    consoleDebugInit(debugDevice_SVC);
    pal_log("ge007: starting");

    const char *err = osPortInit();
    if (err)
        pal_fatal("%s", err);
    pal_log("ge007: port initialised, entering decomp");

    /* The main thread stands in for N64 interrupts (VI retrace, and later
     * audio), so it must preempt every game thread. */
    pal_thread_set_priority(NULL, PAL_PRIO_SERVICE);

    /* On N64 the boot code never returns: starting the first thread leaves
     * the boot context behind for good. Here GE_BOOT_ENTRY returns once it
     * has started that thread, and the main thread goes on to pump APT
     * (home menu, sleep, power) and the VI retrace. */
    GE_BOOT_ENTRY();

    while (aptMainLoop()) {
        gspWaitForVBlank();
        osPortVblank();
    }

    pal_sys_restore_cpu_limit();   /* first: the rest may wait on game threads */
    osPortExit();
}
#endif

static const struct { uint16_t bit; const char *name; } k_n64_buttons[] = {
    { N64_BTN_A, "A" },      { N64_BTN_B, "B" },      { N64_BTN_Z, "Z" },
    { N64_BTN_START, "St" }, { N64_BTN_L, "L" },      { N64_BTN_R, "R" },
    { N64_BTN_CUP, "C^" },   { N64_BTN_CDOWN, "Cv" },
    { N64_BTN_CLEFT, "C<" }, { N64_BTN_CRIGHT, "C>" },
    { N64_BTN_DUP, "D^" },   { N64_BTN_DDOWN, "Dv" },
    { N64_BTN_DLEFT, "D<" }, { N64_BTN_DRIGHT, "D>" },
};

static const char *touch_mode_name(PalTouchMode m)
{
    switch (m) {
    case PAL_TOUCH_ZONE: return "zone";
    case PAL_TOUCH_LOOK: return "look";
    default:             return "none";
    }
}

static void draw_top(PrintConsole *con)
{
    const PalN64Pad *pad = pal_input_pad();
    const PalRawInput *raw = pal_input_raw();

    consoleSelect(con);
    printf("\x1b[1;1HGoldenEye 3DS - input test\x1b[K\n");
    printf("Profile: %s\x1b[K\n\n", pal_input_profile()->name);

    printf("N64 buttons:\x1b[K\n ");
    for (size_t i = 0; i < sizeof(k_n64_buttons) / sizeof(k_n64_buttons[0]); i++) {
        bool on = pad->button & k_n64_buttons[i].bit;
        /* green when pressed, dim grey otherwise */
        printf("%s%-3s\x1b[0m", on ? "\x1b[32;1m" : "\x1b[30;1m", k_n64_buttons[i].name);
    }
    printf("\x1b[K\n\n");

    printf("N64 stick   x=%4d y=%4d\x1b[K\n", pad->stick_x, pad->stick_y);
    printf("Circle pad  x=%4d y=%4d\x1b[K\n", raw->cpad_x, raw->cpad_y);
    printf("C-stick     x=%4d y=%4d\x1b[K\n", raw->cstick_x, raw->cstick_y);
    printf("Raw keys    %08lx\x1b[K\n\n", (unsigned long)raw->held);

    printf("L+R+SELECT: switch profile\x1b[K\n");
    printf("START+SELECT (hold): exit\x1b[K\n");
}

static void draw_bottom(PrintConsole *con)
{
    const PalRawInput *raw = pal_input_raw();
    const PalMapState *st = pal_input_state();
    const PalInputProfile *p = pal_input_profile();

    consoleSelect(con);
    printf("\x1b[1;1HTouch: %s", raw->touching ? "down" : "up  ");
    if (raw->touching)
        printf(" (%3u,%3u)", raw->touch_x, raw->touch_y);
    printf("\x1b[K\nMode: %s  drag dx=%3d dy=%3d\x1b[K\n\n",
           touch_mode_name(st->touch_mode), st->look_dx, st->look_dy);

    printf("Zones (x,y,w,h):\x1b[K\n");
    for (int i = 0; i < p->num_zones; i++) {
        const PalTouchZone *z = &p->zones[i];
        bool active = st->touch_mode == PAL_TOUCH_ZONE && st->touch_zone == i;
        printf(" %s%-7s %3u,%3u %3ux%-3u\x1b[0m\x1b[K\n",
               active ? "\x1b[32;1m" : "", z->label, z->x, z->y, z->w, z->h);
    }
    printf("\nTouch elsewhere and drag to look.\x1b[K\n");
}

static void run_input_test(void)
{
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    pal_input_init();

    int exit_frames = 0;

    while (aptMainLoop()) {
        pal_input_update();

        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();

        if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (down & KEY_SELECT)) {
            pal_input_set_profile(pal_input_profile() == &pal_profile_new3ds
                                      ? &pal_profile_old3ds
                                      : &pal_profile_new3ds);
        }

        if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
            if (++exit_frames >= 60)
                break;
        } else {
            exit_frames = 0;
        }

        draw_top(&top);
        draw_bottom(&bottom);

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

int main(void)
{
    gfxInitDefault();

#ifdef GE_INTEGRATE
    run_game();
#else
    run_input_test();
#endif

    gfxExit();
    return 0;
}
