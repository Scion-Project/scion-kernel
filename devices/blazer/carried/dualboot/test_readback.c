/* SPDX-License-Identifier: MIT */
/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, dualboot/test_readback.c), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#include "readback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

int main(void)
{
    struct hk_rb_session s;
    struct hk_rb_job job, wrong;
    uint8_t block[HK_RB_BLOCK], packet[384], rebuilt[HK_RB_BLOCK];
    for (unsigned i = 0; i < sizeof(block); i++) block[i] = (uint8_t)i;
    for (unsigned gate = 0; gate < 3; gate++) {
        hk_rb_init(&s, gate, 0x123, 5);
        if (gate != 1) {
            assert(hk_rb_bind(&s, 0) == HK_RB_E_UNAVAILABLE);
            assert(hk_rb_select(&s, 1) == HK_RB_E_UNAVAILABLE);
            assert(hk_rb_packet(&s, 1, 0, packet, sizeof(packet)) == 0);
            continue;
        }
        assert(hk_rb_select(&s, 1) == HK_RB_E_UNAVAILABLE);
        assert(hk_rb_bind(&s, 8) == HK_RB_E_ARGUMENT);
        assert(hk_rb_bind(&s, 0) == 0);
        assert(hk_rb_bind(&s, 1) == HK_RB_E_ARGUMENT);
        assert(hk_rb_select(&s, UINT32_MAX) == 0);
        assert(hk_rb_packet(&s, 0, 0, packet, sizeof(packet)) == HK_RB_HEADER);
        assert(le32(packet+64) == 0 && le32(packet+72) == 0);
        assert(hk_rb_take(&s, &job) == 0);
        assert(job.lba == UINT32_MAX && job.generation == 1 && job.lun == 0);
        assert(hk_rb_take(&s, &wrong) == HK_RB_E_BUSY);
        assert(hk_rb_select(&s, 3) == HK_RB_E_BUSY);
        wrong = job; wrong.generation++;
        assert(hk_rb_finish(&s, &wrong, 0, block) == HK_RB_E_GENERATION);
        wrong = job; wrong.lun++;
        assert(hk_rb_finish(&s, &wrong, 0, block) == HK_RB_E_GENERATION);
        wrong = job; wrong.lba--;
        assert(hk_rb_finish(&s, &wrong, 0, block) == HK_RB_E_GENERATION);
        assert(hk_rb_finish(&s, &job, 0, NULL) == HK_RB_E_ARGUMENT);
        assert(hk_rb_finish(&s, &job, 0, block) == 0);
        assert(hk_rb_packet(&s, 1, 0, packet, sizeof(packet)) == HK_RB_HEADER);
        assert(le32(packet+76) == 0xa2912082u);
        size_t done = 0;
        while (done < HK_RB_BLOCK) {
            size_t n = hk_rb_packet(&s, 0, (uint32_t)done, packet, sizeof(packet));
            assert(n > HK_RB_HEADER && n <= sizeof(packet));
            assert(le32(packet+68) == done && le32(packet+72) == n-HK_RB_HEADER);
            memcpy(rebuilt+done, packet+HK_RB_HEADER, n-HK_RB_HEADER);
            done += n-HK_RB_HEADER;
        }
        assert(done == HK_RB_BLOCK && memcmp(block, rebuilt, sizeof(block)) == 0);
        for (size_t cap = 0; cap < HK_RB_HEADER; cap++)
            assert(hk_rb_packet(&s, 1, 0, packet, cap) == 0);
        assert(hk_rb_packet(&s, 0, HK_RB_BLOCK, packet, sizeof(packet)) == 0);
        assert(hk_rb_packet(&s, 1, 1, packet, sizeof(packet)) == 0);
        assert(hk_rb_packet(&s, 2, 0, packet, sizeof(packet)) == 0);
        assert(hk_rb_packet(&s, 0, 0, packet, HK_RB_HEADER) == 0);
        assert(hk_rb_select(&s, 3) == 0);
        assert(hk_rb_take(&s, &job) == 0);
        assert(hk_rb_finish(&s, &job, -99, NULL) == 0);
        assert(hk_rb_packet(&s, 0, 0, packet, sizeof(packet)) == HK_RB_HEADER);
        assert(le32(packet+64) == 0 && le32(packet+72) == 0 && le32(packet+76) == 0);
        assert(s.state == HK_RB_FAILED && s.result == -99);
        s.generation = UINT64_MAX;
        assert(hk_rb_select(&s, 4) == HK_RB_E_GENERATION);
    }
    hk_rb_init(&s, 1, 0, 1);
    assert(hk_rb_bind(&s, 0) == HK_RB_E_UNAVAILABLE);
    assert(hk_rb_bind(NULL, 0) == HK_RB_E_UNAVAILABLE);
    assert(hk_rb_packet(NULL, 1, 0, packet, sizeof(packet)) == 0);
    puts("readback state, receipt binding, withheld-error-data and wire tests PASS");
    return 0;
}
