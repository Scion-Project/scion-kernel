/* SPDX-License-Identifier: GPL-2.0-only
 * The boot catalog (catalog.c): every bootable entry, from the OS store's "catalog" entry
 * (tools/store/sbstore2.py from tools/store/catalog.toml), or built in when the store has none.
 *
 * "SBCATLG1", u32 version 1, u32 count, u32 entry size (64), u32 string pool offset, u32 total
 * size, u32 0; then count x 64 bytes: char key[24], u8 category, u8 method, u8 tag, u8 icon,
 * u16 flags, u16 order, u32 string offsets (from the catalog start, 0 = empty) of name, version,
 * file[4], cmdline, root. Strings are NUL terminated.
 *
 * A file field names store entries ("ufs:" / "sd:" pick the medium), with alternatives separated
 * by '|'; "@boot" is the SWEETXNU payload in BOOT, "@bundle" the "SBUEFI!!" bundle in BOOT.
 * Files by method: xnu payload; linux Image, DTB, initrd (optional); android kernel, ramdisk,
 * cmdline file (optional); uefi image; bare Image. cmdline is appended to the OS's own.
 */
#pragma once
#include <stdint.h>

#define CAT_MAX 32U

#define CAT_APPLE     0U
#define CAT_MICROSOFT 1U
#define CAT_ANDROID   2U
#define CAT_LINUX     3U
#define CAT_STARBITE  4U
#define CAT_OTHER     5U
#define CAT_LAB       6U

#define M_XNU     1U
#define M_LINUX   2U
#define M_ANDROID 3U
#define M_UEFI    4U
#define M_BARE    5U   /* arm64 Image entered with x0 = S-Boot's FDT, the screen as it is */

#define TAG_NONE         0U
#define TAG_DAILY        1U
#define TAG_BETA         2U
#define TAG_EXPERIMENTAL 3U
#define TAG_LAB          4U
#define TAG_STABLE       5U

#define CF_HIDDEN 1U

struct cat_ent {
    char key[24];
    char name[32];
    char version[48];
    char file[4][64];
    char cmdline[512];
    char root[64];
    uint8_t category, method, tag, icon;
    uint16_t flags, order;
};

/* Read the catalog (once): the entry count. */
int cat_load(void);
unsigned cat_count(void);
const struct cat_ent *cat_get(unsigned i);
/* Index of the entry with this key, or -1. */
int cat_find(const char *key);
/* 1 when the store had no catalog and the built-in one is in use. */
int cat_builtin(void);
/* Alternative k of a file field ("a|b") into out (64 bytes): 1 if there is one. */
int cat_alt(const char *field, unsigned k, char *out);

/* xnu_boot.c: 1 if every file the entry needs is there; *why says what is missing. */
int star_entry_ready(unsigned i, const char **why);
