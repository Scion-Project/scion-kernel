// SPDX-License-Identifier: MIT-0
// The warm restart's sizes of BOOT's kernel slot, as the XNU fork's bsd/dev/starlte_warm.c warm_slot() computes them
// (keep the two the same), for the host test (tools/warm_restart_test.c); not built into the loader.
//
// S-Boot loads the BOOT header's kernel_size bytes of the kernel slot to 0x80080000. The arm64 Image header's
// image_size is only how much room it keeps there: tools/ios27/starlte-pack-boot.py sets it to the larger of the slot
// + 64 KiB and the template's (the TWRP kernel's 0x2842000), so it is always larger than the slot. A running kernel
// that starts the loader again therefore reads, hashes and copies kernel_size bytes (whole pages), and checks
// image_size only for what the loader's cold boot needs of it: at least the slot, ending below RAM_BASE (xnu_boot.c
// boot_payload looks for the payload in [_image_end, base + image_size)).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "warm_slot.h"

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

const char *
warm_slot(const uint8_t *s, uint64_t ksize, uint64_t base, uint64_t limit, uint64_t *declared, uint64_t *payload_off,
    char *why, size_t whylen)
{
	uint64_t copy = (ksize + WARM_PG - 1) & ~(WARM_PG - 1);

	*declared = 0;
	*payload_off = 0;
	if (ksize < 0x1000 || ksize > limit - base || copy > limit - base) {
		snprintf(why, whylen, "BOOT's kernel slot (%llu bytes) does not fit 0x%llx..0x%llx",
		    (unsigned long long)ksize, (unsigned long long)base, (unsigned long long)limit);
		return why;
	}
	if (rd32(s + 56) != 0x644d5241U) {
		return "BOOT's kernel slot is not an arm64 Image";
	}
	if (memcmp(s + SCNR_MARK_OFFSET, "SCNR", 4) != 0 || rd32(s + SCNR_MARK_OFFSET + 4) != SCNR_VERSION) {
		return "BOOT's loader has no warm entry (an older loader: make the boot image with this one)";
	}
	*declared = rd64(s + 16);
	if (*declared < ksize || *declared > WARM_LOADER_RAM_BASE - base) {
		snprintf(why, whylen, "the loader's Image header image_size 0x%llx is below the slot's %llu bytes or past "
		    "0x%llx: the loader would not see the payload as it does on a cold boot", (unsigned long long)*declared,
		    (unsigned long long)ksize, (unsigned long long)WARM_LOADER_RAM_BASE);
		return why;
	}
	for (uint64_t o = 0x10000; o + 0x2a8 <= ksize; o += WARM_PG) {
		const uint8_t *p = s + o;
		if (memcmp(p, "SWEETXNU", 8) == 0 && rd32(p + 8) == 1 && rd32(p + 12) >= 1 && rd32(p + 12) <= 32) {
			*payload_off = o;
			return NULL;
		}
	}
	return "no SWEETXNU payload after the loader";
}
