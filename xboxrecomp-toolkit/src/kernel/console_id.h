/*
 * console_id.h -- the player's own console identity, for Xbox Live (Insignia).
 *
 * Xbox Live knows a console by its EEPROM: the factory section (serial number,
 * Ethernet address, online key, AV region) and the hard-drive key in its
 * encrypted section, which also signs the accounts stored on the drive. A
 * player supplies their own console's eeprom.bin; nothing here invents one.
 *
 *   RECOMP_XBOX_EEPROM      path of eeprom.bin (256 bytes; used only when the
 *                           factory and user checksums are right)
 *   RECOMP_XBOX_HDKEY       the hard-drive key, 32 hex digits, as console tools
 *                           show it (EvoX hddinfo.txt, UnleashX, ...)
 *   RECOMP_XBOX_EEPROM_KEY  or: the console BIOS's EEPROM key, 32 hex digits,
 *                           to decrypt the hard-drive key from the EEPROM
 *
 * Without an EEPROM every query answers as before. Keys, serials and addresses
 * are never logged.
 */
#ifndef CONSOLE_ID_H
#define CONSOLE_ID_H

#include <stdint.h>

/* Load and check what the environment names. Safe to call more than once. */
void xbox_ConsoleIdInit(void);

/* 1 when a valid eeprom.bin is loaded. */
int xbox_ConsoleHasEeprom(void);

/* The 256 EEPROM bytes (valid only when xbox_ConsoleHasEeprom). */
const uint8_t *xbox_ConsoleEeprom(void);

/* The hard-drive key: 1 and out filled when one is known. */
int xbox_ConsoleHdKey(uint8_t out[16]);

/* The game region from the encrypted section (0 when not decrypted). */
uint32_t xbox_ConsoleGameRegion(void);

/* The EEPROM checksum: ~(high + low) of the 64-bit sum of the dwords. */
uint32_t xbox_EepromChecksum(const uint8_t *p, unsigned bytes);

#endif
