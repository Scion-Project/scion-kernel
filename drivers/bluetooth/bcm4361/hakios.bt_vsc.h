/* SPDX-License-Identifier: MIT-0 */
/*
 * The vendor commands of iOS's bluetoothd on the Galaxy S9's BCM4361 (drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c, at
 * bt=7, on the kernel control com.apple.uart.bluetooth): a table that says, for each vendor command (OGF 0x3F) Apple's
 * stack sends and the S9's chip does not know, what bluetoothd is told instead, and the H4 filter that applies it. Plain C
 * with no kernel headers (a codec, tools/arch-layers.conf), checked on a computer by tools/bt_vsc_test.c
 * (tools/bt-vsc-test.sh).
 *
 * Why. bluetoothd is built for Apple's own Broadcom parts (BCM4377/4378 and later, with Apple's firmware). It takes the
 * BCM4361 for one of them (manufacturer 0x000F) and sends Apple's vendor commands. Samsung's patch for the BCM4361 answers
 * the ones it does not have with a Command Complete of status 01 (unknown HCI command) and no return parameters. Some of
 * those bluetoothd's stack takes as a failed start and restarts itself ("Bluetooth error - restarting", exit 1), every
 * 30 s. Setting the status to 00 alone (bt_vsc_ok=all) is worse: its parsers then read the return parameters a real reply
 * has and the chip did not send, and one asserts and aborts (SIGABRT): the reply of 0xFDDA is read as a 32-bit word
 * ("ByteStream_NumReadBytesAvail(*pBs) >= 4" in bcm_vsc.c's proprietary-local-features parser, phone 1, 17 November).
 * A reply made up here must therefore be a whole one: the status and every return parameter, with values that are true
 * of this chip.
 *
 * What is done, per table entry (bt_vsc_table below):
 *   ANSWER     when the chip rejects the command with status 01 (and only then: a chip that knows it is passed on as it
 *              answered), its Command Complete is replaced by one of the same Num_HCI_Command_Packets and opcode, with the
 *              entry's status and return parameters. The command itself is still sent to the chip; the chip refuses it
 *              and changes nothing, so the controller's state is what the reply says.
 *   TRANSLATE  the command is not sent as it is but as the standard command that does the same on this chip, built
 *              from its parameters by the entry's function (which refuses parameters it does not understand: then the
 *              command goes as it is); the chip's reply to that command (Command Complete or Status) is given to
 *              bluetoothd with the vendor command's opcode and the chip's own status. Only replies to commands made here
 *              are renamed: a standard command bluetoothd sends itself, and its reply, pass as they are.
 *   (none)     a vendor command not in the table, or whose parameters the entry does not match, passes as the chip
 *              answered it.
 * Nothing bluetoothd sends that is not a vendor command (OGF 0x3F) is changed, nor any event but the replies above: no
 * event of the link layer, no data; so nothing here can change pairing, link keys, encryption or security levels. No
 * entry may answer a command that enables a feature the chip lacks: those are left rejected, and the table's notes say
 * so.
 *
 * A second, older mode (bt_vsc_ok=LIST, tests only, not for use): the status of a listed vendor command's rejection set to
 * 00 and nothing else changed. It is kept because it shows what bluetoothd does next; its replies are malformed for any
 * command that has return parameters (0xFDDA above).
 *
 * Nothing here has been run on a phone (written 17 November 2026 from phone 1's console logs and the iOS 27.0.1, 24A446,
 * bluetoothd binary read for its structure: which parser reads which reply, and how much of it).
 */
#ifndef _HAKIOS_BT_VSC_H_
#define _HAKIOS_BT_VSC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BT_VSC_ANY        (-1)
#define BT_VSC_RET_MAX    8       /* return parameters after the status, at most */
#define BT_VSC_ZEROS_MAX  240     /* zero bytes after those: 4 + 8 + 240 is within an event's 255 */
#define BT_VSC_OUT_MAX    260     /* the most bt_vsc_chip_byte() or _host_byte() writes at once: one whole packet */
#define BT_VSC_PENDING    8       /* vendor commands of the table in flight at once */
#define BT_VSC_LIST_MAX   16      /* bt_vsc_ok's list */
#define BT_VSC_SAID_MAX   64      /* vendor commands whose rejection has been reported */

struct bt_vsc_rule {
	uint16_t opcode;
	int16_t p0, p1;            /* ANSWER: the first and second parameter bytes the command must have, or BT_VSC_ANY */
	uint8_t status;            /* ANSWER: the status bluetoothd is given */
	uint8_t ret_len;           /* ANSWER: return parameters after the status, at most BT_VSC_RET_MAX */
	uint8_t ret[BT_VSC_RET_MAX];
	uint8_t zeros;             /* ANSWER: zero bytes after them (counters that are zero), at most BT_VSC_ZEROS_MAX */
	uint16_t to;               /* TRANSLATE: the standard command sent instead (0: an ANSWER entry) */
	uint8_t (*xlate)(const uint8_t *p, uint8_t n, uint8_t *out);   /* its parameters, or 0: not understood */
	const char *what;          /* for the console: what the command is and what is done with it */
};

/*
 * 0xFE37, LE Set Scan Parameters Multi-Core (Apple's): the parameters of LE Set Extended Scan Parameters (0x2041: own
 * address type, scanning filter policy, scanning PHYs), then two bytes of Apple's (scan-core settings), then per PHY in
 * the PHYs mask the scan type, interval and window and a scan-core window. Phone 1, 17 November: "01 00 01 02 01 01 40
 * 00 30 00 1e 00" and "01 00 01 02 02 00 e0 01 2b 00 1e 00" (own address 01, policy 00, PHYs 01 = LE 1M, Apple's 02 and
 * 01 or 02, one entry: active 0x40/0x30, passive 0x1e0/0x2b, scan core 0x1e), read in that order from bluetoothd's
 * packing of its arguments. This chip has no scan core: LE Set Scan Parameters (0x200B: type, interval, window, own
 * address type, policy) with the LE 1M entry (the first: the PHYs mask's lowest bit) is what it can do.
 */
static inline uint8_t
bt_vsc_x_scan_params(const uint8_t *p, uint8_t n, uint8_t *out)
{
	unsigned phys = 0;
	uint16_t interval, window;

	for (unsigned bit = 0; bit < 8; bit++) {
		phys += (p[n >= 3 ? 2 : 0] >> bit) & 1u;
	}
	if (n < 12 || n != 5u + 7u * phys || (p[2] & 0x01) == 0) {
		return 0;                       /* not the shape above, or no LE 1M entry */
	}
	interval = (uint16_t)(p[6] | p[7] << 8);
	window = (uint16_t)(p[8] | p[9] << 8);
	if (p[5] > 1 || p[0] > 3 || p[1] > 3 || interval < 4 || interval > 0x4000 || window < 4 || window > interval) {
		return 0;
	}
	out[0] = p[5];
	out[1] = p[6];
	out[2] = p[7];
	out[3] = p[8];
	out[4] = p[9];
	out[5] = p[0];
	out[6] = p[1];
	return 7;
}

/*
 * 0xFE29, LE Set Scan Enable (Apple's): five bytes and two 16-bit words, enable and filter-duplicates first (the order of
 * bluetoothd's arguments; NOT yet seen in a trace on this chip). Sent as LE Set Scan Enable (0x200C: enable, filter
 * duplicates); anything but 0 or 1 in the first byte is not understood.
 */
static inline uint8_t
bt_vsc_x_scan_enable(const uint8_t *p, uint8_t n, uint8_t *out)
{
	if (n != 9 || p[0] > 1) {
		return 0;
	}
	out[0] = p[0];
	out[1] = p[1] != 0 ? 1 : 0;
	return 2;
}

/*
 * The table. Each entry: the opcode, the parameters it applies to, what bluetoothd is told or what is sent instead, and
 * why that is true. Add an entry only from a trace of bluetoothd on this chip (bt_trace=N) and the reply's shape as
 * bluetoothd reads it. Seen on phone 1 (17 November) and left as the chip answers, because bluetoothd goes on after them:
 * 0xFDFD with 01 (HDR on), 0xFFF2 01 14 00 01, 0xFCE9 (status 11), 0xFE2C (Apple's advertising-buffer filters).
 * Every ANSWER reply fits an event: 4 + BT_VSC_RET_MAX + BT_VSC_ZEROS_MAX <= 255.
 */
static const struct bt_vsc_rule bt_vsc_table[] = {
	/*
	 * 0xFDDA, Read Proprietary Local Features (no parameters): Apple's bitmask of the Apple-only controller features a
	 * chip has. bluetoothd reads exactly one little-endian 32-bit word after the status and aborts on fewer bytes. The
	 * BCM4361 with Samsung's patch has none of those features (it does not even know the command), so: status 00 and
	 * the mask 0.
	 */
	{ 0xFDDA, BT_VSC_ANY, BT_VSC_ANY, 0x00, 4, { 0, 0, 0, 0 }, 0, 0, NULL,
	  "Read Proprietary Local Features: none (mask 0), as this chip has none of Apple's" },
	/*
	 * 0xFDFD, HDR Control (one byte: 1 on, 0 off): Apple's High Data Rate BR/EDR mode of its own controllers. Its reply
	 * is the status alone. Off (0) is answered done: HDR is off on this chip, as it has none. On (1) is left rejected:
	 * the chip cannot do it (and bluetoothd goes on after that rejection).
	 */
	{ 0xFDFD, 0x00, BT_VSC_ANY, 0x00, 0, { 0 }, 0, 0, NULL,
	  "HDR Control off: done (this chip has no HDR, so it is off); HDR on stays refused" },
	/*
	 * 0xFE3B, LE Meta Get LP-Core Statistics (one byte; phone 1 sent 01): the counters of Apple's low-power scan core.
	 * bluetoothd's parser (_BCM_LowPowerCoreStatsEventParser, bcm_vsc.c) reads them in order as 32- and 16-bit words,
	 * each asserting that it is there ("ByteStream_NumReadBytesAvail(*pBs) >= 4" or ">= 2") and none checking that the
	 * reply ends after them: iOS 27.0.1 (24A446) reads 48 and 8 of them (208 bytes); iOS 27.2.1 (24B156) reads 49 and 8
	 * (43 x 32, 4 x 16, 2 x 32, 4 x 16, 4 x 32 bits: 212 bytes; its last check is at byte 208). 208 bytes made 24B156's
	 * bluetoothd abort on that last check and restart, 15-17 times in 15 minutes (phone 2, 19 November). The reply is the
	 * 212 bytes 24B156 reads, which is also "at least 208" for 24A446; more would only be bytes no parser reads. It was
	 * the last command refused before each restart on phone 1 (17 November). This chip has no LP core, so every counter
	 * of it is zero: status 00 and 212 zero bytes. (BT_NO_SCAN_CORE=1 on the root should stop bluetoothd asking.)
	 */
	{ 0xFE3B, BT_VSC_ANY, BT_VSC_ANY, 0x00, 0, { 0 }, 212, 0, NULL,
	  "LP-core statistics: all zero (this chip has no LP core)" },
	/* bluetoothd restarts when this one is refused (phone 1, 17 November): above */
	{ 0xFE37, BT_VSC_ANY, BT_VSC_ANY, 0, 0, { 0 }, 0, 0x200B, bt_vsc_x_scan_params,
	  "LE scan parameters (Apple's multi-core form) sent as LE Set Scan Parameters (0x200B), its LE 1M entry; this chip "
	  "has no scan core" },
	{ 0xFE29, BT_VSC_ANY, BT_VSC_ANY, 0, 0, { 0 }, 0, 0x200C, bt_vsc_x_scan_enable,
	  "LE scan enable (Apple's form) sent as LE Set Scan Enable (0x200C)" },
};

struct bt_vsc {
	bool table, log;                  /* bt_vsc=1: the table applies; bt_vsc=log (or 1): rejections are reported */
	bool ok_all;                      /* bt_vsc_ok (tests): "all" */
	uint16_t ok[BT_VSC_LIST_MAX], not_ok[BT_VSC_LIST_MAX];
	unsigned ok_n, not_ok_n;
	/* host to chip: a command is held whole; data passes byte by byte */
	uint8_t h_buf[BT_VSC_OUT_MAX];
	uint32_t h_at, h_total;
	/* commands in flight whose reply is looked at: an ANSWER entry's (reply_as 0) or one made by TRANSLATE */
	struct { uint16_t opcode, reply_as; uint8_t p[2], n; bool live; } pend[BT_VSC_PENDING];
	/* chip to host: an event is held whole; data passes byte by byte */
	uint8_t c_buf[BT_VSC_OUT_MAX];
	uint32_t c_at, c_total;
	/* what the driver reports, set here and cleared by the driver: the chip's side (its pump) ... */
	bool note;                        /* a vendor command's first rejection */
	uint16_t note_opcode;
	uint8_t note_status;
	int note_rule;                    /* the table's entry that answered it, -1 none, -2 bt_vsc_ok */
	uint16_t said[BT_VSC_SAID_MAX];
	unsigned said_n;
	/* ... and the host's (the client's send): a vendor command's first translation */
	bool xnote;
	int xnote_rule;
	uint16_t xsaid[BT_VSC_SAID_MAX];
	unsigned xsaid_n;
	uint32_t answered, translated, legacy;   /* replies made from the table, commands translated, bt_vsc_ok's 00s */
};

static inline bool
bt_vsc_is_vendor(uint16_t opcode)
{
	return (opcode >> 10) == 0x3F;
}

/* a new client: both directions start at a packet's start; nothing in flight */
static inline void
bt_vsc_reset(struct bt_vsc *v)
{
	v->h_at = v->h_total = 0;
	v->c_at = v->c_total = 0;
	for (unsigned i = 0; i < BT_VSC_PENDING; i++) {
		v->pend[i].live = false;
	}
}

/* bt_vsc_ok's list: hex opcodes joined by ',' or ' ', "all", "-OPCODE" to leave one out; anything else passed over */
static inline void
bt_vsc_parse_ok(struct bt_vsc *v, const char *s)
{
	size_t at = 0;

	v->ok_n = v->not_ok_n = 0;
	v->ok_all = s[0] == 'a' && s[1] == 'l' && s[2] == 'l';
	while (s[at] != 0) {
		bool neg = s[at] == '-';
		size_t p = at + (neg ? 1 : 0), digits = 0;
		uint32_t val = 0;

		if (s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) {
			p += 2;
		}
		for (;; p++, digits++) {
			char c = s[p];
			uint32_t d;

			if (c >= '0' && c <= '9') {
				d = (uint32_t)(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				d = (uint32_t)(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				d = (uint32_t)(c - 'A' + 10);
			} else {
				break;
			}
			val = val > 0xFFFFu ? val : val * 16 + d;   /* past 0xFFFF it stays too big and is passed over */
		}
		if (digits > 0 && val > 0 && val <= 0xFFFF) {
			if (neg && v->not_ok_n < BT_VSC_LIST_MAX) {
				v->not_ok[v->not_ok_n++] = (uint16_t)val;
			} else if (!neg && v->ok_n < BT_VSC_LIST_MAX) {
				v->ok[v->ok_n++] = (uint16_t)val;
			}
		}
		at = p > at ? p : at + 1;
		while (s[at] == ',' || s[at] == ' ') {
			at++;
		}
	}
}

/* the ANSWER entries' opcodes */
static inline bool
bt_vsc_answers(uint16_t opcode)
{
	for (size_t i = 0; i < sizeof(bt_vsc_table) / sizeof(bt_vsc_table[0]); i++) {
		if (bt_vsc_table[i].opcode == opcode && bt_vsc_table[i].to == 0) {
			return true;
		}
	}
	return false;
}

/* a command in flight: its first parameters kept until its reply, and the opcode that reply is given under (0: its own) */
static inline void
bt_vsc_pend(struct bt_vsc *v, uint16_t opcode, uint16_t reply_as, const uint8_t *p, uint8_t n)
{
	unsigned free_slot = BT_VSC_PENDING;

	for (unsigned i = 0; i < BT_VSC_PENDING; i++) {
		if (v->pend[i].live && v->pend[i].opcode == opcode) {
			free_slot = i;          /* the same opcode again: the newer one is what the next reply answers */
			break;
		}
		if (!v->pend[i].live && free_slot == BT_VSC_PENDING) {
			free_slot = i;
		}
	}
	if (free_slot == BT_VSC_PENDING) {
		free_slot = 0;                  /* full: the oldest slot goes (its reply then passes as the chip gives it) */
	}
	v->pend[free_slot].opcode = opcode;
	v->pend[free_slot].reply_as = reply_as;
	v->pend[free_slot].n = n > 2 ? 2 : n;
	v->pend[free_slot].p[0] = n > 0 ? p[0] : 0;
	v->pend[free_slot].p[1] = n > 1 ? p[1] : 0;
	/* the reply is read by another thread (the driver's pump): the slot is whole before it is live */
	__atomic_store_n(&v->pend[free_slot].live, true, __ATOMIC_RELEASE);
}

/* the live slot for a reply to opcode: translated (true) or not; BT_VSC_PENDING if none */
static inline unsigned
bt_vsc_slot(struct bt_vsc *v, uint16_t opcode, bool translated)
{
	for (unsigned i = 0; i < BT_VSC_PENDING; i++) {
		if (__atomic_load_n(&v->pend[i].live, __ATOMIC_ACQUIRE) && v->pend[i].opcode == opcode &&
		    (v->pend[i].reply_as != 0) == translated) {
			return i;
		}
	}
	return BT_VSC_PENDING;
}

/* a whole command from the host into out, as it is or as the table translates it; returns its length */
static inline size_t
bt_vsc_command(struct bt_vsc *v, uint8_t *out)
{
	const uint8_t *c = v->h_buf;
	uint16_t op = (uint16_t)(c[2] << 8 | c[1]);
	uint8_t len = c[3];

	memcpy(out, c, v->h_total);
	if (!v->table || !bt_vsc_is_vendor(op)) {
		return v->h_total;
	}
	for (size_t r = 0; r < sizeof(bt_vsc_table) / sizeof(bt_vsc_table[0]); r++) {
		const struct bt_vsc_rule *e = &bt_vsc_table[r];
		uint8_t n;
		bool said = false;

		if (e->opcode != op || e->to == 0 || e->xlate == NULL || (n = e->xlate(c + 4, len, out + 4)) == 0) {
			continue;
		}
		out[1] = (uint8_t)e->to;
		out[2] = (uint8_t)(e->to >> 8);
		out[3] = n;
		bt_vsc_pend(v, e->to, op, NULL, 0);
		v->translated++;
		for (unsigned k = 0; k < v->xsaid_n; k++) {
			said |= v->xsaid[k] == op;
		}
		if (!said && v->xsaid_n < BT_VSC_SAID_MAX) {
			v->xsaid[v->xsaid_n++] = op;
			v->xnote = true;
			v->xnote_rule = (int)r;
		}
		return 4u + n;
	}
	if (bt_vsc_answers(op)) {
		bt_vsc_pend(v, op, 0, c + 4, len);
	}
	return v->h_total;
}

/*
 * One byte from the host (bluetoothd, BlueTool) on its way to the chip: returns how many bytes are now ready in out
 * (room for BT_VSC_OUT_MAX). Data packets pass at once; a command is held until it is whole.
 */
static inline size_t
bt_vsc_host_byte(struct bt_vsc *v, uint8_t b, uint8_t *out)
{
	uint32_t i = v->h_at;
	uint8_t t = i == 0 ? b : v->h_buf[0];

	if (i == 0 && b != 0x01 && b != 0x02 && b != 0x03) {
		out[0] = b;                     /* not a packet type: passed, and the next byte may start one */
		return 1;
	}
	v->h_at++;
	if (i < BT_VSC_OUT_MAX) {
		v->h_buf[i] = b;
	}
	if (v->h_total == 0) {
		if (t == 0x01 && i == 3) {
			v->h_total = 4u + b;
		} else if (t == 0x03 && i == 3) {
			v->h_total = 4u + b;
		} else if (t == 0x02 && i == 4) {
			v->h_total = 5u + ((uint32_t)b << 8 | v->h_buf[3]);
		}
	}
	if (t != 0x01) {
		out[0] = b;
		if (v->h_total != 0 && v->h_at >= v->h_total) {
			v->h_at = v->h_total = 0;
		}
		return 1;
	}
	if (v->h_total == 0 || v->h_at < v->h_total) {
		return 0;
	}
	{
		size_t n = bt_vsc_command(v, out);

		v->h_at = v->h_total = 0;
		return n;
	}
}

/* the table's entry for this rejected command, with the parameters it was sent with, or -1 */
static inline int
bt_vsc_rule_for(struct bt_vsc *v, uint16_t opcode)
{
	int got = -1;
	unsigned slot = bt_vsc_slot(v, opcode, false);

	if (slot == BT_VSC_PENDING) {
		return -1;                      /* not seen going out: nothing is made up for it */
	}
	for (size_t r = 0; r < sizeof(bt_vsc_table) / sizeof(bt_vsc_table[0]) && got < 0; r++) {
		const struct bt_vsc_rule *e = &bt_vsc_table[r];

		if (e->opcode != opcode || e->to != 0) {
			continue;
		}
		if (e->p0 != BT_VSC_ANY && (v->pend[slot].n < 1 || v->pend[slot].p[0] != (uint8_t)e->p0)) {
			continue;
		}
		if (e->p1 != BT_VSC_ANY && (v->pend[slot].n < 2 || v->pend[slot].p[1] != (uint8_t)e->p1)) {
			continue;
		}
		got = (int)r;
	}
	v->pend[slot].live = false;
	return got;
}

/* a whole event from the chip: copied to out as it is, or as the table answers it; returns its length */
static inline size_t
bt_vsc_event(struct bt_vsc *v, uint8_t *out)
{
	uint8_t *e = v->c_buf;
	size_t len = v->c_total;
	uint16_t op;
	uint8_t status;
	int rule = -1;
	bool said = false;

	memcpy(out, e, len);
	if (v->table && ((e[1] == 0x0E && len >= 6 && e[2] >= 3) || (e[1] == 0x0F && len >= 7 && e[2] >= 4))) {
		unsigned at = e[1] == 0x0E ? 4 : 5, slot;

		op = (uint16_t)(e[at + 1] << 8 | e[at]);
		slot = bt_vsc_is_vendor(op) ? BT_VSC_PENDING : bt_vsc_slot(v, op, true);
		if (slot != BT_VSC_PENDING) {
			/* the reply to a command made here: given under the vendor command's opcode, with the chip's status */
			out[at] = (uint8_t)v->pend[slot].reply_as;
			out[at + 1] = (uint8_t)(v->pend[slot].reply_as >> 8);
			v->pend[slot].live = false;
			return len;
		}
	}
	if (len < 7 || e[1] != 0x0E || e[2] < 4) {
		return len;                     /* not a Command Complete with a status */
	}
	op = (uint16_t)(e[5] << 8 | e[4]);
	status = e[6];
	if (!bt_vsc_is_vendor(op)) {
		return len;
	}
	if (v->table) {
		rule = bt_vsc_rule_for(v, op);  /* any reply ends the command's wait, whatever its status */
	}
	if (status == 0) {
		return len;
	}
	if (rule >= 0 && status != 0x01) {
		rule = -1;                      /* only a command the chip does not know is answered */
	}
	if (rule >= 0) {
		const struct bt_vsc_rule *r = &bt_vsc_table[rule];
		uint8_t n = r->ret_len > BT_VSC_RET_MAX ? BT_VSC_RET_MAX : r->ret_len;
		uint8_t z = r->zeros > BT_VSC_ZEROS_MAX ? BT_VSC_ZEROS_MAX : r->zeros;

		out[2] = (uint8_t)(4 + n + z);  /* Num_HCI_Command_Packets, the opcode, the status, the return parameters */
		out[6] = r->status;
		memcpy(out + 7, r->ret, n);
		memset(out + 7 + n, 0, z);
		len = 7u + n + z;
		v->answered++;
	} else {
		bool ok = v->ok_all;

		for (unsigned k = 0; k < v->ok_n; k++) {
			ok |= v->ok[k] == op;
		}
		for (unsigned k = 0; k < v->not_ok_n; k++) {
			ok &= v->not_ok[k] != op;
		}
		if (ok) {
			out[6] = 0;
			rule = -2;
			v->legacy++;
		}
	}
	for (unsigned k = 0; k < v->said_n; k++) {
		said |= v->said[k] == op;
	}
	if (!said && v->said_n < BT_VSC_SAID_MAX) {
		v->said[v->said_n++] = op;
		v->note = true;
		v->note_opcode = op;
		v->note_status = status;
		v->note_rule = rule;
	}
	return len;
}

/*
 * One byte from the chip on its way to the client: returns how many bytes are now ready in out (room for
 * BT_VSC_OUT_MAX). Data packets pass at once; an event is held until it is whole, then given as it is or as answered.
 */
static inline size_t
bt_vsc_chip_byte(struct bt_vsc *v, uint8_t b, uint8_t *out)
{
	uint32_t i = v->c_at;
	uint8_t t = i == 0 ? b : v->c_buf[0];

	if (i == 0 && b != 0x02 && b != 0x03 && b != 0x04) {
		out[0] = b;                     /* not a packet type: passed, and the next byte may start one */
		return 1;
	}
	v->c_at++;
	if (i < 5) {
		v->c_buf[i] = b;
	}
	if (v->c_total == 0) {
		if (t == 0x04 && i == 2) {
			v->c_total = 3u + b;
		} else if (t == 0x03 && i == 3) {
			v->c_total = 4u + b;
		} else if (t == 0x02 && i == 4) {
			v->c_total = 5u + ((uint32_t)b << 8 | v->c_buf[3]);
		}
	}
	if (t != 0x04) {
		out[0] = b;
		if (v->c_total != 0 && v->c_at >= v->c_total) {
			v->c_at = v->c_total = 0;
		}
		return 1;
	}
	if (i < BT_VSC_OUT_MAX) {
		v->c_buf[i] = b;
	}
	if (v->c_total == 0 || v->c_at < v->c_total) {
		return 0;
	}
	{
		size_t n = bt_vsc_event(v, out);

		v->c_at = v->c_total = 0;
		return n;
	}
}

#endif /* _HAKIOS_BT_VSC_H_ */
