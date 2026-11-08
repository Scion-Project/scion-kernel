/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, dualboot/xnu_usb_readback.inc), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * Original project adapter, included before blazer_usbcon_core.inc.
 * Only its worker calls storage. The USB thread copies cached RAM under lock.
 * No block-device write API, opcode selector, reset, or slot operation exists.
 */
#include <kern/locks.h>
#include <sys/hakios.blazer_ufs_exact.h>
#include "readback.c"

TUNABLE(uint32_t, hk_rb_mode, "blazer_usb_readback", 0);
static LCK_GRP_DECLARE(hk_rb_group, "hakios-readback");
static LCK_MTX_DECLARE(hk_rb_lock, &hk_rb_group);
static struct hk_rb_session hk_rb;
static uint8_t hk_rb_scratch[HK_RB_BLOCK];
static uint8_t hk_rb_wire[384]; /* USB thread only, same cap as UC_DATA_BYTES. */
static bool hk_rb_started;
static void uc_put_words(const void *, uint32_t);

/* Added to the existing UFS owner; returns failure for unready or fake media.
 * It never switches the command engine's LUN or resets its quarantine state.
 */
extern int n882_blazer_readback_lun(uint32_t *lun);
extern int n882_blazer_read10_exact(const struct n882_read10_request *, struct n882_receipt *);
extern uint64_t n882_blazer_now_us(void);

static int hk_rb_read(const struct hk_rb_job *job)
{
    struct n882_receipt receipt;
    struct n882_read10_request req = {0};
    uint64_t now = n882_blazer_now_us();
    int rc;
    if (now > UINT64_MAX - N882_MAX_BUDGET_US) return HK_RB_E_READ;
    req.abi = N882_ABI;
    req.block_bytes = HK_RB_BLOCK;
    req.absolute_lba = job->lba;
    req.block_count = 1;
    req.deadline_us = now + N882_MAX_BUDGET_US;
    req.destination = hk_rb_scratch;
    req.destination_bytes = sizeof(hk_rb_scratch);
    rc = n882_blazer_read10_exact(&req, &receipt);
    if (rc != N882_OK || receipt.abi != N882_ABI || receipt.status != N882_OK ||
        receipt.decode_status != N882_OK || receipt.quarantined ||
        receipt.dma_state != N882_DMA_RETURNED || receipt.opcode != 0x28 ||
        receipt.lun != job->lun || receipt.lba != job->lba ||
        receipt.expected_bytes != HK_RB_BLOCK || receipt.transferred_bytes != HK_RB_BLOCK ||
        receipt.copied_bytes != HK_RB_BLOCK || receipt.residual_raw != 0)
        return HK_RB_E_READ;
    return 0;
}

static void hk_rb_worker(__unused void *parameter, __unused wait_result_t result)
{
    for (;;) {
        struct hk_rb_job job;
        uint32_t lun;
        bool bound, work;
        int rc;
        lck_mtx_lock(&hk_rb_lock);
        bound = hk_rb.bound != 0;
        lck_mtx_unlock(&hk_rb_lock);
        if (!bound && n882_blazer_readback_lun(&lun) == 0) {
            lck_mtx_lock(&hk_rb_lock);
            (void)hk_rb_bind(&hk_rb, lun);
            lck_mtx_unlock(&hk_rb_lock);
        }
        lck_mtx_lock(&hk_rb_lock);
        work = hk_rb_take(&hk_rb, &job) == 0;
        lck_mtx_unlock(&hk_rb_lock);
        if (work) {
            rc = hk_rb_read(&job);
            lck_mtx_lock(&hk_rb_lock);
            (void)hk_rb_finish(&hk_rb, &job, rc, hk_rb_scratch);
            lck_mtx_unlock(&hk_rb_lock);
        }
        delay_for_interval(10, NSEC_PER_MSEC);
    }
}

static void hk_rb_start(void)
{
    thread_t thread;
    uint64_t nonce, entry;
    if (hk_rb_mode != 1 || hk_rb_started) return;
    blazer_mirror_ident(&nonce, &entry);
    hk_rb_init(&hk_rb, hk_rb_mode, nonce, entry);
    if (!hk_rb.enabled) return;
    if (kernel_thread_start(hk_rb_worker, NULL, &thread) != KERN_SUCCESS) {
        printf("blazer-readback: worker unavailable\n");
        return;
    }
    thread_set_thread_name(thread, "blazer_readback");
    thread_deallocate(thread);
    hk_rb_started = true;
    printf("blazer-readback: enabled, exact READ(10) only, bound LUN only\n");
}

/* Returns true only for our request numbers. Mode off always STALLs them.
 * SELECT has no OUT data stage: its only argument is a 32-bit raw LBA.
 */
static bool hk_rb_usb_setup(uint8_t type, uint8_t request, uint16_t value,
    uint16_t index, uint16_t length, uint32_t cap, bool *ok, uint32_t *n)
{
    size_t bytes = 0;
    if (request != HK_RB_INFO && request != HK_RB_SELECT && request != HK_RB_DATA)
        return false;
    *ok = false;
    *n = 0;
    if (!hk_rb_started || hk_rb_mode != 1) return true;
    lck_mtx_lock(&hk_rb_lock);
    if (request == HK_RB_SELECT) {
        if (type == 0x40 && length == 0)
            *ok = hk_rb_select(&hk_rb, value | (uint32_t)index << 16) == 0;
    } else if (request == HK_RB_INFO) {
        if (type == 0xc0 && value == HK_RB_MARKER && index == HK_RB_ABI && length >= HK_RB_HEADER)
            bytes = hk_rb_packet(&hk_rb, 1, 0, hk_rb_wire, cap < sizeof(hk_rb_wire) ? cap : sizeof(hk_rb_wire));
    } else if (type == 0xc0 && index == 0 && length > HK_RB_HEADER) {
        bytes = hk_rb_packet(&hk_rb, 0, value, hk_rb_wire, cap < sizeof(hk_rb_wire) ? cap : sizeof(hk_rb_wire));
    }
    lck_mtx_unlock(&hk_rb_lock);
    if (bytes) {
        uc_put_words(hk_rb_wire, (uint32_t)bytes);
        *n = (uint32_t)bytes;
        *ok = true;
    }
    return true;
}
