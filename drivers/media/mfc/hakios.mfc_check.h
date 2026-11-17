// SPDX-License-Identifier: MIT-0
/*
 * drivers/media/mfc/hakios.mfc_check.h: the checks hakios.exynos_mfc.c makes on what the firmware reports about a client's
 * stream (its crop, its buffer sizes) and on a client's zero-copy surface (where its planes are), as plain C so that
 * tools/mfc-check-test.sh runs them on a computer. The stream is the client's: every number the firmware reads out of it is
 * as untrusted as the stream.
 */
#ifndef HAKIOS_MFC_CHECK_H
#define HAKIOS_MFC_CHECK_H

/* the most bytes one buffer size the firmware asks for may be (a plane, the scratch): a 4096 x 4096 picture's plane is 16 MB */
#define MFC_BUFFER_MAX          (64u << 20)

/*
 * A crop that leaves a picture inside width x height: CROP1 holds right << 16 | left, CROP2 bottom << 16 | top. Otherwise the
 * picture's width or height (width - left - right) wraps, and the copy out reads rows and columns past the frame.
 */
static inline bool
mfc_crop_inside(uint32_t width, uint32_t height, uint32_t crop1, uint32_t crop2)
{
	return (uint64_t)(crop1 & 0xffffu) + (crop1 >> 16) < width && (uint64_t)(crop2 & 0xffffu) + (crop2 >> 16) < height;
}

/*
 * The firmware's buffer sizes for a stream: each plane's and the scratch's at most MFC_BUFFER_MAX (so that rounding them up does
 * not wrap to a small size), and at most mv_max MV buffers (the D_MV registers there are).
 */
static inline bool
mfc_sizes_sane(uint32_t min_plane0, uint32_t min_plane1, uint32_t min_scratch, uint32_t mv_count, uint32_t mv_max)
{
	return min_plane0 <= MFC_BUFFER_MAX && min_plane1 <= MFC_BUFFER_MAX && min_scratch <= MFC_BUFFER_MAX && mv_count <= mv_max;
}

/* [at, at + bytes) inside [lo, hi), without a sum that can wrap */
static inline bool
mfc_inside(uint64_t at, uint64_t bytes, uint64_t lo, uint64_t hi)
{
	return lo <= hi && at >= lo && at <= hi && bytes <= hi - at;
}

#endif /* HAKIOS_MFC_CHECK_H */
