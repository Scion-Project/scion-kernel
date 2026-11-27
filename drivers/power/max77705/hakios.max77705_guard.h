/*
 * The Galaxy S9's charger guard for the iOS 16.5 kernel (max77705_chg=, max77705_siop=): included by hakios.max77705.c,
 * after its bus functions (read8, charger_write, chg_wdt_kick, say, last_adc), whose statics it uses. The rules are
 * hakios.max77705_charge.h's (Samsung's numbers for the S9; tools/max77705-charge-test.sh checks them on a computer);
 * this file reads the chip, asks the rules, and writes what they say. docs/devices/starlte-charging.md is its guide.
 */
#ifndef HAKIOS_MAX77705_GUARD_H
#define HAKIOS_MAX77705_GUARD_H

#include "hakios.max77705_charge.h"

/*
 * max77705_chg=N: the charger guard, off by default (1 says, 2 writes); Samsung's charging rules for the S9 (hakios.max77705_charge.h, the
 * rules the iOS 27 kernel's guard follows, docs/ios27/CHARGING.md; this is the iOS 16.5 kernel's use of them,
 * docs/devices/starlte-charging.md). Galaxy S9 (starlte) only: on the S9+ and the Note9 it says so and does nothing.
 *   1  read only: every poll the charger, the MUIC's charger type and the battery's thermistor are read, and the log says
 *      what would be written ("would: ..."); nothing is written
 *   2  the guard: as 1, and the charger is told, at 5 V only (no AFC: no MUIC opcode is sent):
 *        CNFG_09 (0xC0) [6:0]  the input limit, at most Samsung's for the type (SDP 475, CDP 1500, DCP 1800, unknown
 *                              500 mA), bit 7 kept as read
 *        CNFG_02 (0xB9) [5:0]  the charge current, at most Samsung's (SDP 550, CDP 1500, DCP 2100, unknown 500 mA;
 *                              700 mA at 15.0 C and down), [7:6] kept as read
 *        CNFG_00 (0xB7) bit 0  charging off at 41.0 C and up (on again at 39.0 C), at 5.0 C and down (on at 10.0 C),
 *                              without a thermistor reading, when full (on at 4280 mV or 98 %), after the session's time;
 *                              latched off at 45.0 C, on a chip fault, a float above 4350 mV or a read-back that differs.
 *                              Only while CNFG_00's mode is 4 or 5 (buck, or buck and charger): never in OTG or off
 *        CNFG_06 (0xBD) [1:0]  WDTCLR = 01, the charger's watchdog kicked every poll while it is on (as max77705_wdt=1)
 *      each written only when it differs, each read back; nothing written while CNFG_06's CHGPROT ([3:2]) is not 11
 *      (unlocked), nothing at all without VBUS or when a read failed. Never written: the float voltage, the timers,
 *      CHGPROT, any other register, the gauge, the MUIC, anything non-volatile.
 * max77705_siop=0: the guard's in-use limit (SIOP) off; it is on with the guard unless this says 0.
 * On top of the rules, only ever lower: max77705_siop=1 (the default with the guard; 0 turns it off) Samsung's "in use"
 * limit while the panel is lit or its level is not known (1200 mA in, 1000 mA charge); and a computer on the USB link (the
 * gadget configured) holds the limits at the CDP's, 1500/1500 mA, whatever the MUIC called the port.
 */
static uint32_t chg_mode, chg_siop = 1, chg_writes, chg_wfail, chg_differs, chg_refused;
static struct chg_state chg_st;
static struct chg_out chg_last = { .why = WHY_NO_VBUS };
static bool chg_last_siop, chg_last_host, chg_said, chg_say_would;
static enum chg_type chg_last_type = CHG_TYPES;

extern int hakios_panel_level(void);            /* hakios.backlight.cpp: 0-255, -1 not known */
extern bool hakios_usb_gadget_host(void);       /* hakios.dwc3.c: a host configured the link (the stub: no) */

/* one register written (only the bits of mask changed), read back; false if it did not read back as written */
static bool
chg_set(const char *what, uint8_t reg, uint8_t now, uint8_t mask, uint8_t bits)
{
	uint8_t want = (uint8_t)((now & ~mask) | (bits & mask)), back = 0;
	uint32_t e;

	if (want == now) {
		return true;
	}
	if (chg_mode < 2) {
		if (chg_say_would) {    /* (with each change of the summary line, not every poll) */
			say("max77705: chg: would: %s: %#x -> %#x\n", what, now, want);
		}
		return true;
	}
	e = charger_write(reg, want);
	if (e == 0) {
		e = read8(CHARGER, reg, &back);
	}
	chg_writes++;
	if (e != 0) {
		chg_wfail++;
		say("max77705: chg: %s: %#x -> %#x: A WRITE OR ITS READ-BACK FAILED (%x)\n", what, now, want, e);
		return false;
	}
	if (!chg_readback_ok(want, back, mask)) {
		chg_differs++;
		say("max77705: chg: %s: %#x -> %#x, read back %#x: DIFFERS\n", what, now, want, back);
		return false;
	}
	say("max77705: chg: %s: %#x -> %#x, read back %#x\n", what, now, want, back);
	return true;
}

static void
chg_guard(const struct hakios_battery_state *b, bool read_ok)
{
	struct chg_in in = { .read_ok = read_ok };
	struct chg_out o;
	uint8_t st1 = 0, cnfg06 = 0, mode;
	uint64_t ns;
	int level;
	bool siop, host, ok;

	if (chg_mode == 0) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	in.now_s = (uint32_t)(ns / NSEC_PER_SEC);
	if (read_ok) {
		in.read_ok = read8(MUIC, MUIC_BC_STATUS, &in.bc) == 0 && read8(MUIC, MUIC_USBC_STATUS1, &st1) == 0 &&
		    read8(CHARGER, CHG_REG_DTLS_00, &in.dtls00) == 0 && read8(CHARGER, CHG_REG_DTLS_01, &in.dtls01) == 0 &&
		    read8(CHARGER, CHG_REG_CNFG_00, &in.cnfg00) == 0 && read8(CHARGER, CHG_REG_CNFG_02, &in.cnfg02) == 0 &&
		    read8(CHARGER, CHG_REG_CNFG_04, &in.cnfg04) == 0 && read8(CHARGER, CHG_REG_CNFG_06, &cnfg06) == 0 &&
		    read8(CHARGER, CHG_REG_CNFG_09, &in.cnfg09) == 0;
	}
	in.vbadc = st1 >> 4;
	in.temp_ok = last_adc >= 0;     /* battery_temperature() gives 25 C when the ADC does not answer: not a reading */
	in.temp_dc = b->temperature_hundredths / 10;
	in.chg_temp_ok = false;         /* (the charger's thermistor only matters for 9 V, which this guard never asks) */
	in.soc = (uint32_t)(b->percent_hundredths < 0 ? 0 : b->percent_hundredths / 100);
	in.vcell_mv = (uint32_t)(b->voltage_uv < 0 ? 0 : b->voltage_uv / 1000);
	in.afc_wanted = false;
	o = chg_step(&chg_st, &in);
	level = hakios_panel_level();
	siop = chg_siop != 0 && level != 0;
	host = hakios_usb_gadget_host();
	if (o.enable && siop) {
		chg_siop_cap(&o, chg_st.type);
	}
	if (o.enable && host) {
		chg_host_cap(&o);
	}
	o.ask_9v = false;               /* 5 V only (no AFC in this kernel yet) */
	o.ask_5v = false;

	chg_say_would = false;
	if (!chg_said || chg_st.type != chg_last_type || o.why != chg_last.why || o.enable != chg_last.enable ||
	    o.input_ma != chg_last.input_ma || o.charge_ma != chg_last.charge_ma || siop != chg_last_siop ||
	    host != chg_last_host) {
		say("max77705: chg: %s; %s; %s, input %u mA, charge %u mA%s%s; battery %d.%d C%s, %u %%, %u mV; BC_STATUS %#x "
		    "VBADC %u; CNFG_00 %#x _02 %#x _04 %#x _06 %#x _09 %#x; DTLS %#x %#x\n",
		    chg_limits[chg_st.type < CHG_TYPES ? chg_st.type : CHG_NONE].name, chg_why_name(o.why),
		    o.enable ? "on" : "off", o.input_ma, o.charge_ma, siop ? ", in use (SIOP)" : "", host ? ", a computer on the link" : "",
		    in.temp_dc / 10, (in.temp_dc < 0 ? -in.temp_dc : in.temp_dc) % 10, in.temp_ok ? "" : " (NOT READ)", in.soc,
		    in.vcell_mv, in.bc, in.vbadc, in.cnfg00, in.cnfg02, in.cnfg04, cnfg06, in.cnfg09, in.dtls00, in.dtls01);
		chg_said = true;
		chg_last = o;
		chg_last_type = chg_st.type;
		chg_last_siop = siop;
		chg_last_host = host;
		chg_say_would = true;
	}
	/* nothing written without VBUS, after a failed read, or with the charger's settings locked */
	if (!in.read_ok || o.why == WHY_NO_VBUS) {
		return;
	}
	if ((cnfg06 & CHG_CNFG06_CHGPROT_MASK) != CHG_CNFG06_CHGPROT_MASK) {
		if (chg_refused++ == 0) {
			say("max77705: chg: CNFG_06 %#x: the charger's settings are locked (CHGPROT); nothing written\n", cnfg06);
		}
		return;
	}
	mode = in.cnfg00 & 0x0Fu;
	ok = true;
	if (o.enable && o.input_ma != 0) {
		ok = chg_set("input limit (CNFG_09)", CHG_REG_CNFG_09, in.cnfg09, CHG_CNFG09_LIM_MASK, chg_input_code(o.input_ma)) && ok;
	}
	if (o.enable && o.charge_ma != 0) {
		ok = chg_set("charge current (CNFG_02)", CHG_REG_CNFG_02, in.cnfg02, CHG_CNFG02_CC_MASK, chg_charge_code(o.charge_ma)) && ok;
	}
	if (!ok && chg_mode >= 2) {
		chg_st.latched_off = true;
		o.enable = false;
		say("max77705: chg: a limit did not read back as written: charging off until the cable is pulled\n");
	}
	if (mode == 0x4 || mode == 0x5) {
		if (o.rearm && chg_mode >= 2) {
			/* the watchdog expired with charging wanted: kicked, and charging off and on, as Samsung's driver does */
			chg_wdt_kick();
			(void)chg_set("charging (CNFG_00, re-arm)", CHG_REG_CNFG_00, in.cnfg00, CHG_CNFG00_CHG, 0);
			(void)read8(CHARGER, CHG_REG_CNFG_00, &in.cnfg00);
		}
		if (!chg_set("charging (CNFG_00)", CHG_REG_CNFG_00, in.cnfg00, CHG_CNFG00_CHG, o.enable ? CHG_CNFG00_CHG : 0)) {
			chg_st.latched_off = true;
		}
	}
	if (chg_mode >= 2) {
		chg_wdt_kick();
	}
}

/* at step 5: the boot-arguments read, the guard said */
static void
chg_guard_start(void)
{
	(void)PE_parse_boot_argn("max77705_chg", &chg_mode, sizeof(chg_mode));
	(void)PE_parse_boot_argn("max77705_siop", &chg_siop, sizeof(chg_siop));
	if (chg_mode != 0 && board != &battery_boards[0]) {
		say("max77705: chg: max77705_chg=%u is for the Galaxy S9 (its limits are Samsung's for it): not on the %s\n",
		    chg_mode, board->platform);
		chg_mode = 0;
	}
	if (chg_mode != 0) {
		chg_state_reset(&chg_st, 0);
		say("max77705: chg: the charger guard, %s (max77705_chg=%u), 5 V only; the in-use limit %s (max77705_siop=%u)\n",
		    chg_mode >= 2 ? "writing" : "read only, saying what it would write", chg_mode, chg_siop ? "on" : "off",
		    chg_siop);
	}
}

/* with every 5-minute line */
static void
chg_guard_say(void)
{
	if (chg_mode != 0) {
		say("max77705: chg: %u writes, %u failed, %u read back differently, %u refused (locked); watchdog %u kicks\n",
		    chg_writes, chg_wfail, chg_differs, chg_refused, wdt_kicks);
	}
}

#endif /* HAKIOS_MAX77705_GUARD_H */
