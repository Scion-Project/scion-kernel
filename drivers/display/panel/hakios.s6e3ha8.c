/*
 * The Galaxy S9's panel brightness (starlte, Exynos 9810): a Samsung S6E3HA8 AMOLED in MIPI command mode (4 lanes, DSC)
 * on DSIM0 (0x16080000), sent its brightness through the DSIM's command FIFO while the DECON keeps showing what the
 * boot loader set up. Only with boot-arg panel=1 (and, to blank the panel for a halt that powers the phone off, halt_off=N):
 * every address below belongs to this phone.
 *
 * The panel has no DCS "set brightness" (0x51) in Samsung's tables. Its brightness is Samsung's own: a gamma (CA), the
 * AOR (B1, the part of each frame the pixels are dark) and ELVSS, behind a level-2 key and taken by F7 03. The gamma of
 * each of its 74 luminances, 2 to 400 nits, is worked out from the panel's own factory calibration, its MTP (C8), which
 * this driver reads at start; hakios.s6e3ha8_gamma.h works it out as Samsung's kernel does, and has Samsung's tables of
 * which luminance and which AOR go with each level of 0-255. A brightness is then
 *   F0 5A 5A        the level-2 key on
 *   CA + 34 bytes   the gamma of the level's row
 *   B1 hi lo        the level's AOR (0x0B76 at 0 ... 0x000C at 255)
 *   F7 03           take them
 *   F0 A5 A5        the key off
 * the first three and the last two packets of Samsung's set-bl sequence (s6e3ha8_star_a3_s0_panel.h, _s1_:
 * star_a3_s0_set_bl_cmdtbl), with the bytes Android would send for that level. These are the panel's working state:
 * a restart clears them.
 *
 * Why each register sent is safe.
 *   F0           the level-2 key: opens Samsung's registers and closes them again. Samsung sends it around every change
 *                of brightness, and this driver did already.
 *   28, 10       display off and sleep-in, only for a halt that powers the phone off (halt_off=N: hakios_s6e3ha8_blank(),
 *                drivers/power/hakios.exynos_poweroff.c), each between the level-1 key as Samsung sends them; the
 *                panel's supplies then go with PS_HOLD.
 *   9F, FC       the level-1 and level-3 keys, only around the reads at start (and 9F around the halt's 28 and 10):
 *                Samsung's resource-init sequence opens all three for the same reads at every start of the panel
 *                (star_a3_s0_res_init_cmdtbl). A key changes nothing that is shown, and each is closed again whatever
 *                the reads gave.
 *   CA           the gamma: each value is a step of one of the panel's voltage dividers, between voltages the panel
 *                makes itself (VREGOUT, VT, the next point), and is added to the panel's own calibration: it sets how
 *                bright a gray is, not a supply. Only rows worked out from this panel's MTP are sent, each value kept
 *                within the register's range as Samsung keeps it (0-255; 0-511 at gray 255). Whatever the MTP, the
 *                400-nit row comes out as the panel's calibrated gamma itself. Android sends one of these rows at
 *                every change of brightness.
 *   B1           the AOR, two bytes from Samsung's table, within the range this driver sent before (0x000C-0x0B76).
 *   F7 03        takes what was sent ("gamma update"), as before.
 * Left out of Samsung's set-bl sequence, so as the boot loader set them:
 *   B5           TSET, MPS and ELVSS (and, at offset 22, ELVSS's temperature part). ELVSS is a supply voltage of the
 *                panel (its OLEDs' cathode), set by Samsung per luminance and per temperature, the temperature from a
 *                thermistor this driver does not read: a voltage register, so not touched. The light is the gamma's
 *                and the AOR's; what the boot loader's ELVSS does at the ends of the range (Samsung's: 0x12 at 2 nits,
 *                0x22 from 14 to about 120, 0x16 at 400; MPS 0xCC up to 39 nits, 0xDC above) is for the phone to show.
 *   F4           VGH and VINT, supply voltages: not touched. (Samsung's VINT is 0x23 at every luminance to 400 nits.)
 *   B4, 55       ACL, the current limit Samsung turns on to save power: left as it is.
 *   B8, B1+6,    IRC (17 values per brightness step, and on) and the "POC compensation" (B1 at offset 6, B8 at 0x53,
 *   B8+0x53      per luminance): Samsung's compensations of the picture, sent with the gamma on Android; the sources do
 *                not say what they compensate. Left as the boot loader set them: if colours at the ends of the range
 *                look wrong on the phone, they are the next to look at.
 *
 * The fallback, with panel_gamma=0 or when the MTP cannot be read: the AOR alone, as before. B1 from the dimmest of
 * Samsung's table for this panel (its level 0: 0x0B76 on S0, 0x0B72 on S1; 0x0B76 when the ID was not read) to 0x000C
 * by the square of the level, gamma as the boot loader left it, so the panel goes from the boot loader's level down
 * (to about 2 % of it).
 *
 * Writing a packet: its payload, four bytes a word, into PAYLOAD (0x5C), then its header into PKTHDR (0x58): the data
 * type, and the length (or the two bytes of a short one). 0x05 a DCS command without a parameter, 0x39 one with (Samsung's
 * tables send every longer one so). Before a header, the header FIFO must have room (FIFOCTRL 0x68, bit 11 full; bit 9
 * the payload's); after it, its being sent shows as INTSRC (0x50) bit 28, written back to clear. The DSIM keeps command
 * packets out of the window the panel is reading the frame in (its TE protection), so the picture is not disturbed;
 * Android sends brightness at any moment, behind a lock.
 *
 * Reading the panel (dsim_drv.c dsim_read_data, dsim_reg.c, common_mipi_lcd.c mipi_read): first "set maximum return
 * packet size" (data type 0x37, the count in the header's first byte), so the panel answers with no more than asked;
 * then the DCS read (0x06, the register in the first byte) with PKTHDR's bit 24 set: the bus is turned round for the
 * answer. The DSIM puts the answer in its RX FIFO, read a word at a time at RXFIFO (0x60); INTSRC bit 18 says it is in
 * (RX data done, written back to clear; bit 20 a turn-round timeout, 21 a low-power receive timeout), and FIFOCTRL bit
 * 12 is set while the FIFO is empty. A word's low byte is a packet's type: 0x02 acknowledge and error report (bits 23-8
 * the errors, 0x3F3F of them real ones), 0x08 end of transmission, 0x21/0x22 (DCS) or 0x11/0x12 (generic) a short
 * answer of 1 or 2 bytes in bits 15-8 and 23-16, 0x1C (DCS) or 0x1A (generic) a long one, its length in bits 23-8 and
 * its bytes in the words after it, four a word, the first lowest. Samsung waits 100 ms for an answer, takes at most 64
 * words, and wants exactly the count asked for; so does this. (A read from an offset would be preceded by B0 <offset>,
 * data type 0x15; every read here starts at 0.) Whatever is in the RX FIFO before a read is read out first.
 * Read at start, each logged (panel_gamma=1 or 2):
 *   04, 3 bytes             the panel's ID, with no key (as Samsung's read_panel_id): which of Samsung's two tables
 *   9F A5 A5, F0 5A 5A, FC 5A 5A    the three keys opened
 *   C8, 34 bytes, twice     the MTP; the two must agree, and 34 times one byte is not an MTP
 *   CA, 34 bytes; B1, 2     the gamma and AOR the boot loader left, and which of the 74 rows that gamma is, if one is:
 *                           on the phone, the check that this calculation is Samsung's
 *   FC A5 A5, F0 A5 A5, 9F 5A 5A    the keys closed
 *
 * Boot-args:
 *   panel=1             the driver: without it nothing here runs
 *   panel_brightness=N  a brightness of 0-255 set once at start
 *   panel_gamma=2       gamma and AOR sent (the default)
 *   panel_gamma=1       the panel read and its gamma worked out and logged; the AOR alone sent
 *   panel_gamma=0       the AOR alone, nothing read: the driver as it was
 *   panel_gamma_log=1   all 74 rows in the log, as Android's kernel prints them at start ("gamma[nits] : ...")
 *                       to set beside Android's log of the same phone
 *
 * Sources: the phone's device tree (dsim@0x16080000; s6e3ha8_star_ddi: mode 2, command mode, data_lane 4, dsc_en;
 * panel_drv@001's panel-lookup), Linux's drivers/video/fbdev/exynos/dpu_9810 (dsim_drv.c, dsim_reg.c, regs-dsim.h,
 * dsim.h, panels/common_mipi_lcd.c), include/video/mipi_display.h, and panel/ (panel.c, panel_bl.c, s6e3ha8/) of the
 * S9's kernel, github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, lineage-24.0 (GPL: read for these facts, not
 * copied).
 *
 * kern.hakios.brightness: 0-255, written to set it (read: what it was set to, -1 never). CTLFLAG_KERN, so that the USB
 * console's "sysctl kern.hakios.brightness" reaches it too (XNU refuses the kernel any sysctl without it, EPERM). What iOS sets reaches the
 * panel through hakios.backlight.cpp, where this driver registers at start: the display service's "brightness", and with
 * backlight=2 AppleARMBacklight's. Each set the DSIM took is logged ("s6e3ha8: brightness N sent (..., AOR 0x....)"),
 * at most once a second, with how many were left out since the last line.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hakios.s6e3ha8_gamma.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern void loader_log_line(const char *line);
extern int hakios_panel_register(const char *name, int (*set)(int level));     /* hakios.backlight.cpp */

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define DSIM_PA         0x16080000UL
#define DSIM_CONFIG     0x4C
#define  PER_FRAME_READ (1u << 8)       /* (a read only once a frame; Samsung leaves it off) */
#define DSIM_INTSRC     0x50
#define  SFR_PH_EMPTY   (1u << 28)
#define  LPRX_TIMEOUT   (1u << 21)
#define  BTA_TIMEOUT    (1u << 20)
#define  RX_DATA_DONE   (1u << 18)
#define DSIM_PKTHDR     0x58
#define  PKTHDR_BTA     (1u << 24)      /* turn the bus round after this packet: the panel answers */
#define DSIM_PAYLOAD    0x5C
#define DSIM_RXFIFO     0x60
#define DSIM_FIFOCTRL   0x68
#define  EMPTY_RX       (1u << 12)
#define  FULL_PH        (1u << 11)
#define  EMPTY_PH       (1u << 10)      /* (the header FIFO empty: Samsung takes that for "sent" as well) */
#define  FULL_PL        (1u << 9)
#define RX_WORDS        64              /* Samsung's DSIM_RX_FIFO_MAX_DEPTH */

#define DCS_SHORT       0x05
#define DCS_READ        0x06
#define MAX_RETURN      0x37            /* set maximum return packet size */
#define DCS_LONG        0x39
#define RX_ACK_ERROR    0x02
#define RX_EOT          0x08
#define RX_GEN_SHORT_1  0x11
#define RX_GEN_SHORT_2  0x12
#define RX_GEN_LONG     0x1A
#define RX_DCS_LONG     0x1C
#define RX_DCS_SHORT_1  0x21
#define RX_DCS_SHORT_2  0x22
#define RX_ERRORS       0x3F3F          /* MIPI_DSI_ERR_BIT_MASK */

#define REG_ID          0x04
#define REG_MTP         0xC8
#define REG_GAMMA       0xCA
#define REG_AOR         0xB1

#define AOR_BRIGHTEST   0x000C
#define AOR_DIMMEST     0x0B76

static vm_offset_t dsim;
static lck_mtx_t *panel_lock;
static int panel_level = -1;
static volatile bool panel_asleep;      /* put to sleep by panel_display(): the next on wakes it first */
static int panel_wanted = -1;   /* the last level asked for (panel_set), sent or not: what a wake puts back */
static bool panel_send_level(int level, uint32_t *aor_out);
static int panel_set_said(int level, uint32_t aor, bool ok);
static uint32_t panel_failures, read_failures;

/* the gamma, once worked out from the MTP at start (read only after) */
static struct ha8_work gamma_work;
static uint8_t gamma_rows[HA8_ROWS][HA8_CA];
static const struct ha8_panel *gamma_panel;
static bool gamma_ready;

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(dsim + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(dsim + off) = v;
}

/* one packet, its payload (if any) and then its header: true once the DSIM says the header has gone out */
static bool
dsim_packet(uint32_t header, const uint8_t *payload, uint32_t len)
{
	uint64_t deadline = deadline_us(50000);

	while ((rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) {
		return false;
	}
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t word = 0;

		for (uint32_t j = 0; j < 4 && i + j < len; j++) {
			word |= (uint32_t)payload[i + j] << (8 * j);
		}
		wr(DSIM_PAYLOAD, word);
	}
	wr(DSIM_PKTHDR, header);
	for (deadline = deadline_us(50000); !(rd(DSIM_INTSRC) & SFR_PH_EMPTY) && !(rd(DSIM_FIFOCTRL) & EMPTY_PH) &&
	    mach_absolute_time() < deadline;) {
	}
	bool sent = (rd(DSIM_INTSRC) & SFR_PH_EMPTY) || (rd(DSIM_FIFOCTRL) & EMPTY_PH);

	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	return sent;
}

/* one DCS command: a byte alone short, anything longer long */
static bool
dsim_send(const uint8_t *data, uint32_t len)
{
	if (len == 1) {
		return dsim_packet(DCS_SHORT | (uint32_t)data[0] << 8, NULL, 0);
	}
	return dsim_packet(DCS_LONG | (len & 0xff) << 8 | (len >> 8) << 16, data, len);
}

/* the next word of the RX FIFO, waiting at most `us' for one */
static bool
rx_pop(uint32_t *word, uint64_t us)
{
	uint64_t deadline = deadline_us(us);

	while ((rd(DSIM_FIFOCTRL) & EMPTY_RX) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & EMPTY_RX) {
		return false;
	}
	*word = rd(DSIM_RXFIFO);
	return true;
}

/* a DCS read of `len' bytes (1-40) of register `reg' (dsim_read_data): true when exactly that many came */
static bool
dsim_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t word = 0, first = 0, got = 0, words = 0, stale = 0, intsrc, ack_error = 0;
	const char *why = NULL;
	uint64_t deadline;

	while (!(rd(DSIM_FIFOCTRL) & EMPTY_RX) && stale < RX_WORDS) {
		(void)rd(DSIM_RXFIFO);
		stale++;
	}
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (!dsim_packet(MAX_RETURN | len << 8, NULL, 0) ||
	    !dsim_packet(DCS_READ | (uint32_t)reg << 8 | PKTHDR_BTA, NULL, 0)) {
		why = "the request was not sent";
		goto out;
	}
	for (deadline = deadline_us(100000); !(rd(DSIM_INTSRC) & (RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT)) &&
	    mach_absolute_time() < deadline;) {
	}
	/* (an answer in the FIFO is read even without RX data done: whether INTSRC shows that bit while it is masked is
	 * for the phone to say) */
	/*
	 * (step 107: the panel's answer came as an acknowledge-and-error report first -- 0x02, errors 0x3080 -- and its data a
	 * moment later; the FIFO looked empty in between, the read gave up as "another length", and the data was left in the
	 * FIFO for the next read to take as its own: values shifted one read along. After a report, the data is waited for,
	 * and the report alone no longer fails a read that does get its data)
	 */
	while (why == NULL && words < RX_WORDS && got == 0 && rx_pop(&word, words == 0 || (first & 0xff) == RX_ACK_ERROR ? 1000 : 0)) {
		uint32_t size;

		first = words++ == 0 ? word : first;
		switch (word & 0xff) {
		case RX_ACK_ERROR:
			ack_error = (word >> 8) & 0xffff;
			break;
		case RX_EOT:
			break;
		case RX_DCS_SHORT_1:
		case RX_GEN_SHORT_1:
		case RX_DCS_SHORT_2:
		case RX_GEN_SHORT_2:
			size = ((word & 0xff) == RX_DCS_SHORT_1 || (word & 0xff) == RX_GEN_SHORT_1) ? 1 : 2;
			for (uint32_t i = 0; i < size && i < len; i++) {
				buf[i] = (uint8_t)(word >> (8 + 8 * i));
			}
			got = size;
			break;
		case RX_DCS_LONG:
		case RX_GEN_LONG:
			size = (word >> 8) & 0xffff;
			for (uint32_t i = 0; i < size && why == NULL; i += 4) {
				uint32_t data;

				if (words++ >= RX_WORDS || !rx_pop(&data, 1000)) {
					why = "a long answer cut short";
					break;
				}
				for (uint32_t j = 0; j < 4 && i + j < size && i + j < len; j++) {
					buf[i + j] = (uint8_t)(data >> (8 * j));
				}
			}
			got = size;
			break;
		default:
			why = "a packet of unknown type";
			break;
		}
	}
	if (why == NULL && got != len) {
		why = words == 0 ? "no answer" : ack_error != 0 ? "only the panel's error report" : "an answer of another length";
	}
	if (ack_error != 0 && why == NULL) {
		static unsigned said_ack;

		if (said_ack++ < 4) {
			say("s6e3ha8: read %02X: the panel's error report %04x came first; its data followed and was taken\n", reg, ack_error);
		}
	}
out:
	intsrc = rd(DSIM_INTSRC);
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (why != NULL && read_failures++ < 8) {
		say("s6e3ha8: read %02X (%u bytes): %s (got %u in %u words, first %08x; INTSRC %x, FIFOCTRL %x%s)\n", reg,
		    len, why, got, words, first, intsrc, rd(DSIM_FIFOCTRL), stale ? ", stale words read out first" : "");
	}
	return why == NULL;
}

/* a read, tried three times (Samsung's mipi_read tries five, resetting the DSIM in between: not done here) */
static bool
panel_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		if (dsim_read(reg, buf, len)) {
			return true;
		}
	}
	return false;
}

/* the level-1, -2 and -3 keys opened (and closed again in the other order), as Samsung's resource-init sequence does */
static bool
panel_keys(bool open)
{
	static const uint8_t on[3][3] = { { 0x9F, 0xA5, 0xA5 }, { 0xF0, 0x5A, 0x5A }, { 0xFC, 0x5A, 0x5A } };
	static const uint8_t off[3][3] = { { 0xFC, 0xA5, 0xA5 }, { 0xF0, 0xA5, 0xA5 }, { 0x9F, 0x5A, 0x5A } };
	bool ok = true;

	for (int i = 0; i < 3; i++) {
		ok = dsim_send(open ? on[i] : off[i], 3) && ok;         /* (all three closed, whatever happened) */
		if (open && !ok) {
			break;
		}
	}
	return ok;
}

static void
say_bytes(const char *what, const uint8_t *bytes, uint32_t len)
{
	char line[3 * 40 + 1];
	uint32_t at = 0;

	for (uint32_t i = 0; i < len && i < 40; i++) {
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, " %02X", bytes[i]);
	}
	line[at] = 0;
	say("s6e3ha8: %s:%s\n", what, line);
}

/* read the panel and work its gamma out (panel_gamma=1 or 2) */
static void
panel_gamma_init(uint32_t mode)
{
	uint8_t id[3] = { 0 }, mtp[2][HA8_CA], ca[HA8_CA], aor[2];
	bool have_id, keys, have_mtp = false, have_ca = false, have_aor = false, same = true, repeated = true;
	uint32_t panel_id, clamped, show_all = 0;
	const char *refused = NULL;
	int match = -1;

	lck_mtx_lock(panel_lock);
	have_id = panel_read(REG_ID, id, sizeof(id));
	keys = panel_keys(true);
	if (keys) {
		have_mtp = panel_read(REG_MTP, mtp[0], HA8_CA) && panel_read(REG_MTP, mtp[1], HA8_CA);
		have_ca = panel_read(REG_GAMMA, ca, HA8_CA);
		have_aor = panel_read(REG_AOR, aor, sizeof(aor));
	}
	keys = panel_keys(false) && keys;
	lck_mtx_unlock(panel_lock);

	panel_id = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
	gamma_panel = ha8_panel_of(panel_id);
	say("s6e3ha8: ID %06x%s: Samsung's tables for the %s panel; keys %s; DSIM CONFIG %x (per-frame read %s)\n",
	    panel_id, have_id ? "" : " (NOT READ)", gamma_panel->name, keys ? "opened and closed" : "NOT ALL SENT",
	    rd(DSIM_CONFIG), (rd(DSIM_CONFIG) & PER_FRAME_READ) ? "on" : "off");
	if (have_mtp) {
		say_bytes("MTP (C8)", mtp[0], HA8_CA);
		for (int i = 0; i < HA8_CA; i++) {
			same = same && mtp[0][i] == mtp[1][i];
			repeated = repeated && mtp[0][i] == mtp[0][0];
		}
		if (!same) {
			say_bytes("MTP read again", mtp[1], HA8_CA);
		}
		refused = !same ? "the two reads differ" : repeated ? "one byte 34 times" : NULL;
	} else {
		refused = keys ? "not read" : "not read (the keys were not sent)";
	}
	if (have_ca) {
		say_bytes("the boot loader's gamma (CA)", ca, HA8_CA);
	}
	if (have_aor) {
		say("s6e3ha8: the boot loader's AOR (B1): %02X %02X\n", aor[0], aor[1]);
	}
	if (refused != NULL) {
		say("s6e3ha8: no MTP (%s): brightness by the AOR alone\n", refused);
		return;
	}
	clamped = ha8_gamma(&gamma_work, gamma_panel, mtp[0], gamma_rows);
	for (int r = 0; r < HA8_ROWS && have_ca && match < 0; r++) {
		bool equal = true;

		for (int i = 0; i < HA8_CA; i++) {
			equal = equal && gamma_rows[r][i] == ca[i];
		}
		match = equal ? r : -1;
	}
	if (match >= 0) {
		say("s6e3ha8: gamma worked out for the 74 luminances (%u values kept within range); the boot loader's is "
		    "the row of %u nits: the calculation agrees with Samsung's\n", clamped,
		    (unsigned)gamma_panel->rows[match].nits);
	} else {
		say("s6e3ha8: gamma worked out for the 74 luminances (%u values kept within range); the boot loader's "
		    "is %s\n", clamped, have_ca ? "none of them" : "not known");
	}
	(void)PE_parse_boot_argn("panel_gamma_log", &show_all, sizeof(show_all));
	for (int r = 0; r < HA8_ROWS; r++) {
		unsigned nits = gamma_panel->rows[r].nits;
		char what[16];

		if (show_all || nits == 2 || nits == 100 || nits == 400) {
			snprintf(what, sizeof(what), "gamma[%3u] ", nits);      /* (Android's "gamma[%3d] : ") */
			say_bytes(what, gamma_rows[r], HA8_CA);
		}
	}
	gamma_ready = mode >= 2;
	say("s6e3ha8: brightness by %s\n", gamma_ready ? "gamma and AOR (Samsung's tables)" :
	    "the AOR alone (panel_gamma=1: worked out, not sent)");
}

/* brightness 0-255: Samsung's gamma row and AOR for the level, or (the fallback) the AOR alone, the light made to
 * follow the level's square (a dim end that is usable) */
static int
panel_set(int level)
{
	uint32_t aor;
	bool ok;

	if (dsim == 0 || level < 0 || level > 255) {
		return EINVAL;
	}
	lck_mtx_lock(panel_lock);
	panel_wanted = level;
	/*
	 * Not to a panel asleep: iOS lowers the level to 0 just after the screen goes off, which reached the panel after its
	 * sleep-in -- AOR and F7 03 (take the registers) written into a sleeping panel, at every screen-off of steps 89-99,
	 * and the woken panel showed black lines whatever the wake sent (none in step 94, where it never slept). Hypothesis
	 * S5. Kept as the level asked for; the wake sends it (panel_wanted).
	 */
	if (panel_asleep) {
		lck_mtx_unlock(panel_lock);
		{
			static int said;

			if (said++ < 4) {
				say("s6e3ha8: brightness %d asked for while the panel sleeps: kept for the wake, not sent\n", level);
			}
		}
		return 0;
	}
	ok = panel_send_level(level, &aor);
	if (ok) {
		panel_level = level;
	} else {
		panel_failures++;
	}
	lck_mtx_unlock(panel_lock);
	return panel_set_said(level, aor, ok);
}

/*
 * The level's packets, panel_lock held: gamma row and AOR, or (the fallback) the AOR alone, the light made to follow the
 * level's square (a dim end that is usable). Its AOR in *aor_out.
 */
static bool
panel_send_level(int level, uint32_t *aor_out)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, take[] = { 0xF7, 0x03 }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t gamma[1 + HA8_CA] = { REG_GAMMA };
	uint32_t aor;
	bool ok;

	if (gamma_ready) {
		int row = ha8_row_of_level(level);

		aor = gamma_panel->aor[level];
		for (int i = 0; i < HA8_CA; i++) {
			gamma[1 + i] = gamma_rows[row][i];
		}
	} else {
		uint32_t lit = (uint32_t)(level * level);

		/* (the dimmest is Samsung's level 0 of this panel's table once its ID is known: 0x0B72 on S1) */
		uint32_t dimmest = gamma_panel != NULL ? gamma_panel->aor[0] : AOR_DIMMEST;

		aor = dimmest - (dimmest - AOR_BRIGHTEST) * lit / (255 * 255);
	}
	uint8_t aor_packet[] = { REG_AOR, (uint8_t)(aor >> 8), (uint8_t)aor };

	ok = dsim_send(key_on, sizeof(key_on));
	ok = ok && (!gamma_ready || dsim_send(gamma, sizeof(gamma)));
	ok = ok && dsim_send(aor_packet, sizeof(aor_packet)) && dsim_send(take, sizeof(take));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;         /* (the key off whatever happened) */
	*aor_out = aor;
	return ok;
}

/* panel_set's line (not under the lock) */
static int
panel_set_said(int level, uint32_t aor, bool ok)
{
	if (!ok && panel_failures <= 3) {
		say("s6e3ha8: brightness %d (%s, AOR 0x%x) not sent: the DSIM's FIFO did not take it "
		    "(FIFOCTRL %x, INTSRC %x)\n", level, gamma_ready ? "gamma row and AOR" : "AOR alone", aor,
		    rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	} else if (ok) {
		/* each set that was sent, at most a line a second (a slider dragged sends many): what went out */
		static uint64_t said_at;
		static uint32_t unsaid;
		uint64_t now = mach_absolute_time(), second;

		nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
		if (said_at == 0 || now - said_at >= second) {
			say("s6e3ha8: brightness %d sent (%s, AOR 0x%04x; the DSIM took every packet; %u sets not logged "
			    "since the last line)\n", level, gamma_ready ? "gamma row and AOR" : "AOR alone", aor, unsaid);
			said_at = now;
			unsaid = 0;
		} else {
			unsaid++;
		}
	}
	return ok ? 0 : EIO;
}

static int
panel_sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_level, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	return panel_set(value);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_KERN,
    NULL, 0,
    panel_sysctl_brightness, "I", "the panel's brightness, 0-255 (-1: never set); writing sets it (Galaxy S9, panel=1)");

/*
 * kern.hakios.display: 0 the panel's output off (DCS 28), 1 on again (DCS 29), between Samsung's keys as for the display
 * off of blank() above -- and no sleep-in: a panel in command mode keeps what it shows in its own memory, so what 29
 * brings back needs no power-on sequence. For a screen that iOS has blanked (userland/src/inject_sb.c, panel_blank=1):
 * the brightness alone leaves the panel lit at its lowest level. -1 read: never set.
 */
static int panel_display_state = -1;

/*
 * Sleep-in with display-off, so that the screen is really off (28 alone left it faintly lit). The panel woke from it with
 * black bands top and bottom and stuttered (15 November): it does not keep what the boot loader set up. Samsung's own init
 * for this panel (drivers/video/fbdev/exynos/panel/s6e3ha8/s6e3ha8_crown_a3_s0_panel.h of the S9/Note9 kernel) sends, after
 * the sleep-out: the scaler, CASET and PASET of the mode (the panel's default rows are 2560 -- PASET 0x9FF -- where the
 * DECON sends 2960: the bands; and, the panel being in command mode, the rows outside the window it took kept their
 * picture from before the sleep: the ghosting), SET_AREA, TE on (the frames are paced by TE: the stutter), then the brightness. Those are
 * sent again here at every wake (panel_restore), for the mode the DECON drives, 1440x2960 (scaler off). On by default
 * again; kern.hakios.panel_sleep=0 goes back to 28/29 alone.
 */
static int panel_sleep = 1;     /* (step 94's A/B: off, no lines; on again in step 95 to count the link's errors with the lines) */

/*
 * Whether the panel is asleep -- from the sleep-in until its display-on after the wake. While it is, it sends no TE, so the
 * DECON takes no flip: MiniIOMobileFramebuffer's scanLatch must not take that for a broken scan-out. It did (step 89, 16
 * November: "a flip not taken in 100 ms" at the first blank after boot), gave the scaler path up for good and drew every
 * frame into the boot framebuffer by the processor from then on -- the ghosting and the stutter seen after every screen-off.
 */
bool hakios_panel_asleep(void);
extern void hakios_scanout_hold(int hold);    /* hakios.exynos_dpu.c (or the stub) */

bool
hakios_panel_asleep(void)
{
	return panel_asleep;
}

/*
 * The screen-off savings (the Wi-Fi poller's longer period, the Mali's cores off, the touch screen's and the CPU
 * governor's longer periods) ask hakios_dark_saving(), not hakios_panel_asleep(). dark_ab=1 (the Note9's default for
 * now): max77705's minute line flips them, a minute on, a minute off, while the screen is dark, and sums the current of
 * each half -- the same run, the same temperature, the same radios, for the comparison between rounds that round 25
 * could not make (its SoC 6 C warmer and Bluetooth on). dark_ab=0: always on.
 */
static int dark_ab = -1;
static volatile bool dark_save_off;
bool hakios_dark_saving(void);
bool hakios_dark_ab_off(void);
bool hakios_dark_ab_flip(void);

bool
hakios_dark_saving(void)
{
	return panel_asleep && !dark_save_off;
}

/* the screen dark, in dark_ab's half without the savings */
bool
hakios_dark_ab_off(void)
{
	return panel_asleep && dark_save_off;
}

/* max77705's minute: the other half from now; true when dark_ab is on */
bool
hakios_dark_ab_flip(void)
{
	if (dark_ab < 0) {
		char platform[24] = "";
		uint32_t value = 0;

		(void)PE_parse_boot_arg_str("loader_platform", platform, sizeof(platform));
		if (PE_parse_boot_argn("dark_ab", &value, sizeof(value))) {
			dark_ab = value != 0;
		} else {
			dark_ab = strcmp(platform, "crownlte") == 0;
		}
	}
	if (dark_ab) {
		dark_save_off = !dark_save_off;
	}
	return dark_ab != 0;
}
SYSCTL_INT(_kern_hakios, OID_AUTO, panel_sleep, CTLFLAG_RW | CTLFLAG_LOCKED, &panel_sleep, 0, "display-off also sleeps the panel");

/*
 * Before the sleep-out, panel_lock held: the compression (DSC) the panel decodes the DECON's stream with -- MIPI's
 * Compression Mode (data type 0x07: on) and the Picture Parameter Set (0x0A, 128 bytes) -- as Samsung's init sends them
 * first (level-1 key, DSC, PPS, key off, then SLEEP_OUT). Without them the woken panel showed black lines and smudges
 * (step 91, 16 November): it decoded with what it had after the sleep. The PPS is crown_a3_s0_pps_table's row for
 * 1440x2960 ("DSU MODE 1 ... Slice Info : 720x40"; bytes 6-9: 2960 rows, 1440 columns), the mode the DECON drives.
 */
static bool
panel_dsc(void)
{
	static const uint8_t l1_on[] = { 0x9F, 0xA5, 0xA5 }, l1_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t pps[128] = {
		0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0B, 0x90, 0x05, 0xA0, 0x00, 0x28, 0x02, 0xD0, 0x02, 0xD0,
		0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x04, 0x6C, 0x00, 0x0A, 0x00, 0x0C, 0x02, 0x77, 0x01, 0xE9,
		0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20, 0x00, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38,
		0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
		0x09, 0xBE, 0x19, 0xFC, 0x19, 0xFA, 0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A, 0xF6,
		0x2B, 0x34, 0x2B, 0x74, 0x3B, 0x74, 0x6B, 0xF4,
	};
	bool ok;

	ok = dsim_send(l1_on, sizeof(l1_on));
	ok = ok && dsim_packet(0x07 | 0x01u << 8, NULL, 0);                                 /* Compression Mode: DSC on */
	ok = ok && dsim_packet(0x0A | (uint32_t)(sizeof(pps) & 0xff) << 8 | (uint32_t)(sizeof(pps) >> 8) << 16, pps, sizeof(pps));
	ok = dsim_send(l1_off, sizeof(l1_off)) && ok;
	return ok;
}

/*
 * What the boot loader set in the panel, read before its first sleep and written back at every wake: the registers of
 * Samsung's init that are not sent here otherwise, whose values depend on the link (FFC: the panel's receiver for this
 * DSI clock) or on the panel (ISC, edge dimming, the error flag's mode, the touch sync, the scaler). The woken panel
 * showed black horizontal lines and filled gaps between them, pixels fully off (step 92, 16 November), with DSC, PPS,
 * CASET/PASET and TE all sent again: something else of the boot loader's was gone. Reading them is also how their
 * values here are known at all (they are logged next to Samsung's defaults for crown_a3_s0).
 */
static struct panel_reg {
	uint8_t reg, len;               /* the register and its parameters' bytes (Samsung's arrays less the opcode) */
	const char *what;
	uint8_t val[96];
	bool have;
} panel_regs[] = {
	{ 0xC5, 5, "FFC (Samsung C5 0D 10 B4 3E 01)", { 0 }, false },
	{ 0xF6, 1, "ISC (F6 43)", { 0 }, false },
	{ 0xC2, 6, "edge dimming (C2 09 00 D8 D8 F4 F4)", { 0 }, false },
	{ 0xED, 1, "ERR_FG (ED 4C)", { 0 }, false },
	{ 0xB9, 9, "TSP_HSYNC (B9 01 B0 91 09 00 00 00 11 03)", { 0 }, false },
	{ 0xBA, 5, "scaler (BA 01 26 08 08 F3)", { 0 }, false },
	/*
	 * And Samsung's brightness sequence (PANEL_SET_BL_SEQ of crown_a3_s0: gamma, AOR, TSET/MPS/ELVSS, ELVSS temperature,
	 * VGH/VINT, ACL control and dim frames, ACL on/off, IRC value and on, POC compensation, then F7 03), of which only
	 * the AOR was sent again here (panel_gamma=0). Read long enough to take the parameters Samsung writes at an offset
	 * (ACL dim frames at 0x0F of B4, POC at 0x06 of B1 and 0x53 of B8, ELVSS and IRC at offsets of B5 and B8): step 96's
	 * black lines stayed with the mode, DSC, TE and the link all good, the panel's emission settings being what is left.
	 */
	{ 0xCA, 37, "gamma (CA 07 80 80 ...)", { 0 }, false },
	{ 0xB1, 16, "AOR and POC (B1 00 0C, POC at 06)", { 0 }, false },
	{ 0xB5, 32, "TSET/MPS/ELVSS (B5 19 DC 0A, temperature further on)", { 0 }, false },
	{ 0xF4, 8, "VGH/VINT (F4 EB 23)", { 0 }, false },
	{ 0xB4, 16, "ACL control and dim frames (B4 00 44 80 65 26 00, 20 at 0F)", { 0 }, false },
	{ 0x55, 1, "ACL on/off (55 00)", { 0 }, false },
	{ 0xB8, 88, "IRC and POC (B8 20 69 E7 ..., 0F FF at 53)", { 0 }, false },
};
static bool panel_regs_taken;

/*
 * kern.hakios.panel_restore: what a wake sends besides sleep-out, 29 and the brightness. 0 (the default since step 99):
 * nothing else. Steps 89-98 sent DSC/PPS, the mode, SET_AREA, TE and the boot loader's registers -- on the guess that the
 * panel had lost them, from the black bands of a wake in step 76; those bands were the framebuffer falling back to the boot
 * window (the scan-out dropped, fixed in step 91), and the black lines stayed whatever was sent. Hypothesis S4: something
 * sent there makes them. 1 sends it all again, as in step 98.
 */
static int panel_restore_all;

/*
 * kern.hakios.panel_reset (1, the default since step 101): the panel held in reset while it sleeps, as Samsung's suspend
 * leaves it (lcd-reset low, then its regulators off -- those, on the S2DOS05 at 0x60 of hsi2c@14350000, are left on
 * here), and brought out of it at the wake with the whole of Samsung's init sent again. Every wake that only sent things
 * to a panel slept-in and slept-out (steps 89-100) left black horizontal lines; one never put to sleep (step 94) had none:
 * this panel is not to be woken from a sleep-in without a reset. The pin: crown26.dts's panel_drv gpio,lcd-reset =
 * <gpa3 4 active-low>, gpa3 of ALIVE's GPIO at 0x14050000 + 0x080 (as hakios.w9018.c reads its pins there).
 */
static int panel_reset_mode = 1;      /* (off in step 102 after step 101's black wake; on again in step 103 with the DSIM's lanes
                                         in LP-11 over the reset, disp_det watched, retries, and the loader if all fail) */
static bool panel_was_reset;
static vm_offset_t alive_gpio;
SYSCTL_INT(_kern_hakios, OID_AUTO, panel_reset, CTLFLAG_RW | CTLFLAG_LOCKED, &panel_reset_mode, 0,
    "the panel held in reset while asleep and fully set up again at the wake (1), or slept-in and out only (0)");

#define ALIVE_GPIO_PA   0x14050000UL
#define GPA3_CON        0x080
#define GPA3_DAT        0x084
#define LCD_RESET_PIN   4

/* the reset line: high, the panel running; low, held in reset. Its pin made an output first if it is not one. */
static bool
panel_reset_line(int high)
{
	volatile uint32_t *con, *dat;
	uint32_t c0, d0;
	static int said;

	if (alive_gpio == 0) {
		alive_gpio = ml_io_map(ALIVE_GPIO_PA, 0x1000);
	}
	if (alive_gpio == 0) {
		return false;
	}
	con = (volatile uint32_t *)(alive_gpio + GPA3_CON);
	dat = (volatile uint32_t *)(alive_gpio + GPA3_DAT);
	c0 = *con;
	d0 = *dat;
	if (((c0 >> (4 * LCD_RESET_PIN)) & 0xf) != 1) {
		*con = (c0 & ~(0xfu << (4 * LCD_RESET_PIN))) | (1u << (4 * LCD_RESET_PIN));
	}
	*dat = high ? (d0 | (1u << LCD_RESET_PIN)) : (d0 & ~(1u << LCD_RESET_PIN));
	__asm__ volatile ("dsb sy" ::: "memory");
	if (said++ < 6) {
		say("s6e3ha8: the panel's reset line (gpa3-4) %s: CON %08x -> %08x, DAT %02x -> %02x\n", high ? "high (running)" :
		    "low (in reset)", c0, *con, d0 & 0xff, *dat & 0xff);
	}
	return true;
}
/*
 * Step 101's wake out of the reset left the panel black: the reset line went high with the DSIM running as ever, its
 * clock lane in HS. Samsung's (dsim_drv.c of the 9810 kernel, _dsim_enable, which TWRP's kernel runs at every unblank)
 * brings the panel out of reset with the D-PHY held in reset and then sets the link up again, so that the lanes are in
 * LP-11 (stop state) when the panel's receiver starts, and only then sends the init; its suspend (_dsim_disable) takes
 * the clock lane out of HS (dsim_reg_stop) before the reset line goes low. Here the clock lane is taken to LP and its
 * stop state waited for around the reset, and back to HS before the init. DSIM_CLK_CTRL / DSIM_DPHY_STATUS: regs-dsim.h.
 */
#define DSIM_DPHY_STATUS        0x1C
#define  TX_READY_HS_CLK        (1u << 10)
#define  STOP_STATE_CLK         (1u << 8)
#define  STOP_STATE_DAT         0xFu
#define DSIM_CLK_CTRL           0x20
#define  NONCONT_CLOCK_LANE     (1u << 25)
#define  TX_REQUEST_HSCLK       (1u << 20)

/* the clock lane in HS (on) or back in LP with every lane in its stop state (off): true when the D-PHY says so in 5 ms */
static bool
dsim_hs_clock(bool on)
{
	uint32_t c = rd(DSIM_CLK_CTRL);
	uint64_t deadline = deadline_us(5000);

	if (on) {
		wr(DSIM_CLK_CTRL, c | TX_REQUEST_HSCLK);
		if (c & NONCONT_CLOCK_LANE) {
			return true;
		}
		while (!(rd(DSIM_DPHY_STATUS) & TX_READY_HS_CLK) && mach_absolute_time() < deadline) {
		}
		return (rd(DSIM_DPHY_STATUS) & TX_READY_HS_CLK) != 0;
	}
	wr(DSIM_CLK_CTRL, c & ~TX_REQUEST_HSCLK);
	while ((rd(DSIM_DPHY_STATUS) & (STOP_STATE_CLK | STOP_STATE_DAT)) != (STOP_STATE_CLK | STOP_STATE_DAT) &&
	    mach_absolute_time() < deadline) {
	}
	return (rd(DSIM_DPHY_STATUS) & STOP_STATE_CLK) != 0;
}

/*
 * The panel's disp_det: high while it runs (crown26.dts panel_drv gpio,disp-det = <gpg2 2>, gpg2 the seventh bank of
 * pinctrl@10430000: CON 0x0C0, DAT 0x0C4). Samsung's __panel_seq_init waits up to 200 x 100 us after its init for it to
 * go high and, if it does not, powers the panel off, waits 100 ms and tries again (panel_sleep_out, up to 30 times).
 * -1: not mapped. disp_det_on: what it read with the panel on, before its first sleep (only a 1 there makes it a test).
 */
#define DISP_DET_GPIO_PA        0x10430000UL
#define GPG2_CON                0x0C0
#define GPG2_DAT                0x0C4
#define DISP_DET_PIN            2

static vm_offset_t disp_det_gpio;
static int disp_det_on = -1;

static int
panel_disp_det(void)
{
	static bool said;

	if (disp_det_gpio == 0) {
		disp_det_gpio = ml_io_map(DISP_DET_GPIO_PA, 0x1000);
	}
	if (disp_det_gpio == 0) {
		return -1;
	}
	uint32_t con = *(volatile uint32_t *)(disp_det_gpio + GPG2_CON), dat = *(volatile uint32_t *)(disp_det_gpio + GPG2_DAT);

	if (!said) {
		said = true;
		say("s6e3ha8: disp_det (gpg2-2): CON %08x (function %x), DAT %02x\n", con, (con >> (4 * DISP_DET_PIN)) & 0xf,
		    dat & 0xff);
	}
	return (int)((dat >> DISP_DET_PIN) & 1);
}

/* panel_lock held (let go over the waits): out of the reset, the lanes in LP-11 as it starts. True when it answers 0A. */
static bool
panel_out_of_reset(int attempt)
{
	uint32_t cc0 = rd(DSIM_CLK_CTRL), st0 = rd(DSIM_DPHY_STATUS), st1;
	bool lp = dsim_hs_clock(false), hs;
	int det0 = panel_disp_det(), det1;
	uint8_t pm = 0xff;
	bool answered;

	st1 = rd(DSIM_DPHY_STATUS);
	(void)panel_reset_line(1);
	lck_mtx_unlock(panel_lock);
	delay(10 * 1000);               /* (Samsung: the reset high, 5 ms, then its init's own 5 ms) */
	lck_mtx_lock(panel_lock);
	det1 = panel_disp_det();
	hs = dsim_hs_clock(true);
	lck_mtx_unlock(panel_lock);
	delay(5 * 1000);
	lck_mtx_lock(panel_lock);
	answered = panel_read(0x0A, &pm, 1);
	say("s6e3ha8: wake %d out of reset: CLK_CTRL %08x, DPHY_STATUS %08x -> LP %s (%08x); disp_det in reset %d, 10 ms out %d; "
	    "HS %s (%08x); 0A %s%02x\n", attempt, cc0, st0, lp ? "ok" : "NOT IN STOP STATE", st1, det0, det1, hs ? "ok" : "NOT READY",
	    rd(DSIM_DPHY_STATUS), answered ? "" : "(NO ANSWER) ", pm);
	return answered;
}

/* the run handed back to the loader -- as the five fingers do -- when the panel did not come back: the log kept */
extern void loader_return(void);
extern void hakios_var_save_on_return(void);

static void
panel_give_up(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	delay(3 * 1000 * 1000);
	say("\n--- s6e3ha8: the panel did not come back out of its reset: back to the loader (the log kept) ---\n");
	hakios_var_save_on_return();
	loader_return();
}

SYSCTL_INT(_kern_hakios, OID_AUTO, panel_restore, CTLFLAG_RW | CTLFLAG_LOCKED, &panel_restore_all, 0,
    "a wake sends the mode, DSC/PPS, TE and the boot loader's registers again (1) or only sleep-out and 29 (0)");

/* panel_lock held, the panel awake and as the boot loader left it */
static void
panel_regs_take(void)
{
	char line[300];

	panel_regs_taken = true;
	if (!panel_keys(true)) {
		(void)panel_keys(false);
		say("s6e3ha8: the boot loader's registers not read (the keys not opened)\n");
		return;
	}
	for (size_t i = 0; i < sizeof(panel_regs) / sizeof(panel_regs[0]); i++) {
		struct panel_reg *r = &panel_regs[i];
		size_t at = 0;

		/*
		 * Read twice, kept only when both agree (step 106: twice the screen stayed black after the Apple logo, with the
		 * panel taken out of reset at the boot's first screen-off and these values written into it; reads made while
		 * frames went out came back of the wrong length -- a value read wrong and written back would leave the panel
		 * "up" and dark, which nothing here would notice)
		 */
		uint8_t again[sizeof(r->val)];
		bool first = panel_read(r->reg, r->val, r->len), second = first && panel_read(r->reg, again, r->len);

		r->have = first && second && memcmp(r->val, again, r->len) == 0;
		if (first && second && !r->have) {
			say("s6e3ha8: the boot loader's %02X read twice, two answers: NOT written back\n", r->reg);
		}
		for (uint32_t k = 0; r->have && k < r->len && at + 4 < sizeof(line); k++) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %02X", r->val[k]);
		}
		say("s6e3ha8: the boot loader's %02X, %s:%s\n", r->reg, r->what, r->have ? line : " NOT READ (not written back)");
	}
	(void)panel_keys(false);
}

/* panel_lock held, after a sleep-out: what panel_regs_take read, written back */
static bool
panel_regs_put(void)
{
	bool ok;

	if (!panel_regs_taken) {
		return true;
	}
	ok = panel_keys(true);
	for (size_t i = 0; ok && i < sizeof(panel_regs) / sizeof(panel_regs[0]); i++) {
		uint8_t buf[1 + sizeof(panel_regs[0].val)];

		if (!panel_regs[i].have) {
			continue;
		}
		buf[0] = panel_regs[i].reg;
		memcpy(buf + 1, panel_regs[i].val, panel_regs[i].len);
		ok = dsim_send(buf, 1u + panel_regs[i].len) && ok;
	}
	{
		static const uint8_t update[] = { 0xF7, 0x03 };    /* (the brightness registers taken: Samsung's sequence ends so) */

		ok = dsim_send(update, sizeof(update)) && ok;
	}
	/* what the panel holds now, read back (no frame going out yet at a wake): a register that differs is logged */
	{
		unsigned same = 0, differ = 0, unread = 0;

		for (size_t i = 0; i < sizeof(panel_regs) / sizeof(panel_regs[0]); i++) {
			uint8_t now[sizeof(panel_regs[0].val)];

			if (!panel_regs[i].have) {
				continue;
			}
			if (!panel_read(panel_regs[i].reg, now, panel_regs[i].len)) {
				unread++;
			} else if (memcmp(now, panel_regs[i].val, panel_regs[i].len) == 0) {
				same++;
			} else {
				differ++;
				say("s6e3ha8: written back %02X, read back otherwise: %02X %02X %02X %02X (written %02X %02X %02X %02X)\n",
				    panel_regs[i].reg, now[0], now[1], now[2], now[3], panel_regs[i].val[0], panel_regs[i].val[1],
				    panel_regs[i].val[2], panel_regs[i].val[3]);
			}
		}
		say("s6e3ha8: the boot loader's registers read back: %u as written, %u otherwise, %u not read\n", same, differ, unread);
	}
	ok = panel_keys(false) && ok;
	{
		char line[300];
		size_t at = 0;

		for (size_t i = 0; i < sizeof(panel_regs) / sizeof(panel_regs[0]) && at + 40 < sizeof(line); i++) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %02X:", panel_regs[i].reg);
			for (uint32_t k = 0; k < panel_regs[i].len && k < 6 && at + 4 < sizeof(line); k++) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, panel_regs[i].have ? "%02X" : "--", panel_regs[i].val[k]);
			}
		}
		say("s6e3ha8: the boot loader's registers written back:%s\n", line);
	}
	return ok;
}

/* after a sleep-out, panel_lock held: what Samsung's init sets after its own sleep-out, for 1440x2960 */
static bool
panel_restore(void)
{
	static const uint8_t l1_on[] = { 0x9F, 0xA5, 0xA5 }, l1_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t l2_on[] = { 0xF0, 0x5A, 0x5A }, l2_off[] = { 0xF0, 0xA5, 0xA5 };
	static const uint8_t scaler[] = { 0xBA, 0x01, 0x26, 0x08, 0x08, 0xF3 };         /* 1440x2960: scaler off */
	static const uint8_t caset[] = { 0x2A, 0x00, 0x00, 0x05, 0x9F };               /* columns 0..1439 */
	static const uint8_t paset[] = { 0x2B, 0x00, 0x00, 0x0B, 0x8F };               /* rows 0..2959 */
	static const uint8_t set_area[] = { 0x1A, 0x1F, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t te_on[] = { 0x35, 0x00 };
	bool ok;

	ok = dsim_send(l2_on, sizeof(l2_on)) && dsim_send(scaler, sizeof(scaler));
	ok = dsim_send(l2_off, sizeof(l2_off)) && ok;
	ok = dsim_send(caset, sizeof(caset)) && dsim_send(paset, sizeof(paset)) && ok;
	ok = dsim_send(l1_on, sizeof(l1_on)) && dsim_send(set_area, sizeof(set_area)) && dsim_send(te_on, sizeof(te_on)) && ok;
	ok = dsim_send(l1_off, sizeof(l1_off)) && ok;
	return ok;
}

static int
panel_display(int on)
{
	static const uint8_t key_on[] = { 0x9F, 0xA5, 0xA5 }, key_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t display_off[] = { 0x28 }, display_on[] = { 0x29 };
	bool ok, shown = false;

	uint8_t before = 0xff, after = 0xff, mode_before = 0xff, mode_after = 0xff;
	bool read_before, read_after;

	if (dsim == 0 || panel_lock == NULL) {
		return ENODEV;
	}
	lck_mtx_lock(panel_lock);
	/*
	 * Off again while already asleep (step 108, 18 November: backboardd crashed at boot just after iOS's first screen-off,
	 * and its successor turned the screen off a second time): the panel is in reset, its clock lane in LP. That second
	 * screen-off sent 28 and 10 to a panel that cannot take them, filled the DSIM's FIFO, and left "asleep" false with the
	 * panel still in reset -- so the screen-on that followed sent 29 alone and the panel was never brought back: black
	 * after the Apple logo, for good. Already off is off: nothing is sent.
	 */
	if (!on && panel_asleep) {
		lck_mtx_unlock(panel_lock);
		panel_display_state = 0;
		say("s6e3ha8: display off asked for while the panel already sleeps%s: nothing sent\n", panel_was_reset ? " in reset" : "");
		return 0;
	}
	/* what the panel says of itself, before and 30 ms after: DCS 0A (power mode: bit 2 display on, bit 3 normal mode,
	   bit 4 sleep out) and 0D (display mode): whether 28 changed anything there */
	read_before = !(on && (panel_was_reset || panel_asleep)) && panel_read(0x0A, &before, 1) && panel_read(0x0D, &mode_before, 1);
	/*
	 * DCS 05 (get_error_count_on_DSI: bit 7 overflow, bits 6-0 the count of DSI errors the panel has seen since it was last
	 * read): before this change (the errors over the time the screen was on) and after it. With sleep-in the woken panel
	 * shows black lines (steps 89-93), without it none (step 94): whether the link itself fails after a wake.
	 */
	uint8_t dsi_errors_before = 0xff, dsi_errors_after = 0xff;
	bool errors_read_before = !(on && (panel_was_reset || panel_asleep)) && panel_read(0x05, &dsi_errors_before, 1);
	/*
	 * Display-off alone (28) left the panel faintly lit (14-15 November: the screen grey-black, not off): its circuits
	 * stay up. Off is now 28 then sleep-in (10), as Samsung's suspend sends them; on is sleep-out (11), its 120 ms, what
	 * the panel lost in the sleep (panel_restore: it does NOT keep its mode and TE, as was first assumed), then 29.
	 */
	if (on && panel_asleep) {
		static const uint8_t sleep_out[] = { 0x11 };
		/* (the DECON's frames held while the panel is woken and set up: S2 of step 95; let go after 29) */
		lck_mtx_unlock(panel_lock);
		hakios_scanout_hold(1);
		lck_mtx_lock(panel_lock);
		bool full = panel_restore_all || panel_was_reset, was_reset = panel_was_reset, alive = true;
		bool dsc = true, restored = true, put = true;

		/* (out of the reset, Samsung's way, up to three times: back into it for 100 ms when the panel did not come up) */
		for (int attempt = 1; ; attempt++) {
		bool answered = was_reset ? panel_out_of_reset(attempt) : true;

		dsc = full ? panel_dsc() : true;

		/* (the sleep-out whatever became of the DSC packets: a panel left asleep is worse than one decoding badly) */
		ok = dsim_send(key_on, sizeof(key_on)) && dsim_send(sleep_out, 1);
		ok = dsc && ok;
		ok = dsim_send(key_off, sizeof(key_off)) && ok;
		lck_mtx_unlock(panel_lock);
		delay(120 * 1000);              /* the sleep-out's own wait before the next command */
		lck_mtx_lock(panel_lock);
		restored = full ? panel_restore() : true;
		put = full ? panel_regs_put() : true;      /* (after panel_restore: the boot loader's scaler wins) */
		if (!was_reset) {
			break;
		}
		{
			int det = panel_disp_det();
			uint8_t pm = 0xff;

			for (int i = 0; i < 200 && det == 0; i++) {     /* Samsung's 200 x 100 us */
				delay(100);
				det = panel_disp_det();
			}
			bool read = panel_read(0x0A, &pm, 1);

			alive = answered && read && (pm & 0x10) && (disp_det_on != 1 || det == 1);
			say("s6e3ha8: wake %d after the init: disp_det %d (on before the sleep: %d), 0A %s%02x (sleep-out bit %s): %s\n",
			    attempt, det, disp_det_on, read ? "" : "(NO ANSWER) ", pm, (pm & 0x10) ? "set" : "NOT SET",
			    alive ? "the panel is up" : attempt < 3 ? "NOT UP: back into reset for 100 ms" : "NOT UP, three times");
		}
		if (alive || attempt >= 3) {
			break;
		}
		(void)dsim_hs_clock(false);
		(void)panel_reset_line(0);
		lck_mtx_unlock(panel_lock);
		delay(100 * 1000);
		lck_mtx_lock(panel_lock);
		}

		ok = restored && ok;
		/* the brightness again (panel_set takes the lock itself) */
		/*
		 * The level asked for LAST, sent here under the lock. It was a copy taken before the wake, sent with the lock let go:
		 * iOS's own level for the screen coming back (89) got in meanwhile and the stale 0 of the screen-off went out over
		 * it -- the panel at its dimmest AOR (0x0B76) for good, and that duty showed as black horizontal lines (steps 89-97;
		 * none in step 94, without the wake, where 89 stayed).
		 */
		if (panel_wanted >= 0) {
			uint32_t aor;

			if (panel_send_level(panel_wanted, &aor)) {
				panel_level = panel_wanted;
			}
		}
		delay(20 * 1000);               /* a frame with the settings in before the output goes on */
		shown = dsim_send(key_on, sizeof(key_on)) && dsim_send(display_on, 1);
		ok = shown && ok;
		ok = dsim_send(key_off, sizeof(key_off)) && ok;
		hakios_scanout_hold(0);
		panel_asleep = false;
		panel_was_reset = false;
		if (!alive) {
			static thread_call_t give_up;

			panel_reset_mode = 0;   /* (the next sleeps without the reset, should the loader not take over) */
			if (give_up == NULL) {
				give_up = thread_call_allocate(panel_give_up, NULL);
				if (give_up != NULL) {
					(void)thread_call_enter(give_up);
				}
			}
		}
		say("s6e3ha8: woken (%s): DSC and PPS %s; scaler, CASET/PASET 1440x2960, SET_AREA, TE on %s; the boot loader's registers %s; "
		    "brightness %d again (the last asked for)\n", was_reset ? (alive ? "out of reset: all set up again" :
		    "out of reset: NOT UP, to the loader in 3 s") : full ? "all restored" : "sleep-out and 29 only: nothing below sent",
		    dsc ? "sent" : "NOT SENT", restored ? "sent" : "NOT ALL SENT",
		    !panel_regs_taken ? "not read" : put ? "written back" : "NOT ALL WRITTEN", panel_wanted);
	} else if (!on && panel_sleep) {
		static const uint8_t sleep_in[] = { 0x10 };

		/*
		 * (no frame in flight while it is turned off and put to sleep; held until the wake has set it up again. "Asleep"
		 * from now on, before the hold: the framebuffer must not take a flip the held DECON does not take for a broken
		 * scan-out -- the step 89 drop)
		 */
		panel_asleep = true;
		lck_mtx_unlock(panel_lock);
		hakios_scanout_hold(1);
		if (!panel_regs_taken) {
			delay(20 * 1000);       /* (the frame in flight out, before anything is read) */
		}
		lck_mtx_lock(panel_lock);
		if (!panel_regs_taken) {
			/* (the first sleep of the run: the panel still as the boot loader set it up; read with no frame going out) */
			panel_regs_take();
			disp_det_on = panel_disp_det();
			say("s6e3ha8: disp_det with the panel on: %d\n", disp_det_on);
		}

		ok = dsim_send(key_on, sizeof(key_on)) && dsim_send(display_off, 1);
		ok = dsim_send(key_off, sizeof(key_off)) && ok;
		lck_mtx_unlock(panel_lock);
		delay(20 * 1000);               /* a frame and more at 60 Hz */
		lck_mtx_lock(panel_lock);
		ok = dsim_send(key_on, sizeof(key_on)) && dsim_send(sleep_in, 1) && ok;
		ok = dsim_send(key_off, sizeof(key_off)) && ok;
		panel_asleep = ok;
		if (!ok) {
			hakios_scanout_hold(0);         /* (not asleep after all: its frames again) */
		} else if (panel_reset_mode && panel_regs_taken) {
			/* (Samsung waits after its sleep-in before the reset line goes low; the boot loader's registers are known, to
			 * set the panel up again from nothing) */
			lck_mtx_unlock(panel_lock);
			delay(100 * 1000);
			lck_mtx_lock(panel_lock);
			bool lp = dsim_hs_clock(false);     /* (Samsung's dsim_reg_stop: HS off before the reset goes low) */

			panel_was_reset = panel_reset_line(0);
			lck_mtx_unlock(panel_lock);
			delay(5 * 1000);
			lck_mtx_lock(panel_lock);
			say("s6e3ha8: asleep in reset: clock lane to LP %s (DPHY_STATUS %08x), disp_det %d\n", lp ? "ok" : "NOT IN STOP STATE",
			    rd(DSIM_DPHY_STATUS), panel_disp_det());
		}
	} else {
		/* 28 or 29 alone (the default: a panel woken from sleep-in came back with black bands top and bottom, 15 Nov) */
		ok = dsim_send(key_on, sizeof(key_on)) && dsim_send(on ? display_on : display_off, 1);
		ok = dsim_send(key_off, sizeof(key_off)) && ok;
	}
	/*
	 * The state the touch screen's gate reads (hakios_panel_touch_blocked): on as soon as 29 went out, whatever else of
	 * the wake did not -- one packet of the restore failing left it "off" and every touch was dropped until the next
	 * screen-off (step 92: touch dead after a wake, the five-finger exit too).
	 */
	if (ok || (on && shown)) {
		panel_display_state = on ? 1 : 0;
	}
	delay(30 * 1000);
	/* (not asked of a panel just put in reset: it cannot answer, and the three tries of each read cost 0.6 s, step 103) */
	bool in_reset = !on && panel_was_reset;

	read_after = !in_reset && panel_read(0x0A, &after, 1) && panel_read(0x0D, &mode_after, 1);
	bool errors_read_after = !in_reset && panel_read(0x05, &dsi_errors_after, 1);
	lck_mtx_unlock(panel_lock);
	say("s6e3ha8: the panel's DSI errors (DCS 05): before the %s %s%u%s, 30 ms after %s%u%s\n", on ? "wake" : "screen-off",
	    errors_read_before ? "" : "(NOT READ) ", dsi_errors_before & 0x7f, (dsi_errors_before & 0x80) && errors_read_before ? "+" : "",
	    errors_read_after ? "" : "(NOT READ) ", dsi_errors_after & 0x7f, (dsi_errors_after & 0x80) && errors_read_after ? "+" : "");
	say("s6e3ha8: display %s (%s) (FIFOCTRL %x, INTSRC %x); power mode 0A/0D before %s%02x/%02x, 30 ms after %s%02x/%02x\n",
	    on ? "on" : "off", ok ? "sent" : "NOT SENT", rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC),
	    read_before ? "" : "(NOT READ) ", before, mode_before, read_after ? "" : "(NOT READ) ", after, mode_after);
	return ok ? 0 : EIO;
}

/*
 * Upstream's entry for the screen's power (Scion's S9 panel_sleep: the framebuffer or user space calls it at iOS's
 * screen-off and screen-on). On this driver it is the Note9's own sequence -- sleep-in with the panel held in reset, and
 * Samsung's whole bring-up at the wake (panel_display) -- not a bare sleep-in/out, which leaves this panel with black lines
 * (steps 89-100). ENODEV without panel_sleep, as upstream's.
 */
int hakios_s6e3ha8_power(bool on);

int
hakios_s6e3ha8_power(bool on)
{
	return panel_sleep ? panel_display(on ? 1 : 0) : ENODEV;
}

/*
 * Boot-arg touch_blank=1: while the panel's output is off (kern.hakios.display=0, userland/src/inject_sb.c's
 * panel_blank=1), the touch screen's new touches are not passed on (hakios.s6sy761.c asks before each) -- a screen that
 * iOS has blanked is not woken by a touch (a pocket, a finger on the way past); the side button still does it. A finger
 * already down when the screen went dark is passed on to its lifting, so that iOS is not left with a finger that
 * never leaves. (Not in hakios.input.c: patch 1203 is made against that file.)
 * type: 1 down, 2 move, 3 up (sys/hakios.input.h). True: not to be passed on.
 */
bool hakios_panel_touch_blocked(uint32_t type, uint32_t finger);

bool
hakios_panel_touch_blocked(uint32_t type, uint32_t finger)
{
	static int gate = -1;
	static uint32_t down;                   /* fingers passed on and not yet lifted (bit = finger) */
	uint32_t on = 0, bit = finger < 32 ? 1u << finger : 0;
	bool dark;

	if (gate < 0) {
		char platform[24] = "";

		/* on by default on the Note9 (its boot line has no room for touch_blank=1); touch_blank=0 turns it off */
		(void)PE_parse_boot_arg_str("loader_platform", platform, sizeof(platform));
		if (PE_parse_boot_argn("touch_blank", &on, sizeof(on))) {
			gate = on != 0;
		} else {
			gate = strcmp(platform, "crownlte") == 0;
		}
	}
	dark = gate && panel_display_state == 0;
	if (type == 1) {                        /* a finger lands: passed on unless the screen is dark */
		if (!dark) {
			down |= bit;
		}
		return dark;
	}
	if (type == 3) {                        /* a finger lifts: passed on if its landing was */
		bool known = (down & bit) != 0;

		down &= ~bit;
		return dark && !known;
	}
	return !(down & bit) && dark;           /* a move: passed on for a finger that was */
}

static int
panel_sysctl_display(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_display_state, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	return value == 0 || value == 1 ? panel_display(value) : EINVAL;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, display, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_KERN,
    NULL, 0,
    panel_sysctl_display, "I", "the panel's output: 0 off, 1 on (-1: never set); writing sets it (panel=1)");


/*
 * The panel dark, for a halt that powers the phone off (drivers/power/hakios.exynos_poweroff.c, halt_off=N): Samsung's
 * display-off sequence (9F A5 A5, 28, 9F 5A 5A: star_a3_s0_display_off_cmdtbl) and then its exit sequence's sleep-in
 * (9F A5 A5, 10, 9F 5A 5A, and its 100 ms: star_a3_s0_exit_cmdtbl, star_a3_s0_wait_sleep_in), with a frame's time between
 * the two, as when Android turns the screen off. Without panel=1 too: the DSIM is mapped here if the driver never started.
 * The lock is only tried (a halt does not wait on a brightness change that never ends); taken or not, the packets go.
 * True when the DSIM took all of them. After it the panel shows nothing and keeps nothing to show: nothing turns it on
 * again but its own power-on sequence, which only a restart (S-Boot) sends.
 */
bool hakios_s6e3ha8_blank(int (*say_as)(const char *, ...));

bool
hakios_s6e3ha8_blank(int (*say_as)(const char *, ...))
{
	static const uint8_t key1_on[] = { 0x9F, 0xA5, 0xA5 }, key1_off[] = { 0x9F, 0x5A, 0x5A };
	static const uint8_t display_off[] = { 0x28 }, sleep_in[] = { 0x10 };
	bool locked = panel_lock != NULL && lck_mtx_try_lock(panel_lock), off, asleep;

	if (dsim == 0) {
		dsim = ml_io_map(DSIM_PA, 0x1000);
	}
	if (dsim == 0) {
		say_as("s6e3ha8: blank: cannot map the DSIM at 0x%lx\n", DSIM_PA);
		if (locked) {
			lck_mtx_unlock(panel_lock);
		}
		return false;
	}
	off = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(display_off, sizeof(display_off));
	off = dsim_send(key1_off, sizeof(key1_off)) && off;
	say_as("s6e3ha8: blank: display off (28) %s (FIFOCTRL %x, INTSRC %x)%s\n", off ? "sent" : "NOT SENT", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC), panel_lock == NULL ? "" : locked ? "" : "; the panel's lock was held: sent without it");
	delay(20 * 1000);                       /* a frame and more at 60 Hz */
	asleep = dsim_send(key1_on, sizeof(key1_on)) && dsim_send(sleep_in, sizeof(sleep_in));
	asleep = dsim_send(key1_off, sizeof(key1_off)) && asleep;
	delay(100 * 1000);                      /* Samsung's wait after sleep-in, before the supplies go */
	say_as("s6e3ha8: blank: sleep-in (10) %s (FIFOCTRL %x, INTSRC %x)\n", asleep ? "sent" : "NOT SENT", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC));
	if (locked) {
		lck_mtx_unlock(panel_lock);
	}
	return off && asleep;
}


void
hakios_s6e3ha8_init(void)
{
	uint32_t enabled = 0, level = 0, mode = 2;

	if (!PE_parse_boot_argn("panel", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6e3ha8", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(DSIM_PA, 0x1000);
	if (dsim == 0) {
		say("s6e3ha8: cannot map the DSIM at 0x%lx\n", DSIM_PA);
		return;
	}
	(void)PE_parse_boot_argn("panel_gamma", &mode, sizeof(mode));
	say("s6e3ha8: DSIM0: FIFOCTRL %x, INTSRC %x; panel_gamma=%u; kern.hakios.brightness sets the panel's brightness\n",
	    rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC), mode);
	if (mode >= 1) {
		panel_gamma_init(mode);
	}
	(void)hakios_panel_register("s6e3ha8", panel_set);
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		int error = panel_set((int)level);

		if (gamma_ready && level <= 255) {
			say("s6e3ha8: brightness %u: %s (the row of %u nits, AOR 0x%04x)\n", level, error == 0 ? "sent" :
			    "NOT SENT", gamma_panel->rows[ha8_row_of_level((int)level)].nits, gamma_panel->aor[level]);
		} else {
			say("s6e3ha8: brightness %u: %s\n", level, error == 0 ? "sent" : "NOT SENT");
		}
	}
}
