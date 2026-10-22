/*
 * os_pi.c - libultra PI (cartridge DMA) backed by the ROM file.
 *
 * On N64, osPiStartDma queues a request for the PI manager thread, which
 * runs the DMA and then posts the request's OSIoMesg to `mq`. The game
 * keeps working and later blocks on `mq`.
 *
 * Here the copy happens inside osPiStartDma (a memcpy in preload mode, an
 * SD read in streaming mode) and the completion message is posted before
 * it returns. That is legal under the N64 contract, which only promises
 * the data is ready once the message arrives, never that it isn't ready
 * earlier. It also needs no manager thread. The usual pattern is to start a
 * DMA and wait on `mq` straight away, and that pattern loses nothing from
 * doing the copy up front. If profiling shows the game overlapping work
 * with large DMAs, a worker thread can be added behind this same API.
 *
 * Cache maintenance (osInvalDCache etc.) that the game does around DMAs is
 * unnecessary here: the CPU does the copy, so the cache is always coherent.
 */
#include <ultra64.h>

#include "pal/pal_rom.h"
#include "pal/pal_sys.h"
#include "ultra/os_event.h"
#include "ultra/os_message.h"
#include "ultra/os_pi.h"

/* ROM addresses reach the PI in two forms:
 *   - the address of an asset symbol (romCopy(buf, &PcctvZ, ...)). Those
 *     symbols sit inside ge_rom_image at their ROM offset (see
 *     tools/gen_rom_symbols.py), so subtract the image base;
 *   - a plain cartridge offset or physical address (0x10xxxxxx). The PI
 *     ORs devAddr into osRomBase (0xB0000000), so masking to 28 bits gives
 *     the same ROM byte. */
extern u8 ge_rom_image[];
extern u8 ge_rom_image_end[];

static u32 pi_rom_offset(u32 addr)
{
    if (addr >= (u32)ge_rom_image && addr < (u32)ge_rom_image_end)
        return addr - (u32)ge_rom_image;
    return addr & 0x0FFFFFFFu;
}
#define PI_ROM_OFFSET(addr) pi_rom_offset(addr)

/* A cartridge address's bytes in place, or NULL (streamed ROM): lets hot
 * small reads skip the DMA (model.c loadAnimationFrame). */
const void *osPortRomPointer(u32 devAddr, u32 size)
{
    return pal_rom_ptr(PI_ROM_OFFSET(devAddr), size);
}

static OSMesgQueue *s_cmd_queue;
static int          s_pi_active;

/* Each warning is logged once, so a bad read in a loop doesn't flood the log. */
static int s_warned_range, s_warned_write;

static void do_dma(s32 direction, u32 devAddr, void *dram, u32 size)
{
    if (direction == OS_READ) {
        if (!pal_rom_read(dram, PI_ROM_OFFSET(devAddr), size) && !s_warned_range) {
            s_warned_range = 1;
            pal_log("PI: read 0x%08lx+0x%lx goes past the ROM end; zero-filled",
                    (unsigned long)devAddr, (unsigned long)size);
        }
    } else if (!s_warned_write) {
        /* Cartridge ROM is read-only; on N64 such a write does nothing. */
        s_warned_write = 1;
        pal_log("PI: ignoring write to cartridge 0x%08lx", (unsigned long)devAddr);
    }
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt)
{
    (void)pri;   /* no manager thread */

    /* Set up the command queue as libultra would, in case game code peeks
     * at it through osPiGetCmdQueue. Nothing is ever sent on it. */
    osCreateMesgQueue(cmdQ, cmdBuf, cmdMsgCnt);
    s_cmd_queue = cmdQ;
    s_pi_active = 1;
}

OSMesgQueue *osPiGetCmdQueue(void)
{
    return s_pi_active ? s_cmd_queue : NULL;
}

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr,
                 void *vAddr, u32 nbytes, OSMesgQueue *mq)
{
    /* libultra fails like this if osCreatePiManager hasn't run. */
    if (!s_pi_active)
        return -1;

    /* Fill in the request like libultra does; the game may check these
     * fields when the message comes back. `priority` (OS_MESG_PRI_HIGH)
     * only affected the order of the manager's queue, and requests here
     * finish immediately, so it is just recorded. */
    mb->hdr.type = (direction == OS_READ) ? OS_MESG_TYPE_DMAREAD : OS_MESG_TYPE_DMAWRITE;
    mb->hdr.pri = (u8)priority;
    mb->hdr.retQueue = mq;
    mb->dramAddr = vAddr;
    mb->devAddr = devAddr;
    mb->size = nbytes;

    static int s_logged_first_dma;
    if (!s_logged_first_dma) {
        s_logged_first_dma = 1;
        pal_log("ge007: first PI DMA: ROM 0x%06lx, %lu bytes",
                (unsigned long)PI_ROM_OFFSET(devAddr), (unsigned long)nbytes);
    }
    do_dma(direction, devAddr, vAddr, nbytes);

    /* The PI interrupt. On N64 only the PI manager listens for it, but if
     * game code registered OS_EVENT_PI itself, it hears about this too. */
    osPortPostEvent(OS_EVENT_PI);

    /* The PI manager posts with NOBLOCK too: if the game's queue is full
     * the notification is dropped, exactly as on hardware. */
    if (mq != NULL)
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 size)
{
    do_dma(direction, devAddr, dramAddr, size);
    return 0;
}

/* One 32-bit word from the cartridge bus. The ROM is big-endian; the value
 * returned is the number the N64 CPU would see, so it is assembled byte by
 * byte rather than copied (copying would give it reversed on little-endian
 * ARM). */
s32 osPiRawReadIo(u32 devAddr, u32 *data)
{
    u8 b[4];
    pal_rom_read(b, PI_ROM_OFFSET(devAddr) & ~3u, 4);
    *data = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32 *data)
{
    return osPiRawReadIo(devAddr, data);
}

u32 osPiGetStatus(void)
{
    return 0;   /* never busy: every transfer finishes inside its call */
}

/* Writes to the cartridge bus. ROM is read-only; nothing else on the bus
 * (SRAM, 64DD) is emulated, so these are dropped with a one-time log. */
s32 osPiRawWriteIo(u32 devAddr, u32 data)
{
    (void)data;
    if (!s_warned_write) {
        s_warned_write = 1;
        pal_log("PI: ignoring write to cartridge 0x%08lx", (unsigned long)devAddr);
    }
    return 0;
}

s32 osPiWriteIo(u32 devAddr, u32 data)
{
    return osPiRawWriteIo(devAddr, data);
}
