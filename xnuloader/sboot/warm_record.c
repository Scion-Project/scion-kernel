/* SPDX-License-Identifier: MIT
 * The warm restart's re-entry record (scion_reentry.h): its CRC-32 and the image's SHA-256, checked before the loader
 * uses anything the record names. Plain C over bytes; built for the loader (build.sh) and on the host
 * (tools/warm-restart-test.sh).
 *
 * The CRC-32 (IEEE 802.3, bitwise) and the SHA-256 (FIPS 180-4) are those of Scion-Project/scion-boot-exynos9810
 * src/lib/crypto.c (the stage-2 image verifier of the project's first-stage loader), taken as they are but for the
 * names, the round constant 60 written as it is (0x90befffa) instead of crypto.c's k_at() correction, and the tail
 * handled without memset/memcpy (the loader is built -fno-builtin). That file's licence:
 *
 *   MIT License
 *   Copyright (c) 2026 The Scion Project contributors
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 *   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 *   OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 *   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "scion_reentry.h"

uint32_t
scnr_crc32(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFFu;

	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int bit = 0; bit < 8; bit++) {
			uint32_t mask = -(crc & 1u);
			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

static const uint32_t sha256_k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t
rotr32(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void
sha256_block(uint32_t state[8], const uint8_t block[64])
{
	uint32_t w[64];

	for (unsigned i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4 + 0] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
		    ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
	}
	for (unsigned i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	for (unsigned i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + S1 + ch + sha256_k[i] + w[i];
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

void
scnr_sha256(const void *data, size_t len, uint8_t out[32])
{
	uint32_t state[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};
	const uint8_t *p = data;
	size_t rem = len;
	uint8_t tail[128];

	while (rem >= 64) {
		sha256_block(state, p);
		p += 64;
		rem -= 64;
	}
	for (unsigned i = 0; i < sizeof(tail); i++) {
		tail[i] = i < rem ? p[i] : 0;
	}
	tail[rem] = 0x80u;
	size_t blocks = (rem < 56) ? 1 : 2;
	uint64_t bits = (uint64_t)len * 8ULL;
	for (unsigned i = 0; i < 8; i++) {
		tail[blocks * 64 - 8 + i] = (uint8_t)(bits >> (56 - i * 8));
	}
	sha256_block(state, tail);
	if (blocks == 2) {
		sha256_block(state, tail + 64);
	}
	for (unsigned i = 0; i < 8; i++) {
		out[i * 4 + 0] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(state[i]);
	}
}

const char *
scnr_check(const struct scion_reentry *r, uint64_t base)
{
	struct scion_reentry c;
	const uint8_t *s = (const uint8_t *)r;
	uint8_t *d = (uint8_t *)&c;

	if (r->magic != SCNR_MAGIC) {
		return "WARM: NO SCNR RECORD";
	}
	if (r->version != SCNR_VERSION || r->size != SCNR_SIZE) {
		return "WARM: RECORD VERSION NOT KNOWN";
	}
	for (unsigned i = 0; i < SCNR_SIZE; i++) {
		d[i] = s[i];
	}
	c.crc32 = 0;
	if (scnr_crc32(&c, SCNR_SIZE) != r->crc32) {
		return "WARM: RECORD CRC MISMATCH";
	}
	if (c.cmdline[SCNR_LINE - 1] != 0) {
		return "WARM: BOOT LINE NOT TERMINATED";
	}
	if (!(c.flags & SCNR_F_DIRECT) || (c.flags & SCNR_F_MENU)) {
		return "WARM: ONLY A DIRECT START IS KNOWN";
	}
	if (c.image_pa != base || c.image_size < 0x1000 || c.image_size > (0x8c000000ULL - base)) {
		return "WARM: IMAGE NOT WHERE THE LOADER RUNS";
	}
	if (c.method == SCNR_M_ANDROID) {
		if (!c.kernel_pa || !c.kernel_size || !c.ramdisk_pa || !c.ramdisk_size || !c.fdt_pa) {
			return "WARM: ANDROID START INCOMPLETE";
		}
	} else if (c.method != SCNR_M_XNU) {
		return "WARM: METHOD NOT KNOWN";
	}
	return 0;
}

int
scnr_image_ok(const struct scion_reentry *r, const void *image, uint64_t own)
{
	uint8_t got[32];
	unsigned diff = 0;

	if (own > r->image_size) {
		return 0;
	}
	scnr_sha256((const uint8_t *)image + own, (size_t)(r->image_size - own), got);
	for (unsigned i = 0; i < 32; i++) {
		diff |= (unsigned)(got[i] ^ r->image_sha256[i]);
	}
	return diff == 0;
}
