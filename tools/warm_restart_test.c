// SPDX-License-Identifier: MIT-0
// The warm restart's re-entry record as the loader checks it (xnuloader/sboot/warm_record.c, scion_reentry.h): the
// CRC-32 and SHA-256 against their published values, a record built the way the XNU fork's bsd/dev/starlte_warm.c
// builds one, and each way a record is refused; the sizes of BOOT's kernel slot (tools/warm_slot.c, as the fork's
// warm_slot()). With a BOOT image and the loader's own size (warm-restart-test.sh makes one with the real packer): that
// image's slot taken, re-staged and checked end to end as the kernel and the loader do it. Run by
// tools/warm-restart-test.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../xnuloader/sboot/scion_reentry.h"
#include "warm_slot.h"

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

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A slot of `size` bytes: the loader's header (image_size `declared`, the warm mark) and a payload at `own`. */
static uint8_t *
slot(uint64_t size, uint64_t declared, uint64_t own)
{
	uint8_t *s = calloc(1, size);
	memcpy(s + 16, &declared, 8);
	memcpy(s + 56, "ARMd", 4);
	memcpy(s + SCNR_MARK_OFFSET, "SCNR\1\0\0\0", 8);
	memcpy(s + own, "SWEETXNU\1\0\0\0\1\0\0\0", 16);
	return s;
}

/* The BOOT image the real packer made: what the kernel re-reads (kernel_size from the slot), the record it writes,
 * what the stub leaves at the base (whole pages: the staging buffer's zeroes after the slot), what the loader takes. */
static void
boot_image(const char *path, uint64_t own)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f == NULL) {
		return;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)len);
	CHECK(fread(b, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	CHECK(memcmp(b, "ANDROID!", 8) == 0 && le32(b + 36) == 2048);
	uint64_t ksize = le32(b + 8), declared, off;
	char why[200];
	CHECK(2048 + ksize <= (uint64_t)len);
	const char *bad = warm_slot(b + 2048, ksize, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why));
	if (bad != NULL) {
		fprintf(stderr, "warm_slot: %s\n", bad);
	}
	CHECK(bad == NULL);
	CHECK(declared > ((ksize + WARM_PG - 1) & ~(WARM_PG - 1)));    /* the packer's padding: the old check refused it */
	CHECK(off == own);                              /* the payload where the loader looks (its _image_end) */

	uint64_t copy = (ksize + WARM_PG - 1) & ~(WARM_PG - 1);
	uint8_t *staged = calloc(1, copy);              /* kmem_alloc(KMA_ZERO) of round_page(kernel_size) */
	memcpy(staged, b + 2048, ksize);
	struct scion_reentry r;
	memset(&r, 0, sizeof(r));
	r.magic = SCNR_MAGIC;
	r.version = SCNR_VERSION;
	r.size = SCNR_SIZE;
	r.flags = SCNR_F_DIRECT | SCNR_F_RESTAGED | SCNR_F_WDT_ARMED | SCNR_F_LINE | SCNR_F_APPEND;
	r.entry = ~0u;
	r.method = SCNR_M_XNU;
	r.image_pa = WARM_SLOT_PA;
	r.image_size = ksize;
	scnr_sha256(staged + off, (size_t)(ksize - off), r.image_sha256);
	seal(&r);
	CHECK(scnr_check(&r, WARM_SLOT_PA) == NULL);
	CHECK(scnr_image_ok(&r, staged, own));
	staged[ksize - 1] ^= 1;                         /* the slot's last byte is covered */
	CHECK(!scnr_image_ok(&r, staged, own));
	staged[ksize - 1] ^= 1;
	printf("warm-restart-test: %s: kernel slot %llu bytes, Image header image_size 0x%llx, %llu pages copied, "
	    "payload at +0x%llx, %llu bytes hashed: taken\n", path, (unsigned long long)ksize,
	    (unsigned long long)declared, (unsigned long long)(copy / WARM_PG), (unsigned long long)off,
	    (unsigned long long)(ksize - off));
	free(staged);
	free(b);
}

int
main(int argc, char **argv)
{
	if (argc == 3) {
		boot_image(argv[1], strtoull(argv[2], NULL, 0));
		if (failures) {
			fprintf(stderr, "warm-restart-test: %d failure(s)\n", failures);
			return 1;
		}
		return 0;
	}
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
	{
		/* the slot's sizes: the header's image_size larger than the slot is what the packer always writes */
		char why[200];
		uint64_t declared, off, k = 20000000;   /* about the phone's slot: 19.9 MB */
		uint8_t *s = slot(k, 0x2842000, 0x197000);
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) == NULL);
		CHECK(declared == 0x2842000 && off == 0x197000);
		memcpy(s + 16, &k, 8);                  /* image_size == the slot: taken too */
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) == NULL);
		uint64_t v = k - 1;                     /* below the slot: the loader would cut the payload short */
		memcpy(s + 16, &v, 8);
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		v = WARM_LOADER_RAM_BASE - WARM_SLOT_PA + 1;    /* past the loader's RAM_BASE: no payload on a cold boot */
		memcpy(s + 16, &v, 8);
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		v = 0x2842000;
		memcpy(s + 16, &v, 8);
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_SLOT_PA + 0x1000000, &declared, &off, why, sizeof(why)) != NULL);
		CHECK(warm_slot(s, 0xfff, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		CHECK(warm_slot(s, 0x197000 + 0x2a7, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why,
		    sizeof(why)) != NULL);                  /* the payload's header not inside the slot */
		s[SCNR_MARK_OFFSET] = 0;                /* an older loader */
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		s[SCNR_MARK_OFFSET] = 'S';
		s[56] = 0;                              /* not an arm64 Image */
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		s[56] = 'A';
		memset(s + 0x197000, 0, 8);             /* no payload */
		CHECK(warm_slot(s, k, WARM_SLOT_PA, WARM_TWRP_K_PA, &declared, &off, why, sizeof(why)) != NULL);
		free(s);
	}
	if (failures) {
		fprintf(stderr, "warm-restart-test: %d failure(s)\n", failures);
		return 1;
	}
	printf("warm-restart-test: OK\n");
	return 0;
}
