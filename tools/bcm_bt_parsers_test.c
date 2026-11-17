/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/bcm_bt_parsers_test.c: the parsers of drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c that are in the driver
 * itself, on a computer under the sanitizers (tools/bcm-bt-parsers-test.sh takes them out of the driver as they stand
 * into bt_pieces.h). The chip passes on what radio peers send, so every count and length here is taken as hostile.
 *
 * What stands in for the kernel: say() (formats into a buffer, so that its arguments are read), the boot-argument
 * reader (gives the text the test sets), the clock (a counter), strlcpy (Linux's C library has none before 2.38).
 *
 * What is checked:
 *   - scan_event() and ad_name(): good Inquiry Result (02), with RSSI (22), Extended (2F) and LE Advertising Report
 *     (3E/02) events put the devices, their RSSI and names in the list; events whose counts and lengths lie (255
 *     responses in a short event, an advertising report longer than the event, a name longer than its data, a data
 *     structure of length 0 or past the end) are read no further than the event's own length: each event is held in
 *     a buffer of exactly 2 + length bytes, so the sanitizer stops any read past it; the list stops at SCAN_MAX
 *     devices; a name is at most 32 characters and ends;
 *   - 300000 events made from the good ones by a fixed-seed mutator, the same way;
 *   - vsc_byte() (bt_vsc_ok): a rejected vendor command that is listed has its status set to 00 and nothing else in
 *     the stream changes; one not listed passes as it is; ACL packets of any length and mutated streams leave it at a
 *     packet's start again after a reset, and it writes nothing outside its seven header bytes; the boot-argument's
 *     list longer than the tables is cut, not written past them;
 *   - trace_bytes() and trace_line() (bt_trace): mutated streams with the longest numbers the line can carry.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* ---- the kernel's side ---- */
static char said[1024];
static unsigned long lines;

static int
say(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(said, sizeof(said), fmt, ap);
	va_end(ap);
	lines++;
	if (n < 0 || (size_t)n >= sizeof(said)) {
		CHECK(false, "a line of %d characters", n);
	}
	return n;
}

static size_t
test_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t n = strlen(src);

	if (cap != 0) {
		size_t k = n < cap - 1 ? n : cap - 1;

		memcpy(dst, src, k);
		dst[k] = 0;
	}
	return n;
}
#define strlcpy test_strlcpy

static const char *boot_arg;

static bool
PE_parse_boot_argn(const char *name, void *out, int cap)
{
	(void)name;
	if (boot_arg == NULL) {
		return false;
	}
	test_strlcpy(out, boot_arg, (size_t)cap);
	return true;
}

#define NSEC_PER_SEC    1000000000ull
#define NSEC_PER_MSEC   1000000ull
static uint64_t clock_now = 0xFFFFFFFF00000000ull;      /* a late time: the longest numbers a line can carry */

static uint64_t
mach_absolute_time(void)
{
	return clock_now += 1000;
}

static void
absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns)
{
	*ns = t;
}

#define SYSCTL_DECL(x)  extern int bt_test_no_sysctl
#define SYSCTL_UINT(...) extern int bt_test_no_sysctl
#define CTLFLAG_RW 0
#define CTLFLAG_KERN 0
#define CTLFLAG_LOCKED 0
#define OID_AUTO 0

static bool inquiry_done;

#include "bt_pieces.h"          /* the driver's own lines (tools/bcm-bt-parsers-test.sh) */

/* ---- the tests ---- */
static uint32_t mut_state = 0xb1e5ca4u;

static uint32_t
mut(void)
{
	mut_state ^= mut_state << 13;
	mut_state ^= mut_state >> 17;
	mut_state ^= mut_state << 5;
	return mut_state;
}

/* an event [code][len][params] handed to scan_event() from a buffer of exactly its size */
static void
scan(uint8_t code, const uint8_t *params, unsigned len)
{
	uint8_t *e = malloc(2 + len);

	e[0] = code;
	e[1] = (uint8_t)len;
	if (len != 0) {
		memcpy(e + 2, params, len);
	}
	scan_event(e);
	free(e);
}

static void
list_clear(void)
{
	nseen = 0;
	nreports = 0;
	memset(seen, 0, sizeof(seen));
}

static void
list_holds(void)
{
	if (nseen > SCAN_MAX) {
		CHECK(false, "%u devices in a list of %u", nseen, (unsigned)SCAN_MAX);
	}
	for (unsigned i = 0; i < nseen && i < SCAN_MAX; i++) {
		if (memchr(seen[i].name, 0, sizeof(seen[i].name)) == NULL) {
			CHECK(false, "a name that does not end");
		}
	}
}

static const uint8_t inq[] = { 2,
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 1, 0, 0, 0x0c, 0x02, 0x5a, 0x34, 0x12,
	0x21, 0x22, 0x33, 0x44, 0x55, 0x66, 1, 0, 0, 0x04, 0x04, 0x24, 0x78, 0x56 };
static const uint8_t inq_rssi[] = { 1, 0x31, 0x22, 0x33, 0x44, 0x55, 0x66, 1, 0, 0x0c, 0x02, 0x5a, 0x34, 0x12, 0xc4 };
static const uint8_t inq_ext[] = { 1, 0x41, 0x22, 0x33, 0x44, 0x55, 0x66, 1, 0, 0x0c, 0x02, 0x5a, 0x34, 0x12, 0xb0,
	0x02, 0x01, 0x06, 0x04, 0x09, 'S', '9', '!', 0x03, 0x08, 'a', 'b', 0x00, 0x00 };
static const uint8_t le_adv[] = { 0x02, 2,
	0x00, 0x01, 0x51, 0x22, 0x33, 0x44, 0x55, 0x66, 7, 0x02, 0x01, 0x06, 0x03, 0x09, 'h', 'i', 0xc0,
	0x04, 0x00, 0x61, 0x22, 0x33, 0x44, 0x55, 0x66, 0, 0xa0 };

static void
scan_good(void)
{
	list_clear();
	inquiry_done = false;
	scan(0x02, inq, sizeof(inq));
	CHECK(nseen == 2 && nreports == 2 && seen[0].cod == 0x5a020c && seen[1].cod == 0x240404 && seen[0].le == 0,
	    "Inquiry Result: %u devices, classes %06x %06x", nseen, seen[0].cod, seen[1].cod);
	scan(0x22, inq_rssi, sizeof(inq_rssi));
	CHECK(nseen == 3 && seen[2].rssi == -60 && seen[2].cod == 0x5a020c, "with RSSI: %u devices, rssi %d", nseen, seen[2].rssi);
	scan(0x2F, inq_ext, sizeof(inq_ext));
	CHECK(nseen == 4 && seen[3].rssi == -80 && strcmp(seen[3].name, "S9!") == 0, "Extended: \"%s\" rssi %d", seen[3].name,
	    seen[3].rssi);
	scan(0x3E, le_adv, sizeof(le_adv));
	CHECK(nseen == 6 && seen[4].le == 1 && seen[4].addr_type == 1 && seen[4].rssi == -64 && strcmp(seen[4].name, "hi") == 0
	    && seen[5].rssi == -96 && seen[5].name[0] == 0, "LE reports: %u devices, \"%s\" %d, %d", nseen, seen[4].name,
	    seen[4].rssi, seen[5].rssi);
	scan(0x3E, le_adv, sizeof(le_adv));
	CHECK(nseen == 6 && seen[4].hits == 2 && nreports == 8, "heard again: listed once (%u, %u hits)", nseen, seen[4].hits);
	scan(0x01, (const uint8_t *)"\0", 1);
	CHECK(inquiry_done, "Inquiry Complete");
	list_holds();
}

static void
scan_hostile(void)
{
	uint8_t p[255];

	/* 255 responses said, one there */
	list_clear();
	memcpy(p, inq, 15);
	p[0] = 255;
	scan(0x02, p, 15);
	scan(0x22, p, 15);
	CHECK(nseen == 1 && nreports == 2, "255 responses said, one read (%u devices, %u reports)", nseen, nreports);
	scan(0x02, p, 14);              /* one byte short of a response */
	scan(0x22, p, 1);
	scan(0x02, p, 0);
	scan(0x2F, p, 14);
	scan(0x2F, p, 0);
	CHECK(nseen == 1 && nreports == 2, "events too short for a response: nothing read");

	/* an LE report whose data length runs past the event; 255 reports said */
	list_clear();
	memcpy(p, le_adv, sizeof(le_adv));
	p[1] = 255;
	p[10] = 200;
	scan(0x3E, p, sizeof(le_adv));
	CHECK(nseen == 0, "an advertising report longer than its event: not read");
	p[10] = 7;
	scan(0x3E, p, sizeof(le_adv));
	CHECK(nseen == 2, "255 reports said, two read (%u)", nseen);
	for (unsigned len = 0; len <= sizeof(le_adv); len++) {
		scan(0x3E, p, len);
	}
	CHECK(nseen == 2, "the event cut at every length");

	/* names: longer than the list's 32 characters, longer than their data, a structure of length 0, of length 1 */
	list_clear();
	memset(p, 'n', sizeof(p));
	memcpy(p, inq_ext, 15);
	p[15] = 239;                    /* 1 + 238 name bytes: to the event's last byte */
	p[16] = 0x09;
	scan(0x2F, p, 255);
	CHECK(nseen == 1 && strlen(seen[0].name) == 32, "a name of 238 bytes kept as 32 (%zu)", strlen(seen[0].name));
	list_clear();
	p[15] = 240;                    /* one past the event's end: left */
	scan(0x2F, p, 255);
	CHECK(nseen == 1 && seen[0].name[0] == 0, "a name longer than its data: left");
	p[15] = 255;
	scan(0x2F, p, 255);
	scan(0x2F, p, 17);
	p[15] = 1;                      /* a structure with a type and nothing else, then the same again and again */
	memset(p + 16, 1, sizeof(p) - 16);
	p[16] = 0x09;
	scan(0x2F, p, 255);
	p[15] = 0;
	scan(0x2F, p, 255);
	CHECK(seen[0].name[0] == 0, "empty and endless structures: no name");
	{
		uint8_t adv[255];

		memset(adv, 0x7e, sizeof(adv));
		memcpy(adv, le_adv, 10);
		adv[1] = 1;
		adv[10] = 255 - 12;     /* data to the event's end but for the RSSI */
		adv[11] = 255 - 14;
		adv[12] = 0x08;
		scan(0x3E, adv, 255);
		CHECK(nseen == 2 && strlen(seen[1].name) == 32, "an advertised name filling the event kept as 32");
	}
	list_holds();

	/* more devices than the list holds */
	list_clear();
	for (unsigned d = 0; d < 300; d++) {
		memcpy(p, inq_rssi, sizeof(inq_rssi));
		p[1] = (uint8_t)d;
		p[2] = (uint8_t)(d >> 8);
		scan(0x22, p, sizeof(inq_rssi));
		memcpy(p, le_adv, sizeof(le_adv));
		p[4] = (uint8_t)d;
		p[5] = (uint8_t)(d >> 8);
		scan(0x3E, p, sizeof(le_adv));
	}
	CHECK(nseen == SCAN_MAX && nreports == 900, "600 devices: %u listed, %u reports", nseen, nreports);
	list_holds();
}

static void
scan_mutated(void)
{
	static const struct { uint8_t code; const uint8_t *p; size_t n; } good[] = {
		{ 0x02, inq, sizeof(inq) }, { 0x22, inq_rssi, sizeof(inq_rssi) }, { 0x2F, inq_ext, sizeof(inq_ext) },
		{ 0x3E, le_adv, sizeof(le_adv) },
	};
	unsigned long heard_any = 0;

	list_clear();
	for (int round = 0; round < 300000; round++) {
		unsigned w = mut() % (sizeof(good) / sizeof(good[0]));
		uint8_t p[255];
		size_t n = good[w].n;
		uint8_t code = good[w].code;
		unsigned before = nreports;

		memcpy(p, good[w].p, n);
		if (mut() % 4 == 0) {           /* padded with noise to a longer event */
			size_t to = n + mut() % (sizeof(p) - n + 1);

			for (size_t i = n; i < to; i++) {
				p[i] = (uint8_t)mut();
			}
			n = to;
		}
		for (int m = 1 + (int)(mut() % 4); m > 0 && n > 0; m--) {
			size_t at = mut() % n;

			switch (mut() % 6) {
			case 0: p[at] = (uint8_t)mut(); break;
			case 1: p[at] = 0xff; break;
			case 2: p[at] ^= (uint8_t)(1u << (mut() % 8)); break;
			case 3: n = at; break;
			case 4: code = (uint8_t)(mut() % 2 ? good[mut() % 4].code : mut()); break;
			default: p[at] = (uint8_t)(p[at] + 1); break;
			}
		}
		scan(code, p, (unsigned)n);
		heard_any += nreports != before;
		if (round % 1024 == 0) {
			list_holds();
			if (mut() % 4 == 0) {
				list_clear();
			}
		}
	}
	list_holds();
	CHECK(heard_any > 30000, "the mutated events still carry reports (%lu)", heard_any);
}

/* ---- bt_vsc_ok ---- */
static void
vsc(void)
{
	/* Command Complete for vendor command fd5a: status 01 (unknown command) */
	static const uint8_t rejected[] = { 0x04, 0x0e, 0x04, 0x01, 0x5a, 0xfd, 0x01 };
	static const uint8_t other[] = { 0x04, 0x0e, 0x04, 0x01, 0x73, 0xfd, 0x01 };
	static const uint8_t std_rej[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x01 };
	static const uint8_t acl[] = { 0x02, 0x01, 0x20, 0x07, 0x00, 0x04, 0x0e, 0x04, 0x01, 0x5a, 0xfd, 0x01 };
	uint8_t out[32];
	char longlist[128];
	size_t k = 0;

	/* a list longer than the tables: 30 opcodes to answer, 30 to leave out */
	for (int i = 0; i < 12; i++) {
		k += (size_t)snprintf(longlist + k, sizeof(longlist) - k, "fd%02x,-fc%02x,", 0x5a + i, i);
	}
	longlist[sizeof(longlist) - 1] = 0;
	boot_arg = longlist;
	vsc_init();
	CHECK(vsc_on && !vsc_all && vsc_ok_n <= sizeof(vsc_ok) / sizeof(vsc_ok[0]) && vsc_not_n <= sizeof(vsc_not) /
	    sizeof(vsc_not[0]) && vsc_ok[0] == 0xfd5a, "the list read: %u to answer, %u to leave", vsc_ok_n, vsc_not_n);
	vsc_ok_n = vsc_not_n = 0;
	boot_arg = "0xfd5a, ,,-,0x,zz,-0xfd73,99999999999999999999,fd5b";
	vsc_init();
	CHECK(vsc_ok_n == 2 && vsc_ok[0] == 0xfd5a && vsc_ok[1] == 0xfd5b && vsc_not_n == 1 && vsc_not[0] == 0xfd73,
	    "a list with junk in it: %u %u", vsc_ok_n, vsc_not_n);

	vsc_reset();
	for (size_t i = 0; i < sizeof(rejected); i++) {
		out[i] = vsc_byte(rejected[i]);
	}
	CHECK(memcmp(out, rejected, 6) == 0 && out[6] == 0, "a listed vendor command's rejection answered 00");
	for (size_t i = 0; i < sizeof(other); i++) {
		out[i] = vsc_byte(other[i]);
	}
	CHECK(memcmp(out, other, sizeof(other)) == 0, "one left out passes as it is");
	for (size_t i = 0; i < sizeof(std_rej); i++) {
		out[i] = vsc_byte(std_rej[i]);
	}
	CHECK(memcmp(out, std_rej, sizeof(std_rej)) == 0, "a standard command's rejection passes as it is");
	for (size_t i = 0; i < sizeof(acl); i++) {
		out[i] = vsc_byte(acl[i]);
	}
	CHECK(memcmp(out, acl, sizeof(acl)) == 0, "the same bytes inside an ACL packet pass as they are");
	for (size_t i = 0; i < sizeof(rejected); i++) {
		out[i] = vsc_byte(rejected[i]);
	}
	CHECK(out[6] == 0, "and the packet after it is found");

	/* every rejected vendor opcode once: the said-once table fills and stops */
	for (unsigned op = 0xfc00; op < 0xfd00; op++) {
		uint8_t e[7] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)op, (uint8_t)(op >> 8), 0x01 };

		for (size_t i = 0; i < sizeof(e); i++) {
			(void)vsc_byte(e[i]);
		}
	}
	CHECK(vsc_said_n == sizeof(vsc_said) / sizeof(vsc_said[0]), "the said-once table full (%u)", vsc_said_n);

	/* mutated streams; an ACL packet of the largest length */
	{
		uint8_t hdr[5] = { 0x02, 0x01, 0x20, 0xff, 0xff };

		vsc_reset();
		for (size_t i = 0; i < sizeof(hdr); i++) {
			(void)vsc_byte(hdr[i]);
		}
		for (unsigned i = 0; i < 0xffff; i++) {
			(void)vsc_byte(0x04);
		}
		CHECK(vsc_in.at == 0 && vsc_in.total == 0, "an ACL packet of 65535 bytes ends where it says");
	}
	for (int round = 0; round < 300000; round++) {
		uint8_t s[64];
		size_t n = 0;

		for (int parts = 1 + (int)(mut() % 3); parts > 0; parts--) {
			const uint8_t *g = mut() % 2 ? rejected : mut() % 2 ? acl : other;
			size_t gn = g == acl ? sizeof(acl) : sizeof(rejected);

			memcpy(s + n, g, gn);
			n += gn;
		}
		for (int m = (int)(mut() % 3); m > 0; m--) {
			size_t at = mut() % n;

			switch (mut() % 4) {
			case 0: s[at] = (uint8_t)mut(); break;
			case 1: s[at] = 0xff; break;
			case 2: s[at] ^= (uint8_t)(1u << (mut() % 8)); break;
			default: n = at + 1; break;
			}
		}
		for (size_t i = 0; i < n; i++) {
			uint8_t b = vsc_byte(s[i]);

			if (b != s[i] && b != 0) {
				CHECK(false, "a byte changed to something other than 00");
			}
		}
		if (vsc_said_n > sizeof(vsc_said) / sizeof(vsc_said[0]) || (vsc_in.total != 0 && vsc_in.at >= vsc_in.total)) {
			CHECK(false, "bt_vsc_ok's state: at %u of %u, %u said", vsc_in.at, vsc_in.total, vsc_said_n);
		}
		if (mut() % 16 == 0) {
			vsc_reset();
		}
	}
	vsc_reset();
	for (size_t i = 0; i < sizeof(rejected); i++) {
		out[i] = vsc_byte(rejected[i]);
	}
	CHECK(out[6] == 0, "after all of it, a reset and a rejection: answered 00");
}

/* ---- bt_trace ---- */
static void
trace(void)
{
	static const uint8_t cmd[] = { 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t evt[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	static const uint8_t acl[] = { 0x02, 0xff, 0x3f, 0x14, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
		                       19, 20 };
	unsigned long before;

	trace_per_s = 0;
	before = lines;
	trace_bytes(&trace_in, evt, sizeof(evt));
	CHECK(lines == before, "bt_trace off: nothing said");
	trace_per_s = 0xffffffffu;
	trace_skipped = 0xffffffffu;    /* the longest "(N not shown before)" */
	trace_bytes(&trace_out, cmd, sizeof(cmd));
	CHECK(strstr(said, "H> cmd 0c03 len 0:") != NULL, "a command's line: %s", said);
	trace_skipped = 0xffffffffu;
	trace_bytes(&trace_in, acl, sizeof(acl));
	CHECK(strstr(said, "<C acl fff pb3 len 20: 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10") != NULL &&
	    strstr(said, "(4294967295 not shown before)") != NULL && said[strlen(said) - 1] == '\n', "an ACL packet's line: %s",
	    said);
	for (size_t i = 0; i < sizeof(evt); i++) {
		trace_bytes(&trace_in, evt + i, 1);
	}
	CHECK(strstr(said, "<C evt 0e len 4: 01 03 0c 00") != NULL, "an event's line, a byte at a time: %s", said);
	for (int round = 0; round < 300000; round++) {
		uint8_t s[96];
		size_t n = 0;

		for (int parts = 1 + (int)(mut() % 3); parts > 0; parts--) {
			unsigned w = mut() % 3;
			const uint8_t *g = w == 0 ? cmd : w == 1 ? evt : acl;
			size_t gn = w == 0 ? sizeof(cmd) : w == 1 ? sizeof(evt) : sizeof(acl);

			memcpy(s + n, g, gn);
			n += gn;
		}
		for (int m = (int)(mut() % 3); m > 0; m--) {
			size_t at = mut() % n;

			switch (mut() % 4) {
			case 0: s[at] = (uint8_t)mut(); break;
			case 1: s[at] = 0xff; break;
			case 2: s[at] ^= (uint8_t)(1u << (mut() % 8)); break;
			default: n = at + 1; break;
			}
		}
		if (mut() % 4 == 0) {
			trace_skipped = 0xffffffffu;
		}
		trace_bytes(mut() % 2 ? &trace_in : &trace_out, s, n);
		if (trace_in.hlen > sizeof(trace_in.hdr) || trace_out.hlen > sizeof(trace_out.hdr)) {
			CHECK(false, "the trace's header is %u bytes", trace_in.hlen);
		}
		if (mut() % 16 == 0) {
			trace_in.type = trace_out.type = 0;     /* as a new client does (kctl_connect) */
		}
	}
}

int
main(void)
{
	scan_good();
	scan_hostile();
	scan_mutated();
	vsc();
	trace();
	printf("bcm_bt_parsers_test: %d checks, %d failed (%lu lines said)\n", checks, failures, lines);
	return failures ? 1 : 0;
}
