/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v6 (Starbite 2, phase 2): the whole screen, drawn at run time from the catalog.
 *
 *   header      starbite wordmark, model                      (menu sprite 0, from the intro)
 *   tabs        Apple · Microsoft · Android · Linux · Starbite · Other · Lab (empty ones hidden,
 *               except the first four and Lab)
 *   list        one card per catalog entry of the category: icon, name, version line (or why it
 *               cannot start), tag chip, DEFAULT chip
 *   bar         Recovery · Download · Lab · Power off
 *   status      what happened (a failed start, a countdown)
 *   hints, footer
 *
 * Keys: Volume moves (down past the last card into the bar), Power starts / runs, Power held opens
 * the card's options, Bixby switches category, Bixby held opens Lab (EngineerMode), both volume
 * keys held power off. Touch: tap a tab; tap a card or button to select it, tap it again to start;
 * swipe left / right for the next / previous category; long-press a card or tap its chevron for
 * its options.
 *
 * Options are dropdowns anchored to what opened them (a card, the Power button): Start now, the
 * default entry switch, Auto-start (a nested choice: Off, 3, 5, 10 s). In a dropdown Volume moves,
 * Power picks, Bixby closes (or goes back from the nested one); a tap picks, a tap outside closes.
 *
 * Frames are composed in CANVAS (write-back RAM, mmu.c) with ui.c drawing into it, and the rows
 * that changed are copied to the panel. Labels come from the catalog, icons are menu sprites
 * 96 + ICON_* (V6=1 tools/brand/menu.mjs, alpha-blended by ui_sprite), text is the Outfit atlas (tools/brand/em-font.py).
 * No pointer tables: everything is filled at run time.
 */
#include <stdint.h>
#include "menu.h"
#include "fbtext.h"
#include "hw.h"
#include "ui.h"
#include "catalog.h"
#include "store.h"

#define W UI_W
#define H UI_H
/* After anim.c's buffers (0x84000000..0x85c00000) and menu.c's MENU_SAVE (0x86000000, 16.3 MiB),
 * below the menu assets at 0x89000000. */
#define CANVAS ((uint32_t *)0x87100000UL)

#define X0 64
#define CW (W - 2 * X0)
#define TAB_Y 262
#define TAB_H 84
#define LIST_Y 400
#define CARD_H 216
#define CARD_STRIDE 240
#define LIST_ROWS 7
#define LIST_END (LIST_Y + LIST_ROWS * CARD_STRIDE)
#define BAR_Y 2120
#define BAR_H 176
#define BAR_GAP 32
#define BAR_W ((CW - 3 * BAR_GAP) / 4)
#define STATUS_Y 2400
#define HINT_Y 2700
#define FOOTER_Y 2790

#define BG 0xff000000U
#define INK 0xffeef1f6U
#define DIM 0xff8a93a3U
#define FAINT 0xff4a5160U
#define LINE 0xff2c313bU
#define CARD 0xff0e131cU
#define CARD_SEL 0xff111c2aU
#define ACCENT 0xff6fb3ffU
#define AMBER 0xffffb547U
#define GREEN 0xff3ddc84U
#define PURPLE 0xffb48cffU
#define RED 0xffff6b6bU

#define ICON_BASE 96U
#define ICON_RECOVERY 16U
#define ICON_DOWNLOAD 17U
#define ICON_LAB 18U
#define ICON_POWER 19U
#define ICON_PLAY 20U           /* dropdown glyphs: 48 px masks, tinted */
#define ICON_STAR 21U
#define ICON_TIMER 22U
#define ICON_CHECK 23U
#define ICON_CHEV_R 24U
#define ICON_CHEV_D 25U
#define ICON_PWR 26U
#define ICON_RESTART 27U
#define ICON_CHEV_L 28U

/* The dropdown glyphs (ICON_PLAY..ICON_CHEV_L) came with the dropdowns: a BOOT ramdisk from
 * before them has the v6 icons but not these. Without them the options button shows three dots,
 * dropdown rows drop their icon column and a choice shows a dot for its check mark. */
static int glyphs_state;            /* 0 not looked yet, 1 there, 2 missing */

static int glyphs(void)
{
    if (!glyphs_state) {
        unsigned x, y, w, h;
        glyphs_state = star_sprite_rect(ICON_BASE + ICON_CHEV_D, &x, &y, &w, &h) ? 1 : 2;
    }
    return glyphs_state == 1;
}

/* the card's options button (a chevron in a disc at its right edge) */
#define MORE_D 84
#define MORE_X (X0 + CW - 34 - MORE_D)

#define NCAT 7U
#define EV_TAP 0x100U
#define EV_LONG 0x200U
#define EV_LEFT 0x400U
#define EV_RIGHT 0x800U
#define EV_BIXBY_HOLD 0x1000U
#define EV_VOL_HOLD 0x2000U
#define EV_POWER_HOLD 0x4000U
#define HOLD_MS 700U
#define LONG_MS 600U

int em_main(const struct star_info *in, char *cmdline, unsigned len);

static struct {
    unsigned tab;                   /* index into tabs[] */
    int zone;                       /* 0 the list, 1 the bar */
    unsigned sel[NCAT];             /* selected card per tab */
    unsigned bar;
    unsigned top;                   /* first card shown */
    int inited;
} M;

static unsigned tabs[NCAT], ntabs;        /* categories shown, in order */
static int tab_x[NCAT], tab_w[NCAT];
static unsigned list[CAT_MAX], nlist;     /* catalog indices of the current tab */
static char status1[96], status2[96];
static uint32_t status_c;

/* ---- text helpers -------------------------------------------------------------------------- */

static void scopy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    for (; s && s[k] && k + 1 < cap; ++k)
        d[k] = s[k];
    d[k] = 0;
}

static void scat(char *d, unsigned cap, const char *s)
{
    unsigned n = 0;
    while (n + 1 < cap && d[n])
        ++n;
    scopy(d + n, cap - n, s);
}

/* "NO KERNEL" -> "No kernel" (the loader's reasons are upper case for fbtext). */
static void sentence(char *d, unsigned cap, const char *s)
{
    scopy(d, cap, s);
    for (unsigned k = 1; d[k]; ++k)
        if (d[k] >= 'A' && d[k] <= 'Z')
            d[k] = (char)(d[k] + 32);
}

static int key_eq(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 23 && a[k] && a[k] == b[k])
        ++k;
    return k == 23 || a[k] == b[k];
}

static const char *cat_name(unsigned c)
{
    switch (c) {
    case CAT_APPLE: return "Apple";
    case CAT_MICROSOFT: return "Microsoft";
    case CAT_ANDROID: return "Android";
    case CAT_LINUX: return "Linux";
    case CAT_STARBITE: return "Starbite";
    case CAT_OTHER: return "Other";
    default: return "Lab";
    }
}

static const char *tag_name(unsigned t, uint32_t *c)
{
    switch (t) {
    case TAG_DAILY: *c = GREEN; return "DAILY";
    case TAG_BETA: *c = ACCENT; return "BETA";
    case TAG_EXPERIMENTAL: *c = AMBER; return "EXPERIMENTAL";
    case TAG_LAB: *c = PURPLE; return "LAB";
    case TAG_STABLE: *c = GREEN; return "STABLE";
    default: *c = DIM; return 0;
    }
}

static unsigned icon_of(const struct cat_ent *e)
{
    if (e->icon)
        return e->icon;
    switch (e->category) {
    case CAT_APPLE: return 1;
    case CAT_MICROSOFT: return 2;
    case CAT_ANDROID: return 3;
    case CAT_LINUX: return 4;
    case CAT_STARBITE: return 5;
    case CAT_LAB: return 7;
    default: return 6;
    }
}

/* ---- the catalog by tab -------------------------------------------------------------------- */

static unsigned count_in(unsigned c)
{
    unsigned n = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *e = cat_get(i);
        n += e->category == c && !(e->flags & CF_HIDDEN);
    }
    return n;
}

static void make_tabs(void)
{
    ntabs = 0;
    for (unsigned c = 0; c < NCAT; ++c)
        if (c <= CAT_LINUX || c == CAT_LAB || count_in(c))
            tabs[ntabs++] = c;
}

/* nlist / list[] for tab t, by the entries' order field. */
static void make_list(unsigned t)
{
    nlist = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *e = cat_get(i);
        if (e->category != tabs[t] || (e->flags & CF_HIDDEN))
            continue;
        unsigned k = nlist++;
        while (k && cat_get(list[k - 1])->order > e->order) {
            list[k] = list[k - 1];
            --k;
        }
        list[k] = i;
    }
    if (M.sel[t] >= nlist)
        M.sel[t] = 0;
}

static unsigned tab_of(unsigned category)
{
    for (unsigned t = 0; t < ntabs; ++t)
        if (tabs[t] == category)
            return t;
    return 0;
}

/* Focus catalog entry idx (its tab, its card). */
static void focus_entry(int idx)
{
    const struct cat_ent *e = idx >= 0 ? cat_get((unsigned)idx) : 0;
    if (!e)
        return;
    M.tab = tab_of(e->category);
    make_list(M.tab);
    for (unsigned k = 0; k < nlist; ++k)
        if (list[k] == (unsigned)idx)
            M.sel[M.tab] = k;
    M.zone = 0;
}

/* ---- drawing (into CANVAS) ------------------------------------------------------------------ */

static void present_rows(int y0, int y1)
{
    if (y0 < 0)
        y0 = 0;
    if (y1 > H)
        y1 = H;
    volatile uint64_t *fb = (volatile uint64_t *)STAR_FB_BASE;
    const uint64_t *s = (const uint64_t *)CANVAS;
    for (uint64_t i = (uint64_t)y0 * W / 2; i < (uint64_t)y1 * W / 2; ++i)
        fb[i] = s[i];
    __asm__ volatile("dsb st" : : : "memory");
}

static void chip(int right, int y, const char *s, uint32_t c)
{
    int w = ui_text_w(F_CHIP, s) + 44;
    ui_round(right - w, y, w, 52, 26, (c & 0x00ffffffU) | 0x2e000000U);
    ui_text(F_CHIP, right - w + 22, y + 36, s, c, UI_BEHIND);
}

static int chip_w(const char *s)
{
    return ui_text_w(F_CHIP, s) + 44;
}

static void draw_tabs(void)
{
    ui_fill(0, TAB_Y - 8, W, TAB_H + 16, BG);
    int x = X0;
    for (unsigned t = 0; t < ntabs; ++t) {
        const char *s = cat_name(tabs[t]);
        int w = ui_text_w(F_META, s) + 68;
        tab_x[t] = x;
        tab_w[t] = w;
        int on = t == M.tab;
        if (on)
            ui_round(x, TAB_Y, w, TAB_H, TAB_H / 2, (ACCENT & 0x00ffffffU) | 0x2a000000U);
        ui_text(F_META, x + 34, TAB_Y + 56, s, on ? ACCENT : DIM, UI_BEHIND);
        x += w + 10;
    }
}

/* st: 0 normal, 1 selected, 2 starting, 3 selected while the bar has focus, 4 its options open */
static void draw_card(unsigned slot, unsigned idx, int st)
{
    const struct cat_ent *e = cat_get(idx);
    const char *why;
    int ready = star_entry_ready(idx, &why);
    int y = LIST_Y + (int)slot * CARD_STRIDE;
    char b[96];
    ui_fill(X0 - 4, y - 4, CW + 8, CARD_H + 8, BG);
    int sel = st == 1 || st == 2 || st == 4;
    ui_round(X0, y, CW, CARD_H, 40, sel ? ACCENT : st == 3 ? 0xff3a4c66U : LINE);
    ui_round(X0 + 3, y + 3, CW - 6, CARD_H - 6, 37, sel || st == 3 ? CARD_SEL : CARD);
    (void)ui_sprite(ICON_BASE + icon_of(e), X0 + 48, y + (CARD_H - 112) / 2, ready ? 256U : 90U, 0);
    ui_text(F_NAME, X0 + 200, y + 100, e->name, ready ? INK : FAINT, UI_BEHIND);
    if (st == 2)
        ui_text(F_META, X0 + 200, y + 166, "starting" UI_ELLIPSIS, ACCENT, UI_BEHIND);
    else if (!ready) {
        sentence(b, sizeof(b), why);
        scat(b, sizeof(b), " " UI_DOT " not installed");
        ui_text(F_META, X0 + 200, y + 166, b, FAINT, UI_BEHIND);
    } else
        ui_text(F_META, X0 + 200, y + 166, e->version, DIM, UI_BEHIND);
    /* the options button, then the chips left of it, all on the card's centre line */
    int cy = y + CARD_H / 2;
    ui_round(MORE_X, cy - MORE_D / 2, MORE_D, MORE_D, MORE_D / 2,
             st == 4 ? (ACCENT & 0x00ffffffU) | 0x30000000U : 0x12ffffffU);
    if (glyphs())
        (void)ui_sprite(ICON_BASE + ICON_CHEV_D, MORE_X + (MORE_D - 48) / 2, cy - 24 + 2, 256, st == 4 ? ACCENT : DIM);
    else
        for (int k = -1; k <= 1; ++k)
            ui_round(MORE_X + MORE_D / 2 + k * 20 - 5, cy - 5, 10, 10, 5, st == 4 ? ACCENT : DIM);
    uint32_t tc;
    const char *tag = tag_name(e->tag, &tc);
    int right = MORE_X - 24;
    if (tag) {
        chip(right, cy - 26, tag, ready ? tc : FAINT);
        right -= chip_w(tag) + 12;
    }
    int valid;
    struct star_settings *s = star_menu_settings(&valid);
    if (s->default_key[0] && key_eq(s->default_key, e->key))
        chip(right, cy - 26, "DEFAULT", INK);
}

static void draw_list(void)
{
    ui_fill(0, LIST_Y - 8, W, LIST_END - LIST_Y + 8, BG);
    make_list(M.tab);
    unsigned sel = M.sel[M.tab];
    if (sel < M.top)
        M.top = sel;
    if (sel >= M.top + LIST_ROWS)
        M.top = sel - LIST_ROWS + 1;
    if (M.top + LIST_ROWS > nlist)
        M.top = nlist > LIST_ROWS ? nlist - LIST_ROWS : 0;
    if (!nlist) {
        ui_text_c(F_NAME, W / 2, LIST_Y + 300, "Nothing here yet", DIM, UI_BEHIND);
        ui_text_c(F_META, W / 2, LIST_Y + 380, "Add an entry to tools/store/catalog.toml", FAINT, UI_BEHIND);
        return;
    }
    for (unsigned k = M.top; k < nlist && k < M.top + LIST_ROWS; ++k)
        draw_card(k - M.top, list[k], k != sel ? 0 : M.zone ? 3 : 1);
    if (M.top + LIST_ROWS < nlist)
        ui_text_c(F_HINT, W / 2, LIST_END - 6, UI_DOWN " more", DIM, UI_BEHIND);
}

static void draw_bar_button(unsigned i, int on)
{
    int x = X0 + (int)i * (BAR_W + BAR_GAP);
    ui_fill(x - 4, BAR_Y - 4, BAR_W + 8, BAR_H + 8, BG);
    ui_round(x, BAR_Y, BAR_W, BAR_H, 36, on ? ACCENT : LINE);
    ui_round(x + 3, BAR_Y + 3, BAR_W - 6, BAR_H - 6, 33, on ? CARD_SEL : CARD);
    (void)ui_sprite(ICON_BASE + ICON_RECOVERY + i, x + (BAR_W - 64) / 2, BAR_Y + 30, 256, on ? INK : DIM);
    const char *s = i == 0 ? "Recovery" : i == 1 ? "Download" : i == 2 ? "Lab" : "Power";
    ui_text_c(F_META, x + BAR_W / 2, BAR_Y + 148, s, on ? INK : DIM, UI_BEHIND);
}

static void draw_bar(void)
{
    ui_fill(0, BAR_Y - 8, W, BAR_H + 16, BG);
    for (unsigned i = 0; i < 4; ++i)
        draw_bar_button(i, M.zone == 1 && M.bar == i);
}

static void draw_status(void)
{
    ui_fill(0, STATUS_Y - 60, W, 180, BG);
    if (status1[0])
        ui_text_c(F_META, W / 2, STATUS_Y, status1, status_c, UI_BEHIND);
    if (status2[0])
        ui_text_c(F_HINT, W / 2, STATUS_Y + 60, status2, DIM, UI_BEHIND);
}

/* mode: 0 the menu, 1 a dropdown, 2 a nested dropdown */
static void draw_hints(int mode)
{
    ui_fill(0, HINT_Y - 50, W, 80, BG);
    ui_text_c(F_HINT, W / 2, HINT_Y,
              mode == 0 ? "Volume move  " UI_DOT "  Power start  " UI_DOT "  hold Power options  " UI_DOT
                          "  Bixby category"
              : mode == 1 ? "Volume move  " UI_DOT "  Power choose  " UI_DOT "  Bixby close"
                          : "Volume move  " UI_DOT "  Power choose  " UI_DOT "  Bixby back",
              FAINT, UI_BEHIND);
}

static void set_status(const char *a, const char *b, uint32_t c)
{
    scopy(status1, sizeof(status1), a);
    scopy(status2, sizeof(status2), b);
    status_c = c;
}

static void compose(void)
{
    ui_target(CANVAS);
    ui_clip(0, H);
    ui_fill(0, 0, W, H, BG);
    (void)star_sprite_to(0, CANVAS, 0, 0, 0, 256);     /* header + footer */
    draw_tabs();
    draw_list();
    draw_bar();
    draw_status();
    draw_hints(0);
    ui_target(0);
}

/* Redraw what a focus change touches and put it on the panel. */
static void refresh(int tabs_too)
{
    ui_target(CANVAS);
    if (tabs_too)
        draw_tabs();
    draw_list();
    draw_bar();
    draw_status();
    ui_target(0);
    present_rows(tabs_too ? TAB_Y - 8 : LIST_Y - 8, STATUS_Y + 130);
}

/* ---- input ----------------------------------------------------------------------------------- */

static int tp_is_down, tp_x0, tp_y0, tp_lx, tp_ly, tp_long_done;
static uint64_t tp_t0;

static int ms_since(uint64_t t0)
{
    return (int)((hw_ticks() - t0) * 1000U / hw_freq());
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* A key (K_* bits, debounced), EV_BIXBY_HOLD / EV_VOL_HOLD, or a touch event with *x, *y. With
 * timeout_ms, 0 when nothing happened in that time. */
static unsigned input(int *x, int *y, unsigned timeout_ms)
{
    uint64_t t_in = hw_ticks();
    while (hw_keys()) {
        hw_wait_ms(20);
        if (timeout_ms && ms_since(t_in) > (int)timeout_ms)
            return 0;
    }
    for (;;) {
        unsigned k = hw_keys();
        if (k) {
            hw_wait_ms(40);
            if (hw_keys() != k)
                continue;
            if (k == K_BIXBY) {
                for (unsigned t = 40; t < HOLD_MS; t += 10) {
                    if (!(hw_keys() & K_BIXBY))
                        return K_BIXBY;
                    hw_wait_ms(10);
                }
                return EV_BIXBY_HOLD;
            }
            unsigned both = 0, power = 40, now;
            while ((now = hw_keys())) {
                both = (now & (K_UP | K_DOWN)) == (K_UP | K_DOWN) ? both + 20U : 0;
                if (both >= HOLD_MS)
                    return EV_VOL_HOLD;
                power = now == K_POWER && k == K_POWER ? power + 20U : 0;
                if (power >= LONG_MS)
                    return EV_POWER_HOLD;           /* while still held: the next input() waits */
                hw_wait_ms(20);
            }
            return k;
        }
        int st, ex, ey;
        while (hw_tp_event(&st, &ex, &ey)) {
            if (st == 1) {
                tp_is_down = 1;
                tp_x0 = tp_lx = ex;
                tp_y0 = tp_ly = ey;
                tp_t0 = hw_ticks();
                tp_long_done = 0;
            } else if (st == 2 && tp_is_down) {
                tp_lx = ex;
                tp_ly = ey;
            } else if (st == 3 && tp_is_down) {
                tp_is_down = 0;
                if (tp_long_done)
                    continue;
                int dx = ex - tp_x0, dy = ey - tp_y0;
                if (iabs(dx) < 80 && iabs(dy) < 80) {
                    *x = ex;
                    *y = ey;
                    return EV_TAP;
                }
                if (iabs(dx) > 220 && iabs(dx) > 2 * iabs(dy))
                    return dx < 0 ? EV_LEFT : EV_RIGHT;
            }
        }
        if (tp_is_down && !tp_long_done && ms_since(tp_t0) > (int)LONG_MS &&
            iabs(tp_lx - tp_x0) < 60 && iabs(tp_ly - tp_y0) < 60) {
            tp_long_done = 1;
            *x = tp_x0;
            *y = tp_y0;
            return EV_LONG;
        }
        if (timeout_ms && ms_since(t_in) > (int)timeout_ms)
            return 0;
        hw_wait_ms(10);
    }
}

/* What is under (x, y): 1 tab (*i), 2 card (*i = list position), 3 bar button, 4 a card's options
 * button (*i = list position), 0 nothing. */
static int hit(int x, int y, unsigned *i)
{
    if (y >= TAB_Y && y < TAB_Y + TAB_H)
        for (unsigned t = 0; t < ntabs; ++t)
            if (x >= tab_x[t] && x < tab_x[t] + tab_w[t]) {
                *i = t;
                return 1;
            }
    if (x >= X0 && x < X0 + CW && y >= LIST_Y && y < LIST_END) {
        unsigned slot = (unsigned)(y - LIST_Y) / CARD_STRIDE;
        if ((y - LIST_Y) % CARD_STRIDE < CARD_H && M.top + slot < nlist) {
            *i = M.top + slot;
            return x >= MORE_X - 36 ? 4 : 2;
        }
    }
    if (y >= BAR_Y && y < BAR_Y + BAR_H && x >= X0)
        for (unsigned b = 0; b < 4; ++b) {
            int bx = X0 + (int)b * (BAR_W + BAR_GAP);
            if (x >= bx && x < bx + BAR_W) {
                *i = b;
                return 3;
            }
        }
    return 0;
}

/* ---- dropdowns (a card's options, the power menu) ---------------------------------------------- */

#define DD_ROW 128
#define DD_PAD 16
#define DD_SEP 25                   /* the gap a separator line sits in */
#define DD_HEAD 112                 /* a nested dropdown's back row */
#define DD_R 44
#define DD_MAXROWS 6
#define SURF 0xff171c26U            /* the dropdown's surface, its hairline edge, the selected row */
#define SURF_EDGE 0xff323b4dU
#define SURF_TOP 0xff4a5670U        /* light on the top edge: lifts the surface off the black */
#define SURF_LINE 0xff252c39U
#define DD_HI ((ACCENT & 0x00ffffffU) | 0x30000000U)
#define TRACK_OFF 0xff3a4355U
#define KNOB_OFF 0xffaeb6c4U

#define DK_PLAIN 0
#define DK_SUB 1                    /* value + chevron: opens a nested dropdown */
#define DK_SWITCH 2
#define DK_CHECK 3                  /* one of a choice: a check mark when on */

struct dd_row {
    char label[40];
    char value[16];
    unsigned icon;                  /* ICON_* or 0 */
    int kind, on, enabled, sep;     /* sep: a separator line above the row */
};

struct dd {
    int x, y, w, h;
    unsigned n, sel;
    int head;                       /* a back row with title[] on top */
    char title[40];
    struct dd_row r[DD_MAXROWS];
};

static struct dd DM[2];             /* [0] the dropdown, [1] the one nested in it */

/* What opened DM[0]: a card (its slot on screen, catalog index) or the Power button. */
static int anchor_card, anchor_slot, anchor_top, anchor_bottom;
static unsigned anchor_idx;

static void dd_begin(struct dd *d, int w, const char *title)
{
    d->w = w;
    d->n = 0;
    d->sel = 0;
    d->head = title != 0;
    scopy(d->title, sizeof(d->title), title);
}

static struct dd_row *dd_add(struct dd *d, const char *label, unsigned icon, int kind)
{
    struct dd_row *r = &d->r[d->n < DD_MAXROWS ? d->n++ : DD_MAXROWS - 1];
    scopy(r->label, sizeof(r->label), label);
    r->value[0] = 0;
    r->icon = icon;
    r->kind = kind;
    r->on = 0;
    r->enabled = 1;
    r->sep = 0;
    return r;
}

static int dd_row_y(const struct dd *d, unsigned i)
{
    int y = d->y + DD_PAD + (d->head ? DD_HEAD : 0) + (int)i * DD_ROW;
    for (unsigned k = 0; k <= i && k < d->n; ++k)
        y += d->r[k].sep ? DD_SEP : 0;
    return y;
}

/* Size d and put it under the anchor (top, bottom), right-aligned to right; above it when there
 * is no room below. */
static void dd_place(struct dd *d, int top, int bottom, int right)
{
    d->y = 0;
    d->h = dd_row_y(d, d->n - 1) + DD_ROW + DD_PAD;
    d->x = right - d->w;
    d->y = bottom + 20;
    if (d->y + d->h > HINT_Y - 70)
        d->y = top - 20 - d->h;
    if (d->y < TAB_Y)
        d->y = TAB_Y;
}

static void timeout_label(char *b, unsigned cap, unsigned t, int after)
{
    if (!t) {
        scopy(b, cap, "Off");
        return;
    }
    char n[3] = { (char)('0' + t / 10), (char)('0' + t % 10), 0 };
    scopy(b, cap, after ? "After " : "");
    scat(b, cap, n[0] == '0' ? n + 1 : n);
    scat(b, cap, " s");
}

static void dd_row_draw(const struct dd *d, unsigned i)
{
    const struct dd_row *r = &d->r[i];
    int ry = dd_row_y(d, i), sel = i == d->sel;
    int mid = ry + DD_ROW / 2;
    ui_fill(d->x + 12, ry, d->w - 24, DD_ROW, SURF);
    if (sel)
        ui_round(d->x + 12, ry + 4, d->w - 24, DD_ROW - 8, 32, DD_HI);
    /* the icon column holds an icon, or a choice's check mark; labels line up after it */
    int tx = d->x + 124;
    if (r->kind == DK_CHECK) {
        if (r->on && glyphs())
            (void)ui_sprite(ICON_BASE + ICON_CHECK, d->x + 40, mid - 24, 256, ACCENT);
        else if (r->on)
            ui_round(d->x + 54, mid - 10, 20, 20, 10, ACCENT);
    } else if (r->icon && glyphs()) {
        (void)ui_sprite(ICON_BASE + r->icon, d->x + 40, mid - 24, 256, !r->enabled ? FAINT : sel ? ACCENT : DIM);
    } else {
        tx = d->x + 48;
    }
    ui_text(F_META, tx, mid + 14, r->label, r->enabled ? INK : FAINT, UI_BEHIND);
    int right = d->x + d->w - 40;
    if (r->kind == DK_SUB) {
        int vr = right;
        if (glyphs()) {
            (void)ui_sprite(ICON_BASE + ICON_CHEV_R, right - 40, mid - 24, 256, sel ? ACCENT : FAINT);
            vr = right - 52;
        }
        ui_text_r(F_META, vr, mid + 14, r->value, sel ? ACCENT : DIM, UI_BEHIND);
    } else if (r->kind == DK_SWITCH) {
        int tw = 100, th = 60, x0 = right - tw, y0 = mid - th / 2;
        ui_round(x0, y0, tw, th, th / 2, r->on ? ACCENT : TRACK_OFF);
        ui_round(r->on ? x0 + tw - 6 - 48 : x0 + 6, y0 + 6, 48, 48, 24, r->on ? 0xffffffffU : KNOB_OFF);
    }
}

static void dd_draw(const struct dd *d)
{
    /* elevation: a soft, offset shadow (seen over the cards), a hairline edge, the surface */
    for (int k = 4; k >= 1; --k)
        ui_round(d->x - 6 * k, d->y - 6 * k + 18, d->w + 12 * k, d->h + 12 * k, DD_R + 6 * k, 0x16000000U);
    /* the hairline edge, lighter along the top (a crescent: full at the middle, gone at the
     * corners), then the surface */
    ui_round(d->x, d->y, d->w, d->h, DD_R, SURF_TOP);
    ui_round(d->x, d->y + 3, d->w, d->h - 3, DD_R, SURF_EDGE);
    ui_round(d->x + 2, d->y + 2, d->w - 4, d->h - 4, DD_R - 2, SURF);
    if (d->head) {
        int mid = d->y + DD_PAD + (DD_HEAD - 12) / 2;
        int g = glyphs() && ui_sprite(ICON_BASE + ICON_CHEV_L, d->x + 40, mid - 24, 256, DIM);
        ui_text(F_META, d->x + (g ? 124 : 48), mid + 14, d->title, DIM, UI_BEHIND);
        ui_fill(d->x + 32, d->y + DD_PAD + DD_HEAD - 10, d->w - 64, 2, SURF_LINE);
    }
    for (unsigned i = 0; i < d->n; ++i) {
        if (d->r[i].sep)
            ui_fill(d->x + 32, dd_row_y(d, i) - DD_SEP / 2 - 1, d->w - 64, 2, SURF_LINE);
        dd_row_draw(d, i);
    }
}

static void dd_present(const struct dd *d)
{
    present_rows(d->y, d->y + d->h);
}

/* Row under (x, y); -2 the back row, -3 outside, -4 padding or a separator. */
static int dd_hit(const struct dd *d, int x, int y)
{
    if (x < d->x || x >= d->x + d->w || y < d->y || y >= d->y + d->h)
        return -3;
    if (d->head && y < d->y + DD_PAD + DD_HEAD)
        return -2;
    for (unsigned i = 0; i < d->n; ++i) {
        int ry = dd_row_y(d, i);
        if (y >= ry && y < ry + DD_ROW)
            return (int)i;
    }
    return -4;
}

static void dd_select(struct dd *d, unsigned i)
{
    unsigned old = d->sel;
    d->sel = i;
    ui_target(CANVAS);
    dd_row_draw(d, old);
    dd_row_draw(d, i);
    ui_target(0);
    dd_present(d);
}

/* The next row that can be picked, down (+1) or up (-1), wrapping. */
static void dd_move(struct dd *d, int dir)
{
    unsigned i = d->sel;
    for (unsigned k = 0; k < d->n; ++k) {
        i = (i + (dir > 0 ? 1U : d->n - 1U)) % d->n;
        if (d->r[i].enabled) {
            dd_select(d, i);
            return;
        }
    }
}

static void power_off(void);

/* The row picked; -1 Bixby, -2 the back row, -3 a tap outside. Both volume keys held power off
 * here too (the emergency exit works everywhere). */
static int dd_pick(struct dd *d)
{
    for (;;) {
        int x = 0, y = 0;
        unsigned ev = input(&x, &y, 0);
        if (ev == EV_TAP || ev == EV_LONG) {
            int h = dd_hit(d, x, y);
            if (h == -2 || h == -3)
                return h;
            if (h < 0 || !d->r[h].enabled)
                continue;
            if ((unsigned)h != d->sel)
                dd_select(d, (unsigned)h);
            return h;
        }
        if (ev == K_BIXBY || ev == EV_BIXBY_HOLD)
            return -1;
        if (ev == EV_VOL_HOLD)
            power_off();
        if (ev & (K_UP | K_DOWN)) {
            dd_move(d, (ev & K_UP) ? -1 : 1);
            continue;
        }
        if (((ev & K_POWER) || ev == EV_POWER_HOLD) && d->r[d->sel].enabled)
            return (int)d->sel;
    }
}

/* Scale CANVAS rows y0..y1 by f/256: the scrim behind a dropdown. */
static void dim_rows(int y0, int y1, unsigned f)
{
    for (uint64_t i = (uint64_t)y0 * W; i < (uint64_t)y1 * W; ++i) {
        uint32_t p = CANVAS[i];
        CANVAS[i] = 0xff000000U | ((((p >> 16) & 255U) * f >> 8) << 16) | ((((p >> 8) & 255U) * f >> 8) << 8) |
                    ((p & 255U) * f >> 8);
    }
}

/* The menu under a scrim, what opened the dropdown lit, the dropdown (and the nested one). */
static void menu_scene(int level)
{
    compose();
    ui_target(CANVAS);
    dim_rows(0, H, 92);
    if (anchor_card)
        draw_card((unsigned)anchor_slot, anchor_idx, 4);
    else
        draw_bar_button(3, 1);
    dd_draw(&DM[level ? 1 : 0]);         /* a nested dropdown takes its parent's place */
    draw_hints(level ? 2 : 1);
    ui_target(0);
    present_rows(0, H);
}

/* Opening and closing fade (FADE_MS): the scrim deepens (or lifts) while the dropdown fades in
 * (or out). CANVAS holds the menu undimmed, with what stays lit (the anchor, the hints) in its
 * final state; DD_SAVE holds what lies under the dropdown and its shadow. Each frame is one pass
 * to the panel that dims on the way, so CANVAS is never dimmed in place; the dropdown is drawn
 * again each frame over its region dimmed by that frame's scrim, so its shadow and corners blend
 * over the right background. */
#define DD_SAVE ((uint32_t *)0x88200000UL)  /* after CANVAS (ends 0x88142000), below ui.c's
                                               UI_SCRATCH (0x88800000): 6 MiB, WB */
#define DD_SAVE_PX (0x600000U / 4U)
#define FADE_MS 120
#define FADE_FRAMES 16                  /* a cap, should the timer not move */
#define SCRIM 92U

static int fx0, fy0, fx1, fy1;          /* the dropdown's region (with its shadow) */
static int kx0, ky0, kx1, ky1;          /* the anchor: kept lit */

static uint32_t dim_px(uint32_t p, unsigned f)
{
    return 0xff000000U | ((((p >> 16) & 255U) * f >> 8) << 16) | ((((p >> 8) & 255U) * f >> 8) << 8) |
           ((p & 255U) * f >> 8);
}

static uint32_t lerp_px(uint32_t a, uint32_t b, unsigned t)    /* a at t/256 over b */
{
    uint32_t o = 0xff000000U;
    for (unsigned sh = 0; sh < 24; sh += 8)
        o |= ((((a >> sh) & 255U) * t + ((b >> sh) & 255U) * (256U - t)) >> 8) << sh;
    return o;
}

static int kept(int x, int y)
{
    return (y >= HINT_Y - 50 && y < HINT_Y + 30) || (y >= ky0 && y < ky1 && x >= kx0 && x < kx1);
}

/* CANVAS: the menu (lit: the anchor and hints in their state after the fade), DD_SAVE: the
 * dropdown's region of it. 0 if the region does not fit DD_SAVE. */
static int fade_stage(const struct dd *d, int open, int level)
{
    fx0 = d->x - 32;
    fx1 = d->x + d->w + 32;
    fy0 = d->y - 24;
    fy1 = d->y + d->h + 48;
    if (fx0 < 0)
        fx0 = 0;
    if (fy0 < 0)
        fy0 = 0;
    if (fx1 > W)
        fx1 = W;
    if (fy1 > H)
        fy1 = H;
    if (fx1 <= fx0 || fy1 <= fy0 || (unsigned)((fx1 - fx0) * (fy1 - fy0)) > DD_SAVE_PX)
        return 0;
    if (anchor_card) {
        kx0 = X0 - 4;
        kx1 = X0 + CW + 4;
        ky0 = anchor_top - 4;
        ky1 = anchor_bottom + 4;
    } else {
        kx0 = X0 + 3 * (BAR_W + BAR_GAP) - 4;
        kx1 = kx0 + BAR_W + 8;
        ky0 = BAR_Y - 4;
        ky1 = BAR_Y + BAR_H + 4;
    }
    compose();
    if (open) {
        ui_target(CANVAS);
        if (anchor_card)
            draw_card((unsigned)anchor_slot, anchor_idx, 4);
        else
            draw_bar_button(3, 1);
        draw_hints(level ? 2 : 1);
        ui_target(0);
    }
    int rw = fx1 - fx0;
    for (int y = fy0; y < fy1; ++y)
        for (int x = fx0; x < fx1; ++x)
            DD_SAVE[(uint64_t)(y - fy0) * (unsigned)rw + (unsigned)(x - fx0)] = CANVAS[(uint64_t)y * W + (unsigned)x];
    return 1;
}

/* One frame to the panel: the scrim at f/256, the dropdown at a/256 over it. */
static void fade_frame(const struct dd *d, unsigned f, unsigned a)
{
    int rw = fx1 - fx0;
    /* the region: what was under the dropdown, dimmed, then the dropdown over it */
    for (int y = fy0; y < fy1; ++y)
        for (int x = fx0; x < fx1; ++x) {
            uint32_t p = DD_SAVE[(uint64_t)(y - fy0) * (unsigned)rw + (unsigned)(x - fx0)];
            CANVAS[(uint64_t)y * W + (unsigned)x] = kept(x, y) ? p : dim_px(p, f);
        }
    ui_target(CANVAS);
    dd_draw(d);
    ui_target(0);
    volatile uint64_t *fb = (volatile uint64_t *)STAR_FB_BASE;
    for (int y = 0; y < H; ++y) {
        const uint32_t *c = CANVAS + (uint64_t)y * W;
        const uint32_t *sv = DD_SAVE + (uint64_t)(y - fy0) * (unsigned)rw;
        int in_y = y >= fy0 && y < fy1;
        int hint = y >= HINT_Y - 50 && y < HINT_Y + 30, anc = y >= ky0 && y < ky1;
        for (int x = 0; x < W; x += 2) {
            uint32_t o[2];
            for (int k = 0; k < 2; ++k) {
                int xx = x + k;
                int keep = hint || (anc && xx >= kx0 && xx < kx1);
                uint32_t p = c[xx];
                if (in_y && xx >= fx0 && xx < fx1) {
                    uint32_t b = sv[xx - fx0];
                    if (!keep)
                        b = dim_px(b, f);
                    p = a >= 256 ? p : a ? lerp_px(p, b, a) : b;
                } else if (!keep) {
                    p = dim_px(p, f);
                }
                o[k] = p;
            }
            fb[((uint64_t)y * W + (unsigned)x) / 2] = o[0] | (uint64_t)o[1] << 32;
        }
    }
    __asm__ volatile("dsb st" : : : "memory");
}

/* Fade the dropdown (DM[level]) in over the menu, then the scene as menu_scene draws it. */
static void dd_open(int level)
{
    const struct dd *d = &DM[level ? 1 : 0];
    if (fade_stage(d, 1, level)) {
        uint64_t t0 = hw_ticks();
        for (int n = 0; n < FADE_FRAMES; ++n) {
            int ms = ms_since(t0);
            if (ms >= FADE_MS)
                break;
            unsigned q = 256U - (unsigned)(ms * 256 / FADE_MS);
            unsigned e = 256U - q * q * q / 65536U;         /* ease out */
            fade_frame(d, 256U - (256U - SCRIM) * e / 256U, e);
        }
    }
    menu_scene(level);
}

/* Fade the dropdown (DM[level]) out and the scrim off: then CANVAS and the panel are the menu. */
static void dd_close(int level)
{
    const struct dd *d = &DM[level ? 1 : 0];
    if (!fade_stage(d, 0, level)) {
        compose();
        present_rows(0, H);
        return;
    }
    uint64_t t0 = hw_ticks();
    for (int n = 0; n < FADE_FRAMES; ++n) {
        int ms = ms_since(t0);
        if (ms >= FADE_MS)
            break;
        unsigned q = 256U - (unsigned)(ms * 256 / FADE_MS);
        unsigned e = 256U - q * q * q / 65536U;
        fade_frame(d, SCRIM + (256U - SCRIM) * e / 256U, 256U - e);
    }
    compose();
    present_rows(0, H);
}

static void card_menu_build(unsigned slot, unsigned idx)
{
    const struct cat_ent *e = cat_get(idx);
    struct star_settings *s = star_menu_settings(0);
    int ready = star_entry_ready(idx, 0);
    struct dd *d = &DM[0];
    struct dd_row *r;
    anchor_card = 1;
    anchor_slot = (int)slot;
    anchor_idx = idx;
    dd_begin(d, 800, 0);
    r = dd_add(d, "Start now", ICON_PLAY, DK_PLAIN);
    r->enabled = ready;
    r = dd_add(d, "Default entry", ICON_STAR, DK_SWITCH);
    r->on = s->default_key[0] && key_eq(s->default_key, e->key);
    r->sep = 1;
    r = dd_add(d, "Auto-start", ICON_TIMER, DK_SUB);
    timeout_label(r->value, sizeof(r->value), s->timeout_s, 0);
    d->sel = ready ? 0 : 1;
    int y = LIST_Y + (int)slot * CARD_STRIDE;
    anchor_top = y;
    anchor_bottom = y + CARD_H;
    dd_place(d, anchor_top, anchor_bottom, X0 + CW);
}

/* Auto-start's choice k: Off, 3, 5, 10 s. */
static unsigned timeout_of(unsigned k)
{
    return k == 0 ? 0 : k == 1 ? 3 : k == 2 ? 5 : 10;
}

/* Auto-start's choices, in the dropdown's place, with a back row. */
static void timeout_menu_build(void)
{
    struct star_settings *s = star_menu_settings(0);
    struct dd *d = &DM[1], *p = &DM[0];
    dd_begin(d, p->w, "Auto-start");
    for (unsigned k = 0; k < 4; ++k) {
        unsigned t = timeout_of(k);
        char b[16];
        timeout_label(b, sizeof(b), t, 1);
        struct dd_row *r = dd_add(d, b, 0, DK_CHECK);
        r->on = s->timeout_s == t;
        if (r->on)
            d->sel = k;
    }
    dd_place(d, anchor_top, anchor_bottom, p->x + p->w);
}

/* A card's options. 1: start it. */
static int card_menu(unsigned slot, unsigned idx)
{
    const struct cat_ent *e = cat_get(idx);
    card_menu_build(slot, idx);
    dd_open(0);
    for (;;) {
        int r = dd_pick(&DM[0]);
        if (r < 0) {
            dd_close(0);
            return 0;
        }
        if (r == 0)
            return 1;                                   /* go() draws the menu again */
        struct star_settings *s = star_menu_settings(0);
        if (r == 1) {
            if (s->default_key[0] && key_eq(s->default_key, e->key))
                s->default_key[0] = 0;
            else
                scopy(s->default_key, sizeof(s->default_key), e->key);
            if (star_menu_settings_save())
                set_status("Could not save the settings", "", RED);
            DM[0].r[1].on = s->default_key[0] != 0;
            menu_scene(0);          /* every card: another one may have been the default */
            continue;
        }
        timeout_menu_build();
        menu_scene(1);
        int k = dd_pick(&DM[1]);
        if (k == -3) {
            dd_close(1);                                /* a tap outside closes both */
            return 0;
        }
        if (k >= 0) {
            s->timeout_s = timeout_of((unsigned)k);
            if (star_menu_settings_save())
                set_status("Could not save the settings", "", RED);
            timeout_label(DM[0].r[2].value, sizeof(DM[0].r[2].value), s->timeout_s, 0);
        }
        menu_scene(0);
    }
}

static void power_menu_build(void)
{
    struct dd *d = &DM[0];
    anchor_card = 0;
    dd_begin(d, 620, 0);
    (void)dd_add(d, "Power off", ICON_PWR, DK_PLAIN);
    (void)dd_add(d, "Restart", ICON_RESTART, DK_PLAIN);
    anchor_top = BAR_Y;
    anchor_bottom = BAR_Y + BAR_H;
    dd_place(d, anchor_top, anchor_bottom, X0 + CW);
}

/* The Power button's dropdown: 0 nothing (the menu is on screen again), 1 power off, 2 restart. */
static int power_menu(void)
{
    power_menu_build();
    dd_open(0);
    int r = dd_pick(&DM[0]);
    if (r < 0)
        dd_close(0);
    return r < 0 ? 0 : r + 1;
}

/* ---- starting ---------------------------------------------------------------------------------- */

/* The card says "starting"; the bottom of the screen is cleared for the loader's status lines. */
static int go(int idx)
{
    focus_entry(idx);
    set_status("", "", DIM);
    compose();
    ui_target(CANVAS);
    make_list(M.tab);
    unsigned k = M.sel[M.tab];
    if (k >= M.top && k < M.top + LIST_ROWS)
        draw_card(k - M.top, (unsigned)idx, 2);
    ui_fill(0, BAR_Y - 8, W, FOOTER_Y - BAR_Y, BG);
    ui_target(0);
    present_rows(0, H);
    star_menu_remember(idx);
    star_fb_row_base((BAR_Y - 200U) / 60U);                 /* fbtext: TOP 200, 60 px rows */
    hw_tp_clear();
    return idx + 1;
}

static void reboot_to(uint32_t inform, const char *what)
{
    star_fb_fill(BG);
    star_fb_row_base(22);
    star_fb_text_c(0, what, 0xffffffffU, BG);
    hw_reboot(inform);
}

static void power_off(void)
{
    star_fb_fill(BG);
    star_fb_row_base(22);
    star_fb_text_c(0, "POWERING OFF", 0xffffffffU, BG);
    hw_power_off();
    star_fb_text_c(1, "STILL ON: CHARGER CONNECTED?", DIM, BG);
    star_fb_text_c(2, "REBOOTING", DIM, BG);
    hw_wait_ms(1500);
    hw_reboot(INFORM_NORMAL);
}

/* The options of card k of the current tab (on screen), then the menu again. 1: start it. */
static int options(unsigned k)
{
    if (k < M.top || k >= M.top + LIST_ROWS)
        return 0;
    return card_menu(k - M.top, list[k]);
}

static void lab(const struct star_info *in, char *cmdline, unsigned len, int *boot)
{
    ui_target(0);
    *boot = em_main(in, cmdline, len);
    compose();
    present_rows(0, H);
}

int menu_v6(const struct star_info *in, char *cmdline, unsigned len)
{
    const char *fname, *fwhy;
    ui_init();
    if (!M.inited) {
        star_menu_begin();
        make_tabs();
        M.inited = 1;
        M.tab = 0;
        M.zone = 0;
        /* focus: the last choice, else the default, else the first entry that can start */
        struct star_settings *s = star_menu_settings(0);
        int f = star_menu_ready_key(s->last_key);
        if (f < 0)
            f = star_menu_ready_key(s->default_key);
        for (unsigned i = 0; f < 0 && i < cat_count(); ++i)
            if (!(cat_get(i)->flags & CF_HIDDEN) && star_entry_ready(i, 0))
                f = (int)i;
        focus_entry(f);
        hw_tp_start();
        int next = star_menu_next();
        if (next >= 0)
            return go(next);
        set_status("", "", DIM);
        compose();
        /* the intro: tabs, cards, bar and hints rise in over the header the splash left */
        int rc[4 * 16], n = 0;
        rc[n++] = 0; rc[n++] = TAB_Y - 8; rc[n++] = W; rc[n++] = TAB_H + 16;
        make_list(M.tab);
        for (unsigned k = 0; k < nlist && k < LIST_ROWS; ++k) {
            rc[n++] = X0 - 4; rc[n++] = LIST_Y + (int)k * CARD_STRIDE - 4; rc[n++] = CW + 8; rc[n++] = CARD_H + 8;
        }
        if (!nlist) {
            rc[n++] = 0; rc[n++] = LIST_Y; rc[n++] = W; rc[n++] = 420;
        }
        rc[n++] = 0; rc[n++] = BAR_Y - 8; rc[n++] = W; rc[n++] = BAR_H + 16;
        rc[n++] = 0; rc[n++] = HINT_Y - 50; rc[n++] = W; rc[n++] = 80;
        star_reveal(CANVAS, rc, (unsigned)(n / 4));
        /* a default entry with a timeout starts unless something is pressed or touched */
        int dflt = s->timeout_s ? star_menu_ready_key(s->default_key) : -1;
        if (dflt >= 0) {
            char b[96];
            focus_entry(dflt);
            for (unsigned left = s->timeout_s; left; --left) {
                char n2[3] = { (char)('0' + left / 10), (char)('0' + left % 10), 0 };
                scopy(b, sizeof(b), cat_get((unsigned)dflt)->name);
                scat(b, sizeof(b), " starts in ");
                scat(b, sizeof(b), n2[0] == '0' ? n2 + 1 : n2);
                scat(b, sizeof(b), " s");
                set_status(b, "Press any key or touch the screen to stay here", INK);
                refresh(1);
                int x, y;
                if (input(&x, &y, 1000)) {
                    dflt = -1;
                    break;
                }
            }
            set_status("", "", DIM);
            if (dflt >= 0)
                return go(dflt);
            refresh(1);
        }
    } else if (star_menu_failure(&fname, &fwhy)) {
        /* back from a start that failed: the menu as it was, and why */
        char b[96], w2[96];
        scopy(b, sizeof(b), "Could not start ");
        scat(b, sizeof(b), fname);
        sentence(w2, sizeof(w2), fwhy ? fwhy : "UNKNOWN ERROR");
        set_status(b, w2, RED);
        compose();
        present_rows(0, H);
    }

    for (;;) {
        int x = 0, y = 0;
        unsigned ev = input(&x, &y, 0);
        unsigned i = 0;
        int boot = 0;
        if (ev == EV_TAP || ev == EV_LONG) {
            int what = hit(x, y, &i);
            if (what == 1) {
                if (i != M.tab) {
                    M.tab = i;
                    M.zone = 0;
                    M.top = 0;
                    refresh(1);
                }
                continue;
            }
            if (what == 4 || (what == 2 && ev == EV_LONG)) {
                M.zone = 0;
                M.sel[M.tab] = i;
                refresh(0);
                if (options(i))
                    return go((int)list[i]);
                continue;
            }
            if (what == 2) {
                if (M.zone == 0 && M.sel[M.tab] == i) {
                    ev = K_POWER;                           /* second tap starts it */
                } else {
                    M.zone = 0;
                    M.sel[M.tab] = i;
                    refresh(0);
                    continue;
                }
            } else if (what == 3) {
                if (M.zone == 1 && M.bar == i) {
                    ev = K_POWER;
                } else {
                    M.zone = 1;
                    M.bar = i;
                    refresh(0);
                    continue;
                }
            } else {
                continue;
            }
        }
        if (ev == EV_LEFT || ev == EV_RIGHT || ev == K_BIXBY) {
            M.tab = (M.tab + (ev == EV_RIGHT ? ntabs - 1U : 1U)) % ntabs;
            M.zone = 0;
            M.top = 0;
            refresh(1);
            continue;
        }
        if (ev == EV_BIXBY_HOLD) {
            lab(in, cmdline, len, &boot);
            if (boot > 0)
                return go(boot - 1);
            continue;
        }
        if (ev == EV_VOL_HOLD)
            power_off();
        if (ev == EV_POWER_HOLD) {
            make_list(M.tab);
            if (M.zone == 0 && nlist) {
                if (options(M.sel[M.tab]))
                    return go((int)list[M.sel[M.tab]]);
                continue;
            }
            ev = K_POWER;
        }
        if (ev & K_POWER) {
            if (M.zone == 0) {
                make_list(M.tab);
                if (nlist && star_entry_ready(list[M.sel[M.tab]], 0))
                    return go((int)list[M.sel[M.tab]]);
                continue;
            }
            if (M.bar == 2) {
                lab(in, cmdline, len, &boot);
                if (boot > 0)
                    return go(boot - 1);
                continue;
            }
            if (M.bar == 3) {
                int p = power_menu();
                if (p == 1)
                    power_off();
                if (p == 2)
                    reboot_to(INFORM_NORMAL, "RESTARTING");
                continue;
            }
            reboot_to(M.bar == 0 ? INFORM_RECOVERY : INFORM_DOWNLOAD,
                      M.bar == 0 ? "REBOOTING TO TWRP RECOVERY" : "REBOOTING TO DOWNLOAD MODE");
        }
        if (ev & (K_UP | K_DOWN)) {
            make_list(M.tab);
            int down = (ev & K_DOWN) != 0;
            if (M.zone == 0) {
                if (down) {
                    if (M.sel[M.tab] + 1 < nlist)
                        ++M.sel[M.tab];
                    else {
                        M.zone = 1;
                        M.bar = 0;
                    }
                } else if (M.sel[M.tab]) {
                    --M.sel[M.tab];
                }
            } else if (down) {
                M.bar = (M.bar + 1U) % 4U;
            } else if (M.bar) {
                --M.bar;
            } else if (nlist) {
                M.zone = 0;
                M.sel[M.tab] = nlist - 1;
            }
            refresh(0);
        }
    }
}
