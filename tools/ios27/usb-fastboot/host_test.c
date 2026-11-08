/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the loader's fastboot protocol, console shell, USB descriptors and control
 * requests (xnuloader/sboot/fastboot.c, usb_desc.c): the pure parts of UO1/FB1, run on the Mac with
 * stub platform functions. tools/ios27/usb-fastboot/run-host-test.sh builds and runs it.
 * The DWC3/PHY/MAX77705 code is hardware-only and is not exercised here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fastboot.h"
#include "usb_desc.h"
#include "catalog.h"
#include "hw.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- stub platform ---- */
static char next_key[32];
static unsigned prep_method;
static struct fb_image prep_img;
static int prep_calls;
const char *fbp_serial(void) { return "R38K30ABCDE"; }
int fbp_battery(unsigned *mv, unsigned *pct) { *mv = 3912; *pct = 77; return 1; }
unsigned fbp_cat_count(void) { return 3; }
int fbp_cat_entry(unsigned i, const char **key, const char **name, unsigned *m, int *ready)
{
    static const char *const keys[] = { "ios16", "linux", "windows" };
    static const char *const names[] = { "iOS 16.5", "Ubuntu", "Windows 11" };
    static const unsigned ms[] = { M_XNU, M_LINUX, M_UEFI };
    if (i >= 3)
        return 0;
    *key = keys[i];
    *name = names[i];
    *m = ms[i];
    *ready = i != 2;
    return 1;
}
int fbp_cat_find(const char *key)
{
    for (unsigned i = 0; i < 3; ++i) {
        const char *k, *n;
        unsigned m;
        int r;
        fbp_cat_entry(i, &k, &n, &m, &r);
        if (!strcmp(k, key))
            return (int)i;
    }
    return -1;
}
static const char fake_log[] =
    "STARBITE: loader start\n"
    "a line that is longer than sixty characters so the INFO splitter has to cut it in two\n"
    "\n"
    "last line without newline";
unsigned fbp_log(const char *which, char *out, unsigned cap)
{
    if (strcmp(which, "pmsg") && strcmp(which, "usb"))
        return 0;
    unsigned n = (unsigned)strlen(fake_log);
    if (n > cap)
        n = cap;
    memcpy(out, fake_log, n);
    return n;
}
int fbp_boot_prepare(unsigned method, const struct fb_image *img, const char **why)
{
    ++prep_calls;
    prep_method = method;
    prep_img = *img;
    if (method == M_LINUX && !img->dtb_size) {
        *why = "NO DTB";
        return 0;
    }
    return 1;
}
void fbp_set_next(const char *key) { snprintf(next_key, sizeof(next_key), "%s", key); }
const char *fbp_speed(void) { return "high"; }
void fbp_status(const char *line) { (void)line; }

/* ---- a fake transport ---- */
#define DL_SIZE (8U << 20)
static uint8_t *dl;
static char scratch[65536], stage[65536];
static char resp[64][FB_RESP_MAX + 1];
static unsigned nresp;
static uint8_t raw[65536];
static unsigned nraw;

static int uploading;   /* the command was "upload": DATA is followed by that many raw bytes */

static void drain(void)
{
    uint8_t out[1000];   /* small on purpose: upload data comes in several pieces */
    uint32_t n, raw_left = 0;
    nresp = 0;
    nraw = 0;
    while ((n = fb_tx_next(out, sizeof(out))) != 0) {
        if (raw_left) {
            CHECK(n <= raw_left, "upload piece too long");
            memcpy(raw + nraw, out, n);
            nraw += n;
            raw_left -= n;
            continue;
        }
        if (uploading && n == 12 && !memcmp(out, "DATA", 4)) {
            char h[9];
            memcpy(h, out + 4, 8);
            h[8] = 0;
            raw_left = (uint32_t)strtoul(h, 0, 16);
        }
        CHECK(n <= FB_RESP_MAX, "response longer than 64 bytes (%u)", n);
        if (nresp < 64) {
            memcpy(resp[nresp], out, n);
            resp[nresp][n] = 0;
            ++nresp;
        }
    }
}

static void cmd(const char *c)
{
    uploading = !strcmp(c, "upload");
    fb_command((const uint8_t *)c, (unsigned)strlen(c));
    drain();
}

static const char *last(void) { return nresp ? resp[nresp - 1] : ""; }

static void send_data(const uint8_t *d, uint32_t n, uint32_t chunk)
{
    uint32_t off = 0;
    while (fb_rx_want()) {
        uint32_t w = fb_rx_want(), k = w < chunk ? w : chunk;
        if (k > n - off)
            k = n - off;
        memcpy((void *)fb_rx_ptr(), d + off, k);
        off += k;
        fb_rx_data(k);
    }
    drain();
}

static void download(const uint8_t *d, uint32_t n)
{
    char c[32];
    snprintf(c, sizeof(c), "download:%08x", n);
    cmd(c);
    char want[16];
    snprintf(want, sizeof(want), "DATA%08x", n);
    CHECK(!strcmp(last(), want), "download: got %s", last());
    send_data(d, n, 3000);
    CHECK(!strcmp(last(), "OKAY"), "after data: %s", last());
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* a fake arm64 Image of n bytes */
static void arm64(uint8_t *p, uint32_t n)
{
    memset(p, 0, n);
    put64(p + 8, 0x80000);
    put64(p + 16, n);
    memcpy(p + 56, "ARMd", 4);
}

static uint32_t pg(uint32_t v, uint32_t page) { return (v + page - 1) / page * page; }

/* AOSP boot_img_hdr v0..v2 (page 2048) */
static uint32_t bootimg(uint8_t *b, uint32_t hv, const uint8_t *k, uint32_t ks, const uint8_t *r, uint32_t rs,
                        const uint8_t *dtb, uint32_t ds, const char *cmdline)
{
    const uint32_t page = 2048;
    memset(b, 0, page);
    memcpy(b, "ANDROID!", 8);
    put32(b + 8, ks);
    put32(b + 12, 0x10008000);
    put32(b + 16, rs);
    put32(b + 20, 0x11000000);
    put32(b + 24, 0);
    put32(b + 32, 0x10000100);
    put32(b + 36, page);
    put32(b + 40, hv);
    size_t cl = strlen(cmdline);
    memcpy(b + 64, cmdline, cl < 512 ? cl : 512);
    if (cl > 512)
        memcpy(b + 608, cmdline + 512, cl - 512);
    uint32_t off = page;
    memcpy(b + off, k, ks);
    off += pg(ks, page);
    if (rs)
        memcpy(b + off, r, rs);
    off += pg(rs, page);
    if (hv >= 1) {
        put32(b + 1632, 0);
        put32(b + 1644, hv == 2 ? 1660 : 1648);
    }
    if (hv == 2) {
        put32(b + 1648, ds);
        memcpy(b + off, dtb, ds);
        off += pg(ds, page);
    }
    return off;
}

static void test_descriptors(void)
{
    uint8_t d[512];
    unsigned n = usb_desc_device(d);
    CHECK(n == 18 && d[0] == 18 && d[1] == 1, "device descriptor");
    CHECK(d[4] == 0xef && d[5] == 2 && d[6] == 1, "IAD device class");
    CHECK(d[7] == 64, "ep0 64");
    CHECK((d[8] | d[9] << 8) == 0x18d1 && (d[10] | d[11] << 8) == 0x4ee0, "VID/PID");
    CHECK(d[2] == 0x00 && d[3] == 0x02, "bcdUSB 2.00");

    for (int fs = 0; fs < 2; ++fs) {
        n = usb_desc_config(d, fs, 2);
        CHECK(n == USB_CONFIG_LEN && (unsigned)(d[2] | d[3] << 8) == n, "config length %u", n);
        CHECK(d[4] == 3, "three interfaces");
        unsigned i = 0, ifs = 0, eps = 0, iad = 0, fbif = 0, cur_if = 99;
        while (i < n) {
            CHECK(d[i] >= 2 && i + d[i] <= n, "descriptor walk at %u", i);
            if (d[i] < 2)
                break;
            if (d[i + 1] == 11) {
                ++iad;
                CHECK(ifs == 0 && d[i + 2] == 0 && d[i + 3] == 2, "IAD covers 0-1 and comes first");
            }
            if (d[i + 1] == 4) {
                ++ifs;
                cur_if = d[i + 2];
                if (d[i + 5] == 0xff && d[i + 6] == 0x42 && d[i + 7] == 3) {
                    ++fbif;
                    CHECK(d[i + 4] == 2, "fastboot has 2 endpoints");
                }
            }
            if (d[i + 1] == 5) {
                ++eps;
                unsigned mps = d[i + 4] | d[i + 5] << 8, a = d[i + 2];
                if (a == 0x81)
                    CHECK(mps == 16 && d[i + 3] == 3 && cur_if == 0, "notify ep");
                else
                    CHECK(mps == (fs ? 64u : 512u) && d[i + 3] == 2, "bulk ep %02x mps %u", a, mps);
                if (a == 0x83 || a == 0x03)
                    CHECK(cur_if == 2, "fastboot ep in interface 2");
            }
            i += d[i];
        }
        CHECK(i == n && ifs == 3 && eps == 5 && iad == 1 && fbif == 1, "config walk ifs %u eps %u", ifs, eps);
    }
    n = usb_desc_config(d, 1, 7);
    CHECK(d[1] == 7, "other speed type");
    n = usb_desc_qualifier(d);
    CHECK(n == 10 && d[1] == 6, "qualifier");
    n = usb_desc_string(d, 0, "X");
    CHECK(n == 4 && d[2] == 0x09 && d[3] == 0x04, "langid");
    n = usb_desc_string(d, 3, "R38K30ABCDE");
    CHECK(n == 2 + 2 * 11 && d[2] == 'R' && d[3] == 0, "serial string");
    n = usb_desc_string(d, 5, 0);
    CHECK(n == 2 + 16 && d[2] == 'f', "fastboot string");
    CHECK(usb_desc_string(d, 9, 0) == 0, "no string 9");
    CHECK(USB_PHYS(0x83) == 7 && USB_PHYS(0x03) == 6 && USB_PHYS(0x81) == 3 && USB_PHYS(0x02) == 4, "phys numbers");
}

static void setup(uint8_t *s, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length)
{
    s[0] = type; s[1] = req; s[2] = (uint8_t)value; s[3] = (uint8_t)(value >> 8);
    s[4] = (uint8_t)index; s[5] = (uint8_t)(index >> 8); s[6] = (uint8_t)length; s[7] = (uint8_t)(length >> 8);
}

static void test_ch9(void)
{
    struct usb_dev_state st;
    struct ch9_res r;
    uint8_t s[8];
    usb_ch9_init(&st, "SER");
    setup(s, 0x80, 6, 0x0100, 0, 64);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 18, "get device 64");
    setup(s, 0x80, 6, 0x0100, 0, 8);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 8, "get device 8 (macOS first read)");
    setup(s, 0x80, 6, 0x0200, 0, 9);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 9 && r.data[2] == USB_CONFIG_LEN, "config header");
    setup(s, 0x80, 6, 0x0200, 0, 0xff);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == USB_CONFIG_LEN, "config full");
    setup(s, 0x80, 6, 0x0303, 0x0409, 0xff);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 8 && r.data[2] == 'S', "serial");
    setup(s, 0x80, 6, 0x0f00, 0, 5);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "BOS stalls (USB 2.00)");
    setup(s, 0x80, 6, 0x0600, 0, 10);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 10, "qualifier");
    setup(s, 0x00, 5, 7, 0, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_ACK && r.set_address == 7, "set address");
    setup(s, 0x00, 9, 1, 0, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_ACK && r.set_config == 1 && st.config == 1, "set config");
    setup(s, 0x80, 8, 0, 0, 1);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 1 && r.data[0] == 1, "get config");
    setup(s, 0x00, 9, 2, 0, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "set config 2 stalls");
    setup(s, 0x02, 1, 0, 0x83, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_ACK && r.clear_halt == 0x83, "clear halt");
    setup(s, 0x01, 11, 0, 2, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_ACK, "set interface 2 alt 0");
    setup(s, 0x01, 11, 1, 2, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "set interface alt 1 stalls");
    setup(s, 0x81, 10, 0, 1, 1);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.data[0] == 0, "get interface");
    setup(s, 0x21, 0x20, 0, 0, 7);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_OUT && r.len == 7, "set line coding");
    uint8_t lc[7] = { 0x80, 0x25, 0, 0, 0, 0, 8 };
    usb_ch9_out(&st, s, lc, 7);
    setup(s, 0xa1, 0x21, 0, 0, 7);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 7 && r.data[0] == 0x80 && r.data[1] == 0x25, "get line coding 9600");
    setup(s, 0x21, 0x22, 3, 0, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_ACK && r.line_changed && st.line_state == 3, "control line state");
    setup(s, 0x21, 0x22, 3, 2, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "class request to the fastboot interface stalls");
    setup(s, 0x40, 1, 0, 0, 0);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "vendor request stalls");
    setup(s, 0x00, 7, 0x0100, 0, 18);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_STALL, "SET_DESCRIPTOR stalls");
    setup(s, 0x80, 0, 0, 0, 2);
    usb_ch9_setup(&st, s, &r);
    CHECK(r.action == CH9_IN && r.len == 2, "get status");
}

static void test_images(void)
{
    static struct fb_image im;
    const char *why;
    static uint8_t k[20000], b[100000];
    arm64(k, sizeof(k));
    CHECK(fb_kernel_kind((uintptr_t)k, sizeof(k)) == FB_K_ARM64, "arm64 kind");
    CHECK(fb_image_parse((uintptr_t)k, sizeof(k), &im, &why) && im.header_version == 0xffffffffU &&
          im.kernel == (uintptr_t)k, "raw image");
    CHECK(fb_pick_method(&im, 0) == M_BARE, "raw arm64 = bare");
    CHECK(!fb_has_fv((uintptr_t)k, sizeof(k)), "no FV");
    memcpy(k + 0x1000 + 0x28, "_FVH", 4);
    CHECK(fb_has_fv((uintptr_t)k, sizeof(k)) && fb_pick_method(&im, 0) == M_UEFI, "FV -> uefi");
    memset(k + 0x1000 + 0x28, 0, 4);

    uint8_t rd[3000];
    memset(rd, 0x5a, sizeof(rd));
    char longcl[700];
    memset(longcl, 'x', sizeof(longcl) - 1);
    longcl[sizeof(longcl) - 1] = 0;
    longcl[0] = 'A';
    longcl[600] = 'B';
    uint32_t n = bootimg(b, 0, k, sizeof(k), rd, sizeof(rd), 0, 0, longcl);
    CHECK(fb_image_parse((uintptr_t)b, n, &im, &why), "v0 parse: %s", why ? why : "");
    CHECK(im.kernel == (uintptr_t)b + 2048 && im.kernel_size == sizeof(k), "v0 kernel");
    CHECK(im.ramdisk == (uintptr_t)b + 2048 + pg(sizeof(k), 2048) && im.ramdisk_size == sizeof(rd), "v0 ramdisk");
    CHECK(strlen(im.cmdline) == 699 && im.cmdline[0] == 'A' && im.cmdline[600] == 'B', "cmdline continues in extra_cmdline (%zu)", strlen(im.cmdline));
    CHECK(fb_pick_method(&im, 0) == M_ANDROID, "kernel + ramdisk, no dtb = android");
    CHECK(fb_pick_method(&im, M_LINUX) == M_LINUX, "forced linux");
    CHECK(fb_pick_method(&im, M_XNU) == 0, "forced xnu on an arm64 Image refused");

    uint8_t dtb[500];
    memset(dtb, 0xd0, sizeof(dtb));
    n = bootimg(b, 2, k, sizeof(k), rd, sizeof(rd), dtb, sizeof(dtb), "console=ttySAC0");
    CHECK(fb_image_parse((uintptr_t)b, n, &im, &why), "v2 parse");
    CHECK(im.dtb_size == sizeof(dtb) && ((uint8_t *)im.dtb)[0] == 0xd0 &&
          im.dtb == (uintptr_t)b + 2048 + pg(sizeof(k), 2048) + pg(sizeof(rd), 2048), "v2 dtb");
    CHECK(fb_pick_method(&im, 0) == M_LINUX && !strcmp(im.cmdline, "console=ttySAC0"), "v2 = linux");

    /* what `fastboot boot foo.xnu` sends: a v0 image with the payload as its kernel */
    uint8_t x[4096];
    memset(x, 0, sizeof(x));
    memcpy(x, "SWEETXNU", 8);
    n = bootimg(b, 0, x, sizeof(x), 0, 0, 0, 0, "");
    CHECK(fb_image_parse((uintptr_t)b, n, &im, &why) && fb_pick_method(&im, 0) == M_XNU, "wrapped xnu");

    n = bootimg(b, 0, k, sizeof(k), rd, sizeof(rd), 0, 0, "");
    CHECK(!fb_image_parse((uintptr_t)b, n - 4096, &im, &why) && why, "truncated boot image refused");
    put32(b + 36, 1000);
    CHECK(!fb_image_parse((uintptr_t)b, n, &im, &why), "bad page size refused");
    put32(b + 36, 2048);
    put32(b + 40, 9);
    CHECK(!fb_image_parse((uintptr_t)b, n, &im, &why), "header version 9 refused");

    /* v3: page 4096, cmdline at 44 */
    memset(b, 0, 4096);
    memcpy(b, "ANDROID!", 8);
    put32(b + 8, sizeof(k));
    put32(b + 12, 0);
    put32(b + 40, 3);
    strcpy((char *)b + 44, "v3line");
    memcpy(b + 4096, k, sizeof(k));
    CHECK(fb_image_parse((uintptr_t)b, 4096 + sizeof(k), &im, &why) && im.kernel == (uintptr_t)b + 4096 &&
          !strcmp(im.cmdline, "v3line") && !im.ramdisk_size, "v3 parse");

    uint8_t garbage[100];
    memset(garbage, 0x11, sizeof(garbage));
    CHECK(fb_image_parse((uintptr_t)garbage, sizeof(garbage), &im, &why) && fb_pick_method(&im, 0) == 0, "garbage: no method");
}

static void test_protocol(void)
{
    dl = aligned_alloc(4096, DL_SIZE);
    fb_init((uintptr_t)dl, DL_SIZE - 4096, scratch, sizeof(scratch), stage, sizeof(stage));
    cmd("getvar:version");
    CHECK(!strcmp(last(), "OKAY0.4"), "version: %s", last());
    cmd("getvar:product");
    CHECK(!strcmp(last(), "OKAYstarlte"), "product");
    cmd("getvar:serialno");
    CHECK(!strcmp(last(), "OKAYR38K30ABCDE"), "serialno");
    cmd("getvar:max-download-size");
    CHECK(!strcmp(last(), "OKAY0x007ff000"), "max-download-size: %s", last());
    cmd("getvar:battery-voltage");
    CHECK(!strcmp(last(), "OKAY3912"), "battery");
    cmd("getvar:has-slot:boot");
    CHECK(!strcmp(last(), "OKAYno"), "has-slot");
    cmd("getvar:catalog:1");
    CHECK(!strcmp(last(), "OKAYlinux linux ready"), "catalog:1 %s", last());
    cmd("getvar:catalog:7");
    CHECK(!strncmp(last(), "FAIL", 4), "catalog:7");
    cmd("getvar:nosuch");
    CHECK(!strncmp(last(), "FAIL", 4), "unknown var");
    cmd("getvar:all");
    CHECK(nresp > 10 && !strcmp(last(), "OKAY"), "getvar all: %u", nresp);
    int saw_product = 0;
    for (unsigned i = 0; i + 1 < nresp; ++i) {
        CHECK(!strncmp(resp[i], "INFO", 4), "all: INFO lines");
        if (!strcmp(resp[i], "INFOproduct: starlte"))
            saw_product = 1;
    }
    CHECK(saw_product, "all lists product");
    cmd("getvar:catalog");
    CHECK(nresp == 4 && !strcmp(last(), "OKAY3 entries") && strstr(resp[2], "windows: Windows 11 (uefi, missing files)"), "catalog list: %s", resp[2]);

    cmd("boot");
    CHECK(!strcmp(last(), "FAILnothing downloaded"), "boot without download");
    cmd("download:zz");
    CHECK(!strncmp(last(), "FAIL", 4), "bad size");
    cmd("download:ffffffff");
    CHECK(!strcmp(last(), "FAILdata too large"), "too large");
    cmd("download:0");
    CHECK(!strncmp(last(), "FAIL", 4), "zero size");

    static uint8_t img[200000];
    arm64(img, sizeof(img));
    download(img, sizeof(img));
    CHECK(fb_dl_size() == sizeof(img) && !memcmp(dl, img, sizeof(img)), "download content");
    prep_calls = 0;
    cmd("boot");
    CHECK(prep_calls == 1 && prep_method == M_BARE && prep_img.kernel == (uintptr_t)dl, "boot bare");
    CHECK(nresp == 2 && !strncmp(resp[0], "INFObooting as bare", 19) && !strcmp(last(), "OKAY"), "boot responses %s", resp[0]);
    uint32_t v;
    CHECK(fb_action(&v) == FB_ACT_BOOT, "boot action");
    CHECK(fb_action(&v) == FB_ACT_NONE, "action taken once");

    /* `fastboot boot foo.xnu` */
    static uint8_t b[300000];
    uint8_t x[8192];
    memset(x, 0, sizeof(x));
    memcpy(x, "SWEETXNU", 8);
    uint32_t n = bootimg(b, 0, x, sizeof(x), 0, 0, 0, 0, "debug=0x8");
    download(b, n);
    cmd("boot");
    CHECK(prep_method == M_XNU && prep_img.kernel == (uintptr_t)dl + 2048 && prep_img.kernel_size == sizeof(x) &&
          !strcmp(prep_img.cmdline, "debug=0x8"), "boot xnu");
    (void)fb_action(&v);

    /* SBUEFI!! bundle */
    static uint8_t u[50000];
    memset(u, 0, sizeof(u));
    put64(u, 0x2121494645554253ULL);
    put64(u + 8, 30000);
    arm64(u + 4096, 30000);
    download(u, sizeof(u));
    cmd("boot");
    CHECK(prep_method == M_UEFI && prep_img.kernel == (uintptr_t)dl + 4096 && prep_img.kernel_size == 30000, "bundle uefi");
    (void)fb_action(&v);

    /* forced linux without a DTB: the start path refuses, FAIL reaches the host */
    cmd("oem boot-as linux");
    CHECK(!strcmp(last(), "OKAYlinux"), "boot-as");
    download(img, sizeof(img));
    cmd("boot");
    CHECK(!strcmp(last(), "FAILNO DTB"), "linux without dtb: %s", last());
    CHECK(fb_action(&v) == FB_ACT_NONE, "no action after FAIL");
    cmd("oem boot-as nonsense");
    CHECK(!strncmp(last(), "FAIL", 4), "bad boot-as");
    cmd("oem boot-as auto");
    CHECK(!strcmp(last(), "OKAYauto"), "auto");
    cmd("getvar:boot-as");
    CHECK(!strcmp(last(), "OKAYauto"), "getvar boot-as");

    static uint8_t junk[5000];
    memset(junk, 0x33, sizeof(junk));
    download(junk, sizeof(junk));
    cmd("boot");
    CHECK(!strncmp(last(), "FAILunknown image", 17), "junk image: %s", last());

    /* logs */
    cmd("upload");
    CHECK(!strncmp(last(), "FAIL", 4), "upload before staging");
    cmd("oem log");
    CHECK(!strcmp(last(), "OKAYstaged: fastboot get_staged <file>"), "oem log ok");
    CHECK(nresp == 6, "oem log lines %u", nresp);
    CHECK(!strcmp(resp[0], "INFOSTARBITE: loader start"), "line 1 %s", resp[0]);
    CHECK(strlen(resp[1]) == 64 && strlen(resp[2]) < 64, "long line split");
    CHECK(!strcmp(resp[3], "INFO"), "empty line");
    CHECK(!strcmp(resp[4], "INFOlast line without newline"), "last line");
    cmd("upload");
    char want[16];
    snprintf(want, sizeof(want), "DATA%08x", (unsigned)strlen(fake_log));
    CHECK(nresp == 2 && !strcmp(resp[0], want) && !strcmp(last(), "OKAY"), "upload responses %s", resp[0]);
    CHECK(nraw == strlen(fake_log) && !memcmp(raw, fake_log, nraw), "upload data (%u)", nraw);
    cmd("oem log nosuch");
    CHECK(!strncmp(last(), "FAIL", 4), "unknown log");

    /* reboots */
    cmd("oem reboot-to nosuch");
    CHECK(!strncmp(last(), "FAIL", 4) && fb_action(&v) == FB_ACT_NONE, "reboot-to nosuch");
    cmd("oem reboot-to linux");
    CHECK(!strcmp(last(), "OKAY") && !strcmp(next_key, "linux"), "reboot-to linux");
    CHECK(fb_action(&v) == FB_ACT_REBOOT && v == INFORM_NORMAL, "reboot-to action");
    next_key[0] = 0;
    cmd("oem boot-next fastboot");
    CHECK(!strcmp(last(), "OKAY") && !strcmp(next_key, "fastboot") && fb_action(&v) == FB_ACT_NONE, "boot-next sets, no reboot");
    cmd("oem boot-next nosuch");
    CHECK(!strncmp(last(), "FAIL", 4), "boot-next nosuch");
    next_key[0] = 0;
    cmd("reboot-bootloader");
    CHECK(!strcmp(next_key, "fastboot") && fb_action(&v) == FB_ACT_REBOOT && v == INFORM_NORMAL, "reboot-bootloader");
    cmd("reboot-recovery");
    CHECK(fb_action(&v) == FB_ACT_REBOOT && v == INFORM_RECOVERY, "reboot-recovery");
    cmd("reboot-download");
    CHECK(fb_action(&v) == FB_ACT_REBOOT && v == INFORM_DOWNLOAD, "reboot-download");
    cmd("reboot");
    CHECK(fb_action(&v) == FB_ACT_REBOOT && v == INFORM_NORMAL, "reboot");
    cmd("continue");
    CHECK(!strcmp(last(), "OKAY") && fb_action(&v) == FB_ACT_MENU, "continue");
    cmd("flash:boot");
    CHECK(!strncmp(last(), "FAIL", 4), "flash refused");
    cmd("erase:userdata");
    CHECK(!strncmp(last(), "FAIL", 4), "erase refused");
    cmd("oem help");
    CHECK(nresp > 5 && !strcmp(last(), "OKAY"), "oem help");
    cmd("whatever");
    CHECK(!strcmp(last(), "FAILunknown command"), "unknown");

    /* a cable pulled during a download */
    cmd("download:00100000");
    CHECK(fb_rx_want() == 0x100000, "want");
    memcpy((void *)fb_rx_ptr(), img, 1000);
    fb_rx_data(1000);
    fb_usb_reset();
    CHECK(fb_rx_want() == 0 && fb_dl_size() == 0 && fb_tx_idle(), "reset drops the download");
    cmd("getvar:version");
    CHECK(!strcmp(last(), "OKAY0.4"), "usable after reset");
    /* a zero-length packet (ZLP after data) is ignored */
    fb_command((const uint8_t *)"", 0);
    CHECK(fb_tx_idle(), "zlp ignored");
    free(dl);
}

static char ring[8192], cscr[65536];

static void con_send(const char *s)
{
    con_input((const uint8_t *)s, (unsigned)strlen(s));
}

static unsigned con_read(char *out, unsigned cap)
{
    unsigned n = 0, k;
    while ((k = con_tx_take((uint8_t *)out + n, 100)) && n + 100 < cap)
        n += k;
    out[n] = 0;
    return n;
}

static void test_console(void)
{
    char out[16384];
    uint32_t v;
    con_init(ring, sizeof(ring), cscr, sizeof(cscr));
    con_hello();
    con_read(out, sizeof(out));
    CHECK(strstr(out, "sb> ") != 0, "prompt");
    con_send("helq\x7fp\r");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "\b \b") && strstr(out, "next <key>") && strstr(out, "\r\n"), "help with backspace");
    con_send("info\r\n");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "product: starlte\r\n") && strstr(out, "battery-level: 77"), "info");
    CHECK(!strstr(out, "unknown command"), "CR LF is one line");
    con_send("log\r");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "STARBITE: loader start\r\n") && strstr(out, "without newline\r\n"), "log");
    con_send("catalog\r");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "ios16: iOS 16.5 (xnu)"), "catalog");
    con_send("next nosuch\r");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "no catalog entry") && con_action(&v) == FB_ACT_NONE, "next nosuch");
    con_send("bogus\x03");
    con_read(out, sizeof(out));
    CHECK(strstr(out, "^C"), "ctrl-c");
    con_send("next ios16\r");
    con_read(out, sizeof(out));
    CHECK(con_action(&v) == FB_ACT_REBOOT && v == INFORM_NORMAL && !strcmp(next_key, "ios16"), "next ios16");
    con_send("reboot download\r");
    CHECK(con_action(&v) == FB_ACT_REBOOT && v == INFORM_DOWNLOAD, "reboot download");
    con_send("menu\r");
    CHECK(con_action(&v) == FB_ACT_MENU, "menu");
    /* a full ring drops, never overruns */
    for (int i = 0; i < 20; ++i)
        con_send("help\r");
    CHECK(con_pending() <= sizeof(ring), "ring bounded");
}

int main(void)
{
    test_descriptors();
    test_ch9();
    test_images();
    test_protocol();
    test_console();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
