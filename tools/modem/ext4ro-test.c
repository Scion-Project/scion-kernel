// SPDX-License-Identifier: MIT-0
/*
 * tools/modem/ext4ro-test.c: the kernel's read-only ext4 reader (drivers/modem/hakios.ext4ro.h) run on the host
 * against an image file, to check it before a phone does: ext4ro-test IMAGE PATH [EXPECTED_SHA256]
 *
 * Reads IMAGE only (opened O_RDONLY), finds PATH in it, and says the file's size and the first 8 hex digits of its
 * SHA-256; with EXPECTED_SHA256 it says MATCH or DIFFERENT. It never prints or writes the file's bytes (the file it is
 * meant for, a phone's nv_data.bin, is private).
 *
 * Build (macOS): cc -O2 -o ext4ro-test ext4ro-test.c -I ../../drivers/modem
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

#include "hakios.ext4ro.h"

static int
image_read(void *ctx, uint64_t off, void *dst, uint32_t len)
{
	ssize_t n = pread(*(int *)ctx, dst, len, (off_t)off);

	return n == (ssize_t)len ? 0 : EIO;
}

int
main(int argc, char **argv)
{
	struct ext4ro fs = { 0 };
	uint64_t size = 0, cap = 64u << 20;
	unsigned char md[CC_SHA256_DIGEST_LENGTH];
	char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
	uint8_t *buf;
	int fd, e;

	if (argc < 3) {
		fprintf(stderr, "usage: ext4ro-test IMAGE PATH [EXPECTED_SHA256]\n");
		return 2;
	}
	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}
	fs.read = image_read;
	fs.ctx = &fd;
	if ((e = ext4ro_open(&fs)) != 0) {
		printf("ext4ro: open: errno %d\n", e);
		return 1;
	}
	printf("ext4ro: block %u bytes, inode %u bytes, %u inodes a group, descriptors %u bytes, %llu blocks%s\n",
	    fs.block_size, fs.inode_size, fs.inodes_per_group, fs.desc_size, (unsigned long long)fs.blocks,
	    fs.is64 ? ", 64-bit" : "");
	fs.blk = malloc(fs.block_size);
	fs.blk2 = malloc(fs.block_size);
	buf = malloc(cap);
	if ((e = ext4ro_read_file(&fs, argv[2], buf, cap, &size)) != 0) {
		printf("ext4ro: %s: errno %d\n", argv[2], e);
		return 1;
	}
	CC_SHA256(buf, (CC_LONG)size, md);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
		snprintf(hex + 2 * i, 3, "%02x", md[i]);
	}
	printf("ext4ro: %s: %llu bytes, sha256 %.8s...\n", argv[2], (unsigned long long)size, hex);
	if (argc > 3) {
		printf("ext4ro: %s\n", strcasecmp(hex, argv[3]) == 0 ? "MATCH" : "DIFFERENT");
		return strcasecmp(hex, argv[3]) == 0 ? 0 : 1;
	}
	return 0;
}
