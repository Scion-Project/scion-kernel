/* SPDX-License-Identifier: GPL-2.0-only
 * UFS from the loader: see ufs.h. Shared by EngineerMode and the OS store.
 */
#include <stdint.h>
#include "hw.h"
#include "ufs.h"

/* ---- UFS: commands from the loader (port of herolte_ufs.c, read only) ------
 * S-Boot leaves the host running with its own request list. Like the iOS
 * driver: polled, auto-hibernate off, our list in slot 0, device woken with
 * TEST UNIT READY / START STOP UNIT on the device W-LUN. DMA memory at UFS_DMA
 * (ufs.h) is non-cacheable (mmu.c): no cache maintenance. Only READ(10),
 * INQUIRY, TEST UNIT READY, START STOP UNIT and QUERY READ DESCRIPTOR are
 * sent, and one WRITE(10) of a single block: ufs_write_block, whose only
 * caller is the OS store's settings record (store.c).
 */
#define UFS_NEXUS (UFS + 0x1140)
#define U_UTRL (UFS_DMA)
#define U_UCD (UFS_DMA + 0x1000)
#define U_RESP (U_UCD + 512)
#define U_PRDT (U_UCD + 1024)

int ufs_state, ufs_why;
uint32_t ufs_ocs, ufs_resp, ufs_status, ufs_qresp;
static uint8_t ufs_tag, ufs_lun;

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static void u8w(uintptr_t a, uint8_t v)
{
    *(volatile uint8_t *)a = v;
}

/* Run the UPIU at U_UCD in slot 0. dd: 0 none, 2 device to memory; len bytes
 * into buf in 4 KiB PRDT entries (16 bytes each). 0 on success. */
static int ufs_ring(unsigned dd, uint32_t len, int scsi, uintptr_t buf)
{
    volatile uint32_t *d = (volatile uint32_t *)U_UTRL;
    d[0] = (1U << 28) | (dd << 25) | (1U << 24);
    d[1] = 0;
    d[2] = 0x0f;
    d[3] = 0;
    d[4] = (uint32_t)U_UCD;
    d[5] = 0;
    d[6] = 512U << 16 | 512U;
    d[7] = 0;
    if (len) {
        uint32_t n = 0;
        for (uint32_t done = 0; done < len; done += 4096U, ++n) {
            volatile uint32_t *e = (volatile uint32_t *)(U_PRDT + 16U * n);
            uint32_t chunk = len - done < 4096U ? len - done : 4096U;
            e[0] = (uint32_t)(buf + done);
            e[1] = (uint32_t)((buf + done) >> 32);
            e[2] = 0;
            e[3] = (chunk - 1U) & 0x3ffffU;
        }
        d[7] = 1024U << 16 | (n * 16U);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    wr(UFS_NEXUS, scsi ? rd(UFS_NEXUS) | 1U : rd(UFS_NEXUS) & ~1U);
    wr(UFS + 0x58, 1);
    uint64_t t0 = hw_ticks(), lim = hw_freq() * 2U;
    while (rd(UFS + 0x58) & 1U) {
        if (hw_ticks() - t0 > lim) {
            wr(UFS + 0x5c, 1U);
            ufs_ocs = 0xff;
            return -1;
        }
    }
    __asm__ volatile("dsb sy" : : : "memory");
    wr(UFS + 0x20, rd(UFS + 0x20));
    ufs_ocs = d[2] & 0xffU;
    ufs_resp = u8r(U_RESP);
    ufs_status = u8r(U_RESP + 7);
    ufs_qresp = u8r(U_RESP + 6);
    return ufs_ocs ? -2 : 0;
}

/* SCSI command to ufs_lun; len bytes in (dd 2) into buf, or out of it (dd 1) if write. */
static int ufs_scsi_dir(const uint8_t *cdb, unsigned cdb_len, uint32_t len, uintptr_t buf, int write)
{
    u_zero(U_UCD, 1024);
    u8w(U_UCD + 0, 0x01);
    u8w(U_UCD + 1, !len ? 0x00 : write ? 0x20 : 0x40);
    u8w(U_UCD + 2, ufs_lun);
    u8w(U_UCD + 3, ++ufs_tag);
    u8w(U_UCD + 12, (uint8_t)(len >> 24));
    u8w(U_UCD + 13, (uint8_t)(len >> 16));
    u8w(U_UCD + 14, (uint8_t)(len >> 8));
    u8w(U_UCD + 15, (uint8_t)len);
    for (unsigned i = 0; i < cdb_len; ++i)
        u8w(U_UCD + 16 + i, cdb[i]);
    int r = ufs_ring(!len ? 0U : write ? 1U : 2U, len, 1, buf);
    if (r)
        return r;
    return (ufs_resp != 0x21 || ufs_qresp || ufs_status) ? -3 : 0;
}

static int ufs_scsi_to(const uint8_t *cdb, unsigned cdb_len, uint32_t len, uintptr_t buf)
{
    return ufs_scsi_dir(cdb, cdb_len, len, buf, 0);
}

int ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len)
{
    return ufs_scsi_to(cdb, cdb_len, len, UFS_BUF);
}

/* QUERY REQUEST: READ DESCRIPTOR idn/index; descriptor bytes copied to out. */
int ufs_desc(uint8_t idn, uint8_t index, uint8_t *out, unsigned cap)
{
    u_zero(U_UCD, 1024);
    u8w(U_UCD + 0, 0x16);
    u8w(U_UCD + 3, ++ufs_tag);
    u8w(U_UCD + 5, 0x01);           /* standard read request */
    u8w(U_UCD + 12, 0x01);          /* READ DESCRIPTOR */
    u8w(U_UCD + 13, idn);
    u8w(U_UCD + 14, index);
    u8w(U_UCD + 18, 0);
    u8w(U_UCD + 19, 0xff);          /* up to 255 bytes */
    int r = ufs_ring(0, 0, 0, 0);
    if (r)
        return r;
    if (ufs_resp != 0x36 || ufs_qresp)
        return -3;
    unsigned n = (unsigned)u8r(U_RESP + 10) << 8 | u8r(U_RESP + 11);
    if (n > cap)
        n = cap;
    for (unsigned i = 0; i < cap; ++i)
        out[i] = i < n ? u8r(U_RESP + 32 + i) : 0;
    return (int)n;
}

static int read10(uint64_t lba, uint32_t count, uintptr_t dst)
{
    if (lba + count > 0xffffffffULL || count > 256)
        return -4;
    uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                        (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return ufs_scsi_to(cdb, sizeof(cdb), count * 4096U, dst);
}

int ufs_read(uint64_t lba, uint32_t count)
{
    return read10(lba, count, UFS_BUF);
}

/* 32 blocks (128 KiB, 32 PRDT entries) per command; a chunk that fails three
 * times is read again one block at a time. */
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (!ufs_up())
        return -5;
    for (uint64_t i = 0, n; i < count; i += n) {
        n = count - i < 32U ? count - i : 32U;
        int r = -1;
        for (int a = 0; a < 3 && r; ++a)
            r = read10(lba + i, (uint32_t)n, dst + i * 4096U);
        for (uint64_t j = 0; r && j < n; ++j) {
            int b = -1;
            for (int a = 0; a < 3 && b; ++a)
                b = read10(lba + i + j, 1, dst + (i + j) * 4096U);
            if (b)
                return b;
        }
    }
    return 0;
}

int ufs_write_block(uint64_t lba, uintptr_t src)
{
    if (!ufs_up() || lba > 0xfffffffeULL)
        return -4;
    uint8_t cdb[10] = { 0x2a, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                        (uint8_t)lba, 0, 0, 1, 0 };
    return ufs_scsi_dir(cdb, sizeof(cdb), 4096U, src, 1);
}

int ufs_up(void)
{
    if (ufs_state)
        return ufs_state > 0;
    uint32_t hcs = rd(UFS + 0x30);
    ufs_state = -1;
    if (!(hcs & 1U) || !(hcs & 2U)) {
        ufs_why = 1;
        return 0;
    }
    if (rd(UFS + 0x58)) {
        ufs_why = 2;
        return 0;
    }
    u_zero(UFS_DMA, 0x4000);
    wr(UFS + 0x24, 0);              /* polled */
    wr(UFS + 0x18, 0);              /* auto-hibernate off (woa Star2LteUfsHcDxe) */
    wr(UFS + 0x20, rd(UFS + 0x20));
    wr(UFS + 0x60, 0);
    wr(UFS + 0x50, (uint32_t)U_UTRL);
    wr(UFS + 0x54, 0);
    wr(UFS + 0x60, 1);
    if (rd(UFS + 0x50) != (uint32_t)U_UTRL || !(rd(UFS + 0x60) & 1U)) {
        ufs_why = 3;
        return 0;
    }
    static const uint8_t tur[6] = { 0 };
    static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    ufs_lun = 0xd0;                 /* device W-LUN */
    for (int i = 0; i < 3 && ufs_scsi(tur, 6, 0); ++i)
        ;
    for (int i = 0; i < 3 && ufs_scsi(ssu, 6, 0); ++i)
        ;
    ufs_lun = 0;
    int r = -1;
    for (int i = 0; i < 3 && (r = ufs_scsi(tur, 6, 0)); ++i)
        ;
    if (r) {
        ufs_why = 4;
        return 0;
    }
    ufs_state = 1;
    return 1;
}

/* ---- GPT on LU0 ------------------------------------------------------------- */

char gpt_name[GPT_MAX][24];
uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
int gpt_n = -1;

static uint64_t le64(uintptr_t p)
{
    uint64_t v = 0;
    for (unsigned i = 8; i; --i)
        v = v << 8 | u8r(p + i - 1);
    return v;
}

int gpt_load(void)
{
    if (gpt_n >= 0)
        return gpt_n;
    if (!ufs_up() || ufs_read(1, 1))
        return -1;
    if (le64(UFS_BUF) != 0x5452415020494645ULL)          /* "EFI PART" */
        return -1;
    uint64_t ent = le64(UFS_BUF + 72);
    uint32_t num = (uint32_t)le64(UFS_BUF + 80), esz = (uint32_t)(le64(UFS_BUF + 84) & 0xffffffffU);
    if (esz != 128 || num > 128)
        return -1;
    uint32_t blocks = (num * 128U + 4095U) / 4096U;
    if (ufs_read(ent, blocks))
        return -1;
    gpt_n = 0;
    for (uint32_t i = 0; i < num && gpt_n < GPT_MAX; ++i) {
        uintptr_t e = UFS_BUF + 128U * i;
        if (!le64(e) && !le64(e + 8))
            continue;
        unsigned k = 0;
        for (unsigned c = 0; c < 23; ++c) {
            uint8_t ch = u8r(e + 56 + 2 * c);
            if (!ch)
                break;
            gpt_name[gpt_n][k++] = (char)ch;
        }
        gpt_name[gpt_n][k] = 0;
        gpt_first[gpt_n] = le64(e + 32);
        gpt_last[gpt_n] = le64(e + 40);
        ++gpt_n;
    }
    return gpt_n;
}

int gpt_find(const char *name, uint64_t *first, uint64_t *last)
{
    if (gpt_load() <= 0)
        return 0;
    for (int i = 0; i < gpt_n; ++i) {
        unsigned k = 0;
        while (name[k] && gpt_name[i][k] == name[k])
            ++k;
        if (!name[k] && !gpt_name[i][k]) {
            *first = gpt_first[i];
            *last = gpt_last[i];
            return 1;
        }
    }
    return 0;
}
