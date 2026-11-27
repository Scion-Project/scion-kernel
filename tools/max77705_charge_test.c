/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/max77705_charge_test.c: drivers/power/max77705/hakios.max77705_charge.h on a computer, under the address and
 * undefined-behaviour sanitizers (tools/max77705-charge-test.sh). The rules the iOS 27 charger guard (sweetchg=2) and its
 * fast-charge path (sweetafc=1) follow on the Galaxy S9 (docs/ios27/CHARGING.md).
 *
 * What is checked:
 *   - the register encodings: every code read back as Samsung's driver reads it, and a code asked for a current never
 *     meaning more than that current;
 *   - every BC_STATUS byte (256): the type, and that only ChgTyp 3 with VBUS is a DCP;
 *   - the limits: every type's input and charge current at most Samsung's (cable-info), 9 V's the only one past 5 V;
 *   - each charger type at every battery temperature from -30.0 to 90.0 C (both ways through the hysteresis): charging off
 *     at 41.0 C and up until 39.0 C, at 5.0 C and down until 10.0 C, 700 mA at 15.0 C and down; 45 C latches off;
 *   - the faults: a read failure, no temperature, an implausible one, the float above 4350 mV, VBUS over-voltage, no
 *     battery, the charger's timer fault or thermal shutdown: off, and latched until the cable goes; the charger's
 *     watchdog expired: re-armed when charging is wanted, never latched;
 *   - the session's time: off after Samsung's time for the type, and on again only with a new cable;
 *   - full and the recharge condition;
 *   - the 9 V path: never asked on a computer port (SDP, CDP), an unknown charger, without sweetafc=1, outside 15-39 C,
 *     with the charger's thermistor at 44 C, full, or with VBUS already above 5.5 V; asked on a DCP after a second inside
 *     the band, with the input at 500 mA meanwhile; a good answer only with result 0 and VBADC 5-7 both ways; 9 V dropped
 *     (5 V asked, 500 mA) when the battery or the charger's thermistor leaves the band, VBUS leaves 7.5-10.5 V, or a fault;
 *     after a drop or a bad answer not asked again on that cable; asked again after the cable goes and comes;
 *   - the read-back check;
 *   - the iOS 16.5 guard's limits on top (max77705_chg=2): SIOP (1200/1000 mA, 500/1000 at 9 V) and a computer on the link
 *     (the CDP's 1500/1500 mA, no 9 V) only ever lower, for every input and charge current;
 *   - a grid over every input that matters: the input limit never above the type's in force, never above 500 mA with
 *     VBUS above 5.5 V outside a 9 V session, 9 V never asked outside every one of its conditions.
 */
#include <stdio.h>
#include <string.h>

#include "../drivers/power/max77705/hakios.max77705_charge.h"

static unsigned long checks, failures;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* BC_STATUS for each type: VBUS and ChgTyp */
#define BC_SDP  0x81
#define BC_CDP  0x82
#define BC_DCP  0x83
#define BC_NONE 0x00

/* a healthy reading: VBUS valid (CHGIN_DTLS 3), battery OK (BAT_DTLS 3), fast charge CC (CHG_DTLS 1), charging on, 4350 mV
 * float (code 0x13), 25.0 C battery, 30.0 C charger, 50 %, 3800 mV, VBUS 5 V */
static struct chg_in
healthy(uint8_t bc, uint32_t now)
{
	struct chg_in in = {
		.read_ok = true, .bc = bc, .vbadc = 2, .dtls00 = 3u << 5, .dtls01 = (3u << 4) | 1u,
		.cnfg00 = CHG_CNFG00_BUCK | CHG_CNFG00_CHG | CHG_CNFG00_WDTEN, .cnfg02 = 0x2b, .cnfg04 = 0x13, .cnfg09 = 0x77,
		.temp_ok = true, .temp_dc = 250, .chg_temp_ok = true, .chg_temp_dc = 300, .soc = 50, .vcell_mv = 3800,
		.now_s = now, .afc_wanted = false,
	};
	return in;
}

static const uint32_t samsung_input[CHG_TYPES] = { 0, 475, 1500, 1800, 1650, 500 };
static const uint32_t samsung_charge[CHG_TYPES] = { 0, 550, 1500, 2100, 2150, 500 };

static void
encodings(void)
{
	for (unsigned c = 0; c < 256; c++) {
		uint32_t ma = chg_input_ma_of((uint8_t)c);

		CHECK(ma >= 100 && ma <= 3200, "input code %#x -> %u mA", c, ma);
		ma = chg_charge_ma_of((uint8_t)c);
		CHECK(ma >= 100 && ma <= 3150, "charge code %#x -> %u mA", c, ma);
	}
	CHECK(chg_input_ma_of(0x77) == 3000, "0x77 is 3000 mA (Samsung's log in TWRP)");
	CHECK(chg_charge_ma_of(0x2b) == 2150, "0x2b is 2150 mA (Samsung's log in TWRP)");
	CHECK(chg_float_mv_of(0x13) == 4350, "0x13 is 4350 mV (Samsung's log in TWRP)");
	CHECK(chg_float_mv_of(0x14) == 4360, "0x14 is 4360 mV");
	CHECK(chg_float_mv_of(0x04) == 4200 && chg_float_mv_of(0x00) == 4000, "the float's low codes");
	for (uint32_t ma = 0; ma <= 4000; ma++) {
		uint8_t c = chg_input_code(ma);

		CHECK(ma < 100 || chg_input_ma_of(c) <= ma, "input %u mA -> code %#x = %u mA", ma, c, chg_input_ma_of(c));
		CHECK(ma < 125 || ma > 3200 || chg_input_ma_of(c) + 25 > ma, "input %u mA rounded down by less than a step", ma);
		c = chg_charge_code(ma);
		CHECK(ma < 150 || chg_charge_ma_of(c) <= ma, "charge %u mA -> code %#x = %u mA", ma, c, chg_charge_ma_of(c));
		CHECK(ma < 150 || ma > 3150 || chg_charge_ma_of(c) + 50 > ma, "charge %u mA rounded down by less than a step", ma);
	}
	for (int t = 0; t < CHG_TYPES; t++) {
		CHECK(chg_input_ma_of(chg_input_code(chg_limits[t].input_ma)) == chg_limits[t].input_ma || chg_limits[t].input_ma < 100,
		    "type %d's input limit is exactly encodable", t);
		CHECK(chg_charge_ma_of(chg_charge_code(chg_limits[t].charge_ma)) == chg_limits[t].charge_ma ||
		    chg_limits[t].charge_ma < 150, "type %d's charge current is exactly encodable", t);
	}
}

static void
types(void)
{
	for (unsigned bc = 0; bc < 256; bc++) {
		enum chg_type t = chg_type_of_bc((uint8_t)bc);

		CHECK(t < CHG_TYPES && t != CHG_AFC_9V, "BC %#x: never 9 V from the status alone", bc);
		CHECK(!(bc & 0x80) == (t == CHG_NONE), "BC %#x: none exactly without VBUS", bc);
		CHECK(t != CHG_DCP || ((bc & 0x80) && (bc & 3) == 3), "BC %#x: DCP only for ChgTyp 3", bc);
	}
	CHECK(chg_type_of_bc(0x82) == CHG_CDP, "phone 2's port on the Mac mini (BC_STATUS 0x82) is a CDP");
	CHECK(chg_type_of_bc(0x81) == CHG_SDP, "0x81 SDP");
	CHECK(chg_type_of_bc(0x85) == CHG_UNKNOWN, "0x85: SDP with DCD time-out");
	CHECK(chg_type_of_bc(0x81 | (2 << 3)) == CHG_UNKNOWN, "Apple 500 mA on an SDP: unknown");
	CHECK(chg_type_of_bc(0x80) == CHG_UNKNOWN, "VBUS without a type");
	for (int t = 0; t < CHG_TYPES; t++) {
		CHECK(chg_limits[t].input_ma == samsung_input[t] && chg_limits[t].charge_ma == samsung_charge[t],
		    "type %d: %u/%u mA, Samsung's %u/%u", t, chg_limits[t].input_ma, chg_limits[t].charge_ma, samsung_input[t],
		    samsung_charge[t]);
		CHECK(t == CHG_NONE || chg_limits[t].session_s > 0, "type %d has a session time", t);
	}
}

/* a cable in for `secs` seconds at a temperature, from a fresh state; the last output */
static struct chg_out
run(struct chg_state *s, uint8_t bc, int32_t temp_dc, bool afc, unsigned secs)
{
	struct chg_out o = { 0 };

	for (unsigned t = 0; t <= secs; t++) {
		struct chg_in in = healthy(bc, 1000 + t);

		in.temp_dc = temp_dc;
		in.afc_wanted = afc;
		o = chg_step(s, &in);
	}
	return o;
}

static void
bands(void)
{
	static const uint8_t bcs[] = { BC_SDP, BC_CDP, BC_DCP, 0x80 };

	for (unsigned b = 0; b < sizeof(bcs); b++) {
		enum chg_type ty = chg_type_of_bc(bcs[b]);
		struct chg_state s;
		struct chg_out o;

		/* upward from -30.0 C to 44.9 C, a tenth at a time, one second each */
		chg_state_reset(&s, 0);
		bool hot = false, cold = true, red = true;
		for (int32_t t = -300; t < 450; t++) {
			struct chg_in in = healthy(bcs[b], 2000 + (uint32_t)(t + 300));

			in.temp_dc = t;
			o = chg_step(&s, &in);
			if (t >= T_STOP_HIGH_DC) hot = true;
			if (t >= T_RESUME_LOW_DC) cold = false;
			if (t >= T_UNREDUCE_DC) red = false;
			CHECK(o.enable == (!hot && !cold), "type %d up at %d: enable %d", ty, t, o.enable);
			CHECK(!o.ask_9v, "type %d: no 9 V without sweetafc", ty);
			if (o.enable) {
				CHECK(o.input_ma == chg_limits[ty].input_ma, "type %d at %d: input %u", ty, t, o.input_ma);
				CHECK(o.charge_ma == (red && chg_limits[ty].charge_ma > 700 ? 700 : chg_limits[ty].charge_ma),
				    "type %d at %d: charge %u (reduced %d)", ty, t, o.charge_ma, red);
				CHECK(t > T_STOP_LOW_DC && t < T_STOP_HIGH_DC, "type %d: charging at %d", ty, t);
			}
		}
		/* and down again from 44.9 C to -30.0 C */
		for (int32_t t = 449; t >= -300; t--) {
			struct chg_in in = healthy(bcs[b], 3000 + (uint32_t)(449 - t));

			in.temp_dc = t;
			o = chg_step(&s, &in);
			if (t <= T_RESUME_HIGH_DC) hot = false;
			if (t <= T_STOP_LOW_DC) cold = true;
			if (t <= T_REDUCE_DC) red = true;
			CHECK(o.enable == (!hot && !cold), "type %d down at %d: enable %d", ty, t, o.enable);
			if (o.enable) {
				CHECK(o.charge_ma <= (red ? 700u : chg_limits[ty].charge_ma), "type %d down at %d: charge %u", ty, t,
				    o.charge_ma);
			}
		}
		/* 45.0 C and above: off, latched while the cable stays, even back at 25 C */
		chg_state_reset(&s, 0);
		o = run(&s, bcs[b], 450, false, 0);
		CHECK(!o.enable && o.why == WHY_ALARM, "type %d at 45.0 C: off (%s)", ty, chg_why_name(o.why));
		o = run(&s, bcs[b], 250, false, 2);
		CHECK(!o.enable && o.why == WHY_FAULT, "type %d: 45 C latched (%s)", ty, chg_why_name(o.why));
		{
			struct chg_in gone = healthy(BC_NONE, 5000);

			o = chg_step(&s, &gone);
			CHECK(!o.enable && o.why == WHY_NO_VBUS, "cable out");
		}
		o = run(&s, bcs[b], 250, false, 2);
		CHECK(o.enable, "type %d: a new cable charges again", ty);
	}
}

static void
faults(void)
{
	struct chg_state s;
	struct chg_out o;
	struct chg_in in;

	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 10);
	in.read_ok = false;
	o = chg_step(&s, &in);
	CHECK(!o.enable && o.why == WHY_READ_FAILED && o.input_ma == 0 && o.charge_ma == 0 && !o.ask_9v, "a read failed");

	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 10);
	in.temp_ok = false;
	o = chg_step(&s, &in);
	CHECK(!o.enable && o.why == WHY_NO_TEMP, "no temperature: off");
	in.temp_ok = true;
	in.now_s++;
	o = chg_step(&s, &in);
	CHECK(o.enable, "the temperature back: on (not latched)");

	for (int32_t t = -1000; t <= 2000; t += 50) {
		chg_state_reset(&s, 0);
		in = healthy(BC_CDP, 10);
		in.temp_dc = t;
		o = chg_step(&s, &in);
		CHECK(chg_temp_plausible(t) || (!o.enable && o.why == WHY_NO_TEMP), "an implausible %d: off", t);
	}

	for (unsigned code = 0; code < 64; code++) {
		chg_state_reset(&s, 0);
		in = healthy(BC_CDP, 10);
		in.cnfg04 = (uint8_t)code;
		o = chg_step(&s, &in);
		CHECK((chg_float_mv_of((uint8_t)code) > 4350) == (o.why == WHY_FLOAT_HIGH), "float code %#x (%u mV): %s", code,
		    chg_float_mv_of((uint8_t)code), chg_why_name(o.why));
		CHECK(chg_float_mv_of((uint8_t)code) <= 4350 || !o.enable, "float code %#x: off", code);
	}

	{
		static const struct { uint8_t d00, d01; const char *what; } bad[] = {
			{ 2u << 5, (3u << 4) | 1, "VBUS over-voltage" },
			{ 3u << 5, (0u << 4) | 1, "no battery" },
			{ 3u << 5, (2u << 4) | 1, "battery dead" },
			{ 3u << 5, (5u << 4) | 1, "battery over-voltage" },
			{ 3u << 5, (3u << 4) | 6, "timer fault" },
			{ 3u << 5, (3u << 4) | 0xA, "thermal shutdown" },
		};

		for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			chg_state_reset(&s, 0);
			in = healthy(BC_DCP, 10);
			in.dtls00 = bad[i].d00;
			in.dtls01 = bad[i].d01;
			o = chg_step(&s, &in);
			CHECK(!o.enable && o.why == WHY_FAULT, "%s: off", bad[i].what);
			in = healthy(BC_DCP, 11);
			o = chg_step(&s, &in);
			CHECK(!o.enable && o.why == WHY_FAULT, "%s: latched", bad[i].what);
		}
	}
	/* the watchdog expired (seen on phone 2 in iOS 70 s after S-Boot: DETAILS_01 0x3b): not a latched fault; re-armed
	 * when charging is wanted, not when it is not (hot, full) */
	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 10);
	in.dtls01 = (3u << 4) | 0xB;
	o = chg_step(&s, &in);
	CHECK(o.enable && o.rearm && o.why == WHY_OK, "watchdog expired: re-armed (%s)", chg_why_name(o.why));
	in.dtls01 = (3u << 4) | 1;
	in.now_s++;
	o = chg_step(&s, &in);
	CHECK(o.enable && !o.rearm, "charging again: no re-arm");
	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 10);
	in.dtls01 = (3u << 4) | 0xB;
	in.temp_dc = 420;
	o = chg_step(&s, &in);
	CHECK(!o.enable && !o.rearm && o.why == WHY_HOT, "watchdog expired at 42 C: left off");
	CHECK(chg_readback_ok(0x2b, 0xeb, CHG_CNFG02_CC_MASK), "read-back: only the bits written");
	CHECK(!chg_readback_ok(0x2b, 0x2a, CHG_CNFG02_CC_MASK), "read-back: a differing bit");
}

static void
session(void)
{
	static const uint8_t bcs[] = { BC_SDP, BC_CDP, BC_DCP };

	for (unsigned b = 0; b < sizeof(bcs); b++) {
		enum chg_type ty = chg_type_of_bc(bcs[b]);
		struct chg_state s;
		struct chg_out o = { 0 };
		uint32_t limit = chg_limits[ty].session_s;

		chg_state_reset(&s, 0);
		for (uint32_t t = 0; t <= limit + 20; t += 5) {
			struct chg_in in = healthy(bcs[b], t);

			o = chg_step(&s, &in);
			if (t + 5 < limit) {
				CHECK(o.enable, "type %d at %u s: on", ty, t);
			}
		}
		CHECK(!o.enable && o.why == WHY_EXPIRED, "type %d after %u s: off (%s)", ty, limit, chg_why_name(o.why));
		{
			struct chg_in gone = healthy(BC_NONE, limit + 30), back = healthy(bcs[b], limit + 35);

			chg_step(&s, &gone);
			o = chg_step(&s, &back);
			CHECK(o.enable, "type %d: a new cable, a new session", ty);
		}
	}
	/* not counted while the chip does not charge (full, or charging off) */
	{
		struct chg_state s;
		struct chg_out o = { 0 };

		chg_state_reset(&s, 0);
		for (uint32_t t = 0; t <= 40000; t += 10) {
			struct chg_in in = healthy(BC_CDP, t);

			in.dtls01 = (3u << 4) | 0x8;    /* off */
			o = chg_step(&s, &in);
		}
		CHECK(o.why != WHY_EXPIRED, "time not counted while the chip is off");
	}
}

static void
full(void)
{
	struct chg_state s;
	struct chg_out o;
	struct chg_in in;

	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 1);
	in.dtls01 = (3u << 4) | 0x4;    /* done */
	in.soc = 100;
	in.vcell_mv = 4340;
	o = chg_step(&s, &in);
	CHECK(!o.enable && o.why == WHY_FULL, "done: off (%s)", chg_why_name(o.why));
	in.dtls01 = (3u << 4) | 0x8;    /* off, as asked */
	in.now_s = 2;
	o = chg_step(&s, &in);
	CHECK(!o.enable && o.why == WHY_FULL, "stays off above the recharge condition");
	in.vcell_mv = 4279;
	in.now_s = 3;
	o = chg_step(&s, &in);
	CHECK(o.enable && o.why == WHY_OK, "on again under 4280 mV");
	chg_state_reset(&s, 0);
	in = healthy(BC_CDP, 1);
	in.dtls01 = (3u << 4) | 0x4;
	in.soc = 100;
	in.vcell_mv = 4340;
	chg_step(&s, &in);
	in.dtls01 = (3u << 4) | 0x8;
	in.soc = 97;
	in.now_s = 2;
	o = chg_step(&s, &in);
	CHECK(o.enable, "on again under 98 %%");
}

/* a DCP with sweetafc=1 until 9 V is asked; returns the step that asked (or the last) */
static struct chg_out
to_ask(struct chg_state *s, struct chg_in *in, unsigned max_steps)
{
	struct chg_out o = { 0 };

	for (unsigned i = 0; i < max_steps; i++) {
		o = chg_step(s, in);
		if (o.ask_9v) {
			break;
		}
		in->now_s++;
	}
	return o;
}

static void
afc(void)
{
	struct chg_state s;
	struct chg_out o;
	struct chg_in in;

	/* refused: computer ports, unknown, no sweetafc, the bands, full, VBUS already high */
	{
		static const uint8_t bcs[] = { BC_SDP, BC_CDP, 0x80, 0x85, 0x81 | (4 << 3) };

		for (unsigned b = 0; b < sizeof(bcs); b++) {
			chg_state_reset(&s, 0);
			in = healthy(bcs[b], 1);
			in.afc_wanted = true;
			o = to_ask(&s, &in, 30);
			CHECK(!o.ask_9v, "BC %#x: never 9 V", bcs[b]);
			chg_afc_result(&s, true);       /* even a "good" answer out of the blue */
			CHECK(s.type != CHG_AFC_9V, "BC %#x: a stray answer makes no 9 V session", bcs[b]);
		}
	}
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	o = to_ask(&s, &in, 30);
	CHECK(!o.ask_9v, "no 9 V without sweetafc=1");
	for (int32_t t = -100; t <= 440; t += 5) {
		chg_state_reset(&s, 0);
		in = healthy(BC_DCP, 1);
		in.afc_wanted = true;
		in.temp_dc = t;
		o = to_ask(&s, &in, 5);
		CHECK(o.ask_9v == (t > T_FAST_MIN_DC && t <= T_FAST_MAX_DC), "DCP at %d: ask %d", t, o.ask_9v);
	}
	for (int32_t t = 300; t <= 500; t += 5) {
		chg_state_reset(&s, 0);
		in = healthy(BC_DCP, 1);
		in.afc_wanted = true;
		in.chg_temp_dc = t;
		o = to_ask(&s, &in, 5);
		CHECK(o.ask_9v == (t < T_CHG_FAST_MAX_DC), "charger thermistor %d: ask %d", t, o.ask_9v);
	}
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	in.afc_wanted = true;
	in.chg_temp_ok = false;
	CHECK(!to_ask(&s, &in, 5).ask_9v, "no charger thermistor: no 9 V");
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	in.afc_wanted = true;
	in.soc = 98;
	CHECK(!to_ask(&s, &in, 5).ask_9v, "98 %%: no 9 V");
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	in.afc_wanted = true;
	in.vbadc = 3;
	o = to_ask(&s, &in, 5);
	CHECK(!o.ask_9v && o.input_ma == CHG_PRE_AFC_INPUT_MA, "VBUS above 5.5 V on a plain DCP: no request, 500 mA");

	/* the good path */
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	in.afc_wanted = true;
	o = chg_step(&s, &in);
	CHECK(!o.ask_9v && o.enable && o.input_ma == 1800 && o.charge_ma == 2100, "the first second of a DCP: 5 V limits");
	in.now_s++;
	o = chg_step(&s, &in);
	CHECK(o.ask_9v && o.input_ma == CHG_PRE_AFC_INPUT_MA, "after a second: 9 V asked, the input at 500 mA");
	{
		uint8_t good[3] = { MUIC_OP_AFC_RESULT, 0, 6 }, wrong_op[3] = { 0x05, 0, 6 }, res1[3] = { MUIC_OP_AFC_RESULT, 2, 6 },
		        low[3] = { MUIC_OP_AFC_RESULT, 0, 2 }, high[3] = { MUIC_OP_AFC_RESULT, 0, 8 };

		CHECK(chg_afc_answer_9v(good, 6), "the good answer");
		CHECK(!chg_afc_answer_9v(good, 2), "the good answer but VBUS read at 5 V");
		CHECK(!chg_afc_answer_9v(wrong_op, 6), "another opcode's answer");
		CHECK(!chg_afc_answer_9v(res1, 6), "result 2 (not a high-voltage DCP)");
		CHECK(!chg_afc_answer_9v(low, 6), "result 0 at 5 V");
		CHECK(!chg_afc_answer_9v(high, 8), "result 0 at 10.5-11.5 V");
		for (unsigned r = 0; r < 256; r++) {
			for (unsigned v = 0; v < 16; v++) {
				uint8_t a[3] = { MUIC_OP_AFC_RESULT, (uint8_t)r, (uint8_t)v };

				CHECK(chg_afc_answer_9v(a, (uint8_t)v) == (r == 0 && v >= 5 && v <= 7), "answer r %u v %u", r, v);
			}
		}
	}
	chg_afc_result(&s, true);
	CHECK(s.type == CHG_AFC_9V, "a good answer: 9 V");
	in.vbadc = 6;
	in.now_s++;
	o = chg_step(&s, &in);
	CHECK(o.enable && o.input_ma == 1650 && o.charge_ma == 2150 && !o.ask_9v && !o.ask_5v, "9 V: 1650/2150");

	/* the watchdog's reasons to drop it */
	{
		struct { const char *what; int32_t temp, chg_temp; uint8_t vbadc, d00; uint32_t soc; } drop[] = {
			{ "battery 39.1 C", 391, 300, 6, 3u << 5, 50 },
			{ "battery 15.0 C", 150, 300, 6, 3u << 5, 50 },
			{ "charger 44.0 C", 250, 440, 6, 3u << 5, 50 },
			{ "VBUS 5 V", 250, 300, 2, 3u << 5, 50 },
			{ "VBUS 10.5-11.5 V", 250, 300, 8, 3u << 5, 50 },
			{ "98 %", 250, 300, 6, 3u << 5, 98 },
			{ "over-voltage", 250, 300, 6, 2u << 5, 50 },
			{ "45 C", 450, 300, 6, 3u << 5, 50 },
		};

		for (unsigned i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
			struct chg_state s2 = s;
			struct chg_in in2 = in;

			in2.temp_dc = drop[i].temp;
			in2.chg_temp_dc = drop[i].chg_temp;
			in2.vbadc = drop[i].vbadc;
			in2.dtls00 = drop[i].d00;
			in2.soc = drop[i].soc;
			in2.now_s++;
			o = chg_step(&s2, &in2);
			CHECK(o.ask_5v && s2.type == CHG_DCP && s2.afc_failed, "%s: back to 5 V", drop[i].what);
			CHECK(o.input_ma <= CHG_PRE_AFC_INPUT_MA || (in2.vbadc <= VBADC_5V_MAX && o.input_ma <= 1800),
			    "%s: input %u", drop[i].what, o.input_ma);
			in2.temp_dc = 250;
			in2.chg_temp_dc = 300;
			in2.vbadc = 2;
			in2.dtls00 = 3u << 5;
			in2.soc = 50;
			for (int k = 0; k < 10; k++) {
				in2.now_s++;
				o = chg_step(&s2, &in2);
				CHECK(!o.ask_9v, "%s: not asked again on this cable", drop[i].what);
			}
		}
	}
	/* a bad answer: not again on this cable; again after a new cable */
	chg_state_reset(&s, 0);
	in = healthy(BC_DCP, 1);
	in.afc_wanted = true;
	o = to_ask(&s, &in, 5);
	CHECK(o.ask_9v, "asked");
	chg_afc_result(&s, false);
	for (int k = 0; k < 10; k++) {
		in.now_s++;
		o = chg_step(&s, &in);
		CHECK(!o.ask_9v && o.input_ma == 1800, "after a bad answer: 5 V limits, no new request");
	}
	in.bc = BC_NONE;
	in.now_s++;
	chg_step(&s, &in);
	in.bc = BC_DCP;
	o = to_ask(&s, &in, 5);
	CHECK(o.ask_9v, "a new cable: asked again");
	/* the MUIC changing its mind under 9 V */
	chg_afc_result(&s, true);
	in.vbadc = 6;
	in.bc = BC_CDP;
	in.now_s++;
	o = chg_step(&s, &in);
	CHECK(s.type == CHG_CDP && o.input_ma <= 1500, "9 V, then the MUIC says CDP: CDP limits (%u)", o.input_ma);
}

/* every input that matters, for every state reached from them: the invariants */
static void
grid(void)
{
	static const uint8_t bcs[] = { BC_NONE, BC_SDP, BC_CDP, BC_DCP, 0x80, 0x85 };
	static const int32_t temps[] = { -300, 0, 50, 51, 100, 150, 151, 200, 250, 390, 391, 409, 410, 449, 450, 600 };
	static const int32_t ctemps[] = { 200, 439, 440, 500 };
	static const uint8_t vbadcs[] = { 0, 2, 3, 5, 6, 7, 8, 10 };
	static const uint32_t socs[] = { 5, 50, 97, 98, 100 };
	unsigned long combos = 0;

	for (unsigned b = 0; b < sizeof(bcs); b++)
	for (unsigned t = 0; t < sizeof(temps) / sizeof(temps[0]); t++)
	for (unsigned c = 0; c < sizeof(ctemps) / sizeof(ctemps[0]); c++)
	for (unsigned v = 0; v < sizeof(vbadcs); v++)
	for (unsigned q = 0; q < sizeof(socs) / sizeof(socs[0]); q++)
	for (unsigned afc_on = 0; afc_on < 2; afc_on++)
	for (unsigned start9 = 0; start9 < 2; start9++) {
		struct chg_state s;
		struct chg_in in = healthy(bcs[b], 100);
		struct chg_out o;
		enum chg_type before;

		chg_state_reset(&s, 0);
		if (start9) {
			/* a 9 V session already in force, reached the proper way */
			struct chg_in d = healthy(BC_DCP, 90);

			d.afc_wanted = true;
			to_ask(&s, &d, 5);
			chg_afc_result(&s, true);
		}
		in.temp_dc = temps[t];
		in.chg_temp_dc = ctemps[c];
		in.vbadc = vbadcs[v];
		in.soc = socs[q];
		in.afc_wanted = afc_on;
		before = s.type;
		o = chg_step(&s, &in);
		combos++;
		(void)before;
		CHECK(s.type < CHG_TYPES, "state type");
		CHECK(o.input_ma <= chg_limits[s.type].input_ma || o.input_ma <= CHG_PRE_AFC_INPUT_MA,
		    "input %u above type %d's", o.input_ma, s.type);
		CHECK(o.charge_ma <= chg_limits[s.type].charge_ma, "charge %u above type %d's", o.charge_ma, s.type);
		CHECK(o.input_ma <= 1800 && o.charge_ma <= 2150, "never above Samsung's highest");
		CHECK(s.type == CHG_AFC_9V || in.vbadc <= VBADC_5V_MAX || o.input_ma <= CHG_PRE_AFC_INPUT_MA,
		    "VBUS %u outside a 9 V session: input %u", in.vbadc, o.input_ma);
		CHECK(!o.ask_9v || (afc_on && chg_type_of_bc(in.bc) == CHG_DCP && in.temp_dc > T_FAST_MIN_DC &&
		    in.temp_dc <= T_FAST_MAX_DC && in.chg_temp_dc < T_CHG_FAST_MAX_DC && in.soc < RECHARGE_SOC &&
		    in.vbadc <= VBADC_5V_MAX), "9 V asked outside its conditions");
		CHECK(!o.enable || (in.temp_dc > T_STOP_LOW_DC && in.temp_dc < T_STOP_HIGH_DC), "charging at %d", in.temp_dc);
		CHECK(s.type != CHG_AFC_9V || (in.temp_dc <= T_FAST_MAX_DC && in.temp_dc > T_FAST_MIN_DC &&
		    in.chg_temp_dc < T_CHG_FAST_MAX_DC && in.vbadc >= VBADC_9V_MIN && in.vbadc <= VBADC_9V_MAX),
		    "a 9 V session kept outside its band (t %d, c %d, v %u)", in.temp_dc, in.chg_temp_dc, in.vbadc);
		CHECK(!(o.ask_9v && o.ask_5v), "both requests at once");
		CHECK(!o.rearm || o.enable, "a re-arm only with charging wanted");
	}
	printf("max77705_charge_test: grid of %lu states\n", combos);
}

/* SIOP and a computer on the link (iOS 16.5's guard): only ever lower, at Samsung's S9 numbers, and no 9 V with a host */
static void
caps(void)
{
	for (int t = CHG_SDP; t < CHG_TYPES; t++) {
		for (uint32_t in_ma = 0; in_ma <= 3200; in_ma += 25) {
			for (uint32_t cc_ma = 0; cc_ma <= 3150; cc_ma += 50) {
				struct chg_out o = { .enable = true, .input_ma = in_ma, .charge_ma = cc_ma, .ask_9v = true };
				struct chg_out h = o;

				chg_siop_cap(&o, (enum chg_type)t);
				CHECK(o.input_ma <= in_ma && o.charge_ma <= cc_ma, "SIOP raised a limit");
				CHECK(o.input_ma <= (t == CHG_AFC_9V ? 500u : 1200u) && o.charge_ma <= 1000u,
				    "SIOP type %d: %u/%u", t, o.input_ma, o.charge_ma);
				CHECK(o.input_ma == (in_ma < (t == CHG_AFC_9V ? 500u : 1200u) ? in_ma : (t == CHG_AFC_9V ? 500u : 1200u)),
				    "SIOP lowered more than asked");
				chg_host_cap(&h);
				CHECK(h.input_ma <= 1500 && h.charge_ma <= 1500 && !h.ask_9v, "host cap: %u/%u", h.input_ma, h.charge_ma);
				CHECK(h.input_ma <= in_ma && h.charge_ma <= cc_ma, "host cap raised a limit");
			}
		}
	}
	{
		/* a DCP at 25 C: Samsung's 1800/2100 mA; in use 1200/1000; a computer on the link 1500/1500 */
		struct chg_state s;
		struct chg_in in = healthy(BC_DCP, 100);
		struct chg_out o;

		chg_state_reset(&s, 100);
		o = chg_step(&s, &in);
		CHECK(o.enable && o.input_ma == 1800 && o.charge_ma == 2100, "DCP %u/%u", o.input_ma, o.charge_ma);
		chg_siop_cap(&o, s.type);
		CHECK(o.input_ma == 1200 && o.charge_ma == 1000, "DCP in use %u/%u", o.input_ma, o.charge_ma);
		o = chg_step(&s, &in);
		chg_host_cap(&o);
		CHECK(o.input_ma == 1500 && o.charge_ma == 1500, "DCP with a host %u/%u", o.input_ma, o.charge_ma);
	}
}

int
main(void)
{
	encodings();
	types();
	bands();
	faults();
	session();
	full();
	afc();
	grid();
	caps();
	printf("max77705_charge_test: %lu checks, %lu failures\n", checks, failures);
	return failures != 0;
}
