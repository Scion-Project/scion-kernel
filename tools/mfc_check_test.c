/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/mfc_check_test.c: drivers/media/mfc/hakios.mfc_check.h on a computer, under the sanitizers (tools/mfc-check-test.sh).
 * What it checks, with what hakios.exynos_mfc.c then does with the numbers worked out the same way:
 *   - a crop the check lets through leaves a picture of 1 to width columns and 1 to height rows, and the copy out of it
 *     (frame_out(): rows crop_y .. crop_y + crop_h, columns crop_x .. crop_x + crop_w, of an NV12 frame of the driver's
 *     stride and planes) stays inside the frame: run on a frame buffer of exactly that size; and every crop that would
 *     wrap is refused (every left/right pair around the width, and random ones);
 *   - buffer sizes: what the check lets through rounds up without wrapping, MV counts past the registers are refused;
 *   - mfc_inside() at both ends of a range and across 2^64.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/media/mfc/hakios.mfc_check.h"

#define ROUND_UP(x, a) (((x) + (uint32_t)(a) - 1) & ~((uint32_t)(a) - 1))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static unsigned checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 20261117;

static uint32_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* inst_header()'s planes for width x height (H.264), then frame_out()'s reads of the crop from a frame of exactly that size */
static void
copy_out(uint32_t width, uint32_t height, uint32_t c1, uint32_t c2)
{
	uint32_t stride = ROUND_UP(width, 16), rows = ROUND_UP(height, 16);
	uint32_t plane0 = ROUND_UP(stride * ROUND_UP(rows, 32), 256), plane1 = ROUND_UP(stride * (rows / 2 + 16), 256);
	uint32_t x = c1 & 0xffff, w = width - (c1 & 0xffff) - (c1 >> 16), y = c2 & 0xffff, h = height - (c2 & 0xffff) - (c2 >> 16);
	uint8_t *frame = malloc(plane0 + plane1), *row = malloc(w), *luma = frame, *chroma = frame + plane0;
	uint32_t sum = 0;

	CHECK(w >= 1 && w <= width && h >= 1 && h <= height && w <= stride);
	memset(frame, 1, plane0 + plane1);
	for (uint32_t i = 0; i < h; i++) {
		CHECK((uint64_t)(i + y) * stride + x + w <= plane0);
		memcpy(row, luma + (i + y) * stride + x, w);
		sum += row[0];
	}
	for (uint32_t i = 0; i < h / 2; i++) {
		CHECK((uint64_t)(i + y / 2) * stride + (x & ~1u) + w <= plane1);
		memcpy(row, chroma + (i + y / 2) * stride + (x & ~1u), w);
		sum += row[0];
	}
	CHECK(sum == h + h / 2);
	free(row);
	free(frame);
}

int
main(void)
{
	static const uint32_t sizes[][2] = { { 1, 1 }, { 2, 2 }, { 16, 16 }, { 176, 144 }, { 1280, 720 }, { 1920, 1080 }, { 1920, 1088 },
		{ 4096, 2160 }, { 4096, 4096 }, { 33, 4095 } };

	for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		uint32_t width = sizes[k][0], height = sizes[k][1];

		/* every left/right pair around the edge: the last one that fits, and the first that does not */
		for (uint32_t left = 0; left <= width + 2 && left < 4200; left += 1 + left / 64) {
			for (uint32_t right = 0; right <= width + 2; right += 1 + right / 64) {
				bool ok = mfc_crop_inside(width, height, right << 16 | left, 0);

				CHECK(ok == (left + right < width));
			}
		}
		CHECK(!mfc_crop_inside(width, height, 0xffffu << 16 | 0xffffu, 0));
		CHECK(!mfc_crop_inside(width, height, 0, 0xffffu << 16 | 0xffffu));
		CHECK(mfc_crop_inside(width, height, 0, 0));
		for (uint32_t it = 0; it < 2000; it++) {
			uint32_t c1 = rnd() % 3 ? (rnd() % (width + 1)) << 16 | rnd() % (width + 1) : rnd();
			uint32_t c2 = rnd() % 3 ? (rnd() % (height + 1)) << 16 | rnd() % (height + 1) : rnd();

			if (mfc_crop_inside(width, height, c1, c2)) {
				if (it < 60 || width * height < 100000) {
					copy_out(width, height, c1, c2);
				}
			} else {
				CHECK((c1 & 0xffff) + (c1 >> 16) >= width || (c2 & 0xffff) + (c2 >> 16) >= height);
			}
		}
		copy_out(width, height, 0, 0);
	}
	/* sizes: anything let through rounds up to 256 without wrapping; MV counts past the 64 registers refused */
	for (uint32_t it = 0; it < 1000000; it++) {
		uint32_t a = rnd() % 4 ? rnd() % (MFC_BUFFER_MAX + 2) : rnd(), b = rnd(), c = rnd() % (MFC_BUFFER_MAX + 2), mv = rnd() % 70;
		bool ok = mfc_sizes_sane(a, b % (MFC_BUFFER_MAX + 2), c, mv, 64);

		if (ok) {
			CHECK(ROUND_UP(a, 256) >= a && ROUND_UP(c, 256) >= c && mv <= 64);
		}
		CHECK(ok == (a <= MFC_BUFFER_MAX && b % (MFC_BUFFER_MAX + 2) <= MFC_BUFFER_MAX && c <= MFC_BUFFER_MAX && mv <= 64));
	}
	CHECK(!mfc_sizes_sane(0xffffff01u, 0, 0, 0, 64));
	CHECK(!mfc_sizes_sane(0, 0, 0, 65, 64));
	CHECK(mfc_sizes_sane(MFC_BUFFER_MAX, MFC_BUFFER_MAX, MFC_BUFFER_MAX, 64, 64));
	/* ranges: the MFC's 128 MB at 0xC4000000 */
	{
		const uint64_t lo = 0xC4000000ull, hi = 0xCC000000ull;

		CHECK(mfc_inside(lo, hi - lo, lo, hi));
		CHECK(mfc_inside(hi, 0, lo, hi));
		CHECK(!mfc_inside(hi, 1, lo, hi));
		CHECK(!mfc_inside(lo - 1, 1, lo, hi));
		CHECK(!mfc_inside(hi - 256, 257, lo, hi));
		CHECK(!mfc_inside(0xffffffffffffff00ull, 0x200, lo, hi));
		CHECK(!mfc_inside(lo, 0xffffffffffffffffull, lo, hi));
		for (uint32_t it = 0; it < 1000000; it++) {
			uint64_t at = lo - 0x10000 + rnd() % 0x8020000u, bytes = rnd() % 0x1000000u;

			CHECK(mfc_inside(at, bytes, lo, hi) == (at >= lo && at + bytes <= hi));
		}
	}
	printf("mfc_check_test: %u checks, %u failed\n", checks, failures);
	return failures != 0;
}
