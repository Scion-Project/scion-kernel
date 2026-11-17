/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/ufshc_upiu_test.c: drivers/storage/ufs/hakios.ufshc_upiu.h on a computer, under the address and
 * undefined-behaviour sanitizers (tools/ufshc-upiu-test.sh). hc_refusal() is the one check every command of the kernel's
 * own UFS driver passes before it goes into a slot, and the last thing between a mistake anywhere above it and a write
 * to the phone's flash; the builders are what makes the commands it checks.
 *
 * What is checked:
 *   - the builders: hc_make_rw gives READ(10)/WRITE(10) while the range fits 32 bits and 65535 blocks, (16) beyond,
 *     with the LBA, the length, the expected byte count and the data direction read back as they were asked for, at
 *     the edges (2^32 - 1, 2^32, 65535 and 65536 blocks); hc_make_tur, hc_make_scsi, hc_make_query lay out their bytes;
 *   - hand-made commands under six policies (shared mode, own mode with a window, own mode enabling QEMU's
 *     controller, ufs=3's fences, read only, a window on a unit never written), each with the answer it must get;
 *   - every command the builders make for reads and writes all over a window and the ranges around it, against what the
 *     window and the fences allow, worked out here independently, including windows and fences at the top of the
 *     64-bit block space, where a sum would wrap;
 *   - 4,000,000 commands made from those by a fixed-seed mutator, under every policy: whatever passes must satisfy
 *     the rules that matter for the flash -- never data to the device while read only, a write only inside the
 *     window, on its unit, outside every fence and never on a unit never written, a read only of a unit reads may go
 *     to, no well-known unit but the device unit's START STOP UNIT to ACTIVE in own mode, no query that writes but
 *     fDeviceInit while enabling, only the operations on the list, and the expected length always the blocks' --
 *     and hc_refusal never reads past the 32 bytes of the UPIU (the sanitizer).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/storage/ufs/hakios.ufshc_upiu.h"

static unsigned long checks, failures;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* ---- the policies ---- */

static const struct hc_deny fences[] = {
	{ .lun = 0, .first = 0, .blocks = 6, .what = "protected: LBA 0-5" },
	{ .lun = 0, .first = 5000, .blocks = 10, .what = "protected: EFS" },
	{ .lun = 0, .first = UINT64_MAX - 9, .blocks = 10, .what = "protected: the last ten blocks" },
};

enum { P_SHARED, P_OWN, P_ENABLING, P_FENCED, P_RO, P_DENIED_UNIT, P_TOP, P_COUNT };

static struct hc_policy pol[P_COUNT] = {
	[P_SHARED] = { .read_luns = 1 },
	[P_OWN] = { .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 4000, .w_blocks = 2000 },
	[P_ENABLING] = { .read_luns = 1, .control = true, .device_init = true },
	[P_FENCED] = { .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 4000, .w_blocks = 2000,
		       .deny_luns = 0xfe, .deny = fences, .ndeny = 3 },
	[P_RO] = { .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 4000, .w_blocks = 2000, .ro = true },
	[P_DENIED_UNIT] = { .read_luns = 3, .control = true, .window = true, .w_lun = 2, .w_first = 0, .w_blocks = 100,
			    .deny_luns = 1u << 2 },
	/* a window that ends at the very top of the block space, with the last ten blocks fenced */
	[P_TOP] = { .read_luns = 1, .control = true, .window = true, .w_lun = 0, .w_first = UINT64_MAX - 999,
		    .w_blocks = 1000, .deny = fences, .ndeny = 3 },
};

static const char *const pol_name[P_COUNT] = { "shared", "own", "enabling", "fenced", "read only", "denied unit", "top" };

/* ---- what a command says, read here independently of the header ---- */

static uint64_t
rd_be(const uint8_t *p, int n)
{
	uint64_t v = 0;

	for (int i = 0; i < n; i++) {
		v = v << 8 | p[i];
	}
	return v;
}

/* a write's [lba, lba + blocks) may go to the flash under p: in the window, outside every fence, on a unit written */
static bool
write_allowed(const struct hc_policy *p, uint8_t lun, uint64_t lba, uint64_t blocks)
{
	if (p->ro || !p->window || lun != p->w_lun || lun >= UFSHC_LUNS || (p->deny_luns & (1u << lun)) || blocks == 0) {
		return false;
	}
	/* in the window, worked out in 128 bits so that nothing wraps */
	unsigned __int128 end = (unsigned __int128)lba + blocks, w_end = (unsigned __int128)p->w_first + p->w_blocks;
	if (lba < p->w_first || end > w_end) {
		return false;
	}
	for (uint32_t i = 0; i < p->ndeny; i++) {
		unsigned __int128 d_end = (unsigned __int128)p->deny[i].first + p->deny[i].blocks;
		if (p->deny[i].lun == lun && lba < d_end && end > p->deny[i].first) {
			return false;
		}
	}
	return true;
}

/* ---- the builders ---- */

static void
test_builders(void)
{
	static const struct { uint64_t lba; uint32_t blocks; bool sixteen; } rw[] = {
		{ 0, 1, false }, { 12345, 64, false }, { 0xffffffffull - 1, 1, false }, { 0xffffffffull, 1, false },
		{ 0xffffffffull, 2, true }, { 0x100000000ull, 1, true }, { 0, 0xffff, false }, { 0, 0x10000, true },
		{ 0x123456789abcull, 8, true }, { UINT64_MAX - 8, 8, true },
	};
	struct hc_cmd c;

	for (unsigned i = 0; i < sizeof(rw) / sizeof(rw[0]); i++) {
		for (int w = 0; w <= 1; w++) {
			memset(&c, 0xa5, sizeof(c));
			hc_make_rw(&c, 3, 7, rw[i].lba, rw[i].blocks, w);
			const uint8_t *cdb = c.upiu + 16;
			bool sixteen = cdb[0] == (w ? 0x8a : 0x88);
			CHECK(sixteen == rw[i].sixteen && (sixteen || cdb[0] == (w ? 0x2a : 0x28)),
			    "hc_make_rw(%llu, %u, %d): operation %02x", (unsigned long long)rw[i].lba, rw[i].blocks, w, cdb[0]);
			uint64_t lba = sixteen ? rd_be(cdb + 2, 8) : rd_be(cdb + 2, 4);
			uint64_t blocks = sixteen ? rd_be(cdb + 10, 4) : rd_be(cdb + 7, 2);
			CHECK(lba == rw[i].lba && blocks == rw[i].blocks, "hc_make_rw(%llu, %u): reads back %llu, %llu",
			    (unsigned long long)rw[i].lba, rw[i].blocks, (unsigned long long)lba, (unsigned long long)blocks);
			CHECK(c.upiu[0] == UPIU_COMMAND && c.upiu[1] == (w ? UPIU_FLAG_WRITE : UPIU_FLAG_READ) && c.upiu[2] == 3 &&
			    c.upiu[3] == 7, "hc_make_rw: header %02x %02x %02x %02x", c.upiu[0], c.upiu[1], c.upiu[2], c.upiu[3]);
			CHECK(rd_be(c.upiu + 12, 4) == (uint32_t)(rw[i].blocks * BLOCK), "hc_make_rw: expected length");
			CHECK(c.dw0 == (UTRD_CT_UFS | (w ? UTRD_DD_OUT : UTRD_DD_IN)), "hc_make_rw: descriptor %08x", c.dw0);
			bool rest_zero = true;
			for (int k = 4; k < 12; k++) {
				rest_zero &= c.upiu[k] == 0;
			}
			for (int k = sixteen ? 14 : 9; k < 16; k++) {
				rest_zero &= cdb[k] == 0;
			}
			CHECK(rest_zero, "hc_make_rw: a byte it does not set is not zero");
		}
	}
	hc_make_tur(&c, 5, 9);
	CHECK(c.upiu[0] == UPIU_COMMAND && c.upiu[1] == 0 && c.upiu[2] == 5 && c.upiu[3] == 9 && c.upiu[16] == 0 &&
	    rd_be(c.upiu + 12, 4) == 0 && c.dw0 == (UTRD_CT_UFS | UTRD_DD_NONE), "hc_make_tur");
	static const uint8_t cap16[16] = { 0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32 };
	hc_make_scsi(&c, 1, 2, cap16, sizeof(cap16), 32);
	CHECK(c.upiu[1] == UPIU_FLAG_READ && memcmp(c.upiu + 16, cap16, 16) == 0 && rd_be(c.upiu + 12, 4) == 32 &&
	    c.dw0 == (UTRD_CT_UFS | UTRD_DD_IN), "hc_make_scsi: READ CAPACITY(16)");
	hc_make_query(&c, 4, QUERY_READ, QOP_READ_DESC, QDESC_UNIT, 3, 0x123);
	CHECK(c.upiu[0] == UPIU_QUERY && c.upiu[3] == 4 && c.upiu[5] == QUERY_READ && c.upiu[12] == QOP_READ_DESC &&
	    c.upiu[13] == QDESC_UNIT && c.upiu[14] == 3 && c.upiu[18] == 0x01 && c.upiu[19] == 0x23 &&
	    c.dw0 == (UTRD_CT_UFS | UTRD_DD_NONE), "hc_make_query");
	CHECK(be32((const uint8_t *)"\x01\x02\x03\x04") == 0x01020304u, "be32");
	CHECK(be64((const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08") == 0x0102030405060708ull, "be64");
}

/* ---- commands made by hand, each with its answer ---- */

static bool
passes(const uint8_t *upiu, uint32_t dw0, int p)
{
	return hc_refusal(upiu, dw0, &pol[p], false) == NULL;
}

static void
cmd(uint8_t *u, uint8_t type, uint8_t flags, uint8_t lun, uint32_t expected, const uint8_t *cdb, size_t n)
{
	memset(u, 0, 32);
	u[0] = type;
	u[1] = flags;
	u[2] = lun;
	u[12] = (uint8_t)(expected >> 24);
	u[13] = (uint8_t)(expected >> 16);
	u[14] = (uint8_t)(expected >> 8);
	u[15] = (uint8_t)expected;
	memcpy(u + 16, cdb, n);
}

static void
test_by_hand(void)
{
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0x0f, 0xa0, 0, 0, 1 };      /* block 4000 */
	static const uint8_t write10[10] = { 0x2a, 0, 0, 0, 0x0f, 0xa0, 0, 0, 1 };
	static const uint8_t sync10[10] = { 0x35 };
	static const uint8_t tur[6] = { 0 };
	static const uint8_t ssu_active[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	static const uint8_t ssu_sleep[6] = { 0x1b, 0, 0, 0, 0x20, 0 };
	static const uint8_t unmap[16] = { 0x42 };
	static const uint8_t format[16] = { 0x04 };
	static const uint8_t wbuf[16] = { 0x3b };
	static const uint8_t sec_out[16] = { 0xb5 };
	static const uint8_t vendor[16] = { 0xc0 };
	static const uint8_t sai_other[16] = { 0x9e, 0x11 };
	uint8_t u[32];

	/* shared mode: reads of LUN 0 and nothing else */
	cmd(u, UPIU_COMMAND, UPIU_FLAG_READ, 0, BLOCK, read10, sizeof(read10));
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_SHARED), "shared: READ(10) of LUN 0");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_SHARED), "shared: READ(10) with data to the device");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_MASK, P_SHARED), "shared: READ(10) with direction 3");
	CHECK(!passes(u, UTRD_DD_IN, P_SHARED), "shared: READ(10) of another command type");
	CHECK(hc_refusal(u, UTRD_DD_IN, &pol[P_SHARED], true) == NULL, "shared: any command type when the caller says so");
	cmd(u, UPIU_COMMAND, UPIU_FLAG_READ, 1, BLOCK, read10, sizeof(read10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_SHARED), "shared: READ(10) of LUN 1");
	cmd(u, UPIU_COMMAND, UPIU_FLAG_READ, 0, 2 * BLOCK, read10, sizeof(read10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_SHARED), "shared: READ(10) of a length other than expected");
	cmd(u, UPIU_COMMAND, UPIU_FLAG_READ | UPIU_FLAG_WRITE, 0, BLOCK, read10, sizeof(read10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_SHARED), "shared: R and W");
	cmd(u, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, BLOCK, write10, sizeof(write10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_SHARED), "shared: WRITE(10)");
	cmd(u, UPIU_COMMAND, 0, 0, 0, sync10, sizeof(sync10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_SHARED), "shared: SYNCHRONIZE CACHE");
	cmd(u, UPIU_COMMAND, 0, 0, 0, tur, sizeof(tur));
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_SHARED), "shared: TEST UNIT READY of LUN 0");
	cmd(u, UPIU_NOP_OUT, 0, 0, 0, tur, 0);
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_SHARED), "shared: NOP OUT");
	cmd(u, UPIU_QUERY, 0, 0, 0, tur, 0);
	u[5] = QUERY_READ;
	u[12] = QOP_READ_ATTR;
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_SHARED), "shared: a query");
	for (unsigned lun = 0x80; lun <= 0xff; lun++) {
		cmd(u, UPIU_COMMAND, UPIU_FLAG_READ, (uint8_t)lun, BLOCK, read10, sizeof(read10));
		CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_OWN), "own: READ(10) of well-known unit %02x", lun);
	}
	cmd(u, UPIU_COMMAND, UPIU_FLAG_READ, 8, BLOCK, read10, sizeof(read10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_OWN), "own: READ(10) of LUN 8");

	/* own mode */
	cmd(u, UPIU_NOP_OUT, 0, 0, 0, tur, 0);
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_OWN), "own: NOP OUT");
	cmd(u, UPIU_COMMAND, 0, LUN_DEVICE, 0, ssu_active, sizeof(ssu_active));
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_OWN), "own: START STOP UNIT to ACTIVE");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_SHARED), "shared: START STOP UNIT to ACTIVE");
	cmd(u, UPIU_COMMAND, 0, LUN_DEVICE, 0, ssu_sleep, sizeof(ssu_sleep));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_OWN), "own: START STOP UNIT to SLEEP");
	cmd(u, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, BLOCK, write10, sizeof(write10));
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_OWN), "own: WRITE(10) of the window's first block");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_IN, P_OWN), "own: WRITE(10) with data from the device");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_RO), "read only: WRITE(10)");
	u[17] = 0x08;   /* FUA */
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_OWN), "own: WRITE(10) FUA");
	u[17] = 0x10;   /* DPO */
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_OUT, P_OWN), "own: WRITE(10) DPO");
	cmd(u, UPIU_COMMAND, 0, 0, 0, sync10, sizeof(sync10));
	CHECK(passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_OWN), "own: SYNCHRONIZE CACHE of the window's unit");
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_RO), "read only: SYNCHRONIZE CACHE");
	cmd(u, UPIU_COMMAND, 0, 2, 0, sync10, sizeof(sync10));
	CHECK(!passes(u, UTRD_CT_UFS | UTRD_DD_NONE, P_DENIED_UNIT), "a unit never written: SYNCHRONIZE CACHE");
	const uint8_t *const never[] = { unmap, format, wbuf, sec_out, vendor, sai_other };
	for (int p = 0; p < P_COUNT; p++) {
		for (unsigned i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
			for (int w = 0; w <= 2; w++) {
				uint32_t dd = w == 0 ? UTRD_DD_NONE : w == 1 ? UTRD_DD_OUT : UTRD_DD_IN;
				cmd(u, UPIU_COMMAND, w == 1 ? UPIU_FLAG_WRITE : w == 2 ? UPIU_FLAG_READ : 0, 0, w ? 512 : 0, never[i], 16);
				CHECK(!passes(u, UTRD_CT_UFS | dd, p), "%s: operation %02x passes", pol_name[p], never[i][0]);
			}
		}
	}

	/* queries: reads in own mode; of writes, fDeviceInit while enabling and nothing else */
	for (int p = 0; p < P_COUNT; p++) {
		for (unsigned fn = 0; fn < 256; fn++) {
			for (unsigned op = 0; op < 16; op++) {
				cmd(u, UPIU_QUERY, 0, 0, 0, tur, 0);
				u[5] = (uint8_t)fn;
				u[12] = (uint8_t)op;
				u[13] = QFLAG_DEVICE_INIT;
				bool ok = passes(u, UTRD_CT_UFS | UTRD_DD_NONE, p);
				bool want = pol[p].control && ((fn == QUERY_READ && (op == QOP_READ_DESC || op == QOP_READ_ATTR ||
				    op == QOP_READ_FLAG)) || (fn == QUERY_WRITE && op == QOP_SET_FLAG && pol[p].device_init));
				CHECK(ok == want, "%s: query function %02x operation %02x %s", pol_name[p], fn, op,
				    ok ? "passes" : "is refused");
			}
		}
	}
}

/* ---- every write the builders make, around the window and the fences ---- */

static void
test_windows(void)
{
	struct hc_cmd c;
	static const int ps[] = { P_OWN, P_FENCED, P_RO, P_DENIED_UNIT, P_TOP, P_SHARED };

	for (unsigned k = 0; k < sizeof(ps) / sizeof(ps[0]); k++) {
		const struct hc_policy *p = &pol[ps[k]];
		uint64_t centres[] = { p->w_first, p->w_first + p->w_blocks, 0, 5000, 5010, UINT64_MAX - 9, UINT64_MAX };
		for (unsigned ci = 0; ci < sizeof(centres) / sizeof(centres[0]); ci++) {
			for (int64_t d = -20; d <= 20; d++) {
				uint64_t lba = centres[ci] + (uint64_t)d;
				for (uint32_t blocks = 1; blocks <= 24; blocks += 7) {
					for (uint8_t lun = 0; lun < 3; lun++) {
						hc_make_rw(&c, lun, 1, lba, blocks, true);
						bool ok = hc_refusal(c.upiu, c.dw0, p, false) == NULL;
						bool want = write_allowed(p, lun, lba, blocks);
						/* a range past 2^64 cannot be said in a CDB: hc_make_rw's LBA is what the device would read */
						if ((unsigned __int128)lba + blocks > ((unsigned __int128)1 << 64)) {
							want = false;
						}
						CHECK(ok == want, "%s: WRITE of %u blocks at %llu on LUN %u %s", pol_name[ps[k]], blocks,
						    (unsigned long long)lba, lun, ok ? "passes" : "is refused");
					}
				}
			}
		}
	}
}

/* ---- mutated commands: what passes must keep the rules ---- */

static void
keeps_the_rules(const uint8_t *u, uint32_t dw0, int pi)
{
	const struct hc_policy *p = &pol[pi];
	const uint8_t *cdb = u + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;
	uint8_t type = u[0] & 0x3f, flags = u[1], lun = u[2];
	uint64_t expected = rd_be(u + 12, 4);

	CHECK(dd != UTRD_DD_MASK, "%s: direction 3 passes", pol_name[pi]);
	CHECK((dw0 & UTRD_CT_MASK) == UTRD_CT_UFS, "%s: command type %08x passes", pol_name[pi], dw0);
	CHECK(!(p->ro && (dd == UTRD_DD_OUT || (type == UPIU_COMMAND && (flags & UPIU_FLAG_WRITE)))),
	    "%s: data to the device while read only", pol_name[pi]);
	if (type == UPIU_NOP_OUT || type == UPIU_QUERY) {
		CHECK(p->control && dd == UTRD_DD_NONE, "%s: a %s passes", pol_name[pi], type ? "query" : "NOP OUT");
		if (type == UPIU_QUERY && u[5] != QUERY_READ) {
			CHECK(p->device_init && u[5] == QUERY_WRITE && u[12] == QOP_SET_FLAG && u[13] == QFLAG_DEVICE_INIT,
			    "%s: a query that writes passes", pol_name[pi]);
		}
		return;
	}
	CHECK(type == UPIU_COMMAND, "%s: transaction type %02x passes", pol_name[pi], type);
	CHECK((u[4] & 0x0f) == 0, "%s: a command set other than SCSI passes", pol_name[pi]);
	if (lun & LUN_WELL_KNOWN) {
		CHECK(p->control && lun == LUN_DEVICE && cdb[0] == 0x1b && cdb[4] == 0x10 && dd == UTRD_DD_NONE,
		    "%s: well-known unit %02x, operation %02x passes", pol_name[pi], lun, cdb[0]);
		return;
	}
	CHECK(lun < UFSHC_LUNS, "%s: LUN %u passes", pol_name[pi], lun);
	switch (cdb[0]) {
	case 0x28:
	case 0x88: {
		uint64_t blocks = cdb[0] == 0x28 ? rd_be(cdb + 7, 2) : rd_be(cdb + 10, 4);
		CHECK((p->read_luns & (1u << lun)) && dd == UTRD_DD_IN && blocks > 0 && expected == blocks * BLOCK,
		    "%s: a read of LUN %u, %llu blocks, %llu bytes passes", pol_name[pi], lun, (unsigned long long)blocks,
		    (unsigned long long)expected);
		break;
	}
	case 0x2a:
	case 0x8a: {
		uint64_t lba = cdb[0] == 0x2a ? rd_be(cdb + 2, 4) : rd_be(cdb + 2, 8);
		uint64_t blocks = cdb[0] == 0x2a ? rd_be(cdb + 7, 2) : rd_be(cdb + 10, 4);
		CHECK(write_allowed(p, lun, lba, blocks) && dd == UTRD_DD_OUT && expected == blocks * BLOCK,
		    "%s: a write of LUN %u, blocks %llu + %llu passes", pol_name[pi], lun, (unsigned long long)lba,
		    (unsigned long long)blocks);
		break;
	}
	case 0x35:
		CHECK(p->window && !p->ro && lun == p->w_lun && !(p->deny_luns & (1u << lun)) && dd == UTRD_DD_NONE,
		    "%s: SYNCHRONIZE CACHE of LUN %u passes", pol_name[pi], lun);
		break;
	case 0x00:
	case 0x03:
	case 0x12:
	case 0x25:
	case 0x9e:
		CHECK(dd != UTRD_DD_OUT && !(flags & UPIU_FLAG_WRITE), "%s: operation %02x with data to the device passes",
		    pol_name[pi], cdb[0]);
		CHECK(p->control || (p->read_luns & (1u << lun)), "%s: operation %02x of LUN %u passes", pol_name[pi],
		    cdb[0], lun);
		break;
	default:
		CHECK(false, "%s: operation %02x passes", pol_name[pi], cdb[0]);
	}
}

static void
test_mutated(void)
{
	struct hc_cmd seeds[8];
	unsigned long passed = 0;

	hc_make_rw(&seeds[0], 0, 1, 4000, 8, true);
	hc_make_rw(&seeds[1], 0, 1, 5005, 1, true);
	hc_make_rw(&seeds[2], 0, 1, UINT64_MAX - 12, 4, true);
	hc_make_rw(&seeds[3], 1, 1, 77, 16, false);
	hc_make_tur(&seeds[4], 0, 2);
	hc_make_query(&seeds[5], 3, QUERY_WRITE, QOP_SET_FLAG, QFLAG_DEVICE_INIT, 0, 0);
	static const uint8_t sync10[10] = { 0x35 };
	hc_make_scsi(&seeds[6], 0, 4, sync10, sizeof(sync10), 0);
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	hc_make_scsi(&seeds[7], LUN_DEVICE, 5, ssu, sizeof(ssu), 0);

	for (unsigned long n = 0; n < 4000000; n++) {
		/* exactly the 32 bytes of the UPIU, on the heap, so that the sanitizer stops any read past them */
		uint8_t *u = malloc(32);
		const struct hc_cmd *s = &seeds[next() % 8];
		uint32_t dw0 = s->dw0;

		memcpy(u, s->upiu, 32);
		for (int m = (int)(next() % 4); m >= 0; m--) {
			uint64_t r = next();
			switch (r % 5) {
			case 0: u[(r >> 8) % 32] = (uint8_t)(r >> 16); break;                         /* a byte */
			case 1: u[(r >> 8) % 32] ^= (uint8_t)(1u << ((r >> 16) % 8)); break;          /* a bit */
			case 2: u[16 + (r >> 8) % 16] = (uint8_t)(r >> 16); break;                    /* the CDB */
			case 3: dw0 ^= 1u << ((r >> 8) % 32); break;                                  /* the descriptor */
			default: u[12 + (r >> 8) % 4] = (uint8_t)(r >> 16); break;                    /* the length */
			}
		}
		int pi = (int)(next() % P_COUNT);
		if (hc_refusal(u, dw0, &pol[pi], false) == NULL) {
			passed++;
			keeps_the_rules(u, dw0, pi);
		}
		free(u);
	}
	printf("ufshc_upiu_test: 4000000 mutated commands, %lu passed the whitelist and kept the rules\n", passed);
}

int
main(void)
{
	test_builders();
	test_by_hand();
	test_windows();
	test_mutated();
	printf("ufshc_upiu_test: %lu checks, %lu failed\n", checks, failures);
	return failures ? 1 : 0;
}
