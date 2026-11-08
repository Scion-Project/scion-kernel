/* SPDX-License-Identifier: GPL-2.0-only
 * Starting an image that is already in RAM (xnu_boot.c star_boot_ram), for the loader's
 * fastboot (fastboot_dev.c). The same placements and handoff state as the catalog's start paths
 * (boot_xnu / boot_linux / boot_android / boot_uefi / boot_bare): the caller returns the entry
 * from star_prepare_xnu and xnu_boot.S enters it with x0 = log[17].
 */
#pragma once
#include <stdint.h>

struct star_ram {
    unsigned method;                  /* catalog.h M_XNU .. M_BARE */
    uintptr_t kernel, ramdisk, dtb;   /* where they lie (fastboot's download buffer) */
    uint64_t kernel_size, ramdisk_size, dtb_size;
    const char *cmdline;              /* appended to the OS's own ("" for none) */
};

/* The download buffer: xnu_boot.c's STAGE (an XNU payload is copied from there, a UEFI image
 * is entered there), free while the loader runs. */
#define STAR_RAM_DL     0x8c000000UL
#define STAR_RAM_DL_END 0x91000000UL

/* Entry PA (log page set up for xnu_boot.S), or 0 with *why. fdt: S-Boot's. */
uintptr_t star_boot_ram(const struct star_ram *r, uintptr_t fdt, const char **why);
