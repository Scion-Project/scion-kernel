/* SPDX-License-Identifier: MIT */
/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, dualboot/readback.c), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#include "readback.h"

static void rb_le(uint8_t *out, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) out[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t rb_crc(const uint8_t *data)
{
    uint32_t c = UINT32_MAX;
    for (unsigned i = 0; i < HK_RB_BLOCK; i++) {
        c ^= data[i];
        for (unsigned j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void hk_rb_init(struct hk_rb_session *s, unsigned enable, uint64_t nonce, uint64_t entry)
{
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < sizeof(*s); i++) p[i] = 0;
    s->nonce = nonce;
    s->entry = entry;
    s->enabled = enable == 1 && nonce != 0;
    s->state = HK_RB_UNAVAILABLE;
    s->result = HK_RB_E_UNAVAILABLE;
}

int hk_rb_bind(struct hk_rb_session *s, uint32_t lun)
{
    if (!s || !s->enabled) return HK_RB_E_UNAVAILABLE;
    if (lun >= 8 || s->bound) return HK_RB_E_ARGUMENT;
    s->bound = 1;
    s->lun = lun;
    s->state = HK_RB_IDLE;
    s->result = 0;
    return 0;
}

int hk_rb_select(struct hk_rb_session *s, uint32_t lba)
{
    if (!s || !s->enabled || !s->bound) return HK_RB_E_UNAVAILABLE;
    if (s->state == HK_RB_QUEUED || s->state == HK_RB_LOADING) return HK_RB_E_BUSY;
    if (s->generation == UINT64_MAX) return HK_RB_E_GENERATION;
    s->generation++;
    s->lba = lba;
    s->state = HK_RB_QUEUED;
    s->result = 0;
    s->crc = 0;
    return 0;
}

int hk_rb_take(struct hk_rb_session *s, struct hk_rb_job *job)
{
    if (!s || !job) return HK_RB_E_ARGUMENT;
    if (!s->enabled || !s->bound) return HK_RB_E_UNAVAILABLE;
    if (s->state != HK_RB_QUEUED) return HK_RB_E_BUSY;
    job->generation = s->generation;
    job->lba = s->lba;
    job->lun = s->lun;
    s->state = HK_RB_LOADING;
    return 0;
}

int hk_rb_finish(struct hk_rb_session *s, const struct hk_rb_job *job, int result,
    const uint8_t block[HK_RB_BLOCK])
{
    if (!s || !job || (!result && !block)) return HK_RB_E_ARGUMENT;
    if (!s->enabled || !s->bound) return HK_RB_E_UNAVAILABLE;
    if (s->state != HK_RB_LOADING || job->generation != s->generation ||
        job->lba != s->lba || job->lun != s->lun) return HK_RB_E_GENERATION;
    if (result) {
        s->state = HK_RB_FAILED;
        s->result = result < 0 ? result : HK_RB_E_READ;
        s->crc = 0;
        return 0;
    }
    for (unsigned i = 0; i < HK_RB_BLOCK; i++) s->block[i] = block[i];
    s->crc = rb_crc(s->block);
    s->result = 0;
    s->state = HK_RB_COMPLETE;
    return 0;
}

size_t hk_rb_packet(const struct hk_rb_session *s, unsigned info, uint32_t offset,
    uint8_t *out, size_t cap)
{
    static const uint8_t magic[8] = {'H','K','R','B','0','0','0','1'};
    size_t n = 0;
    if (!s || !out || !s->enabled || cap < HK_RB_HEADER || info > 1 ||
        (info ? offset != 0 : offset >= HK_RB_BLOCK)) return 0;
    if (!info && s->state == HK_RB_COMPLETE) {
        n = cap - HK_RB_HEADER;
        if (n > HK_RB_BLOCK - offset) n = HK_RB_BLOCK - offset;
        if (!n) return 0;
    }
    for (unsigned i = 0; i < HK_RB_HEADER; i++) out[i] = 0;
    for (unsigned i = 0; i < 8; i++) out[i] = magic[i];
    rb_le(out+8, HK_RB_ABI, 4);
    rb_le(out+12, HK_RB_HEADER, 4);
    rb_le(out+16, HK_RB_BLOCK, 4);
    rb_le(out+20, s->lun, 4);
    rb_le(out+24, s->state, 4);
    rb_le(out+28, (uint32_t)s->result, 4);
    rb_le(out+32, s->nonce, 8);
    rb_le(out+40, s->entry, 8);
    rb_le(out+48, s->generation, 8);
    rb_le(out+56, s->lba, 8);
    if (s->state == HK_RB_COMPLETE) {
        rb_le(out+64, HK_RB_BLOCK, 4);
        rb_le(out+76, s->crc, 4);
    }
    rb_le(out+68, offset, 4);
    rb_le(out+72, n, 4);
    for (size_t i = 0; i < n; i++) out[HK_RB_HEADER+i] = s->block[offset+i];
    return HK_RB_HEADER + n;
}
