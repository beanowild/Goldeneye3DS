/*
 * os_eeprom.h - libultra cartridge EEPROM (src/ultra/os_eeprom.c).
 *
 * The libultra prototypes repeat the decomp's declarations so the compiler
 * catches any signature drift (see os_message.h).
 */
#ifndef ULTRA_OS_EEPROM_H
#define ULTRA_OS_EEPROM_H

#include <ultra64.h>

s32 osEepromProbe(OSMesgQueue *mq);
s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer);
s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer);
s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes);
s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes);

#endif /* ULTRA_OS_EEPROM_H */
