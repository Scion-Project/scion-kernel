/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite OS store (store.c, written by tools/store/sbstore2.py).
 *
 * One store per medium, the same layout on each, in 4 KiB blocks from the store's start:
 *   UFS     USERDATA +16 GiB, up to the end of USERDATA (2.75 GiB: the old +17 GiB root is free)
 *   microSD MBR partition 2 (type 0xDA) of the card                       [sd.c, later]
 *
 *   block 0  "SBSTORE1": the version 1 view of the entries that fit it (name < 24, below 1 GiB),
 *            for older loaders and the iOS overlay installer (xnu/src/starlte_overlay.c)
 *   block 1  "SBSTORE2" header: u32 version 2, u32 count, u64 capacity, u64 used (blocks),
 *            u32 dir_block, u32 dir_blocks, u32 settings_block, u32 dir CRC32, u32 CRC32 of 0..47
 *   block 2  settings record "SBSETTNG" (UFS only; the loader writes it, the tools keep it)
 *   block 3  directory: 64-byte entries: char name[32], u64 block, u64 size, u32 CRC32,
 *            u16 type, u16 flags, 8 bytes zero
 *   data     4 KiB aligned
 */
#pragma once
#include <stdint.h>

#define STORE_UFS   0U
#define STORE_SD    1U
#define STORE_MEDIA 2U
#define STORE_MAX   256U       /* directory entries per medium */

/* entry types */
#define ST_DATA    0U
#define ST_IMAGE   1U          /* arm64 Image */
#define ST_DTB     2U
#define ST_INITRD  3U
#define ST_TEXT    4U          /* command line */
#define ST_XNU     5U          /* SWEETXNU payload */
#define ST_UEFI    6U          /* bootshim + FD, an arm64 Image */
#define ST_CATALOG 7U

struct store_ent {
    char name[32];
    uint64_t lba;              /* absolute 4 KiB block on the medium */
    uint64_t size;
    uint32_t crc;
    uint16_t type, flags;
    uint8_t medium;
};

/* Directory of a medium, read once: entry count, 0 without a store. */
int store_open(unsigned medium);
/* 1 or 2 once opened, 0 without a store. */
int store_version(unsigned medium);
int store_get(unsigned medium, unsigned i, struct store_ent *e);
/* By name: "ufs:" or "sd:" picks the medium, else UFS first. 1 found. */
int store_find(const char *name, struct store_ent *e);
/* Whole entry to dst (at most cap bytes), CRC checked: 0, -1 read error, -2 CRC, -3 too large. */
int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap);
/* The last store_load since store_last_clear: its result (0, -1, -2, -3; 1 = none since the clear),
 * the entry's name (cut to cap), size, the directory's CRC32 and the one computed (0 unless read).
 * For the menu's error card ("expected 0x... · read 0x..."). */
int store_last_load(char *name, unsigned cap, uint64_t *size, uint32_t *want, uint32_t *got);
void store_last_clear(void);
/* The entry's first 4 KiB block to dst (no CRC): 0 or -1. */
int store_head(const struct store_ent *e, uintptr_t dst);
/* First block, capacity and blocks in use (header + data). */
uint64_t store_base(unsigned medium);
uint64_t store_capacity(unsigned medium);
uint64_t store_used(unsigned medium);

/* Forget what store_open found, so the next call reads the medium again. */
void store_reset(unsigned medium);

/* Settings record (UFS store, block 2). Keys are catalog keys ("" = none). */
#define SET_SD_STORE 1U        /* flags: read the microSD's store at boot (sd.h) */
struct star_settings {
    uint32_t seq, timeout_s, flags;
    char default_key[24], last_key[24], next_key[24];
};
/* 1 if a valid record was read; otherwise *s = defaults (zeros) and 0. */
int settings_load(struct star_settings *s);
/* Write and read back: 0 on success. The only flash write the loader makes. */
int settings_save(struct star_settings *s);
