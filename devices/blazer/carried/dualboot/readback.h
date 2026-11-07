/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, dualboot/readback.h), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * Original project code. Read-only block export; no storage command encoder.
 * The adapter serializes access and validates real exact-read receipts.
 */
#ifndef HK_READBACK_H
#define HK_READBACK_H
#include <stddef.h>
#include <stdint.h>

#define HK_RB_BLOCK 4096u
#define HK_RB_HEADER 80u
#define HK_RB_INFO 0x64u
#define HK_RB_SELECT 0x65u
#define HK_RB_DATA 0x66u
#define HK_RB_MARKER 0x5242u
#define HK_RB_ABI 1u

enum hk_rb_state {
    HK_RB_UNAVAILABLE, HK_RB_IDLE, HK_RB_QUEUED, HK_RB_LOADING,
    HK_RB_COMPLETE, HK_RB_FAILED
};
enum hk_rb_error {
    HK_RB_OK = 0, HK_RB_E_ARGUMENT = -1, HK_RB_E_UNAVAILABLE = -2,
    HK_RB_E_BUSY = -3, HK_RB_E_GENERATION = -4, HK_RB_E_READ = -5
};
struct hk_rb_job { uint64_t generation, lba; uint32_t lun; };
struct hk_rb_session {
    uint64_t nonce, entry, generation, lba;
    uint32_t enabled, bound, lun, state, crc;
    int32_t result;
    uint8_t block[HK_RB_BLOCK];
};
void hk_rb_init(struct hk_rb_session *, unsigned enable, uint64_t nonce, uint64_t entry);
int hk_rb_bind(struct hk_rb_session *, uint32_t lun);
int hk_rb_select(struct hk_rb_session *, uint32_t lba);
int hk_rb_take(struct hk_rb_session *, struct hk_rb_job *);
int hk_rb_finish(struct hk_rb_session *, const struct hk_rb_job *, int result,
    const uint8_t block[HK_RB_BLOCK]);
/* Wire bytes are explicitly little endian, independent of C structure layout.
 * INFO: offset=0, info=1. DATA: info=0, offset in [0,4096). On any non-complete
 * state the header has total/count/crc zero and no cached bytes are exposed.
 */
size_t hk_rb_packet(const struct hk_rb_session *, unsigned info, uint32_t offset,
    uint8_t *destination, size_t capacity);
#endif
