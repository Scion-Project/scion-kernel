/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-phaseb-r1/chg/xnu-src/0001-hk-chg1.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * The max77779 charger's registers as numbers a person can read: pure decoding, no bus, no state. C, kernel and host.
 * blazer_gauge.c reads the registers (blazer_battery_chg=2, EXT_REG_READ frames only) and prints what this returns.
 *
 * Own code. Field positions and unit steps are facts from the GPL vendor driver and its register header
 * (google-modules/bms max77779_charger.c get_* functions, max77779_v20240115_regs.h, max77779_regs.h; links and
 * the line numbers in research/ios26-phaseb-r1/chg/REGISTERS.md). This file encodes nothing: there is no
 * value-to-register direction here, so nothing that could be used to program a limit.
 */
#ifndef BLAZER_CHARGER_H
#define BLAZER_CHARGER_H
#include <stdbool.h>
#include <stdint.h>

/* The raw bytes, as read. ADC words are L | H << 8 from one two-byte read at the L register. */
struct blazer_charger_regs {
	uint8_t int_ok;         /* 0xb6 CHG_INT_OK */
	uint8_t d0, d1, d2, d3; /* 0xb7..0xba CHG_DETAILS_00..03 */
	uint8_t cnfg00;         /* 0xbc MODE[3:0], CP_EN bit 5 */
	uint8_t cnfg02;         /* 0xbe CHGCC[5:0]: fast-charge current */
	uint8_t cnfg04;         /* 0xc0 CHG_CV_PRM[5:0]: regulation (float) voltage */
	uint8_t cnfg09;         /* 0xc5 CHGIN_ILIM[6:0]: wired input limit */
	uint8_t cnfg10;         /* 0xc6 WCIN_ILIM[6:0]: wireless input limit */
	uint8_t cnfg15;         /* 0xcb WDTEN bit 0 */
	uint16_t chgin_i;       /* 0x00/0x01 CHGIN_I_ADC */
	uint16_t chgin_v;       /* 0x02/0x03 CHGIN_V_ADC: no unit in the vendor source, logged raw */
	uint16_t wcin_i;        /* 0x04/0x05 WCIN_I_ADC */
	uint16_t wcin_v;        /* 0x06/0x07 WCIN_V_ADC */
};

struct blazer_charger_view {
	bool wired;             /* CHGIN_DTLS 2 or 3: max77779_is_online */
	bool wireless;          /* WCIN_DTLS 2 or 3 */
	unsigned chgin_dtls, wcin_dtls, chg_dtls, bat_dtls, mode;
	bool cp_en, wdt_en;
	bool charging;          /* an input is valid and CHG_DTLS is 0..3 (dead battery, CC, CV, top-off) */
	bool cv_valid;          /* CHG_CV_PRM 0x3a..0x3f has no meaning in the vendor's decoder */
	uint32_t cc_ma;         /* programmed fast-charge current; 0 = charging suspended by MODE 0 */
	uint32_t cv_mv;         /* programmed regulation voltage; 0 if !cv_valid */
	uint32_t icl_ma;        /* programmed wired input limit */
	uint32_t wicl_ma;       /* programmed wireless input limit */
	uint32_t ichgin_ma;     /* measured wired input current. Vendor: "only valid in mode 1, 5, 6, 7, c, d" */
	uint32_t iwcin_ma;      /* measured wireless input current. Vendor: "only valid in mode e, f" */
	uint32_t vwcin_mv;      /* measured wireless input voltage */
};

/* max77779_get_charger_current_max_ua */
static inline uint32_t
blazer_charger_cc_ma(uint8_t cnfg02)
{
	unsigned v = cnfg02 & 0x3fu;

	if (v == 0) {
		return 0;
	}
	if (v < 3) {
		return 133;
	}
	if (v >= 0x3c) {
		return 4000;
	}
	return (133000u + (v - 2u) * 66670u) / 1000u;
}

/* max77779_get_regulation_voltage_uv. False: a code the vendor calls -EINVAL. */
static inline bool
blazer_charger_cv_mv(uint8_t cnfg04, uint32_t *mv)
{
	unsigned v = cnfg04 & 0x3fu;

	if (v < 0x38) {
		*mv = 4000u + v * 10u;
	} else if (v == 0x38) {
		*mv = 3800u;
	} else if (v == 0x39) {
		*mv = 3900u;
	} else {
		*mv = 0;
		return false;
	}
	return true;
}

/* max77779_chgin_get_ilim_max_ua (without its software input-suspend override, which is driver state, not a register) */
static inline uint32_t
blazer_charger_icl_ma(uint8_t cnfg09)
{
	unsigned v = cnfg09 & 0x7fu;

	if (v == 0) {
		return 0;
	}
	return v > 3 ? 100u + (v - 3u) * 25u : 100u;
}

/* max77779_wcin_get_ilim_max_ua (same remark) */
static inline uint32_t
blazer_charger_wicl_ma(uint8_t cnfg10)
{
	unsigned v = cnfg10 & 0x7fu;

	if (v == 0) {
		return 0;
	}
	return v < 4 ? 100u : 125u + (v - 4u) * 25u;
}

/* CHG_DETAILS_01 CHG_DTLS names, max77779_charger.c CHGR_DTLS_*. Codes it does not name print as "dtls<hex>". */
static inline const char *
blazer_charger_chg_name(unsigned chg_dtls)
{
	switch (chg_dtls) {
	case 0x0: return "deadbat";
	case 0x1: return "cc";
	case 0x2: return "cv";
	case 0x3: return "topoff";
	case 0x4: return "done";
	case 0x6: return "timer-fault";
	case 0x7: return "detbat-suspend";
	case 0x8: return "off";
	case 0xa: return "off-hot";
	case 0xb: return "off-watchdog";
	case 0xc: return "off-jeita";
	case 0xd: return "off-temp";
	default: return 0;
	}
}

static inline const char *
blazer_charger_in_name(const struct blazer_charger_view *v)
{
	return v->wired ? (v->wireless ? "both" : "usb") : (v->wireless ? "wireless" : "none");
}

static inline void
blazer_charger_decode(const struct blazer_charger_regs *r, struct blazer_charger_view *v)
{
	v->chgin_dtls = (r->d0 >> 5) & 3u;
	v->wcin_dtls = (r->d0 >> 3) & 3u;
	v->chg_dtls = r->d1 & 0xfu;
	v->bat_dtls = (r->d1 >> 4) & 7u;
	v->mode = r->cnfg00 & 0xfu;
	v->cp_en = (r->cnfg00 >> 5) & 1u;
	v->wdt_en = r->cnfg15 & 1u;
	v->wired = v->chgin_dtls >= 2;
	v->wireless = v->wcin_dtls >= 2;
	v->charging = (v->wired || v->wireless) && v->chg_dtls <= 3;
	v->cc_ma = blazer_charger_cc_ma(r->cnfg02);
	v->cv_valid = blazer_charger_cv_mv(r->cnfg04, &v->cv_mv);
	v->icl_ma = blazer_charger_icl_ma(r->cnfg09);
	v->wicl_ma = blazer_charger_wicl_ma(r->cnfg10);
	/* MAX77779_CHGIN_RAW_TO_UA 166, MAX77779_WCIN_RAW_TO_UA 166, MAX77779_WCIN_RAW_TO_UV 625 */
	v->ichgin_ma = (uint32_t)r->chgin_i * 166u / 1000u;
	v->iwcin_ma = (uint32_t)r->wcin_i * 166u / 1000u;
	v->vwcin_mv = (uint32_t)r->wcin_v * 625u / 1000u;
}

#endif
