#define HAKIOS_SOC_FIXED
/*
 * The Galaxy S9's battery (starlte, Exynos 9810): its fuel gauge and charger, both blocks of a Maxim MAX77705 on the I2C
 * of USI03 of the CMGP block (HSI2C at 0x14360000), read every few seconds and handed to iOS's power source through
 * hakios_battery_report() (drivers/power/hakios.battery.c). Only with boot-arg max77705=1: every address below belongs
 * to this phone.
 *
 * THIS DRIVER ONLY READS THE CHIP, but for the charger's two currents (charger_tune, max77705_chg=0 to leave them) and the
 * watchdog's kick (max77705_wdt=1, off by default). The MAX77705 is the phone's charger: a wrong write there changes how
 * the battery is charged. Nothing else is written to it, not even the latches its interrupt handler would clear on Android (the interrupt
 * registers are not read at all: reading them clears them). What is written is the bus's own: the clock gates of
 * CMGP's USI03, its mode word, the controller (drivers/soc/exynos/hakios.exynos_hsi2c.c).
 *
 *   the fuel gauge, 0x36: 16-bit registers, little-endian
 *     0x00 status (bit 3: no battery), 0x05 RepCap and 0x35 FullCapRep (1 mAh), 0x06 RepSOC (1/256 %),
 *     0x09 VCELL (78.125 uV), 0x0A current (signed, 1.5625 uV over the 5 mOhm sense resistor: 312.5 uA, more than 0
 *     while charging), 0x17 cycles (1/100)
 *   the charger, 0x69: 0xB2 CHG_INT_OK (bit 6 the USB input, bit 5 wireless), 0xB4 DTLS_01 ([3:0]: 0-2 charging,
 *     3-4 full, 5-7 not charging, 8 and up off)
 *   the chip, 0x66: 0x01 its revision (bits 2:0) and version (7:3)
 * The gauge learns the battery on its own and keeps that while the battery is connected (it is not removable); no model
 * is loaded by the host on Android either. The charge iOS sees is RepSOC; Android's own percentage is the same scaled to
 * a learnt maximum, so near full the two may differ by some percent. The temperature is not the gauge's (Android writes
 * an ADC thermistor's reading into it): it is the thermistor's, channel 4 of the CMGP block's ADC (0x14230000, 12 bits),
 * read as Android does (five conversions, the highest and the lowest dropped) and turned into degrees by the phone's own
 * table (battery,temp_table_adc and _data in its device tree). The ADC: CON1 0x00 (bit 2 reset, bit 1 out of reset,
 * bit 0 start), CON2 0x04 ([6:4] conversion time, [3:0] the channel), DAT 0x08 [11:0], INT_EN 0x10, INT_ST 0x14 (bit 0
 * done, 1 clears it); its clock gates CMU_CMGP +0x2018 and +0x201C, its Q-channels +0x3008 and +0x300C.
 * Sources: the phone's device tree (hsi2c@14360000, max77705 at 0x66, the fuel gauge's settings: fg_resistor 2, capacity
 * 2967 mAh; adc@14230000, the temperature table), Linux's drivers/battery_v2/max77705_fuelgauge.c, _charger.c, sec_adc.c,
 * drivers/mfd/max77705.c and drivers/iio/adc/exynos_adc.c of the S9's kernel (GPL: read for these facts, not copied).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg max77705_steps=N stops after the
 * first N:
 *   1  map the bus's blocks and read what they are set to   (nothing written)
 *   2  the bus's clocks on and USI03 to I2C, the ADC's on    (the first writes)
 *   3  the bus reset and set to 400 kHz
 *   4  read the chip's revision, the gauge and the charger once
 *   5  report every 5 seconds (the default)
 *
 * Boot-arg max77705_diag=1 (read-only, off by default): for the charging and the power measurements, at step 4 and with
 * every 5-minute line, the charger's own state as it is, raw: CHG_INT_OK and DETAILS_00-03 (0xB2-0xB6) and CNFG_00-14
 * (0xB7-0xC5), with what the Android driver names in them (CNFG_00 [3:0] the mode, bit 4 WDTEN, the charger's watchdog;
 * DETAILS_00 [6:5] CHGIN_DTLS, 3 a valid input; DETAILS_01 [3:0] CHG_DTLS, [6:4] BAT_DTLS; DETAILS_02 [3:0] BYP_DTLS;
 * CNFG_02 [5:0] the fast-charge current, 50 mA a step; CNFG_09 [6:0] the input's limit). 0xB0 (CHG_INT) is not read:
 * reading it clears it. Boot-arg max77705_fg_s=S (5 to 600, read-only): a short gauge line every S seconds (rounded to
 * the 5 s poll) with AvgCurrent (0x0B) and QH (0x4D, the raw coulomb counter, to difference) besides Current,
 * for a drain measured over minutes rather than read off a percentage.
 *
 * Boot-arg max77705_wdt=1 (off by default; the one write to the charger, given its go by the phone's owner on 7 November):
 * the charger's watchdog kicked, as Samsung's driver kicks it at every health poll (max77705_chg_set_wdtmr_kick()). On
 * this kernel the charger was seen to stop charging within five minutes of every boot on USB ("charging" at about 1 A,
 * then "on a charger, not charging" for the rest of the run): S-Boot leaves the watchdog on (CNFG_00 0xB7 bit 4, WDTEN)
 * and Android's driver keeps it fed; nothing here did. Every 30 s, and only while CNFG_00 reads WDTEN set: CNFG_06 (0xBD)
 * read, its bits [1:0] (WDTCLR) set to 01, the rest as read, written back. Nothing else: not WDTEN, not a current, a
 * voltage, CHGPROT or the mode. Kicked rather than switched off, the watchdog still stops the charging if this kernel
 * hangs. The log: the first kick with both registers, then a count with every 5-minute line.
 *
 * Boot-arg max77705_ipclk=MHz: the HSI2C's input clock, if step 1 says it is not the 200 MHz Android sets (the device
 * tree's default-clk); the bus's timing is worked out from it. (CMGP's mux, +0x1018 bit 0, picks the RCO oscillator at
 * 0 and CMGP's bus clock at 1; the divider is +0x1814 [3:0].)
 * The clocks forced on are the four of USI_CMGP03 in CMU_CMGP: GATE_CLK_USI_CMGP03 +0x2014, its IPCLK +0x20A8 and PCLK
 * +0x20AC, its RSTnSYNC +0x2074; its Q-channel +0x3050 (cal-if/exynos9810/cmucal-sfr.c of the S9's kernel).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.log.h>
#include <sys/hakios.xnu.h>
#include "hakios.dev_strict.h"

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */

#define PMIC            0x66
#define CHARGER         0x69
#define GAUGE           0x36

#define FG_STATUS       0x00
#define FG_REPCAP       0x05
#define FG_REPSOC       0x06
#define FG_VCELL        0x09
#define FG_CURRENT      0x0A
#define FG_AVGCURRENT   0x0B
#define FG_QH           0x4D
#define FG_CYCLES       0x17
#define FG_FULLCAPREP   0x35
#define CHG_INT_OK      0xB2
#define CHG_DTLS_01     0xB4
#define CHG_CNFG_00     0xB7            /* bit 4 WDTEN: the charger's watchdog on */
#define CHG_CNFG_06     0xBD            /* [1:0] WDTCLR: 01 kicks the watchdog (max77705_wdt=1) */
#define CHG_WDTEN       (1u << 4)
#define CHG_WDTCLR_MASK 0x03u
#define CHG_WDTCLR      0x01u
#define WDT_KICK_POLLS  6               /* every 30 s at the 5 s poll */
#define CHG_DIAG_FIRST  0xB2            /* CHG_INT_OK: max77705_diag reads from here (not 0xB0, CHG_INT: read clears) */
#define CHG_DIAG_LAST   0xC5            /* CNFG_14 */


#define ADC_PA          EXYNOS_ADC_PA
#define ADC_CON1        0x00
#define ADC_CON2        0x04
#define ADC_DAT         0x08
#define ADC_INT_EN      0x10
#define ADC_INT_ST      0x14
#define ADC_CHANNEL     4               /* the battery's thermistor (sec-battery's adc-temp) */
static const uint32_t adc_gates[] = { 0x2018, 0x201C }, adc_qch[] = { 0x3008, 0x300C };

/* the device tree's battery,temp_table_adc and _data: the ADC's reading, and tenths of a degree */
static const uint16_t temp_adc[] = { 275, 327, 385, 451, 535, 626, 737, 864, 1013, 1210, 1361, 1554, 1769, 2007, 2239,
	                             2476, 2702, 2924, 3116, 3294, 3446, 3564, 3676 };
static const int16_t temp_data[] = { 900, 850, 800, 750, 700, 650, 600, 550, 500, 450, 400, 350, 300, 250, 200, 150, 100,
	                             50, 0, -50, -100, -150, -200 };

/*
 * The other phones of the Exynos 9810 with this chip at the same address, their batteries' own numbers: the Galaxy S9+
 * (star2lte) and the Note9 (crownlte), told apart by the loader's platform (the device_type of /arm-io, which the loader
 * writes from its loader_platform=). Same bus, same ADC channel; a bigger battery, and another thermistor curve. From
 * Samsung's device trees of the BSP (exynos9810-star2lte_eur_open_26.dts and exynos9810-crownlte_eur_open_26.dts:
 * max77705-fuelgauge capacity, battery temp_table_adc and _data). Written, not seen: nobody has run this on either phone.
 * Anything else, the S9 included, keeps the S9's numbers above, exactly as before.
 */
static const uint16_t star2lte_temp_adc[] = { 275, 336, 415, 483, 551, 667, 764, 903, 1054, 1189, 1384, 1575, 1787, 2042,
	                                      2263, 2492, 2723, 2937, 3127, 3309, 3463, 3578, 3678 };
static const uint16_t crownlte_temp_adc[] = { 313, 357, 429, 488, 571, 677, 789, 890, 920, 945, 1037, 1078, 1114, 1194,
	                                      1234, 1273, 1384, 1429, 1475, 1633, 1861, 2090, 2311, 2495, 2539, 2587, 2717,
	                                      2737, 2808, 2933, 2969, 3006, 3132, 3165, 3201, 3336, 3481, 3693, 3765 };
static const int16_t crownlte_temp_data[] = { 900, 850, 800, 750, 700, 650, 600, 560, 550, 540, 510, 500, 490, 460, 450,
	                                      440, 410, 400, 390, 350, 300, 250, 200, 160, 150, 140, 110, 100, 90, 60, 50,
	                                      40, 10, 0, -10, -50, -100, -200, -250 };
static const struct battery_board {
	const char *platform;           /* the loader's platform name, /arm-io's device_type */
	uint32_t design_mah;            /* the device tree's fuelgauge,capacity */
	const uint16_t *adc;
	const int16_t *data;
	unsigned n;
} battery_boards[] = {
	{ "starlte", 2967, temp_adc, temp_data, sizeof(temp_adc) / sizeof(temp_adc[0]) },
	{ "star2lte", 3303, star2lte_temp_adc, temp_data, sizeof(star2lte_temp_adc) / sizeof(star2lte_temp_adc[0]) },
	{ "crownlte", 3753, crownlte_temp_adc, crownlte_temp_data, sizeof(crownlte_temp_adc) / sizeof(crownlte_temp_adc[0]) },
};
_Static_assert(sizeof(star2lte_temp_adc) / sizeof(star2lte_temp_adc[0]) == sizeof(temp_data) / sizeof(temp_data[0]),
    "the S9+'s table has the S9's temperatures");
_Static_assert(sizeof(crownlte_temp_adc) / sizeof(crownlte_temp_adc[0]) ==
    sizeof(crownlte_temp_data) / sizeof(crownlte_temp_data[0]), "the Note9's table is in pairs");
static const struct battery_board *board = &battery_boards[0];
static vm_offset_t adc;
#define POLL_MS         5000

static struct hakios_hsi2c bus = {
	.name = "max77705",
	.regs_pa = EXYNOS_CMGP_USI03_HSI2C_PA,                  /* hsi2c@14360000: USI03 of CMGP */
	.usi_mode_pa = EXYNOS_CMGP_USI03_MODE_PA,              /* usi@14212030: its mode word */
	.cmu_pa = EXYNOS_CMU_CMGP_PA, .cmu_size = 0x4000,       /* CMU_CMGP */
	/* GATE_CLK_USI_CMGP03 (the divider's source), USI_CMGP03 IPCLK and PCLK, its RSTnSYNC: as the touch's bus */
	.gates = { 0x2014, 0x20A8, 0x20AC, 0x2074 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1814, .mux = 0x1018,
	.pins_pa = 0,                           /* GPM14-0 and GPM15-0: the boot loader talks to the chip, they are set */
	.ipclk_mhz = 400, .bus_hz = 400000,       /* CMU divider 0 on the 400 MHz branch */
};

/* The bus is the battery's and the motor's (max77705_haptic=1): one transfer at a time, whoever asks. */
static lck_mtx_t *bus_lock;

static uint32_t
locked_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t error;

	lck_mtx_lock(bus_lock);
	error = hakios_hsi2c_read_reg(&bus, addr, reg, buf, len);
	lck_mtx_unlock(bus_lock);
	return error;
}

static uint32_t
read16(uint8_t addr, uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	uint32_t error = locked_read(addr, reg, b, 2);

	if (error == 0) {
		*value = (uint16_t)(b[1] << 8 | b[0]);
	}
	return error;
}

static uint32_t
read8(uint8_t addr, uint8_t reg, uint8_t *value)
{
	return locked_read(addr, reg, value, 1);
}

static inline uint32_t
adc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(adc + off);
}

static inline void
adc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(adc + off) = v;
}

/* the ADC's clocks on (in CMU_CMGP, which the bus has mapped), reset, its conversion time set */
static void
adc_init(void)
{
	adc = ml_io_map(ADC_PA, 0x1000);
	if (!adc || !bus.cmu) {
		adc = 0;
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		*(volatile uint32_t *)(bus.cmu + adc_gates[i]) |= (1u << 20) | (1u << 21);
		*(volatile uint32_t *)(bus.cmu + adc_qch[i]) |= 3;
	}
	adc_wr(ADC_CON1, 1u << 2);
	adc_wr(ADC_CON1, 1u << 1);
	adc_wr(ADC_CON2, 6u << 4);
	adc_wr(ADC_INT_EN, 1);
}

/* one conversion of the thermistor's channel; -1 if it does not finish */
static int
adc_convert(void)
{
	adc_wr(ADC_CON2, (adc_rd(ADC_CON2) & ~0xfu) | ADC_CHANNEL);
	adc_wr(ADC_CON1, adc_rd(ADC_CON1) | 1);
	for (int waited = 0; waited < 10000; waited++) {
		if (adc_rd(ADC_INT_ST) & 1) {
			int value = (int)(adc_rd(ADC_DAT) & 0xfff);

			adc_wr(ADC_INT_ST, 1);
			return value;
		}
		delay(1);
	}
	return -1;
}

/* the battery's temperature in hundredths of a degree, or 2500 (25 C) when the ADC does not answer */
static int32_t
battery_temperature(int *raw_out)
{
	int sample[5], low = 0, high = 0, sum = 0, raw;
	const uint16_t *temp_adc = board->adc;
	const int16_t *temp_data = board->data;
	unsigned n = board->n;

	*raw_out = -1;
	if (!adc) {
		return 2500;
	}
	for (int i = 0; i < 5; i++) {
		if ((sample[i] = adc_convert()) < 0) {
			return 2500;
		}
		low = (i == 0 || sample[i] < sample[low]) ? i : low;
		high = (i == 0 || sample[i] > sample[high]) ? i : high;
	}
	for (int i = 0; i < 5; i++) {
		sum += (i == low || i == high) ? 0 : sample[i];
	}
	raw = sum / (low == high ? 4 : 3);
	*raw_out = raw;
	if (raw <= temp_adc[0]) {
		return temp_data[0] * 10;
	}
	if (raw >= temp_adc[n - 1]) {
		return temp_data[n - 1] * 10;
	}
	for (unsigned i = 1; i < n; i++) {
		if (raw <= temp_adc[i]) {
			/* between two points of the table, on the line through them (tenths of a degree, to hundredths) */
			return (temp_data[i - 1] * 10) + (temp_data[i] - temp_data[i - 1]) * 10 * (raw - temp_adc[i - 1]) /
			       (temp_adc[i] - temp_adc[i - 1]);
		}
	}
	return 2500;
}

static int last_adc = -1;              /* the thermistor's last reading, for the log */

/* the gauge and the charger as one state; false if a read failed */
static bool
battery_read(struct hakios_battery_state *b)
{
	uint16_t status, soc, vcell, cap, full, cycles;
	int16_t current;
	uint8_t ok, dtls;

	if (read16(GAUGE, FG_STATUS, &status) || read16(GAUGE, FG_REPSOC, &soc) || read16(GAUGE, FG_VCELL, &vcell) ||
	    read16(GAUGE, FG_CURRENT, (uint16_t *)&current) || read16(GAUGE, FG_REPCAP, &cap) ||
	    read16(GAUGE, FG_FULLCAPREP, &full) || read16(GAUGE, FG_CYCLES, &cycles) ||
	    read8(CHARGER, CHG_INT_OK, &ok) || read8(CHARGER, CHG_DTLS_01, &dtls)) {
		return false;
	}
	memset(b, 0, sizeof(*b));
	b->size = sizeof(*b);
	b->percent_hundredths = (int32_t)((uint32_t)soc * 100 / 256);
	if (b->percent_hundredths > 10000) {
		b->percent_hundredths = 10000;
	}
	b->voltage_uv = (int32_t)((uint64_t)vcell * 78125 / 1000);
	b->current_ua = (int32_t)current * 3125 / 10;
	b->temperature_hundredths = battery_temperature(&last_adc);    /* (the thermistor's: see the top of this file) */
	b->present = !(status & (1u << 3));
	b->external = (ok & (3u << 5)) != 0;           /* the USB input or the wireless one */
	switch (dtls & 0xf) {
	case 0: case 1: case 2:
		b->status = 1;                          /* charging */
		break;
	case 3: case 4:
		b->status = 4;                          /* full */
		break;
	default:
		b->status = b->external ? 3 : 2;        /* not charging, discharging */
		break;
	}
	b->charge_uah = (int32_t)cap * 1000;
	b->full_uah = (int32_t)full * 1000;
	b->design_uah = (int32_t)board->design_mah * 1000;
	b->cycles = cycles / 100;
	b->usb_type = b->external ? 2 : 0;
	return true;
}

/*
 * The battery's current, a line a minute and a sum since the start by whether the screen was on, for comparing what a change
 * costs (round 21 on the Note9: battery optimisation needs numbers; the 5-minute line gave one or two a test). A reading
 * every POLL_MS; positive current is charging, so what is drawn is the negative. Nothing is said while a charger is attached.
 */
extern bool hakios_panel_asleep(void);          /* drivers/display/panel/hakios.s6e3ha8.c (stubs.c: never) */
static struct {
	int64_t minute_ua;
	unsigned minute_reads, minute_on;
	int64_t on_ua, off_ua;
	uint64_t on_reads, off_reads;
	int64_t save_ua, nosave_ua;             /* dark_ab: the dark readings in each half (hakios.s6e3ha8.c) */
	uint64_t save_reads, nosave_reads;
} power;
extern bool hakios_dark_ab_off(void);
extern bool hakios_dark_ab_flip(void);

static void
power_note(const struct hakios_battery_state *b)
{
	bool on = !hakios_panel_asleep();

	if (b->external) {
		power.minute_reads = power.minute_on = 0;
		power.minute_ua = 0;
		return;
	}
	power.minute_ua += b->current_ua;
	power.minute_reads++;
	power.minute_on += on;
	if (on) {
		power.on_ua += b->current_ua;
		power.on_reads++;
	} else {
		power.off_ua += b->current_ua;
		power.off_reads++;
		if (hakios_dark_ab_off()) {
			power.nosave_ua += b->current_ua;
			power.nosave_reads++;
		} else {
			power.save_ua += b->current_ua;
			power.save_reads++;
		}
	}
	if (power.minute_reads * (POLL_MS / 1000) >= 60) {
		say("max77705: a minute: %lld mA drawn on average (the screen on in %u of %u readings), %d.%02d%%, %d mV; since the "
		    "start: screen on %llu s at %lld mA, off %llu s at %lld mA\n", -power.minute_ua / power.minute_reads / 1000,
		    power.minute_on, power.minute_reads, b->percent_hundredths / 100, b->percent_hundredths % 100,
		    b->voltage_uv / 1000, power.on_reads * (POLL_MS / 1000),
		    power.on_reads ? -power.on_ua / (int64_t)power.on_reads / 1000 : 0, power.off_reads * (POLL_MS / 1000),
		    power.off_reads ? -power.off_ua / (int64_t)power.off_reads / 1000 : 0);
		power.minute_ua = 0;
		power.minute_reads = power.minute_on = 0;
		if (hakios_dark_ab_flip()) {
			say("max77705: dark A/B since the start: the savings on %llu s at %lld mA, off %llu s at %lld mA; the next minute "
			    "%s\n", power.save_reads * (POLL_MS / 1000),
			    power.save_reads ? -power.save_ua / (int64_t)power.save_reads / 1000 : 0, power.nosave_reads * (POLL_MS / 1000),
			    power.nosave_reads ? -power.nosave_ua / (int64_t)power.nosave_reads / 1000 : 0,
			    !hakios_panel_asleep() ? "(the screen on)" : hakios_dark_ab_off() ? "off" : "on");
		}
	}
}

static void
say_state(const char *when, const struct hakios_battery_state *b)
{
	say("max77705: %s: %d.%02d%%, %d mV, %d mA, %d.%d C (ADC %d), %s, %s, %d of %d mAh (full %d), %d cycles\n", when,
	    b->percent_hundredths / 100, b->percent_hundredths % 100, b->voltage_uv / 1000, b->current_ua / 1000,
	    b->temperature_hundredths / 100, (b->temperature_hundredths < 0 ? -b->temperature_hundredths : b->temperature_hundredths) % 100 / 10,
	    last_adc,
	    b->present ? "a battery" : "NO BATTERY", b->status == 1 ? "charging" : b->status == 4 ? "full" :
	    b->status == 3 ? "on a charger, not charging" : "discharging", b->charge_uah / 1000, b->design_uah / 1000,
	    b->full_uah / 1000, b->cycles);
}

/* ---- the USB-C port: the switch and VBUS (max77705_usbsw=1) -------------------------------------------------------- */

/*
 * The MUIC's firmware, at 0x25, takes opcodes: written to OPCODE_WRITE (0x21) with their data, ended by a write of
 * OPCODE_WRITE_END (0x41); the answer waits at OPCODE_READ (0x51), the opcode first. CONTROL1 is the switch: 0x05 reads it,
 * 0x06 writes it (COM_USB 0x09: D+/D- to the AP's USB; COM_OPEN 0). BC_STATUS (0x08, read as a plain register, which clears
 * nothing) has VBUS in bit 7 and the charger type in bits 1:0 (0 none, 1 a USB host, 2 a charging host, 3 a wall charger).
 * Samsung's driver opens the switch on every detach and closes it on every attach once the charger type is known, from
 * the chip's interrupts (drivers/muic/max77705-muic.c com_to_usb_ap, com_to_open, max77705_muic_handle_detach;
 * include/linux/ccic/max77705.h:470-485, include/linux/muic/max77705-muic.h:216-223). This driver has no interrupt, so a
 * thread reads VBUS four times a second: on a cable coming in it closes the switch, again as the detection may run on
 * (the firmware may leave it open meanwhile), and tells the gadget driver about VBUS (hakios.dwc3.c, THE CABLE COMING AND
 * GOING).
 */
#define MUIC                    0x25
#define MUIC_USBC_STATUS1       0x06
#define MUIC_USBC_STATUS2       0x07            /* the firmware's last SYSMSG (its errors with opcodes among them) */
#define MUIC_BC_STATUS          0x08
#define  BC_VBUS                (1u << 7)
#define  BC_CHGTYP(v)           ((v) & 3u)
#define OPCODE_WRITE            0x21
#define OPCODE_WRITE_END        0x41
#define OPCODE_READ             0x51
#define OPCODE_CTRL1_R          0x05
#define OPCODE_CTRL1_W          0x06
#define COM_OPEN                0x00
#define COM_USB                 0x09
#define VBUS_POLL_MS            250
#define VBUS_LOWS               2               /* readings in a row before VBUS counts as gone */

extern void hakios_usb_gadget_vbus(bool present);
static bool usbsw_running;
static volatile uint32_t usbsw_test_ms;         /* the console's usb unplug/muic: how long */
static volatile bool usbsw_test_tell;           /* usb unplug, usb vbus: VBUS taken as gone meanwhile */
static volatile bool usbsw_test_open;           /* usb unplug, usb muic: the switch open meanwhile */
static volatile bool usbsw_want_say;            /* the console's usb */
static volatile bool usbsw_want_close;          /* hakios_max77705_usb_close */

static uint32_t
usbsw_write(uint8_t value)
{
	uint8_t w1[3] = { OPCODE_WRITE, OPCODE_CTRL1_W, value }, w2[2] = { OPCODE_WRITE_END, 0 };
	uint32_t e;

	lck_mtx_lock(bus_lock);
	e = hakios_hsi2c_message(&bus, MUIC, false, w1, 3, true);
	if (e == 0) {
		e = hakios_hsi2c_message(&bus, MUIC, false, w2, 2, true);
	}
	lck_mtx_unlock(bus_lock);
	return e;
}

/* CONTROL1 as the firmware says it, or -1 when no answer came in 100 ms */
static int
usbsw_read(void)
{
	uint8_t w1[2] = { OPCODE_WRITE, OPCODE_CTRL1_R }, w2[2] = { OPCODE_WRITE_END, 0 }, answer[2] = { 0, 0 };
	uint32_t e;

	lck_mtx_lock(bus_lock);
	e = hakios_hsi2c_message(&bus, MUIC, false, w1, 2, true);
	if (e == 0) {
		e = hakios_hsi2c_message(&bus, MUIC, false, w2, 2, true);
	}
	lck_mtx_unlock(bus_lock);
	for (int i = 0; e == 0 && i < 10; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
		if ((e = locked_read(MUIC, OPCODE_READ, answer, 2)) == 0 && answer[0] == OPCODE_CTRL1_R) {
			return answer[1];
		}
	}
	return -1;
}

static void
usbsw_say(const char *when)
{
	uint8_t bc = 0, st1 = 0, st2 = 0;
	uint32_t e = read8(MUIC, MUIC_BC_STATUS, &bc);
	int ctrl1 = usbsw_read();

	(void)read8(MUIC, MUIC_USBC_STATUS1, &st1);
	(void)read8(MUIC, MUIC_USBC_STATUS2, &st2);
	say("max77705: usb switch %s: CONTROL1 %#x, BC_STATUS %#x (VBUS %u, charger type %u), USBC_STATUS1 %#x, SYSMSG %#x%s\n", when, ctrl1, bc,
	    (bc & BC_VBUS) ? 1 : 0, BC_CHGTYP(bc), st1, st2, e ? " (not read)" : "");
}

/* the switch closed and read back: tried again when the firmware has not taken it; true when it says so */
static bool
usbsw_close(void)
{
	for (int attempt = 0; attempt < 4; attempt++) {
		int ctrl1;

		(void)usbsw_write(COM_USB);
		delay_for_interval(50, NSEC_PER_MSEC);
		ctrl1 = usbsw_read();
		if (ctrl1 == COM_USB) {
			return true;
		}
		say("max77705: usb switch: asked to close, CONTROL1 still %#x (try %d)\n", ctrl1, attempt + 1);
		delay_for_interval(200, NSEC_PER_MSEC);
	}
	usbsw_say("did not close");
	return false;
}

static void
usbsw_thread(__unused void *arg, __unused wait_result_t result)
{
	/* after a cable comes in: the switch said and closed at these readings (at 250 ms each: 0, 0.5, 1.5, 3, 6 s) */
	static const uint32_t again[] = { 0, 2, 6, 12, 24 };
	uint32_t lows = 0, since_in = 0xffffffffu;
	bool vbus = false, known = false;

	for (;;) {
		uint8_t bc = 0;
		uint32_t test = usbsw_test_ms;

		if (usbsw_want_say) {
			usbsw_want_say = false;
			usbsw_say("now");
		}
		if (usbsw_want_close) {
			usbsw_want_close = false;
			usbsw_say("asked to close it");
			(void)usbsw_close();
		}
		if (test != 0) {
			bool tell = usbsw_test_tell, open = usbsw_test_open;
			uint32_t e = 0;

			usbsw_test_ms = 0;
			usbsw_say("before the test");
			if (open) {
				e = usbsw_write(COM_OPEN);
			}
			say("max77705: the test, %u ms: the switch %s (%x)%s\n", test, open ? "open" : "left alone", e, tell ? ", VBUS taken as gone" : "");
			for (uint32_t t = 0; t < test; t += VBUS_POLL_MS) {
				if (tell) {
					hakios_usb_gadget_vbus(false);
				}
				delay_for_interval(VBUS_POLL_MS, NSEC_PER_MSEC);
			}
			if (open) {
				say("max77705: usb switch %s\n", usbsw_close() ? "closed again" : "NOT closed again");
			}
			usbsw_say("after the test");
		}
		if (read8(MUIC, MUIC_BC_STATUS, &bc) == 0) {
			bool now = (bc & BC_VBUS) != 0;

			if (now) {
				lows = 0;
				if (known && !vbus) {
					say("max77705: VBUS in: BC_STATUS %#x\n", bc);
					since_in = 0;
				}
				vbus = true;
			} else if (!known || ++lows >= VBUS_LOWS) {
				if (known && vbus) {
					usbsw_say("the cable out");
				}
				vbus = false;
				since_in = 0xffffffffu;
			}
			known = true;
			hakios_usb_gadget_vbus(vbus);
		}
		if (since_in != 0xffffffffu) {
			for (unsigned i = 0; i < sizeof(again) / sizeof(again[0]); i++) {
				if (since_in == again[i]) {
					usbsw_say("the cable in");
					(void)usbsw_close();
				}
			}
			since_in = since_in > again[sizeof(again) / sizeof(again[0]) - 1] ? 0xffffffffu : since_in + 1;
		}
		delay_for_interval(VBUS_POLL_MS, NSEC_PER_MSEC);
	}
}

/* the gadget driver has VBUS and no host: the switch closed again, as after a cable coming in; never waits */
void hakios_max77705_usb_close(void);

void
hakios_max77705_usb_close(void)
{
	usbsw_want_close = true;
}

/* the USB console's usb (the switch and VBUS said), usb unplug MS, usb muic MS, usb vbus MS: done on the thread above */
void hakios_max77705_usb_console(const char *arg);

void
hakios_max77705_usb_console(const char *arg)
{
	const char *p = arg;
	uint32_t ms = 0;

	if (!usbsw_running) {
		loader_log_line("usb: the USB-C switch is not watched (max77705_usbsw=1 not given)\n");
		return;
	}
	if (*arg == 0) {
		usbsw_want_say = true;
		return;
	}
	while (*p != 0 && *p != ' ') {
		p++;
	}
	while (*p == ' ') {
		p++;
	}
	while (*p >= '0' && *p <= '9' && ms < 600000) {
		ms = ms * 10 + (uint32_t)(*p++ - '0');
	}
	if (ms != 0) {
		usbsw_test_tell = arg[0] != 'm';
		usbsw_test_open = arg[0] != 'v';
		usbsw_test_ms = ms;
	}
}

/* ---- the motor: the MAX77705's haptic block, driven by a PWM of the SoC (max77705_haptic=1) ------------------------ */

/*
 * The S9's vibration motor is an LRA driven by the MAX77705's haptic block (0x66, the chip's "top"), its strength the
 * duty of a PWM from the SoC: channel 1 of pwm@10510000 at 20.48 kHz (period 48828 ns), which the block divides by 128
 * to drive the motor at 160 Hz; 50 % duty does not drive it, the further from 50 % the stronger, 70 % (34179 ns) as far
 * as Samsung goes. Set up once: the PWM's clock gate on (CMU_PERIC0 +0x2044, its Q-channel +0x3010), the PWM pin
 * (gpp3-1) to its function, the motor's boost (gpg1-7) on, channel 1 inverted and stopped, then in the chip
 * MAINCTRL1 (0x02) bit 7 (the bias) and MCONFIG (0x10) LRA (bit 7) with the divide-by-128 (bit 1), external PWM
 * (bit 5 clear), not enabled (bit 6). A buzz: the duty set, the channel started (auto-reload), MCONFIG bits 7 and 6
 * set; still: MCONFIG bits 7 and 6 clear, the duty back to 50 %, the channel stopped.
 *
 * THE CHIP IS THE PHONE'S CHARGER as well. Two registers are written and those only in the bits above (MAINCTRL1 0x80,
 * MCONFIG 0xC2), each change read back; one that does not read back ends the motor for the run.
 *
 * What iOS plays on the motor arrives as a waveform (the audio plug-in's actuator, /dev/hakios-haptic: samples of one
 * channel), as on the POCO (drivers/haptic/hakios.haptic.c, whose rules this follows): a write that is not silence keeps
 * the motor going for 30 ms; its loudness is the strength. A run of it is cut at a second. Facts from the phone's
 * device tree (/haptic, the PWM and its pins) and the S9 kernel's max77705_haptic.c, sec_haptic.c and pwm-samsung.c
 * (GPL: read, not copied).
 */
#define TOP                     0x66
#define MAINCTRL1               0x02
#define MCONFIG                 0x10
#define PWM_PA                  EXYNOS_PWM_PA
#define PWM_TCFG0               0x00
#define PWM_TCFG1               0x04
#define PWM_TCON                0x08
#define PWM_TCNTB1              0x18
#define PWM_TCMPB1              0x1C
#define TCON1_START             (1u << 8)
#define TCON1_UPDATE            (1u << 9)
#define TCON1_INVERT            (1u << 10)
#define TCON1_RELOAD            (1u << 11)
#define PWM_PERIOD_NS           48828u
#define PWM_MAX_DUTY_NS         34179u
#define OSC_HZ                  26000000u
#define CMU_PERIC0_PA           EXYNOS_CMU_PERIC0_PA
#define GPIO_PERIC0_PA          EXYNOS_GPIO_PERIC0_PA
#define GPP3                    0x060           /* pin 1: the PWM's output (function 2) */
#define GPG1                    0x0A0           /* pin 7: the motor's boost, an output held high */
#define HAPTIC_ACTIVE_US        30000
#define HAPTIC_SILENT_BELOW     256
#define HAPTIC_MAX_MS           1000

static vm_offset_t pwm, peric0_gpio, peric0_cmu;
static uint32_t pwm_count;                      /* the period in the PWM's ticks */
static bool motor_broken, motor_on;
static volatile uint64_t heard_us;
static volatile uint32_t heard_peak;
static uint64_t motor_since_us;
static unsigned buzzes, capped;

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* a change of the haptic block's own bits, read back */
static bool
motor_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0, back = 0, buf[2];
	bool ok;

	if (motor_broken || !((reg == MAINCTRL1 && (mask & ~0x80) == 0) || (reg == MCONFIG && (mask & ~0xC2) == 0))) {
		return false;
	}
	lck_mtx_lock(bus_lock);
	ok = hakios_hsi2c_read_reg(&bus, TOP, reg, &now, 1) == 0;
	if (ok) {
		buf[0] = reg;
		buf[1] = (uint8_t)((now & ~mask) | (value & mask));
		ok = hakios_hsi2c_message(&bus, TOP, false, buf, 2, true) == 0 &&
		    hakios_hsi2c_read_reg(&bus, TOP, reg, &back, 1) == 0 && back == buf[1];
	}
	lck_mtx_unlock(bus_lock);
	if (!ok) {
		say("max77705: haptic: register %#x did not take %#x (read %#x, then %#x): the motor stays still for this run\n",
		    reg, value & mask, now, back);
		motor_broken = true;
	}
	return ok;
}

static void
pwm_duty(uint32_t strength)                     /* 0 (50 %, still) to 10000 (70 %) */
{
	uint32_t duty_ns = PWM_PERIOD_NS / 2 + (PWM_MAX_DUTY_NS - PWM_PERIOD_NS / 2) * strength / 10000;
	uint32_t duty = (uint32_t)(((uint64_t)duty_ns * pwm_count + PWM_PERIOD_NS / 2) / PWM_PERIOD_NS);

	*(volatile uint32_t *)(pwm + PWM_TCNTB1) = pwm_count - 1;
	*(volatile uint32_t *)(pwm + PWM_TCMPB1) = pwm_count - duty - 1;
}

static void
motor_go(uint32_t strength)
{
	volatile uint32_t *tcon = (volatile uint32_t *)(pwm + PWM_TCON);

	pwm_duty(strength);
	if (!motor_on) {
		*tcon = (*tcon & ~TCON1_START) | TCON1_UPDATE | TCON1_INVERT;
		*tcon = (*tcon & ~TCON1_UPDATE) | TCON1_START | TCON1_RELOAD;
		if (!motor_bits(MCONFIG, 0xC0, 0xC0)) {
			*tcon &= ~(TCON1_START | TCON1_RELOAD);
			return;
		}
		motor_on = true;
		motor_since_us = now_us();
		buzzes++;
	}
}

static void
motor_still(void)
{
	volatile uint32_t *tcon = (volatile uint32_t *)(pwm + PWM_TCON);

	if (!motor_on) {
		return;
	}
	(void)motor_bits(MCONFIG, 0xC0, 0x00);
	pwm_duty(0);
	*tcon &= ~(TCON1_START | TCON1_RELOAD);
	motor_on = false;
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		bytes = bytes > (user_ssize_t)sizeof(samples) ? (user_ssize_t)sizeof(samples) : bytes & ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		if (peak > HAPTIC_SILENT_BELOW) {
			heard_us = now_us();
			heard_peak = peak;
		}
	}
	return error;
}

static bool haptic_node_made;

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

/*
 * hakios_dev_strict=1 (drivers/core/hakios.platform.c): /dev/hakios-haptic is opened only by root and by
 * HakiosAudio.driver in the audio server (audio_plugin.c)
 */
static const char *const haptic_open_programs[] = { "/usr/sbin/mediaserverd", "/usr/libexec/audiomxd", NULL };

static int
haptic_open_strict(dev_t dev, int flags, int type, struct proc *p)
{
	if (hakios_dev_open_check("/dev/hakios-haptic", haptic_open_programs) != 0) {
		return EPERM;
	}
	return haptic_open(dev, flags, type, p);
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open_strict, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
haptic_thread(__unused void *arg, __unused wait_result_t result)
{
	for (unsigned tick = 1;; tick++) {
		uint64_t now = now_us();
		bool wanted = heard_us != 0 && now - heard_us < HAPTIC_ACTIVE_US;

		if (wanted && motor_on && now - motor_since_us >= HAPTIC_MAX_MS * 1000ull) {
			capped++;
			motor_still();
			heard_us = 0;           /* (until iOS plays anew) */
		} else if (wanted && !motor_broken) {
			/* the strength from iOS's loudness, with a floor a tap can be felt at */
			uint32_t strength = heard_peak * 10000u / 32768u;

			motor_go(strength < 3000 ? 3000 : strength);
		} else {
			motor_still();
		}
		if (tick % (60 * 500) == 0) {
			say("max77705: haptic: a minute: %u buzzes, %u cut at %u ms\n", buzzes, capped, HAPTIC_MAX_MS);
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
}

/* the PWM, its pins and the haptic block set up; /dev/hakios-haptic made. In the battery's thread, after the bus. */
static void
haptic_start(void)
{
	uint32_t tcfg0, tcfg1, prescale, divider, tick_hz;
	thread_t thread;
	int major;

	pwm = ml_io_map(PWM_PA, 0x1000);
	peric0_gpio = ml_io_map(GPIO_PERIC0_PA, 0x1000);
	peric0_cmu = ml_io_map(CMU_PERIC0_PA, 0x4000);
	if (!pwm || !peric0_gpio || !peric0_cmu) {
		say("max77705: haptic: the PWM or its pins could not be mapped\n");
		return;
	}
	*(volatile uint32_t *)(peric0_cmu + 0x2044) |= (1u << 20) | (1u << 21);
	*(volatile uint32_t *)(peric0_cmu + 0x3010) |= 3u;
	{
		volatile uint32_t *con = (volatile uint32_t *)(peric0_gpio + GPP3);

		*con = (*con & ~(0xfu << 4)) | (2u << 4);                       /* gpp3-1: TOUT1 */
		con = (volatile uint32_t *)(peric0_gpio + GPG1);
		*(volatile uint32_t *)(peric0_gpio + GPG1 + 4) |= 1u << 7;      /* gpg1-7 high ... */
		*con = (*con & ~(0xfu << 28)) | (1u << 28);                     /* ... an output */
	}
	/* channel 1's clock as it is set (timers 0 and 1 share the prescaler: left alone), its own divider 1 */
	tcfg0 = *(volatile uint32_t *)(pwm + PWM_TCFG0);
	*(volatile uint32_t *)(pwm + PWM_TCFG1) &= ~(0xfu << 4);
	tcfg1 = *(volatile uint32_t *)(pwm + PWM_TCFG1);
	prescale = (tcfg0 & 0xff) + 1;
	divider = 1u << ((tcfg1 >> 4) & 0x7);
	tick_hz = OSC_HZ / prescale / divider;
	pwm_count = (uint32_t)(((uint64_t)PWM_PERIOD_NS * tick_hz + 500000000ull) / 1000000000ull);
	if (pwm_count < 16) {
		say("max77705: haptic: the PWM's tick (%u Hz) is too slow for its period: no motor\n", tick_hz);
		return;
	}
	pwm_duty(0);
	*(volatile uint32_t *)(pwm + PWM_TCON) = (*(volatile uint32_t *)(pwm + PWM_TCON) &
	    ~(TCON1_START | TCON1_RELOAD)) | TCON1_UPDATE | TCON1_INVERT;
	*(volatile uint32_t *)(pwm + PWM_TCON) &= ~TCON1_UPDATE;
	if (!motor_bits(MAINCTRL1, 0x80, 0x80) || !motor_bits(MCONFIG, 0xC2, 0x82)) {
		return;
	}
	if (!haptic_node_made) {
		say("max77705: haptic: /dev/hakios-haptic was not made at start\n");
		return;
	}
	(void)major;
	say("max77705: haptic: the motor for iOS (/dev/hakios-haptic): PWM %u ticks of %u Hz a period, up to %u ms a run\n",
	    pwm_count, tick_hz, HAPTIC_MAX_MS);
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * The charger's two currents, set when a charger is on the USB input (max77705_chg, on by default; max77705_chg=0 leaves the
 * charger exactly as the boot loader left it, as before). With nothing setting them, the MAX77705 stayed at its own input
 * limit: 1 to 2 W from a wall charger (seen 15 November 2026). Only these are written, read back, and written again
 * should the chip lose them (its watchdog, a replug): the input's limit, CNFG_09 [6:0] (mA / 25 - 1), and the fast-charge
 * current, CNFG_02 [5:0] (mA / 50), behind CHGPROT, CNFG_06 [3:2] = 3; and, when the watchdog is on (CNFG_00 bit 4),
 * its clear, CNFG_06 [1:0] = 1, at every poll. The float voltage (CNFG_04) and the mode are never touched.
 * Values: a wall charger or a charging host (BC_STATUS [1:0] 3 or 2) 2000 mA in, 2000 mA into the battery (10 W at 5 V,
 * 0.5 C of the Note9's 4000 mAh; 9 V adaptive charging, for 15 W, is not negotiated here); a plain USB host (1), or no type
 * yet: left as it is. The battery under 0 or over 45 C: 500 mA into it.
 * Facts from Samsung's drivers/battery_v2/max77705_charger.c and include/linux/mfd/max77705-private.h of the S9's kernel
 * (LineageOS android_kernel_samsung_exynos9810): read for the registers and formulas, not copied.
 */
#define CHG_CNFG_02     0xB9
#define CHG_CNFG_09     0xC0
#define CHG_IN_MA       2000
#define CHG_CC_MA       2000
#define CHG_CC_TEMP_MA  500

static bool chg_enabled = true;
static int chg_said;

/* the charger's bits under mask, read back; under the bus's lock */
static bool
chg_bits(uint8_t reg, uint8_t mask, uint8_t value, uint8_t *was)
{
	uint8_t now = 0, back = 0, buf[2];
	bool ok;

	lck_mtx_lock(bus_lock);
	ok = hakios_hsi2c_read_reg(&bus, CHARGER, reg, &now, 1) == 0;
	if (ok && (now & mask) != (value & mask)) {
		buf[0] = reg;
		buf[1] = (uint8_t)((now & ~mask) | (value & mask));
		ok = hakios_hsi2c_message(&bus, CHARGER, false, buf, 2, true) == 0 &&
		    hakios_hsi2c_read_reg(&bus, CHARGER, reg, &back, 1) == 0 && (back & mask) == (value & mask);
	}
	lck_mtx_unlock(bus_lock);
	if (was != NULL) {
		*was = now;
	}
	return ok;
}

/*
 * 15 W from a Samsung fast charger: the AFC 9 V request (max77705_afc=1, the Note9's default from round 30; 0: never), as
 * Samsung's MUIC driver makes it (drivers/muic/max77705-muic-afc.c) and with the rules of Scion's iOS 27 charger guard
 * (scion-kernel drivers/power/max77705/hakios.max77705_charge.h, docs/ios27/CHARGING.md: Samsung's own numbers, stricter
 * where they differ). Asked only on a wall charger (BC_STATUS type 3) seen for two polls, VBUS at 5 V (USBC_STATUS1
 * [7:4] VBADC 2 or less), the battery at 15-39 C and under 98 %, and not tried yet on this cable: the input to 500 mA
 * (pre_afc_input_current), the D+/D- switch open (as Samsung leaves it for a TA), then opcode 0x21 with 0x46 (9 V). The
 * answer (at 0x51: the opcode, the result, VBADC) counts as 9 V only with result 0 and VBADC 5-7 (7.5-10.5 V), read again
 * from USBC_STATUS1; only then Samsung's 9 V limits, 1650 mA in and 2150 mA into the battery. At every poll at 9 V: the
 * battery out of 15-39 C, at 98 %, or VBUS out of 7.5-10.5 V -- the input to 500 mA and 5 V asked for (0x08), and no new
 * request until the cable goes. Anything else: the 5 V limits below.
 */
#define AFC_OP                  0x21    /* COMMAND_AFC_RESULT_READ */
#define AFC_9V                  0x46
#define AFC_5V                  0x08
#define AFC_IN_MA               1650
#define AFC_CC_MA               2150
#define AFC_PRE_MA              500
static bool afc_enabled;
static enum { AFC_IDLE, AFC_ON_9V, AFC_SPENT } afc_state;
static uint32_t afc_dcp_polls, afc_said;

/* one AFC opcode with its byte; the answer's first three bytes when it comes in 1 s (false: none) */
static bool
afc_request(uint8_t byte, uint8_t answer[3])
{
	uint8_t w1[3] = { OPCODE_WRITE, AFC_OP, byte }, w2[2] = { OPCODE_WRITE_END, 0 };
	uint32_t e;

	lck_mtx_lock(bus_lock);
	e = hakios_hsi2c_message(&bus, MUIC, false, w1, 3, true);
	if (e == 0) {
		e = hakios_hsi2c_message(&bus, MUIC, false, w2, 2, true);
	}
	lck_mtx_unlock(bus_lock);
	for (int i = 0; e == 0 && i < 20; i++) {
		delay_for_interval(50, NSEC_PER_MSEC);
		if (locked_read(MUIC, OPCODE_READ, answer, 3) == 0 && answer[0] == AFC_OP) {
			return true;
		}
	}
	return false;
}

static uint8_t
afc_vbadc(void)
{
	uint8_t st1 = 0;

	return read8(MUIC, MUIC_USBC_STATUS1, &st1) == 0 ? (uint8_t)((st1 >> 4) & 0xf) : 0xff;
}

static void
charger_tune(const struct hakios_battery_state *b)
{
	uint8_t bc = 0, cnfg00 = 0, in_now = 0, cc_now = 0, vbadc;
	uint32_t in_ma, cc_ma;
	bool temp_fast;

	if (!b->external) {
		if (afc_state != AFC_IDLE || afc_dcp_polls != 0) {
			afc_state = AFC_IDLE;           /* (the cable gone: a new one may be asked again) */
			afc_dcp_polls = 0;
		}
		return;
	}
	if (!chg_enabled) {
		return;
	}
	if (read8(CHARGER, CHG_CNFG_00, &cnfg00) == 0 && (cnfg00 & 0x10)) {
		(void)chg_bits(CHG_CNFG_06, 0x0C, 0x0C, NULL);
		(void)chg_bits(CHG_CNFG_06, 0x03, 0x01, NULL);  /* the watchdog's clear (self-clearing) */
	}
	if (read8(MUIC, MUIC_BC_STATUS, &bc) != 0 || ((bc & 3) != 2 && (bc & 3) != 3)) {
		if (chg_said == 0) {
			chg_said = 1;
			say("max77705: charger: the input's type %u (BC_STATUS %#x): its currents left as they are\n", bc & 3, bc);
		}
		return;
	}
	vbadc = afc_vbadc();
	temp_fast = b->temperature_hundredths > 1500 && b->temperature_hundredths <= 3900;
	afc_dcp_polls = (bc & 3) == 3 ? afc_dcp_polls + 1 : 0;
	if (afc_state == AFC_ON_9V && (!temp_fast || b->percent_hundredths >= 9800 || vbadc < 5 || vbadc > 7 ||
	    (bc & 3) != 3)) {
		uint8_t answer[3] = { 0, 0, 0 };

		(void)chg_bits(CHG_CNFG_06, 0x0C, 0x0C, NULL);
		(void)chg_bits(CHG_CNFG_09, 0x7F, (uint8_t)(AFC_PRE_MA / 25 - 1), NULL);
		bool got = afc_request(AFC_5V, answer);
		say("max77705: AFC: back to 5 V (battery %d.%02d C, %d.%02d%%, VBADC %u, type %u): the answer %s %x %x %x\n",
		    b->temperature_hundredths / 100, (b->temperature_hundredths < 0 ? -b->temperature_hundredths : b->temperature_hundredths) % 100,
		    b->percent_hundredths / 100, b->percent_hundredths % 100, vbadc, bc & 3, got ? "came:" : "did NOT come:",
		    answer[0], answer[1], answer[2]);
		afc_state = AFC_SPENT;
		vbadc = afc_vbadc();
	} else if (afc_enabled && afc_state == AFC_IDLE && (bc & 3) == 3 && afc_dcp_polls >= 2 && vbadc <= 2 && temp_fast &&
	    b->percent_hundredths < 9800) {
		uint8_t answer[3] = { 0, 0, 0 };
		uint8_t now_vbadc;
		bool got;

		say("max77705: AFC: a wall charger, the battery at %d.%02d C and %d.%02d%%: the input to %u mA, the switch open, "
		    "9 V asked for\n", b->temperature_hundredths / 100, b->temperature_hundredths % 100, b->percent_hundredths / 100,
		    b->percent_hundredths % 100, AFC_PRE_MA);
		(void)chg_bits(CHG_CNFG_06, 0x0C, 0x0C, NULL);
		(void)chg_bits(CHG_CNFG_09, 0x7F, (uint8_t)(AFC_PRE_MA / 25 - 1), NULL);
		(void)usbsw_write(COM_OPEN);
		delay_for_interval(500, NSEC_PER_MSEC);         /* (Samsung's 500 ms, "Maxim's request") */
		got = afc_request(AFC_9V, answer);
		delay_for_interval(200, NSEC_PER_MSEC);
		now_vbadc = afc_vbadc();
		if (got && answer[1] == 0 && answer[2] >= 5 && answer[2] <= 7 && now_vbadc >= 5 && now_vbadc <= 7) {
			afc_state = AFC_ON_9V;
			say("max77705: AFC: 9 V taken (answer %x %x %x, VBADC %u): %u mA in, %u mA into the battery\n", answer[0],
			    answer[1], answer[2], now_vbadc, AFC_IN_MA, AFC_CC_MA);
		} else {
			afc_state = AFC_SPENT;
			say("max77705: AFC: no 9 V (the answer %s %x %x %x, VBADC %u): 5 V limits, not asked again on this cable\n",
			    got ? "came:" : "did NOT come:", answer[0], answer[1], answer[2], now_vbadc);
			if (now_vbadc > 2) {
				(void)afc_request(AFC_5V, answer);
			}
		}
		vbadc = now_vbadc;
	}
	if (afc_state == AFC_ON_9V) {
		in_ma = AFC_IN_MA;
		cc_ma = AFC_CC_MA;
	} else if (vbadc != 0xff && vbadc > 2) {
		in_ma = AFC_PRE_MA;             /* (VBUS above 5.5 V outside a 9 V session: held low, as the guard does) */
		cc_ma = CHG_CC_MA;
	} else {
		in_ma = CHG_IN_MA;
		cc_ma = CHG_CC_MA;
	}
	if (b->temperature_hundredths < 0 || b->temperature_hundredths > 4500) {
		cc_ma = CHG_CC_TEMP_MA;
	}
	uint8_t in_code = (uint8_t)(in_ma / 25 - 1), cc_code = (uint8_t)(cc_ma / 50);

	if (read8(CHARGER, CHG_CNFG_09, &in_now) != 0 || read8(CHARGER, CHG_CNFG_02, &cc_now) != 0) {
		return;
	}
	if ((in_now & 0x7F) == in_code && (cc_now & 0x3F) == cc_code) {
		return;
	}
	bool ok = chg_bits(CHG_CNFG_06, 0x0C, 0x0C, NULL) && chg_bits(CHG_CNFG_09, 0x7F, in_code, NULL) &&
	    chg_bits(CHG_CNFG_02, 0x3F, cc_code, NULL);

	if (chg_said < 12 || afc_said < 8) {
		chg_said = chg_said < 2 ? 2 : chg_said + 1;
		afc_said++;
		say("max77705: charger: type %u%s: input limit %u -> %u mA, fast charge %u -> %u mA (battery %d.%02d C, watchdog %s): %s\n",
		    bc & 3, afc_state == AFC_ON_9V ? " at 9 V" : "", (in_now & 0x7F) <= 3 ? 100 : ((in_now & 0x7F) + 1) * 25, in_ma,
		    (cc_now & 0x3F) <= 2 ? 100 : (cc_now & 0x3F) * 50, cc_ma, b->temperature_hundredths / 100,
		    (b->temperature_hundredths < 0 ? -b->temperature_hundredths : b->temperature_hundredths) % 100,
		    (cnfg00 & 0x10) ? "on, cleared every poll" : "off", ok ? "written" : "NOT TAKEN");
	}
}

/*
 * max77705_diag=1: the charger's registers 0xB2-0xC5, raw, in one line, and what they say (only reads; see the top of
 * this file). A register that does not answer reads "--".
 */
static uint32_t chg_diag;
static uint32_t fg_ticks;               /* max77705_fg_s, in 5 s polls (0: off) */

static void
chg_diag_say(const char *when)
{
	uint8_t r[CHG_DIAG_LAST - CHG_DIAG_FIRST + 1];
	bool got[sizeof(r)];
	char line[3 * sizeof(r) + 1];
	size_t at = 0;

	for (unsigned i = 0; i < sizeof(r); i++) {
		got[i] = read8(CHARGER, (uint8_t)(CHG_DIAG_FIRST + i), &r[i]) == 0;
		at += (size_t)snprintf(line + at, sizeof(line) - at, got[i] ? " %02x" : " --", r[i]);
	}
	say("max77705: charger %s: 0xb2-0xc5:%s\n", when, line);
#define CHG_R(reg)      (got[(reg) - CHG_DIAG_FIRST] ? (int)r[(reg) - CHG_DIAG_FIRST] : -1)
	say("max77705: charger %s: CNFG_00 mode %#x WDTEN %d; CHGIN_DTLS %d; CHG_DTLS %#x BAT_DTLS %d; BYP_DTLS %#x; "
	    "fast charge %d mA; input limit code %#x (CNFG_09)\n", when,
	    CHG_R(0xB7) < 0 ? -1 : CHG_R(0xB7) & 0xf, CHG_R(0xB7) < 0 ? -1 : CHG_R(0xB7) >> 4 & 1,
	    CHG_R(0xB3) < 0 ? -1 : CHG_R(0xB3) >> 5 & 3,
	    CHG_R(0xB4) < 0 ? -1 : CHG_R(0xB4) & 0xf, CHG_R(0xB4) < 0 ? -1 : CHG_R(0xB4) >> 4 & 7,
	    CHG_R(0xB5) < 0 ? -1 : CHG_R(0xB5) & 0xf,
	    CHG_R(0xB9) < 0 ? -1 : (CHG_R(0xB9) & 0x3f) * 50, CHG_R(0xC0) < 0 ? -1 : CHG_R(0xC0) & 0x7f);
#undef CHG_R
}

/*
 * max77705_wdt=1: the charger's watchdog kicked, only while it is on (see the top of this file). The one write this driver
 * makes to the charger: CNFG_06's two WDTCLR bits, the rest of it as read.
 */
static uint32_t chg_wdt, wdt_kicks, wdt_skipped, wdt_failed;

static uint32_t
charger_write(uint8_t reg, uint8_t value)
{
	uint8_t b[2] = { reg, value };
	uint32_t error;

	lck_mtx_lock(bus_lock);
	error = hakios_hsi2c_message(&bus, CHARGER, false, b, 2, true);
	lck_mtx_unlock(bus_lock);
	return error;
}

static void
chg_wdt_kick(void)
{
	uint8_t cnfg00, cnfg06, kick;
	uint32_t error;

	if (read8(CHARGER, CHG_CNFG_00, &cnfg00) != 0 || read8(CHARGER, CHG_CNFG_06, &cnfg06) != 0) {
		wdt_failed++;
		return;
	}
	if (!(cnfg00 & CHG_WDTEN)) {
		if (wdt_skipped++ == 0) {
			say("max77705: wdt: CNFG_00 %#x: the watchdog is off, nothing to kick (asked again every 30 s)\n", cnfg00);
		}
		return;
	}
	kick = (uint8_t)((cnfg06 & ~CHG_WDTCLR_MASK) | CHG_WDTCLR);
	error = charger_write(CHG_CNFG_06, kick);
	if (error != 0) {
		wdt_failed++;
	} else {
		wdt_kicks++;
	}
	if (wdt_kicks + wdt_failed == 1) {
		say("max77705: wdt: CNFG_00 %#x (WDTEN on); CNFG_06 %#x written %#x: %s (%x)\n", cnfg00, cnfg06, kick,
		    error ? "FAILED" : "kicked", error);
	}
}

/* max77705_fg_s=S: the gauge's short line (only reads) */
static void
fg_short_say(void)
{
	uint16_t soc, cap, qh;
	int16_t current, avg;
	uint64_t ns;

	if (read16(GAUGE, FG_REPSOC, &soc) || read16(GAUGE, FG_REPCAP, &cap) || read16(GAUGE, FG_CURRENT, (uint16_t *)&current) ||
	    read16(GAUGE, FG_AVGCURRENT, (uint16_t *)&avg) || read16(GAUGE, FG_QH, &qh)) {
		say("max77705: fg: a read failed (%x)\n", bus.last_error);
		return;
	}
	/* 312.5 uA a count for the currents; QH raw (the gauge's own coulomb count, its scale not taken as known here) */
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	say("max77705: fg: t %llu s, %u.%02u%%, %u mAh, current %d mA, avg %d mA, QH %u\n", ns / NSEC_PER_SEC,
	    (unsigned)soc * 100 / 256 / 100, (unsigned)soc * 100 / 256 % 100, cap, (int)current * 3125 / 10000,
	    (int)avg * 3125 / 10000, qh);
}

static void
battery_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, failed = 0;
	struct hakios_battery_state b;
	thread_t thread;
	uint8_t rev;

	say("max77705: step 1: map the HSI2C, the CMGP clocks and USI03's mode word\n");
	if (!hakios_hsi2c_map(&bus, say) || ++step >= steps) {
		goto out;
	}
	say("max77705: step 2: the clocks of CMGP's USI03 on, USI03 to I2C; the ADC's clocks on, the ADC reset\n");
	hakios_hsi2c_power(&bus);
	{
		/* the bus's pins: GPM14-0 (SCL) and GPM15-0 (SDA) of CMGP's pin controller (0x14220000; banks at +0x180/+0x1A0,
		 * CON +0, DAT +4, PUD +8): function 2, no pull (the device tree's hsi2c40-bus) */
		vm_offset_t g = ml_io_map(0x14220000, 0x1000);

		if (g != 0) {
			for (uint32_t b = 0x180; b <= 0x1A0; b += 0x20) {
				volatile uint32_t *con = (volatile uint32_t *)(g + b), *pud = (volatile uint32_t *)(g + b + 8);
				uint32_t c0 = *con, p0 = *pud;

				*con = (c0 & ~0xfu) | 2;
				*pud = p0 & ~0xfu;
				say("max77705: pins: bank +%#x CON %x -> %x, PUD %x -> %x, DAT %x\n", b, c0, *con, p0, *pud,
				    *(volatile uint32_t *)(g + b + 4));
			}
		}
	}
	adc_init();
	if (++step >= steps) {
		goto out;
	}
	say("max77705: step 3: the bus reset, at %u kHz from %u MHz\n", bus.bus_hz / 1000, bus.ipclk_mhz);
	hakios_hsi2c_reset(&bus);
	if (++step >= steps) {
		goto out;
	}
	say("max77705: step 4: the chip, the gauge, the charger\n");
	/* (a first answer missed is not the end: step 5 goes on asking, and reports once one comes) */
	if (read8(PMIC, 0x01, &rev) != 0) {
		say("max77705: no answer at 0x%x (%x)\n", PMIC, bus.last_error);
	} else {
		say("max77705: version %u, revision %u\n", rev >> 3, rev & 7);
	}
	if (!battery_read(&b)) {
		say("max77705: the gauge or the charger did not answer (%x)\n", bus.last_error);
	} else {
		say_state("now", &b);
	}
	(void)PE_parse_boot_argn("max77705_diag", &chg_diag, sizeof(chg_diag));
	if (chg_diag) {
		chg_diag_say("now");
	}
	{
		/*
		 * max77705_usbsw=1: the USB-C switch's D+/D- to the AP's USB (Samsung's com_to_usb_ap: the MUIC at 0x25, opcode
		 * COMMAND_CONTROL1_WRITE 0x06 with COM_USB 0x09, written to OPCODE_WRITE 0x21, then OPCODE_WRITE_END 0x41 = 0).
		 */
		uint32_t usbsw = 0;

		if (PE_parse_boot_argn("max77705_usbsw", &usbsw, sizeof(usbsw)) && usbsw != 0) {
			uint8_t w1[3] = { 0x21, 0x06, 0x09 }, w2[2] = { 0x41, 0x00 };
			uint32_t e1 = hakios_hsi2c_message(&bus, 0x25, false, w1, 3, true);
			uint32_t e2 = e1 ? e1 : hakios_hsi2c_message(&bus, 0x25, false, w2, 2, true);

			say("max77705: usb switch: CONTROL1 = 0x09 (D+/D- to the AP's USB): %s (%x)\n", e2 ? "FAILED" : "written", e2);
			if (kernel_thread_start(usbsw_thread, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
				usbsw_running = true;
			}
		}
	}
	if (++step >= steps) {
		goto out;
	}
	{
		uint32_t haptic = 0;

		if (PE_parse_boot_argn("max77705_haptic", &haptic, sizeof(haptic)) && haptic != 0) {
			haptic_start();
		}
	}
	say("max77705: step 5: reporting every %u s\n", POLL_MS / 1000);
	{
		uint32_t fg_s = 0;

		(void)PE_parse_boot_argn("max77705_fg_s", &fg_s, sizeof(fg_s));
		fg_ticks = fg_s >= 5 && fg_s <= 600 ? (fg_s + POLL_MS / 2000) / (POLL_MS / 1000) : 0;
		(void)PE_parse_boot_argn("max77705_wdt", &chg_wdt, sizeof(chg_wdt));
		if (chg_wdt) {
			say("max77705: wdt: the charger's watchdog kicked every %u s while it is on (max77705_wdt=1)\n",
			    WDT_KICK_POLLS * (POLL_MS / 1000));
		}
		if (fg_ticks != 0) {
			say("max77705: fg: a short gauge line every %u s (max77705_fg_s)\n", fg_ticks * (POLL_MS / 1000));
		}
	}
	for (unsigned tick = 0;; tick++) {
		if (chg_wdt && tick % WDT_KICK_POLLS == 0) {
			chg_wdt_kick();
		}
		if (battery_read(&b)) {
			failed = 0;
			hakios_battery_report(&b);
			charger_tune(&b);
			power_note(&b);
			if (tick % 60 == 0) {
				say_state("every 5 minutes", &b);
				if (chg_diag) {
					chg_diag_say("every 5 minutes");
				}
				if (chg_wdt) {
					say("max77705: wdt: %u kicks, %u with the watchdog off, %u failed\n", wdt_kicks, wdt_skipped, wdt_failed);
				}
			}
			if (fg_ticks != 0 && tick % fg_ticks == 0) {
				fg_short_say();
			}
		} else if (++failed == 10) {
			say("max77705: 10 reads in a row have failed (%x); still trying\n", bus.last_error);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (steps < 5) {
		say("max77705: stopping here (max77705_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_max77705_init(void)
{
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	if (!PE_parse_boot_argn("max77705", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("max77705_steps", &steps, sizeof(steps));
	{
		uint32_t chg = 1;

		PE_parse_boot_argn("max77705_chg", &chg, sizeof(chg));
		chg_enabled = chg != 0;
	}
	{
		/* max77705_afc: the 9 V request (above); the Note9's default 1 (its boot line has no room), elsewhere 0 */
		char platform[24] = "";
		uint32_t afc = 0;

		(void)PE_parse_boot_arg_str("loader_platform", platform, sizeof(platform));
		afc = strcmp(platform, "crownlte") == 0;
		PE_parse_boot_argn("max77705_afc", &afc, sizeof(afc));
		afc_enabled = afc != 0;
	}
	PE_parse_boot_argn("max77705_ipclk", &bus.ipclk_mhz, sizeof(bus.ipclk_mhz));
	if (steps == 0 || bus.ipclk_mhz < 26 || bus.ipclk_mhz > 400) {
		return;
	}
	{
		/* which phone's battery (battery_boards): /arm-io's device_type, the loader's platform; the S9's if no other's */
		DTEntry node;
		void const *prop;
		unsigned int size;

		if (SecureDTLookupEntry(NULL, "/arm-io", &node) == kSuccess &&
		    SecureDTGetProperty(node, "device_type", &prop, &size) == kSuccess && size > 0) {
			for (unsigned i = 1; i < sizeof(battery_boards) / sizeof(battery_boards[0]); i++) {
				if (strncmp(prop, battery_boards[i].platform, size) == 0) {
					board = &battery_boards[i];
					say("max77705: the %s's battery: %u mAh, its own thermistor table (written, not yet seen on "
					    "this phone)\n", board->platform, board->design_mah);
				}
			}
		}
	}
	bus_lock = lck_mtx_alloc_init(lck_grp_alloc_init("max77705", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	{
		/* /dev/hakios-haptic made now, before user space starts: the audio server opens it once, at its first sound,
		 * and keeps "not there" for good if it was not there yet (seen on the phone: the motor came up later, in the
		 * battery's thread). Writes before the motor is ready are only listened to. */
		uint32_t haptic = 0;

		if (PE_parse_boot_argn("max77705_haptic", &haptic, sizeof(haptic)) && haptic != 0) {
			int major = cdevsw_add(-1, &haptic_cdevsw);

			haptic_node_made = major >= 0 &&
			    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") != NULL;
		}
	}
	if (kernel_thread_start(battery_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
