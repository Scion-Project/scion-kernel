/* SPDX-License-Identifier: MIT-0
 * The warm restart's re-entry record (docs/ios27/WARM-RESTART.md, "The contract"): what a running kernel hands the
 * loader when it starts it again without a reset of the SoC. One contract for the loaders of this project: this file
 * (the loader the test phones run, xnuloader/sboot) and Scion-Project/scion-boot-exynos9810 include/boot.h (struct
 * scion_reentry) must agree byte for byte; the XNU fork writes it (bsd/dev/starlte_warm.c).
 *
 * The kernel enters the loader Image at its base + 0x40 (SCNR_ENTRY_OFFSET), x0 = the record's physical address,
 * x1-x3 = 0, at EL1, MMU and data cache off, the data cache cleaned to the point of coherency for everything the
 * loader reads, the instruction cache invalidated, DAIF masked, one CPU running. A loader that has this entry carries
 * "SCNR" and the version at base + 0x44 and + 0x48 (the kernel looks before it lets go).
 *
 * Plain C over bytes (no loader state): scnr_check() is built on the host too (tools/warm-restart-test.sh).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SCNR_MAGIC        0x524e4353U  /* "SCNR" */
#define SCNR_VERSION      1U
#define SCNR_SIZE         728U
#define SCNR_ENTRY_OFFSET 0x40U
#define SCNR_MARK_OFFSET  0x44U
#define SCNR_LINE         608U         /* the boot-args' length, as XNU's boot_args.CommandLine */

/* flags */
#define SCNR_F_DIRECT     (1U << 0)    /* no menu: start what the record names */
#define SCNR_F_LINE       (1U << 1)    /* cmdline is used: it replaces the payload's own line ... */
#define SCNR_F_MENU       (1U << 2)    /* come back to the menu instead (not implemented by xnuloader/sboot) */
#define SCNR_F_RESTAGED   (1U << 3)    /* the kernel put the Image (and its payload) where S-Boot does */
#define SCNR_F_WDT_ARMED  (1U << 4)    /* the cluster-1 watchdog runs: the loader stops it before the OS starts ... */
#define SCNR_F_WDT_KEEP   (1U << 5)    /* ... unless this is set too (an OS that feeds or stops it itself) */
#define SCNR_F_APPEND     (1U << 6)    /* ... or, with this too, is appended to it (without SCNR_F_LINE: ignored) */

/* method */
#define SCNR_M_XNU        0U           /* the SWEETXNU payload appended to the Image */
#define SCNR_M_ANDROID    1U           /* an Android kernel + ramdisk (TWRP), with the FDT at fdt_pa */

/* reason */
#define SCNR_R_USER       0U
#define SCNR_R_PANIC      1U
#define SCNR_R_WATCHDOG   2U
#define SCNR_R_TEST       3U

struct scion_reentry {
	uint32_t magic;             /* 0x000 SCNR_MAGIC */
	uint16_t version;           /* 0x004 SCNR_VERSION */
	uint16_t size;              /* 0x006 SCNR_SIZE (a later version only grows) */
	uint32_t reason;            /* 0x008 SCNR_R_* */
	uint32_t flags;             /* 0x00c SCNR_F_* */
	uint32_t entry;             /* 0x010 the payload's index in BOOT, ~0 = the default (xnuloader/sboot: always ~0) */
	uint32_t crc32;             /* 0x014 IEEE CRC-32 of the first `size` bytes with this field 0 */
	char cmdline[SCNR_LINE];    /* 0x018 NUL-terminated */
	uint32_t method;            /* 0x278 SCNR_M_* */
	uint32_t wdt_ms;            /* 0x27c how long the watchdog was armed for (information) */
	uint64_t image_pa;          /* 0x280 the loader Image the kernel entered (its base) */
	uint64_t image_size;        /* 0x288 the Image with its appended payload, as BOOT's kernel slot */
	uint64_t kernel_pa;         /* 0x290 SCNR_M_ANDROID: the Android kernel (arm64 Image) */
	uint64_t kernel_size;       /* 0x298 */
	uint64_t ramdisk_pa;        /* 0x2a0 SCNR_M_ANDROID: its ramdisk */
	uint64_t ramdisk_size;      /* 0x2a8 */
	uint64_t fdt_pa;            /* 0x2b0 S-Boot's FDT as the cold boot saved it (SCNR_FDT_SAVE + 0x1000), 0 = none */
	uint8_t image_sha256[32];   /* 0x2b8 SHA-256 of what is appended to the Image: [image_pa + the Image's own size
	                             * (its _image_end, where the SWEETXNU payload starts), image_pa + image_size), as
	                             * the kernel staged it. The Image's own bytes are not covered: the loader runs in
	                             * them and writes its data and stack there before it can look. */
};

_Static_assert(offsetof(struct scion_reentry, cmdline) == 0x18, "scion_reentry layout");
_Static_assert(offsetof(struct scion_reentry, method) == 0x278, "scion_reentry layout");
_Static_assert(offsetof(struct scion_reentry, image_pa) == 0x280, "scion_reentry layout");
_Static_assert(offsetof(struct scion_reentry, fdt_pa) == 0x2b0, "scion_reentry layout");
_Static_assert(sizeof(struct scion_reentry) == SCNR_SIZE, "scion_reentry size");

/* S-Boot's FDT as the cold-boot loader keeps it for a later warm start into Android (only with sweetwarm= on the
 * payload's line): 0x91100000..0x91200000 is in no OS's RAM (tools/ios27/adt-extra-ram.py leaves 0x8F000000-0x91FFFFFF
 * out of XNU's) and unused by the loader (fastboot_dev.c: USB DMA ends at 0x91050000). Layout: u64 "SCNRFDT1", u64
 * size, the FDT from +0x1000. */
#define SCNR_FDT_SAVE      0x91100000UL
#define SCNR_FDT_SAVE_END  0x91200000UL
#define SCNR_FDT_MAGIC     0x31544446524e4353ULL   /* "SCNRFDT1" */

uint32_t scnr_crc32(const void *data, size_t len);
void scnr_sha256(const void *data, size_t len, uint8_t out[32]);
/* 0 when the record is one this loader takes for an Image at `base` (its own); else why not, in capitals for the
 * screen. The image's hash is not checked here (scnr_image_ok). */
const char *scnr_check(const struct scion_reentry *r, uint64_t base);
/* 1 when SHA-256 of [image + own, image + r->image_size) matches the record's (own: the Image's own size) */
int scnr_image_ok(const struct scion_reentry *r, const void *image, uint64_t own);
