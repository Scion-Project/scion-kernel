/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite OS store: see store.h. Read only, except the UFS settings record.
 */
#include <stdint.h>
#include "store.h"
#include "ufs.h"
#include "sd.h"

#define UFS_STORE_OFFSET 4194304ULL            /* USERDATA +16 GiB, in 4 KiB blocks */
#define V1_BLOCKS        262144ULL             /* the version 1 store: 1 GiB */
#define SCRATCH          (UFS_BUF + (512U << 10))
#define SETTINGS_MAGIC   0x474e545445534253ULL /* "SBSETTNG" */

struct medium {
    int state;                 /* 0 not tried, 1 open, -1 none */
    int version;
    uint64_t base, capacity, used, settings;
    unsigned count;
    struct store_ent ent[STORE_MAX];
};
static struct medium med[STORE_MEDIA];

static uint64_t le(uintptr_t p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | *(volatile uint8_t *)(p + i - 1);
    return v;
}

/* CRC32 (IEEE, reflected, as zlib.crc32) with the ARMv8 CRC32 instructions. */
__attribute__((target("+crc"))) uint32_t star_crc32(uintptr_t p, uint64_t n)
{
    uint32_t c = 0xffffffffU;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8)
        __asm__("crc32x %w0, %w0, %x1" : "+r"(c) : "r"(*(volatile uint64_t *)(p + i)));
    for (; i < n; ++i)
        __asm__("crc32b %w0, %w0, %w1" : "+r"(c) : "r"((uint32_t)*(volatile uint8_t *)(p + i)));
    return ~c;
}

/* count 4 KiB blocks of a medium to dst (DMA-visible memory). */
static int med_read(unsigned m, uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (m == STORE_UFS)
        return ufs_read_to(lba, count, dst);
    return sd_read4k(lba, count, dst);
}

static void name_copy(char *dst, uintptr_t src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && *(volatile uint8_t *)(src + k); ++k)
        dst[k] = (char)*(volatile uint8_t *)(src + k);
    for (; k < 32; ++k)
        dst[k] = 0;
}

/* Version 2 at block 1: the header, then its directory into SCRATCH. */
static int open_v2(struct medium *s, unsigned m, uint64_t limit)
{
    if (med_read(m, s->base + 1, 1, SCRATCH) || le(SCRATCH, 8) != 0x3245524f54534253ULL ||  /* "SBSTORE2" */
        le(SCRATCH + 8, 4) != 2 || star_crc32(SCRATCH, 48) != (uint32_t)le(SCRATCH + 48, 4))
        return 0;
    uint32_t n = (uint32_t)le(SCRATCH + 12, 4), dir = (uint32_t)le(SCRATCH + 32, 4),
             dir_blocks = (uint32_t)le(SCRATCH + 36, 4), crc = (uint32_t)le(SCRATCH + 44, 4);
    s->capacity = le(SCRATCH + 16, 8);
    s->used = le(SCRATCH + 24, 8);
    s->settings = le(SCRATCH + 40, 4);
    if (s->capacity > limit || s->used > s->capacity || n > STORE_MAX || !dir_blocks ||
        dir_blocks > STORE_MAX * 64U / 4096U || n * 64U > dir_blocks * 4096U ||
        dir + dir_blocks > s->capacity)
        return 0;
    if (med_read(m, s->base + dir, dir_blocks, SCRATCH) || star_crc32(SCRATCH, n * 64ULL) != crc)
        return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uintptr_t p = SCRATCH + 64U * i;
        struct store_ent *e = &s->ent[s->count];
        uint64_t blk = le(p + 32, 8), size = le(p + 40, 8);
        if (blk > s->capacity || (size + 4095U) / 4096U > s->capacity - blk)
            continue;
        name_copy(e->name, p, 32);
        e->lba = s->base + blk;
        e->size = size;
        e->crc = (uint32_t)le(p + 48, 4);
        e->type = (uint16_t)le(p + 52, 2);
        e->flags = (uint16_t)le(p + 54, 2);
        e->medium = (uint8_t)m;
        ++s->count;
    }
    s->version = 2;
    return 1;
}

/* Version 1 at block 0 (tools/linux/sbstore.py): byte offsets, names of 24, 63 entries. */
static int open_v1(struct medium *s, unsigned m)
{
    if (med_read(m, s->base, 1, SCRATCH) || le(SCRATCH, 8) != 0x3145524f54534253ULL ||   /* "SBSTORE1" */
        le(SCRATCH + 8, 4) != 1)
        return 0;
    uint32_t n = (uint32_t)le(SCRATCH + 12, 4);
    s->capacity = V1_BLOCKS;
    s->used = le(SCRATCH + 16, 8);
    s->settings = 0;
    for (uint32_t i = 0; i < n && i < 63; ++i) {
        uintptr_t p = SCRATCH + 64 + 64U * i;
        struct store_ent *e = &s->ent[s->count];
        uint64_t off = le(p + 24, 8), size = le(p + 32, 8);
        if ((off & 4095U) || off / 4096U + (size + 4095U) / 4096U > V1_BLOCKS)
            continue;
        name_copy(e->name, p, 24);
        e->lba = s->base + off / 4096U;
        e->size = size;
        e->crc = (uint32_t)le(p + 40, 4);
        e->type = ST_DATA;
        e->flags = 0;
        e->medium = (uint8_t)m;
        ++s->count;
    }
    s->version = 1;
    return 1;
}

int store_open(unsigned m)
{
    if (m >= STORE_MEDIA)
        return 0;
    struct medium *s = &med[m];
    if (s->state)
        return s->state > 0 ? (int)s->count : 0;
    s->state = -1;
    s->count = 0;
    uint64_t first, last, limit;
    if (m == STORE_UFS) {
        if (!ufs_up() || !gpt_find("USERDATA", &first, &last) || last - first + 1 <= UFS_STORE_OFFSET + 16)
            return 0;
        s->base = first + UFS_STORE_OFFSET;
        limit = last - first + 1 - UFS_STORE_OFFSET;
    } else {
        /* The card hardware is touched only once allowed (sd.h): by the settings record or by
         * EngineerMode's microSD page, which then calls store_reset. */
        struct star_settings st;
        if (!sd_allowed() && settings_load(&st) && (st.flags & SET_SD_STORE))
            sd_allow();
        if (!sd_allowed())
            return 0;
        if (!sd_store_area(&first, &limit))
            return 0;
        s->base = first;
    }
    if (!open_v2(s, m, limit)) {
        s->count = 0;
        if (m != STORE_UFS || !open_v1(s, m))
            return 0;
    }
    s->state = 1;
    return (int)s->count;
}

void store_reset(unsigned m)
{
    if (m < STORE_MEDIA)
        med[m].state = 0;
}

/* Struct copies would call memcpy: there is no libc. */
static void ent_copy(struct store_ent *d, const struct store_ent *e)
{
    volatile const uint8_t *a = (volatile const uint8_t *)e;
    uint8_t *b = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*e); ++i)
        b[i] = a[i];
}

/* 1 if medium m has a store (possibly empty). */
static int opened(unsigned m)
{
    if (m >= STORE_MEDIA)
        return 0;
    (void)store_open(m);
    return med[m].state > 0;
}

int store_version(unsigned m)
{
    return opened(m) ? med[m].version : 0;
}

int store_get(unsigned m, unsigned i, struct store_ent *e)
{
    if (!opened(m) || i >= med[m].count)
        return 0;
    ent_copy(e, &med[m].ent[i]);
    return 1;
}

static int name_is(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 32 && a[k] && a[k] == b[k])
        ++k;
    return k == 32 || a[k] == b[k];
}

int store_find(const char *name, struct store_ent *e)
{
    unsigned from = 0, to = STORE_MEDIA;
    if (name[0] == 'u' && name[1] == 'f' && name[2] == 's' && name[3] == ':') {
        name += 4;
        to = 1;
    } else if (name[0] == 's' && name[1] == 'd' && name[2] == ':') {
        name += 3;
        from = 1;
    }
    for (unsigned m = from; m < to; ++m) {
        if (!opened(m))
            continue;
        for (unsigned i = 0; i < med[m].count; ++i)
            if (name_is(med[m].ent[i].name, name)) {
                ent_copy(e, &med[m].ent[i]);
                return 1;
            }
    }
    return 0;
}

static struct {
    int res;
    uint32_t want, got;
    uint64_t size;
    char name[32];
} last_ld = { 1, 0, 0, 0, { 0 } };

void store_last_clear(void)
{
    last_ld.res = 1;
    last_ld.name[0] = 0;
}

int store_last_load(char *name, unsigned cap, uint64_t *size, uint32_t *want, uint32_t *got)
{
    unsigned i = 0;
    for (; cap && i + 1 < cap && i < sizeof(last_ld.name) && last_ld.name[i]; ++i)
        name[i] = last_ld.name[i];
    if (cap)
        name[i] = 0;
    *size = last_ld.size;
    *want = last_ld.want;
    *got = last_ld.got;
    return last_ld.res;
}

int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    int r;
    uint32_t got = 0;
    if (e->size > cap) {
        r = -3;
    } else if (med_read(e->medium, e->lba, (e->size + 4095U) / 4096U, dst)) {
        r = -1;
    } else {
        got = star_crc32(dst, e->size);
        r = got == e->crc ? 0 : -2;
    }
    unsigned i = 0;
    for (; i + 1 < sizeof(last_ld.name) && i < sizeof(e->name) && e->name[i]; ++i)
        last_ld.name[i] = e->name[i];
    last_ld.name[i] = 0;
    last_ld.res = r;
    last_ld.size = e->size;
    last_ld.want = e->crc;
    last_ld.got = got;
    return r;
}

int store_head(const struct store_ent *e, uintptr_t dst)
{
    return med_read(e->medium, e->lba, 1, dst) ? -1 : 0;
}

uint64_t store_base(unsigned m)
{
    return opened(m) ? med[m].base : 0;
}

uint64_t store_capacity(unsigned m)
{
    return opened(m) ? med[m].capacity : 0;
}

uint64_t store_used(unsigned m)
{
    return opened(m) ? med[m].used : 0;
}

/* ---- settings record --------------------------------------------------------
 * Block 2 of the UFS store: "SBSETTNG", u32 version 1, u32 CRC32 of bytes 16..255, u32 seq,
 * u32 timeout_s, u32 flags, u32 0, char default_key[24], last_key[24], next_key[24], zeros.
 */
static uint64_t settings_lba(void)
{
    struct medium *s = &med[STORE_UFS];
    if (!opened(STORE_UFS) || s->version != 2 || s->settings != 2)
        return 0;
    return s->base + s->settings;
}

static void key_get(char *dst, uintptr_t src)
{
    for (unsigned k = 0; k < 24; ++k)
        dst[k] = (char)*(volatile uint8_t *)(src + k);
    dst[23] = 0;
}

int settings_load(struct star_settings *st)
{
    uint8_t *z = (uint8_t *)st;
    for (unsigned i = 0; i < sizeof(*st); ++i)
        z[i] = 0;
    uint64_t lba = settings_lba();
    if (!lba || ufs_read(lba, 1) || le(UFS_BUF, 8) != SETTINGS_MAGIC || le(UFS_BUF + 8, 4) != 1 ||
        star_crc32(UFS_BUF + 16, 240) != (uint32_t)le(UFS_BUF + 12, 4))
        return 0;
    st->seq = (uint32_t)le(UFS_BUF + 16, 4);
    st->timeout_s = (uint32_t)le(UFS_BUF + 20, 4);
    st->flags = (uint32_t)le(UFS_BUF + 24, 4);
    key_get(st->default_key, UFS_BUF + 32);
    key_get(st->last_key, UFS_BUF + 56);
    key_get(st->next_key, UFS_BUF + 80);
    return 1;
}

static void put(uintptr_t p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        *(volatile uint8_t *)(p + i) = (uint8_t)(v >> (8 * i));
}

static void key_put(uintptr_t p, const char *k)
{
    unsigned i = 0;
    for (; i < 23 && k[i]; ++i)
        *(volatile uint8_t *)(p + i) = (uint8_t)k[i];
    for (; i < 24; ++i)
        *(volatile uint8_t *)(p + i) = 0;
}

int settings_save(struct star_settings *st)
{
    uint64_t lba = settings_lba();
    if (!lba)
        return -1;
    ++st->seq;
    uintptr_t b = SCRATCH;
    u_zero(b, 4096);
    put(b, SETTINGS_MAGIC, 8);
    put(b + 8, 1, 4);
    put(b + 16, st->seq, 4);
    put(b + 20, st->timeout_s, 4);
    put(b + 24, st->flags, 4);
    key_put(b + 32, st->default_key);
    key_put(b + 56, st->last_key);
    key_put(b + 80, st->next_key);
    put(b + 12, star_crc32(b + 16, 240), 4);
    if (ufs_write_block(lba, b) || ufs_read(lba, 1))
        return -2;
    for (unsigned i = 0; i < 256; i += 8)
        if (*(volatile uint64_t *)(UFS_BUF + i) != *(volatile uint64_t *)(b + i))
            return -3;
    return 0;
}
