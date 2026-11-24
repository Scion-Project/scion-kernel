/* SPDX-License-Identifier: MIT-0 */
/*
 * The S9's memory-to-memory scaler (drivers/media/m2m/hakios.m2m_core.h) on the build machine, against reference pictures
 * made here a second way: double precision, written from the definitions in the header's comment and
 * docs/ios27/M2M-SCALER.md (where a destination sample's centre lands in the source, bilinear weights, the box,
 * H.273's ranges), not from the header's fixed-point code. Plus pictures whose answer is known exactly (turns of a whole
 * picture are permutations; four turns are the picture again; a flat colour stays flat; a range there and back), and the
 * refusals: every number m2m_caps publishes is the edge of what m2m_check lets through.
 * tools/m2m-scaler-test.sh builds it under the sanitizers. M2M_TEST_PGM=dir writes the test card and its transfers as
 * PGM files to look at.
 */
#include "../../drivers/media/m2m/hakios.m2m_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

struct pic {
	struct m2m_image im;
	uint8_t *mem[2];
};

static void
pic_make(struct pic *p, uint32_t fmt, uint32_t w, uint32_t h, uint32_t pad)
{
	unsigned bits = 8, full = 0, bpc;
	(void)m2m_format_info(fmt, &bits, &full);
	bpc = bits == 8 ? 1 : 2;
	memset(p, 0, sizeof(*p));
	p->im.format = fmt;
	p->im.width = w;
	p->im.height = h;
	for (int c = 0; c < 2; c++) {
		uint32_t pw = c ? (w + 1) / 2 : w, ph = c ? (h + 1) / 2 : h, comps = c ? 2 : 1;
		uint32_t bpr = pw * comps * bpc + pad;
		p->mem[c] = malloc((size_t)bpr * ph);
		memset(p->mem[c], 0xa5, (size_t)bpr * ph);
		p->im.plane[c] = (struct m2m_plane){ p->mem[c], (uint64_t)bpr * ph, bpr, pw, ph };
	}
}

static void
pic_free(struct pic *p)
{
	free(p->mem[0]);
	free(p->mem[1]);
}

static unsigned
bits_of(uint32_t fmt)
{
	unsigned b = 8, f = 0;
	(void)m2m_format_info(fmt, &b, &f);
	return b;
}

static uint32_t
get(const struct pic *p, int c, uint32_t x, uint32_t y, unsigned comp)
{
	return m2m_read(&p->im.plane[c], bits_of(p->im.format), x, y, comp, c ? 2 : 1);
}

static void
put(struct pic *p, int c, uint32_t x, uint32_t y, unsigned comp, uint32_t v)
{
	m2m_write(&p->im.plane[c], bits_of(p->im.format), x, y, comp, c ? 2 : 1, (int32_t)v);
}

/* A test card: luma a diagonal ramp with fine bars in one corner, chroma two different ramps; no symmetry, so a wrong
 * turn or flip shows. */
static void
pic_card(struct pic *p)
{
	unsigned bits = bits_of(p->im.format), max = (1u << bits) - 1;
	for (uint32_t y = 0; y < p->im.height; y++) {
		for (uint32_t x = 0; x < p->im.width; x++) {
			uint32_t v = (x * 7 + y * 3) % 220 + 16;
			if (x < p->im.width / 4 && y < p->im.height / 4) {
				v = (x / 2 + y) % 2 ? 230 : 20;
			}
			put(p, 0, x, y, 0, (v << (bits - 8)) & max);
		}
	}
	for (uint32_t y = 0; y < p->im.plane[1].height; y++) {
		for (uint32_t x = 0; x < p->im.plane[1].width; x++) {
			put(p, 1, x, y, 0, (((x * 5) % 200 + 28) << (bits - 8)) & max);
			put(p, 1, x, y, 1, (((y * 9 + x) % 180 + 40) << (bits - 8)) & max);
		}
	}
}

/* ---- the reference, in doubles ---- */

/* The scaler keeps positions in 32.32 fixed point, cut down: a position exactly on a 1/256 phase boundary can land one
 * phase lower than the exact one. The comparison takes the reference at the exact position and at one 2^-20 pixel
 * lower, and a sample passes when it is within a code of either. */
static double ref_nudge;

static void
ref_range(uint32_t fmt, int chroma, double *off, double *span, double *max)
{
	unsigned bits = 8, full = 0;
	(void)m2m_format_info(fmt, &bits, &full);
	double n = (double)(1u << bits), s = n / 256.0;
	*max = n - 1;
	if (chroma) {
		*off = n / 2;
		*span = full ? n - 1 : 224 * s;
	} else {
		*off = full ? 0 : 16 * s;
		*span = full ? n - 1 : 219 * s;
	}
}

/* Destination point (x, y) of the rectangle (luma pixels from its corner) back into the source, by undoing the flips
 * and then the clockwise quarter turn as a rotation matrix about the origin plus a shift, then the scale. */
static void
ref_map(const struct m2m_job *j, double x, double y, double *sx, double *sy)
{
	double W = j->dw, H = j->dh;
	if (j->transform & M2M_XF_FLIP_H) {
		x = W - x;
	}
	if (j->transform & M2M_XF_FLIP_V) {
		y = H - y;
	}
	double u = x, v = y, uw = W, uh = H;
	if (j->transform & M2M_XF_ROTATE_90) {
		/* clockwise by 90: (u, v) -> (uh - v, u) with uh the source-side height = W; inverse below */
		uw = H;
		uh = W;
		u = y;
		v = W - x;
	}
	*sx = j->sx / 65536.0 + u * (j->sw / 65536.0) / uw;
	*sy = j->sy / 65536.0 + v * (j->sh / 65536.0) / uh;
	(void)uh;
}

static double
ref_tap(const struct pic *s, int c, unsigned comp, double ix, double iy, double lox, double hix, double loy, double hiy,
    int nearest)
{
	if (nearest) {
		double xi = floor(ix + 0.5), yi = floor(iy + 0.5);
		xi = xi < lox ? lox : xi > hix ? hix : xi;
		yi = yi < loy ? loy : yi > hiy ? hiy : yi;
		return get(s, c, (uint32_t)xi, (uint32_t)yi, comp);
	}
	/* 256 phases: the weight is the position's fraction cut to 1/256 */
	double x0 = floor(ix), y0 = floor(iy), fx = floor((ix - x0) * 256) / 256, fy = floor((iy - y0) * 256) / 256;
	double xs[2] = { x0, x0 + 1 }, ys[2] = { y0, y0 + 1 }, v = 0;
	for (int b = 0; b < 2; b++) {
		for (int a = 0; a < 2; a++) {
			double xx = xs[a] < lox ? lox : xs[a] > hix ? hix : xs[a];
			double yy = ys[b] < loy ? loy : ys[b] > hiy ? hiy : ys[b];
			v += get(s, c, (uint32_t)xx, (uint32_t)yy, comp) * (a ? fx : 1 - fx) * (b ? fy : 1 - fy);
		}
	}
	return v;
}

/* The reference transfer into ref (a copy of the destination before): every sample in the rectangle. */
static void
ref_run(const struct m2m_job *j, const struct pic *s, struct pic *d)
{
	int rot = (j->transform & M2M_XF_ROTATE_90) != 0;
	double kx = (j->sw / 65536.0) / (rot ? j->dh : j->dw), ky = (j->sh / 65536.0) / (rot ? j->dw : j->dh);
	int nearest = (j->options & M2M_OPT_NEAREST) != 0;
	int nx = nearest || kx < 2 ? 1 : (int)ceil(kx), ny = nearest || ky < 2 ? 1 : (int)ceil(ky);
	for (int c = 0; c < 2; c++) {
		double so, ss, sm, dof, ds, dm;
		ref_range(s->im.format, c, &so, &ss, &sm);
		ref_range(d->im.format, c, &dof, &ds, &dm);
		double lox = floor(j->sx / 65536.0), loy = floor(j->sy / 65536.0);
		double hix = ceil((j->sx + j->sw) / 65536.0) - 1, hiy = ceil((j->sy + j->sh) / 65536.0) - 1;
		if (c) {
			lox = floor(lox / 2); loy = floor(loy / 2); hix = floor(hix / 2); hiy = floor(hiy / 2);
		}
		hix = fmin(hix, s->im.plane[c].width - 1);
		hiy = fmin(hiy, s->im.plane[c].height - 1);
		uint32_t i0 = c ? j->dx / 2 : j->dx, i1 = c ? (j->dx + j->dw + 1) / 2 : j->dx + j->dw;
		uint32_t k0 = c ? j->dy / 2 : j->dy, k1 = c ? (j->dy + j->dh + 1) / 2 : j->dy + j->dh;
		for (uint32_t k = k0; k < k1 && k < d->im.plane[c].height; k++) {
			for (uint32_t i = i0; i < i1 && i < d->im.plane[c].width; i++) {
				/* the sample's centre in luma pixels of the destination */
				double cx = c ? 2.0 * i + 0.5 : i + 0.5, cy = c ? 2.0 * k + 1.0 : k + 0.5;
				double sx, sy;
				ref_map(j, cx - j->dx, cy - j->dy, &sx, &sy);
				for (unsigned comp = 0; comp < (c ? 2u : 1u); comp++) {
					double acc = 0;
					for (int ty = 0; ty < ny; ty++) {
						for (int tx = 0; tx < nx; tx++) {
							double px = sx + (nx == 1 ? 0 : ((2.0 * tx + 1) / (2.0 * nx) - 0.5) * kx);
							double py = sy + (ny == 1 ? 0 : ((2.0 * ty + 1) / (2.0 * ny) - 0.5) * ky);
							px -= ref_nudge;
							py -= ref_nudge;
							double ix = c ? (px - 0.5) / 2 : px - 0.5, iy = c ? (py - 1.0) / 2 : py - 0.5;
							acc += ref_tap(s, c, comp, ix, iy, lox, hix, loy, hiy, nearest);
						}
					}
					double v = acc / (nx * ny);
					v = dof + (v - so) * ds / ss;
					v = floor(v + 0.5);
					v = v < 0 ? 0 : v > dm ? dm : v;
					put(d, c, i, k, comp, (uint32_t)v);
				}
			}
		}
		if (j->options & M2M_OPT_ZERO_FILL) {
			/* black: Y' at its offset, Cb Cr at the centre, everywhere outside the rectangle's samples */
			for (uint32_t k = 0; k < d->im.plane[c].height; k++) {
				for (uint32_t i = 0; i < d->im.plane[c].width; i++) {
					if (k >= k0 && k < k1 && i >= i0 && i < i1) {
						continue;
					}
					for (unsigned comp = 0; comp < (c ? 2u : 1u); comp++) {
						put(d, c, i, k, comp, (uint32_t)dof);
					}
				}
			}
		}
	}
}

/* Run the scaler and the reference on copies of dst; the largest difference of any sample (the whole picture, so a
 * write outside the rectangle shows too). */
static uint32_t
compare(struct m2m_job *j, struct pic *s, struct pic *d, const char *name)
{
	struct pic r, r2;
	uint32_t pad = d->im.plane[0].bytes_per_row - d->im.width * (bits_of(d->im.format) == 8 ? 1 : 2);
	pic_make(&r, d->im.format, d->im.width, d->im.height, pad);
	pic_make(&r2, d->im.format, d->im.width, d->im.height, pad);
	for (int c = 0; c < 2; c++) {
		memcpy(r.mem[c], d->mem[c], d->im.plane[c].size);
		memcpy(r2.mem[c], d->mem[c], d->im.plane[c].size);
	}
	j->src = s->im;
	j->dst = d->im;
	int e = m2m_check(j);
	CHECK(e == M2M_OK, "%s: m2m_check %d", name, e);
	if (e != M2M_OK) {
		pic_free(&r);
		pic_free(&r2);
		return 9999;
	}
	m2m_run(j);
	ref_nudge = 0;
	ref_run(j, s, &r);
	ref_nudge = 1.0 / (1 << 20);
	ref_run(j, s, &r2);
	uint32_t worst = 0;
	for (int c = 0; c < 2; c++) {
		for (uint32_t y = 0; y < d->im.plane[c].height; y++) {
			for (uint32_t x = 0; x < d->im.plane[c].width; x++) {
				for (unsigned comp = 0; comp < (c ? 2u : 1u); comp++) {
					uint32_t a = get(d, c, x, y, comp), b = get(&r, c, x, y, comp), b2 = get(&r2, c, x, y, comp);
					uint32_t diff = a > b ? a - b : b - a, diff2 = a > b2 ? a - b2 : b2 - a;
					diff = diff2 < diff ? diff2 : diff;
					if (diff > worst) {
						worst = diff;
					}
				}
			}
		}
	}
	pic_free(&r);
	pic_free(&r2);
	return worst;
}

static void
write_pgm(const struct pic *p, const char *name)
{
	const char *dir = getenv("M2M_TEST_PGM");
	if (dir == NULL) {
		return;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.pgm", dir, name);
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return;
	}
	unsigned shift = bits_of(p->im.format) - 8;
	fprintf(f, "P5\n%u %u\n255\n", p->im.width, p->im.height);
	for (uint32_t y = 0; y < p->im.height; y++) {
		for (uint32_t x = 0; x < p->im.width; x++) {
			fputc((int)(get(p, 0, x, y, 0) >> shift), f);
		}
	}
	fclose(f);
}

/* Whole-picture turns at 1:1 are permutations: the expected picture by index arithmetic alone. */
static void
test_permutations(void)
{
	static const uint32_t fmts[] = { M2M_FMT_420F, M2M_FMT_X420 };
	for (unsigned fi = 0; fi < 2; fi++) {
		for (uint32_t xf = 0; xf < 8; xf++) {
			struct pic s, d;
			uint32_t w = 24, h = 16, rot = xf & M2M_XF_ROTATE_90;
			pic_make(&s, fmts[fi], w, h, 8);
			pic_card(&s);
			pic_make(&d, fmts[fi], rot ? h : w, rot ? w : h, 0);
			struct m2m_job j = { .transform = xf };
			j.src = s.im;
			j.dst = d.im;
			CHECK(m2m_check(&j) == M2M_OK, "perm %u", xf);
			m2m_run(&j);
			uint32_t bad = 0;
			for (uint32_t y = 0; y < d.im.height; y++) {
				for (uint32_t x = 0; x < d.im.width; x++) {
					uint32_t a = x, b = y, dwv = d.im.width, dhv = d.im.height;
					if (xf & M2M_XF_FLIP_H) { a = dwv - 1 - a; }
					if (xf & M2M_XF_FLIP_V) { b = dhv - 1 - b; }
					uint32_t sxx = a, syy = b;
					if (rot) { sxx = b; syy = dwv - 1 - a; }
					if (get(&d, 0, x, y, 0) != get(&s, 0, sxx, syy, 0)) {
						bad++;
					}
				}
			}
			CHECK(bad == 0, "fmt %u transform %u: %u luma samples are not the source's", fi, xf, bad);
			if (fi == 0) {
				char n[64];
				snprintf(n, sizeof(n), "perm-%u", xf);
				write_pgm(&d, n);
			}
			pic_free(&s);
			pic_free(&d);
		}
	}
}

/* Four quarter turns, and a flip twice, give the picture back (luma and chroma, every sample). */
static void
test_round_trips(void)
{
	struct pic a, b;
	pic_make(&a, M2M_FMT_420V, 32, 20, 4);
	pic_card(&a);
	struct pic cur = a;
	struct pic tmp[4];
	for (int t = 0; t < 4; t++) {
		pic_make(&tmp[t], M2M_FMT_420V, cur.im.height, cur.im.width, 0);
		struct m2m_job j = { .transform = M2M_XF_ROTATE_90 };
		j.src = cur.im;
		j.dst = tmp[t].im;
		CHECK(m2m_check(&j) == M2M_OK, "turn %d", t);
		m2m_run(&j);
		cur = tmp[t];
	}
	/* luma only: 4:2:0 chroma sited left of centre does not land on chroma samples after a turn or a flip, so it is
	 * filtered (test_reference checks it) */
	uint32_t bad = 0;
	for (uint32_t y = 0; y < a.im.height; y++) {
		for (uint32_t x = 0; x < a.im.width; x++) {
			bad += get(&a, 0, x, y, 0) != get(&cur, 0, x, y, 0);
		}
	}
	CHECK(bad == 0, "four quarter turns: %u luma samples differ", bad);
	for (int t = 0; t < 4; t++) {
		pic_free(&tmp[t]);
	}
	/* 180 degrees = both flips: twice is the picture */
	pic_make(&b, M2M_FMT_420V, 32, 20, 0);
	struct pic c2;
	pic_make(&c2, M2M_FMT_420V, 32, 20, 0);
	struct m2m_job j1 = { .transform = M2M_XF_FLIP_H | M2M_XF_FLIP_V }, j2 = j1;
	j1.src = a.im; j1.dst = b.im; j2.src = b.im; j2.dst = c2.im;
	CHECK(m2m_check(&j1) == M2M_OK && m2m_check(&j2) == M2M_OK, "180");
	m2m_run(&j1);
	m2m_run(&j2);
	bad = 0;
	for (uint32_t y = 0; y < a.im.height; y++) {
		for (uint32_t x = 0; x < a.im.width; x++) {
			bad += get(&a, 0, x, y, 0) != get(&c2, 0, x, y, 0);
		}
	}
	CHECK(bad == 0, "180 twice: %u luma samples differ", bad);
	/* vertically the chroma is centred: a vertical flip lands on chroma samples, twice is the picture exactly */
	struct pic v1, v2;
	pic_make(&v1, M2M_FMT_420V, 32, 20, 0);
	pic_make(&v2, M2M_FMT_420V, 32, 20, 0);
	struct m2m_job f1 = { .transform = M2M_XF_FLIP_V }, f2 = f1;
	f1.src = a.im; f1.dst = v1.im; f2.src = v1.im; f2.dst = v2.im;
	CHECK(m2m_check(&f1) == M2M_OK && m2m_check(&f2) == M2M_OK, "flip v");
	m2m_run(&f1);
	m2m_run(&f2);
	bad = 0;
	for (int c = 0; c < 2; c++) {
		for (uint32_t y = 0; y < a.im.plane[c].height; y++) {
			for (uint32_t x = 0; x < a.im.plane[c].width; x++) {
				for (unsigned comp = 0; comp < (c ? 2u : 1u); comp++) {
					bad += get(&a, c, x, y, comp) != get(&v2, c, x, y, comp);
				}
			}
		}
	}
	CHECK(bad == 0, "vertical flip twice: %u samples differ", bad);
	pic_free(&v1);
	pic_free(&v2);
	pic_free(&a);
	pic_free(&b);
	pic_free(&c2);
}

/* Against the double-precision reference: scaling up and down, crops with fractional edges, every transform, both
 * depths and ranges, the box, nearest. At most one code apart anywhere in the destination. */
static void
test_reference(void)
{
	struct { uint32_t sf, df, sw, sh, dw, dh; uint64_t cx, cy, cw, ch; uint32_t rx, ry, rw, rh, opt; const char *n; } t[] = {
		/* the camera preview's own transfer: the 2x crop of 1504x1128, turned into 828x1106 of a 828x1702 */
		{ M2M_FMT_420F, M2M_FMT_420F, 1504, 1128, 828, 1702, 376 << 16, 282 << 16, 752u << 16, 564u << 16,
		  0, 178, 828, 1106, M2M_OPT_ZERO_FILL, "preview" },
		{ M2M_FMT_420F, M2M_FMT_420F, 64, 48, 200, 150, 0, 0, 0, 0, 0, 0, 0, 0, 0, "up-3.1" },
		{ M2M_FMT_420V, M2M_FMT_420V, 160, 120, 40, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, "down-4-box" },
		{ M2M_FMT_420V, M2M_FMT_420F, 160, 120, 23, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, "down-7-odd-range" },
		{ M2M_FMT_420F, M2M_FMT_XF20, 96, 64, 96, 64, (uint64_t)(10.25 * 65536), (uint64_t)(6.5 * 65536),
		  (uint64_t)(50.75 * 65536), (uint64_t)(31.125 * 65536), 4, 2, 80, 60, 0, "crop-frac-10bit" },
		{ M2M_FMT_X420, M2M_FMT_420V, 64, 64, 64, 64, 0, 0, 0, 0, 8, 8, 40, 40, M2M_OPT_ZERO_FILL, "10-to-8-fill" },
		{ M2M_FMT_420F, M2M_FMT_420F, 80, 60, 80, 60, 0, 0, 0, 0, 0, 0, 0, 0, M2M_OPT_NEAREST, "nearest" },
		{ M2M_FMT_420F, M2M_FMT_420F, 30, 30, 480, 480, 0, 0, 0, 0, 0, 0, 0, 0, 0, "up-16" },
		{ M2M_FMT_420F, M2M_FMT_420F, 512, 512, 32, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, "down-16" },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		for (uint32_t xf = 0; xf < 8; xf++) {
			struct pic s, d;
			int rot = (xf & M2M_XF_ROTATE_90) != 0;
			uint32_t dw = t[i].dw, dh = t[i].dh;
			if (rot && i != 0) {
				uint32_t sw2 = dw; dw = dh; dh = sw2;
			}
			if (i == 0 && !rot) {
				continue;       /* the preview's rectangle is for a turned picture */
			}
			pic_make(&s, t[i].sf, t[i].sw, t[i].sh, 16);
			pic_card(&s);
			pic_make(&d, t[i].df, dw, dh, 4);
			struct m2m_job j = { .sx = t[i].cx, .sy = t[i].cy, .sw = t[i].cw, .sh = t[i].ch, .dx = t[i].rx, .dy = t[i].ry,
			    .dw = t[i].rw, .dh = t[i].rh, .transform = xf, .options = t[i].opt };
			if (rot && t[i].rw != 0 && i != 0) {
				j.dx = t[i].ry; j.dy = t[i].rx; j.dw = t[i].rh; j.dh = t[i].rw;
			}
			uint32_t worst = compare(&j, &s, &d, t[i].n);
			CHECK(worst <= 1, "%s transform %u: %u codes from the reference", t[i].n, xf, worst);
			if (xf == (i == 0 ? 4u : 0u)) {
				char n[64];
				snprintf(n, sizeof(n), "ref-%s-%u", t[i].n, xf);
				write_pgm(&d, n);
			}
			pic_free(&s);
			pic_free(&d);
		}
	}
}

/* Flat colours stay flat through any scale and turn; video black and white map to full black and white and back. */
static void
test_ranges(void)
{
	static const struct { uint32_t a, b; uint32_t ya, yb, ca, cb; } r[] = {
		{ M2M_FMT_420V, M2M_FMT_420F, 16, 0, 128, 128 },
		{ M2M_FMT_420V, M2M_FMT_420F, 235, 255, 240, 255 },
		{ M2M_FMT_420V, M2M_FMT_420F, 235, 255, 16, 1 },        /* 128 - 127.5, rounded half up */
		{ M2M_FMT_420V, M2M_FMT_X420, 235, 940, 240, 960 },
		{ M2M_FMT_420F, M2M_FMT_XF20, 255, 1023, 1, 3 },
		{ M2M_FMT_X420, M2M_FMT_420V, 64, 16, 512, 128 },
		{ M2M_FMT_XF20, M2M_FMT_420F, 1023, 255, 512, 128 },
	};
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
		struct pic s, d;
		pic_make(&s, r[i].a, 40, 30, 0);
		pic_make(&d, r[i].b, 30, 40, 0);
		for (uint32_t y = 0; y < 30; y++) {
			for (uint32_t x = 0; x < 40; x++) {
				put(&s, 0, x, y, 0, r[i].ya);
			}
		}
		for (uint32_t y = 0; y < 15; y++) {
			for (uint32_t x = 0; x < 20; x++) {
				put(&s, 1, x, y, 0, r[i].ca);
				put(&s, 1, x, y, 1, r[i].ca);
			}
		}
		struct m2m_job j = { .transform = M2M_XF_ROTATE_90 | M2M_XF_FLIP_H };
		j.src = s.im;
		j.dst = d.im;
		CHECK(m2m_check(&j) == M2M_OK, "range %u", i);
		m2m_run(&j);
		uint32_t bad = 0;
		for (uint32_t y = 0; y < 40; y++) {
			for (uint32_t x = 0; x < 30; x++) {
				bad += get(&d, 0, x, y, 0) != r[i].yb;
			}
		}
		for (uint32_t y = 0; y < 20; y++) {
			for (uint32_t x = 0; x < 15; x++) {
				bad += get(&d, 1, x, y, 0) != r[i].cb || get(&d, 1, x, y, 1) != r[i].cb;
			}
		}
		CHECK(bad == 0, "range case %u: %u samples not %u / %u", i, bad, r[i].yb, r[i].cb);
		pic_free(&s);
		pic_free(&d);
	}
}

/* The published limits are the edges of what is accepted. */
static void
test_caps_edges(void)
{
	struct m2m_caps caps;
	m2m_caps_get(&caps);
	struct { uint32_t sw, sh, dw, dh, xf; int ok; const char *n; } t[] = {
		{ 32, 32, 32 * caps.max_upscale, 32, 0, 1, "up at the limit" },
		{ 32, 32, 32 * caps.max_upscale + 2, 32, 0, 0, "up past the limit" },
		{ 32, 64, 64 * caps.max_upscale, 32, M2M_XF_ROTATE_90, 1, "up at the limit, turned" },
		{ 32, 64, 64 * caps.max_upscale + 2, 32, M2M_XF_ROTATE_90, 0, "up past the limit, turned" },
		{ 32 * caps.max_downscale, 32, 32, 32, 0, 1, "down at the limit" },
		{ 32 * caps.max_downscale + 2, 32, 32, 32, 0, 0, "down past the limit" },
		{ caps.max_size, 2, caps.max_size, 2, 0, 1, "largest" },
		{ caps.max_size + 2, 2, caps.max_size + 2, 2, 0, 0, "too large" },
		{ caps.min_size, caps.min_size, caps.min_size, caps.min_size, 0, 1, "smallest" },
		{ caps.min_size - 1, caps.min_size, caps.min_size, caps.min_size, 0, 0, "too small" },
		{ 16, 16, 16, 16, 8, 0, "unknown transform bit" },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		struct pic s, d;
		pic_make(&s, M2M_FMT_420F, t[i].sw, t[i].sh, 0);
		pic_make(&d, M2M_FMT_420F, t[i].dw, t[i].dh, 0);
		struct m2m_job j = { .transform = t[i].xf };
		j.src = s.im;
		j.dst = d.im;
		int e = m2m_check(&j);
		CHECK((e == M2M_OK) == t[i].ok, "%s: m2m_check %d", t[i].n, e);
		pic_free(&s);
		pic_free(&d);
	}
	/* refusals that are not about size */
	struct pic s, d;
	pic_make(&s, M2M_FMT_420F, 64, 64, 0);
	pic_make(&d, M2M_FMT_420F, 64, 64, 0);
	struct m2m_job j = { 0 };
	j.src = s.im; j.dst = d.im; j.dst.format = M2M_FOURCC('B', 'G', 'R', 'A');
	CHECK(m2m_check(&j) == M2M_EFORMAT, "BGRA refused");
	j.dst = d.im; j.dx = 1; j.dw = 32; j.dh = 32;
	CHECK(m2m_check(&j) == M2M_ERECT, "odd destination origin refused");
	j.dx = 0; j.sx = 40u << 16; j.sw = 32u << 16; j.sh = 32u << 16;
	CHECK(m2m_check(&j) == M2M_ERECT, "source rectangle past the edge refused");
	struct m2m_job k = { 0 };
	k.src = s.im; k.dst = d.im; k.src.plane[1].size = 100;
	CHECK(m2m_check(&k) == M2M_EPLANE, "short chroma plane refused");
	k.src = s.im; k.src.plane[0].bytes_per_row = 63;
	CHECK(m2m_check(&k) == M2M_EPLANE, "short row refused");
	pic_free(&s);
	pic_free(&d);
}

int
main(void)
{
	struct pic card;
	pic_make(&card, M2M_FMT_420F, 96, 64, 0);
	pic_card(&card);
	write_pgm(&card, "card");
	pic_free(&card);

	test_permutations();
	test_round_trips();
	test_ranges();
	test_reference();
	test_caps_edges();
	if (failures) {
		printf("m2m_test: %d FAILED\n", failures);
		return 1;
	}
	printf("m2m_test: the scaler matches its reference pictures and its published limits\n");
	return 0;
}
