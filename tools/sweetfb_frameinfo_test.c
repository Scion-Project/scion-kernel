/* SPDX-License-Identifier: MIT-0 */
/*
 * Host test of drivers/display/sweetfb/hakios.sweetfb_frameinfo.h, the framebuffer stand-in's frame-info answers
 * (sweetfb_frameinfo=N): the key list IOMFB reads as iOS 27.0.1's InfoKeyInitialize does (index 0 first, give up on a
 * failure or a count of 0), the mask its ChangeFrameInfo calls build, and every notification decoded as its
 * FrameInfoNotifyFunc does (mask, swap id, one value per bit in index order), for every boot-argument value, every mask
 * and every combination of known and unknown values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/display/sweetfb/hakios.sweetfb_frameinfo.h"

static int failures;

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

/* IOMFB's side: the names it would hold after InfoKeyInitialize, or 0 names when it gives up */
static unsigned
read_keys(uint32_t on, char names[8][SFI_NAME_BYTES + 1])
{
	uint64_t count = 99, mode = 99;
	char name[SFI_NAME_BYTES];
	int r = sfi_supported(on, 0, &count, &mode, name);
	CHECK(mode == 0, "on %u: mode %llu", on, (unsigned long long)mode);
	if (r != 0 || count == 0) {
		CHECK(count == 0, "on %u: index 0 refused with a count of %llu", on, (unsigned long long)count);
		return 0;
	}
	CHECK(count <= SFI_KEYS, "on %u: count %llu", on, (unsigned long long)count);
	memcpy(names[0], name, SFI_NAME_BYTES);
	names[0][SFI_NAME_BYTES] = 0;
	for (uint64_t i = 1; i < count; i++) {
		r = sfi_supported(on, i, &count, &mode, name);
		CHECK(r == 0, "on %u: index %llu refused", on, (unsigned long long)i);
		memcpy(names[i], name, SFI_NAME_BYTES);
		names[i][SFI_NAME_BYTES] = 0;
	}
	r = sfi_supported(on, count, &count, &mode, name);
	CHECK(r != 0, "on %u: index %llu past the end answered", on, (unsigned long long)count);
	for (unsigned i = 0; i < SFI_NAME_BYTES; i++) {
		CHECK(name[i] == 0, "on %u: refused index left a name", on);
	}
	return (unsigned)count;
}

/* CoreAnimation's side: enable each wanted tag the list offers (enable_frame_info_tag_list, figure_frame_info_mask) */
static uint32_t
enable(unsigned n, char names[8][SFI_NAME_BYTES + 1], uint32_t want_bits)
{
	static const char *const wanted[] = { "Presentation_time", "Presentation_delta", "Requested_presentation",
		                              "Performance_headroom", "Thermal_throttle", "Min_FrameTime", "Max_FrameTime",
		                              "Vbl_FrameTime", "Last_Fence_or_Event_Time" };
	uint32_t mask = 0;
	for (unsigned w = 0; w < sizeof(wanted) / sizeof(wanted[0]); w++) {
		if (!((want_bits >> w) & 1)) {
			continue;
		}
		for (unsigned i = 0; i < n; i++) {
			if (strncmp(names[i], wanted[w], SFI_NAME_BYTES) == 0) {
				mask |= 1u << i;
			}
		}
	}
	return mask;
}

int
main(void)
{
	unsigned messages = 0;
	for (uint32_t on = 0; on < 64; on++) {
		char names[8][SFI_NAME_BYTES + 1];
		memset(names, 0, sizeof(names));
		unsigned n = read_keys(on, names);
		CHECK(n == sfi_count(on), "on %u: %u keys read, %u offered", on, n, sfi_count(on));
		if ((on & 3) == 0) {
			CHECK(n == 0, "on %u (off): %u keys offered", on, n);
		}
		for (unsigned i = 0; i < n; i++) {
			int k = sfi_key_at(on, i);
			CHECK(k >= 0 && strcmp(names[i], sfi_key_name[k]) == 0, "on %u: index %u is '%s'", on, i, names[i]);
			for (unsigned j = 0; j < i; j++) {
				CHECK(strcmp(names[i], names[j]) != 0, "on %u: '%s' twice", on, names[i]);
			}
		}
		for (uint32_t want = 0; want < 512; want += 1) {
			uint32_t mask = sfi_mask(on, enable(n, names, want));
			CHECK(mask == enable(n, names, want), "on %u want %#x: mask cut", on, want);
			CHECK(sfi_mask(on, 0xffffffffffffffffull) == (n ? (1u << n) - 1 : 0), "on %u: all-ones mask", on);
			for (unsigned known = 0; known < 4; known++) {
				struct sfi_frame f = { .swap_id = 1000 + want, .value = { 0, 0 } };
				if (known & 1) {
					f.value[SFI_KEY_PRESENTED] = 0x1234567890ull + want;
				}
				if (known & 2) {
					f.value[SFI_KEY_FENCE] = 0x1234000000ull + want;
				}
				uint64_t out[SFI_ARGS + 4];
				memset(out, 0xa5, sizeof(out));
				uint32_t c = sfi_message(on, mask, &f, out);
				CHECK(c <= SFI_ARGS, "on %u: %u arguments", on, c);
				for (unsigned i = c; i < SFI_ARGS + 4; i++) {
					CHECK(out[i] == 0xa5a5a5a5a5a5a5a5ull, "on %u: wrote past %u", on, c);
				}
				int pt_index = -1;
				for (unsigned i = 0; i < n; i++) {
					if (sfi_key_at(on, i) == SFI_KEY_PRESENTED) {
						pt_index = (int)i;
					}
				}
				int should = pt_index >= 0 && ((mask >> pt_index) & 1) && (known & 1);
				CHECK((c != 0) == should, "on %u mask %#x known %u: sent %u", on, mask, known, c);
				if (c == 0) {
					continue;
				}
				messages++;
				/* IOMFB's FrameInfoNotifyFunc: at least 2 arguments, the mask (bit 31: split 32-bit values, never here) */
				CHECK(c >= 2 && !(out[0] & 0x80000000u), "on %u: header", on);
				CHECK(out[1] == f.swap_id, "on %u: swap id %llu", on, (unsigned long long)out[1]);
				CHECK((out[0] & ~(uint64_t)mask) == 0, "on %u: sent %#llx outside the mask %#x", on,
				    (unsigned long long)out[0], mask);
				CHECK(out[0] & 1, "on %u: Presentation_time missing", on);
				unsigned a = 2;
				for (unsigned i = 0; i < 32; i++) {
					if (!((out[0] >> i) & 1)) {
						continue;
					}
					CHECK(i < n && a < c, "on %u: bit %u has no key or no value", on, i);
					if (i >= n || a >= c) {
						break;
					}
					int k = sfi_key_at(on, i);
					CHECK(out[a] == f.value[k] && out[a] != 0, "on %u: %s = %#llx", on, sfi_key_name[k],
					    (unsigned long long)out[a]);
					a++;
				}
				CHECK(a == c, "on %u: %u values for %u arguments", on, a - 2, c);
				if ((mask >> 1) & 1 && sfi_key_at(on, 1) == SFI_KEY_FENCE) {
					CHECK(((out[0] >> 1) & 1) == ((known >> 1) & 1), "on %u: fence sent %llu known %u", on,
					    (unsigned long long)((out[0] >> 1) & 1), known);
				}
			}
		}
	}
	CHECK(messages > 0, "no message was ever built");
	printf("sweetfb_frameinfo: %u messages checked, %d failures\n", messages, failures);
	return failures != 0;
}
