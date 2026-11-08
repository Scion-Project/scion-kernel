/* SPDX-License-Identifier: GPL-2.0-only
 * UFS from the loader (ufs.c): commands on S-Boot's running host, used by
 * EngineerMode (em.c) and the OS store (store.c). Read only, except the store's
 * settings record.
 */
#pragma once
#include <stdint.h>

#define UFS 0x11120000UL
/* DMA memory: the top of the loader's free window (xnu_boot.c RAM_BASE ..
 * RAM_END), clear of kernels loaded below it. */
#define UFS_DMA 0xaf000000UL
#define UFS_BUF (UFS_DMA + 0x4000)
#define UFS_BUF_SIZE (1U << 20)

extern int ufs_state;           /* 0 untouched, 1 ready, -1 failed */
extern int ufs_why;             /* 1 host not ready, 2 busy, 3 list refused, 4 no answer */
extern uint32_t ufs_ocs, ufs_resp, ufs_status, ufs_qresp;

int ufs_up(void);
/* SCSI command to LU0; len bytes in, into UFS_BUF. */
int ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len);
/* QUERY READ DESCRIPTOR; returns the bytes copied or < 0. */
int ufs_desc(uint8_t idn, uint8_t index, uint8_t *out, unsigned cap);
/* READ(10) of 4 KiB blocks into UFS_BUF (count <= 256). */
int ufs_read(uint64_t lba, uint32_t count);
/* READ(10) of 4 KiB blocks straight into RAM at dst (DMA, 128 KiB per command). */
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst);
/* WRITE(10) of one 4 KiB block from src: only for the store's settings record (store.c). */
int ufs_write_block(uint64_t lba, uintptr_t src);

static inline uint8_t u8r(uintptr_t a)
{
    return *(volatile uint8_t *)a;
}

static inline void u_zero(uintptr_t a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 8)
        *(volatile uint64_t *)(a + i) = 0;
}

/* LU0 GPT, loaded once. */
#define GPT_MAX 48
extern char gpt_name[GPT_MAX][24];
extern uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
extern int gpt_n;
int gpt_load(void);
/* Partition by GPT name: 1 found. */
int gpt_find(const char *name, uint64_t *first, uint64_t *last);
