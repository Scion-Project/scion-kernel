/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-phaseb-r1/pwr/patches/0003-blazer-haptic-tap.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * Tap feedback policy for blazer_haptic_tap=1, no XNU types: the host test runs this exact logic.
 * A new touch contact asks for one ROM effect. The worker plays it through blazer_haptic_play_rom() (0832), at most one
 * per HH_TAP_GAP_MS and HH_TAP_MAX per boot. EBUSY skips that tap; any other error turns tap feedback off for the boot. */
#ifndef BLAZER_HAPTIC_TAP_H
#define BLAZER_HAPTIC_TAP_H
#include <stdbool.h>
#include <stdint.h>

#define HH_TAP_GAP_MS   250u    /* a fast double tap gives one buzz, not two overlapping 100 ms effects */
#define HH_TAP_MAX      300u    /* bound for one attended boot */
#define HH_TAP_EBUSY    16      /* errno values, as sys/errno.h */

struct hh_tap {
	uint64_t last_ms;
	uint32_t sent, skipped, refused;
	int last_rc;
	bool played, off;
};

/* 1: play now. 0: drop this request (too soon after the last one, budget used, or off). */
static inline int
hh_tap_due(struct hh_tap *t, uint64_t now_ms)
{
	if (t->off || t->sent >= HH_TAP_MAX) {
		t->skipped++;
		return 0;
	}
	if (t->played && now_ms - t->last_ms < HH_TAP_GAP_MS) {
		t->skipped++;
		return 0;
	}
	return 1;
}

/* After blazer_haptic_play_rom(): rc 0 played, EBUSY skipped, anything else ends tap feedback. */
static inline void
hh_tap_done(struct hh_tap *t, uint64_t now_ms, int rc)
{
	t->last_rc = rc;
	if (rc == 0) {
		t->sent++;
		t->played = true;
		t->last_ms = now_ms;
	} else if (rc == HH_TAP_EBUSY) {
		t->skipped++;
	} else {
		t->refused++;
		t->off = true;
	}
	if (t->sent >= HH_TAP_MAX) {
		t->off = true;
	}
}
#endif
