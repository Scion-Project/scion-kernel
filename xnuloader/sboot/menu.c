/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite boot menu and Starbite Engineer mode, run by the loader
 * before it copies XNU (MMU off, one CPU, S-Boot's state).
 *
 * Keys are the S9's ALIVE GPIOs, active low (starlte DTB gpio_keys;
 * a5xelte_keys.c STARLTE): GPA0 DAT 0x14050024 bit 3 Volume Up, bit 4 Volume
 * Down, bit 6 Bixby; GPA2 DAT 0x14050064 bit 4 Power. Volume moves, Power (or
 * Bixby) selects. Reboots go through the Exynos9810 PMU 0x14060000 the way
 * the vendor kernel does: INFORM2 0x12345678, INFORM3 0x12345671 Download or
 * 0x12345674 Recovery, then SWRESET.
 *
 * Engineer mode only reads hardware, except where marked RISKY: those items
 * change the boot-args (UFS multi-block, CPU probe) or write two PMIC
 * regulators with the kernel's own safety checks (touch rails).
 * No pointer tables here: the Image must have no absolute relocations.
 */
#include "menu.h"
#include "fbtext.h"
#include "hw.h"
#include "catalog.h"
#include "store.h"
#include "ui.h"

#define ALIVE_GPA0_DAT 0x14050024UL
#define ALIVE_GPA2_DAT 0x14050064UL
#define K_SELECT (K_BIXBY | K_POWER)

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BLUE 0xff2a5bf0U      /* Starbite mark gradient */
#define C_MUTED 0xffb1bad3U
#define C_GOLD 0xffffd43bU
#define C_RED 0xffff4d4dU
#define C_GREEN 0xff3ddc84U

/* ---- time, keys, reboot ------------------------------------------------ */

static uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (v));
    return v;
}

static uint64_t freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    return f ? f : 26000000U;   /* S-Boot leaves CNTFRQ 0; the timer is 26 MHz */
}

static void wait_us(uint64_t us)
{
    uint64_t t0 = ticks(), n = freq() / 1000000U * us;
    while (ticks() - t0 < n)
        ;
}

static void wait_ms(unsigned ms)
{
    wait_us((uint64_t)ms * 1000U);
}

static unsigned keys(void)
{
    uint32_t a = *(volatile uint32_t *)ALIVE_GPA0_DAT;
    uint32_t b = *(volatile uint32_t *)ALIVE_GPA2_DAT;
    return (!((a >> 3) & 1U) ? K_UP : 0) | (!((a >> 4) & 1U) ? K_DOWN : 0) |
           (!((a >> 6) & 1U) ? K_BIXBY : 0) | (!((b >> 4) & 1U) ? K_POWER : 0);
}

/* One debounced press, returned once every key is released again. */
static unsigned key_wait(void)
{
    while (keys())
        wait_ms(20);
    for (;;) {
        unsigned k = keys();
        if (k) {
            wait_ms(40);
            if (keys() == k) {
                while (keys())
                    wait_ms(20);
                return k;
            }
        }
        wait_ms(10);
    }
}

static void reboot(uint32_t inform3)
{
    volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;
    pmu[0x808 / 4] = 0x12345678U;
    pmu[0x80c / 4] = inform3;
    __asm__ volatile("dsb sy" : : : "memory");
    pmu[0x400 / 4] = 1;
    __asm__ volatile("dsb sy" : : : "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- text ------------------------------------------------------------ */

struct sb { char b[STAR_FB_COLS + 2]; unsigned n; };

static void sb_init(struct sb *s)
{
    s->n = 0;
    s->b[0] = 0;
}

static void sb_s(struct sb *s, const char *t)
{
    while (*t && s->n < STAR_FB_COLS)
        s->b[s->n++] = *t++;
    s->b[s->n] = 0;
}

static void sb_c(struct sb *s, char c)
{
    if (s->n < STAR_FB_COLS)
        s->b[s->n++] = c;
    s->b[s->n] = 0;
}

static void sb_hex(struct sb *s, uint64_t v, unsigned digits)
{
    sb_s(s, "0X");
    for (int sh = (int)digits * 4 - 4; sh >= 0; sh -= 4) {
        unsigned d = (unsigned)(v >> sh) & 15U;
        sb_c(s, (char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

static void sb_dec(struct sb *s, uint64_t v)
{
    char t[24];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v);
    while (n)
        sb_c(s, t[--n]);
}

/* "label value" in one row. */
static void row_hex(unsigned row, const char *label, uint64_t v, unsigned digits)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, label);
    sb_c(&s, ' ');
    sb_hex(&s, v, digits);
    star_fb_text(row, s.b);
}

static void row_dec(unsigned row, const char *label, uint64_t v, const char *unit)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, label);
    sb_c(&s, ' ');
    sb_dec(&s, v);
    sb_s(&s, unit);
    star_fb_text(row, s.b);
}

/* ---- menus ----------------------------------------------------------- */

#define MAX_ITEMS 10
static char items[MAX_ITEMS][STAR_FB_COLS + 2];
static uint32_t item_fg[MAX_ITEMS];

static void item(unsigned i, const char *s, uint32_t fg)
{
    unsigned n = 0;
    while (s[n] && n < STAR_FB_COLS - 2) {
        items[i][n] = s[n];
        ++n;
    }
    items[i][n] = 0;
    item_fg[i] = fg;
}

static void draw_item(unsigned row, unsigned i, int selected)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, selected ? "> " : "  ");
    sb_s(&s, items[i]);
    if (selected)
        star_fb_text_c(row, s.b, C_WHITE, C_BLUE);
    else
        star_fb_text_c(row, s.b, item_fg[i], C_BLACK);
}

/* Items 0..n-1 at rows row0..; Volume moves, Power/Bixby picks. */
static unsigned choose(unsigned row0, unsigned n, unsigned sel)
{
    for (unsigned i = 0; i < n; ++i)
        draw_item(row0 + i, i, i == sel);
    for (;;) {
        unsigned k = key_wait(), old = sel;
        if (k & K_SELECT)
            return sel;
        if (k & K_UP)
            sel = sel ? sel - 1 : n - 1;
        else if (k & K_DOWN)
            sel = (sel + 1) % n;
        if (sel != old) {
            draw_item(row0 + old, old, 0);
            draw_item(row0 + sel, sel, 1);
        }
    }
}

/* ---- boot-args ------------------------------------------------------- */

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}

/* Does the token t[0..n) have key `key` (before '=', or the whole token)? */
static int tok_is(const char *t, unsigned n, const char *key)
{
    unsigned i = 0;
    while (key[i] && i < n && t[i] == key[i])
        ++i;
    return !key[i] && (i == n || t[i] == '=');
}

static const char *arg_find(const char *c, const char *key, unsigned *len)
{
    for (const char *p = c; *p;) {
        while (*p == ' ')
            ++p;
        unsigned n = 0;
        while (p[n] && p[n] != ' ')
            ++n;
        if (n && tok_is(p, n, key)) {
            *len = n;
            return p;
        }
        p += n;
    }
    return 0;
}

/* Remove every `key` / `key=...` token. */
static void arg_del(char *c, const char *key)
{
    char *w = c;
    for (const char *p = c; *p;) {
        while (*p == ' ')
            ++p;
        unsigned n = 0;
        while (p[n] && p[n] != ' ')
            ++n;
        if (n && !tok_is(p, n, key)) {
            if (w != c)
                *w++ = ' ';
            for (unsigned i = 0; i < n; ++i)
                *w++ = p[i];
        }
        p += n;
    }
    *w = 0;
}

/* Replace `key` with `key=val` (val 0: the bare flag). */
static void arg_set(char *c, unsigned len, const char *key, const char *val)
{
    arg_del(c, key);
    unsigned n = slen(c), need = 1 + slen(key) + (val ? 1 + slen(val) : 0);
    if (n + need >= len)
        return;
    if (n)
        c[n++] = ' ';
    for (const char *k = key; *k;)
        c[n++] = *k++;
    if (val) {
        c[n++] = '=';
        while (*val)
            c[n++] = *val++;
    }
    c[n] = 0;
}

/* Decimal value of `key=N`, or dflt. */
static unsigned arg_num(const char *c, const char *key, unsigned dflt)
{
    unsigned n;
    const char *t = arg_find(c, key, &n);
    unsigned k = slen(key);
    if (!t || n <= k + 1)
        return dflt;
    unsigned v = 0;
    for (unsigned i = k + 1; i < n && t[i] >= '0' && t[i] <= '9'; ++i)
        v = v * 10U + (unsigned)(t[i] - '0');
    return v;
}

struct star_opt opt;

static void opt_load(const char *c)
{
    unsigned n;
    opt.cpus = arg_num(c, "cpus", 1);
    opt.rw = arg_num(c, "ufsrw", 0) == 1;
    opt.verbose = arg_find(c, "-v", &n) != 0;
    opt.touch = arg_num(c, "startouch", 1) != 0;
    opt.touchpwr = arg_num(c, "startouchpwr", 1) != 0;
    opt.multi = arg_num(c, "ufsmulti", 0) != 0;
    opt.smpprobe = arg_num(c, "starsmpprobe", 0) != 0;
}

static void opt_apply(char *c, unsigned len)
{
    char num[4];
    num[0] = (char)('0' + opt.cpus);
    num[1] = 0;
    arg_set(c, len, "cpus", num);
    /* Multi-block UFS with 16-byte PRDT entries and the 9810 vs_hci offsets
     * is proven (2026-10-27): it no longer forces a read-only root. */
    int rw = opt.rw;
    arg_set(c, len, "ufsrw", rw ? "1" : "0");
    arg_set(c, len, "rootrw", rw ? "1" : "0");
    if (opt.verbose)
        arg_set(c, len, "-v", 0);
    else
        arg_del(c, "-v");
    if (opt.touch)
        arg_del(c, "startouch");
    else
        arg_set(c, len, "startouch", "0");
    if (opt.touchpwr)
        arg_del(c, "startouchpwr");
    else
        arg_set(c, len, "startouchpwr", "0");
    if (opt.multi)
        arg_set(c, len, "ufsmulti", "1");
    else
        arg_del(c, "ufsmulti");
    if (opt.smpprobe)
        arg_set(c, len, "starsmpprobe", "1");
    else
        arg_del(c, "starsmpprobe");
}

/* ---- ACPM / PMIC (as bsd/dev/starlte_touch.c) ------------------------ */

#define ACPM_MBOX 0x14100000UL
#define ACPM_SRAM 0x02039000UL
#define ACPM_SRAM_SIZE 0x30000U
#define ACPM_INITDATA 0x7f00U
#define ACPM_PMIC_CH 2U

static uint32_t ard(uint32_t off)
{
    return *(volatile uint32_t *)(ACPM_SRAM + off);
}

static void awr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(ACPM_SRAM + off) = v;
}

static uint32_t acpm_seq;

/* SRAM offset of channel id's descriptor, or 0. */
static uint32_t acpm_find(uint32_t id)
{
    uint32_t chans = ard(ACPM_INITDATA + 0x08), ap_max = ard(ACPM_INITDATA + 0x18);
    if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48)
        return 0;
    for (uint32_t i = 0; i < ap_max; ++i) {
        uint32_t c = chans + i * 0x48;
        if (ard(c) == id) {
            uint32_t q = ard(c + 0x34), e = ard(c + 0x38);
            return (q && q <= 64 && e >= 16 && e <= 64 && !(e & 3)) ? c : 0;
        }
    }
    return 0;
}

static int acpm_send(uint32_t c, uint32_t id, uint32_t cmd[4])
{
    uint32_t qlen = ard(c + 0x34), esz = ard(c + 0x38);
    uint32_t tx_rear = ard(c + 0x10), tx_front = ard(c + 0x14), tx_base = ard(c + 0x18);
    uint32_t rx_rear = ard(c + 0x28), rx_front = ard(c + 0x2c), rx_base = ard(c + 0x30);
    uint32_t front = ard(tx_front), next = (front + 1) % qlen;
    for (unsigned us = 0; next == ard(tx_rear); ++us) {
        if (us > 100000)
            return 0;
        wait_us(1);
    }
    if (++acpm_seq >= 64)
        acpm_seq = 1;
    uint32_t seq = acpm_seq;
    cmd[0] = (cmd[0] & ~(0x3fU << 16)) | (seq << 16);
    uint32_t slot = tx_base + esz * front;
    for (uint32_t i = 0; i < esz / 4; ++i)
        awr(slot + 4 * i, i < 4 ? cmd[i] : 0);
    __asm__ volatile("dsb sy" : : : "memory");
    awr(tx_front, next);
    __asm__ volatile("dsb sy" : : : "memory");
    *(volatile uint32_t *)(ACPM_MBOX + 0x08) = (1U << id) << 16;    /* INTGR0 */
    for (unsigned us = 0; us < 100000; ++us) {
        uint32_t rear = ard(rx_rear), rfront = ard(rx_front);
        for (uint32_t i = rear; i != rfront; i = (i + 1) % qlen) {
            uint32_t e = rx_base + esz * i;
            if (((ard(e) >> 16) & 0x3fU) != seq)
                continue;
            for (uint32_t w = 0; w < 4; ++w)
                cmd[w] = ard(e + 4 * w);
            if (i != rear)
                for (uint32_t w = 0; w < esz / 4; ++w)
                    awr(e + 4 * w, ard(rx_base + esz * rear + 4 * w));
            rear = (rear + 1) % qlen;
            awr(rx_rear, rear);
            __asm__ volatile("dsb sy" : : : "memory");
            if (rear == ard(rx_front))
                *(volatile uint32_t *)(ACPM_MBOX + 0x20) = 1U << id;  /* INTCR1 */
            return 1;
        }
        wait_us(1);
    }
    return 0;
}

static int pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = cmd[2] = cmd[3] = 0;
    if (!c || !acpm_send(c, ACPM_PMIC_CH, cmd) || (cmd[1] >> 24))
        return 0;
    *val = (uint8_t)(cmd[1] >> 8);
    return 1;
}

static int pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = 1U | (uint32_t)val << 8;
    cmd[2] = cmd[3] = 0;
    return c && acpm_send(c, ACPM_PMIC_CH, cmd) && !(cmd[1] >> 24);
}

/* ---- touch: S6SY761 on HSI2C (port of bsd/dev/starlte_touch.c) -------- */

/*
 * Samsung S6SY761 at 0x48 on HSI2C 0x104B0000 (USI03; pins GPP1-4/5
 * function 2), INT on GPA1-0 active low (starlte DTB). Same bring-up as the
 * iOS driver: tsp_io/tsp_avdd (S2MPS18 LDO35/LDO43) on through ACPM with the
 * same voltage checks, CMU_PERIC0 gates, SYSREG_PERIC0 USI mode, pin mux,
 * then BOOT_STATUS 0x55 (0x20 = application), PANEL_INFO 0x23, SENSE_ON 0x10,
 * CLEAR_EVENT_STACK 0x62, READ_ONE_EVENT 0x60 (8 bytes: EID [1:0] 0 =
 * coordinate, TID [5:2] - 1, state [7:6] 1 press / 2 move / 3 release,
 * x = b1 << 4 | b3 >> 4, y = b2 << 4 | b3 & 0xf). Any failure leaves the menu
 * on the keys alone.
 */
#define TP_CMU 0x10400000UL
#define TP_SYSREG 0x10411000UL
#define TP_GPIO 0x10430000UL
#define TP_I2C 0x104b0000UL
#define TP_ALIVE 0x14050000UL
#define TP_ADDR 0x48U

static int tp_ok;
static unsigned tp_max_x = 4096, tp_max_y = 4096;

static uint32_t mrd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void mwr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static int tp_power(void)
{
    uint8_t id = 0, l35 = 0, l43 = 0;
    if (!pmic_read(0, 0x00, &id) || !pmic_read(1, 0x6c, &l35) || !pmic_read(1, 0x74, &l43))
        return 0;
    if ((l35 & 0x3fU) != 0x2cU && (l35 & 0x3fU) != 0)
        return 0;
    if ((l43 & 0x3fU) != 0 && ((l43 & 0x3fU) < 0x28U || (l43 & 0x3fU) > 0x3cU))
        return 0;
    if ((l35 & 0xc0U) == 0xc0U && (l43 & 0xc0U) == 0xc0U)
        return 1;
    return pmic_write(1, 0x6c, 0xec) && pmic_write(1, 0x74, 0xf0);
}

static void tp_platform(void)
{
    static const uint32_t gates[4] = { 0x2018, 0x206c, 0x20dc, 0x20e0 };
    for (unsigned i = 0; i < 4; ++i)
        mwr(TP_CMU + gates[i], mrd(TP_CMU + gates[i]) | 0x00300000U);
    mwr(TP_CMU + 0x3038, mrd(TP_CMU + 0x3038) | 0x3U);
    mwr(TP_SYSREG + 0x1c, 0x4U);
    mwr(TP_GPIO + 0x20, (mrd(TP_GPIO + 0x20) & ~0x00ff0000U) | 0x00220000U);
    mwr(TP_GPIO + 0x28, mrd(TP_GPIO + 0x28) & ~0x00ff0000U);
    mwr(TP_GPIO + 0x2c, mrd(TP_GPIO + 0x2c) & ~0x00ff0000U);
    __asm__ volatile("dsb sy" : : : "memory");
}

static void tp_bus(void)
{
    mwr(TP_I2C + 0x20, 0);
    mwr(TP_I2C + 0xc4, 0);
    mwr(TP_I2C + 0x00, mrd(TP_I2C + 0x00) | 0x80000000U);
    mwr(TP_I2C + 0x00, mrd(TP_I2C + 0x00) & ~0x80000000U);
    mwr(TP_I2C + 0x68, (mrd(TP_I2C + 0x68) & ~0x00ff0000U) | (0x21U << 16));
    mwr(TP_I2C + 0x64, (mrd(TP_I2C + 0x64) & ~0x000000ffU) | 0xe0U);
    mwr(TP_I2C + 0x60, (mrd(TP_I2C + 0x60) & ~0x00ff0000U) | (0xf0U << 16));
    mwr(TP_I2C + 0x00, 0x8U);
    mwr(TP_I2C + 0x08, 0x00ffffffU);
    mwr(TP_I2C + 0x48, mrd(TP_I2C + 0x48) & ~0x80000000U);
    mwr(TP_I2C + 0x40, mrd(TP_I2C + 0x40) | 0x80000000U);
    mwr(TP_I2C + 0x24, mrd(TP_I2C + 0x24) & 0xfffU);
}

/* One message with STOP, count <= 32 (st_message). */
static int tp_message(int receive, uint8_t *bytes, unsigned count)
{
    int xfer_done = 0;
    unsigned done = 0;
    if (!count || count > 32)
        return 0;
    tp_bus();
    unsigned trig = count < 8 ? count : 8;
    mwr(TP_I2C + 0x04, 3U | (trig << 4) | (trig << 16));
    mwr(TP_I2C + 0x70, TP_ADDR << 10);
    mwr(TP_I2C + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    mwr(TP_I2C + 0x20, 0x80U);
    mwr(TP_I2C + 0x44, count | (receive ? 0x10000U : 0) | 0x20000U);
    mwr(TP_I2C + 0x44, mrd(TP_I2C + 0x44) | 0x80000000U);
    for (unsigned us = 0; us < 50000; ++us) {
        uint32_t irq = mrd(TP_I2C + 0x24);
        if (irq)
            mwr(TP_I2C + 0x24, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(mrd(TP_I2C + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)mrd(TP_I2C + 0x38);
        } else {
            while (done < count && !(mrd(TP_I2C + 0x30) & (1U << 7)))
                mwr(TP_I2C + 0x34, bytes[done++]);
        }
        if (done == count && (xfer_done || receive)) {
            mwr(TP_I2C + 0x20, 0);
            for (unsigned n = 0; n < 10000; ++n) {
                if (!(mrd(TP_I2C + 0x50) & (1U << 17)))
                    return 1;
                wait_us(1);
            }
            break;
        }
        wait_us(1);
    }
    mwr(TP_I2C + 0x20, 0);
    tp_bus();
    return 0;
}

static int tp_read(uint8_t reg, uint8_t *data, unsigned len, unsigned tries)
{
    for (unsigned a = 0; a < tries; ++a) {
        if (a)
            wait_us(200);
        if (tp_message(0, &reg, 1) && tp_message(1, data, len))
            return 1;
    }
    return 0;
}

static int tp_cmd(uint8_t reg)
{
    for (unsigned a = 0; a < 10; ++a) {
        if (a)
            wait_us(200);
        if (tp_message(0, &reg, 1))
            return 1;
    }
    return 0;
}

static void tp_init(void)
{
    uint8_t b[12];
    tp_ok = 0;
    if (!tp_power())
        return;
    wait_ms(150);                   /* supplies up, the S6SY761 boots its firmware */
    tp_platform();
    tp_bus();
    if (!tp_read(0x55, b, 1, 10) || b[0] != 0x20)
        return;
    if (tp_read(0x23, b, 11, 10)) {
        unsigned mx = (unsigned)b[0] << 8 | b[1], my = (unsigned)b[2] << 8 | b[3];
        if (mx >= 1000 && my >= 1000) {
            tp_max_x = mx;
            tp_max_y = my;
        }
    }
    if (!tp_cmd(0x10))
        return;
    wait_ms(20);
    (void)tp_cmd(0x62);
    tp_ok = 1;
}

/* One finger-0 event: *st 1 down, 2 move, 3 up, with panel coordinates.
 * Returns 0 when INT is high or the stack is empty. */
static int tp_event(int *st, int *x, int *y)
{
    uint8_t e[8];
    if (!tp_ok || (mrd(TP_ALIVE + 0x44) & 1U))
        return 0;
    for (unsigned n = 0; n < 16; ++n) {
        if (!tp_read(0x60, e, 8, 3) || e[0] == 0)
            return 0;
        if ((e[0] & 0x3U) != 0)
            continue;
        int tid = (int)((e[0] >> 2) & 0xfU) - 1;
        unsigned state = e[0] >> 6;
        if (tid != 0 || state < 1)
            continue;
        unsigned rx = (unsigned)e[1] << 4 | e[3] >> 4;
        unsigned ry = (unsigned)e[2] << 4 | (e[3] & 0xfU);
        *x = (int)((uint64_t)rx * STAR_FB_WIDTH / tp_max_x);
        *y = (int)((uint64_t)ry * STAR_FB_HEIGHT / tp_max_y);
        *st = (int)state;
        return 1;
    }
    return 0;
}

/* Returns 1 on a tap (finger 0 lifted within 80 px of where it went down). */
static int tp_down, tp_x0, tp_y0;

static int tp_tap(int *x, int *y)
{
    int st, ex, ey, tap = 0;
    while (tp_event(&st, &ex, &ey)) {
        if (st == 1) {
            tp_down = 1;
            tp_x0 = ex;
            tp_y0 = ey;
        } else if (st == 3 && tp_down) {
            tp_down = 0;
            int dx = ex - tp_x0, dy = ey - tp_y0;
            if (dx > -80 && dx < 80 && dy > -80 && dy < 80) {
                *x = ex;
                *y = ey;
                tap = 1;
            }
        }
    }
    return tap;
}

/* "LDO43 0XF0 ON 3.000 V": S2MPS18 LDO CTRL, bits 7:6 enable, 5:0 25 mV steps. */
static void row_ldo(unsigned row, const char *name, int ok, uint8_t v, unsigned base_mv)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, name);
    sb_c(&s, ' ');
    if (!ok) {
        sb_s(&s, "READ FAILED");
        star_fb_text_c(row, s.b, C_RED, C_BLACK);
        return;
    }
    unsigned mv = base_mv + (v & 0x3fU) * 25U;
    sb_hex(&s, v, 2);
    sb_s(&s, (v & 0xc0U) ? " ON  " : " OFF ");
    sb_dec(&s, mv / 1000U);
    sb_c(&s, '.');
    sb_c(&s, (char)('0' + mv / 100U % 10U));
    sb_c(&s, (char)('0' + mv / 10U % 10U));
    sb_c(&s, (char)('0' + mv % 10U));
    sb_s(&s, " V");
    star_fb_text_c(row, s.b, (v & 0xc0U) ? C_GREEN : C_GOLD, C_BLACK);
}

/* ---- Engineer pages --------------------------------------------------- */

#define ROW_TITLE 3U
#define ROW_BODY 5U
#define ROW_HINT 43U

static void page(const char *title, const char *hint)
{
    star_fb_fill(C_BLACK);
    star_fb_text_c(0, "STARBITE ENGINEER", C_WHITE, C_BLUE);
    star_fb_text_c(1, "PROJECT STARBITE", C_MUTED, C_BLACK);
    star_fb_text_c(ROW_TITLE, title, C_GOLD, C_BLACK);
    star_fb_text_c(ROW_HINT, hint, C_MUTED, C_BLACK);
}

#define HINT_MENU "VOL: MOVE   POWER: SELECT"
#define HINT_BACK "PRESS POWER TO GO BACK"

static void wait_select(void)
{
    while (!(key_wait() & K_SELECT))
        ;
}

static void diag_sysinfo(const struct star_info *in, const char *cmdline)
{
    uint64_t midr, mpidr;
    __asm__ volatile("mrs %0, midr_el1" : "=r" (midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    page("SYSTEM INFO", HINT_BACK);
    unsigned r = ROW_BODY;
    row_hex(r++, "MIDR     ", midr, 8);
    star_fb_text_c(r++, ((midr >> 4) & 0xfffU) == 0xd05 ? "         CORTEX-A55 (LITTLE)" :
                   "         NOT A CORTEX-A55", C_MUTED, C_BLACK);
    row_hex(r++, "MPIDR    ", mpidr, 8);
    row_dec(r++, "EL       ", in->el >> 2, "");
    row_hex(r++, "SCTLR    ", in->sctlr, 8);
    if (f)
        row_dec(r++, "CNTFRQ   ", f, " HZ");
    else
        star_fb_text(r++, "CNTFRQ   0 (26 MHZ ASSUMED)");
    row_hex(r++, "FDT      ", in->fdt, 8);
    row_hex(r++, "PAYLOAD  ", in->rd_start, 8);
    row_dec(r++, "  SIZE   ", (in->rd_end - in->rd_start) >> 10, " KIB");
    row_hex(r++, "XNU RAM  ", in->phys_base, 8);
    row_dec(r++, "  SIZE   ", in->mem_size >> 20, " MIB");
    row_hex(r++, "ENTRY VA ", in->entry_va, 16);
    row_dec(r++, "SEGMENTS ", in->nsegs, "");
    ++r;
    star_fb_text_c(r++, "BOOT-ARGS:", C_GOLD, C_BLACK);
    /* Wrap the boot-args over the remaining rows. */
    for (const char *p = cmdline; *p && r < ROW_HINT - 1;) {
        struct sb s;
        sb_init(&s);
        while (*p && s.n < STAR_FB_COLS)
            sb_c(&s, *p++);
        star_fb_text(r++, s.b);
    }
    wait_select();
}

/* Bars written as 0xAARRGGBB; a swapped label shows an R/B swap. */
static void diag_display(void)
{
    page("DISPLAY COLOUR TEST", "ANY KEY: BACK");
    static const uint32_t bars[6] = {
        0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xffffffffU, 0xff808080U, 0xffffd43bU,
    };
    static const char names[6][16] = {
        "RED", "GREEN", "BLUE", "WHITE", "50% GREY", "GOLD",
    };
    for (unsigned i = 0; i < 6; ++i) {
        unsigned row = ROW_BODY + i * 6U;
        star_fb_text(row, names[i]);
        star_fb_fill_rows(star_fb_row_y(row + 1), star_fb_row_y(row + 5), bars[i]);
    }
    star_fb_text_c(ROW_BODY + 36, "EACH BAR SHOULD MATCH ITS NAME.", C_MUTED, C_BLACK);
    key_wait();
}

static void diag_buttons(void)
{
    page("BUTTON TEST", "HOLD VOL UP + VOL DOWN: BACK");
    star_fb_text_c(ROW_BODY, "PRESS ANY BUTTON:", C_MUTED, C_BLACK);
    unsigned last = ~0U;
    for (;;) {
        unsigned k = keys();
        if ((k & (K_UP | K_DOWN)) == (K_UP | K_DOWN))
            break;
        if (k != last) {
            star_fb_text_c(ROW_BODY + 2, (k & K_UP) ? "VOLUME UP     PRESSED" : "VOLUME UP     -",
                           (k & K_UP) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 3, (k & K_DOWN) ? "VOLUME DOWN   PRESSED" : "VOLUME DOWN   -",
                           (k & K_DOWN) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 4, (k & K_BIXBY) ? "BIXBY         PRESSED" : "BIXBY         -",
                           (k & K_BIXBY) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 5, (k & K_POWER) ? "POWER         PRESSED" : "POWER         -",
                           (k & K_POWER) ? C_GREEN : C_WHITE, C_BLACK);
            row_hex(ROW_BODY + 7, "GPA0 DAT", *(volatile uint32_t *)ALIVE_GPA0_DAT, 8);
            row_hex(ROW_BODY + 8, "GPA2 DAT", *(volatile uint32_t *)ALIVE_GPA2_DAT, 8);
            last = k;
        }
        wait_ms(20);
    }
    while (keys())
        wait_ms(20);
}

static void show_pmic(unsigned r)
{
    uint8_t id = 0, l35 = 0, l43 = 0;
    char build[26];
    for (unsigned i = 0; i < 24; i += 4) {
        uint32_t w = ard(ACPM_INITDATA + 0x60 + i);
        for (unsigned b = 0; b < 4; ++b) {
            char c = (char)(w >> (8 * b));
            build[i + b] = (c >= 32 && c < 127) ? c : '.';
        }
    }
    build[24] = 0;
    struct sb s;
    sb_init(&s);
    sb_s(&s, "ACPM ");
    sb_s(&s, build);
    star_fb_text(r++, s.b);
    if (!acpm_find(ACPM_PMIC_CH)) {
        star_fb_text_c(r, "ACPM PMIC CHANNEL NOT FOUND", C_RED, C_BLACK);
        return;
    }
    int ok = pmic_read(0, 0x00, &id);
    if (ok)
        row_hex(r++, "S2MPS18 ID", id, 2);
    else
        star_fb_text_c(r++, "S2MPS18 ID READ FAILED", C_RED, C_BLACK);
    row_ldo(r++, "LDO35 TSP_IO  ", pmic_read(1, 0x6c, &l35), l35, 700);
    row_ldo(r++, "LDO43 TSP_AVDD", pmic_read(1, 0x74, &l43), l43, 1800);
    star_fb_text_c(r + 1, "TOUCH WANTS LDO35 1.800 V, LDO43 3.000 V", C_MUTED, C_BLACK);
}

static void diag_pmic(void)
{
    page("PMIC / TOUCH RAILS (READ ONLY)", HINT_BACK);
    show_pmic(ROW_BODY);
    wait_select();
}

/* 16 MiB of the XNU window, before XNU is copied there. */
#define RAMTEST_BASE 0x92000000UL
#define RAMTEST_SIZE (16UL << 20)

static void diag_ram(void)
{
    page("RAM TEST (16 MIB AT 0X92000000)", HINT_BACK);
    volatile uint64_t *m = (volatile uint64_t *)RAMTEST_BASE;
    uint64_t n = RAMTEST_SIZE / 8, errors = 0, t0 = ticks();
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t inv = pass ? ~0ULL : 0;
        star_fb_text(ROW_BODY + pass, pass ? "PASS 2: INVERTED PATTERN..." : "PASS 1: ADDRESS PATTERN...");
        for (uint64_t i = 0; i < n; ++i)
            m[i] = (i * 0x9e3779b97f4a7c15ULL) ^ inv;
        __asm__ volatile("dsb sy" : : : "memory");
        for (uint64_t i = 0; i < n; ++i)
            if (m[i] != ((i * 0x9e3779b97f4a7c15ULL) ^ inv))
                ++errors;
    }
    uint64_t ms = (ticks() - t0) * 1000U / freq();
    if (errors)
        row_dec(ROW_BODY + 3, "FAIL: BAD WORDS", errors, "");
    else
        star_fb_text_c(ROW_BODY + 3, "PASS: NO ERRORS", C_GREEN, C_BLACK);
    row_dec(ROW_BODY + 4, "TIME", ms, " MS");
    row_dec(ROW_BODY + 5, "SPEED", ms ? (RAMTEST_SIZE * 4 / 1024 / 1024) * 1000 / ms : 0, " MIB/S (MMU OFF)");
    wait_select();
}

static void diag_timer(void)
{
    page("TIMER TEST", HINT_BACK);
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    row_dec(ROW_BODY, "CNTFRQ_EL0", f, f ? " HZ" : " (26 MHZ ASSUMED)");
    star_fb_text_c(ROW_BODY + 1, "START A STOPWATCH WITH THE COUNT:", C_MUTED, C_BLACK);
    uint64_t t0 = ticks(), hz = freq();
    for (unsigned s = 0; s <= 10; ++s) {
        while (ticks() - t0 < hz * s)
            ;
        row_dec(ROW_BODY + 3, "SECONDS", s, "");
    }
    star_fb_text_c(ROW_BODY + 5, "10 S HERE = 10 S ON THE STOPWATCH?", C_MUTED, C_BLACK);
    row_hex(ROW_BODY + 6, "CNTPCT", ticks(), 16);
    wait_select();
}

static void diagnostics(const struct star_info *in, const char *cmdline)
{
    unsigned sel = 0;
    for (;;) {
        page("DIAGNOSTICS", HINT_MENU);
        item(0, "SYSTEM INFO", C_WHITE);
        item(1, "DISPLAY COLOUR TEST", C_WHITE);
        item(2, "BUTTON TEST", C_WHITE);
        item(3, "PMIC / TOUCH RAILS", C_WHITE);
        item(4, "RAM TEST (16 MIB)", C_WHITE);
        item(5, "TIMER TEST (10 S)", C_WHITE);
        item(6, "BACK", C_MUTED);
        sel = choose(ROW_BODY, 7, sel);
        if (sel == 0) diag_sysinfo(in, cmdline);
        else if (sel == 1) diag_display();
        else if (sel == 2) diag_buttons();
        else if (sel == 3) diag_pmic();
        else if (sel == 4) diag_ram();
        else if (sel == 5) diag_timer();
        else return;
    }
}

static void label(unsigned i, const char *name, const char *value, uint32_t fg)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, name);
    sb_s(&s, value);
    item(i, s.b, fg);
}

static void boot_options(void)
{
    unsigned sel = 0;
    for (;;) {
        page("BOOT OPTIONS", "VOL: MOVE   POWER: CHANGE");
        label(0, "CPU CORES       ", opt.cpus == 4 ? "4" : opt.cpus == 2 ? "2" : "1", C_WHITE);
        label(1, "ROOT FILESYSTEM ", opt.rw && !opt.multi ? "READ-WRITE" : "READ-ONLY", C_WHITE);
        label(2, "VERBOSE BOOT    ", opt.verbose ? "ON" : "OFF", C_WHITE);
        label(3, "TOUCHSCREEN     ", opt.touch ? "ON" : "OFF", C_WHITE);
        label(4, "TOUCH POWER-ON  ", opt.touchpwr ? "ON" : "OFF", C_WHITE);
        item(5, "BACK", C_MUTED);
        star_fb_text_c(ROW_BODY + 7, "APPLIED WHEN YOU PICK", C_MUTED, C_BLACK);
        star_fb_text_c(ROW_BODY + 8, "BOOT IOS WITH THESE SETTINGS.", C_MUTED, C_BLACK);
        if (opt.multi)
            star_fb_text_c(ROW_BODY + 9, "UFS MULTI-BLOCK FORCES READ-ONLY.", C_GOLD, C_BLACK);
        sel = choose(ROW_BODY, 6, sel);
        if (sel == 0) opt.cpus = opt.cpus == 4 ? 1 : opt.cpus == 1 ? 2 : 4;
        else if (sel == 1) opt.rw = !opt.rw;
        else if (sel == 2) opt.verbose = !opt.verbose;
        else if (sel == 3) opt.touch = !opt.touch;
        else if (sel == 4) opt.touchpwr = !opt.touchpwr;
        else return;
    }
}

/* Second press of Power arms a risky item; Volume cancels. */
static int confirm(const char *what)
{
    star_fb_text_c(ROW_BODY + 9, what, C_GOLD, C_BLACK);
    star_fb_text_c(ROW_BODY + 10, "POWER AGAIN: DO IT   VOL: CANCEL", C_RED, C_BLACK);
    int yes = (key_wait() & K_SELECT) != 0;
    star_fb_text(ROW_BODY + 9, "");
    star_fb_text(ROW_BODY + 10, "");
    return yes;
}

/* The kernel's touch-rail sequence, with the same voltage checks. */
static void risky_rails(void)
{
    page("TOUCH RAILS ON NOW (PMIC WRITE)", HINT_BACK);
    uint8_t l35 = 0, l43 = 0;
    int ok35 = pmic_read(1, 0x6c, &l35), ok43 = pmic_read(1, 0x74, &l43);
    star_fb_text_c(ROW_BODY, "BEFORE:", C_MUTED, C_BLACK);
    row_ldo(ROW_BODY + 1, "LDO35", ok35, l35, 700);
    row_ldo(ROW_BODY + 2, "LDO43", ok43, l43, 1800);
    if (!ok35 || !ok43 || ((l35 & 0x3fU) != 0x2cU && (l35 & 0x3fU) != 0) ||
        ((l43 & 0x3fU) != 0 && ((l43 & 0x3fU) < 0x28U || (l43 & 0x3fU) > 0x3cU))) {
        star_fb_text_c(ROW_BODY + 4, "UNEXPECTED VOLTAGE: NOT WRITING", C_RED, C_BLACK);
        wait_select();
        return;
    }
    int ok = pmic_write(1, 0x6c, 0xec) && pmic_write(1, 0x74, 0xf0);
    ok35 = pmic_read(1, 0x6c, &l35);
    ok43 = pmic_read(1, 0x74, &l43);
    star_fb_text_c(ROW_BODY + 4, ok ? "AFTER (WRITE OK):" : "AFTER (WRITE FAILED):",
                   ok ? C_GREEN : C_RED, C_BLACK);
    row_ldo(ROW_BODY + 5, "LDO35", ok35, l35, 700);
    row_ldo(ROW_BODY + 6, "LDO43", ok43, l43, 1800);
    wait_select();
}

static void risky(void)
{
    unsigned sel = 0;
    for (;;) {
        page("RISKY EXPERIMENTS", HINT_MENU);
        label(0, "UFS MULTI-BLOCK I/O  ", opt.multi ? "[ARMED]" : "OFF", opt.multi ? C_RED : C_WHITE);
        label(1, "CPU CORE PROBE       ", opt.smpprobe ? "[ARMED]" : "OFF", opt.smpprobe ? C_RED : C_WHITE);
        item(2, "TOUCH RAILS ON NOW (PMIC)", C_WHITE);
        item(3, "BACK", C_MUTED);
        star_fb_text_c(ROW_BODY + 6, "ARMED ITEMS APPLY TO THE NEXT BOOT", C_MUTED, C_BLACK);
        star_fb_text_c(ROW_BODY + 7, "FROM BOOT IOS WITH THESE SETTINGS.", C_MUTED, C_BLACK);
        sel = choose(ROW_BODY, 4, sel);
        if (sel == 0) {
            if (opt.multi)
                opt.multi = 0;
            else if (confirm("MAY PANIC. ROOT GOES READ-ONLY."))
                opt.multi = 1;
        } else if (sel == 1) {
            if (opt.smpprobe)
                opt.smpprobe = 0;
            else if (confirm("PARKS CORES: REBOOT AFTER IT."))
                opt.smpprobe = 1;
        } else if (sel == 2) {
            if (confirm("WRITES LDO35 + LDO43 NOW."))
                risky_rails();
        } else {
            return;
        }
    }
}

/* Returns 1 to boot iOS, 0 for the boot menu. */
static int engineer(const struct star_info *in, char *cmdline, unsigned len)
{
    unsigned sel = 0;
    star_fb_row_base(0);
    for (;;) {
        page("MAIN MENU", HINT_MENU);
        item(0, "DIAGNOSTICS", C_WHITE);
        item(1, "BOOT OPTIONS", C_WHITE);
        item(2, "RISKY EXPERIMENTS", C_GOLD);
        item(3, "BOOT IOS WITH THESE SETTINGS", C_GREEN);
        item(4, "BACK TO BOOT MENU", C_MUTED);
        item(5, "REBOOT TO RECOVERY (TWRP)", C_MUTED);
        item(6, "REBOOT TO DOWNLOAD MODE", C_MUTED);
        sel = choose(ROW_BODY, 7, sel);
        if (sel == 0) {
            diagnostics(in, cmdline);
        } else if (sel == 1) {
            boot_options();
        } else if (sel == 2) {
            risky();
        } else if (sel == 3) {
            opt_apply(cmdline, len);
            return 1;
        } else if (sel == 4) {
            return 0;
        } else {
            star_fb_fill(C_BLACK);
            star_fb_text(ROW_BODY, sel == 5 ? "REBOOTING TO RECOVERY" : "REBOOTING TO DOWNLOAD MODE");
            reboot(sel == 5 ? INFORM_RECOVERY : INFORM_DOWNLOAD);
        }
    }
}

/* ---- boot menu -------------------------------------------------------- */

/* In the gap under Samsung's logo: overlay row_base 23, rows 0-4
 * (no room left for a key hint: Volume moves, Power selects). */
#define MENU_ROW_BASE 23U

/* ---- boot menu v5 (sprites around Samsung's logo) --------------------- */

/*
 * tools/brand/menu.mjs: items 0 iOS, 1 Windows, 2 Android and 3 Linux (shown
 * dimmed: not installed), 4 Recovery chip, 5 an unused card slot,
 * 6 Download chip. Sprite id 16 + 8 * item + state; state
 * 0 normal, 1 selected, 2 starting, 3 disabled, 4 selected but inactive.
 *
 * Two selectors, as in the user's concept:
 *   OS list   Volume Up/Down over iOS, Windows, Android, Linux;
 *   service   a short Bixby press moves here (Recovery | Download, side by
 *             side), further presses flip between the two; Volume goes back
 *             to the OS list.
 * Power starts whatever has focus. Holding Bixby (0.7 s) opens Starbite
 * Engineer and nothing else; the whole screen (Samsung's logo included) is
 * saved first and put back after. Touch: tap an entry to focus it, tap it
 * again to start it (hit boxes = the sprites' own rectangles).
 */
#define V5_SPRITE(item, st) (16U + 8U * (item) + (st))
#define V5_CARD 5U
#define V5_RECOVERY 4U
#define V5_DOWNLOAD 6U
#define V5_HOLD_MS 700U
#define K_BIXBY_HOLD 16U
#define K_TAP 32U
#define K_VOL_HOLD 64U      /* both volume keys held V5_HOLD_MS: power off */
/* Free at menu time: the payload must end below RAM_BASE (0x92000000), the
 * Engineer RAM test uses 0x92000000 + 16 MiB, and nothing is copied anywhere
 * until a choice is made (the woa FD lands at 0x90000000 only after). 17 MiB. */
#define SCREEN_SAVE 0x94000000UL

static const unsigned char v5_os[5] = { 2, 0, 3, 1, V5_CARD };   /* top to bottom: Android, iOS, Linux, Windows, card */
static const unsigned char v5_svc[2] = { V5_RECOVERY, V5_DOWNLOAD };

static void screen_copy(uintptr_t dst, uintptr_t src)
{
    volatile uint64_t *d = (volatile uint64_t *)dst;
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    for (uint64_t i = 0; i < (uint64_t)STAR_FB_WIDTH * STAR_FB_HEIGHT / 2; ++i)
        d[i] = s[i];
}

/* One debounced press or a tap; Bixby held for V5_HOLD_MS reports
 * K_BIXBY_HOLD at once (the next wait lets go of it first), and so do both
 * volume keys held together (K_VOL_HOLD). */
static unsigned input_event(int *tx, int *ty)
{
    while (keys())
        wait_ms(20);
    for (;;) {
        unsigned k = keys();
        if (k) {
            wait_ms(40);
            if (keys() != k)
                continue;
            if (k == K_BIXBY) {
                for (unsigned t = 40; t < V5_HOLD_MS; t += 10) {
                    if (!(keys() & K_BIXBY))
                        return K_BIXBY;
                    wait_ms(10);
                }
                return K_BIXBY_HOLD;
            }
            unsigned both = 0, now;
            while ((now = keys())) {
                both = (now & (K_UP | K_DOWN)) == (K_UP | K_DOWN) ? both + 20U : 0;
                if (both >= V5_HOLD_MS)
                    return K_VOL_HOLD;
                wait_ms(20);
            }
            return k;
        }
        if (tp_tap(tx, ty))
            return K_TAP;
        wait_ms(10);
    }
}

/* The OS rows show the catalog entries with these keys (catalog.h; menu v6 draws the whole
 * catalog), selectable when every file they need is there (star_entry_ready). */
static int v5_cat[4], v5_ready[4];

static int v5_avail(unsigned item)
{
    /* Item 5 is an unused (blank) card slot. */
    return item < 4 ? v5_ready[item] : item != V5_CARD;
}

static void v5_lookup(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        v5_cat[i] = cat_find(i == 0 ? "ios26" : i == 1 ? "win11" : i == 2 ? "android" : "linux");
        v5_ready[i] = v5_cat[i] >= 0 && star_entry_ready((unsigned)v5_cat[i], 0);
    }
}

/* Row of catalog entry idx, or V5_CARD when it has none. */
static unsigned v5_item_of(int idx)
{
    for (unsigned i = 0; i < 4; ++i)
        if (v5_cat[i] == idx)
            return i;
    return V5_CARD;
}

static unsigned v5_step(unsigned os, int dir)
{
    unsigned at = os;
    for (unsigned n = 0; n < 5; ++n) {
        at = (at + 5U + (unsigned)dir) % 5U;
        if (v5_avail(v5_os[at]))
            return at;
    }
    return os;                              /* nothing installed */
}

/* Index of v5_os holding item (4 = the card's place when it has none). */
static unsigned v5_index(unsigned item)
{
    unsigned i = 0;
    while (i < 4 && v5_os[i] != item)
        ++i;
    return i;
}

/* Item under a tap (state-0 sprite rectangle), or -1. Rows whose OS is not installed (v5_avail)
 * do not count. */
static int v5_hit(int x, int y)
{
    static const unsigned char items[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (unsigned i = 0; i < 8; ++i) {
        if (!v5_avail(items[i]))
            continue;
        unsigned rx, ry, rw, rh;
        if (star_sprite_rect(16U + 8U * items[i], &rx, &ry, &rw, &rh) &&
            x >= (int)rx && x < (int)(rx + rw) && y >= (int)ry && y < (int)(ry + rh))
            return items[i];
    }
    return -1;
}

struct v5 { unsigned svc_focus, os, svc; };


/* Last drawn state per item (0xff = not drawn yet): only changes are drawn,
 * the card slot alone is ~0.9 Mpx. */
static unsigned char v5_shown[9] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void v5_put(unsigned item, unsigned st)
{
    if (v5_shown[item] != st) {
        (void)star_sprite(V5_SPRITE(item, st));
        v5_shown[item] = (unsigned char)st;
    }
}

static void v5_draw(const struct v5 *m)
{
    for (unsigned i = 0; i < 5; ++i)
        v5_put(v5_os[i], v5_os[i] == V5_CARD ? 0U : !v5_avail(v5_os[i]) ? 3U : i != m->os ? 0U : m->svc_focus ? 4U : 1U);
    v5_put(7, 0);
    v5_put(8, 0);
    for (unsigned i = 0; i < 2; ++i)
        v5_put(v5_svc[i], m->svc_focus && i == m->svc ? 1U : 0U);
}

/* The rows, chips and hints come in animated (anim.c), in the states v5_draw is about to give
 * them; v5_draw then draws them exactly (v5_shown is still empty). */
void star_intro_rows(const unsigned *ids, unsigned n);
static void v5_intro(const struct v5 *m)
{
    unsigned ids[8], n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned it = v5_os[i];
        ids[n++] = V5_SPRITE(it, !v5_avail(it) ? 3U : i != m->os ? 0U : m->svc_focus ? 4U : 1U);
    }
    for (unsigned i = 0; i < 2; ++i)
        ids[n++] = V5_SPRITE(v5_svc[i], m->svc_focus && i == m->svc ? 1U : 0U);
    ids[n++] = V5_SPRITE(7, 0);
    ids[n++] = V5_SPRITE(8, 0);
    star_intro_rows(ids, n);
}

/* The chosen entry says "starting"; the card leaves unless it was the pick,
 * and the loader's status lines go where it was. */
static void v5_starting(unsigned item)
{
    unsigned x, y, w, h;
    v5_put(item, 2);
    if (item != V5_CARD && star_sprite_rect(V5_SPRITE(V5_CARD, 0), &x, &y, &w, &h)) {
        star_fb_fill_rows(y, y + h, C_BLACK);
        star_fb_row_base((y + 59U - 200U) / 60U);   /* fbtext TOP 200, rows 60 px */
    }
    if (tp_ok)
        (void)tp_cmd(0x62);                         /* hand over an empty event stack */
}

/* ---- settings, boot-next, coming back after a failed start ---------------
 * The menu remembers the last choice and honours a one-shot "boot X next" (store.h settings
 * record, written by the loader and by tools/store/settings.py; or the RAM mailbox at 0xFED17FE0,
 * above the pmsg ring, which survives warm resets: u64 "SBNEXT2!" + char key[24], written by any
 * OS; the older u64 "SBNEXTLX" from Linux's reboot-linux means "linux"). A next entry from the
 * settings record is only started once clearing it was written back, so it cannot loop.
 * settings.timeout_s > 0 starts settings.default_key after that many seconds unless a key is
 * pressed (0, the default: wait).
 */
#define NEXT_MAILBOX 0xFED17FE0UL
#define NEXT2_MAGIC  0x21325458454e4253ULL   /* "SBNEXT2!" */
#define NEXTLX_MAGIC 0x584c5458454e4253ULL   /* "SBNEXTLX" */
/* The screen as it was just before a start, put back if the start fails (16.3 MiB, free WB RAM
 * between anim.c's buffers and the menu assets at 0x89000000; no OS is loaded there). */
#define MENU_SAVE 0x86000000UL

static struct star_settings settings;
static int settings_ok, menu_resumed;
static const char *fail_name, *fail_why;
static struct v5 v5_m;

void star_menu_failed(const char *name, const char *why)
{
    fail_name = name;
    fail_why = why;
    menu_resumed = 1;
}

static int key_eq(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 23 && a[k] && a[k] == b[k])
        ++k;
    return k == 23 || a[k] == b[k];
}

static void key_copy(char *dst, const char *src)
{
    unsigned k = 0;
    for (; k < 23 && src[k]; ++k)
        dst[k] = src[k];
    for (; k < 24; ++k)
        dst[k] = 0;
}

static int ready_key(const char *key)
{
    int i = key[0] ? cat_find(key) : -1;
    return i >= 0 && star_entry_ready((unsigned)i, 0) ? i : -1;
}

/* A one-shot next entry: catalog index or -1. */
static int boot_next(void)
{
    volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
    char key[24];
    key[0] = 0;
    if (mb[0] == NEXT2_MAGIC) {
        for (unsigned k = 0; k < 23; ++k)
            key[k] = *(volatile char *)(NEXT_MAILBOX + 8 + k);
        key[23] = 0;
    } else if (mb[0] == NEXTLX_MAGIC) {
        key_copy(key, "linux");
    }
    if (mb[0] == NEXT2_MAGIC || mb[0] == NEXTLX_MAGIC) {
        for (unsigned i = 0; i < 4; ++i)
            mb[i] = 0;
        __asm__ volatile("dsb sy" : : : "memory");
    }
    if (!key[0] && settings_ok && settings.next_key[0]) {
        key_copy(key, settings.next_key);
        settings.next_key[0] = 0;
        if (settings_save(&settings))
            key[0] = 0;
    }
    return ready_key(key);
}

/* Remember the choice (one block write, only when it changed; the first one creates the record). */
static void remember(int idx)
{
    const struct cat_ent *e = idx >= 0 ? cat_get((unsigned)idx) : 0;
    if (!e || key_eq(settings.last_key, e->key))
        return;
    key_copy(settings.last_key, e->key);
    (void)settings_save(&settings);
}

static void upper(char *dst, unsigned cap, const char *src)
{
    unsigned k = 0;
    for (; src && src[k] && k + 1 < cap; ++k)
        dst[k] = src[k] >= 'a' && src[k] <= 'z' ? (char)(src[k] - 32) : src[k];
    dst[k] = 0;
}

/* Start catalog entry idx from the menu: its row says "starting", the screen before that is
 * kept for a failed start. */
static int v5_go(struct v5 *m, int idx)
{
    unsigned item = v5_item_of(idx);
    if (item != V5_CARD) {
        m->svc_focus = 0;
        m->os = v5_index(item);
    }
    v5_draw(m);
    screen_copy(MENU_SAVE, STAR_FB_BASE);
    v5_starting(item);
    remember(idx);
    return idx + 1;
}

static int menu_v5(const struct star_info *in, char *cmdline, unsigned len)
{
    struct v5 *mp = &v5_m;
    if (menu_resumed) {
        /* back from a start that failed: the screen as it was, and why */
        char b[STAR_FB_COLS + 1];
        menu_resumed = 0;
        screen_copy(STAR_FB_BASE, MENU_SAVE);
        star_fb_row_base(36);
        upper(b, sizeof(b), "COULD NOT START ");
        unsigned n = 0;
        while (b[n])
            ++n;
        upper(b + n, sizeof(b) - n, fail_name);
        star_fb_text_c(0, b, C_RED, C_BLACK);
        upper(b, sizeof(b), fail_why ? fail_why : "UNKNOWN ERROR");
        star_fb_text_c(1, b, C_MUTED, C_BLACK);
    } else {
        v5_lookup();
        settings_ok = settings_load(&settings);
        mp->svc_focus = mp->svc = 0;
        mp->os = 0;
        while (mp->os < 4 && !v5_avail(v5_os[mp->os]))   /* start on the first installed OS... */
            ++mp->os;
        int last = ready_key(settings.last_key);         /* ...or the last choice */
        if (last < 0)
            last = ready_key(settings.default_key);
        if (last >= 0 && v5_item_of(last) != V5_CARD)
            mp->os = v5_index(v5_item_of(last));
        if (mp->os >= 4)
            mp->os = 0;
        int next = boot_next();
        if (next >= 0)
            return v5_go(mp, next);
        v5_intro(mp);
        v5_draw(mp);
        tp_init();
        int dflt = settings_ok && settings.timeout_s ? ready_key(settings.default_key) : -1;
        if (dflt >= 0) {
            char b[STAR_FB_COLS + 1];
            const struct cat_ent *e = cat_get((unsigned)dflt);
            star_fb_row_base(36);
            upper(b, sizeof(b), e->name);
            star_fb_text_c(0, b, C_WHITE, C_BLACK);
            star_fb_text_c(1, "STARTS SOON: PRESS ANY KEY TO STOP", C_MUTED, C_BLACK);
            unsigned stop = 0;
            for (unsigned t = 0; t < settings.timeout_s * 100U && !stop; ++t) {
                int tx, ty;
                if (keys() || tp_tap(&tx, &ty))
                    stop = 1;
                wait_ms(10);
            }
            star_fb_text_c(0, "", C_WHITE, C_BLACK);
            star_fb_text_c(1, "", C_WHITE, C_BLACK);
            if (!stop)
                return v5_go(mp, dflt);
        }
    }
    struct v5 m = *mp;
    for (;;) {
        *mp = m;
        int tx = 0, ty = 0;
        unsigned k = input_event(&tx, &ty);
        if (k == K_TAP) {
            int hit = v5_hit(tx, ty);
            if (hit < 0)
                continue;
            if (hit == 7)
                k = K_BIXBY_HOLD;                   /* the Engineer hint */
        }
        if (k == K_TAP) {
            int hit = v5_hit(tx, ty);
            int svc = hit == (int)V5_RECOVERY || hit == (int)V5_DOWNLOAD;
            unsigned idx = svc ? (unsigned)(hit == (int)V5_DOWNLOAD) : v5_index((unsigned)hit);
            if ((unsigned)svc == m.svc_focus && idx == (svc ? m.svc : m.os)) {
                k = K_POWER;                        /* second tap starts it */
            } else {
                m.svc_focus = (unsigned)svc;
                if (svc)
                    m.svc = idx;
                else
                    m.os = idx;
                v5_draw(&m);
                continue;
            }
        }
        if (k == K_VOL_HOLD) {
            star_fb_fill(C_BLACK);
            star_fb_row_base(22);
            star_fb_text_c(0, "POWERING OFF", C_WHITE, C_BLACK);
            hw_power_off();
            star_fb_text_c(1, "STILL ON: CHARGER CONNECTED?", C_MUTED, C_BLACK);
            star_fb_text_c(2, "REBOOTING", C_MUTED, C_BLACK);
            wait_ms(1500);
            reboot(INFORM_NORMAL);
        }
        if (k == K_BIXBY_HOLD) {
            screen_copy(SCREEN_SAVE, STAR_FB_BASE);
            uint64_t fs, fe;
            int boot = star_font_blob(&fs, &fe) ? em_main(in, cmdline, len) : engineer(in, cmdline, len) ? star_ios_pick() : 0;
            screen_copy(STAR_FB_BASE, SCREEN_SAVE);
            star_fb_row_base(36);
            if (boot > 0) {                         /* a catalog entry, as from the menu */
                *mp = m;
                return v5_go(mp, boot - 1);
            }
            continue;
        }
        if (k & K_POWER) {
            unsigned item = m.svc_focus ? v5_svc[m.svc] : v5_os[m.os];
            if (item < 4 || item == V5_CARD) {
                if (!v5_avail(item))
                    continue;
                *mp = m;
                return v5_go(mp, v5_cat[item]);
            }
            v5_starting(item);
            reboot(item == V5_DOWNLOAD ? INFORM_DOWNLOAD : INFORM_RECOVERY);
        }
        if (k & K_BIXBY) {
            if (m.svc_focus)
                m.svc ^= 1U;
            else
                m.svc_focus = 1;
        } else if (k & (K_UP | K_DOWN)) {
            if (m.svc_focus)
                m.svc_focus = 0;
            else
                m.os = v5_step(m.os, (k & K_UP) ? -1 : 1);
        } else {
            continue;
        }
        v5_draw(&m);
    }
}

/* ---- shared with menu v6 (menu6.c, menu.h) ---- */

struct star_settings *star_menu_settings(int *valid)
{
    if (valid)
        *valid = settings_ok;
    return &settings;
}

int star_menu_settings_save(void)
{
    int r = settings_save(&settings);
    if (!r)
        settings_ok = 1;
    return r;
}

int star_menu_ready_key(const char *key)
{
    return ready_key(key);
}

void star_menu_begin(void)
{
    settings_ok = settings_load(&settings);
}

int star_menu_next(void)
{
    return boot_next();
}

void star_menu_remember(int idx)
{
    remember(idx);
}

int star_menu_failure(const char **name, const char **why)
{
    if (!menu_resumed)
        return 0;
    menu_resumed = 0;
    *name = fail_name;
    *why = fail_why;
    return 1;
}

/* The catalog entry of iOS from BOOT (what Engineer's "boot iOS" means): index + 1, or 0. */
int star_ios_pick(void)
{
    int i = cat_find("ios26");
    if (i >= 0 && star_entry_ready((unsigned)i, 0))
        return i + 1;
    for (unsigned j = 0; j < cat_count(); ++j) {
        const struct cat_ent *e = cat_get(j);
        if (e->method == M_XNU && e->file[0][0] == '@' && star_entry_ready(j, 0))
            return (int)j + 1;
    }
    return 0;
}

/* Menu v7 (menu7.c): the default when the BOOT ramdisk has its assets. It runs from S-Boot's
 * splash (star_splash_draw left it on the panel); if it cannot start (nothing drawn), v6 gets the
 * intro it would have had and runs, now and after every failed start. */
void star_intro_splash(void);   /* anim.c */
static int v7_active, v7_failed;

void star_menu_progress(unsigned stage, unsigned n, const char *title, const char *detail)
{
    if (v7_active)
        menu7_progress(stage, n, title, detail);
}

void star_menu_handoff(const char *detail)
{
    if (v7_active)
        menu7_handoff(detail);
}

int star_menu(const struct star_info *in, char *cmdline, unsigned len)
{
    unsigned sel = 0;
    opt_load(cmdline);
    if (!v7_failed && menu7_ready()) {
        int r = menu7_main(in, cmdline, len);
        if (r >= 0) {
            m7_report("pick");
            v7_active = 1;
            return r;
        }
        v7_failed = 1;
        v7_active = 0;
        star_intro_splash();
        star_fb_row_base(36);
    }
    unsigned ix, iy, iw, ih;
    if (star_menu_assets() > 0 && star_sprite_rect(V6_ICON_SPRITE, &ix, &iy, &iw, &ih) && ui_init())
        return menu_v6(in, cmdline, len);
    if (star_menu_assets() > 0)
        return menu_v5(in, cmdline, len);
    /* No menu sprites: the text menu (iOS and Windows from the catalog). */
    int ios = star_ios_pick(), win = cat_find("win11");
    for (;;) {
        star_fb_row_base(MENU_ROW_BASE);
        if (menu_resumed) {
            menu_resumed = 0;
            star_fb_text_c(4, fail_why ? fail_why : "COULD NOT START", C_RED, C_BLACK);
        }
        item(0, "IOS 26", ios ? C_WHITE : C_MUTED);
        item(1, "WINDOWS 11", win >= 0 && star_entry_ready((unsigned)win, 0) ? C_WHITE : C_MUTED);
        item(2, "RECOVERY (TWRP)", C_WHITE);
        item(3, "STARBITE ENGINEER", C_GOLD);
        sel = choose(0, 4, sel);
        if (sel == 0 && ios) {
            break;
        } else if (sel == 1 && win >= 0 && star_entry_ready((unsigned)win, 0)) {
            star_fb_text_c(sel, "> STARTING WINDOWS 11", C_WHITE, C_BLUE);
            return win + 1;
        } else if (sel == 2) {
            star_fb_text(sel, "> REBOOTING TO RECOVERY");
            reboot(INFORM_RECOVERY);
        } else if (sel == 3) {
            int boot = engineer(in, cmdline, len);
            star_fb_fill(C_BLACK);
            star_overlay_redraw();
            if (boot && ios)
                break;
        }
    }
    star_fb_row_base(MENU_ROW_BASE);
    for (unsigned r = 0; r < 5; ++r)
        star_fb_text(r, "");
    /* The card slot belongs to the boot selector only: clear it
     * (the band above the footer) before iOS boots. */
    star_fb_fill_rows(1880, 2500, C_BLACK);
    return ios;
}

/* ---- exported helpers (hw.h) ------------------------------------------- */

uint64_t hw_ticks(void) { return ticks(); }
uint64_t hw_freq(void) { return freq(); }
void hw_wait_us(uint64_t us) { wait_us(us); }
void hw_wait_ms(unsigned ms) { wait_ms(ms); }
unsigned hw_keys(void) { return keys(); }
void hw_reboot(uint32_t inform3) { reboot(inform3); }

/* PS_HOLD low (exynos-reboot.c exynos_power_off: PMU +0x330C bit 8). With a
 * cable in, the PMIC keeps the phone on: returns after 3 s, the caller
 * reboots. */
void hw_power_off(void)
{
    if (tp_ok)
        (void)tp_cmd(0x62);
    while (keys())
        wait_ms(20);
    wait_ms(300);
    mwr(PMU_BASE + 0x330c, mrd(PMU_BASE + 0x330c) & ~0x100U);
    wait_ms(3000);
}
int hw_pmic_read(uint32_t type, uint32_t reg, uint8_t *val) { return pmic_read(type, reg, val); }
int hw_pmic_write(uint32_t type, uint32_t reg, uint8_t val) { return pmic_write(type, reg, val); }
void hw_opt_load(const char *cmdline) { opt_load(cmdline); }
void hw_opt_apply(char *cmdline, unsigned len) { opt_apply(cmdline, len); }
int hw_tp_ok(void) { return tp_ok; }
void hw_tp_start(void) { if (!tp_ok) tp_init(); }
int hw_tp_event(int *st, int *x, int *y) { return tp_event(st, x, y); }
int hw_tp_read(uint8_t reg, uint8_t *data, unsigned len) { return tp_ok && tp_read(reg, data, len, 5); }
void hw_tp_max(unsigned *mx, unsigned *my) { *mx = tp_max_x; *my = tp_max_y; }
void hw_tp_clear(void) { if (tp_ok) (void)tp_cmd(0x62); }

int hw_acpm_build(char out[25])
{
    if (!acpm_find(ACPM_PMIC_CH)) {
        out[0] = 0;
        return 0;
    }
    for (unsigned i = 0; i < 24; i += 4) {
        uint32_t w = ard(ACPM_INITDATA + 0x60 + i);
        for (unsigned b = 0; b < 4; ++b) {
            char c = (char)(w >> (8 * b));
            out[i + b] = (c >= 32 && c < 127) ? c : '.';
        }
    }
    out[24] = 0;
    return 1;
}

/* microSD rails for Linux (stock DT: vmmc = S2MPS18 LDO31 2.8 V, vqmmc =
 * LDO2 1.7-3.3 V; both 1.8 V + 25 mV steps, CTRL 0x68 / 0x4A). LDO31 has been
 * found at vsel 0x30 (3.0 V, off) after a forced reset, so any setting in
 * the SD card's VDD range (2.7-3.3 V, vsel 0x24-0x3c) or 0 is accepted; the
 * card is power-cycled (rails off, 20 ms, then LDO31 2.8 V and LDO2 3.3 V on)
 * so a card left mid-transfer by a reset starts from idle. Enable bits [7:6].
 * Same return values as hw_touch_rails. */
int hw_sd_rails(uint8_t *l31, uint8_t *l2)
{
    if (!pmic_read(1, 0x68, l31) || !pmic_read(1, 0x4a, l2))
        return -1;
    unsigned v = *l31 & 0x3fU;
    if (v != 0 && (v < 0x24U || v > 0x3cU))
        return 0;
    int ok = pmic_write(1, 0x68, 0x28) && pmic_write(1, 0x4a, (uint8_t)(*l2 & 0x3fU));
    wait_ms(20);
    ok = ok && pmic_write(1, 0x68, 0xe8) && pmic_write(1, 0x4a, 0xfc);
    wait_ms(5);
    (void)pmic_read(1, 0x68, l31);
    (void)pmic_read(1, 0x4a, l2);
    return ok ? 1 : -1;
}

int hw_touch_rails(uint8_t *l35, uint8_t *l43)
{
    if (!pmic_read(1, 0x6c, l35) || !pmic_read(1, 0x74, l43))
        return -1;
    if (((*l35 & 0x3fU) != 0x2cU && (*l35 & 0x3fU) != 0) ||
        ((*l43 & 0x3fU) != 0 && ((*l43 & 0x3fU) < 0x28U || (*l43 & 0x3fU) > 0x3cU)))
        return 0;
    int ok = pmic_write(1, 0x6c, 0xec) && pmic_write(1, 0x74, 0xf0);
    (void)pmic_read(1, 0x6c, l35);
    (void)pmic_read(1, 0x74, l43);
    return ok ? 1 : -1;
}
