/* SPDX-License-Identifier: MIT-0 */
/*
 * Host test of userland/src/scion_bootline.h (tools/bootline-test.sh builds it under the sanitizers): the line is read
 * whole through a stand-in for sysctl kern.bootargs that behaves as the S9's kernel does (a buffer too small for the
 * whole line gets ENOMEM and nothing; asked with no buffer, the size with its NUL), and each argument is found exactly.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *fake_line = "";
static int fake_calls, fake_grow;

/* sysctl kern.bootargs as the fork's kern_sysctl.c answers it (sysctl_io_string, trunc 0) */
static int
fake_sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen)
{
	size_t need = strlen(fake_line) + 1;
	(void)newp;
	(void)newlen;
	fake_calls++;
	if (strcmp(name, "kern.bootargs") != 0) {
		errno = ENOENT;
		return -1;
	}
	if (old == NULL) {
		*oldlen = need - (fake_grow ? 600 : 0);    /* fake_grow: the size said is short of the line */
		return 0;
	}
	if (*oldlen < need) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(old, fake_line, need);
	*oldlen = need;
	return 0;
}

#define SCION_BOOTLINE_SYSCTLBYNAME fake_sysctlbyname
#include "../userland/src/scion_bootline.h"

static int failures, checks;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		checks++;                                                              \
		if (!(cond)) {                                                         \
			failures++;                                                    \
			fprintf(stderr, "bootline_test:%d: FAILED: %s\n", __LINE__, #cond); \
		}                                                                      \
	} while (0)

static int
value_is(const char *line, const char *name, const char *want)
{
	size_t length = 99;
	const char *v = scion_boot_arg_in(line, name, &length);
	if (want == NULL) {
		return v == NULL;
	}
	return v != NULL && length == strlen(want) && strncmp(v, want, length) == 0;
}

/* the S9's line on 24 Nov 2026 (L6btm, phone 2): 1108 bytes, the loader's 434 and the kernel's built-in defaults */
static const char s9_line[] =
    "-n sbgl=0 sweetspeed=442337279 sweetfb_lh=1702 wifi_delay=10 sweetepoch=1794957305 "
    "sweetinject=/usr/lib/s.dylib,backboardd,Preferences;/usr/lib/m.dylib,PosterBoard,SpringBoard;/usr/lib/b.dylib,"
    "backboardd;/usr/lib/c.dylib,SpringBoard;/usr/lib/cam.dylib,cameracaptured,Camera;/usr/lib/o.dylib,backboardd "
    "bt_vsc=1 bt_addr_efs=1 wifi_pcie=6 brcmfmac=8 sweet_priv_ent=1 bt_module=semco scion_ldr=0x80178000,0x1180000 "
    "scion_guard=0xfed17fc0 debug=0xc serial=3 cpus=8 rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 "
    "cs_enforcement_disable=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 a5fb_keep_on=1 sweetfb_scale=1 "
    "sweetfb_lw=828 a5fb_noconsole=1 a5fb_hz=60 sweetfb_gpu=1 vm_compression_limit=196608 cs_integrity_skip=1 "
    "sweetmemscale=4 sweetacl=1 sweetreboot=86400 usb_gadget=5 sbprof=0 sbmtl=1 extra_ram=1 extra_ram_mask=127 "
    "cpufreq_gov=1 cpufreq_gov_big_min=1794000 sweetm3unpin=2 tmu=3 shub=6 shub_hid=3 sweethalt=1 sweetfg=1 "
    "rtc_pmic=1 persona_lax=1 abox=8 max98512=3 cs47l93=6 abox_mic=1 abox_rmem=1 bt=7 cp=7 cp_nv=2 cp_delay=40 "
    "cp_rfs=1 cp_if=1 cam_s9=7 cam_switch=1 sgu_irq=1 decmpfs_lz4=1 sweetatpath=1";

static void
test_parse(void)
{
	/* exact names: no prefix, no suffix */
	CHECK(value_is("xfoo=1", "foo", NULL));
	CHECK(value_is("foox=1", "foo", NULL));
	CHECK(value_is("xfoo=1 foo=2", "foo", "2"));
	CHECK(value_is("a=foo=1", "foo", NULL));               /* inside another's value */
	CHECK(value_is("foo=10", "foo", "10"));
	CHECK(!scion_boot_arg_is_in("foo=10", "foo", "1"));
	CHECK(!scion_boot_arg_is_in("foo=1", "foo", "10"));
	CHECK(scion_boot_arg_is_in("foo=1", "foo", "1"));
	CHECK(!scion_boot_arg_is_in("xfoo=1", "foo", "1"));
	CHECK(!scion_boot_arg_is_in("foo=1x", "foo", "1"));
	/* where it stands: first, last, alone, after blanks and tabs */
	CHECK(value_is("foo=a b=c", "foo", "a"));
	CHECK(value_is("b=c foo=a", "foo", "a"));
	CHECK(value_is("  foo=a  ", "foo", "a"));
	CHECK(value_is("b=c\tfoo=a\tx=y", "foo", "a"));
	CHECK(value_is("b=c\t\tfoo=a", "foo", "a"));
	/* the first word of the name wins (PE_parse_boot_argn) */
	CHECK(value_is("foo=1 foo=2", "foo", "1"));
	/* bare words and empty values */
	CHECK(value_is("-v foo", "foo", ""));
	CHECK(value_is("foo= x=1", "foo", ""));
	CHECK(scion_boot_arg_set_in("-v -n", "-n"));
	CHECK(!scion_boot_arg_set_in("-v -nx", "-n"));
	CHECK(scion_boot_arg_set_in("metal_fps=draws", "metal_fps"));
	CHECK(!scion_boot_arg_set_in("metal_fps_x=1", "metal_fps"));
	CHECK(!scion_boot_arg_is_in("foo", "foo", "1"));
	CHECK(scion_boot_arg_is_in("foo=", "foo", ""));
	/* a value with = and : in it */
	CHECK(value_is("metal_dump=cfg:a=b,c", "metal_dump", "cfg:a=b,c"));
	/* nothing to find */
	CHECK(value_is("", "foo", NULL));
	CHECK(value_is(NULL, "foo", NULL));
	CHECK(value_is("foo=1", "", NULL));
	CHECK(value_is("=1 foo=2", "foo", "2"));
	/* numbers */
	CHECK(scion_boot_arg_number_in("n=42", "n", -1) == 42);
	CHECK(scion_boot_arg_number_in("n=0x10", "n", -1) == 16);
	CHECK(scion_boot_arg_number_in("n=-3", "n", 7) == -3);
	CHECK(scion_boot_arg_number_in("n=4x", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("xn=5", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=99999999999999999999999", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=010", "n", 7) == 8);
	CHECK(scion_boot_arg_number_in("n=0", "n", 7) == 0);
	CHECK(scion_boot_arg_number_in("n=0x", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=09", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=0xFfff", "n", 7) == 0xffff);
	CHECK(scion_boot_arg_number_in("n=9223372036854775807", "n", 7) == 9223372036854775807LL);
	CHECK(scion_boot_arg_number_in("n=9223372036854775808", "n", 7) == 7);
	CHECK(scion_boot_arg_number_in("n=-9223372036854775808", "n", 7) == -9223372036854775807LL - 1);
	CHECK(scion_boot_arg_number_in("n=-", "n", 7) == 7);
	/* copies */
	char buf[8];
	CHECK(scion_boot_arg_copy_in("a=hello b=2", "a", buf, sizeof(buf)) == 1 && strcmp(buf, "hello") == 0);
	CHECK(scion_boot_arg_copy_in("a=helloworld", "a", buf, sizeof(buf)) == 1 && strcmp(buf, "hellowo") == 0);
	CHECK(scion_boot_arg_copy_in("b=1", "a", buf, sizeof(buf)) == 0);
	CHECK(scion_boot_arg_copy_in("a b=1", "a", buf, sizeof(buf)) == 1 && buf[0] == '\0');
	CHECK(scion_boot_arg_copy_in("a=1", "a", NULL, 0) == 1);
}

static void
test_s9_line(void)
{
	CHECK(strlen(s9_line) == 1108);
	CHECK(scion_boot_arg_is_in(s9_line, "sbgl", "0"));
	CHECK(scion_boot_arg_is_in(s9_line, "sbprof", "0"));
	CHECK(scion_boot_arg_is_in(s9_line, "sbmtl", "1"));
	CHECK(scion_boot_arg_set_in(s9_line, "-n"));
	CHECK(scion_boot_arg_is_in(s9_line, "sweetatpath", "1"));     /* the last word, past 1 KB */
	CHECK(scion_boot_arg_is_in(s9_line, "cam_s9", "7"));
	CHECK(scion_boot_arg_number_in(s9_line, "sweetspeed", 0) == 442337279);  /* the loader's, not the default */
	CHECK(scion_boot_arg_number_in(s9_line, "debug", 0) == 0xc);
	CHECK(!scion_boot_arg_set_in(s9_line, "sweet"));
	CHECK(!scion_boot_arg_set_in(s9_line, "cam"));
	CHECK(!scion_boot_arg_set_in(s9_line, "bt_addr"));
	CHECK(!scion_boot_arg_set_in(s9_line, "metal"));
}

static void
test_read(void)
{
	/* the whole S9 line through the kernel's answer: a 1 KB buffer would get ENOMEM */
	char small[1024];
	size_t n = sizeof(small) - 1;
	fake_line = s9_line;
	CHECK(fake_sysctlbyname("kern.bootargs", small, &n, NULL, 0) == -1 && errno == ENOMEM);
	char *line = scion_boot_line_read();
	CHECK(line != NULL && strcmp(line, s9_line) == 0);
	SCION_BOOTLINE_FREE(line);

	/* the longest line the kernel has (2047 bytes), its last word at the end */
	static char longest[SCION_BOOT_LINE_MAX];
	memset(longest, 'x', sizeof(longest) - 1);
	longest[0] = 'a';
	longest[1] = '=';
	memcpy(longest + sizeof(longest) - 1 - 6, " end=1", 6);
	longest[sizeof(longest) - 1] = '\0';
	fake_line = longest;
	line = scion_boot_line_read();
	CHECK(line != NULL && strlen(line) == SCION_BOOT_LINE_MAX - 1 && scion_boot_arg_is_in(line, "end", "1"));
	SCION_BOOTLINE_FREE(line);

#ifdef SCION_BOOTLINE_STATIC
	/* libmini's build: the kernel's longest line is all it holds; a longer one is not read at all (never cut) */
	char *longer = malloc(5000);
	memset(longer, 'y', 4999);
	longer[4999] = '\0';
	fake_line = longer;
	CHECK(scion_boot_line_read() == NULL);
	free(longer);
#else
	/* longer than the kernel's limit (another kernel): still whole, by the size asked */
	char *longer = malloc(5000);
	memset(longer, 'y', 4999);
	memcpy(longer, "z=", 2);
	memcpy(longer + 4999 - 6, " end=2", 6);
	longer[4999] = '\0';
	fake_line = longer;
	line = scion_boot_line_read();
	CHECK(line != NULL && strlen(line) == 4999 && scion_boot_arg_is_in(line, "end", "2"));
	free(line);

	/* the size said is short (the line grew between the two calls): asked again, never cut */
	fake_grow = 1;
	fake_calls = 0;
	line = scion_boot_line_read();
	CHECK(line != NULL && strlen(line) == 4999);
	free(line);
	fake_grow = 0;
	free(longer);
#endif

	/* read once and kept */
	fake_line = s9_line;
	fake_calls = 0;
	CHECK(scion_boot_arg_is("sbgl", "0"));
	int after_first = fake_calls;
	CHECK(scion_boot_arg_is("sweetatpath", "1") && scion_boot_arg_set("-n") && !scion_boot_arg_set("sweet"));
	CHECK(scion_boot_arg_number("cpus", 0) == 8);
	char value[16];
	CHECK(scion_boot_arg_copy("bt_module", value, sizeof(value)) && strcmp(value, "semco") == 0);
	size_t length = 0;
	const char *v = scion_boot_arg("ufsroot", &length);
	CHECK(v != NULL && length == 8 && strncmp(v, "USERDATA", length) == 0);
	CHECK(after_first > 0 && fake_calls == after_first);
	CHECK(strcmp(scion_boot_line(), s9_line) == 0);
}

int
main(void)
{
	test_parse();
	test_s9_line();
	test_read();
	if (failures != 0) {
		fprintf(stderr, "bootline_test: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("bootline_test: %d checks passed\n", checks);
	return 0;
}
