/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite POST, the power-on self test (docs/STARBITE-RECOVERY.md section 4).
 *
 * Quick (every boot, after the Bixby window): RAM pattern on 1 MiB of the free XNU window, the
 * generic timer, ACPM + the S2MPS18, UFS up, the LU0 GPT, the OS store header and directory, the
 * catalog (its CRC: catalog.c falls back to the built-in one when it fails), the battery (MAX77705
 * fuel gauge, read only), the TMU temperatures, the touch controller, the microSD card detect.
 * Full (the recovery menu): 64 MiB of RAM with four patterns, the timer measured against the PMIC
 * RTC, the battery's voltage and current, every TMU probe; the rest as quick.
 *
 * Every check is bounded: the RAM pattern by its size, the timer by a loop count, ACPM by
 * acpm_send's own 100 ms waits, I2C by a deadline per message and per check (below), the TMU and
 * the card detect are single register reads. UFS has ufs.c's own limits (2 s per command, at most
 * nine commands: 18 s on a dead device, what the boot menu pays today without POST); touch has
 * menu.c's (150 ms power-up plus ten bounded I2C tries), and the boot menu brings it up anyway.
 * Only RAM and timer failures are fatal (the loader then shows Starbite Recovery instead of the
 * menu); any other failure is a red line, a 1.5 s pause and an entry in the logs.
 *
 * Results: post_last() (the menu can show them), the log page slots 200..212 (readable from
 * Linux through /dev/mem: 0x91fff000 + 8 * slot) and one line in the pmsg ring.
 * RAM used: 0x9E000000 (the panel rows POST draws over, put back after), 0x8C000000 = STAGE (the RAM
 * pattern: 1 MiB quick, 64 MiB full). No pointer tables (no absolute relocations).
 */
#include "post.h"
#include "fbtext.h"
#include "hw.h"
#include "ufs.h"
#include "store.h"
#include "catalog.h"
#include "recovery.h"

#define POST_ROWS_SAVE 0x9E000000UL
/* STAGE (xnu_boot.c, 0x8c000000..0x91000000): the loader's own download buffer, unused until after
 * POST. NOT 0xA0000000: that is S-Boot's ECT (stock DT `ect { parameter_address = <0xa0000000>;
 * parameter_size = <0x32000>; }`), which the Android kernel's ACPM init reads; the pattern test
 * there made LineageOS/GSI panic at 1.1 s in exynos_acpm_binary_update (2026-11-05). */
#define POST_RAM_BASE  0x8C000000UL
#define POST_RAM_QUICK (1UL << 20)
#define POST_RAM_FULL  (64UL << 20)
#define POST_LOG       ((volatile uint64_t *)0x91fff000UL)
#define POST_LOG_SLOT  200U
#define POST_HOLD_MS   1500U

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_MUTED 0xffb1bad3U
#define C_GOLD  0xffffd43bU
#define C_RED   0xffff4d4dU
#define C_GREEN 0xff3ddc84U

static struct post_report rep;
static int rep_valid;

const struct post_report *post_last(void)
{
    return rep_valid ? &rep : 0;
}

unsigned post_ok_count(const struct post_report *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < r->n && i < POST_MAX; ++i)
        n += r->c[i].state == POST_OK;
    return n;
}

/* ---- time ----------------------------------------------------------------------------------- */

static uint64_t p_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t p_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 26000000U;
}

static uint64_t p_us(uint64_t t0)
{
    return (p_ticks() - t0) * 1000000U / p_freq();
}

/* ---- text ----------------------------------------------------------------------------------- */

struct pl { char b[STAR_FB_COLS + 1]; unsigned n; };

static void pl_init(struct pl *l)
{
    l->n = 0;
    l->b[0] = 0;
}

static void pl_s(struct pl *l, const char *s)
{
    while (s && *s && l->n < STAR_FB_COLS)
        l->b[l->n++] = *s++;
    l->b[l->n] = 0;
}

static void pl_u(struct pl *l, uint64_t v)
{
    char d[20];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < sizeof(d));
    while (k && l->n < STAR_FB_COLS)
        l->b[l->n++] = d[--k];
    l->b[l->n] = 0;
}

static void pl_i(struct pl *l, int64_t v)
{
    if (v < 0) {
        pl_s(l, "-");
        v = -v;
    }
    pl_u(l, (uint64_t)v);
}

static void pl_hex(struct pl *l, uint64_t v, unsigned digits)
{
    pl_s(l, "0X");
    while (digits-- && l->n < STAR_FB_COLS) {
        unsigned d = (unsigned)(v >> (4 * digits)) & 15U;
        l->b[l->n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    l->b[l->n] = 0;
}

static void pl_pad(struct pl *l, unsigned col)
{
    while (l->n < col && l->n < STAR_FB_COLS)
        l->b[l->n++] = ' ';
    l->b[l->n] = 0;
}

static void str_copy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    while (s && s[k] && k + 1 < cap) {
        d[k] = s[k];
        ++k;
    }
    d[k] = 0;
}

/* ---- the run -------------------------------------------------------------------------------- */

static unsigned p_mode, p_row0;
static int *p_bixby;

static const char *check_name(unsigned id)
{
    switch (id) {
    case POST_RAM:     return "RAM";
    case POST_TIMER:   return "TIMER";
    case POST_PMIC:    return "PMIC";
    case POST_UFS:     return "UFS";
    case POST_GPT:     return "GPT";
    case POST_STORE:   return "STORE";
    case POST_CATALOG: return "CATALOG";
    case POST_BATTERY: return "BATTERY";
    case POST_TEMP:    return "TEMP";
    case POST_TOUCH:   return "TOUCH";
    default:           return "MICROSD";
    }
}

/* Column layout: mark, name (8), detail (20), time "9999MS" right aligned (38 in all). */
static void draw_check(unsigned id)
{
    const struct post_check *c = &rep.c[id];
    struct pl l;
    pl_init(&l);
    pl_s(&l, c->state == POST_OK ? FBT_CHECK : c->state == POST_FAIL ? FBT_CROSS : c->state == POST_WARN ? "!" : "-");
    pl_s(&l, " ");
    pl_s(&l, c->name);
    pl_pad(&l, 11);
    pl_s(&l, c->detail);
    if (c->state != POST_SKIP) {
        uint64_t ms = (c->us + 500U) / 1000U;
        unsigned digits = 1;
        for (uint64_t v = ms; v >= 10; v /= 10)
            ++digits;
        if (ms > 9999)
            ms = 9999, digits = 4;
        pl_pad(&l, STAR_FB_COLS - 2 - digits);
        pl_u(&l, ms);
        pl_s(&l, "MS");
    }
    uint32_t fg = c->state == POST_OK ? C_WHITE : c->state == POST_FAIL ? C_RED : c->state == POST_WARN ? C_GOLD : C_MUTED;
    star_fb_text_c(p_row0 + 1 + id, l.b, fg, C_BLACK);
}

static void check_begin(unsigned id)
{
    struct post_check *c = &rep.c[id];
    str_copy(c->name, sizeof(c->name), check_name(id));
    c->state = POST_SKIP;
    c->fatal = 0;
    c->value = 0;
    c->us = 0;
    str_copy(c->detail, sizeof(c->detail), "...");
    draw_check(id);
}

/* The check's result; over cap_ms an OK becomes a warning ("SLOW"). */
static void check_end(unsigned id, unsigned state, int32_t value, const char *detail, uint64_t t0, unsigned cap_ms)
{
    struct post_check *c = &rep.c[id];
    uint64_t us = p_us(t0);
    c->us = us > 0xffffffffU ? 0xffffffffU : (uint32_t)us;
    c->state = (uint8_t)state;
    c->value = value;
    str_copy(c->detail, sizeof(c->detail), detail);
    if (state == POST_OK && cap_ms && us > (uint64_t)cap_ms * 1000U) {
        c->state = POST_WARN;
        struct pl l;
        pl_init(&l);
        pl_s(&l, detail);
        pl_s(&l, " SLOW");
        str_copy(c->detail, sizeof(c->detail), l.b);
    }
    if (c->state == POST_FAIL)
        ++rep.fails;
    else if (c->state == POST_WARN)
        ++rep.warns;
    draw_check(id);
    if (p_bixby && !*p_bixby && rec_bixby_now())
        *p_bixby = 1;
}

/* ---- RAM -------------------------------------------------------------------------------------- */

/* Word i's value in pattern k: its address, the address inverted, 0101..., 1010... */
static inline uint64_t ram_word(uintptr_t base, uint64_t i, unsigned k)
{
    uint64_t a = base + 8U * i;
    return k == 0 ? a : k == 1 ? ~a : k == 2 ? 0x5555555555555555ULL : 0xaaaaaaaaaaaaaaaaULL;
}

/* Pattern pass k over n bytes at base (non-cacheable: every access reaches DRAM): write all, then
 * read all, eight loads in flight at a time. Mismatching words. */
static uint64_t ram_pass(uintptr_t base, uint64_t n, unsigned k)
{
    volatile uint64_t *p = (volatile uint64_t *)base;
    uint64_t words = n / 8U & ~7ULL, bad = 0;
    for (uint64_t i = 0; i < words; ++i)
        p[i] = ram_word(base, i, k);
    __asm__ volatile("dsb sy" : : : "memory");
    for (uint64_t i = 0; i < words; i += 8) {
        uint64_t v[8];
        for (unsigned j = 0; j < 8; ++j)
            v[j] = p[i + j];
        for (unsigned j = 0; j < 8; ++j)
            bad += v[j] != ram_word(base, i + j, k);
    }
    return bad;
}

static void check_ram(void)
{
    uint64_t t0 = p_ticks();
    uint64_t n = p_mode == POST_FULL ? POST_RAM_FULL : POST_RAM_QUICK, bad = 0;
    unsigned passes = p_mode == POST_FULL ? 4U : 2U;
    check_begin(POST_RAM);
    for (unsigned k = 0; k < passes; ++k)
        bad += ram_pass(POST_RAM_BASE, n, k);
    struct pl l;
    pl_init(&l);
    pl_u(&l, n >> 20);
    pl_s(&l, " MIB ");
    if (bad) {
        pl_u(&l, bad);
        pl_s(&l, " BAD WORDS");
    } else {
        pl_u(&l, passes);
        pl_s(&l, " PATTERNS OK");
    }
    rep.c[POST_RAM].fatal = bad != 0;
    check_end(POST_RAM, bad ? POST_FAIL : POST_OK, (int32_t)(bad > 0x7fffffff ? 0x7fffffff : bad), l.b, t0,
              p_mode == POST_FULL ? 8000U : 60U);
}

/* ---- timer ------------------------------------------------------------------------------------ */

static void check_timer(void)
{
    uint64_t t0 = p_ticks(), f;
    check_begin(POST_TIMER);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    uint64_t a = p_ticks(), b = a;
    for (unsigned i = 0; i < 100000 && b == a; ++i)
        b = p_ticks();
    struct pl l;
    pl_init(&l);
    if (b == a) {
        rep.c[POST_TIMER].fatal = 1;
        check_end(POST_TIMER, POST_FAIL, 0, "COUNTER STOPPED", t0, 0);
        return;
    }
    uint64_t hz = p_freq();
    unsigned state = POST_OK;
    if (p_mode == POST_FULL) {
        /* Ticks between two PMIC RTC second edges (each read about 3 ms: +-0.3 %). */
        uint64_t e0 = 0, e = 0, ta = 0, tb = 0;
        int ok = em_rtc_epoch(&e0);
        for (unsigned i = 0; ok && i < 600 && !ta; ++i)
            if (em_rtc_epoch(&e) && e != e0)
                ta = p_ticks(), e0 = e;
        for (unsigned i = 0; ta && i < 600 && !tb; ++i)
            if (em_rtc_epoch(&e) && e != e0)
                tb = p_ticks();
        if (tb) {
            uint64_t m = tb - ta;   /* ticks per RTC second */
            pl_u(&l, m / 1000000U);
            pl_s(&l, ".");
            pl_u(&l, m / 100000U % 10U);
            pl_s(&l, " MHZ BY RTC");
            if (m * 100U < hz * 97U || m * 100U > hz * 103U)
                state = POST_WARN;
            check_end(POST_TIMER, state, (int32_t)(m / 100000U), l.b, t0, 3000U);
            return;
        }
        check_end(POST_TIMER, POST_WARN, (int32_t)(hz / 100000U), "RTC UNREAD", t0, 0);
        return;
    }
    pl_u(&l, hz / 1000000U);
    pl_s(&l, ".");
    pl_u(&l, hz / 100000U % 10U);
    pl_s(&l, f ? " MHZ" : " MHZ (DEFAULT)");
    check_end(POST_TIMER, state, (int32_t)(hz / 100000U), l.b, t0, 20U);
}

/* ---- PMIC through ACPM ------------------------------------------------------------------------ */

static void check_pmic(void)
{
    uint64_t t0 = p_ticks();
    char build[25];
    uint8_t id = 0;
    check_begin(POST_PMIC);
    if (!hw_acpm_build(build)) {
        check_end(POST_PMIC, POST_FAIL, 0, "NO ACPM PMIC CHANNEL", t0, 0);
        return;
    }
    if (!hw_pmic_read(0, 0x00, &id)) {
        check_end(POST_PMIC, POST_FAIL, 0, "ACPM: NO ANSWER", t0, 0);
        return;
    }
    struct pl l;
    pl_init(&l);
    pl_s(&l, "S2MPS18 ID ");
    pl_hex(&l, id, 2);
    check_end(POST_PMIC, POST_OK, id, l.b, t0, 250U);
}

/* ---- UFS, GPT, store, catalog ----------------------------------------------------------------- */

static void check_storage(void)
{
    struct pl l;
    uint64_t t0 = p_ticks();
    check_begin(POST_UFS);
    int up = ufs_up();
    if (!up) {
        const char *w = ufs_why == 1 ? "HOST NOT READY" : ufs_why == 2 ? "HOST BUSY" : ufs_why == 3 ? "LIST REFUSED" : "DEVICE NO ANSWER";
        check_end(POST_UFS, POST_FAIL, ufs_why, w, t0, 0);
    } else {
        check_end(POST_UFS, POST_OK, 0, "READY", t0, 2000U);
    }

    t0 = p_ticks();
    check_begin(POST_GPT);
    uint64_t first, last;
    int n = up ? gpt_load() : 0;
    pl_init(&l);
    if (!up) {
        check_end(POST_GPT, POST_SKIP, 0, "NO UFS", t0, 0);
    } else if (n <= 0) {
        check_end(POST_GPT, POST_FAIL, n, "UNREADABLE", t0, 0);
    } else {
        int ud = gpt_find("USERDATA", &first, &last);
        pl_u(&l, (uint64_t)n);
        pl_s(&l, ud ? " PARTITIONS" : " NO USERDATA");
        check_end(POST_GPT, ud ? POST_OK : POST_FAIL, n, l.b, t0, 1000U);
    }

    t0 = p_ticks();
    check_begin(POST_STORE);
    int cnt = up ? store_open(STORE_UFS) : 0, ver = up ? store_version(STORE_UFS) : 0;
    pl_init(&l);
    if (!up) {
        check_end(POST_STORE, POST_SKIP, 0, "NO UFS", t0, 0);
    } else if (!ver) {
        check_end(POST_STORE, POST_FAIL, 0, "NO STORE OR BAD CRC", t0, 0);
    } else {
        pl_s(&l, "V");
        pl_u(&l, (uint64_t)ver);
        pl_s(&l, " ");
        pl_u(&l, (uint64_t)cnt);
        pl_s(&l, " ENTRIES");
        check_end(POST_STORE, cnt ? POST_OK : POST_WARN, cnt, l.b, t0, 1000U);
    }

    t0 = p_ticks();
    check_begin(POST_CATALOG);
    int ce = cat_load();
    struct store_ent e;
    pl_init(&l);
    pl_u(&l, (uint64_t)ce);
    if (!cat_builtin()) {
        pl_s(&l, " ENTRIES CRC OK");
        check_end(POST_CATALOG, POST_OK, ce, l.b, t0, 1000U);
    } else if (up && ver && store_find("catalog", &e)) {
        check_end(POST_CATALOG, POST_FAIL, ce, "BAD CRC: BUILT-IN", t0, 0);
    } else {
        check_end(POST_CATALOG, POST_WARN, ce, "NONE: BUILT-IN", t0, 0);
    }
}

/* ---- battery: MAX77705 fuel gauge on HSI2C 0x14360000 (USI_CMGP03), read only -----------------
 * A bounded copy of em.c's bus code (build/em-research-battery.md): the same platform setup
 * S-Boot does, the timing registers snapshotted before the first controller reset and written
 * back, register reads with a repeated start, then with STOP. Never writes a MAX77705 register;
 * never reads the clear-on-read interrupt registers. Each message gives up after 3 ms and the
 * whole check after 40 ms. (A shared batt.c can replace this copy once it exists.) */
#define BI 0x14360000UL
#define BI_PMIC 0x66U
#define BI_FG 0x36U
#define BI_MSG_US 3000U
#define BI_CHECK_MS 40U

static uint32_t bi_rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void bi_wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static uint32_t bi_fs1, bi_fs2, bi_fs3;
static int bi_snap;
static uint64_t bi_deadline;

static int bi_late(void)
{
    return p_ticks() > bi_deadline;
}

static void bi_bus(void)
{
    bi_wr(BI + 0x20, 0);
    bi_wr(BI + 0xc4, 0);
    bi_wr(BI + 0x00, bi_rd(BI + 0x00) | 0x80000000U);
    bi_wr(BI + 0x00, bi_rd(BI + 0x00) & ~0x80000000U);
    bi_wr(BI + 0x68, bi_fs3);
    bi_wr(BI + 0x64, bi_fs2);
    bi_wr(BI + 0x60, bi_fs1);
    bi_wr(BI + 0x00, 0x8U);
    bi_wr(BI + 0x08, 0x00ffffffU);
    bi_wr(BI + 0x48, bi_rd(BI + 0x48) & ~0x80000000U);
    bi_wr(BI + 0x40, bi_rd(BI + 0x40) | 0x80000000U);
    bi_wr(BI + 0x24, bi_rd(BI + 0x24) & 0xfffU);
}

static int bi_msg(unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop)
{
    int xfer_done = 0;
    unsigned done = 0, settle = 0;
    unsigned trig = count < 8 ? count : 8;
    uint64_t t0 = p_ticks(), lim = p_freq() / 1000000U * BI_MSG_US;
    bi_wr(BI + 0x04, 3U | (trig << 4) | (trig << 16));
    bi_wr(BI + 0x70, addr << 10);
    bi_wr(BI + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    bi_wr(BI + 0x20, 0x80U);
    bi_wr(BI + 0x44, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    bi_wr(BI + 0x44, bi_rd(BI + 0x44) | 0x80000000U);
    for (unsigned guard = 0; guard < 200000U && p_ticks() - t0 < lim; ++guard) {
        uint32_t irq = bi_rd(BI + 0x24);
        if (irq)
            bi_wr(BI + 0x24, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(bi_rd(BI + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)bi_rd(BI + 0x38);
        } else {
            while (done < count && !(bi_rd(BI + 0x30) & (1U << 7)))
                bi_wr(BI + 0x34, bytes[done++]);
        }
        if (!receive && !stop && done == count && (bi_rd(BI + 0x30) & (1U << 8)))
            ++settle;
        if (done == count && (xfer_done || (receive && stop) || settle > 200)) {
            bi_wr(BI + 0x20, 0);
            if (!stop)
                return 1;
            for (unsigned k = 0; k < 2000; ++k) {
                if (!(bi_rd(BI + 0x50) & (1U << 17)))
                    return 1;
                hw_wait_us(1);
            }
            break;
        }
        hw_wait_us(1);
    }
    bi_wr(BI + 0x20, 0);
    bi_bus();
    return 0;
}

static int bi_read(unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    if (bi_late())
        return 0;
    if (bi_msg(addr, 0, &reg, 1, 0) && bi_msg(addr, 1, data, len, 1))
        return 1;
    bi_bus();
    if (bi_late())
        return 0;
    if (bi_msg(addr, 0, &reg, 1, 1) && bi_msg(addr, 1, data, len, 1))
        return 1;
    bi_bus();
    return 0;
}

static int bi_init(void)
{
    static const uint32_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };   /* CMU_CMGP: USI_CMGP03 */
    for (unsigned i = 0; i < 4; ++i)
        bi_wr(0x14200000UL + gates[i], bi_rd(0x14200000UL + gates[i]) | 0x00300000U);
    bi_wr(0x14203050UL, bi_rd(0x14203050UL) | 3U);
    bi_wr(0x14212030UL, 4U);                                   /* USI mode: I2C */
    for (uintptr_t g = 0x14220180UL; g <= 0x142201a0UL; g += 0x20) {   /* GPM14/15[0]: func 2, no pull */
        bi_wr(g, (bi_rd(g) & ~0xfU) | 0x2U);
        bi_wr(g + 8, bi_rd(g + 8) & ~0xfU);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    if (!bi_snap) {
        bi_snap = 1;
        bi_fs1 = bi_rd(BI + 0x60);
        bi_fs2 = bi_rd(BI + 0x64);
        bi_fs3 = bi_rd(BI + 0x68);
        if (!((bi_fs3 >> 16) & 0xffU)) {                       /* nothing from S-Boot: 200 MHz, 400 kHz */
            bi_fs3 = (bi_fs3 & ~0x00ff0000U) | (0x21U << 16);
            bi_fs2 = (bi_fs2 & ~0xffU) | 0xe0U;
            bi_fs1 = (bi_fs1 & ~0x00ff0000U) | (0xf0U << 16);
        }
    }
    bi_bus();
    uint8_t id;
    return bi_read(BI_PMIC, 0x00, &id, 1);
}

static int fg_word(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    if (!bi_read(BI_FG, reg, b, 2))
        return 0;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

static void check_battery(void)
{
    uint64_t t0 = p_ticks();
    uint16_t soc = 0, mv = 0, cur = 0;
    check_begin(POST_BATTERY);
    bi_deadline = t0 + p_freq() / 1000U * BI_CHECK_MS * (p_mode == POST_FULL ? 3U : 1U);
    if (!bi_init()) {
        check_end(POST_BATTERY, POST_FAIL, -1, "MAX77705 NO ANSWER", t0, 0);
        return;
    }
    if (!fg_word(0x06, &soc)) {
        check_end(POST_BATTERY, POST_FAIL, -1, "FUEL GAUGE UNREAD", t0, 0);
        return;
    }
    unsigned pct = soc >> 8;
    struct pl l;
    pl_init(&l);
    pl_u(&l, pct);
    pl_s(&l, " %");
    if (p_mode == POST_FULL && fg_word(0x09, &mv)) {
        pl_s(&l, " ");
        pl_u(&l, (uint64_t)mv * 5U / 64U);                    /* 78.125 uV per bit */
        pl_s(&l, "MV");
    }
    if (p_mode == POST_FULL && fg_word(0x0a, &cur)) {
        pl_s(&l, " ");
        pl_i(&l, (int32_t)(int16_t)cur * 5 / 16);             /* 0.3125 mA per bit (5 mOhm) */
        pl_s(&l, "MA");
    }
    check_end(POST_BATTERY, pct > 100 ? POST_WARN : pct < 10 ? POST_WARN : POST_OK, (int32_t)pct, l.b, t0,
              BI_CHECK_MS * (p_mode == POST_FULL ? 3U : 1U));
}

/* ---- TMU (em.c fill_thermal), read only ------------------------------------------------------- */
#define TMU_TOP 0x10070000UL
#define TMU_SUB 0x10074000UL

static int tmu_temp(uintptr_t base, unsigned p)
{
    uint32_t tri = bi_rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((bi_rd(base) >> 23) & 1U);
    if (!te1)
        te1 = 55;
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    int code = (int)((bi_rd(base + off) >> sh) & 0x1ffU);
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

static int tmu_on(uintptr_t base)
{
    return (bi_rd(base + 0x20) & 1U) != 0;
}

static void check_temp(void)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_TEMP);
    int top = tmu_on(TMU_TOP), sub = tmu_on(TMU_SUB), cpu = -100, gpu = -100;
    if (top) {
        static const unsigned char probes[6] = { 1, 2, 3, 4, 6, 5 };   /* big (DT mask 0x5E), little */
        for (unsigned i = 0; i < (p_mode == POST_FULL ? 6U : 2U); ++i) {
            int t = tmu_temp(TMU_TOP, probes[i]);
            if (t > cpu)
                cpu = t;
        }
    }
    if (sub) {
        gpu = tmu_temp(TMU_SUB, 1);
        if (p_mode == POST_FULL) {
            int isp = tmu_temp(TMU_SUB, 2);
            if (isp > gpu)
                gpu = isp;
        }
    }
    if (!top && !sub) {
        check_end(POST_TEMP, POST_WARN, 0, "TMU OFF", t0, 0);
        return;
    }
    int hot = cpu > gpu ? cpu : gpu;
    struct pl l;
    pl_init(&l);
    if (top) {
        pl_s(&l, "CPU ");
        pl_i(&l, cpu);
        pl_s(&l, "C ");
    }
    if (sub) {
        pl_s(&l, "GPU ");
        pl_i(&l, gpu);
        pl_s(&l, "C");
    }
    unsigned st = (hot >= 90 || hot < -20) ? POST_FAIL : hot >= 70 ? POST_WARN : POST_OK;
    check_end(POST_TEMP, st, hot, l.b, t0, 5U);
}

/* ---- touch, microSD --------------------------------------------------------------------------- */

static void check_touch(int touch)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_TOUCH);
    if (!touch) {
        check_end(POST_TOUCH, POST_SKIP, 0, "NOT STARTED", t0, 0);
        return;
    }
    hw_tp_start();
    if (hw_tp_ok())
        check_end(POST_TOUCH, POST_OK, 1, "S6SY761 READY", t0, 600U);
    else
        check_end(POST_TOUCH, POST_FAIL, 0, "S6SY761 NO ANSWER", t0, 0);
}

static void check_microsd(void)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_MICROSD);
    int present = !(bi_rd(0x14050044UL) & (1U << 5));          /* card detect GPA1-5, active low */
    check_end(POST_MICROSD, POST_OK, present, present ? "CARD PRESENT" : "NO CARD", t0, 2U);
}

/* ---- logs ------------------------------------------------------------------------------------- */

static void post_log(void)
{
    volatile uint64_t *log = POST_LOG;
    log[POST_LOG_SLOT] = (uint64_t)POST_MAGIC << 32 | (uint64_t)rep.mode << 24 | (uint64_t)rep.n << 16 |
                         (uint64_t)(rep.fails & 0xffU) << 8 | (rep.warns & 0xffU);
    log[POST_LOG_SLOT + 1] = rep.total_us;
    for (unsigned i = 0; i < rep.n; ++i) {
        const struct post_check *c = &rep.c[i];
        log[POST_LOG_SLOT + 2 + i] = (uint64_t)c->state << 56 | (uint64_t)((uint32_t)c->value & 0xffffffU) << 32 | c->us;
    }
    char b[96];
    unsigned n = 0;
    static const char head[] = "STARBITE: POST ";
    for (unsigned k = 0; head[k]; ++k)
        b[n++] = head[k];
    struct pl l;
    pl_init(&l);
    pl_s(&l, rep.mode == POST_FULL ? "full " : "quick ");
    pl_u(&l, rep.total_us / 1000U);
    pl_s(&l, "ms fail ");
    pl_u(&l, rep.fails);
    pl_s(&l, " warn ");
    pl_u(&l, rep.warns);
    for (unsigned k = 0; k < l.n; ++k)
        b[n++] = l.b[k];
    b[n++] = '\n';
    b[n] = 0;
    star_pmsg(b);
    for (unsigned i = 0; i < rep.n; ++i) {
        const struct post_check *c = &rep.c[i];
        if (c->state != POST_FAIL && c->state != POST_WARN)
            continue;
        pl_init(&l);
        pl_s(&l, c->state == POST_FAIL ? "STARBITE: POST fail " : "STARBITE: POST warn ");
        pl_s(&l, c->name);
        star_pmsg(l.b);
        star_pmsg(": ");
        star_pmsg(c->detail);
        star_pmsg("\n");
    }
}

/* ---- entry points ----------------------------------------------------------------------------- */

int post_run(unsigned mode, unsigned row0, int touch, int *bixby)
{
    uint64_t t0 = p_ticks();
    p_mode = mode;
    p_row0 = row0;
    p_bixby = bixby;
    rep.magic = 0;
    rep.mode = mode;
    rep.n = POST_N;
    rep.fails = rep.warns = rep.fatal = 0;
    star_fb_text_c(row0, mode == POST_FULL ? "STARBITE POST (FULL)" : "STARBITE POST", C_GOLD, C_BLACK);
    check_ram();
    check_timer();
    if (!rep.c[POST_RAM].fatal && !rep.c[POST_TIMER].fatal) {
        check_pmic();
        check_storage();
        check_battery();
        check_temp();
        check_touch(touch);
        check_microsd();
    } else {
        for (unsigned id = POST_PMIC; id < POST_N; ++id) {
            check_begin(id);
            check_end(id, POST_SKIP, 0, "SKIPPED", p_ticks(), 0);
        }
    }
    rep.fatal = rep.c[POST_RAM].fatal || rep.c[POST_TIMER].fatal;
    rep.total_us = p_us(t0);
    struct pl l;
    pl_init(&l);
    pl_u(&l, post_ok_count(&rep));
    pl_s(&l, " OK  ");
    pl_u(&l, rep.warns);
    pl_s(&l, " WARN  ");
    pl_u(&l, rep.fails);
    pl_s(&l, " FAILED  ");
    pl_u(&l, rep.total_us / 1000U);
    pl_s(&l, " MS");
    star_fb_text_c(row0 + 1 + POST_N, l.b, rep.fails ? C_RED : rep.warns ? C_GOLD : C_GREEN, C_BLACK);
    rep.magic = POST_MAGIC;
    rep_valid = 1;
    post_log();
    p_bixby = 0;
    return (int)rep.fatal;
}

int post_boot(int touch, int *bixby, const char **why)
{
    unsigned base = rec_row_base();
    star_fb_row_base(0);
    unsigned y0 = star_fb_row_y(0), y1 = star_fb_row_y(POST_N + 2);
    rec_rows_save(y0, y1, POST_ROWS_SAVE);
    int fatal = post_run(POST_QUICK, 0, touch, bixby);
    if (rep.fails && !fatal) {
        /* a red line: time to read it (Bixby still enters recovery) */
        uint64_t t0 = p_ticks(), n = p_freq() / 1000U * POST_HOLD_MS;
        for (unsigned g = 0; g < POST_HOLD_MS + 100U && p_ticks() - t0 < n; ++g) {
            if (bixby && !*bixby && rec_bixby_now())
                *bixby = 1;
            hw_wait_us(1000);
        }
    }
    rec_rows_restore(y0, y1, POST_ROWS_SAVE);
    star_fb_row_base(base);
    if (why)
        *why = rep.c[POST_RAM].fatal ? "POST: RAM PATTERN FAILED" : rep.c[POST_TIMER].fatal ? "POST: TIMER STOPPED" : 0;
    return fatal;
}
