/* SPDX-License-Identifier: MIT-0 */
/*
 * The S9's memory-to-memory scaler and colour-space converter, the part that touches pixels: plain C over two images in
 * memory, no kernel (a codec: tools/arch-layers.conf). The kernel service that iOS's IOSurfaceAccelerator opens
 * (hakios.m2m_scaler.cpp, "AppleM2ScalerCSCDriver") hands it the two surfaces' planes; tools/m2m/m2m_test.c runs it on
 * the build machine against reference pictures. docs/ios27/M2M-SCALER.md has the interface and the design.
 *
 * What it does, all of it, and nothing it does not:
 *   - formats: Y'CbCr 4:2:0, two planes (luma, then Cb Cr interleaved), 8 bits ('420v' video range, '420f' full range)
 *     or 10 bits in 16-bit little-endian samples, high bits ('x420' video, 'xf20' full); any of them into any of them,
 *     the range and the depth converted (H.273's equations for video and full range; no change of colour matrix, which
 *     Y'CbCr to Y'CbCr of one matrix does not need);
 *   - a source rectangle in 16.16 fixed point (fractional origin and size), scaled to a destination rectangle in whole
 *     pixels, at most M2M_MAX_UPSCALE times up and M2M_MAX_DOWNSCALE times down on each axis;
 *   - turned by 90 degrees clockwise and/or flipped (the Transform bits: 1 flip vertical, 2 flip horizontal, 4 rotate;
 *     180 degrees is both flips, 270 is all three), the flips applied after the turn, in the destination;
 *   - filtered bilinearly (2 taps per axis, 8-bit weights: 256 phases) going up and going down by less than 2 (a
 *     bilinear tap already spans 2 source samples); going down by 2 or more on an axis, the source footprint of each
 *     destination sample is averaged over ceil(ratio) bilinear taps on that axis (a box filter); or nearest-neighbour;
 *   - chroma sited as 4:2:0 video is ("left": co-sited with the even luma column, half way between two luma rows) in both
 *     pictures, the turn and flips taken into account sample by sample;
 *   - samples outside the destination rectangle untouched, or set to black (Y' at black, Cb Cr at the centre) when
 *     M2M_OPT_ZERO_FILL is asked.
 * The numbers the service publishes (m2m_caps) are the constants this file checks against.
 */
#ifndef HAKIOS_M2M_CORE_H
#define HAKIOS_M2M_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the kernel builds with -fno-builtin: ask for the inline copy (a 2-byte load or store) by name */
#if defined(__GNUC__) || defined(__clang__)
#define M2M_COPY(d, s, n)       __builtin_memcpy((d), (s), (n))
#else
#define M2M_COPY(d, s, n)       memcpy((d), (s), (n))
#endif

#define M2M_FOURCC(a, b, c, d)  (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define M2M_FMT_420V            M2M_FOURCC('4', '2', '0', 'v')
#define M2M_FMT_420F            M2M_FOURCC('4', '2', '0', 'f')
#define M2M_FMT_X420            M2M_FOURCC('x', '4', '2', '0')
#define M2M_FMT_XF20            M2M_FOURCC('x', 'f', '2', '0')

#define M2M_MAX_UPSCALE         16u     /* destination / source, each axis */
#define M2M_MAX_DOWNSCALE       16u     /* source / destination, each axis (= the most taps of the box) */
#define M2M_MIN_SIZE            2u      /* pixels, source (whole part of its size) and destination, each axis */
#define M2M_MAX_SIZE            8192u   /* pixels, a surface's width or height */
#define M2M_FILTER_TAPS         2u      /* bilinear */
#define M2M_FILTER_PHASES       256u    /* 8-bit weights */
#define M2M_COEF_PRE_BITS       1u      /* a weight is 0 .. 1 */
#define M2M_COEF_POST_BITS      8u

#define M2M_XF_FLIP_V           1u
#define M2M_XF_FLIP_H           2u
#define M2M_XF_ROTATE_90        4u
#define M2M_XF_ALL              7u

#define M2M_OPT_ZERO_FILL       1u      /* black outside the destination rectangle */
#define M2M_OPT_NEAREST         2u      /* nearest-neighbour instead of bilinear / box */

enum {
	M2M_OK = 0,
	M2M_EFORMAT,            /* a format it does not do */
	M2M_EPLANE,             /* a plane smaller than its picture, or not there */
	M2M_ERECT,              /* a rectangle outside its picture, too small, or a destination origin that is odd */
	M2M_ESCALE,             /* more than the caps allow */
	M2M_ETRANSFORM,         /* Transform bits it does not know */
};

struct m2m_plane {
	uint8_t  *base;
	uint64_t  size;         /* bytes from base that may be read or written */
	uint32_t  bytes_per_row;
	uint32_t  width, height;        /* samples (a Cb Cr pair is one) */
};

struct m2m_image {
	uint32_t format;
	uint32_t width, height;
	struct m2m_plane plane[2];      /* 0 luma, 1 chroma */
};

struct m2m_job {
	struct m2m_image src, dst;
	uint64_t sx, sy, sw, sh;        /* source rectangle, 16.16; sw or sh 0: the whole source */
	uint32_t dx, dy, dw, dh;        /* destination rectangle; dw or dh 0: the whole destination */
	uint32_t transform;             /* M2M_XF_* */
	uint32_t options;               /* M2M_OPT_* */
};

struct m2m_caps {
	uint32_t max_upscale, max_downscale;
	uint32_t min_size, max_size;
	uint32_t taps, phases, coef_pre_bits, coef_post_bits;
	uint32_t fractional;            /* a source rectangle may have a fractional origin and size */
	uint32_t ten_bit;               /* 4:2:0 two planes, 10 bits, in and out */
};

static inline void
m2m_caps_get(struct m2m_caps *c)
{
	c->max_upscale = M2M_MAX_UPSCALE;
	c->max_downscale = M2M_MAX_DOWNSCALE;
	c->min_size = M2M_MIN_SIZE;
	c->max_size = M2M_MAX_SIZE;
	c->taps = M2M_FILTER_TAPS;
	c->phases = M2M_FILTER_PHASES;
	c->coef_pre_bits = M2M_COEF_PRE_BITS;
	c->coef_post_bits = M2M_COEF_POST_BITS;
	c->fractional = 1;
	c->ten_bit = 1;
}

/* Depth (8 or 10) and range (1 full, 0 video) of a format; 0 if it is not one of the four. */
static inline int
m2m_format_info(uint32_t format, unsigned *bits, unsigned *full)
{
	switch (format) {
	case M2M_FMT_420V: *bits = 8; *full = 0; return 1;
	case M2M_FMT_420F: *bits = 8; *full = 1; return 1;
	case M2M_FMT_X420: *bits = 10; *full = 0; return 1;
	case M2M_FMT_XF20: *bits = 10; *full = 1; return 1;
	default: return 0;
	}
}

/* Y' black, the span from black to white, the chroma centre and its span from -0.5 to +0.5, all doubled (a full-range
 * chroma span, 2^n - 1, is odd: doubling keeps everything whole). H.273: video Y' 219 << (n - 8) from 16 << (n - 8),
 * chroma 224 << (n - 8); full Y' 2^n - 1 from 0, chroma 2^n - 1; the chroma centre 2^(n-1). */
struct m2m_range {
	int64_t y_black2, y_span2, c_mid2, c_span2;
	int32_t max;            /* the largest code */
	unsigned bits;
};

static inline void
m2m_range_get(uint32_t format, struct m2m_range *r)
{
	unsigned bits = 8, full = 0;

	(void)m2m_format_info(format, &bits, &full);
	r->bits = bits;
	r->max = (int32_t)((1u << bits) - 1);
	r->c_mid2 = (int64_t)2 << (bits - 1);
	if (full) {
		r->y_black2 = 0;
		r->y_span2 = 2 * (int64_t)r->max;
		r->c_span2 = 2 * (int64_t)r->max;
	} else {
		r->y_black2 = (int64_t)32 << (bits - 8);
		r->y_span2 = (int64_t)438 << (bits - 8);
		r->c_span2 = (int64_t)448 << (bits - 8);
	}
}

/* The source's code (times 256, from the filter) to the destination's: d = black_d + (s - black_s) * span_d / span_s,
 * rounded half up, kept in 0 .. max. Identity when both are the same range and depth. */
static inline int32_t
m2m_convert(int64_t s256, int64_t s_off2, int64_t s_span2, int64_t d_off2, int64_t d_span2, int32_t max)
{
	/* d = d_off2/2 + (s256/256 - s_off2/2) * d_span2 / s_span2 */
	int64_t num = (2 * s256 - s_off2 * 256) * d_span2 + d_off2 * 256 * s_span2;     /* d * 512 * s_span2 */
	int64_t den = 512 * s_span2;
	int64_t d = num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);

	return d < 0 ? 0 : d > max ? max : (int32_t)d;
}

static inline uint32_t
m2m_read(const struct m2m_plane *p, unsigned bits, uint32_t x, uint32_t y, unsigned comp, unsigned comps)
{
	const uint8_t *row = p->base + (size_t)y * p->bytes_per_row;

	if (bits == 8) {
		return row[x * comps + comp];
	}
	uint16_t v;
	M2M_COPY(&v, row + 2 * ((size_t)x * comps + comp), sizeof(v));
	return (uint32_t)(v >> 6);
}

static inline void
m2m_write(const struct m2m_plane *p, unsigned bits, uint32_t x, uint32_t y, unsigned comp, unsigned comps, int32_t code)
{
	uint8_t *row = p->base + (size_t)y * p->bytes_per_row;

	if (bits == 8) {
		row[x * comps + comp] = (uint8_t)code;
		return;
	}
	uint16_t v = (uint16_t)((uint32_t)code << 6);
	M2M_COPY(row + 2 * ((size_t)x * comps + comp), &v, sizeof(v));
}

/* Geometry of one plane of the transfer: where a destination sample's centre falls in the source, in the plane's own
 * sample index (0.0 is the first sample's centre), as 32.32 fixed point. */
struct m2m_geom {
	int64_t sx, sy;                 /* source rectangle origin, 32.32, luma pixels */
	int64_t kx, ky;                 /* source luma pixels per pixel of the turned destination, 32.32 */
	int64_t uw2, uh2;               /* twice the turned destination's width and height */
	uint32_t transform;
	int64_t lo_x, hi_x, lo_y, hi_y; /* sample indices the filter may read (the source rectangle, in the plane) */
	unsigned nx, ny;                /* taps of the box, each axis */
};

static inline int64_t
m2m_floor32(int64_t v)
{
	return v >> 32;         /* arithmetic: floor */
}

/* Source position (luma pixels, 32.32) of the destination point (a2/2, b2/2) of the destination rectangle. */
static inline void
m2m_map(const struct m2m_geom *g, uint32_t dw, uint32_t dh, int64_t a2, int64_t b2, int64_t *px, int64_t *py)
{
	int64_t u2, v2;

	if (g->transform & M2M_XF_FLIP_H) {
		a2 = 2 * (int64_t)dw - a2;
	}
	if (g->transform & M2M_XF_FLIP_V) {
		b2 = 2 * (int64_t)dh - b2;
	}
	if (g->transform & M2M_XF_ROTATE_90) {
		/* the destination is the source turned clockwise: its column a came from the source's row (dw - a) */
		u2 = b2;
		v2 = 2 * (int64_t)dw - a2;
	} else {
		u2 = a2;
		v2 = b2;
	}
	*px = g->sx + (u2 * g->kx) / 2;
	*py = g->sy + (v2 * g->ky) / 2;
}

/* Bilinear sample (times 65536) of one component at plane index (x, y) in 32.32, clamped into the rectangle. */
static inline uint32_t
m2m_bilinear(const struct m2m_plane *p, unsigned bits, unsigned comp, unsigned comps, const struct m2m_geom *g,
    int64_t x, int64_t y)
{
	int64_t x0 = m2m_floor32(x), y0 = m2m_floor32(y);
	uint32_t fx = (uint32_t)((x >> 24) & 0xff), fy = (uint32_t)((y >> 24) & 0xff);
	int64_t x1 = x0 + 1, y1 = y0 + 1;

	if (x0 < g->lo_x) { x0 = g->lo_x; } else if (x0 > g->hi_x) { x0 = g->hi_x; }
	if (x1 < g->lo_x) { x1 = g->lo_x; } else if (x1 > g->hi_x) { x1 = g->hi_x; }
	if (y0 < g->lo_y) { y0 = g->lo_y; } else if (y0 > g->hi_y) { y0 = g->hi_y; }
	if (y1 < g->lo_y) { y1 = g->lo_y; } else if (y1 > g->hi_y) { y1 = g->hi_y; }

	uint32_t a = m2m_read(p, bits, (uint32_t)x0, (uint32_t)y0, comp, comps);
	uint32_t b = m2m_read(p, bits, (uint32_t)x1, (uint32_t)y0, comp, comps);
	uint32_t c = m2m_read(p, bits, (uint32_t)x0, (uint32_t)y1, comp, comps);
	uint32_t d = m2m_read(p, bits, (uint32_t)x1, (uint32_t)y1, comp, comps);
	uint32_t top = a * (256 - fx) + b * fx, bottom = c * (256 - fx) + d * fx;

	return top * (256 - fy) + bottom * fy;
}

static inline uint32_t
m2m_nearest(const struct m2m_plane *p, unsigned bits, unsigned comp, unsigned comps, const struct m2m_geom *g,
    int64_t x, int64_t y)
{
	int64_t xi = m2m_floor32(x + ((int64_t)1 << 31)), yi = m2m_floor32(y + ((int64_t)1 << 31));

	if (xi < g->lo_x) { xi = g->lo_x; } else if (xi > g->hi_x) { xi = g->hi_x; }
	if (yi < g->lo_y) { yi = g->lo_y; } else if (yi > g->hi_y) { yi = g->hi_y; }
	return m2m_read(p, bits, (uint32_t)xi, (uint32_t)yi, comp, comps) << 16;
}

/* The source position, as this plane's sample index (32.32), of the destination sample (i, k) of the plane. */
static inline void
m2m_index_of(const struct m2m_job *j, const struct m2m_geom *g, int chroma, uint32_t i, uint32_t k, int64_t *x, int64_t *y)
{
	int64_t a2 = chroma ? 4 * (int64_t)i + 1 - 2 * (int64_t)j->dx : 2 * ((int64_t)i - j->dx) + 1;
	int64_t b2 = chroma ? 4 * (int64_t)k + 2 - 2 * (int64_t)j->dy : 2 * ((int64_t)k - j->dy) + 1;
	int64_t px, py;

	m2m_map(g, j->dw, j->dh, a2, b2, &px, &py);
	if (chroma) {
		*x = (px - ((int64_t)1 << 31)) / 2;
		*y = (py - ((int64_t)1 << 32)) / 2;
	} else {
		*x = px - ((int64_t)1 << 31);
		*y = py - ((int64_t)1 << 31);
	}
}

#define M2M_TILE        32u     /* destination samples a side: a turned tile's source stays in the data cache */

/*
 * The common case, one bilinear tap: the same arithmetic as the general loop below (m2m_bilinear, the same rounding), with
 * the source position stepped along each destination row instead of mapped again, and the destination walked in tiles so
 * that a turn reads the source a cache-sized window at a time.
 */
static inline void
m2m_run_plane_bilinear(const struct m2m_job *j, int chroma, const struct m2m_geom *g, const struct m2m_range *sr,
    const struct m2m_range *dr, int identity, uint32_t i0, uint32_t i1, uint32_t k0, uint32_t k1)
{
	const struct m2m_plane *sp = &j->src.plane[chroma], *dp = &j->dst.plane[chroma];
	unsigned comps = chroma ? 2 : 1;
	int64_t sx0, sy0, sx1, sy1, dx, dy;

	if (i0 >= i1 || k0 >= k1) {
		return;
	}
	/* the step of one destination sample along a row, from two mapped points (exact: the map is affine) */
	m2m_index_of(j, g, chroma, i0, k0, &sx0, &sy0);
	m2m_index_of(j, g, chroma, i0 + 1, k0, &sx1, &sy1);
	dx = sx1 - sx0;
	dy = sy1 - sy0;
	for (uint32_t tk = k0; tk < k1; tk += M2M_TILE) {
		uint32_t kend = k1 - tk > M2M_TILE ? tk + M2M_TILE : k1;
		for (uint32_t ti = i0; ti < i1; ti += M2M_TILE) {
			uint32_t iend = i1 - ti > M2M_TILE ? ti + M2M_TILE : i1;
			for (uint32_t k = tk; k < kend; k++) {
				int64_t x, y;
				m2m_index_of(j, g, chroma, ti, k, &x, &y);
				for (uint32_t i = ti; i < iend; i++, x += dx, y += dy) {
					int64_t xa = m2m_floor32(x), ya = m2m_floor32(y), xb = xa + 1, yb = ya + 1;
					uint32_t fx = (uint32_t)((x >> 24) & 0xff), fy = (uint32_t)((y >> 24) & 0xff);
					if (xa < g->lo_x) { xa = g->lo_x; } else if (xa > g->hi_x) { xa = g->hi_x; }
					if (xb < g->lo_x) { xb = g->lo_x; } else if (xb > g->hi_x) { xb = g->hi_x; }
					if (ya < g->lo_y) { ya = g->lo_y; } else if (ya > g->hi_y) { ya = g->hi_y; }
					if (yb < g->lo_y) { yb = g->lo_y; } else if (yb > g->hi_y) { yb = g->hi_y; }
					if (identity && sr->bits == 8) {
						/* 8 bits in, the same out: the reads and the write as bytes */
						const uint8_t *ra = sp->base + (size_t)ya * sp->bytes_per_row;
						const uint8_t *rb = sp->base + (size_t)yb * sp->bytes_per_row;
						uint8_t *o = dp->base + (size_t)k * dp->bytes_per_row + (size_t)i * comps;
						size_t ca = (size_t)xa * comps, cb = (size_t)xb * comps;
						for (unsigned c = 0; c < comps; c++) {
							uint32_t v = (ra[ca + c] * (256 - fx) + ra[cb + c] * fx) * (256 - fy) +
							    (rb[ca + c] * (256 - fx) + rb[cb + c] * fx) * fy;
							o[c] = (uint8_t)((((v + 128) >> 8) + 128) >> 8);
						}
						continue;
					}
					for (unsigned c = 0; c < comps; c++) {
						uint32_t a = m2m_read(sp, sr->bits, (uint32_t)xa, (uint32_t)ya, c, comps);
						uint32_t b = m2m_read(sp, sr->bits, (uint32_t)xb, (uint32_t)ya, c, comps);
						uint32_t e = m2m_read(sp, sr->bits, (uint32_t)xa, (uint32_t)yb, c, comps);
						uint32_t f = m2m_read(sp, sr->bits, (uint32_t)xb, (uint32_t)yb, c, comps);
						uint32_t v = (a * (256 - fx) + b * fx) * (256 - fy) + (e * (256 - fx) + f * fx) * fy;
						int64_t s256 = (int64_t)((v + 128) >> 8);
						int32_t code;
						if (identity) {
							code = (int32_t)((s256 + 128) >> 8);
						} else if (chroma) {
							code = m2m_convert(s256, sr->c_mid2, sr->c_span2, dr->c_mid2, dr->c_span2, dr->max);
						} else {
							code = m2m_convert(s256, sr->y_black2, sr->y_span2, dr->y_black2, dr->y_span2,
							    dr->max);
						}
						m2m_write(dp, dr->bits, i, k, c, comps, code);
					}
				}
			}
		}
	}
}

/*
 * One plane. chroma: the plane's samples are at half the luma rate both ways, sited at luma (2j + 0.5, 2k + 1); the
 * plane index of a luma position (x, y) is ((x - 0.5) / 2, (y - 1) / 2); for luma it is (x - 0.5, y - 0.5).
 */
static inline void
m2m_run_plane(const struct m2m_job *j, int chroma, const struct m2m_range *sr, const struct m2m_range *dr)
{
	const struct m2m_plane *sp = &j->src.plane[chroma], *dp = &j->dst.plane[chroma];
	unsigned comps = chroma ? 2 : 1;
	struct m2m_geom g;
	int identity = sr->bits == dr->bits && sr->y_span2 == dr->y_span2 && sr->c_span2 == dr->c_span2;
	uint32_t i0, i1, k0, k1;

	g.sx = (int64_t)j->sx << 16;
	g.sy = (int64_t)j->sy << 16;
	g.transform = j->transform;
	if (j->transform & M2M_XF_ROTATE_90) {
		g.kx = (int64_t)((j->sw << 16) / j->dh);
		g.ky = (int64_t)((j->sh << 16) / j->dw);
	} else {
		g.kx = (int64_t)((j->sw << 16) / j->dw);
		g.ky = (int64_t)((j->sh << 16) / j->dh);
	}
	g.nx = (j->options & M2M_OPT_NEAREST) || g.kx < ((int64_t)2 << 32) ? 1 : (unsigned)((g.kx + 0xffffffffLL) >> 32);
	g.ny = (j->options & M2M_OPT_NEAREST) || g.ky < ((int64_t)2 << 32) ? 1 : (unsigned)((g.ky + 0xffffffffLL) >> 32);

	/* the source rectangle's samples, in this plane */
	int64_t rx0 = (int64_t)(j->sx >> 16), ry0 = (int64_t)(j->sy >> 16);
	int64_t rx1 = (int64_t)((j->sx + j->sw + 0xffff) >> 16) - 1, ry1 = (int64_t)((j->sy + j->sh + 0xffff) >> 16) - 1;
	if (chroma) {
		rx0 /= 2; ry0 /= 2; rx1 /= 2; ry1 /= 2;
	}
	g.lo_x = rx0; g.hi_x = rx1 < (int64_t)sp->width - 1 ? rx1 : (int64_t)sp->width - 1;
	g.lo_y = ry0; g.hi_y = ry1 < (int64_t)sp->height - 1 ? ry1 : (int64_t)sp->height - 1;

	/* the destination samples whose centres are in the rectangle */
	if (chroma) {
		i0 = j->dx / 2; i1 = (j->dx + j->dw + 1) / 2;
		k0 = j->dy / 2; k1 = (j->dy + j->dh + 1) / 2;
	} else {
		i0 = j->dx; i1 = j->dx + j->dw;
		k0 = j->dy; k1 = j->dy + j->dh;
	}
	if (i1 > dp->width) { i1 = dp->width; }
	if (k1 > dp->height) { k1 = dp->height; }

	unsigned n = g.nx * g.ny;
	if (n == 1 && !(j->options & M2M_OPT_NEAREST)) {
		m2m_run_plane_bilinear(j, chroma, &g, sr, dr, identity, i0, i1, k0, k1);
		return;
	}
	for (uint32_t k = k0; k < k1; k++) {
		/* b2: twice the sample centre's luma row, from the rectangle's top */
		int64_t b2 = chroma ? 4 * (int64_t)k + 2 - 2 * (int64_t)j->dy : 2 * ((int64_t)k - j->dy) + 1;
		for (uint32_t i = i0; i < i1; i++) {
			int64_t a2 = chroma ? 4 * (int64_t)i + 1 - 2 * (int64_t)j->dx : 2 * ((int64_t)i - j->dx) + 1;
			int64_t px, py;
			m2m_map(&g, j->dw, j->dh, a2, b2, &px, &py);
			for (unsigned c = 0; c < comps; c++) {
				uint64_t acc = 0;
				for (unsigned ty = 0; ty < g.ny; ty++) {
					int64_t oy = g.ny == 1 ? 0 : ((int64_t)(2 * ty + 1) - (int64_t)g.ny) * (g.ky / (2 * (int64_t)g.ny));
					for (unsigned tx = 0; tx < g.nx; tx++) {
						int64_t ox = g.nx == 1 ? 0 :
						    ((int64_t)(2 * tx + 1) - (int64_t)g.nx) * (g.kx / (2 * (int64_t)g.nx));
						int64_t x = px + ox, y = py + oy;
						/* luma position to this plane's index */
						if (chroma) {
							x = (x - ((int64_t)1 << 31)) / 2;
							y = (y - ((int64_t)1 << 32)) / 2;
						} else {
							x -= (int64_t)1 << 31;
							y -= (int64_t)1 << 31;
						}
						acc += (j->options & M2M_OPT_NEAREST) ? m2m_nearest(sp, sr->bits, c, comps, &g, x, y) :
						    m2m_bilinear(sp, sr->bits, c, comps, &g, x, y);
					}
				}
				int64_t s256 = (int64_t)((acc / n + 128) >> 8);
				int32_t code;
				if (identity) {
					code = (int32_t)((s256 + 128) >> 8);
				} else if (chroma) {
					code = m2m_convert(s256, sr->c_mid2, sr->c_span2, dr->c_mid2, dr->c_span2, dr->max);
				} else {
					code = m2m_convert(s256, sr->y_black2, sr->y_span2, dr->y_black2, dr->y_span2, dr->max);
				}
				m2m_write(dp, dr->bits, i, k, c, comps, code);
			}
		}
	}
}

/* ZERO_FILL: black in every sample of a plane outside [i0, i1) x [k0, k1). */
static inline void
m2m_fill_outside(const struct m2m_plane *p, const struct m2m_range *r, int chroma, uint32_t i0, uint32_t i1, uint32_t k0,
    uint32_t k1)
{
	unsigned comps = chroma ? 2 : 1;
	int32_t v = chroma ? (int32_t)(r->c_mid2 / 2) : (int32_t)(r->y_black2 / 2);

	for (uint32_t k = 0; k < p->height; k++) {
		int inside_row = k >= k0 && k < k1;
		for (uint32_t i = 0; i < p->width; i++) {
			if (inside_row && i >= i0 && i < i1) {
				i = i1 - 1;
				continue;
			}
			for (unsigned c = 0; c < comps; c++) {
				m2m_write(p, r->bits, i, k, c, comps, v);
			}
		}
	}
}

static inline int
m2m_plane_ok(const struct m2m_plane *p, uint32_t width, uint32_t height, unsigned bits, unsigned comps)
{
	uint64_t row = (uint64_t)width * comps * (bits == 8 ? 1 : 2);

	return p->base != NULL && p->width >= width && p->height >= height && p->bytes_per_row >= row &&
	       (uint64_t)p->bytes_per_row * (height - 1) + row <= p->size;
}

static inline int
m2m_image_ok(const struct m2m_image *im, unsigned bits)
{
	if (im->width < M2M_MIN_SIZE || im->height < M2M_MIN_SIZE || im->width > M2M_MAX_SIZE || im->height > M2M_MAX_SIZE) {
		return 0;
	}
	return m2m_plane_ok(&im->plane[0], im->width, im->height, bits, 1) &&
	       m2m_plane_ok(&im->plane[1], (im->width + 1) / 2, (im->height + 1) / 2, bits, 2);
}

/* Fill the defaults (whole pictures) and check everything; M2M_OK when m2m_run may go. */
static inline int
m2m_check(struct m2m_job *j)
{
	unsigned sbits, sfull, dbits, dfull;

	if (!m2m_format_info(j->src.format, &sbits, &sfull) || !m2m_format_info(j->dst.format, &dbits, &dfull)) {
		return M2M_EFORMAT;
	}
	if (j->transform & ~M2M_XF_ALL) {
		return M2M_ETRANSFORM;
	}
	if (!m2m_image_ok(&j->src, sbits) || !m2m_image_ok(&j->dst, dbits)) {
		return M2M_EPLANE;
	}
	if (j->sw == 0 || j->sh == 0) {
		j->sx = j->sy = 0;
		j->sw = (uint64_t)j->src.width << 16;
		j->sh = (uint64_t)j->src.height << 16;
	}
	if (j->dw == 0 || j->dh == 0) {
		j->dx = j->dy = 0;
		j->dw = j->dst.width;
		j->dh = j->dst.height;
	}
	if (j->sw < ((uint64_t)M2M_MIN_SIZE << 16) || j->sh < ((uint64_t)M2M_MIN_SIZE << 16) ||
	    j->sx + j->sw > ((uint64_t)j->src.width << 16) || j->sy + j->sh > ((uint64_t)j->src.height << 16) ||
	    j->dw < M2M_MIN_SIZE || j->dh < M2M_MIN_SIZE || (uint64_t)j->dx + j->dw > j->dst.width ||
	    (uint64_t)j->dy + j->dh > j->dst.height || (j->dx & 1) || (j->dy & 1)) {
		return M2M_ERECT;
	}
	/* the turned destination's size against the source's, each axis */
	uint64_t uw = (j->transform & M2M_XF_ROTATE_90) ? j->dh : j->dw;
	uint64_t uh = (j->transform & M2M_XF_ROTATE_90) ? j->dw : j->dh;
	if ((uw << 16) > M2M_MAX_UPSCALE * j->sw || (uh << 16) > M2M_MAX_UPSCALE * j->sh ||
	    j->sw > M2M_MAX_DOWNSCALE * (uw << 16) || j->sh > M2M_MAX_DOWNSCALE * (uh << 16)) {
		return M2M_ESCALE;
	}
	return M2M_OK;
}

/* The transfer. m2m_check first. */
static inline void
m2m_run(const struct m2m_job *j)
{
	struct m2m_range sr, dr;

	m2m_range_get(j->src.format, &sr);
	m2m_range_get(j->dst.format, &dr);
	m2m_run_plane(j, 0, &sr, &dr);
	m2m_run_plane(j, 1, &sr, &dr);
	if (j->options & M2M_OPT_ZERO_FILL) {
		m2m_fill_outside(&j->dst.plane[0], &dr, 0, j->dx, j->dx + j->dw, j->dy, j->dy + j->dh);
		m2m_fill_outside(&j->dst.plane[1], &dr, 1, j->dx / 2, (j->dx + j->dw + 1) / 2, j->dy / 2,
		    (j->dy + j->dh + 1) / 2);
	}
}

#endif /* HAKIOS_M2M_CORE_H */
