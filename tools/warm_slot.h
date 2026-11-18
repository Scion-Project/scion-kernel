// SPDX-License-Identifier: MIT-0
// tools/warm_slot.c: the warm restart's sizes of BOOT's kernel slot (the XNU fork's bsd/dev/starlte_warm.c).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../xnuloader/sboot/scion_reentry.h"

#define WARM_PG              4096ULL
#define WARM_SLOT_PA         0x80080000ULL     /* where S-Boot puts the kernel slot */
#define WARM_TWRP_K_PA       0x84000000ULL     /* the end of the slot's room (a TWRP kernel goes here) */
#define WARM_LOADER_RAM_BASE 0x92000000ULL     /* xnu_boot.c RAM_BASE: the payload must end below it */

/* NULL when slot `s` of BOOT's kernel_size `ksize` can be re-staged at `base` (below `limit`); the Image header's
 * image_size in *declared, the SWEETXNU payload's offset in *payload_off. Else why not (maybe in `why`). */
const char *warm_slot(const uint8_t *s, uint64_t ksize, uint64_t base, uint64_t limit, uint64_t *declared,
    uint64_t *payload_off, char *why, size_t whylen);
