/*
 * os_eeprom.c - cartridge EEPROM as a file on the SD card.
 *
 * GoldenEye's cartridge has a 4 Kbit EEPROM: 64 blocks of 8 bytes,
 * addressed by block number. The whole image is kept in RAM, loaded on the
 * first access, and written back to sdmc:/3ds/ge007/eeprom.bin after every
 * write call. Saves happen only at menus, so writing straight through is
 * cheap.
 *
 * The file is the raw EEPROM image in the same layout emulators use for
 * .eep files, so saves can be moved between this port and an emulator.
 *
 * A blank EEPROM reads as 0x00. GoldenEye checksums its save blocks and
 * formats the area itself when they don't match.
 */
#include <ultra64.h>

#include "pal/pal_save.h"
#include "pal/pal_sys.h"
#include "ultra/os_eeprom.h"

#define EEP_BLOCK_SIZE 8
#define EEP_BLOCKS     64            /* EEPROM_MAXBLOCKS for 4 Kbit */
#define EEP_TYPE       EEPROM_TYPE_4K
#define EEP_FILE       "eeprom.bin"

/* Guarded by the OS lock. */
static u8  s_image[EEP_BLOCKS * EEP_BLOCK_SIZE];
static int s_loaded;

static void ensure_loaded(void)
{
    if (s_loaded)
        return;
    u8 tmp[sizeof(s_image)] = { 0 };
    pal_save_load(EEP_FILE, tmp, sizeof(tmp));   /* stays blank if missing */

    pal_os_lock();
    if (!s_loaded) {
        for (unsigned i = 0; i < sizeof(s_image); i++)
            s_image[i] = tmp[i];
        s_loaded = 1;
    }
    pal_os_unlock();
}

static int range_ok(u8 address, int nbytes)
{
    return nbytes >= 0 && (nbytes % EEP_BLOCK_SIZE) == 0 &&
           (unsigned)address * EEP_BLOCK_SIZE + (unsigned)nbytes <= sizeof(s_image);
}

s32 osEepromProbe(OSMesgQueue *mq)
{
    (void)mq;
    ensure_loaded();
    return EEP_TYPE;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    (void)mq;
    if (!range_ok(address, nbytes))
        return -1;
    ensure_loaded();

    pal_os_lock();
    const u8 *src = s_image + address * EEP_BLOCK_SIZE;
    for (int i = 0; i < nbytes; i++)
        buffer[i] = src[i];
    pal_os_unlock();
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    (void)mq;
    if (!range_ok(address, nbytes))
        return -1;
    ensure_loaded();

    /* Update the image and take a snapshot under the lock, then write the
     * file without the lock held so SD latency never stalls other threads. */
    u8 snapshot[sizeof(s_image)];
    pal_os_lock();
    u8 *dst = s_image + address * EEP_BLOCK_SIZE;
    for (int i = 0; i < nbytes; i++)
        dst[i] = buffer[i];
    for (unsigned i = 0; i < sizeof(s_image); i++)
        snapshot[i] = s_image[i];
    pal_os_unlock();

    /* If the SD write fails, the game still sees its data for this
     * session; the failure is logged and reported as an EEPROM error. */
    return pal_save_store(EEP_FILE, snapshot, sizeof(snapshot)) ? 0 : -1;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongRead(mq, address, buffer, EEP_BLOCK_SIZE);
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer)
{
    return osEepromLongWrite(mq, address, buffer, EEP_BLOCK_SIZE);
}
