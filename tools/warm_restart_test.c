// SPDX-License-Identifier: MIT-0
// The warm restart's re-entry record as the loader checks it (xnuloader/sboot/warm_record.c, scion_reentry.h): the
// CRC-32 and SHA-256 against their published values, a record built the way the XNU fork's bsd/dev/starlte_warm.c
// builds one, and each way a record is refused. Run by tools/warm-restart-test.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xnuloader/sboot/scion_reentry.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void
hex(const uint8_t *d, char *out)
{
	for (int i = 0; i < 32; i++) {
		snprintf(out + 2 * i, 3, "%02x", d[i]);
	}
}

static void
seal(struct scion_reentry *r)
{
	r->crc32 = 0;
	r->crc32 = scnr_crc32(r, SCNR_SIZE);
}

#define BASE 0x80080000ULL
#define OWN  0x1000U

int
main(void)
{
	uint8_t d[32];
	char h[65];
	static uint8_t image[OWN + 3 * 4096];
	struct scion_reentry r;

	CHECK(scnr_crc32("123456789", 9) == 0xCBF43926u);
	scnr_sha256("abc", 3, d);
	hex(d, h);
	CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
	scnr_sha256("", 0, d);
	hex(d, h);
	CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
	{
		const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";        /* 56 bytes: two tail blocks */
		scnr_sha256(m, strlen(m), d);
		hex(d, h);
		CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
	}
	{
		static char a[1000000];
		memset(a, 'a', sizeof(a));
		scnr_sha256(a, sizeof(a), d);
		hex(d, h);
		CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
	}

	for (unsigned i = 0; i < sizeof(image); i++) {
		image[i] = (uint8_t)(i * 7 + 3);
	}
	memset(&r, 0, sizeof(r));
	r.magic = SCNR_MAGIC;
	r.version = SCNR_VERSION;
	r.size = SCNR_SIZE;
	r.reason = SCNR_R_TEST;
	r.flags = SCNR_F_DIRECT | SCNR_F_RESTAGED | SCNR_F_WDT_ARMED | SCNR_F_LINE | SCNR_F_APPEND;
	r.entry = ~0u;
	strcpy(r.cmdline, "sweetwarmed=1");
	r.method = SCNR_M_XNU;
	r.wdt_ms = 60000;
	r.image_pa = BASE;
	r.image_size = sizeof(image);
	scnr_sha256(image + OWN, sizeof(image) - OWN, r.image_sha256);
	seal(&r);
	CHECK(scnr_check(&r, BASE) == NULL);
	CHECK(scnr_image_ok(&r, image, OWN));
	CHECK(!scnr_image_ok(&r, image, OWN + 4096));
	CHECK(!scnr_image_ok(&r, image, sizeof(image) + 1));
	image[0] ^= 1;                                  /* the Image's own bytes are not covered */
	CHECK(scnr_image_ok(&r, image, OWN));
	image[OWN + 5] ^= 1;                            /* the payload's are */
	CHECK(!scnr_image_ok(&r, image, OWN));
	image[OWN + 5] ^= 1;

	{
		struct scion_reentry b = r;
		b.cmdline[3] ^= 1;                      /* any byte changed after sealing */
		CHECK(scnr_check(&b, BASE) != NULL && strstr(scnr_check(&b, BASE), "CRC"));
		b = r;
		b.magic = 0;
		CHECK(scnr_check(&b, BASE) != NULL);
		b = r;
		b.version = 2;
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		CHECK(scnr_check(&r, BASE + 0x1000) != NULL);   /* not where the loader runs */
		b = r;
		b.image_size = 0x8c000000ULL - BASE + 1;        /* over the loader's STAGE */
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		b = r;
		memset(b.cmdline, 'x', SCNR_LINE);             /* not terminated */
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		b = r;
		b.flags &= ~SCNR_F_DIRECT;
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		b = r;
		b.method = SCNR_M_ANDROID;                      /* no kernel, ramdisk or FDT named */
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		b.kernel_pa = 0x84000000;
		b.kernel_size = 1 << 20;
		b.ramdisk_pa = 0x86000000;
		b.ramdisk_size = 1 << 20;
		b.fdt_pa = SCNR_FDT_SAVE;                       /* the header, not the FDT */
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
		b.fdt_pa = SCNR_FDT_SAVE + 0x1000;
		seal(&b);
		CHECK(scnr_check(&b, BASE) == NULL);
		b.method = 7;
		seal(&b);
		CHECK(scnr_check(&b, BASE) != NULL);
	}
	if (failures) {
		fprintf(stderr, "warm-restart-test: %d failure(s)\n", failures);
		return 1;
	}
	printf("warm-restart-test: OK\n");
	return 0;
}
