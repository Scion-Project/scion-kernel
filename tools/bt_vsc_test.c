/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/bt_vsc_test.c: drivers/bluetooth/bcm4361/hakios.bt_vsc.h (the vendor commands of iOS's bluetoothd on the Galaxy
 * S9's BCM4361) on a computer, under the address and undefined-behaviour sanitizers (tools/bt-vsc-test.sh). The chip passes
 * on what radio peers send and bluetoothd sends what it likes, so both directions are taken as hostile.
 *
 * What is checked:
 *   - the table itself: every entry a vendor command (OGF 0x3F), its return parameters within BT_VSC_RET_MAX, a text;
 *   - with bt_vsc=1, a rejected 0xFDDA becomes a whole reply (status 00 and a 32-bit mask of 0: event length 8); 0xFDFD
 *     with 00 (off) is answered 00 and with 01 (on) left rejected; a vendor command the chip accepts, one rejected with
 *     another status than 01, one whose command was never seen going out, and one not in the table pass as the chip
 *     gave them; Num_HCI_Command_Packets is the chip's;
 *   - with bt_vsc=1, 0xFE37 as phone 1's bluetoothd sent it goes as LE Set Scan Parameters (0x200B) with its LE 1M entry,
 *     and the chip's reply (status kept) comes back as 0xFE37's; parameters out of range or of another shape go as
 *     they are; 0xFE29 goes as LE Set Scan Enable and a Command Status is renamed too; a 0x200B of bluetoothd's own and
 *     a reply to none made here pass as they are;
 *   - nothing that is not a vendor command is changed, with the table on and bt_vsc_ok=all both: a standard command's
 *     rejection, other events, ACL and SCO data that carry bytes looking like a vendor command's rejection;
 *   - bt_vsc=log: rejections reported once each, nothing changed; bt_vsc_ok's list (with junk in it) and "all,-OP";
 *   - streams fed a byte at a time and in pieces give the same output; a data packet's length is followed (65535 bytes);
 *   - 300000 mutated streams in both directions: no write past what the filter owns, never more than BT_VSC_OUT_MAX at
 *     once, and with the table off every byte comes out as it went in, in order.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../drivers/bluetooth/bcm4361/hakios.bt_vsc.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t seed = 0x5eed1117u;

static uint32_t
mut(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

/* the chip's bytes through the filter, one at a time; returns the output's length */
static size_t
chip(struct bt_vsc *v, const uint8_t *in, size_t n, uint8_t *out, size_t room)
{
	size_t got = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t buf[BT_VSC_OUT_MAX];
		size_t k = bt_vsc_chip_byte(v, in[i], buf);

		if (k > BT_VSC_OUT_MAX) {
			CHECK(false, "%zu bytes at once", k);
			return got;
		}
		if (got + k <= room) {
			memcpy(out + got, buf, k);
		}
		got += k;
	}
	return got;
}

/* the host's bytes through the filter, one at a time; returns the output's length */
static size_t
host(struct bt_vsc *v, const uint8_t *in, size_t n, uint8_t *out, size_t room)
{
	size_t got = 0;

	for (size_t i = 0; i < n; i++) {
		uint8_t buf[BT_VSC_OUT_MAX];
		size_t k = bt_vsc_host_byte(v, in[i], buf);

		if (k > BT_VSC_OUT_MAX) {
			CHECK(false, "%zu bytes at once", k);
			return got;
		}
		if (got + k <= room) {
			memcpy(out + got, buf, k);
		}
		got += k;
	}
	return got;
}

/* a command the host sends, which must go to the chip as it is */
static void
sent(struct bt_vsc *v, const uint8_t *in, size_t n)
{
	uint8_t out[600];
	size_t got = host(v, in, n, out, sizeof(out));

	CHECK(got == n && memcmp(out, in, n) == 0, "a command changed on its way (%zu of %zu bytes)", got, n);
}

static void
fresh(struct bt_vsc *v, bool table)
{
	memset(v, 0, sizeof(*v));
	v->table = table;
	v->log = true;
	bt_vsc_reset(v);
}

static void
table_itself(void)
{
	for (size_t i = 0; i < sizeof(bt_vsc_table) / sizeof(bt_vsc_table[0]); i++) {
		const struct bt_vsc_rule *r = &bt_vsc_table[i];

		CHECK(bt_vsc_is_vendor(r->opcode), "entry %zu (%04x) is not a vendor command", i, r->opcode);
		CHECK(r->ret_len <= BT_VSC_RET_MAX, "entry %zu: %u return bytes", i, r->ret_len);
		CHECK(r->what != NULL && strlen(r->what) > 10, "entry %zu has no text", i);
	}
}

static void
answers(void)
{
	static const uint8_t fdda_cmd[] = { 0x01, 0xda, 0xfd, 0x00 };
	static const uint8_t fdda_rej[] = { 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 };
	static const uint8_t fdda_made[] = { 0x04, 0x0e, 0x08, 0x01, 0xda, 0xfd, 0x00, 0, 0, 0, 0 };
	static const uint8_t fdda_real[] = { 0x04, 0x0e, 0x08, 0x05, 0xda, 0xfd, 0x00, 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t fdda_rej12[] = { 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x12 };
	static const uint8_t fdfd_off[] = { 0x01, 0xfd, 0xfd, 0x01, 0x00 };
	static const uint8_t fdfd_on[] = { 0x01, 0xfd, 0xfd, 0x01, 0x01 };
	static const uint8_t fdfd_rej[] = { 0x04, 0x0e, 0x04, 0x03, 0xfd, 0xfd, 0x01 };
	static const uint8_t fdfd_made[] = { 0x04, 0x0e, 0x04, 0x03, 0xfd, 0xfd, 0x00 };
	static const uint8_t other_cmd[] = { 0x01, 0xf2, 0xff, 0x02, 0x05, 0x00 };
	static const uint8_t other_rej[] = { 0x04, 0x0e, 0x04, 0x01, 0xf2, 0xff, 0x01 };
	struct bt_vsc v;
	uint8_t out[600];
	size_t n;

	fresh(&v, true);
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	n = chip(&v, fdda_rej, sizeof(fdda_rej), out, sizeof(out));
	CHECK(n == sizeof(fdda_made) && memcmp(out, fdda_made, n) == 0, "0xFDDA rejected: a whole reply (%zu bytes)", n);
	CHECK(v.note && v.note_opcode == 0xfdda && v.note_status == 1 && v.note_rule == 0 && v.answered == 1,
	    "0xFDDA's first rejection reported, answered by entry 0");
	v.note = false;
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	n = chip(&v, fdda_rej, sizeof(fdda_rej), out, sizeof(out));
	CHECK(n == sizeof(fdda_made) && memcmp(out, fdda_made, n) == 0 && !v.note, "again: answered, not reported again");
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	n = chip(&v, fdda_real, sizeof(fdda_real), out, sizeof(out));
	CHECK(n == sizeof(fdda_real) && memcmp(out, fdda_real, n) == 0, "a chip that knows 0xFDDA: its reply as it is");
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	n = chip(&v, fdda_rej12, sizeof(fdda_rej12), out, sizeof(out));
	CHECK(n == sizeof(fdda_rej12) && memcmp(out, fdda_rej12, n) == 0, "rejected with 12, not 01: as it is");
	n = chip(&v, fdda_rej, sizeof(fdda_rej), out, sizeof(out));
	CHECK(n == sizeof(fdda_rej) && memcmp(out, fdda_rej, n) == 0, "a reply to a command never seen: as it is");

	sent(&v, fdfd_off, sizeof(fdfd_off));
	n = chip(&v, fdfd_rej, sizeof(fdfd_rej), out, sizeof(out));
	CHECK(n == sizeof(fdfd_made) && memcmp(out, fdfd_made, n) == 0, "HDR off: done, with the chip's 3 command packets");
	sent(&v, fdfd_on, sizeof(fdfd_on));
	n = chip(&v, fdfd_rej, sizeof(fdfd_rej), out, sizeof(out));
	CHECK(n == sizeof(fdfd_rej) && memcmp(out, fdfd_rej, n) == 0, "HDR on: stays refused");

	sent(&v, other_cmd, sizeof(other_cmd));
	n = chip(&v, other_rej, sizeof(other_rej), out, sizeof(out));
	CHECK(n == sizeof(other_rej) && memcmp(out, other_rej, n) == 0, "a vendor command not in the table: as it is");

	/* commands split anywhere, replies a byte at a time: the same */
	fresh(&v, true);
	CHECK(host(&v, fdfd_off, 2, out, sizeof(out)) == 0, "a command is held until it is whole");
	CHECK(host(&v, fdfd_off + 2, 3, out, sizeof(out)) == 5 && memcmp(out, fdfd_off, 5) == 0, "then sent as it is");
	n = chip(&v, fdfd_rej, sizeof(fdfd_rej), out, sizeof(out));
	CHECK(n == sizeof(fdfd_made) && memcmp(out, fdfd_made, n) == 0, "a command in two pieces");

	/* bt_vsc=log: reported, nothing changed */
	fresh(&v, false);
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	n = chip(&v, fdda_rej, sizeof(fdda_rej), out, sizeof(out));
	CHECK(n == sizeof(fdda_rej) && memcmp(out, fdda_rej, n) == 0 && v.note && v.note_rule == -1, "log only: as it is");
}

static void
translations(void)
{
	static const uint8_t fe37[] = { 0x01, 0x37, 0xfe, 0x0c, 0x01, 0x00, 0x01, 0x02, 0x01, 0x01, 0x40, 0x00, 0x30, 0x00,
		0x1e, 0x00 };
	static const uint8_t to_200b[] = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x40, 0x00, 0x30, 0x00, 0x01, 0x00 };
	static const uint8_t cc_200b[] = { 0x04, 0x0e, 0x04, 0x01, 0x0b, 0x20, 0x00 };
	static const uint8_t cc_fe37[] = { 0x04, 0x0e, 0x04, 0x01, 0x37, 0xfe, 0x00 };
	static const uint8_t cc_200b_12[] = { 0x04, 0x0e, 0x04, 0x01, 0x0b, 0x20, 0x12 };
	static const uint8_t fe37_bad[] = { 0x01, 0x37, 0xfe, 0x0c, 0x01, 0x00, 0x01, 0x02, 0x01, 0x01, 0x40, 0x00, 0x50, 0x00,
		0x1e, 0x00 };   /* a window longer than the interval */
	static const uint8_t fe37_short[] = { 0x01, 0x37, 0xfe, 0x05, 0x01, 0x00, 0x01, 0x02, 0x01 };
	static const uint8_t own_200b[] = { 0x01, 0x0b, 0x20, 0x07, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };
	static const uint8_t fe29[] = { 0x01, 0x29, 0xfe, 0x09, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t to_200c[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x01 };
	static const uint8_t cs_200c[] = { 0x04, 0x0f, 0x04, 0x0c, 0x01, 0x0c, 0x20 };
	static const uint8_t cs_fe29[] = { 0x04, 0x0f, 0x04, 0x0c, 0x01, 0x29, 0xfe };
	struct bt_vsc v;
	uint8_t out[600];
	size_t n;

	fresh(&v, true);
	n = host(&v, fe37, sizeof(fe37), out, sizeof(out));
	CHECK(n == sizeof(to_200b) && memcmp(out, to_200b, n) == 0, "0xFE37 sent as LE Set Scan Parameters (%zu bytes)", n);
	CHECK(v.xnote && v.xnote_rule >= 0 && bt_vsc_table[v.xnote_rule].opcode == 0xfe37 && v.translated == 1,
	    "its first translation reported");
	n = chip(&v, cc_200b, sizeof(cc_200b), out, sizeof(out));
	CHECK(n == sizeof(cc_fe37) && memcmp(out, cc_fe37, n) == 0, "its reply given as 0xFE37's");
	n = chip(&v, cc_200b, sizeof(cc_200b), out, sizeof(out));
	CHECK(n == sizeof(cc_200b) && memcmp(out, cc_200b, n) == 0, "a second 0x200B reply (none made here): as it is");
	(void)host(&v, fe37, sizeof(fe37), out, sizeof(out));
	n = chip(&v, cc_200b_12, sizeof(cc_200b_12), out, sizeof(out));
	CHECK(n == sizeof(cc_200b_12) && out[4] == 0x37 && out[5] == 0xfe && out[6] == 0x12, "the chip's status kept");
	sent(&v, fe37_bad, sizeof(fe37_bad));
	sent(&v, fe37_short, sizeof(fe37_short));
	sent(&v, own_200b, sizeof(own_200b));
	n = chip(&v, cc_200b, sizeof(cc_200b), out, sizeof(out));
	CHECK(n == sizeof(cc_200b) && memcmp(out, cc_200b, n) == 0, "bluetoothd's own 0x200B and its reply: as they are");
	n = host(&v, fe29, sizeof(fe29), out, sizeof(out));
	CHECK(n == sizeof(to_200c) && memcmp(out, to_200c, n) == 0, "0xFE29 sent as LE Set Scan Enable");
	n = chip(&v, cs_200c, sizeof(cs_200c), out, sizeof(out));
	CHECK(n == sizeof(cs_fe29) && memcmp(out, cs_fe29, n) == 0, "a Command Status renamed too");
	/* the table off: nothing translated */
	fresh(&v, false);
	sent(&v, fe37, sizeof(fe37));
	n = chip(&v, cc_200b, sizeof(cc_200b), out, sizeof(out));
	CHECK(n == sizeof(cc_200b) && memcmp(out, cc_200b, n) == 0, "table off: 0x200B's reply as it is");
}

static void
untouched(void)
{
	static const uint8_t std_rej[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x01 };
	static const uint8_t std_cmd[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t status_ev[] = { 0x04, 0x0f, 0x04, 0x01, 0x01, 0xda, 0xfd };
	static const uint8_t short_cc[] = { 0x04, 0x0e, 0x03, 0x01, 0xda, 0xfd };
	static const uint8_t acl[] = { 0x02, 0x01, 0x20, 0x07, 0x00, 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 };
	static const uint8_t sco[] = { 0x03, 0x01, 0x00, 0x07, 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 };
	static const uint8_t fdda_cmd[] = { 0x01, 0xda, 0xfd, 0x00 };
	const uint8_t *all[] = { std_rej, status_ev, short_cc, acl, sco };
	const size_t len[] = { sizeof(std_rej), sizeof(status_ev), sizeof(short_cc), sizeof(acl), sizeof(sco) };
	struct bt_vsc v;
	uint8_t out[600], big[5];
	size_t n;

	fresh(&v, true);
	bt_vsc_parse_ok(&v, "all");
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		sent(&v, std_cmd, sizeof(std_cmd));
		sent(&v, fdda_cmd, sizeof(fdda_cmd));
		n = chip(&v, all[i], len[i], out, sizeof(out));
		CHECK(n == len[i] && memcmp(out, all[i], n) == 0, "packet %zu changed", i);
	}
	/* an ACL packet of 65535 bytes is followed to its end; the event after it is found */
	fresh(&v, true);
	big[0] = 0x02; big[1] = 0x01; big[2] = 0x20; big[3] = 0xff; big[4] = 0xff;
	n = chip(&v, big, sizeof(big), out, sizeof(out));
	for (unsigned i = 0; i < 65535; i++) {
		uint8_t b = 0x04, o[BT_VSC_OUT_MAX];

		n += bt_vsc_chip_byte(&v, b, o);
	}
	CHECK(n == 5 + 65535 && v.c_at == 0 && v.c_total == 0, "a 65535-byte ACL packet ends where it says");
	sent(&v, fdda_cmd, sizeof(fdda_cmd));
	{
		static const uint8_t rej[] = { 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 };

		n = chip(&v, rej, sizeof(rej), out, sizeof(out));
		CHECK(n == 11 && out[2] == 8, "and the reply after it is answered");
	}
}

static void
legacy(void)
{
	static const uint8_t rej[] = { 0x04, 0x0e, 0x04, 0x01, 0x5a, 0xfd, 0x01 };
	static const uint8_t fdda[] = { 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 };
	struct bt_vsc v;
	uint8_t out[64];
	char longlist[400];
	size_t n, k = 0;

	fresh(&v, false);
	bt_vsc_parse_ok(&v, "0xfd5a, ,,-,0x,zz,-0xfd73,99999999999999999999,fd5b,0x10000");
	CHECK(v.ok_n == 2 && v.ok[0] == 0xfd5a && v.ok[1] == 0xfd5b && v.not_ok_n == 1 && v.not_ok[0] == 0xfd73 && !v.ok_all,
	    "a list with junk in it: %u %u", v.ok_n, v.not_ok_n);
	n = chip(&v, rej, sizeof(rej), out, sizeof(out));
	CHECK(n == sizeof(rej) && memcmp(out, rej, 6) == 0 && out[6] == 0 && v.note_rule == -2, "bt_vsc_ok: status 00");
	for (int i = 0; i < 20; i++) {
		k += (size_t)snprintf(longlist + k, sizeof(longlist) - k, "0x%04x,-0x%04x,", 0xfd00 + i, 0xfe00 + i);
	}
	bt_vsc_parse_ok(&v, longlist);
	CHECK(v.ok_n == BT_VSC_LIST_MAX && v.not_ok_n == BT_VSC_LIST_MAX, "a list longer than the tables is cut");
	bt_vsc_parse_ok(&v, "all,-0xfdda");
	n = chip(&v, fdda, sizeof(fdda), out, sizeof(out));
	CHECK(v.ok_all && n == sizeof(fdda) && out[6] == 1, "\"all,-0xfdda\" leaves 0xFDDA rejected");
	/* the reported list fills and stops */
	for (unsigned op = 0; op < 200; op++) {
		uint8_t e[] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)op, 0xfc, 0x01 };

		(void)chip(&v, e, sizeof(e), out, sizeof(out));
	}
	CHECK(v.said_n == BT_VSC_SAID_MAX, "the reported list full (%u)", v.said_n);
}

static void
mutated(void)
{
	static const uint8_t pieces[][12] = {
		{ 0x04, 0x0e, 0x04, 0x01, 0xda, 0xfd, 0x01 },
		{ 0x04, 0x0e, 0x04, 0x01, 0xfd, 0xfd, 0x01 },
		{ 0x02, 0x01, 0x20, 0x03, 0x00, 1, 2, 3 },
		{ 0x03, 0x01, 0x00, 0x02, 9, 9 },
		{ 0x04, 0x3e, 0x03, 0x02, 0x01, 0x00 },
		{ 0x01, 0xda, 0xfd, 0x00 },
		{ 0x01, 0xfd, 0xfd, 0x01, 0x00 },
	};
	static const size_t plen[] = { 7, 7, 8, 6, 6, 4, 5 };

	for (int round = 0; round < 300000; round++) {
		struct bt_vsc v;
		uint8_t s[128], out[128 * 2 + BT_VSC_OUT_MAX];
		size_t n = 0, got;
		bool table = mut() % 2;

		fresh(&v, table);
		if (mut() % 3 == 0) {
			bt_vsc_parse_ok(&v, mut() % 2 ? "all" : "fdda,-fdfd");
		}
		for (int parts = 1 + (int)(mut() % 6); parts > 0 && n + 12 <= sizeof(s); parts--) {
			unsigned w = mut() % 7;

			memcpy(s + n, pieces[w], plen[w]);
			n += plen[w];
		}
		for (int m = (int)(mut() % 4); m > 0; m--) {
			size_t at = mut() % n;

			switch (mut() % 4) {
			case 0: s[at] = (uint8_t)mut(); break;
			case 1: s[at] = 0xff; break;
			case 2: s[at] ^= (uint8_t)(1u << (mut() % 8)); break;
			default: n = at + 1; break;
			}
		}
		got = host(&v, s, n, out, sizeof(out));
		if (!table && got > n) {
			CHECK(false, "round %d: the host's stream grew with the table off", round);
		} else if (!table && memcmp(out, s, got) != 0) {
			CHECK(false, "round %d: the host's stream changed with the table off", round);
		}
		got = chip(&v, s, n, out, sizeof(out));
		if (v.c_at > BT_VSC_OUT_MAX + 0u && v.c_buf[0] == 0x04) {
			CHECK(false, "an event held past the buffer: %u", v.c_at);
		}
		if (!table && !v.ok_all && v.ok_n == 0) {
			/* nothing to change: what came out is what went in, less what is still held */
			if (got > n || memcmp(out, s, got) != 0) {
				CHECK(false, "round %d: the stream changed with the table off", round);
			}
		}
	}
}

int
main(void)
{
	table_itself();
	answers();
	translations();
	untouched();
	legacy();
	mutated();
	printf("bt_vsc_test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
