/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-nfc-r1/receipts/0001-hk-nfc1.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * The phone's NFC controller, ST21NFC/ST54L (FDT st21nfc@8 on i2c@3a331000, the haptics bus), no XNU types: the host test
 * runs this exact logic. Stages, each the vote for the next (blazer_nfc=N):
 *   1 pads     reads only: chip enable (HSIO_N pad 1), power state (HSIO_N pad 3), host IRQ (GDMC pad 3). No CPM, no I2C.
 *   2 probe    1 + haptics' I2C votes, the block check, chip enable high (30 ms low first if it was on), address 0x08 acked,
 *              the power-up CORE_RESET_NTF read: HW, firmware and loader version, NCI version, manufacturer.
 *   3 init     2 + NCI CORE_RESET (reset config) and CORE_INIT.
 *   4 discover 3 + ST's NFC mode on, RF discovery (poll A/B/F/V) for listen_s seconds: a tag's technology and ID, then deactivate.
 * On every way out the controller is disabled and the enable pad goes back to the word it was found in (unless keep). */
#ifndef BLAZER_NFC_HW_H
#define BLAZER_NFC_HW_H
#include "blazer_hw_port.h"

enum { NF_STAGE_PADS = 1, NF_STAGE_PROBE = 2, NF_STAGE_INIT = 3, NF_STAGE_DISCOVER = 4 };

enum nf_stop {
	NF_OK = 0, NF_STOP_REFUSED_PA, NF_STOP_REFUSED_CMD, NF_STOP_REFUSED_FRAME, NF_STOP_FAULT, NF_STOP_MBA, NF_STOP_LSIOE_OFF, NF_STOP_SMC,
	NF_STOP_LPCM, NF_STOP_RATE, NF_STOP_CLI, NF_STOP_NOT_DW, NF_STOP_PAD_NOT_I2C, NF_STOP_EN_PAD, NF_STOP_BUS_BUSY, NF_STOP_BUS, NF_STOP_NAK,
	NF_STOP_NO_NTF, NF_STOP_BAD_PACKET, NF_STOP_NCI_STATUS, NF_STOP_LOADER, NF_STOP_STAGE,
};

struct nf_ops {
	int (*read32)(void *ctx, uint64_t pa, uint32_t *value);
	int (*write32)(void *ctx, uint64_t pa, uint32_t value);
	void (*smc)(void *ctx, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t r[2]);
	void (*delay_us)(void *ctx, uint32_t us);
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
	void (*cpm_enter)(void *ctx);   /* the AP's one CPM lock around each polled frame; NULL only in tests that don't care */
	void (*cpm_exit)(void *ctx);
	void (*bus_enter)(void *ctx);   /* the cli10 bus lock shared with blazer_haptic; held from the votes to the end */
	void (*bus_exit)(void *ctx);
	void *ctx;
};

struct nf_cfg {
	uint32_t stage;         /* blazer_nfc= */
	uint32_t listen_s;      /* blazer_nfc_wait=, stage 4: 1..60, default 20 */
	uint32_t keep;          /* blazer_nfc_keep=1: leave the chip enabled at the end */
};

#define NF_PKT_MAX 258u        /* 3-byte NCI header + 255 */

struct nf_pkt {
	uint32_t len;
	uint8_t b[NF_PKT_MAX];
};

struct nf {
	struct nf_ops ops;
	struct nf_cfg cfg;
	uint32_t stop, stop_detail;
	uint32_t en_dmux, en_tx, en_rx, en_param, pwr_rx, irq_param, irq_rx, abort_source;
	bool irq_usable, en_was_on, en_driven, en_dmux_set, bus_on, bus_held, addr_ack, rf_on;
	/* What the chip said */
	uint32_t irq_naks, ntf_count, reset_trigger, nci_version, manuf, hw_version, fw_version, loader_version, init_status, rf_ntfs, tags;
	uint8_t last_tech, last_proto, uid_len, uid[10];
	struct nf_pkt pkt;
};

void nf_init(struct nf *n, const struct nf_ops *ops, const struct nf_cfg *cfg);
int nf_run(struct nf *n);      /* up to cfg.stage. 0, or an NF_STOP_*. Bus released, pad restored, on every way out */
const char *nf_stop_name(uint32_t stop);
#endif
