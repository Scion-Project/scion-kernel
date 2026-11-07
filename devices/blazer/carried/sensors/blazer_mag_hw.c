/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-sensors-r1/mag-r1/patches/0930-blazer-sensor-compass.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#if defined(BLAZER)
/* SPDX-License-Identifier: MIT
 * MMC5616WA by the AP: on-demand measurements with automatic SET/RESET, pipelined so the 6.6 ms conversion runs between two calls.
 * Rules this file keeps:
 *   Writes: only MAG_REG_CTRL0, only DO_SET (once at start) and MAG_MEASURE. No ODR, no continuous mode, no OTP, no self-test, no software reset.
 *   Reads: the product id, Status1 (logged once), and the nine output bytes. The bus below refuses anything else too.
 *   Address 0x7e is never sent: that would switch the part to I3C until power is removed (datasheet page 27).
 *   Any failure stops for good: no retry. The caller then puts its pads back. */
#include "blazer_hw_port.h"
#include "blazer_mag_hw.h"

static bool
mag_halt(struct mag *m, uint32_t code, uint32_t detail)
{
	if (m->stop == MAG_OK) {
		m->stop = code;
		m->stop_detail = detail;
		m->bus.log(m->bus.ctx, "mag stopped: %s, detail 0x%x\n", mag_stop_name(code), detail);
	}
	return false;
}

const char *
mag_stop_name(uint32_t stop)
{
	static const char *const names[] = { "ok", "bus", "product id is not 0x11", "output reads all 00 or all ff", "output did not change for 50 samples", "not started" };

	return stop < sizeof(names) / sizeof(names[0]) ? names[stop] : "?";
}

void
mag_decode(const uint8_t b[9], int32_t counts[3])
{
	for (unsigned i = 0; i < 3; i++) {
		uint32_t u = (uint32_t)b[2 * i] << 12 | (uint32_t)b[2 * i + 1] << 4 | (uint32_t)b[6 + i] >> 4;

		counts[i] = (int32_t)u - MAG_NULL_COUNT;
	}
}

void
mag_to_ios_nt(const int32_t c[3], uint32_t flip, int32_t nt[3])
{
	/* Registry: /dev/mmc5616/0/mag orientation_transform=-2 -1 -3, so Android x = -part y, y = -part x, z = -part z.
	 * A magnetic field has no gravity-style sign convention between Android and iOS (both report the field in device axes): iOS = Android. Inferred until the phone is turned */
	const int32_t a[3] = { -c[1], -c[0], -c[2] };

	for (unsigned i = 0; i < 3; i++) {
		/* 1 count = 1/16384 G = 100000/16384 nT = 3125/512 nT. |c| < 2^19, so the product fits easily in 64 bits */
		int32_t v = (int32_t)((int64_t)a[i] * 3125 / 512);

		nt[i] = (flip >> i) & 1u ? -v : v;
	}
}

int
mag_start(struct mag *m, const struct mag_bus *bus)
{
	uint8_t id = 0, st = 0;

	*m = (struct mag){ .bus = *bus };
	m->bus.sleep_us(m->bus.ctx, MAG_POWER_ON_US);
	if (m->bus.read(m->bus.ctx, MAG_REG_PRODUCT_ID, &id, 1)) {
		return (void)mag_halt(m, MAG_STOP_BUS, MAG_REG_PRODUCT_ID), (int)m->stop;
	}
	m->id = id;
	m->bus.log(m->bus.ctx, "mag product id 0x%02x at 0x%02x\n", id, MAG_ADDR);
	if (id != MAG_PRODUCT_ID) {
		return (void)mag_halt(m, MAG_STOP_ID, id), (int)m->stop;
	}
	if (m->bus.read(m->bus.ctx, MAG_REG_STATUS1, &st, 1)) {
		return (void)mag_halt(m, MAG_STOP_BUS, MAG_REG_STATUS1), (int)m->stop;
	}
	m->status_first = st;
	/* Status1's bit names disagree between the table (page 11: bit 6 Meas_m_done, bit 4 OTP_read_done) and the text (page 16: "bit 1"), so nothing is decided on it: logged only */
	m->bus.log(m->bus.ctx, "mag status1 0x%02x at start (not checked)\n", st);
	/* One SET clears residual magnetisation from the last power cycle; then measurements use automatic SET/RESET (page 12) */
	if (m->bus.write(m->bus.ctx, MAG_REG_CTRL0, MAG_CTRL0_DO_SET)) {
		return (void)mag_halt(m, MAG_STOP_BUS, MAG_CTRL0_DO_SET), (int)m->stop;
	}
	m->bus.sleep_us(m->bus.ctx, MAG_SR_GAP_US);
	if (m->bus.write(m->bus.ctx, MAG_REG_CTRL0, MAG_MEASURE)) {
		return (void)mag_halt(m, MAG_STOP_BUS, MAG_MEASURE), (int)m->stop;
	}
	m->started = m->pending = true;
	return 0;
}

bool
mag_sample(struct mag *m, uint32_t flip, int32_t nt[3])
{
	uint8_t b[9];
	int32_t c[3];
	bool flat = true, same = m->samples > 0;

	if (m->stop != MAG_OK) {
		return false;
	}
	if (!m->started || !m->pending) {
		return mag_halt(m, MAG_STOP_NOT_STARTED, 0);
	}
	m->pending = false;
	if (m->bus.read(m->bus.ctx, MAG_REG_XOUT0, b, sizeof(b))) {
		return mag_halt(m, MAG_STOP_BUS, MAG_REG_XOUT0);
	}
	for (unsigned i = 0; i < sizeof(b); i++) {
		flat = flat && (b[i] == b[0]) && (b[0] == 0x00 || b[0] == 0xff);
		same = same && b[i] == m->last[i];
	}
	if (flat) {
		return mag_halt(m, MAG_STOP_FLAT, b[0]);
	}
	m->same_run = same ? m->same_run + 1 : 0;
	if (m->same_run >= MAG_STUCK_SAMPLES) {
		return mag_halt(m, MAG_STOP_STUCK, m->same_run);
	}
	memcpy(m->last, b, sizeof(b));
	/* The next conversion runs while the caller sleeps */
	if (m->bus.write(m->bus.ctx, MAG_REG_CTRL0, MAG_MEASURE)) {
		return mag_halt(m, MAG_STOP_BUS, MAG_MEASURE);
	}
	m->pending = true;
	mag_decode(b, c);
	mag_to_ios_nt(c, flip, nt);
	m->samples++;
	return true;
}

#endif /* BLAZER */
