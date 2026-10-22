/*
 * os_cont.c - libultra controller API on top of pal_input.
 *
 * Built only with GE_INTEGRATE=ON, against the decomp's <ultra64.h>.
 * On N64 these calls kick off an SI DMA and post to `mq` when it finishes.
 * Here the "transfer" is instant: we poll HID and post right away, so the
 * game's osRecvMesg() after osContStartReadData() returns immediately.
 */
#include <ultra64.h>

#include "pal/pal_input.h"
#include "ultra/os_message.h"

/* libultra calls this field `errno`, which collides with newlib's errno
 * macro if <errno.h> is ever pulled in. Some decomps rename it; override
 * here if this one does. */
#ifndef OSCONT_ERRNO_FIELD
#define OSCONT_ERRNO_FIELD errno
#endif

_Static_assert(N64_BTN_A == A_BUTTON && N64_BTN_B == B_BUTTON &&
               N64_BTN_Z == Z_TRIG && N64_BTN_START == START_BUTTON &&
               N64_BTN_L == L_TRIG && N64_BTN_R == R_TRIG &&
               N64_BTN_CUP == U_CBUTTONS && N64_BTN_CDOWN == D_CBUTTONS &&
               N64_BTN_CLEFT == L_CBUTTONS && N64_BTN_CRIGHT == R_CBUTTONS &&
               N64_BTN_DUP == U_JPAD && N64_BTN_DDOWN == D_JPAD &&
               N64_BTN_DLEFT == L_JPAD && N64_BTN_DRIGHT == R_JPAD,
               "pal_input button bits must match libultra");

/* Only port 0 exists. Multiplayer would need a second input source. */
#define CONNECTED_MASK 0x01

static void fill_status(OSContStatus *data)
{
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        if (CONNECTED_MASK & (1 << i)) {
            data[i].type = CONT_TYPE_NORMAL;
            data[i].status = 0;   /* no Controller/Rumble Pak inserted */
            data[i].OSCONT_ERRNO_FIELD = 0;
        } else {
            data[i].type = 0;
            data[i].status = 0;
            data[i].OSCONT_ERRNO_FIELD = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *data)
{
    (void)mq;
    pal_input_init();
    *bitpattern = CONNECTED_MASK;
    fill_status(data);
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq)
{
    osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus *data)
{
    fill_status(data);
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    /* This is the one place per game frame where HID is sampled, so the pad
     * the game reads always matches the frame it's simulating. */
    pal_input_update();
    osSendMesg(mq, (OSMesg)0, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad *data)
{
    const PalN64Pad *pad = pal_input_pad();

    for (int i = 0; i < MAXCONTROLLERS; i++) {
        if (CONNECTED_MASK & (1 << i)) {
            data[i].button = pad->button;
            data[i].stick_x = pad->stick_x;
            data[i].stick_y = pad->stick_y;
            data[i].OSCONT_ERRNO_FIELD = 0;
        } else {
            data[i].button = 0;
            data[i].stick_x = 0;
            data[i].stick_y = 0;
            data[i].OSCONT_ERRNO_FIELD = CONT_NO_RESPONSE_ERROR;
        }
    }
}

/* ---- Controller accessories (Controller Pak, Rumble Pak) ----------------
 *
 * The 3DS has neither. Every port reports "no pak", the same answer a
 * real N64 gives with nothing plugged into the controller, so the game's
 * own no-pak paths handle it (Rumble Pak support is skipped; saves go to
 * EEPROM anyway). The decomp's motor.c drives the Rumble Pak through
 * __osContRamRead/Write, so those fail the same way.
 */

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern)
{
    (void)mq;
    *pattern = 0;
    return 0;
}

s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq;
    (void)pfs;
    (void)channel;
    return PFS_ERR_NOPACK;
}

s32 __osContRamRead(OSMesgQueue *mq, int channel, u16 address, u8 *buffer)
{
    (void)mq;
    (void)channel;
    (void)address;
    (void)buffer;
    return PFS_ERR_NOPACK;
}

s32 __osContRamWrite(OSMesgQueue *mq, int channel, u16 address, u8 *buffer, int force)
{
    (void)mq;
    (void)channel;
    (void)address;
    (void)buffer;
    (void)force;
    return PFS_ERR_NOPACK;
}
