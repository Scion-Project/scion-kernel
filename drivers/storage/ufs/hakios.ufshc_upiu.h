/*
 * The bytes of a UFS command and the one check every command passes before it reaches the flash, in plain C with no
 * kernel headers, so that tools/ufshc_upiu_test.c checks them on a computer (tools/ufshc-upiu-test.sh). Nothing in here
 * touches hardware, keeps state or says anything: hakios.ufshc.c includes it, builds its commands with it and asks
 * hc_refusal() of every one before it goes into a slot (ufshc_permitted), and says the reason when one is refused.
 *
 *   UTRD, UPIU    the request descriptor's first word (command type, data direction) and the UPIU's transaction
 *                 types, flags, well-known units and query functions, as JESD220 and JESD223 give them; BLOCK, the
 *                 logical block size of every unit this driver reads or writes
 *   the policy    struct hc_policy: what may be sent now -- the units reads may go to, own mode's control commands,
 *                 the one window writes may go into, and ufs=3's fences (read only, units never written, ranges of
 *                 blocks never written: struct hc_deny)
 *   the check     hc_refusal(): NULL when a command may be sent, else why not; hc_protected(), the fences of a write
 *   the builders  hc_make_rw (READ or WRITE, (10) while it fits, (16) beyond), hc_make_scsi, hc_make_tur,
 *                 hc_make_query, and the big-endian helpers they and the check use
 *
 * Moved here unchanged from hakios.ufshc.c (its head says where each fact comes from); the code is that file's.
 */
#ifndef _HAKIOS_UFSHC_UPIU_H_
#define _HAKIOS_UFSHC_UPIU_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "hakios.ufshc.h"

/* the request descriptor's first word: command type, data direction */
#define UTRD_CT_MASK        0xf0000000u
#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_MASK        (3u << 25)
#define UTRD_DD_NONE        (0u << 25)
#define UTRD_DD_OUT         (1u << 25)          /* host to device: own mode's window only */
#define UTRD_DD_IN          (2u << 25)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_NOP_OUT        0x00
#define UPIU_COMMAND        0x01
#define UPIU_QUERY          0x16
#define UPIU_NOP_IN         0x20
#define UPIU_RESPONSE       0x21
#define UPIU_QUERY_RSP      0x36
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20
#define LUN_WELL_KNOWN      0x80
#define LUN_DEVICE          0xd0                /* the UFS device well-known unit */
#define QUERY_READ          0x01                /* query functions */
#define QUERY_WRITE         0x81
#define QOP_READ_DESC       0x01
#define QOP_READ_ATTR       0x03
#define QOP_READ_FLAG       0x05
#define QOP_SET_FLAG        0x06
#define QFLAG_DEVICE_INIT   0x01
#define QATTR_POWER_MODE    0x02
#define QDESC_UNIT          0x02
#define POWER_ACTIVE        0x11

#define BLOCK               4096u               /* the logical block size of every unit here */

struct hc_cmd {
	uint8_t upiu[32];
	uint32_t dw0;
};

/* what may be sent now (hc_refusal) */
struct hc_policy {
	uint32_t read_luns;                     /* the general units reads may go to */
	bool control;                           /* own mode: NOP, query reads, START STOP UNIT (ACTIVE) to the device */
	bool device_init;                       /* own mode, enabling QEMU's controller: SET FLAG fDeviceInit */
	bool window;                            /* own mode: writes into [w_first, w_first + w_blocks) of w_lun */
	uint8_t w_lun;
	uint64_t w_first, w_blocks;
	/*
	 * ufs=3 (hakios.ufs.c): checked on every write and SYNCHRONIZE CACHE after the window's own
	 * check, so that a window opened wrong can still not reach them. ro: no write at all
	 * (ufs_native_ro=1); deny_luns: whole logical units never written (every unit but the one
	 * USERDATA is on: the others hold the boot loaders); deny: ranges of blocks never written,
	 * whatever the window says (LBA 0-5, both GPTs, EFS, RADIO, BOOT and the rest, by name).
	 */
	bool ro;
	uint32_t deny_luns;
	const struct hc_deny *deny;
	uint32_t ndeny;
};

/* a range of blocks no write may touch (ufshc_deny) */
struct hc_deny {
	uint8_t lun;
	uint64_t first, blocks;
	char what[40];                          /* "protected: EFS", said in every refusal */
};

static uint32_t
be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
be64(const volatile uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/*
 * ufs=3's second fence for a write (or SYNCHRONIZE CACHE, blocks 0): read only, a logical unit
 * never written, or any block of a range never written -- each refused with its name. Written
 * without sums that can wrap.
 */
static const char *
hc_protected(const struct hc_policy *p, uint8_t lun, uint64_t lba, uint64_t blocks)
{
	if (p->ro) {
		return "a write, and everything is read only (ufs_native_ro=1, or no write asked for)";
	}
	if (lun >= UFSHC_LUNS || (p->deny_luns & (1u << lun))) {
		return "a write to a logical unit that is never written (a boot unit, or not USERDATA's)";
	}
	for (uint32_t i = 0; i < p->ndeny; i++) {
		const struct hc_deny *d = &p->deny[i];

		if (d->lun != lun) {
			continue;
		}
		/* [lba, lba + blocks) and [first, first + blocks) meet; a write of no blocks still names its first */
		if (lba >= d->first ? lba - d->first < d->blocks : d->first - lba < (blocks == 0 ? 1 : blocks)) {
			return d->what;
		}
	}
	return NULL;
}

static const char *
hc_refusal(const uint8_t *upiu, uint32_t dw0, const struct hc_policy *p, bool any_command_type)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;
	uint64_t expected = be32(upiu + 12);
	uint8_t type = upiu[0] & 0x3f, flags = upiu[1], lun = upiu[2];

	if (dd == UTRD_DD_MASK) {
		return "data direction 3";
	}
	if (p->ro && (dd == UTRD_DD_OUT || (type == UPIU_COMMAND && (flags & UPIU_FLAG_WRITE)))) {
		return "data to the device, and everything is read only (ufs_native_ro=1, or no write asked for)";
	}
	if (!any_command_type && (dw0 & UTRD_CT_MASK) != UTRD_CT_UFS) {
		return "not a UFS storage command type";
	}
	if (type == UPIU_NOP_OUT) {
		if (!p->control) {
			return "NOP OUT (own mode only)";
		}
		return dd == UTRD_DD_NONE && flags == 0 ? NULL : "NOP OUT with data or flags";
	}
	if (type == UPIU_QUERY) {
		if (!p->control) {
			return "a query (own mode only)";
		}
		if (dd != UTRD_DD_NONE || flags != 0 || upiu[10] != 0 || upiu[11] != 0) {
			return "a query with data";
		}
		if (upiu[5] == QUERY_READ && (upiu[12] == QOP_READ_DESC || upiu[12] == QOP_READ_ATTR ||
		    upiu[12] == QOP_READ_FLAG)) {
			return NULL;
		}
		if (upiu[5] == QUERY_WRITE && upiu[12] == QOP_SET_FLAG && upiu[13] == QFLAG_DEVICE_INIT && p->device_init) {
			return NULL;
		}
		return "a query that writes (only fDeviceInit, only while enabling QEMU's controller)";
	}
	if (type != UPIU_COMMAND) {
		return "not a SCSI command, query or NOP (task management, ...)";
	}
	if (upiu[4] & 0x0f) {
		return "not the SCSI command set";
	}
	if ((flags & UPIU_FLAG_READ) && (flags & UPIU_FLAG_WRITE)) {
		return "both the R and the W flag";
	}
	/* the device's well-known unit: START STOP UNIT to ACTIVE, nothing else; no other well-known unit */
	if (lun == LUN_DEVICE && p->control && cdb[0] == 0x1b) {
		return dd == UTRD_DD_NONE && flags == 0 && cdb[1] == 0 && cdb[2] == 0 && cdb[3] == 0 && cdb[4] == 0x10 &&
		       cdb[5] == 0 ? NULL : "START STOP UNIT other than to ACTIVE";
	}
	if (lun & LUN_WELL_KNOWN) {
		return "a well-known logical unit";
	}
	if (lun >= UFSHC_LUNS) {
		return "not a general logical unit";
	}
	switch (cdb[0]) {
	case 0x28:      /* READ(10) */
	case 0x88: {    /* READ(16) */
		uint64_t blocks = cdb[0] == 0x28 ? ((uint64_t)cdb[7] << 8 | cdb[8]) : be32(cdb + 10);

		if ((p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		if (dd != UTRD_DD_IN || (flags & UPIU_FLAG_READ) == 0 || (flags & UPIU_FLAG_WRITE)) {
			return "a read without data from the device";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a read whose length is not what the UPIU expects";
		}
		return NULL;
	}
	case 0x2a:      /* WRITE(10) */
	case 0x8a: {    /* WRITE(16) */
		uint64_t lba = cdb[0] == 0x2a ? be32(cdb + 2) : be64(cdb + 2);
		uint64_t blocks = cdb[0] == 0x2a ? ((uint64_t)cdb[7] << 8 | cdb[8]) : be32(cdb + 10);

		if (!p->window) {
			return "a write, and no window is open";
		}
		if (lun != p->w_lun) {
			return "a write to a logical unit other than the window's";
		}
		if (dd != UTRD_DD_OUT || (flags & UPIU_FLAG_WRITE) == 0 || (flags & UPIU_FLAG_READ)) {
			return "a write without data to the device";
		}
		if ((cdb[1] & ~0x08) != 0) {
			return "a write with options other than FUA";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a write whose length is not what the UPIU expects";
		}
		if (lba < p->w_first || lba - p->w_first >= p->w_blocks || blocks > p->w_blocks - (lba - p->w_first)) {
			return "a write outside the window";
		}
		return hc_protected(p, lun, lba, blocks);
	}
	case 0x35:      /* SYNCHRONIZE CACHE(10) */
		if (!p->window || lun != p->w_lun) {
			return "SYNCHRONIZE CACHE outside the window's unit";
		}
		if (p->ro || (p->deny_luns & (1u << lun))) {
			return "SYNCHRONIZE CACHE, and nothing is written (read only, or a unit never written)";
		}
		return dd == UTRD_DD_NONE && flags == 0 && expected == 0 ? NULL : "SYNCHRONIZE CACHE with data";
	case 0x00:      /* TEST UNIT READY */
		if (!p->control && (p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		return dd == UTRD_DD_NONE && expected == 0 && flags == 0 ? NULL : "TEST UNIT READY with data";
	case 0x03:      /* REQUEST SENSE */
	case 0x12:      /* INQUIRY */
	case 0x25:      /* READ CAPACITY(10) */
	case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
		if (!p->control && (p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		if (dd == UTRD_DD_OUT || (flags & UPIU_FLAG_WRITE)) {
			return "data to the device";
		}
		return cdb[0] != 0x9e || (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
	default:
		return "a SCSI operation not on the list";
	}
}

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* READ or WRITE of blocks from lba: (10) while it fits, (16) beyond */
static void
hc_make_rw(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint64_t lba, uint32_t blocks, bool write)
{
	uint8_t *cdb = c->upiu + 16;

	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = write ? UPIU_FLAG_WRITE : UPIU_FLAG_READ;  /* simple task attribute */
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	put_be32(c->upiu + 12, blocks * BLOCK);
	if (lba + blocks <= 0x100000000ULL && blocks <= 0xffff) {
		cdb[0] = write ? 0x2a : 0x28;
		put_be32(cdb + 2, (uint32_t)lba);
		cdb[7] = (uint8_t)(blocks >> 8);
		cdb[8] = (uint8_t)blocks;
	} else {
		cdb[0] = write ? 0x8a : 0x88;
		put_be32(cdb + 2, (uint32_t)(lba >> 32));
		put_be32(cdb + 6, (uint32_t)lba);
		put_be32(cdb + 10, blocks);
	}
	c->dw0 = UTRD_CT_UFS | (write ? UTRD_DD_OUT : UTRD_DD_IN);      /* no interrupt, no inline encryption */
}

static void
hc_make_scsi(struct hc_cmd *c, uint8_t lun, uint8_t tag, const uint8_t *cdb, uint32_t cdb_len, uint32_t data_in)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = data_in ? UPIU_FLAG_READ : 0;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	put_be32(c->upiu + 12, data_in);
	memcpy(c->upiu + 16, cdb, cdb_len);
	c->dw0 = UTRD_CT_UFS | (data_in ? UTRD_DD_IN : UTRD_DD_NONE);
}

static void
hc_make_tur(struct hc_cmd *c, uint8_t lun, uint8_t tag)
{
	static const uint8_t tur[6] = { 0x00 };

	hc_make_scsi(c, lun, tag, tur, sizeof(tur), 0);
}

static void
hc_make_query(struct hc_cmd *c, uint8_t tag, uint8_t function, uint8_t opcode, uint8_t idn, uint8_t index, uint16_t length)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_QUERY;
	c->upiu[3] = tag;
	c->upiu[5] = function;
	c->upiu[12] = opcode;
	c->upiu[13] = idn;
	c->upiu[14] = index;
	c->upiu[18] = (uint8_t)(length >> 8);
	c->upiu[19] = (uint8_t)length;
	c->dw0 = UTRD_CT_UFS | UTRD_DD_NONE;
}

#endif /* _HAKIOS_UFSHC_UPIU_H_ */
