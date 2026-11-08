/* SPDX-License-Identifier: GPL-2.0-only
 * Loader fastboot (FB1) protocol subset and the USB console's shell (UO1). Pure code: no
 * registers and no fixed addresses (fastboot.h); tools/ios27/usb-fastboot/host_test.c runs it on the
 * Mac against stub fbp_* functions. docs/USB-FASTBOOT.md has the command list.
 *
 * Protocol (AOSP system/core/fastboot/README.md): the host sends one command per bulk OUT
 * transfer, the device answers with "INFO<text>" lines and one "OKAY<text>" / "FAIL<text>"
 * (64 bytes at most each), or "DATA<8 hex digits>" for download: (the host then sends that many
 * bytes) and upload (the device sends them, then OKAY).
 */
#include "fastboot.h"
#include "catalog.h"
#include "hw.h"

/* ---- text ---------------------------------------------------------------------------------- */

void tb_init(struct tb *t, char *buf, unsigned cap)
{
    t->b = buf;
    t->n = 0;
    t->cap = cap;
    if (cap)
        buf[0] = 0;
}

void tb_c(struct tb *t, char c)
{
    if (t->n + 1 < t->cap) {
        t->b[t->n++] = c;
        t->b[t->n] = 0;
    }
}

void tb_s(struct tb *t, const char *s)
{
    while (s && *s)
        tb_c(t, *s++);
}

void tb_u(struct tb *t, uint64_t v)
{
    char d[20];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < sizeof(d));
    while (k)
        tb_c(t, d[--k]);
}

void tb_hex(struct tb *t, uint64_t v, unsigned digits)
{
    tb_s(t, "0x");
    while (digits--)
        tb_c(t, "0123456789abcdef"[(v >> (4U * digits)) & 15U]);
}

void tb_reg(struct tb *t, const char *name, uint32_t v)
{
    tb_s(t, name);
    tb_c(t, ' ');
    tb_hex(t, v, 8);
    tb_c(t, '\n');
}

int fb_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

int fb_prefix(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

unsigned fb_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}

static const char *skip_sp(const char *s)
{
    while (*s == ' ')
        ++s;
    return s;
}

/* ---- images ------------------------------------------------------------------------------- */

static uint32_t le32(uintptr_t a)
{
    const volatile uint8_t *p = (const volatile uint8_t *)a;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(uintptr_t a)
{
    return (uint64_t)le32(a) | (uint64_t)le32(a + 4) << 32;
}

static int bytes_eq(uintptr_t a, const char *s, unsigned n)
{
    const volatile uint8_t *p = (const volatile uint8_t *)a;
    for (unsigned i = 0; i < n; ++i)
        if (p[i] != (uint8_t)s[i])
            return 0;
    return 1;
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Copy a NUL-terminated field of at most n bytes onto the end of the command line. */
static int cmdline_add(struct fb_image *o, unsigned *at, uintptr_t src, unsigned n)
{
    const volatile char *s = (const volatile char *)src;
    unsigned k = 0;
    for (; k < n && s[k]; ++k)
        if (*at < FB_CMDLINE_MAX)
            o->cmdline[(*at)++] = s[k];
    o->cmdline[*at] = 0;
    return k == n;   /* 1: the field was full (continues in the next one) */
}

int fb_image_parse(uintptr_t a, uint64_t size, struct fb_image *o, const char **why)
{
    o->kernel = o->ramdisk = o->dtb = 0;
    o->kernel_size = o->ramdisk_size = o->dtb_size = 0;
    o->cmdline[0] = 0;
    *why = 0;
    if (size < 8 || !bytes_eq(a, "ANDROID!", 8)) {
        o->header_version = 0xffffffffU;
        o->kernel = a;
        o->kernel_size = size;
        if (!size) {
            *why = "empty image";
            return 0;
        }
        return 1;
    }
    if (size < 4096) {
        *why = "boot image too short";
        return 0;
    }
    uint32_t hv = le32(a + 40);
    uint64_t page, ks, rs, koff, roff, end;
    unsigned at = 0;
    o->header_version = hv;
    if (hv == 3 || hv == 4) {
        page = 4096;
        ks = le32(a + 8);
        rs = le32(a + 12);
        (void)cmdline_add(o, &at, a + 44, 1536);
        koff = page;
        roff = koff + align_up(ks, page);
        end = roff + align_up(rs, page);
    } else if (hv <= 2) {
        page = le32(a + 36);
        if (page < 2048 || page > 16384 || (page & (page - 1))) {
            *why = "boot image: bad page size";
            return 0;
        }
        ks = le32(a + 8);
        rs = le32(a + 16);
        uint64_t ss = le32(a + 24);
        if (cmdline_add(o, &at, a + 64, 512))
            (void)cmdline_add(o, &at, a + 608, 1024);
        koff = page;
        roff = koff + align_up(ks, page);
        end = roff + align_up(rs, page) + align_up(ss, page);
        if (hv >= 1)
            end += align_up(le32(a + 1632), page);       /* recovery_dtbo */
        if (hv == 2) {
            uint64_t ds = le32(a + 1648);
            if (ds) {
                if (end > size || ds > size - end) {
                    *why = "boot image: DTB past the end";
                    return 0;
                }
                o->dtb = a + end;
                o->dtb_size = ds;
            }
            end += align_up(ds, page);
        }
    } else {
        *why = "boot image: unknown header version";
        return 0;
    }
    if (!ks) {
        *why = "boot image has no kernel";
        return 0;
    }
    if (koff + ks > size || (rs && roff + rs > size)) {
        *why = "boot image truncated";
        return 0;
    }
    (void)end;
    o->kernel = a + koff;
    o->kernel_size = ks;
    if (rs) {
        o->ramdisk = a + roff;
        o->ramdisk_size = rs;
    }
    return 1;
}

unsigned fb_kernel_kind(uintptr_t k, uint64_t size)
{
    if (size >= 0x2a8 && bytes_eq(k, "SWEETXNU", 8))
        return FB_K_XNU;
    if (size > 8192 && le64(k) == 0x2121494645554253ULL)        /* "SBUEFI!!" */
        return FB_K_BUNDLE;
    if (size >= 64 && le32(k + 56) == 0x644d5241U)              /* "ARMd" */
        return FB_K_ARM64;
    return FB_K_UNKNOWN;
}

int fb_has_fv(uintptr_t k, uint64_t size)
{
    uint64_t lim = size < (8U << 20) ? size : (8U << 20);
    for (uint64_t off = 0; off + 0x2c <= lim; off += 8)
        if (le32(k + off + 0x28) == 0x4856465fU)                /* "_FVH" */
            return 1;
    return 0;
}

unsigned fb_pick_method(const struct fb_image *img, unsigned forced)
{
    unsigned k = fb_kernel_kind(img->kernel, img->kernel_size);
    if (forced == M_XNU)
        return k == FB_K_XNU ? M_XNU : 0;
    if (forced)
        return (k == FB_K_ARM64 || (forced == M_UEFI && k == FB_K_BUNDLE)) ? forced : 0;
    if (k == FB_K_XNU)
        return M_XNU;
    if (k == FB_K_BUNDLE)
        return M_UEFI;
    if (k != FB_K_ARM64)
        return 0;
    if (img->dtb_size)
        return M_LINUX;
    if (fb_has_fv(img->kernel, img->kernel_size))
        return M_UEFI;
    if (img->ramdisk_size)
        return M_ANDROID;
    return M_BARE;
}

const char *fb_method_name(unsigned m)
{
    if (m == M_XNU)
        return "xnu";
    if (m == M_LINUX)
        return "linux";
    if (m == M_ANDROID)
        return "android";
    if (m == M_UEFI)
        return "uefi";
    if (m == M_BARE)
        return "bare";
    return "auto";
}

static unsigned method_by_name(const char *s)
{
    if (fb_streq(s, "xnu"))
        return M_XNU;
    if (fb_streq(s, "linux"))
        return M_LINUX;
    if (fb_streq(s, "android"))
        return M_ANDROID;
    if (fb_streq(s, "uefi"))
        return M_UEFI;
    if (fb_streq(s, "bare"))
        return M_BARE;
    return 0;
}

/* ---- the response queue --------------------------------------------------------------------- */

#define Q_MSG  1U
#define Q_INFO 2U   /* text[pos, end) as INFO lines */
#define Q_RAW  3U   /* text[pos, end) as raw bytes (upload) */
#define QN 6U

struct item {
    unsigned kind;
    const char *text;
    uint32_t pos, end;
    char msg[FB_RESP_MAX + 1];
    unsigned mlen;
};

static struct {
    uintptr_t dl;
    uint32_t dl_max, dl_size, want, got;
    char *scr, *stg;
    unsigned scr_cap, stg_cap, stg_len;
    unsigned forced;
    int act;
    uint32_t act_val;
    struct item q[QN];
    unsigned qh, qn;
} fb;

void fb_init(uintptr_t dl, uint32_t dl_max, char *scratch, unsigned scratch_cap, char *stage,
             unsigned stage_cap)
{
    fb.dl = dl;
    fb.dl_max = dl_max;
    fb.dl_size = fb.want = fb.got = 0;
    fb.scr = scratch;
    fb.scr_cap = scratch_cap;
    fb.stg = stage;
    fb.stg_cap = stage_cap;
    fb.stg_len = 0;
    fb.forced = 0;
    fb.act = FB_ACT_NONE;
    fb.act_val = 0;
    fb.qh = fb.qn = 0;
}

static struct item *q_push(unsigned kind)
{
    if (fb.qn >= QN)
        return 0;
    struct item *it = &fb.q[(fb.qh + fb.qn++) % QN];
    it->kind = kind;
    it->text = 0;
    it->pos = it->end = 0;
    it->mlen = 0;
    it->msg[0] = 0;
    return it;
}

/* "OKAY"/"FAIL"/"DATA"/"INFO" + text, cut to 64 bytes */
static void q_msg(const char *tag, const char *text)
{
    struct item *it = q_push(Q_MSG);
    if (!it)
        return;
    struct tb t;
    tb_init(&t, it->msg, sizeof(it->msg));
    tb_s(&t, tag);
    while (text && *text && t.n < FB_RESP_MAX)
        tb_c(&t, *text++);
    it->mlen = t.n;
}

static void q_text(unsigned kind, const char *text, uint32_t len)
{
    struct item *it = q_push(kind);
    if (!it)
        return;
    it->text = text;
    it->end = len;
}

static void okay(const char *s) { q_msg("OKAY", s); }
static void failr(const char *s) { q_msg("FAIL", s); }

static void q_data(uint32_t n)
{
    char b[12];
    struct tb t;
    tb_init(&t, b, sizeof(b));
    for (int i = 7; i >= 0; --i)
        tb_c(&t, "0123456789abcdef"[(n >> (4 * i)) & 15U]);
    q_msg("DATA", b);
}

static int tx_msg;

int fb_tx_was_msg(void)
{
    return tx_msg;
}

uint32_t fb_tx_next(uint8_t *out, uint32_t max)
{
    while (fb.qn) {
        struct item *it = &fb.q[fb.qh];
        tx_msg = it->kind != Q_RAW;
        if (it->kind == Q_MSG) {
            uint32_t n = it->mlen < max ? it->mlen : max;
            for (uint32_t i = 0; i < n; ++i)
                out[i] = (uint8_t)it->msg[i];
            fb.qh = (fb.qh + 1) % QN;
            --fb.qn;
            return n;
        }
        if (it->pos >= it->end) {
            fb.qh = (fb.qh + 1) % QN;
            --fb.qn;
            continue;
        }
        if (it->kind == Q_RAW) {
            uint32_t n = it->end - it->pos;
            if (n > max)
                n = max;
            for (uint32_t i = 0; i < n; ++i)
                out[i] = (uint8_t)it->text[it->pos + i];
            it->pos += n;
            return n;
        }
        /* Q_INFO: one line, at most 60 characters (a longer one goes on in the next) */
        uint32_t n = 4;
        out[0] = 'I';
        out[1] = 'N';
        out[2] = 'F';
        out[3] = 'O';
        while (it->pos < it->end && n < FB_RESP_MAX && n < max) {
            char c = it->text[it->pos++];
            if (c == '\n')
                break;
            if (c == '\r')
                continue;
            out[n++] = (uint8_t)((c < ' ' || c > '~') ? ' ' : c);
        }
        return n;
    }
    return 0;
}

int fb_tx_idle(void)
{
    return fb.qn == 0;
}

int fb_action(uint32_t *value)
{
    if (fb.qn)
        return FB_ACT_NONE;
    int a = fb.act;
    *value = fb.act_val;
    fb.act = FB_ACT_NONE;
    return a;
}

void fb_usb_reset(void)
{
    if (fb.want) {
        fb.want = fb.got = 0;
        fb.dl_size = 0;
    }
    fb.qh = fb.qn = 0;
}

uint32_t fb_dl_size(void) { return fb.dl_size; }
unsigned fb_forced(void) { return fb.forced; }
uint32_t fb_rx_want(void) { return fb.want > fb.got ? fb.want - fb.got : 0; }
uintptr_t fb_rx_ptr(void) { return fb.dl + fb.got; }

void fb_rx_data(uint32_t n)
{
    if (!fb.want)
        return;
    fb.got += n;
    if (fb.got >= fb.want) {
        fb.dl_size = fb.want;
        fb.want = fb.got = 0;
        okay("");
        fbp_status("DOWNLOAD DONE");
    }
}

/* ---- variables ------------------------------------------------------------------------------ */

/* every variable `getvar all` lists, NUL separated */
static const char var_names[] =
    "version\0version-bootloader\0product\0serialno\0secure\0unlocked\0is-userspace\0"
    "max-download-size\0battery-voltage\0battery-level\0battery-soc-ok\0usb-speed\0boot-as\0"
    "downloaded\0catalog-count\0";

static int var_get(const char *name, struct tb *t)
{
    unsigned mv = 0, pct = 0;
    if (fb_streq(name, "version"))
        tb_s(t, "0.4");
    else if (fb_streq(name, "version-bootloader"))
        tb_s(t, "starbite-fb1");
    else if (fb_streq(name, "product"))
        tb_s(t, "starlte");
    else if (fb_streq(name, "serialno"))
        tb_s(t, fbp_serial());
    else if (fb_streq(name, "secure"))
        tb_s(t, "no");
    else if (fb_streq(name, "unlocked"))
        tb_s(t, "yes");
    else if (fb_streq(name, "is-userspace"))
        tb_s(t, "no");
    else if (fb_streq(name, "max-download-size"))
        tb_hex(t, fb.dl_max, 8);
    else if (fb_streq(name, "battery-voltage")) {
        if (!fbp_battery(&mv, &pct))
            return 0;
        tb_u(t, mv);
    } else if (fb_streq(name, "battery-level")) {
        if (!fbp_battery(&mv, &pct))
            return 0;
        tb_u(t, pct);
    } else if (fb_streq(name, "battery-soc-ok"))
        tb_s(t, !fbp_battery(&mv, &pct) || pct >= 5 ? "yes" : "no");
    else if (fb_streq(name, "usb-speed"))
        tb_s(t, fbp_speed());
    else if (fb_streq(name, "boot-as"))
        tb_s(t, fb_method_name(fb.forced));
    else if (fb_streq(name, "downloaded"))
        tb_u(t, fb.dl_size);
    else if (fb_streq(name, "catalog-count"))
        tb_u(t, fbp_cat_count());
    else if (fb_prefix(name, "has-slot:"))
        tb_s(t, "no");
    else if (fb_prefix(name, "catalog:")) {
        unsigned i = 0;
        const char *s = name + 8, *key, *nm;
        unsigned m;
        int ready;
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9')
            i = i * 10U + (unsigned)(*s++ - '0');
        if (*s || !fbp_cat_entry(i, &key, &nm, &m, &ready))
            return 0;
        tb_s(t, key);
        tb_c(t, ' ');
        tb_s(t, fb_method_name(m));
        tb_s(t, ready ? " ready" : " missing");
    } else
        return 0;
    return 1;
}

/* the catalog as text: "key  name (method)" lines */
static void catalog_text(struct tb *t)
{
    unsigned n = fbp_cat_count();
    for (unsigned i = 0; i < n; ++i) {
        const char *key, *nm;
        unsigned m;
        int ready;
        if (!fbp_cat_entry(i, &key, &nm, &m, &ready))
            continue;
        tb_s(t, key);
        tb_s(t, ": ");
        tb_s(t, nm);
        tb_s(t, " (");
        tb_s(t, fb_method_name(m));
        tb_s(t, ready ? ")" : ", missing files)");
        tb_c(t, '\n');
    }
}

static void getvar(const char *name)
{
    struct tb t;
    char v[FB_RESP_MAX + 1];
    if (fb_streq(name, "all")) {
        tb_init(&t, fb.scr, fb.scr_cap);
        for (const char *n = var_names; *n; n += fb_strlen(n) + 1) {
            struct tb w;
            tb_init(&w, v, sizeof(v));
            if (!var_get(n, &w))
                continue;
            tb_s(&t, n);
            tb_s(&t, ": ");
            tb_s(&t, v);
            tb_c(&t, '\n');
        }
        unsigned cn = fbp_cat_count();
        for (unsigned i = 0; i < cn; ++i) {
            char nm[24];
            struct tb w, k;
            tb_init(&k, nm, sizeof(nm));
            tb_s(&k, "catalog:");
            tb_u(&k, i);
            tb_init(&w, v, sizeof(v));
            if (!var_get(nm, &w))
                continue;
            tb_s(&t, nm);
            tb_s(&t, ": ");
            tb_s(&t, v);
            tb_c(&t, '\n');
        }
        q_text(Q_INFO, fb.scr, t.n);
        okay("");
        return;
    }
    if (fb_streq(name, "catalog")) {
        tb_init(&t, fb.scr, fb.scr_cap);
        catalog_text(&t);
        q_text(Q_INFO, fb.scr, t.n);
        tb_init(&t, v, sizeof(v));
        tb_u(&t, fbp_cat_count());
        tb_s(&t, " entries");
        okay(v);
        return;
    }
    tb_init(&t, v, sizeof(v));
    if (var_get(name, &t))
        okay(v);
    else
        failr("unknown variable");
}

/* ---- commands ------------------------------------------------------------------------------- */

static int hexval(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    unsigned n = 0;
    for (; *s; ++s, ++n) {
        unsigned d;
        if (*s >= '0' && *s <= '9')
            d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            d = (unsigned)(*s - 'A' + 10);
        else
            return 0;
        if (n >= 8)
            return 0;
        v = v << 4 | d;
    }
    *out = v;
    return n > 0;
}

static void do_boot(void)
{
    static struct fb_image im;
    struct fb_image *img = &im;
    const char *why = 0;
    char v[FB_RESP_MAX + 1];
    struct tb t;
    if (!fb.dl_size) {
        failr("nothing downloaded");
        return;
    }
    if (!fb_image_parse(fb.dl, fb.dl_size, img, &why)) {
        failr(why);
        return;
    }
    unsigned m = fb_pick_method(img, fb.forced);
    if (!m) {
        failr(fb.forced ? "image does not fit the forced method" : "unknown image (not XNU, arm64 or UEFI)");
        return;
    }
    if (fb_kernel_kind(img->kernel, img->kernel_size) == FB_K_BUNDLE) {
        uint64_t bs = le64(img->kernel + 8);
        img->kernel += 4096;
        img->kernel_size -= 4096;
        if (bs && bs < img->kernel_size)
            img->kernel_size = bs;
    }
    tb_init(&t, v, sizeof(v));
    tb_s(&t, "booting as ");
    tb_s(&t, fb_method_name(m));
    tb_s(&t, ", kernel ");
    tb_u(&t, img->kernel_size);
    if (img->ramdisk_size) {
        tb_s(&t, ", ramdisk ");
        tb_u(&t, img->ramdisk_size);
    }
    if (img->dtb_size) {
        tb_s(&t, ", dtb ");
        tb_u(&t, img->dtb_size);
    }
    fbp_status(v);
    if (!fbp_boot_prepare(m, img, &why)) {
        failr(why ? why : "start failed");
        return;
    }
    q_msg("INFO", v);
    okay("");
    fb.act = FB_ACT_BOOT;
}

static const char oem_help[] =
    "Starbite loader fastboot (FB1)\n"
    "fastboot boot <file>          XNU payload, arm64 Image, UEFI\n"
    "fastboot boot <Image> <initrd> [--dtb x --header-version 2]\n"
    "fastboot getvar all | catalog | battery-level\n"
    "fastboot oem log [pmsg|dmesg|loader|usb]  (then get_staged)\n"
    "fastboot oem reboot-to <catalog key|fastboot>\n"
    "fastboot oem boot-next <key|fastboot>  (next reset, no reboot)\n"
    "fastboot oem boot-as <auto|xnu|linux|android|uefi|bare>\n"
    "fastboot oem catalog | oem usb | continue (back to menu)\n"
    "fastboot reboot [bootloader|recovery|download]\n";

static void oem(const char *a)
{
    a = skip_sp(a);
    if (fb_streq(a, "help") || !*a) {
        q_text(Q_INFO, oem_help, sizeof(oem_help) - 1);
        okay("");
    } else if (fb_prefix(a, "log") && (a[3] == 0 || a[3] == ' ')) {
        const char *w = skip_sp(a + 3);
        unsigned n = fbp_log(*w ? w : "pmsg", fb.stg, fb.stg_cap);
        if (!n) {
            fb.stg_len = 0;
            failr("no such log (pmsg, dmesg, loader, usb)");
            return;
        }
        fb.stg_len = n;
        q_text(Q_INFO, fb.stg, n);
        okay("staged: fastboot get_staged <file>");
    } else if (fb_prefix(a, "reboot-to ")) {
        const char *k = skip_sp(a + 10);
        if (!fb_streq(k, "fastboot") && fbp_cat_find(k) < 0) {
            failr("no catalog entry with that key");
            return;
        }
        fbp_set_next(k);
        okay("");
        fb.act = FB_ACT_REBOOT;
        fb.act_val = INFORM_NORMAL;
    } else if (fb_prefix(a, "boot-next ")) {
        /* the mailbox only: the reboot after the next boot lands there (e.g. back in fastboot) */
        const char *k = skip_sp(a + 10);
        if (!fb_streq(k, "fastboot") && fbp_cat_find(k) < 0) {
            failr("no catalog entry with that key");
            return;
        }
        fbp_set_next(k);
        okay("");
    } else if (fb_prefix(a, "boot-as ")) {
        const char *m = skip_sp(a + 8);
        if (fb_streq(m, "auto")) {
            fb.forced = 0;
        } else {
            unsigned v = method_by_name(m);
            if (!v) {
                failr("auto, xnu, linux, android, uefi or bare");
                return;
            }
            fb.forced = v;
        }
        okay(fb_method_name(fb.forced));
    } else if (fb_streq(a, "catalog")) {
        struct tb t;
        tb_init(&t, fb.scr, fb.scr_cap);
        catalog_text(&t);
        q_text(Q_INFO, fb.scr, t.n);
        okay("");
    } else if (fb_streq(a, "usb")) {
        unsigned n = fbp_log("usb", fb.scr, fb.scr_cap);
        q_text(Q_INFO, fb.scr, n);
        okay("");
    } else if (fb_streq(a, "menu")) {
        okay("");
        fb.act = FB_ACT_MENU;
    } else {
        failr("unknown oem command (fastboot oem help)");
    }
}

void fb_command(const uint8_t *cmd, unsigned len)
{
    char c[FB_CMD_MAX + 1];
    unsigned n = 0;
    for (; n < len && n < FB_CMD_MAX && cmd[n]; ++n)
        c[n] = (char)cmd[n];
    c[n] = 0;
    if (!n)
        return;                       /* a zero-length packet after download data */
    if (fb.want) {
        failr("busy downloading");
        return;
    }
    {
        char s[40];
        struct tb t;
        tb_init(&t, s, sizeof(s));
        for (unsigned i = 0; i < n && i < 36; ++i)
            tb_c(&t, (c[i] >= 'a' && c[i] <= 'z') ? (char)(c[i] - 32) : c[i]);
        fbp_status(s);
    }
    if (fb_prefix(c, "getvar:")) {
        getvar(c + 7);
    } else if (fb_prefix(c, "download:")) {
        uint32_t size;
        if (!hexval(c + 9, &size) || !size) {
            failr("bad size");
            return;
        }
        if (size > fb.dl_max) {
            failr("data too large");
            return;
        }
        fb.dl_size = 0;
        fb.want = size;
        fb.got = 0;
        q_data(size);
    } else if (fb_streq(c, "upload")) {
        if (!fb.stg_len) {
            failr("nothing staged (fastboot oem log first)");
            return;
        }
        q_data(fb.stg_len);
        q_text(Q_RAW, fb.stg, fb.stg_len);
        okay("");
    } else if (fb_streq(c, "boot")) {
        do_boot();
    } else if (fb_streq(c, "continue")) {
        okay("");
        fb.act = FB_ACT_MENU;
    } else if (fb_streq(c, "reboot")) {
        okay("");
        fb.act = FB_ACT_REBOOT;
        fb.act_val = INFORM_NORMAL;
    } else if (fb_streq(c, "reboot-bootloader") || fb_streq(c, "reboot-fastboot")) {
        fbp_set_next("fastboot");
        okay("");
        fb.act = FB_ACT_REBOOT;
        fb.act_val = INFORM_NORMAL;
    } else if (fb_streq(c, "reboot-recovery")) {
        okay("");
        fb.act = FB_ACT_REBOOT;
        fb.act_val = INFORM_RECOVERY;
    } else if (fb_streq(c, "reboot-download")) {
        okay("");
        fb.act = FB_ACT_REBOOT;
        fb.act_val = INFORM_DOWNLOAD;
    } else if (fb_prefix(c, "oem")) {
        oem(c + 3);
    } else if (fb_prefix(c, "flash:") || fb_prefix(c, "erase:") || fb_prefix(c, "flashing")) {
        failr("no flashing in FB1: fastboot boot runs from RAM");
    } else {
        failr("unknown command");
    }
}

/* ---- console --------------------------------------------------------------------------------- */

static struct {
    char *ring;
    unsigned cap, head, len, drops;
    char *scr;
    unsigned scr_cap;
    char line[128];
    unsigned ln;
    int act;
    uint32_t val;
} con;

void con_init(char *ring, unsigned cap, char *scratch, unsigned scratch_cap)
{
    con.ring = ring;
    con.cap = cap;
    con.head = con.len = con.drops = 0;
    con.scr = scratch;
    con.scr_cap = scratch_cap;
    con.ln = 0;
    con.act = FB_ACT_NONE;
}

static void con_putc(char c)
{
    if (con.len < con.cap) {
        con.ring[(con.head + con.len) % con.cap] = c;
        ++con.len;
    } else {
        ++con.drops;
    }
}

static void con_puts(const char *s)
{
    for (; s && *s; ++s) {
        if (*s == '\n')
            con_putc('\r');
        con_putc(*s);
    }
}

static void con_putn(const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (s[i] == '\n')
            con_putc('\r');
        if (s[i])
            con_putc(s[i]);
    }
}

unsigned con_tx_take(uint8_t *out, unsigned cap)
{
    unsigned n = con.len < cap ? con.len : cap;
    for (unsigned i = 0; i < n; ++i)
        out[i] = (uint8_t)con.ring[(con.head + i) % con.cap];
    con.head = (con.head + n) % con.cap;
    con.len -= n;
    return n;
}

unsigned con_pending(void)
{
    return con.len;
}

int con_action(uint32_t *value)
{
    int a = con.act;
    *value = con.val;
    con.act = FB_ACT_NONE;
    return a;
}

static void con_prompt(void)
{
    con_puts("sb> ");
}

void con_hello(void)
{
    con_puts("\nStarbite loader console (fastboot mode). Type help.\n");
    con_prompt();
}

static const char con_help[] =
    "help                  this list\n"
    "info                  device, battery, USB, download buffer\n"
    "log [pmsg|dmesg|loader|usb]  a log (pmsg: the ramoops ring of the last boot)\n"
    "catalog               the boot catalog\n"
    "next <key>            boot <key> next (boot-next mailbox) and reboot\n"
    "reboot [recovery|download|fastboot]\n"
    "menu                  leave fastboot mode: back to the boot menu\n";

static void con_run(char *l)
{
    struct tb t;
    char *a = l;
    while (*a == ' ')
        ++a;
    if (!*a)
        return;
    if (fb_streq(a, "help") || fb_streq(a, "?")) {
        con_puts(con_help);
    } else if (fb_streq(a, "info")) {
        char v[FB_RESP_MAX + 1];
        tb_init(&t, con.scr, con.scr_cap);
        for (const char *n = var_names; *n; n += fb_strlen(n) + 1) {
            struct tb w;
            tb_init(&w, v, sizeof(v));
            if (!var_get(n, &w))
                continue;
            tb_s(&t, n);
            tb_s(&t, ": ");
            tb_s(&t, v);
            tb_c(&t, '\n');
        }
        con_putn(con.scr, t.n);
    } else if (fb_streq(a, "log") || fb_prefix(a, "log ")) {
        const char *w = skip_sp(a + 3);
        unsigned n = fbp_log(*w ? w : "pmsg", con.scr, con.scr_cap);
        if (!n)
            con_puts("no such log (pmsg, dmesg, loader, usb)\n");
        con_putn(con.scr, n);
        if (n && con.scr[n - 1] != '\n')
            con_puts("\n");
    } else if (fb_streq(a, "catalog") || fb_streq(a, "cat")) {
        tb_init(&t, con.scr, con.scr_cap);
        catalog_text(&t);
        con_putn(con.scr, t.n);
    } else if (fb_prefix(a, "next ")) {
        const char *k = skip_sp(a + 5);
        if (!fb_streq(k, "fastboot") && fbp_cat_find(k) < 0) {
            con_puts("no catalog entry with that key\n");
        } else {
            fbp_set_next(k);
            con_puts("rebooting\n");
            con.act = FB_ACT_REBOOT;
            con.val = INFORM_NORMAL;
        }
    } else if (fb_streq(a, "reboot") || fb_prefix(a, "reboot ")) {
        const char *w = skip_sp(a + 6);
        con.val = INFORM_NORMAL;
        if (fb_streq(w, "recovery"))
            con.val = INFORM_RECOVERY;
        else if (fb_streq(w, "download"))
            con.val = INFORM_DOWNLOAD;
        else if (fb_streq(w, "fastboot") || fb_streq(w, "bootloader"))
            fbp_set_next("fastboot");
        else if (*w) {
            con_puts("reboot [recovery|download|fastboot]\n");
            return;
        }
        con_puts("rebooting\n");
        con.act = FB_ACT_REBOOT;
    } else if (fb_streq(a, "menu") || fb_streq(a, "exit")) {
        con_puts("back to the boot menu\n");
        con.act = FB_ACT_MENU;
    } else {
        con_puts("unknown command (help)\n");
    }
}

void con_input(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        char c = (char)b[i];
        if (c == '\r' || c == '\n') {
            if (c == '\n' && i && b[i - 1] == '\r')
                continue;
            con_puts("\n");
            con.line[con.ln] = 0;
            con_run(con.line);
            con.ln = 0;
            con_prompt();
        } else if (c == 0x7f || c == 0x08) {
            if (con.ln) {
                --con.ln;
                con_puts("\b \b");
            }
        } else if (c == 0x03) {
            con.ln = 0;
            con_puts("^C\n");
            con_prompt();
        } else if (c >= ' ' && c <= '~' && con.ln + 1 < sizeof(con.line)) {
            con.line[con.ln++] = c;
            con_putc(c);
        }
    }
}
