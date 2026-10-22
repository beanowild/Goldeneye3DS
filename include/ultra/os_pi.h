/*
 * os_pi.h - libultra Peripheral Interface (src/ultra/os_pi.c).
 *
 * On N64 the PI DMAs cartridge ROM into RDRAM. Here the "cartridge" is the
 * user's ROM file on the SD card (see pal_rom.h). The libultra prototypes
 * repeat the decomp's declarations so the compiler catches any signature
 * drift (see os_message.h).
 */
#ifndef ULTRA_OS_PI_H
#define ULTRA_OS_PI_H

#include <ultra64.h>

void         osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt);
OSMesgQueue *osPiGetCmdQueue(void);
s32          osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr,
                          void *vAddr, u32 nbytes, OSMesgQueue *mq);
s32          osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 size);
s32          osPiReadIo(u32 devAddr, u32 *data);
s32          osPiRawReadIo(u32 devAddr, u32 *data);
s32          osPiWriteIo(u32 devAddr, u32 data);
s32          osPiRawWriteIo(u32 devAddr, u32 data);
u32          osPiGetStatus(void);

/* Port-only init lives in ultra/os_port.h, which main.c can include
 * without pulling in <ultra64.h>. */

#endif /* ULTRA_OS_PI_H */
