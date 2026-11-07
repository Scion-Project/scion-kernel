/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-nfc-r1/receipts/0001-hk-nfc1.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * The cli10 I2C bus (i2c@3a331000: USB repeater 0x3e, NFC 0x08, haptics 0x43): one owner at a time. Thread context only.
 * Defined in blazer_haptic.c (the bus's first user). blazer_haptic: the ID stage and each ROM effect (try: a busy bus skips the tap);
 * blazer_nfc: a whole run. */
#ifndef BLAZER_CLI10_BUS_H
#define BLAZER_CLI10_BUS_H
void blazer_cli10_enter(void);
void blazer_cli10_exit(void);
#endif
