/* SPDX-License-Identifier: MIT */
/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-phaseb-r1/pwr/patches/0832-blazer-haptic-native-rom.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#ifndef BLAZER_HAPTIC_SERVICE_H
#define BLAZER_HAPTIC_SERVICE_H
#include <stdint.h>
/* Native kernel ROM effect API, thread context only. Index 0..38, duration_ms
 * 1..100 stop deadline. Amplitude 0 = silence, 10000 = uncalibrated ROM nominal;
 * intermediate amplitudes return ENOTSUP. Success is command/cleanup success,
 * never physical-motion confirmation. Default compile gate and boot gate off.
 * EBUSY if another request owns controller; ENXIO if unready; EIO latches off.
 */
int blazer_haptic_play_rom(uint32_t index, uint32_t amplitude, uint32_t duration_ms);
#endif
