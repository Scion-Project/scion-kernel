/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Recovery: hold Bixby early in boot (docs/STARBITE-RECOVERY.md).
 *
 * The path up to the Bixby check uses only what cannot fail: the ALIVE GPIO data register of the
 * keys (GPA0 0x14050024 bit 6 Bixby, bits 3/4 Volume Up/Down, GPA2 0x14050064 bit 4 Power, all
 * active low, set up by S-Boot), the generic timer (CNTPCT; every wait also stops after a fixed
 * number of iterations, so a stopped counter cannot hang it) and CPU writes to the framebuffer
 * (fbtext.c; S-Boot lit the panel, star_fb_enable set the DECON trigger). No UFS, store, catalog,
 * PMIC, ACPM, I2C or touch until the user picks an item that needs one.
 *
 * Volume Up / Down move, Power (or Bixby, once released) selects. Reboots use the PMU INFORM
 * registers the way the vendor kernel does (hw.h). Rescue mode writes the boot-next mailbox
 * ("SBNEXT2!" + "rescue") and resets; the loader takes it right after the Bixby window and starts
 * the rescue system without the menu (xnu_boot.c star_rescue_boot).
 *
 * RAM used (the loader's free XNU window, mmu.c non-cacheable, unused until an OS is loaded):
 *   REC_PANEL_SAVE 0x9C000000  the panel on entry (1440 x 2960 x 4 = 16.3 MiB), put back on Continue
 *   REC_HINT_SAVE  0x9E800000  the Bixby hint's row (345 KiB)
 *   post.c uses 0x9E000000 (its rows) and 0x8C000000 = STAGE (the RAM pattern; never 0xA0000000, the ECT).
 * No pointer tables: the Image must have no absolute relocations.
 */
#include "recovery.h"
#include "fbtext.h"
#include "hw.h"
#include "catalog.h"
#include "post.h"

#define ALIVE_GPA0_DAT 0x14050024UL
#define ALIVE_GPA2_DAT 0x14050064UL
#define NEXT_MAILBOX   0xFED17FE0UL
#define NEXT2_MAGIC    0x21325458454e4253ULL   /* "SBNEXT2!" (menu.c) */
#define REC_PANEL_SAVE 0x9C000000UL
#define REC_HINT_SAVE  0x9E800000UL
#define REC_LOG        ((volatile uint64_t *)0x91fff000UL)   /* xnu_boot.S's log page */
#define REC_LOG_SLOT   216U

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BLUE  0xff2a5bf0U
#define C_MUTED 0xffb1bad3U
#define C_GOLD  0xffffd43bU
#define C_RED   0xffff4d4dU
#define C_GREEN 0xff3ddc84U

/* ---- time and keys: nothing else ------------------------------------------------------------ */

static uint64_t r_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t r_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 26000000U;   /* S-Boot leaves CNTFRQ 0; the timer is 26 MHz */
}

/* Waits us microseconds, or about 200 loop turns per microsecond if the counter stops. */
static void r_wait_us(uint64_t us)
{
    uint64_t t0 = r_ticks(), n = r_freq() / 1000000U * us, guard = us * 200U + 1000U;
    while (r_ticks() - t0 < n && --guard)
        ;
}

static void r_wait_ms(unsigned ms)
{
    r_wait_us((uint64_t)ms * 1000U);
}

static unsigned r_keys(void)
{
    uint32_t a = *(volatile uint32_t *)ALIVE_GPA0_DAT;
    uint32_t b = *(volatile uint32_t *)ALIVE_GPA2_DAT;
    return (!((a >> 3) & 1U) ? K_UP : 0) | (!((a >> 4) & 1U) ? K_DOWN : 0) |
           (!((a >> 6) & 1U) ? K_BIXBY : 0) | (!((b >> 4) & 1U) ? K_POWER : 0);
}

static void r_release(void)
{
    while (r_keys())
        r_wait_ms(20);
}

/* One debounced press (reported while the key is still down; the next call waits for it to go). */
static unsigned r_key_wait(void)
{
    r_release();
    for (;;) {
        unsigned k = r_keys();
        if (k) {
            r_wait_ms(30);
            if (r_keys() == k)
                return k;
        }
        r_wait_ms(10);
    }
}

int rec_bixby_now(void)
{
    if (!(r_keys() & K_BIXBY))
        return 0;
    r_wait_ms(5);
    return (r_keys() & K_BIXBY) != 0;
}

/* ---- panel rows ----------------------------------------------------------------------------- */

/* Both ends non-cacheable (mmu.c): eight loads before their stores, so the in-order core keeps
 * several reads in flight instead of waiting for each. */
static void copy_words(volatile uint64_t *d, volatile const uint64_t *s, uint64_t n)
{
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];
        uint64_t f = s[i + 4], g = s[i + 5], h = s[i + 6], k = s[i + 7];
        d[i] = a;
        d[i + 1] = b;
        d[i + 2] = c;
        d[i + 3] = e;
        d[i + 4] = f;
        d[i + 5] = g;
        d[i + 6] = h;
        d[i + 7] = k;
    }
    for (; i < n; ++i)
        d[i] = s[i];
    __asm__ volatile("dsb sy" : : : "memory");
}

void rec_rows_save(unsigned y0, unsigned y1, uintptr_t buf)
{
    if (y1 > STAR_FB_HEIGHT)
        y1 = STAR_FB_HEIGHT;
    if (y0 >= y1)
        return;
    copy_words((volatile uint64_t *)buf,
               (volatile const uint64_t *)(STAR_FB_BASE + (uint64_t)y0 * STAR_FB_WIDTH * 4U),
               (uint64_t)(y1 - y0) * STAR_FB_WIDTH / 2U);
}

void rec_rows_restore(unsigned y0, unsigned y1, uintptr_t buf)
{
    if (y1 > STAR_FB_HEIGHT)
        y1 = STAR_FB_HEIGHT;
    if (y0 >= y1)
        return;
    copy_words((volatile uint64_t *)(STAR_FB_BASE + (uint64_t)y0 * STAR_FB_WIDTH * 4U),
               (volatile const uint64_t *)buf, (uint64_t)(y1 - y0) * STAR_FB_WIDTH / 2U);
}

unsigned rec_row_base(void)
{
    unsigned now = star_fb_row_y(0), cell = star_fb_row_y(1) - now;
    star_fb_row_base(0);
    unsigned top = star_fb_row_y(0);
    unsigned base = cell ? (now - top) / cell : 0;
    star_fb_row_base(base);
    return base;
}

/* ---- the Bixby window ----------------------------------------------------------------------- */

int rec_bixby_window(unsigned ms)
{
    unsigned base = rec_row_base();
    star_fb_row_base(0);
    unsigned y0 = star_fb_row_y(0), y1 = star_fb_row_y(1);
    rec_rows_save(y0, y1, REC_HINT_SAVE);
    star_fb_text_c(0, "HOLD BIXBY: STARBITE RECOVERY", C_MUTED, C_BLACK);
    int held = 0;
    unsigned run = 0, steps = ms / 10U + 1U;
    for (unsigned t = 0; t < steps && !held; ++t) {
        if (r_keys() & K_BIXBY) {
            if (++run >= 3)             /* 3 reads 10 ms apart: about 30 ms held */
                held = 1;
        } else {
            run = 0;
        }
        if (!held)
            r_wait_ms(10);
    }
    rec_rows_restore(y0, y1, REC_HINT_SAVE);
    star_fb_row_base(base);
    return held;
}

/* ---- the rescue request (boot-next mailbox) ------------------------------------------------- */

static int mb_is_rescue(void)
{
    static const char want[7] = "rescue";
    if (*(volatile uint64_t *)NEXT_MAILBOX != NEXT2_MAGIC)
        return 0;
    for (unsigned k = 0; k < 7; ++k)
        if (*(volatile char *)(NEXT_MAILBOX + 8 + k) != want[k])
            return 0;
    return 1;
}

int rec_rescue_take(void)
{
    int r = mb_is_rescue();
    if (r) {
        volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
        for (unsigned i = 0; i < 4; ++i)
            mb[i] = 0;
        __asm__ volatile("dsb sy" : : : "memory");
    }
    return r;
}

/* The key first, the magic last (as tools/linux/reboot-to), then a normal reset. */
static void rescue_reboot(void)
{
    static const char key[8] = "rescue";
    volatile uint64_t *mb = (volatile uint64_t *)NEXT_MAILBOX;
    mb[0] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    for (unsigned k = 0; k < 24; ++k)
        *(volatile char *)(NEXT_MAILBOX + 8 + k) = k < 8 ? key[k] : 0;
    __asm__ volatile("dsb sy" : : : "memory");
    mb[0] = NEXT2_MAGIC;
    __asm__ volatile("dsb sy" : : : "memory");
    star_pmsg("STARBITE: recovery: rescue mode requested, rebooting\n");
    r_wait_ms(300);
    hw_reboot(INFORM_NORMAL);
}

/* ---- text ----------------------------------------------------------------------------------- */

#define ROW_TITLE 0U
#define ROW_SUB   1U
#define ROW_ITEM0 3U
#define ROW_MSG   13U
#define ROW_BODY  16U
#define ROW_HINT  44U
#define N_ITEMS   10U

#define IT_CONTINUE 0U
#define IT_RESCUE   1U
#define IT_ENGINEER 2U
#define IT_POST     3U
#define IT_TWRP     4U
#define IT_DOWNLOAD 5U
#define IT_REBOOT   6U
#define IT_POWEROFF 7U
#define IT_FASTBOOT 8U
#define IT_ENTRY    9U

static const char *item_name(unsigned i)
{
    switch (i) {
    case IT_CONTINUE: return "CONTINUE NORMAL BOOT";
    case IT_RESCUE:   return "STARBITE RESCUE MODE";
    case IT_ENGINEER: return "ENGINEER LITE";
    case IT_POST:     return "RUN POST (FULL)";
    case IT_TWRP:     return "REBOOT TO TWRP";
    case IT_DOWNLOAD: return "REBOOT TO DOWNLOAD MODE";
    case IT_REBOOT:   return "REBOOT";
    case IT_POWEROFF: return "POWER OFF";
    case IT_FASTBOOT: return "USB FASTBOOT / CONSOLE";
    default:          return "BOOT ENTRY...";
    }
}

struct line { char b[STAR_FB_COLS + 1]; unsigned n; };

static void ln_init(struct line *l)
{
    l->n = 0;
    l->b[0] = 0;
}

static void ln_s(struct line *l, const char *s)
{
    while (s && *s && l->n < STAR_FB_COLS)
        l->b[l->n++] = *s++;
    l->b[l->n] = 0;
}

static void ln_u(struct line *l, uint64_t v)
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

static void ln_pad(struct line *l, unsigned col)
{
    while (l->n < col && l->n < STAR_FB_COLS)
        l->b[l->n++] = ' ';
    l->b[l->n] = 0;
}

/* A message over two rows (38 columns each); the rest is cut. */
static void msg(const char *a, const char *b, uint32_t fg)
{
    char t[2 * STAR_FB_COLS + 1];
    unsigned n = 0;
    for (const char *p = a; p && *p && n < 2 * STAR_FB_COLS; ++p)
        t[n++] = *p;
    for (const char *p = b; p && *p && n < 2 * STAR_FB_COLS; ++p)
        t[n++] = *p;
    t[n] = 0;
    char second[STAR_FB_COLS + 1];
    unsigned k = 0;
    if (n > STAR_FB_COLS) {
        for (unsigned i = STAR_FB_COLS; i < n; ++i)
            second[k++] = t[i];
        t[STAR_FB_COLS] = 0;
    }
    second[k] = 0;
    star_fb_text_c(ROW_MSG, t, fg, C_BLACK);
    star_fb_text_c(ROW_MSG + 1, second, fg, C_BLACK);
}

static void clear_rows(unsigned r0, unsigned r1)
{
    for (unsigned r = r0; r < r1; ++r)
        star_fb_text_c(r, "", C_WHITE, C_BLACK);
}

static void draw_item(unsigned i, int sel)
{
    struct line l;
    ln_init(&l);
    ln_s(&l, sel ? "> " : "  ");
    ln_s(&l, item_name(i));
    uint32_t fg = i == IT_RESCUE ? C_GOLD : C_WHITE;
    star_fb_text_c(ROW_ITEM0 + i, l.b, sel ? C_WHITE : fg, sel ? C_BLUE : C_BLACK);
}

static void post_summary(void)
{
    const struct post_report *p = post_last();
    struct line l;
    if (!p) {
        star_fb_text_c(ROW_BODY, "POST: NOT RUN THIS BOOT", C_MUTED, C_BLACK);
        return;
    }
    ln_init(&l);
    ln_s(&l, "LAST POST: ");
    ln_u(&l, post_ok_count(p));
    ln_s(&l, " OK ");
    ln_u(&l, p->warns);
    ln_s(&l, " WARN ");
    ln_u(&l, p->fails);
    ln_s(&l, " FAILED");
    star_fb_text_c(ROW_BODY, l.b, p->fails ? C_RED : p->warns ? C_GOLD : C_GREEN, C_BLACK);
    unsigned r = ROW_BODY + 1;
    for (unsigned i = 0; i < p->n && r < ROW_HINT - 1; ++i) {
        const struct post_check *c = &p->c[i];
        if (c->state != POST_FAIL && c->state != POST_WARN)
            continue;
        ln_init(&l);
        ln_s(&l, c->state == POST_FAIL ? FBT_CROSS " " : "! ");
        ln_s(&l, c->name);
        ln_pad(&l, 11);
        ln_s(&l, c->detail);
        star_fb_text_c(r++, l.b, c->state == POST_FAIL ? C_RED : C_GOLD, C_BLACK);
    }
}

static void draw_menu(unsigned sel, const char *why)
{
    star_fb_mute(0);
    star_fb_row_base(0);
    star_fb_fill(C_BLACK);
    star_fb_text_c(ROW_TITLE, "STARBITE RECOVERY", C_GOLD, C_BLACK);
    star_fb_text_c(ROW_SUB, "GALAXY S9 - BIXBY WAS HELD AT BOOT", C_MUTED, C_BLACK);
    for (unsigned i = 0; i < N_ITEMS; ++i)
        draw_item(i, i == sel);
    if (why)
        msg(why, 0, C_RED);
    post_summary();
    star_fb_text_c(ROW_HINT, "VOL UP/DOWN: MOVE   POWER: SELECT", C_MUTED, C_BLACK);
}

/* ---- the catalog's entries (UFS: only once the user asked for them) ------------------------- */

#define LIST_ROW0 3U
#define LIST_MAX  38U   /* rows 3..40 */

static void draw_entry(unsigned i, unsigned n, int sel)
{
    struct line l;
    ln_init(&l);
    ln_s(&l, sel ? "> " : "  ");
    uint32_t fg = C_WHITE;
    if (i == n) {
        ln_s(&l, "BACK");
    } else {
        const struct cat_ent *e = cat_get(i);
        ln_s(&l, e->key);
        ln_pad(&l, 18);
        ln_s(&l, e->name);
        if (!star_entry_ready(i, 0))
            fg = C_MUTED;
        else if (e->flags & CF_HIDDEN)
            fg = C_GOLD;
    }
    star_fb_text_c(LIST_ROW0 + i, l.b, sel ? C_WHITE : fg, sel ? C_BLUE : C_BLACK);
}

/* A catalog index, or -1 for Back. */
static int pick_entry(void)
{
    star_fb_fill(C_BLACK);
    star_fb_text_c(ROW_TITLE, "STARBITE RECOVERY: BOOT ENTRY", C_GOLD, C_BLACK);
    star_fb_text_c(ROW_SUB, "READING THE CATALOG (UFS)...", C_MUTED, C_BLACK);
    (void)cat_load();
    unsigned n = cat_count();
    if (n > LIST_MAX - 1)
        n = LIST_MAX - 1;
    struct line l;
    ln_init(&l);
    ln_u(&l, n);
    ln_s(&l, cat_builtin() ? " ENTRIES (BUILT-IN CATALOG)" : " ENTRIES (STORE CATALOG)");
    star_fb_text_c(ROW_SUB, l.b, C_MUTED, C_BLACK);
    unsigned sel = 0;
    for (unsigned i = 0; i <= n; ++i)
        draw_entry(i, n, i == sel);
    star_fb_text_c(ROW_HINT - 1, "GREY: FILES MISSING  GOLD: HIDDEN", C_MUTED, C_BLACK);
    star_fb_text_c(ROW_HINT, "VOL UP/DOWN: MOVE   POWER: START", C_MUTED, C_BLACK);
    for (;;) {
        unsigned k = r_key_wait();
        unsigned old = sel;
        if (k & K_UP)
            sel = sel ? sel - 1 : n;
        else if (k & K_DOWN)
            sel = sel < n ? sel + 1 : 0;
        else if (k & (K_POWER | K_BIXBY)) {
            if (sel == n)
                return -1;
            const char *why = 0;
            if (star_entry_ready(sel, &why))
                return (int)sel;
            star_fb_text_c(LIST_ROW0 + n + 2, why ? why : "NOT READY", C_RED, C_BLACK);
            continue;
        }
        if (old != sel) {
            draw_entry(old, n, 0);
            draw_entry(sel, n, 1);
        }
    }
}

/* ---- the menu ------------------------------------------------------------------------------- */

static int panel_saved, panel_valid;

void rec_panel_lost(void)
{
    panel_valid = 0;
}

static void rec_log(unsigned item)
{
    static unsigned count;
    REC_LOG[REC_LOG_SLOT] = 0x52454356ULL << 32 | (uint64_t)(++count & 0xffffU) << 16 | item;
}

static void leave(void)
{
    r_release();
    if (panel_valid)
        rec_rows_restore(0, STAR_FB_HEIGHT, REC_PANEL_SAVE);
    else
        star_fb_fill(C_BLACK);
}

int recovery_main(const char *why, int *pick)
{
    unsigned base = rec_row_base();
    if (!panel_saved) {
        rec_rows_save(0, STAR_FB_HEIGHT, REC_PANEL_SAVE);
        panel_saved = panel_valid = 1;
    }
    star_pmsg("STARBITE: Starbite Recovery menu\n");
    rec_log(0xffffU);
    unsigned sel = 0;
    draw_menu(sel, why);
    for (;;) {
        unsigned k = r_key_wait();
        unsigned old = sel;
        if (k & K_UP) {
            sel = sel ? sel - 1 : N_ITEMS - 1;
        } else if (k & K_DOWN) {
            sel = sel + 1 < N_ITEMS ? sel + 1 : 0;
        } else if (k & (K_POWER | K_BIXBY)) {
            rec_log(sel);
            switch (sel) {
            case IT_CONTINUE:
                star_pmsg("STARBITE: recovery: continue\n");
                leave();
                star_fb_row_base(base);
                return REC_CONTINUE;
            case IT_RESCUE: {
                const char *w = 0;
                msg("LOOKING FOR THE RESCUE SYSTEM...", 0, C_MUTED);
                if (!star_rescue_ready(&w)) {
                    msg("NO RESCUE SYSTEM: ", w ? w : "NOT FOUND", C_RED);
                    break;
                }
                msg("REBOOTING INTO STARBITE RESCUE MODE", 0, C_GREEN);
                rescue_reboot();
                break;
            }
            case IT_ENGINEER: {
                char *cmd;
                unsigned len;
                msg("STARTING ENGINEER LITE...", 0, C_MUTED);
                const struct star_info *in = star_boot_setup(&cmd, &len);
                uint64_t fs, fe;
                if (!star_font_blob(&fs, &fe)) {
                    msg("ENGINEER LITE NEEDS THE MENU ASSETS", " (FONT ATLAS) IN BOOT", C_RED);
                    break;
                }
                r_release();
                int r = em_main(in, cmd, len);
                if (r > 0) {
                    *pick = r - 1;
                    r_release();
                    panel_valid = 0;
                    star_fb_row_base(base);
                    return REC_BOOT;
                }
                draw_menu(sel, 0);
                break;
            }
            case IT_POST: {
                clear_rows(ROW_MSG, ROW_HINT);
                msg("RUNNING THE FULL POST...", 0, C_MUTED);
                int bix = 0;
                (void)post_run(POST_FULL, ROW_BODY, 1, &bix);
                const struct post_report *p = post_last();
                struct line l;
                ln_init(&l);
                ln_s(&l, "POST DONE: ");
                ln_u(&l, p ? p->fails : 0);
                ln_s(&l, " FAILED, ");
                ln_u(&l, p ? p->warns : 0);
                ln_s(&l, " WARN");
                msg(l.b, 0, p && p->fails ? C_RED : p && p->warns ? C_GOLD : C_GREEN);
                star_fb_text_c(ROW_HINT, "PRESS ANY KEY", C_MUTED, C_BLACK);
                (void)r_key_wait();
                draw_menu(sel, 0);
                break;
            }
            case IT_TWRP:
                msg("REBOOTING TO TWRP", 0, C_GREEN);
                star_pmsg("STARBITE: recovery: reboot to TWRP\n");
                r_wait_ms(200);
                hw_reboot(INFORM_RECOVERY);
                break;
            case IT_DOWNLOAD:
                msg("REBOOTING TO DOWNLOAD MODE", 0, C_GREEN);
                star_pmsg("STARBITE: recovery: reboot to Download mode\n");
                r_wait_ms(200);
                hw_reboot(INFORM_DOWNLOAD);
                break;
            case IT_REBOOT:
                msg("REBOOTING", 0, C_GREEN);
                star_pmsg("STARBITE: recovery: reboot\n");
                r_wait_ms(200);
                hw_reboot(INFORM_NORMAL);
                break;
            case IT_FASTBOOT:
                star_pmsg("STARBITE: recovery: USB fastboot\n");
                leave();
                star_fb_row_base(base);
                return REC_FASTBOOT;
            case IT_POWEROFF:
                msg("POWERING OFF", 0, C_GREEN);
                star_pmsg("STARBITE: recovery: power off\n");
                hw_power_off();
                msg("STILL ON: IS A USB CABLE CONNECTED?", 0, C_RED);
                break;
            default: {
                int e = pick_entry();
                if (e >= 0) {
                    *pick = e;
                    r_release();
                    panel_valid = 0;
                    star_fb_row_base(base);
                    return REC_BOOT;
                }
                draw_menu(sel, 0);
                break;
            }
            }
            continue;
        }
        if (old != sel) {
            draw_item(old, 0);
            draw_item(sel, 1);
        }
    }
}
