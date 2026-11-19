/*
 * The Galaxy Note9's S Pen: a Wacom W9018 digitizer at 0x56 on the HSI2C of USI09 (hsi2c@10910000, "hsi2c23"), its pen
 * touches given to Darwin as a finger's (finger SPEN_FINGER of /dev/hakios-touch): the iPhone this runs as has no pencil.
 *
 * Boot-arg spen=N (on the Note9, loader_platform=crownlte, 3 unless spen= says otherwise -- its boot line has no room):
 *   1  the bus set up and the chip's query read: its firmware version and its coordinates' ranges, nothing sent to it
 *   2  ... and its sampling started (COM_SAMPLERATE_START); the first packets said, nothing given to Darwin
 *   3  ... and the pen's contact given to Darwin as a touch: down when the tip presses, moves, up when it lifts
 *
 * Facts, from Samsung's device tree for the Note9 (exynos9810-crownlte_eur_open_26.dts: hsi2c@10910000, wacom@56,
 * hsi2c23-bus, epen-int-active, usi@10811034, wacom_ldo_en) and its GPL driver (drivers/input/wacom/wacom_i2c.c,
 * wacom.h: the command bytes, the query's and the coordinate packet's layouts), and the Exynos 9810's clock table
 * (cal-if/exynos9810/cmucal-sfr.c: the USI09_I2C gates). Read for facts; nothing copied.
 *   the bus      hsi2c@10910000 = USI09_I2C of PERIC1; its USI mode word 0x10811034; CMU_PERIC1 0x10800000 gates
 *                GATE_CLK_PERIC1_USI_I2C 0x2050, RSTNSYNC_CLK_PERIC1_USI09_I2C 0x20A0, USI09_I2C IPCLK 0x20FC and PCLK 0x2100,
 *                QCH_CON_USI09_I2C 0x3044; the pins gpp5-6 (SCL) and gpp5-7 (SDA), function 3 (gpp5's CON 0x10830020)
 *   the pins     the pen's interrupt gpa3-3 (low while a packet waits), its detection (pdct) gpa3-7, both ALIVE
 *                (DAT 0x14050084); the firmware-update pin (fwe, gpm41-0) is never touched
 *   the supply   wacom_3.3v, a fixed regulator on gpm42-0 that the boot loader leaves on (regulator-boot-on); not touched
 *   the query    29 bytes read; the query proper from byte 14: [0] 0x0F, [1,2] max x, [3,4] max y, [5,6] max pressure,
 *                [7,8] firmware version (big-endian 16-bit)
 *   a packet     14 bytes read: [0] bit 7 ready (a coordinate), bit 4 the pen in range, bit 5 its side button;
 *                [1,2] x, [3,4] y, [5,6] pressure (big-endian 16-bit), [8] [9] tilt; [12] bit 7 a soft key (ignored)
 *   the axes     wacom,invert = <1 0 1>: x inverted, then x and y swapped -- the display's x is the digitizer's y, its y
 *                the digitizer's (max - x); max 14563 x 7085 (wacom,max_coords)
 *
 * Palm rejection: while the pen is near (a coordinate packet, hover or contact, in the last NEAR_MS) the touch screen's
 * fingers are dropped (hakios_spen_near(), asked by hakios.s6sy761.c), as Samsung's driver tells its touch screen to
 * stop: the fingers holding the pen were taken for touches beside the tip (15 November: touches "in the air", up and
 * left of the tip, and the pen's own taps lost among them). kern.hakios.spen_palm=0 turns it off.
 * kern.hakios.spen_dx / spen_dy (of 65535, signed) shift the pen's point, to calibrate. Not yet: hover, the side
 * button, pressure.
 *
 * Copyright (c) 2026 the HakiOS contributors. APSL 2.0.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <stdarg.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void loader_log_line(const char *line);
extern bool hakios_panel_touch_blocked(uint32_t type, uint32_t finger);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define CHIP                    0x56
#define SPEN_FINGER             11              /* the touch screen's are 0-9 */

#define ALIVE_PA                0x14050000UL
#define GPA3                    0x080
#define IRQ_PIN                 3               /* low: a packet waits */
#define PDCT_PIN                7

#define COM_SAMPLERATE_STOP     0x30
#define COM_SAMPLERATE_START    0x31            /* 133 a second */
#define QUERY_BYTES             29
#define QUERY_POS               14
#define PACKET_BYTES            14

#define PRESS_ON                24              /* of 4095: down; a press's first packet carries 30 or more */
#define RELEASE_PACKETS         3               /* about 22 ms at 133 a second */
#define NEAR_MS                 300             /* the pen counts as near this long after its last packet */
#define RAW_SAID                120             /* raw packets said while pressed */
#define MAX_X                   14563
#define MAX_Y                   7085

static struct hakios_hsi2c bus = {
	.name = "w9018",
	.regs_pa = 0x10910000,                  /* hsi2c@10910000: USI09_I2C of PERIC1 (hsi2c23) */
	.usi_mode_pa = 0x10811034,              /* usi@10811034: its mode word ("i2c" in the device tree) */
	.cmu_pa = 0x10800000, .cmu_size = 0x4000,       /* CMU_PERIC1 */
	.gates = { 0x2050, 0x20A0, 0x20FC, 0x2100 }, .ngates = 4,
	.qch = 0x3044, .div = 0x1830, .mux = 0x0120,
	.pins_pa = 0x10830020,                  /* gpp5: SCL pin 6, SDA pin 7, function 3 */
	.pins_mask = 0xffu << 24, .pins_want = 0x33u << 24,
	.ipclk_mhz = 400, .bus_hz = 400000,
};

static vm_offset_t alive;
static uint32_t max_x = MAX_X, max_y = MAX_Y;

SYSCTL_DECL(_kern_hakios);
static int spen_palm = 1, spen_dx, spen_dy;
SYSCTL_INT(_kern_hakios, OID_AUTO, spen_palm, CTLFLAG_RW | CTLFLAG_LOCKED, &spen_palm, 0,
    "the touch screen's fingers dropped while the S Pen is near");
SYSCTL_INT(_kern_hakios, OID_AUTO, spen_dx, CTLFLAG_RW | CTLFLAG_LOCKED, &spen_dx, 0, "the S Pen's x shift (of 65535)");
SYSCTL_INT(_kern_hakios, OID_AUTO, spen_dy, CTLFLAG_RW | CTLFLAG_LOCKED, &spen_dy, 0, "the S Pen's y shift (of 65535)");

static volatile uint64_t near_until;            /* mach_absolute_time() */
static volatile uint32_t near_x, near_y;        /* the pen's last point, of 65535 */

bool hakios_spen_near(uint32_t *x, uint32_t *y);

bool
hakios_spen_near(uint32_t *x, uint32_t *y)
{
	if (!spen_palm || mach_absolute_time() >= near_until) {
		return false;
	}
	*x = near_x;
	*y = near_y;
	return true;
}

static uint32_t
shift(uint32_t v, int by)
{
	int64_t r = (int64_t)v + by;

	return r < 0 ? 0 : (r > 65535 ? 65535 : (uint32_t)r);
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static bool
irq_low(void)
{
	return ((rd(alive, GPA3 + 4) >> IRQ_PIN) & 1) == 0;
}

static uint32_t
chip_read(uint8_t *buf, uint32_t len)
{
	return hakios_hsi2c_message(&bus, CHIP, true, buf, len, true);
}

static uint32_t
chip_cmd(uint8_t cmd)
{
	return hakios_hsi2c_message(&bus, CHIP, false, &cmd, 1, true);
}

static void
post(uint32_t type, uint32_t x, uint32_t y)
{
	if (!hakios_panel_touch_blocked(type, SPEN_FINGER)) {
		hakios_touch_post(type, SPEN_FINGER, x, y);
	}
}

static void
pen_thread(void *param, __unused wait_result_t wr)
{
	uint32_t steps = (uint32_t)(uintptr_t)param, error, said = 0;
	uint8_t q[QUERY_BYTES], p[PACKET_BYTES];
	bool down = false;
	uint32_t last_x = 0, last_y = 0;
	uint64_t packets = 0, touches = 0, near_ticks;

	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (!alive || !hakios_hsi2c_map(&bus, say)) {
		say("w9018: the bus or the pins could not be mapped: no S Pen\n");
		thread_terminate(current_thread());
		return;
	}
	hakios_hsi2c_power(&bus);
	hakios_hsi2c_reset(&bus);
	say("w9018: 1: the pins: interrupt %s, pen detection %s; the bus at %u Hz\n", irq_low() ? "low" : "high",
	    ((rd(alive, GPA3 + 4) >> PDCT_PIN) & 1) ? "high" : "low", bus.bus_hz);

	error = chip_read(q, sizeof(q));
	if (error != 0) {
		say("w9018: 1: the query: no answer at 0x%x (%x): no S Pen (its supply off, or another bus)\n", CHIP, error);
		thread_terminate(current_thread());
		return;
	}
	{
		const uint8_t *qq = q + QUERY_POS;
		uint32_t qx = (uint32_t)qq[1] << 8 | qq[2], qy = (uint32_t)qq[3] << 8 | qq[4];

		say("w9018: 1: the query raw: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9],
		    q[10], q[11], q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23], q[24], q[25], q[26],
		    q[27], q[28]);
		say("w9018: 1: the query: header %02x, max x %u, max y %u, max pressure %u, firmware %04x\n", qq[0], qx, qy,
		    (uint32_t)qq[5] << 8 | qq[6], (uint32_t)qq[7] << 8 | qq[8]);
		if (qq[0] == 0x0F && qx > 1000 && qy > 1000) {
			max_x = qx;
			max_y = qy;
		}
	}
	if (steps < 2) {
		say("w9018: stopping here (spen=%u)\n", steps);
		thread_terminate(current_thread());
		return;
	}
	nanoseconds_to_absolutetime((uint64_t)NEAR_MS * NSEC_PER_MSEC, &near_ticks);
	error = chip_cmd(COM_SAMPLERATE_START);
	say("w9018: 2: sampling started (%s)%s\n", error ? "refused" : "taken", steps < 3 ? "; packets said, nothing given to Darwin" :
	    "; the pen's contact goes to Darwin as a touch");

	for (;;) {
		if (!irq_low()) {
			delay_for_interval(4, NSEC_PER_MSEC);
			continue;
		}
		if (chip_read(p, sizeof(p)) != 0) {
			delay_for_interval(20, NSEC_PER_MSEC);
			continue;
		}
		packets++;
		bool ready = p[0] & 0x80, prox = p[0] & 0x10;
		uint32_t wx = (uint32_t)p[1] << 8 | p[2], wy = (uint32_t)p[3] << 8 | p[4];
		uint32_t pressure = (uint32_t)p[5] << 8 | p[6];

		/*
		 * The raw bytes while the tip presses (the first 400 such packets; and one in 500 of the rest): the hover's are
		 * known (14 November: status 0x8n, pressure 0), a contact's carry 0x10 and a pressure.
		 */
		if (((p[0] & 0x10) || pressure > 0) ? said < RAW_SAID : (packets % 2000) == 0) {
			said += ((p[0] & 0x10) || pressure > 0) ? 1 : 0;
			say("w9018: raw %llu: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    (unsigned long long)packets, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
		}
		if (steps < 3 || !ready || (p[12] & 0x80)) {
			continue;
		}
		/* the display's axes: x inverted, then swapped (wacom,invert = <1 0 1>) */
		uint32_t ix = wx > max_x ? 0 : max_x - wx;
		uint32_t sx = shift(wy >= max_y ? 65535 : wy * 65535 / max_y, spen_dx);
		uint32_t sy = shift(ix >= max_x ? 65535 : ix * 65535 / max_x, spen_dy);

		near_x = sx;
		near_y = sy;
		near_until = mach_absolute_time() + near_ticks;
		/*
		 * Contact with hysteresis: down once the tip presses (0x10 with a pressure over PRESS_ON), held while 0x10
		 * stays (Samsung's BTN_TOUCH is 0x10 alone), up only after RELEASE_PACKETS packets in a row without it. The
		 * 14 November log: no packet had a pressure without 0x10, a press's first carried 31 and then 86 and more.
		 */
		static uint32_t zero_run;
		bool pressing = (p[0] & 0x10) && (down || pressure > PRESS_ON);
		bool contact;

		if (pressing) {
			zero_run = 0;
			contact = true;
		} else if (down && ++zero_run < RELEASE_PACKETS) {
			contact = true;
		} else {
			contact = false;
		}
		(void)prox;

		if (contact && !down) {
			down = true;
			touches++;
			post(HAKIOS_TOUCH_DOWN, sx, sy);
		} else if (contact) {
			post(HAKIOS_TOUCH_MOVE, sx, sy);
		} else if (down) {
			down = false;
			post(HAKIOS_TOUCH_UP, last_x, last_y);
			if (touches <= 5) {
				say("w9018: 3: touch %llu lifted at %u,%u (of 65535)\n", (unsigned long long)touches, last_x, last_y);
			}
		}
		last_x = sx;
		last_y = sy;
	}
}

void hakios_w9018_init(void);

void
hakios_w9018_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("spen", &steps, sizeof(steps))) {
		char platform[24] = "";

		(void)PE_parse_boot_arg_str("loader_platform", platform, sizeof(platform));
		steps = strcmp(platform, "crownlte") == 0 ? 3 : 0;
	}
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start_priority((thread_continue_t)pen_thread, (void *)(uintptr_t)steps, 92, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
