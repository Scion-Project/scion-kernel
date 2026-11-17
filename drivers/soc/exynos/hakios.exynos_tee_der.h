/*
 * The DER reader the Galaxy S9's secure-OS probe (hakios.exynos_tee.c, tee_probe=4) reads Samsung's keymaster TA's
 * answers with, in plain C with no kernel headers, so that tools/exynos_tee_der_test.c checks it on a computer
 * (tools/exynos-tee-der-test.sh). The answer comes from the secure world through shared memory: every tag, length and
 * count in it is taken as hostile, and nothing here reads outside [0, limit).
 *
 *   der_next          one TLV at *at of a buffer of limit bytes: its tag, where its contents begin and how long they
 *                     are (short form, or long form of one to three length bytes); false, and nothing read past
 *                     limit, when it does not fit
 *   der_read_integer  an INTEGER's contents, sign-extended, at most nine bytes of them
 *
 * Moved here unchanged from hakios.exynos_tee.c, which writes its requests with der_raw, der_wrap and the rest (they
 * are bounded by its own buffer, KM_DER_MAX) and parses the answers with these.
 */
#ifndef _HAKIOS_EXYNOS_TEE_DER_H_
#define _HAKIOS_EXYNOS_TEE_DER_H_

#include <stdbool.h>
#include <stdint.h>

/* A TLV at *at of limit: its tag, contents and length; *at past it. */
static bool
der_next(const uint8_t *base, uint32_t limit, uint32_t *at, uint8_t *tag, uint32_t *contents, uint32_t *length)
{
	uint32_t i = *at, l;

	if (i + 2 > limit) {
		return false;
	}
	*tag = base[i++];
	l = base[i++];
	if (l & 0x80) {
		uint32_t count = l & 0x7f;

		if (count == 0 || count > 3 || i + count > limit) {
			return false;
		}
		l = 0;
		while (count-- > 0) {
			l = (l << 8) | base[i++];
		}
	}
	if (l > limit - i) {
		return false;
	}
	*contents = i;
	*length = l;
	*at = i + l;
	return true;
}

static int64_t
der_read_integer(const uint8_t *base, uint32_t contents, uint32_t length)
{
	uint64_t v = (length > 0 && (base[contents] & 0x80)) ? ~0ull : 0;

	for (uint32_t i = 0; i < length && i < 9; i++) {
		v = (v << 8) | base[contents + i];
	}
	return (int64_t)v;
}

#endif /* _HAKIOS_EXYNOS_TEE_DER_H_ */
