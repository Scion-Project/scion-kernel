/* SPDX-License-Identifier: GPL-2.0-only
 * EngineerMode drawing toolkit (ui.h). Glyph atlas "SBFONT01" from the BOOT
 * ramdisk blob (xnu_boot.c star_font_blob). No pointer tables: the Image must
 * have no absolute relocations, so every table here is filled at run time.
 */
#include "ui.h"
#include "fbtext.h"
#include "hw.h"
#include "menu.h"          /* star_sprite_to, star_sprite_rect (xnu_boot.c) */

#define MAX_FONTS 12

static uintptr_t font_hdr[MAX_FONTS];
static int clip_y0, clip_y1 = UI_H;
static volatile uint32_t *target;

static volatile uint32_t *fbuf(void)
{
    return target ? target : (volatile uint32_t *)STAR_FB_BASE;
}

void ui_target(uint32_t *buf)
{
    target = buf;
}

static uint32_t r32(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}

static uint16_t r16(uintptr_t a)
{
    return *(volatile uint16_t *)a;
}

int ui_init(void)
{
    uint64_t start, end;
    if (!star_font_blob(&start, &end))
        return 0;
    uint32_t n = r32((uintptr_t)start + 8);
    uintptr_t q = (uintptr_t)start + 12;
    for (unsigned i = 0; i < MAX_FONTS; ++i)
        font_hdr[i] = 0;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        if (q + 32 > end)
            return 0;
        uint32_t id = r32(q), count = r32(q + 24), size = r32(q + 28);
        if (q + 32 + (uint64_t)count * 16 + size > end || (size & 3))
            return 0;
        if (id < MAX_FONTS)
            font_hdr[id] = q;
        q += 32 + (uintptr_t)count * 16 + size;
    }
    clip_y0 = 0;
    clip_y1 = UI_H;
    return font_hdr[0] != 0;
}

void ui_clip(int y0, int y1)
{
    clip_y0 = y0 < 0 ? 0 : y0;
    clip_y1 = y1 > UI_H ? UI_H : y1;
}

void ui_fill(int x, int y, int w, int h, uint32_t c)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w > UI_W ? UI_W : x + w;
    int y0 = y < clip_y0 ? clip_y0 : y, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;
    volatile uint32_t *fb = fbuf();
    for (int yy = y0; yy < y1; ++yy) {
        volatile uint32_t *row = fb + (uint64_t)yy * UI_W;
        for (int xx = x0; xx < x1; ++xx)
            row[xx] = c;
    }
}

static uint32_t mix(uint32_t fg, uint32_t bg, unsigned a)
{
    if (a >= 255)
        return fg;
    uint32_t o = 0xff000000U;
    for (unsigned sh = 0; sh < 24; sh += 8) {
        uint32_t f = (fg >> sh) & 255U, b = (bg >> sh) & 255U;
        o |= ((f * a + b * (255U - a) + 127U) / 255U) << sh;
    }
    return o;
}

static unsigned isqrt(unsigned v)
{
    unsigned r = 0, b = 1U << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

void ui_round(int x, int y, int w, int h, int r, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    unsigned ca = c >> 24;
    volatile uint32_t *fb = fbuf();
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < clip_y0 || yy >= clip_y1)
            continue;
        volatile uint32_t *row = fb + (uint64_t)yy * UI_W;
        /* dx, dy: how far into a corner square the pixel is (0 = not in a corner) */
        int dy = yy < y + r ? y + r - yy : yy >= y + h - r ? yy - (y + h - r - 1) : 0;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= UI_W)
                continue;
            int dx = xx < x + r ? x + r - xx : xx >= x + w - r ? xx - (x + w - r - 1) : 0;
            unsigned a = ca;
            if (dx && dy) {
                /* the pixel centre's distance from the corner centre, in 1/16 px: coverage ramps
                 * from 0 to 16 across the one pixel the circle's edge passes through */
                int ex = 2 * dx - 1, ey = 2 * dy - 1;               /* doubled centre offsets */
                int d = (int)isqrt((unsigned)(ex * ex + ey * ey) * 64U);
                int cov = 16 * r - d + 8;
                if (cov <= 0)
                    continue;
                if (cov < 16)
                    a = a * (unsigned)cov / 16U;
            }
            if (!a)
                continue;
            row[xx] = a >= 255 ? (c | 0xff000000U) : mix(c, row[xx], a);
        }
    }
}

/* ui_sprite: the sprite is copied here first (star_sprite_to, panel stride), then blended. Free
 * write-back RAM between menu6.c's CANVAS (0x87100000..0x88142000) and the menu assets
 * (0x89000000), UI_W x UI_SCRATCH_ROWS. */
#define UI_SCRATCH ((uint32_t *)0x88800000UL)
#define UI_SCRATCH_ROWS 192U

int ui_sprite(unsigned id, int x, int y, unsigned fade, uint32_t tint)
{
    unsigned sx, sy, w, h;
    if (!star_sprite_rect(id, &sx, &sy, &w, &h) || !w || w > UI_W || h > UI_SCRATCH_ROWS)
        return 0;
    if (star_sprite_to(id, UI_SCRATCH, 0, 0, 1, 256) <= 0)
        return 0;
    if (fade > 256)
        fade = 256;
    volatile uint32_t *fb = fbuf();
    for (unsigned r = 0; r < h; ++r) {
        int yy = y + (int)r;
        if (yy < clip_y0 || yy >= clip_y1)
            continue;
        const volatile uint32_t *src = UI_SCRATCH + (uint64_t)r * UI_W;
        volatile uint32_t *row = fb + (uint64_t)yy * UI_W;
        for (unsigned c = 0; c < w; ++c) {
            int xx = x + (int)c;
            uint32_t s = src[c];
            unsigned a = ((s >> 24) * fade) >> 8;
            if (!a || xx < 0 || xx >= UI_W)
                continue;
            row[xx] = mix(tint ? tint | 0xff000000U : s, row[xx], a);
        }
    }
    return 1;
}

/* Glyph record for code c (falls back to '?'). */
static uintptr_t glyph(uintptr_t h, unsigned c)
{
    uint32_t first = r32(h + 20), count = r32(h + 24);
    if (c < first || c >= first + count)
        c = '?';
    return h + 32 + (uintptr_t)(c - first) * 16;
}

static int16_t s16(uintptr_t a)
{
    return (int16_t)r16(a);
}

int ui_text_w(int font, const char *s)
{
    if (font < 0 || font >= MAX_FONTS || !font_hdr[font])
        return 0;
    int w = 0;
    for (; *s; ++s)
        w += s16(glyph(font_hdr[font], (unsigned char)*s));
    return w;
}

int ui_ascent(int font)
{
    return (font >= 0 && font < MAX_FONTS && font_hdr[font]) ? (int)r32(font_hdr[font] + 8) : 0;
}

int ui_line(int font)
{
    return (font >= 0 && font < MAX_FONTS && font_hdr[font]) ? (int)r32(font_hdr[font] + 16) : 40;
}

int ui_text(int font, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    if (font < 0 || font >= MAX_FONTS || !font_hdr[font])
        return x;
    uintptr_t h = font_hdr[font];
    uintptr_t data = h + 32 + (uintptr_t)r32(h + 24) * 16;
    volatile uint32_t *fb = fbuf();
    for (; *s; ++s) {
        uintptr_t g = glyph(h, (unsigned char)*s);
        int adv = s16(g), w = r16(g + 2), gh = r16(g + 4), bx = s16(g + 6), by = s16(g + 8);
        uintptr_t px = data + r32(g + 12);
        for (int r = 0; r < gh; ++r) {
            int yy = y + by + r;
            if (yy < clip_y0 || yy >= clip_y1)
                continue;
            volatile uint32_t *row = fb + (uint64_t)yy * UI_W;
            for (int c = 0; c < w; ++c) {
                unsigned a = *(volatile uint8_t *)(px + (uintptr_t)r * w + c);
                int xx = x + bx + c;
                if (a && xx >= 0 && xx < UI_W)
                    row[xx] = mix(fg, bg == UI_BEHIND ? row[xx] : bg, a);
            }
        }
        x += adv;
    }
    return x;
}

int ui_text_r(int font, int right, int y, const char *s, uint32_t fg, uint32_t bg)
{
    return ui_text(font, right - ui_text_w(font, s), y, s, fg, bg);
}

int ui_text_c(int font, int cx, int y, const char *s, uint32_t fg, uint32_t bg)
{
    return ui_text(font, cx - ui_text_w(font, s) / 2, y, s, fg, bg);
}

/* ---- string builder ---------------------------------------------------- */

unsigned ui_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}

void ub_init(struct ub *u)
{
    u->n = 0;
    u->b[0] = 0;
}

void ub_c(struct ub *u, char c)
{
    if (u->n + 1 < sizeof(u->b)) {
        u->b[u->n++] = c;
        u->b[u->n] = 0;
    }
}

void ub_s(struct ub *u, const char *s)
{
    while (*s)
        ub_c(u, *s++);
}

void ub_dec(struct ub *u, uint64_t v)
{
    char t[24];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v);
    while (n)
        ub_c(u, t[--n]);
}

void ub_sdec(struct ub *u, int64_t v)
{
    if (v < 0) {
        ub_c(u, '-');
        ub_dec(u, (uint64_t)(-v));
    } else {
        ub_dec(u, (uint64_t)v);
    }
}

void ub_hexn(struct ub *u, uint64_t v, unsigned digits)
{
    for (unsigned i = digits; i; --i)
        ub_c(u, "0123456789ABCDEF"[(v >> (4 * (i - 1))) & 15U]);
}

void ub_hex(struct ub *u, uint64_t v, unsigned digits)
{
    ub_s(u, "0x");
    ub_hexn(u, v, digits);
}

void ub_dec2(struct ub *u, unsigned v)
{
    ub_c(u, (char)('0' + (v / 10U) % 10U));
    ub_c(u, (char)('0' + v % 10U));
}

void ub_fix(struct ub *u, uint64_t milli, unsigned decimals)
{
    ub_dec(u, milli / 1000U);
    if (decimals) {
        ub_c(u, '.');
        uint64_t f = milli % 1000U;
        if (decimals == 1)
            ub_dec(u, f / 100U);
        else if (decimals == 2) {
            ub_c(u, (char)('0' + f / 100U));
            ub_c(u, (char)('0' + (f / 10U) % 10U));
        } else {
            ub_c(u, (char)('0' + f / 100U));
            ub_c(u, (char)('0' + (f / 10U) % 10U));
            ub_c(u, (char)('0' + f % 10U));
        }
    }
}

void ub_size(struct ub *u, uint64_t bytes)
{
    if (bytes >= (1ULL << 30) && !(bytes & ((1ULL << 30) - 1))) {
        ub_dec(u, bytes >> 30);
        ub_s(u, " GiB");
    } else if (bytes >= (1ULL << 20)) {
        ub_dec(u, bytes >> 20);
        if (bytes & ((1ULL << 20) - 1)) {
            ub_c(u, '.');
            ub_dec(u, ((bytes & ((1ULL << 20) - 1)) * 10U) >> 20);
        }
        ub_s(u, " MiB");
    } else if (bytes >= 1024U) {
        ub_dec(u, bytes >> 10);
        ub_s(u, " KiB");
    } else {
        ub_dec(u, bytes);
        ub_s(u, " B");
    }
}
