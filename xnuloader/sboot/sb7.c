/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 assets: the "SB7ASSET" reader (sb7.h, docs/MENU7-PORT.md section 4.4).
 *
 * The container is appended to the BOOT ramdisk at a 4 KiB boundary (tools/brand/menu7-assets.py)
 * and read in place: sb7_open finds it, checks its header, both CRC-32s (the A55's CRC32
 * instructions; a bitwise fallback for other builds) and the directory, and from then on
 * sb7_get is a binary search over the (type, id) sorted directory. No pointer tables, no
 * initialised statics holding addresses: only .bss.
 *
 * Also defines memcpy, memset and memmove for the FP/SIMD-compiled v7 files (GCC may emit calls
 * to them for struct copies); the preview harness (M7_PREVIEW) uses its libc's.
 */
#include <stddef.h>
#include <stdint.h>

#include "sb7.h"

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#define SB7_HEAD 32U
#define SB7_DIRENT 16U

static uintptr_t sb7_start;         /* 0 until sb7_open succeeds */
static uint32_t sb7_size, sb7_count;

static inline uint32_t rd32(uintptr_t a)
{
    return *(const uint32_t *)a;    /* every field is naturally aligned (4 KiB / 16-byte sections) */
}

static uint32_t crc32_ieee(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
#if defined(__ARM_FEATURE_CRC32)
    while (n && ((uintptr_t)p & 7)) {
        crc = __crc32b(crc, *p++);
        n--;
    }
    while (n >= 32) {
        const uint64_t *q = (const uint64_t *)p;
        crc = __crc32d(crc, q[0]);
        crc = __crc32d(crc, q[1]);
        crc = __crc32d(crc, q[2]);
        crc = __crc32d(crc, q[3]);
        p += 32;
        n -= 32;
    }
    while (n >= 8) {
        crc = __crc32d(crc, *(const uint64_t *)p);
        p += 8;
        n -= 8;
    }
    while (n--)
        crc = __crc32b(crc, *p++);
#else
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
#endif
    return ~crc;
}

/* "SB7ASSET" as two little-endian words */
#define SB7_MAGIC0 0x41374253U      /* "SB7A" */
#define SB7_MAGIC1 0x54455353U      /* "SSET" */

static int sb7_check(uintptr_t c, uintptr_t end)
{
    if (end - c < SB7_HEAD || rd32(c) != SB7_MAGIC0 || rd32(c + 4) != SB7_MAGIC1)
        return 0;
    uint32_t ver = rd32(c + 8), n = rd32(c + 12), size = rd32(c + 16);
    if (ver != SB7_VERSION || n == 0 || n > 65536U || size > end - c)
        return 0;
    uint64_t dir_end = SB7_HEAD + (uint64_t)SB7_DIRENT * n;
    if (dir_end > size)
        return 0;
    if (crc32_ieee(0, (const uint8_t *)(c + SB7_HEAD), (uint32_t)(dir_end - SB7_HEAD)) != rd32(c + 20))
        return 0;
    if (crc32_ieee(0, (const uint8_t *)(c + dir_end), size - (uint32_t)dir_end) != rd32(c + 24))
        return 0;
    /* the directory: sorted by (type, id), sections 16-byte aligned, inside the container */
    uint64_t last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t e = c + SB7_HEAD + SB7_DIRENT * i;
        uint32_t t = rd32(e), id = rd32(e + 4), off = rd32(e + 8), sz = rd32(e + 12);
        uint64_t key = ((uint64_t)t << 32) | id;
        if ((i && key <= last) || (off & 15U) || off < dir_end || (uint64_t)off + sz > size)
            return 0;
        last = key;
    }
    sb7_start = c;
    sb7_size = size;
    sb7_count = n;
    return 1;
}

int sb7_open(uintptr_t start, uintptr_t end)
{
    sb7_start = 0;
    sb7_size = sb7_count = 0;
    start &= ~(uintptr_t)(SB7_ALIGN - 1U);
    for (uintptr_t c = start; c + SB7_HEAD <= end; c += SB7_ALIGN) {
        if (rd32(c) == SB7_MAGIC0 && rd32(c + 4) == SB7_MAGIC1)
            return sb7_check(c, end);
    }
    return 0;
}

uintptr_t sb7_base(uint32_t *size)
{
    if (size)
        *size = sb7_size;
    return sb7_start;
}

/* run-time sections (sb7_put): the captured splash */
static struct {
    uint32_t type, id, size;
    uintptr_t p;
} sb7_rt[SB7_RT_MAX];
static uint32_t sb7_nrt;

int sb7_put(uint32_t type, uint32_t id, const void *p, uint32_t size)
{
    for (uint32_t i = 0; i < sb7_nrt; i++)
        if (sb7_rt[i].type == type && sb7_rt[i].id == id) {
            if (!p) {
                sb7_rt[i] = sb7_rt[--sb7_nrt];
                return 1;
            }
            sb7_rt[i].p = (uintptr_t)p;
            sb7_rt[i].size = size;
            return 1;
        }
    if (!p)
        return 1;
    if (sb7_nrt >= SB7_RT_MAX)
        return 0;
    sb7_rt[sb7_nrt].type = type;
    sb7_rt[sb7_nrt].id = id;
    sb7_rt[sb7_nrt].size = size;
    sb7_rt[sb7_nrt].p = (uintptr_t)p;
    sb7_nrt++;
    return 1;
}

const void *sb7_get(uint32_t type, uint32_t id, uint32_t *size)
{
    if (!sb7_start)
        return 0;
    for (uint32_t i = 0; i < sb7_nrt; i++)
        if (sb7_rt[i].type == type && sb7_rt[i].id == id) {
            if (size)
                *size = sb7_rt[i].size;
            return (const void *)sb7_rt[i].p;
        }
    uint64_t key = ((uint64_t)type << 32) | id;
    uint32_t lo = 0, hi = sb7_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uintptr_t e = sb7_start + SB7_HEAD + SB7_DIRENT * mid;
        uint64_t k = ((uint64_t)rd32(e) << 32) | rd32(e + 4);
        if (k == key) {
            if (size)
                *size = rd32(e + 12);
            return (const void *)(sb7_start + rd32(e + 8));
        }
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

unsigned sb7_tile_for(const char *key, unsigned category)
{
    uint32_t size;
    const struct sb7_tilemap_ent *m = sb7_get(SB7_T_TILEMAP, 0, &size);
    if (!m || !key)
        return 0;
    unsigned n = size / sizeof(*m), best = 0, best_len = 0, cat = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *k = m[i].key;
        if (!k[0]) {
            if (m[i].category == category && !cat)
                cat = m[i].tile;
            continue;
        }
        unsigned j = 0;
        while (j < sizeof(m[i].key) && k[j] && key[j] == k[j])
            j++;
        int whole = j == sizeof(m[i].key) || !k[j];
        if (!whole)
            continue;
        if (!key[j])
            return m[i].tile;                   /* exact */
        if ((m[i].flags & SB7_TM_PREFIX) && j > best_len) {
            best = m[i].tile;
            best_len = j;
        }
    }
    return best ? best : cat;
}

#ifndef M7_PREVIEW
/* Plain loops; GCC must not turn them back into calls to themselves. */
#define SB7_NOLIB __attribute__((optimize("no-tree-loop-distribute-patterns"), used))

SB7_NOLIB void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

SB7_NOLIB void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || !n)
        return d;
    if (dp < sp || dp >= sp + n) {
        while (n--)
            *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--)
            *--dp = *--sp;
    }
    return d;
}

SB7_NOLIB void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}
#endif
