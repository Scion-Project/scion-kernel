/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/bcm_shub_test.c: drivers/input/ssp/hakios.bcm_shub.h on a computer, under the sanitizers (tools/bcm-shub-test.sh).
 * What it checks:
 *   - the CRC-8 and the framing against the three sample frames in Samsung's S9 driver (bbdpl/bcm_gps_regs.c's "get
 *     version" request and its answer, bbd_rpc_lh.c's sensor packet), byte for byte;
 *   - build -> escape -> receive -> parse round trips with every byte value, sizes 0..SHUB_TL_MAX, every flag;
 *   - the receiver against noise, bad escapes and truncation (nothing read past the buffers: the sanitizers);
 *   - RPC TLVs with one- and two-byte ids and lengths;
 *   - the firmware patch given as argv[1] (devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin): every record a
 *     write, 452 of them, 0x00800000-0x0086444c, and the patch's own vector table: the reset vector at 0x00800004 is
 *     PatchAddress 0x00800161;
 *   - SSP data frames (a hand-made one with each record kind, and one the phone's hub sent) and the SSP receive buffer;
 *   - hostile input (hostile(), below): the driver's receiving side restated over the header, with lengths that lie at
 *     every layer, records and files cut at every length, and 300000 packets and 200000 patch files from a fixed-seed
 *     mutator: nothing is read or written outside the data or the driver's buffers.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../drivers/input/ssp/hakios.bcm_shub.h"

static unsigned checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345;
static uint32_t
rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

/* feed a framed packet to a receiver; the number of packets that came out (the last kept in *out) */
static int
receive(struct shub_tl_rx *r, const uint8_t *p, size_t n, struct shub_pkt *out, int *parse)
{
	int got = 0;

	for (size_t i = 0; i < n; i++) {
		if (shub_tl_byte(r, p[i]) == SHUB_TL_PACKET) {
			*parse = shub_tl_parse(r->buf, r->len, out);
			got++;
		}
	}
	return got;
}

static void
samples(void)
{
	/* bcm_gps_regs.c: "get version", after the SSI byte; and the answer in its comment */
	static const uint8_t req[] = { 0xB0, 0x00, 0x00, 0x02, 0x80, 0x03, 0x00, 0x00, 0x00, 0x01, 0xB1, 0xB0, 0x01 };
	static const uint8_t rsp[] = { 0xB0, 0x00, 0x00, 0x08, 0x80, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00,
		                       0xFF, 0x43, 0xB0, 0x01 };
	/* bbd_rpc_lh.c's sample1, after its two SSI length bytes */
	static const uint8_t sensor[] = { 0xB0, 0x00, 0x85, 0x26, 0x00, 0x21, 0x24, 0x22, 0x00, 0x02, 0x00, 0x1E, 0x00,
		0x37, 0x0E, 0xD3, 0x00, 0xCB, 0xFF, 0x50, 0x00, 0xDE, 0x00, 0x56, 0x00, 0xC7, 0xFF, 0x02, 0x00,
		0x00, 0x00, 0x37, 0x00, 0x90, 0xFE, 0x18, 0xFF, 0xB7, 0x46, 0x02, 0x00, 0x00, 0x00, 0xB2, 0xB0, 0x01 };
	static const uint8_t ver_payload[] = { 0x00, 0x01 };
	uint8_t out[64];
	struct shub_pkt p = { .seq = 0, .flags = SHUB_F_INTERNAL | SHUB_F_IGNORE_SEQID, .payload = ver_payload, .size = 2 };
	struct shub_tl_rx r;
	struct shub_pkt got;
	int parse = -1;
	size_t n = shub_tl_build(out, sizeof(out), &p), at = 0;
	uint16_t id = 0, len = 0;
	const uint8_t *data;

	CHECK(n == sizeof(req) && memcmp(out, req, n) == 0);

	memset(&r, 0, sizeof(r));
	CHECK(receive(&r, rsp, sizeof(rsp), &got, &parse) == 1 && parse == SHUB_TL_OK);
	CHECK(got.flags == (SHUB_F_EXTENDED | SHUB_F_INTERNAL) && got.size == 8 && got.payload[0] == 1);
	/* and built back the same */
	n = shub_tl_build(out, sizeof(out), &(struct shub_pkt){ .seq = 0, .flags = SHUB_F_INTERNAL, .payload = got.payload,
	                                                       .size = 8 });
	CHECK(n == sizeof(rsp) && memcmp(out, rsp, n) == 0);

	memset(&r, 0, sizeof(r));
	CHECK(receive(&r, sensor, sizeof(sensor), &got, &parse) == 1 && parse == SHUB_TL_OK);
	CHECK(got.seq == 0x85 && got.size == 0x26 && got.flags == 0);
	CHECK(shub_rpc_next(got.payload, got.size, &at, &id, &data, &len) == 1 && id == SHUB_RPC_SENSOR_RSP && len == 0x24);
	CHECK(shub_le16(data) == len - 2);
	CHECK(shub_rpc_next(got.payload, got.size, &at, &id, &data, &len) == 0);
	n = shub_tl_build(out, sizeof(out), &(struct shub_pkt){ .seq = 0x85, .payload = got.payload, .size = got.size });
	CHECK(n == sizeof(sensor) && memcmp(out, sensor, n) == 0);
}

static void
round_trips(void)
{
	static uint8_t payload[SHUB_TL_MAX], framed[2 * SHUB_TL_MAX + 64];
	struct shub_tl_rx r;

	for (unsigned t = 0; t < 4000; t++) {
		struct shub_pkt p = { 0 }, got;
		uint16_t size = (uint16_t)(t < 300 ? t : rnd() % (SHUB_TL_MAX + 1));
		int parse = -1;
		size_t n;

		for (unsigned i = 0; i < size; i++) {
			payload[i] = (uint8_t)(t % 5 == 0 ? (i % 3 == 0 ? 0xB0 : i % 3 == 1 ? 0x11 : 0x13) : rnd());
		}
		p.seq = (uint8_t)rnd();
		p.flags = (uint16_t)(rnd() & 0x033F);   /* the bits a sender sets itself; 6 and 7 come from the size and flags */
		for (int b = 0; b < 16; b++) {
			p.detail[b] = (uint8_t)rnd();
		}
		p.payload = payload;
		p.size = size;
		n = shub_tl_build(framed, sizeof(framed), &p);
		CHECK(n > 0);
		memset(&r, 0, sizeof(r));
		CHECK(receive(&r, framed, n, &got, &parse) == 1);
		CHECK(parse == SHUB_TL_OK);
		if (parse != SHUB_TL_OK) {
			continue;
		}
		CHECK(got.seq == p.seq && got.size == size && memcmp(got.payload, payload, size) == 0);
		CHECK((got.flags & 0x033F) == p.flags);
		for (int b = 0; b < 16; b++) {
			if ((p.flags & (1u << b)) && b != 6 && b != 7) {
				CHECK(got.detail[b] == p.detail[b]);
			}
		}
		/* every escaped byte is one of the three, nothing raw 0x11/0x13 */
		for (size_t i = 2; i + 2 < n; i++) {
			CHECK(framed[i] != 0x11 && framed[i] != 0x13);
			if (framed[i] == SHUB_ESC) {
				CHECK(framed[i + 1] >= SHUB_ESC_ESC && framed[i + 1] <= SHUB_ESC_XOFF);
				i++;
			}
		}
		/* a flipped bit inside: a packet that still parses has the same size and payload (the CRC does not cover the
		 * sequence number, and one flip can turn an escape into a dropped byte the CRC then catches) */
		if (n > 6) {
			size_t at = 2 + rnd() % (n - 4);

			framed[at] ^= (uint8_t)(1u << (rnd() % 8));
			memset(&r, 0, sizeof(r));
			if (receive(&r, framed, n, &got, &parse) == 1 && parse == SHUB_TL_OK) {
				CHECK(got.size == size && memcmp(got.payload, payload, size) == 0);
			}
		}
	}
	/* too big: refused */
	{
		struct shub_pkt p = { .payload = payload, .size = SHUB_TL_MAX + 1 };

		CHECK(shub_tl_build(framed, sizeof(framed), &p) == 0);
		p.size = 100;
		CHECK(shub_tl_build(framed, 50, &p) == 0);
	}
}

static void
noise(void)
{
	static uint8_t junk[200000];
	struct shub_tl_rx r;
	struct shub_pkt got;

	for (size_t i = 0; i < sizeof(junk); i++) {
		junk[i] = (uint8_t)(i % 7 == 0 ? 0xB0 : rnd());
	}
	memset(&r, 0, sizeof(r));
	for (size_t i = 0; i < sizeof(junk); i++) {
		if (shub_tl_byte(&r, junk[i]) == SHUB_TL_PACKET) {
			(void)shub_tl_parse(r.buf, r.len, &got);
		}
		CHECK(r.len <= sizeof(r.buf));
	}
	for (size_t len = 0; len < 40; len++) {
		uint8_t raw[40];

		for (size_t i = 0; i < len; i++) {
			raw[i] = (uint8_t)rnd();
		}
		raw[2 % (len ? len : 1)] |= 0xC0;       /* sizes and flags extended: the details run past the end */
		(void)shub_tl_parse(raw, len, &got);
	}
}

static void
rpcs(void)
{
	uint8_t buf[3000], data[1000];
	size_t n = 0, at = 0;
	uint16_t id, len;
	const uint8_t *d;

	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)i;
	}
	n += shub_rpc_put(buf + n, 0x05, data, 918);
	n += shub_rpc_put(buf + n, 0x7F, data, 0x7F);
	n += shub_rpc_put(buf + n, 0x402, data, 3);
	n += shub_rpc_put(buf + n, 0x00, NULL, 0);
	CHECK(buf[0] == 0x05 && buf[1] == 0x83 && buf[2] == 0x96);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x05 && len == 918 && memcmp(d, data, 918) == 0);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x7F && len == 0x7F);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x402 && len == 3);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0 && len == 0);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 0);
	for (size_t cut = 0; cut < 921; cut++) {      /* 921: the first RPC whole */
		at = 0;
		CHECK(shub_rpc_next(buf, cut, &at, &id, &d, &len) == (cut == 0 ? 0 : -1));
	}
}

static void
patch(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p;
	long size;
	struct shub_patch_info info;
	size_t at = 0;
	uint32_t addr, expect = 0x00800000, vector = 0;
	const uint8_t *data;
	uint16_t n;

	if (f == NULL) {
		fprintf(stderr, "no patch at %s: its checks skipped\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc((size_t)size);
	CHECK(p != NULL && fread(p, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	CHECK(size == 413864);
	CHECK(shub_patch_check(p, (size_t)size, &info));
	CHECK(info.records == 452 && info.lo == 0x00800000 && info.hi == 0x0086444c);
	while (shub_patch_next(p, (size_t)size, &at, &addr, &data, &n) == 1) {
		CHECK(addr == expect);  /* contiguous, in order */
		if (addr == 0x00800000) {
			vector = shub_le32(data + 4);
		}
		expect = addr + n;
	}
	CHECK(vector == 0x00800161);
	printf("patch: %u writes, %u bytes, %08x-%08x, reset vector %08x\n", info.records, info.bytes, info.lo, info.hi,
	    vector);
	/* truncated anywhere: refused, never read past */
	for (long cut = 1; cut < 4000; cut += 7) {
		struct shub_patch_info i2;
		uint8_t *c = malloc((size_t)cut);

		memcpy(c, p, (size_t)cut);
		(void)shub_patch_check(c, (size_t)cut, &i2);
		free(c);
	}
	p[0] = 0xF0;
	CHECK(!shub_patch_check(p, (size_t)size, &info));
	free(p);
}

static unsigned seen_samples;
static int16_t seen_x;

static void
on_sample(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	seen_samples++;
	if (s->sensor == SSP_ACCEL) {
		seen_x = (int16_t)shub_le16(s->data);
		CHECK(s->time == 0x0102030405060708ull);
	}
}

static int32_t seen_accel[3], seen_gyro[3], seen_mag[3];

static void
on_seen(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	seen_samples++;
	for (int i = 0; i < 3; i++) {
		if (s->sensor == SSP_ACCEL) {
			seen_accel[i] = (int16_t)shub_le16(s->data + 2 * i);
		} else if (s->sensor == SSP_MAG) {
			seen_mag[i] = (int16_t)shub_le16(s->data + 2 * i);
		} else if (s->sensor == SSP_GYRO) {
			seen_gyro[i] = (int32_t)shub_le32(s->data + 4 * i);
		}
	}
}

static void
frames(void)
{
	uint8_t f[200];
	size_t n = 0;
	struct shub_frame_counts c = { 0 };
	struct shub_ssp_rx rx = { 0 };
	uint16_t opts;
	const uint8_t *d;
	size_t dn, used;

	f[n++] = SSP_REC_SENSOR_INIT;
	f[n++] = SSP_REC_SAMPLE;
	f[n++] = SSP_ACCEL;
	f[n++] = 0x34; f[n++] = 0x12; f[n++] = 0; f[n++] = 0; f[n++] = 0; f[n++] = 0x10;
	for (int i = 0; i < 8; i++) {
		f[n++] = (uint8_t)(8 - i);
	}
	f[n++] = SSP_REC_DEBUG; f[n++] = 3; f[n++] = 0; f[n++] = 'h'; f[n++] = 'i'; f[n++] = '!';
	f[n++] = SSP_REC_SAMPLE; f[n++] = SSP_LIGHT;
	memset(f + n, 0, 18 + 8);
	n += 18 + 8;
	f[n++] = SSP_REC_META; f[n++] = 1; f[n++] = SSP_ACCEL;
	CHECK(shub_ssp_frame(f, n, &c, NULL, on_sample, NULL) == SSP_FRAME_OK);
	CHECK(c.init_done == 1 && c.samples == 2 && c.debug == 1 && c.meta == 1 && seen_samples == 2 && seen_x == 0x1234);
	for (size_t cut = 0; cut < n; cut++) {
		struct shub_frame_counts c2 = { 0 };

		(void)shub_ssp_frame(f, cut, &c2, NULL, NULL, NULL);
	}
	f[0] = 0x99;
	CHECK(shub_ssp_frame(f, n, &c, NULL, NULL, NULL) == SSP_FRAME_UNKNOWN);

	/* the receive buffer: a data frame in two pieces, then a READ answer */
	{
		uint8_t hdr[4] = { SSP_HUB2AP_WRITE, 0, 5, 0 }, body[5] = { 1, 2, 3, 4, 5 }, rd[5] = { 0, 0, 0, 0, 0x55 };

		shub_ssp_rx_add(&rx, hdr, 4);
		shub_ssp_rx_add(&rx, body, 2);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_NONE);
		shub_ssp_rx_add(&rx, body + 2, 3);
		rx.want_read = 1;
		shub_ssp_rx_add(&rx, rd, 5);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_DATA && dn == 5 && d[4] == 5);
		shub_ssp_rx_consume(&rx, used);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_READ && dn == 1 && d[0] == 0x55);
		shub_ssp_rx_consume(&rx, used);
		CHECK(rx.len == 0);
	}
	{
		uint8_t h[9];

		shub_ssp_header(h, SSP_SENSOR_ADD, 10, SSP_AP2HUB_WRITE, 0x11223344);
		CHECK(h[0] == 0xA1 && h[1] == 10 && h[2] == 0 && h[3] == 1 && h[4] == 0 && h[5] == 0x44 && h[8] == 0x11);
	}
	CHECK(shub_ssi_room(0x0B) == 3 * 256 && shub_ssi_room(0x40) == 0);

	/* a frame the S9's hub sent (phone 2, 3 November): the magnetometer (8 bytes), the accelerometer lying flat, the
	 * gyroscope at rest */
	{
		static const uint8_t seen[56] = {
			0x37, 0x04, 0x58, 0xfa, 0xfd, 0xfc, 0x51, 0xf4, 0x00, 0x00, 0xc1, 0xd8, 0xbd, 0x29, 0x04, 0x00, 0x00, 0x00,
			0x37, 0x00, 0xac, 0x00, 0xfa, 0xff, 0xf3, 0x0f, 0x80, 0xd5, 0xbd, 0x29, 0x04, 0x00, 0x00, 0x00,
			0x37, 0x01, 0xb8, 0x88, 0x00, 0x00, 0x30, 0xfe, 0xfe, 0xff, 0xf8, 0x30, 0xff, 0xff, 0x80, 0xd5, 0xbd, 0x29,
			0x04, 0x00, 0x00, 0x00 };
		struct shub_frame_counts c3 = { 0 };

		seen_samples = 0;
		seen_x = 0;
		CHECK(shub_ssp_frame(seen, sizeof(seen), &c3, NULL, on_seen, NULL) == SSP_FRAME_OK);
		CHECK(c3.samples == 3 && seen_samples == 3);
		CHECK(seen_accel[0] == 172 && seen_accel[1] == -6 && seen_accel[2] == 4083);
		CHECK(seen_gyro[0] == 35000 && seen_gyro[1] == -66000 && seen_gyro[2] == -53000);
		CHECK(seen_mag[0] == -1448 && seen_mag[1] == -771 && seen_mag[2] == -2991);
	}
}

/*
 * ---- hostile input ----
 * The hub is another processor running its own firmware: every length it sends is taken as hostile. The driver's
 * receiving side (drivers/input/ssp/hakios.ssp.c: feed() -> handle_packet() -> deliver_rpcs() -> ssp_stream() ->
 * on_sample()) is restated here with the same checks in the same order and the same buffer sizes, and every piece of
 * data is handed to the next layer from a buffer of exactly its size, so that the sanitizer stops a read past it.
 */
#include <string.h>

static struct shub_tl_rx h_tlrx;
static struct shub_ssp_rx h_ssprx;
static uint8_t h_answer[2048];          /* ssp.answer */
static uint8_t h_last[24];              /* last[].data */
static unsigned long h_packets, h_rpcs_seen, h_frames, h_samples, h_texts, h_answers;
static unsigned h_sink;

static uint32_t h_state = 0x5e2507u;
static uint32_t
h_mut(void)
{
	h_state ^= h_state << 13;
	h_state ^= h_state >> 17;
	h_state ^= h_state << 5;
	return h_state;
}

static void
h_sample(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	if (s->sensor >= SSP_SENSORS || s->size == 0 || s->size > sizeof(h_last)) {
		CHECK(!"a sample of a sensor or a size the driver has no room for");
		return;
	}
	memcpy(h_last, s->data, s->size);       /* on_sample(): last[s->sensor].data, 24 bytes */
	h_samples++;
}

static void
h_text(void *ctx, const uint8_t *p, size_t n)
{
	(void)ctx;
	for (size_t i = 0; i < n; i++) {
		h_sink += p[i];
	}
	h_texts++;
}

static void
h_stream(void)                          /* ssp_stream() */
{
	uint16_t opts;
	const uint8_t *d;
	size_t n, used;
	int k;

	while ((k = shub_ssp_rx_peek(&h_ssprx, &opts, &d, &n, &used)) != SSP_RX_NONE) {
		if (used < 4 || used > h_ssprx.len || d != h_ssprx.buf + 4 || n != used - 4) {
			CHECK(!"what the receive buffer starts with lies outside it");
			h_ssprx.len = 0;
			return;
		}
		if (k == SSP_RX_DATA) {
			struct shub_frame_counts c = { 0 };
			uint8_t *copy = malloc(n ? n : 1);

			memcpy(copy, d, n);
			(void)shub_ssp_frame(copy, n, &c, NULL, h_sample, h_text);
			CHECK(c.stopped_at <= n);
			free(copy);
			h_frames++;
		} else {
			size_t m = n < sizeof(h_answer) ? n : sizeof(h_answer);

			memcpy(h_answer, d, m);
			h_ssprx.want_read = 0;
			h_answers++;
		}
		shub_ssp_rx_consume(&h_ssprx, used);
	}
}

static void
h_rpcs(const uint8_t *p, size_t n)      /* deliver_rpcs() */
{
	size_t at = 0;
	uint16_t id, len;
	const uint8_t *d;

	while (shub_rpc_next(p, n, &at, &id, &d, &len) == 1) {
		if (d < p || d + len > p + n || at > n) {
			CHECK(!"an RPC outside its packet");
			return;
		}
		h_rpcs_seen++;
		switch (id) {
		case SHUB_RPC_VERSION:
			if (len >= 12) {
				h_sink += shub_le32(d) + shub_le32(d + 4) + shub_le32(d + 8);
			}
			break;
		case SHUB_RPC_SENSOR_RSP:
			if (len >= 2 && shub_le16(d) <= len - 2) {
				shub_ssp_rx_add(&h_ssprx, d + 2, shub_le16(d));
				CHECK(h_ssprx.len <= sizeof(h_ssprx.buf));
				h_stream();
			}
			break;
		case SHUB_RPC_LOG:
		case SHUB_RPC_LOG2:
			if (len >= 2) {
				h_text(NULL, d + 2, (size_t)len - 2);
			}
			break;
		default:
			h_text(NULL, d, len);
			break;
		}
	}
}

static void
h_packet(const uint8_t *raw, size_t len)        /* handle_packet() */
{
	struct shub_pkt p;
	uint8_t *copy = malloc(len ? len : 1);

	memcpy(copy, raw, len);
	if (shub_tl_parse(copy, len, &p) == SHUB_TL_OK) {
		h_packets++;
		if (p.payload < copy || p.payload + p.size > copy + len) {
			CHECK(!"a payload outside its packet");
		} else if (p.flags & SHUB_F_INTERNAL) {
			if (p.size >= 8 && p.payload[0] == SHUB_TLI_SEQ_RESPONSE) {      /* handle_internal() */
				for (int i = 0; i < 8; i++) {
					h_sink += p.payload[i];
				}
			} else {
				h_sink += p.size ? p.payload[0] : 0xff;
			}
		} else if (p.size > 0) {
			h_rpcs(p.payload, p.size);
		}
	}
	free(copy);
}

static void
h_feed(const uint8_t *p, size_t n)      /* feed() */
{
	for (size_t i = 0; i < n; i++) {
		if (shub_tl_byte(&h_tlrx, p[i]) == SHUB_TL_PACKET) {
			h_packet(h_tlrx.buf, h_tlrx.len);
		}
		if (h_tlrx.len > sizeof(h_tlrx.buf)) {
			CHECK(!"the packet receiver is past its buffer");
			h_tlrx.len = 0;
		}
	}
}

static void
h_mutate(uint8_t *p, size_t *n, size_t room, int most)
{
	for (int m = (int)(h_mut() % (unsigned)(most + 1)); m > 0 && *n > 0; m--) {
		size_t at = h_mut() % *n;

		switch (h_mut() % 7) {
		case 0: p[at] = (uint8_t)h_mut(); break;
		case 1: p[at] = 0xff; break;
		case 2: p[at] ^= (uint8_t)(1u << (h_mut() % 8)); break;
		case 3: p[at] = (uint8_t)(p[at] + 1); break;
		case 4: *n = at; break;                                                         /* cut */
		case 5: memmove(p + at, p + at + 1, *n - at - 1); (*n)--; break;                /* a byte lost */
		default: if (*n < room) { memmove(p + at + 1, p + at, *n - at); (*n)++; } break; /* a byte twice */
		}
	}
}

/* a good data frame: a few records of each kind */
static size_t
h_good_frame(uint8_t *f)
{
	static const uint8_t sensors[] = { 0, 1, 4, 5, 7, 9, 15, 16, 36 };
	size_t n = 0;

	for (int recs = 1 + (int)(h_mut() % 5); recs > 0; recs--) {
		switch (h_mut() % 8) {
		case 0: f[n++] = SSP_REC_SENSOR_INIT; break;
		case 1: f[n++] = SSP_REC_META; f[n++] = 1; f[n++] = 0; break;
		case 2: {
			unsigned t = h_mut() % 20;

			f[n++] = SSP_REC_DEBUG; f[n++] = (uint8_t)t; f[n++] = 0;
			memset(f + n, 'x', t); n += t;
			break;
		}
		case 3: f[n++] = SSP_REC_TIMESTAMP_OFF; memset(f + n, 1, 8); n += 8; break;
		case 4: f[n++] = SSP_REC_GYRO_CAL; memset(f + n, 2, 12); n += 12; break;
		default: {
			uint8_t s = sensors[h_mut() % sizeof(sensors)];

			f[n++] = SSP_REC_SAMPLE; f[n++] = s;
			memset(f + n, 0x11, (size_t)ssp_sample_size[s] + 8); n += (size_t)ssp_sample_size[s] + 8;
			break;
		}
		}
	}
	return n;
}

static void
hostile(void)
{
	/* ---- the packet layer ---- */
	{
		struct shub_pkt p;
		uint8_t raw[40];

		/* every flag set, so sixteen detail bytes, in packets cut at every length */
		memset(raw, 0xff, sizeof(raw));
		for (size_t len = 0; len <= sizeof(raw); len++) {
			uint8_t *copy = malloc(len ? len : 1);
			int r;

			memcpy(copy, raw, len);
			r = shub_tl_parse(copy, len, &p);
			CHECK(r == SHUB_TL_SHORT || r == SHUB_TL_LENGTH);
			free(copy);
		}
		/* a size of 0xFFFF (SIZE_EXT) in a packet of a few bytes */
		raw[0] = 1; raw[1] = 0xff; raw[2] = SHUB_F_SIZE_EXT; raw[3] = 0xff; raw[4] = 0; raw[5] = shub_crc8(raw + 1, 4);
		CHECK(shub_tl_parse(raw, 6, &p) == SHUB_TL_LENGTH);
		/* a packet that never ends: counted, the buffer never passed, and the next packet is received */
		memset(&h_tlrx, 0, sizeof(h_tlrx));
		{
			uint8_t sop[2] = { SHUB_ESC, SHUB_ESC_SOP }, b = 0x41, good[16];
			struct shub_pkt g = { .seq = 7 };
			size_t gn = shub_tl_build(good, sizeof(good), &g);
			unsigned long before = h_packets;

			h_feed(sop, 2);
			for (int i = 0; i < 5000; i++) {
				h_feed(&b, 1);
			}
			CHECK(h_tlrx.overflow == 1 && !h_tlrx.in_packet && h_tlrx.len <= sizeof(h_tlrx.buf));
			h_feed(good, gn);
			CHECK(gn != 0 && h_packets == before + 1);
		}
		/* the largest packet the driver makes, every byte one that is escaped: it fits the driver's send buffer */
		{
			static uint8_t pay[SHUB_TL_MAX], sbuf[2 * SHUB_TL_MAX + 64], small[32];
			struct shub_pkt big = { .seq = 0xB0, .flags = SHUB_F_ACK | SHUB_F_RELIABLE, .payload = pay, .size = SHUB_TL_MAX };
			struct shub_pkt ack = { .seq = 0xB0, .flags = SHUB_F_ACK | SHUB_F_RELIABLE_ACK | SHUB_F_RELIABLE_NACK };

			memset(pay, SHUB_ESC, sizeof(pay));
			memset(big.detail, SHUB_ESC, sizeof(big.detail));
			memset(ack.detail, SHUB_ESC, sizeof(ack.detail));
			CHECK(shub_tl_build(sbuf, sizeof(sbuf), &big) != 0);     /* send_packet(): sbuf */
			CHECK(shub_tl_build(small, sizeof(small), &ack) != 0);   /* pay_ack(): b[32] */
			big.size = SHUB_TL_MAX + 1;
			CHECK(shub_tl_build(sbuf, sizeof(sbuf), &big) == 0);
		}
	}

	/* ---- RPCs: lengths past the payload ---- */
	{
		static const uint8_t past[] = { 0x21, 0xff, 0xff, 1, 2, 3 };        /* a two-byte length of 0x7FFF, 3 bytes there */
		static const uint8_t half[] = { 0x21, 0x02, 1, 2, 0x84 };           /* a two-byte id cut after its first byte */
		static const uint8_t lies[] = { 0x21, 0x04, 0xff, 0xff, 1, 2 };     /* SSP's own length (0xFFFF) past its RPC */
		size_t at = 0;
		uint16_t id, len;
		const uint8_t *d;
		unsigned long before = h_frames + h_answers;

		CHECK(shub_rpc_next(past, sizeof(past), &at, &id, &d, &len) == -1);
		at = 0;
		CHECK(shub_rpc_next(half, sizeof(half), &at, &id, &d, &len) == 1 && shub_rpc_next(half, sizeof(half), &at, &id, &d,
		    &len) == -1);
		memset(&h_ssprx, 0, sizeof(h_ssprx));
		h_rpcs(lies, sizeof(lies));
		CHECK(h_ssprx.len == 0 && h_frames + h_answers == before);
	}

	/* ---- the SSP receive buffer: a data frame said to be 65535 bytes ---- */
	{
		uint8_t rpc[4 + 1000], f[64];
		size_t fn;
		unsigned long before;

		memset(&h_ssprx, 0, sizeof(h_ssprx));
		memset(rpc, 0, sizeof(rpc));
		rpc[0] = SSP_HUB2AP_WRITE; rpc[1] = 0; rpc[2] = 0xff; rpc[3] = 0xff;
		before = h_frames;
		for (int i = 0; i < 10; i++) {
			shub_ssp_rx_add(&h_ssprx, rpc, i == 0 ? sizeof(rpc) : 1000);
			CHECK(h_ssprx.len <= sizeof(h_ssprx.buf));
			h_stream();
		}
		CHECK(h_frames == before && h_ssprx.dropped > 0);        /* never whole: dropped when the buffer is full */
		/* the driver then starts again at the next header: a good frame alone in the buffer is taken */
		h_ssprx.len = 0;
		f[0] = SSP_HUB2AP_WRITE; f[1] = 0; f[2] = 1; f[3] = 0; f[4] = SSP_REC_SENSOR_INIT;
		fn = 5;
		shub_ssp_rx_add(&h_ssprx, f, fn);
		h_stream();
		CHECK(h_frames == before + 1 && h_ssprx.len == 0);
		/* an answer to a READ of 2000 bytes (the most /dev/hakios-gnss asks for) fits; one of 65535 never completes */
		h_ssprx.want_read = 2000;
		memset(rpc, 0, sizeof(rpc));
		shub_ssp_rx_add(&h_ssprx, rpc, 1004);
		h_stream();
		shub_ssp_rx_add(&h_ssprx, rpc, 1000);
		before = h_answers;
		h_stream();
		CHECK(h_answers == before + 1 && h_ssprx.len == 0 && h_ssprx.want_read == 0);
		h_ssprx.want_read = 0xffff;
		for (int i = 0; i < 6; i++) {
			shub_ssp_rx_add(&h_ssprx, rpc, 1000);
			h_stream();
			CHECK(h_ssprx.len <= sizeof(h_ssprx.buf));
		}
		CHECK(h_answers == before + 1);
		/* more than the buffer in one piece: refused whole */
		{
			static uint8_t huge[4096];

			h_ssprx.len = 5;
			shub_ssp_rx_add(&h_ssprx, huge, sizeof(huge));
			CHECK(h_ssprx.len == 0);
		}
	}

	/* ---- data frames: every sensor number, every record kind cut at every length ---- */
	{
		for (unsigned sensor = 0; sensor < 256; sensor++) {
			uint8_t f[2 + 24 + 8];
			struct shub_frame_counts c = { 0 };
			unsigned long before = h_samples;
			int r;

			memset(f, 0x22, sizeof(f));
			f[0] = SSP_REC_SAMPLE;
			f[1] = (uint8_t)sensor;
			r = shub_ssp_frame(f, sensor < SSP_SENSORS ? 2u + ssp_sample_size[sensor] + 8u : sizeof(f), &c, NULL, h_sample,
			    h_text);
			if (sensor < SSP_SENSORS && ssp_sample_size[sensor] != 0) {
				CHECK(r == SSP_FRAME_OK && h_samples == before + 1);
			} else {
				CHECK(r == SSP_FRAME_UNKNOWN && h_samples == before);
			}
		}
		for (unsigned s = 0; s < SSP_SENSORS; s++) {
			CHECK(ssp_sample_size[s] <= sizeof(h_last));    /* the driver's last[].data */
		}
		for (int round = 0; round < 2000; round++) {
			uint8_t f[256];
			size_t n = h_good_frame(f);

			for (size_t cut = 0; cut <= n; cut++) {
				uint8_t *copy = malloc(cut ? cut : 1);
				struct shub_frame_counts c = { 0 };

				memcpy(copy, f, cut);
				(void)shub_ssp_frame(copy, cut, &c, NULL, h_sample, h_text);
				CHECK(c.stopped_at <= cut);
				free(copy);
			}
		}
		{
			static const uint8_t debug[] = { SSP_REC_DEBUG, 0xff, 0xff, 'a', 'b' };     /* text of 65535 bytes, 2 there */
			struct shub_frame_counts c = { 0 };

			CHECK(shub_ssp_frame(debug, sizeof(debug), &c, NULL, h_sample, h_text) == SSP_FRAME_SHORT);
		}
	}

	/* ---- the whole receiving side against mutated streams (a fixed seed) ---- */
	{
		unsigned long packets0 = h_packets, frames0 = h_frames, samples0 = h_samples;

		memset(&h_tlrx, 0, sizeof(h_tlrx));
		memset(&h_ssprx, 0, sizeof(h_ssprx));
		for (int round = 0; round < 300000; round++) {
			uint8_t frame[300], ssp_bytes[320], pay[400], stream[900];
			size_t fn, sn = 0, pn = 0, tn;
			struct shub_pkt p = { .seq = (uint8_t)round };
			unsigned level = h_mut() % 4;

			/* SSP: [options][length][frame], or a READ's answer */
			fn = h_good_frame(frame);
			if (level == 0) {
				h_mutate(frame, &fn, sizeof(frame), 3);
			}
			if (h_mut() % 8 == 0) {
				h_ssprx.want_read = (uint16_t)(h_mut() % 16);
				ssp_bytes[sn++] = SSP_AP2HUB_READ; ssp_bytes[sn++] = 0; ssp_bytes[sn++] = 0; ssp_bytes[sn++] = 0;
				memset(ssp_bytes + sn, 0x55, h_ssprx.want_read); sn += h_ssprx.want_read;
			} else {
				ssp_bytes[sn++] = SSP_HUB2AP_WRITE; ssp_bytes[sn++] = 0;
				ssp_bytes[sn++] = (uint8_t)fn; ssp_bytes[sn++] = (uint8_t)(fn >> 8);
				memcpy(ssp_bytes + sn, frame, fn); sn += fn;
			}
			if (level == 1) {
				h_mutate(ssp_bytes, &sn, sizeof(ssp_bytes), 2);
			}
			/* RPCs: 0x21 [u16 n][SSP bytes], sometimes in two pieces, sometimes with a version or a log beside it */
			{
				uint8_t body[2 + 320];
				size_t first = h_mut() % 4 == 0 ? h_mut() % (sn + 1) : sn;

				body[0] = (uint8_t)first; body[1] = (uint8_t)(first >> 8);
				memcpy(body + 2, ssp_bytes, first);
				pn += shub_rpc_put(pay + pn, SHUB_RPC_SENSOR_RSP, body, (uint16_t)(first + 2));
				if (first < sn) {
					body[0] = (uint8_t)(sn - first); body[1] = (uint8_t)((sn - first) >> 8);
					memcpy(body + 2, ssp_bytes + first, sn - first);
					pn += shub_rpc_put(pay + pn, SHUB_RPC_SENSOR_RSP, body, (uint16_t)(sn - first + 2));
				}
				if (h_mut() % 8 == 0) {
					static const uint8_t ver[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

					pn += shub_rpc_put(pay + pn, h_mut() % 2 ? SHUB_RPC_VERSION : SHUB_RPC_LOG2, ver, sizeof(ver));
				}
			}
			if (level == 2) {
				h_mutate(pay, &pn, sizeof(pay), 2);
			}
			/* the packet */
			if (h_mut() % 4 == 0) {
				p.flags |= SHUB_F_RELIABLE;
			}
			if (h_mut() % 16 == 0) {
				p.flags |= SHUB_F_INTERNAL | (h_mut() % 2 ? SHUB_F_IGNORE_SEQID : 0);
			}
			if (h_mut() % 16 == 0) {
				p.flags |= (uint16_t)h_mut();
			}
			p.payload = pay;
			p.size = (uint16_t)pn;
			tn = shub_tl_build(stream, sizeof(stream), &p);
			if (tn == 0) {
				CHECK(!"a packet of the test's own that does not fit");
				continue;
			}
			if (level == 3) {
				h_mutate(stream, &tn, sizeof(stream), 3);
			}
			h_feed(stream, tn);
			if (h_ssprx.len > sizeof(h_ssprx.buf)) {
				CHECK(!"the SSP receive buffer is past its end");
				h_ssprx.len = 0;
			}
			if (h_mut() % 16 == 0) {
				h_ssprx.len = 0;        /* a bent length leaves the buffer waiting for bytes that are not coming, as it
				                         * does the driver until it is full: emptied now and then, so that good frames
				                         * are read again */
			}
		}
		CHECK(h_packets - packets0 > 200000 && h_frames - frames0 > 50000 && h_samples - samples0 > 50000);
	}

	/* ---- the firmware patch: mutated files, each in a buffer of exactly its size ---- */
	{
		uint8_t base[600];
		size_t n = 0;
		unsigned long good = 0;
		struct shub_patch_info info;

		for (int rec = 0; rec < 6; rec++) {
			uint16_t rl = (uint16_t)(4 + 16 + rec * 20);

			base[n++] = SHUB_PATCH_WRITE; base[n++] = (uint8_t)rl; base[n++] = (uint8_t)(rl >> 8);
			shub_put32(base + n, 0x00800000u + (uint32_t)rec * 0x100);
			memset(base + n + 4, rec, (size_t)rl - 4);
			n += rl;
		}
		CHECK(shub_patch_check(base, n, &info) && info.records == 6 && info.lo == 0x00800000u);
		for (int round = 0; round < 200000; round++) {
			uint8_t tmp[sizeof(base) + 8], *f;
			size_t size = n, at = 0, recs = 0;
			uint32_t addr;
			const uint8_t *data;
			uint16_t dn;
			bool ok;
			int r;

			memcpy(tmp, base, n);
			h_mutate(tmp, &size, sizeof(base), 3);
			f = malloc(size ? size : 1);
			memcpy(f, tmp, size);
			ok = shub_patch_check(f, size, &info);
			while ((r = shub_patch_next(f, size, &at, &addr, &data, &dn)) == 1) {
				/* load_patch(): the record as RPC 0x05 [u32 address][u16 n][data] in the driver's buffers */
				uint8_t body[4 + 2 + SHUB_PATCH_MAX_RECORD], pay[SHUB_TL_MAX];

				recs++;
				if (data < f + 7 || data + dn > f + size || at > size || dn == 0 || 6u + dn > sizeof(body)) {
					CHECK(!"a patch record outside its file, or larger than the driver's buffer");
					break;
				}
				shub_put32(body, addr);
				body[4] = (uint8_t)dn;
				body[5] = (uint8_t)(dn >> 8);
				memcpy(body + 6, data, dn);
				CHECK(shub_rpc_put(pay, SHUB_RPC_WRITE_MEM, body, (uint16_t)(6 + dn)) <= sizeof(pay));
			}
			if (ok) {
				good++;
				CHECK(r == 0 && recs == info.records && at == size);
			}
			free(f);
		}
		CHECK(good > 1000 && good < 199000);
		/* the largest record there can be, as the driver sends it */
		{
			static uint8_t rec[3 + SHUB_PATCH_MAX_RECORD], pay[SHUB_TL_MAX], body[4 + 2 + SHUB_PATCH_MAX_RECORD];
			size_t at = 0;
			uint32_t addr;
			const uint8_t *data;
			uint16_t dn;

			rec[0] = SHUB_PATCH_WRITE; rec[1] = (uint8_t)SHUB_PATCH_MAX_RECORD; rec[2] = (uint8_t)(SHUB_PATCH_MAX_RECORD >> 8);
			memset(rec + 3, 0xff, SHUB_PATCH_MAX_RECORD);       /* the address 0xFFFFFFFF too */
			CHECK(shub_patch_next(rec, sizeof(rec), &at, &addr, &data, &dn) == 1 && dn == SHUB_PATCH_MAX_RECORD - 4);
			memcpy(body + 6, data, dn);
			CHECK(shub_rpc_put(pay, SHUB_RPC_WRITE_MEM, body, (uint16_t)(6 + dn)) <= sizeof(pay));
			rec[1] = (uint8_t)(SHUB_PATCH_MAX_RECORD + 1); rec[2] = (uint8_t)((SHUB_PATCH_MAX_RECORD + 1) >> 8);
			at = 0;
			CHECK(shub_patch_next(rec, sizeof(rec), &at, &addr, &data, &dn) == -1);
			rec[1] = 4; rec[2] = 0;                         /* an address and no data */
			at = 0;
			CHECK(shub_patch_next(rec, sizeof(rec), &at, &addr, &data, &dn) == -1);
		}
	}
	(void)h_sink;
}

int
main(int argc, char **argv)
{
	samples();
	round_trips();
	noise();
	rpcs();
	patch(argc > 1 ? argv[1] : "devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin");
	frames();
	hostile();
	printf("bcm_shub_test: %u checks, %u failed\n", checks, failures);
	return failures != 0;
}
