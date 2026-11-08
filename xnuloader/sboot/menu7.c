/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 (docs/MENU7-PORT.md section 4.2): the approved concept docs/concepts/ios27-boot.html
 * ported function by function. The concept's <script> is the reference: its names are kept in the
 * comments (go, update, render, openCtx, launchBack, ...), its numbers are used as they are, and the
 * layout numbers come from its CSS as measured in docs/concepts/boot-geometry/<state>.json.
 *
 * menu7 never touches a pixel. Every frame it describes the concept's two layers (the #menu DOM over
 * the base glass, the #over DOM over the overlay glass) as a struct g7_frame and glass.c draws them.
 *
 * Differences from the concept are the doc's D1..D15 (section 6) and the ones marked "device:" here.
 * Loader rules: no libc, no static tables of pointers (every string is chosen in code), stack
 * arrays under 512 bytes. Compiled with FP/SIMD (CFLAGS_V7 in build.sh).
 */
#include <stdint.h>
#include "menu7.h"
#include "intro7.h"
#include "glass.h"
#include "sb7.h"
#include "menu.h"
#include "hw.h"
#include "catalog.h"
#include "store.h"
#include "fbtext.h"
#include "batt.h"

#define PX 1440.0f
#define PY 2960.0f
#define PI_F 3.14159265358979f

/* ================================================================================================
 * math (no libm: the A55's frint / fsqrt through builtins, exp / sin / cos as polynomials)
 * ============================================================================================== */

static float absf(float x) { return x < 0 ? -x : x; }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float c01(float x) { return clampf(x, 0, 1); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float floorf_(float x) { return __builtin_floorf(x); }
static float ceilf_(float x) { return __builtin_ceilf(x); }
static float sqrtf_(float x) { return __builtin_sqrtf(x); }

static float expf_(float x)
{
    if (x < -87.0f)
        return 0;
    if (x > 87.0f)
        x = 87.0f;
    float n = floorf_(x * 1.44269504f + 0.5f);
    float r = x - n * 0.693145751953125f - n * 1.428606765330187e-06f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666668f +
              r * (0.008333334f + r * 0.0013888889f)))));
    union { float f; int32_t i; } u;
    u.i = ((int32_t)n + 127) << 23;
    return p * u.f;
}

/* sin on [-pi/2, pi/2] (odd polynomial to x^11), the rest by symmetry */
static float sin_core(float x)
{
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.008333333f + x2 * (-0.0001984127f +
           x2 * (2.7557319e-06f + x2 * -2.5052108e-08f)))));
}

static float sinf_(float x)
{
    x -= 2.0f * PI_F * floorf_(x / (2.0f * PI_F) + 0.5f);      /* -pi .. pi */
    if (x > PI_F * 0.5f)
        x = PI_F - x;
    else if (x < -PI_F * 0.5f)
        x = -PI_F - x;
    return sin_core(x);
}

static float cosf_(float x) { return sinf_(x + PI_F * 0.5f); }

float m7_clamp(float x, float a, float b) { return clampf(x, a, b); }

float m7_ease(float t)
{
    if (t < 0.5f)
        return 4 * t * t * t;
    float u = -2 * t + 2;
    return 1 - u * u * u / 2;
}

float m7_eout(float t)
{
    float u = 1 - t;
    return 1 - u * u * u;
}

/* spA(t, r, z): the closed-form step response of a spring released at t = 0 (t in s) */
float m7_spa(float t, float r, float z)
{
    if (t <= 0)
        return 0;
    float w = 2 * PI_F / r;
    if (z >= 1)
        return 1 - (1 + w * t) * expf_(-w * t);
    float wd = w * sqrtf_(1 - z * z);
    return 1 - expf_(-z * w * t) * (cosf_(wd * t) + z * w / wd * sinf_(wd * t));
}

/* CSS cubic-bezier(x1, y1, x2, y2) at progress x (transitions: 'ease' and the switch knob) */
static float bez_c(float a, float b, float t)
{
    float u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static float cubic_bezier(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; ++i) {
        t = (lo + hi) * 0.5f;
        if (bez_c(x1, x2, t) < x)
            lo = t;
        else
            hi = t;
    }
    return bez_c(y1, y2, t);
}

static float css_ease(float x) { return cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, x); }

/* ================================================================================================
 * springs (spec "Springs": m = 1, k = (2 pi / response)^2, c = 4 pi zeta / response, h = 4 ms)
 * ============================================================================================== */

void m7_spring_cfg(struct m7_spring *s, float r, float z)
{
    float w = 2 * PI_F / r;
    s->k = w * w;
    s->c = 4 * PI_F * z / r;
}

void m7_spring_init(struct m7_spring *s, float x, float r, float z, float eps)
{
    s->x = x;
    s->t = x;
    s->v = 0;
    s->eps = eps;
    m7_spring_cfg(s, r, z);
}

void m7_spring_to(struct m7_spring *s, float t) { s->t = t; }

void m7_spring_snap(struct m7_spring *s, float t)
{
    s->t = t;
    s->x = t;
    s->v = 0;
}

void m7_spring_kick(struct m7_spring *s, float v) { s->v += v; }

void m7_spring_step(struct m7_spring *s, float dt)
{
    if (s->x == s->t && s->v == 0)
        return;
    int n = (int)ceilf_(dt / M7_SPRING_H);
    if (n < 1)
        n = 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; ++i) {
        float a = -s->k * (s->x - s->t) - s->c * s->v;
        s->v += a * h;
        s->x += s->v * h;
    }
    if (absf(s->x - s->t) < s->eps && absf(s->v) < s->eps * 8) {
        s->x = s->t;
        s->v = 0;
    }
}

static int sp_moving(const struct m7_spring *s) { return s->x != s->t || s->v != 0; }

/* ================================================================================================
 * the concept's state
 * ============================================================================================== */

#define NROW (CAT_MAX + 1)
#define NPRESS 24
#define NSHAKE 8
#define NTMR 16

/* focus keys (the concept's data-f strings): kind << 16 | a << 8 | b */
#define FK(k, a, b) ((uint32_t)(k) << 16 | (uint32_t)(a) << 8 | (uint32_t)(b))
#define FKIND(f) ((f) >> 16)
#define FA(f) (((f) >> 8) & 0xffU)
#define FB(f) ((f) & 0xffU)
enum { F_NONE, F_P, F_R, F_T, F_M, F_A, F_SH, F_E };
enum { T_RECOVERY, T_DOWNLOAD, T_LAB, T_POWER };                      /* t:* */
enum { MI_START, MI_DEFAULT, MI_AUTO0, MI_AUTO3, MI_AUTO5, MI_AUTO10, MI_DETAILS };  /* m:* */
enum { A_OFF, A_RESTART, A_CANCEL, A_RECOVERY, A_DOWNLOAD };          /* a:* */
enum { SH_CLOSE, SH_DEF, SH_START };                                  /* sh:* */
enum { E_RETRY, E_BACK };                                             /* e:* */

enum { MD_BOOT, MD_SPLASH, MD_INTRO, MD_MENU, MD_LAUNCH, MD_END };
enum { OV_NONE, OV_CTX, OV_ALERT, OV_SHEET };
enum { AL_POWER, AL_RECOVERY, AL_DOWNLOAD };
enum { EN_HANDOFF, EN_OFF, EN_RESTART, EN_RECOVERY, EN_DOWNLOAD, EN_OFF_FAILED };
enum { ST_READY, ST_MISSING, ST_COMING };
enum { SRC_NONE, SRC_ROW, SRC_PILL, SRC_RECT };
enum { RES_NONE, RES_PICK, RES_DOWNLOAD, RES_LAB };

struct rc { float x, y, w, h; };
/* a CSS transform with a uniform scale: p' = s p + (bx, by) */
struct xf { float s, bx, by; };

struct row {
    int16_t cat;                    /* catalog index; -1 = the Starbite Lab row (EngineerMode) */
    uint8_t sec, j, st, tag;
    uint16_t tile;
    char name[64];                  /* UTF-8 */
    char det[96];                   /* the .dd line: version, or why it cannot start (UTF-8) */
};

struct sec {
    uint8_t cat, n, r0, pad;
    uint16_t glyph, pad2;
};

static struct row R[NROW];
static unsigned NR;
static struct sec SEC[M7_MAX_SECTIONS];
static unsigned NS;

struct lensx { int init, clip; };

static struct springs {
    struct m7_spring lb[6];         /* LB: x y w h r a (mkLens) */
    struct m7_spring lo[6];         /* LO */
    struct m7_spring scroll, room, back;
    struct m7_spring ctx, lift, sub, alert, sheet, toast;
    struct m7_spring pill_y, pill_s, pill_a;
    struct m7_spring lx, ly, lw, lh, lr, la;
    struct m7_spring run_a, err_a, shake, err_tint;
    struct m7_spring hint;          /* device: the launch hint fades once the pick is returned */
    struct m7_spring open[M7_MAX_SECTIONS];
    struct m7_spring press[NPRESS];
    struct m7_spring rshake[NSHAKE];
} SP;
static uint32_t press_key[NPRESS];
static int rshake_row[NSHAKE];      /* row + 1 */
static struct lensx LBm, LOm;

enum { LX_, LY_, LW_, LH_, LR_, LA_ };

static struct tmr { double due; int kind, a, b; } TM[NTMR];
enum { T_NONE_, T_PRESS_UP, T_CTX_START, T_CTX_AUTO, T_SHEET_START, T_ALERT_END, T_RUN_IN,
       T_ERR_SHOW, T_RETRY_SHOW, T_PILL_HIDE, T_END_ACT };

static struct st {
    int mode, ov, open;
    uint32_t focus, ofocus;
    int def, last, autos;           /* rows (-1 none); auto-start seconds */
    int set_valid;                  /* a settings record exists */
    int cd;
    double cd_t0, cd_dur;
    double now, ensure_until;
    float last_scroll;
    /* options menu (S.ctx) */
    int ctx_vis, ctx_row, ctx_below, ctx_closing;
    float ctx_shift, ctx_mtop;      /* menu top (below) or bottom (above), panel px */
    struct rc ctx_r;                /* the row's rect when it opened */
    int seg_on;                     /* .sg.on: 0..3 or -1 */
    double seg_t;                   /* when .on last changed (background transition .25 s) */
    int seg_prev;
    /* alert */
    int alert, alert_vis;
    float alert_top, alert_h;
    /* sheet */
    int sheet_row, sheet_vis;
    int sw_on;
    double sw_t;                    /* the switch's transition start */
    /* toast */
    int toast_vis, toast_icon;
    double toast_t;
    char toast1[64], toast2[96];
    /* island */
    int pill_kind, pill_icon;       /* 0 countdown, 1 message */
    char pill1[64], pill2[64];
    /* launch (S.run) */
    int run_on, run_cat, run_row, run_err, run_src;
    uint32_t run_src_key;
    float from[5];
    int launch_vis, launch_closing, lnerr_vis, returned;
    char run_stage[64], run_det[64];
    float bar, bar_from, bar_to;
    double bar_t;
    char err_title[96], err_why[64], err_msg[224], err_code[64];
    /* end screens */
    int end_vis, end_kind;
    double end_t;
    char end_det[64];
    /* the loader's side */
    int result, result_pick;
    int no_menu;                    /* boot-next: the launch card over the wallpaper only */
    int refreeze, full;
    float menu_sc;
} S;

/* touch (the concept's ptr) */
static struct ptr {
    int on, drag, lng, inlist, lt_armed;
    float x, y, s0;
    double t0, lt_due;
    uint32_t tgt, pe;
    int nv;
    double vt[6];
    float vs[6];
} P;

/* ================================================================================================
 * small string helpers (no libc)
 * ============================================================================================== */

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        ++n;
    return n;
}

static int seq(const char *a, const char *b)
{
    unsigned k = 0;
    while (a[k] && a[k] == b[k])
        ++k;
    return a[k] == b[k];
}

/* strings built for a frame live here until the next m7_build */
static char ARENA[12288];
static unsigned arena_n;

struct sbuf { char *b; unsigned n, cap; };

static void sb_init(struct sbuf *s, char *b, unsigned cap)
{
    s->b = b;
    s->n = 0;
    s->cap = cap;
    b[0] = 0;
}

static void sb_s(struct sbuf *s, const char *t)
{
    for (unsigned k = 0; t && t[k] && s->n + 1 < s->cap; ++k)
        s->b[s->n++] = t[k];
    s->b[s->n] = 0;
}

static void sb_u(struct sbuf *s, unsigned v)
{
    char d[12];
    unsigned n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof(d));
    while (n && s->n + 1 < s->cap)
        s->b[s->n++] = d[--n];
    s->b[s->n] = 0;
}

/* "0x9A3F1C07" */
static void sb_x8(struct sbuf *s, uint32_t v)
{
    char d[11];
    d[0] = '0';
    d[1] = 'x';
    for (unsigned i = 0; i < 8; ++i) {
        unsigned n = (v >> (28 - 4 * i)) & 15U;
        d[2 + i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    d[10] = 0;
    sb_s(s, d);
}

/* "31.4 MB" */
static void sb_mb(struct sbuf *s, uint64_t bytes)
{
    uint64_t t = (bytes * 10 + (1U << 19)) >> 20;        /* tenths of a MiB */
    if (t >= 10000) {
        sb_u(s, (unsigned)((t + 5) / 10));
    } else {
        sb_u(s, (unsigned)(t / 10));
        sb_s(s, ".");
        sb_u(s, (unsigned)(t % 10));
    }
    sb_s(s, " MB");
}

static void scopy(char *d, unsigned cap, const char *s)
{
    struct sbuf b;
    sb_init(&b, d, cap);
    sb_s(&b, s);
}

/* Catalog strings are in the loader font's code page (tools/store/sbstore2.py GLYPHS: 0x80 + index
 * for ° · … ↑ ↓ ← → µ ± × ■ ▲ ▼ ✓); glass takes UTF-8. */
static void cat_utf8(char *d, unsigned cap, const char *s)
{
    unsigned o = 0;
    for (unsigned i = 0; s && s[i] && o + 1 < cap; ++i) {
        unsigned c = (uint8_t)s[i], cp = c;
        if (c >= 0x80) {
            switch (c) {
            case 0x80: cp = 0xb0; break;
            case 0x81: cp = 0xb7; break;
            case 0x82: cp = 0x2026; break;
            case 0x83: cp = 0x2191; break;
            case 0x84: cp = 0x2193; break;
            case 0x85: cp = 0x2190; break;
            case 0x86: cp = 0x2192; break;
            case 0x87: cp = 0xb5; break;
            case 0x88: cp = 0xb1; break;
            case 0x89: cp = 0xd7; break;
            case 0x8a: cp = 0x25a0; break;
            case 0x8b: cp = 0x25b2; break;
            case 0x8c: cp = 0x25bc; break;
            case 0x8d: cp = 0x2713; break;
            default: cp = '?'; break;
            }
        }
        if (cp < 0x80) {
            d[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 3 > cap)
                break;
            d[o++] = (char)(0xc0 | cp >> 6);
            d[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            if (o + 4 > cap)
                break;
            d[o++] = (char)(0xe0 | cp >> 12);
            d[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            d[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    d[o] = 0;
}

static const char *arena(const char *s)
{
    unsigned n = slen(s);
    if (arena_n + n + 1 > sizeof(ARENA))
        return "";
    char *d = ARENA + arena_n;
    for (unsigned k = 0; k < n; ++k)
        d[k] = s[k];
    d[n] = 0;
    arena_n += n + 1;
    return d;
}

static int is_up(char c) { return c >= 'A' && c <= 'Z'; }
static char lower(char c) { return is_up(c) ? (char)(c + 32) : c; }

static int word_is(const char *w, unsigned n, const char *k)
{
    unsigned i = 0;
    for (; i < n && k[i]; ++i)
        if (w[i] != k[i])
            return 0;
    return i == n && !k[i];
}

/* The loader's upper-case reasons in sentence case, acronyms kept: "PAYLOAD CRC MISMATCH" ->
 * "Payload CRC mismatch" (doc 4.2.5). device: proper nouns too (Linux, Android, Windows, iOS). */
static void sentence(char *d, unsigned cap, const char *s)
{
    unsigned o = 0, i = 0, first = 1;
    if (!s)
        s = "";
    while (s[i] && o + 1 < cap) {
        if (s[i] == ' ' || s[i] == ',' || s[i] == ':' || s[i] == '-' || s[i] == '(' || s[i] == ')' ||
            s[i] == '/') {
            d[o++] = s[i++];
            continue;
        }
        unsigned n = 0;
        while (s[i + n] && s[i + n] != ' ' && s[i + n] != ',' && s[i + n] != ':' && s[i + n] != '-' &&
               s[i + n] != '(' && s[i + n] != ')' && s[i + n] != '/')
            ++n;
        const char *w = s + i, *rep = 0;
        int keep = word_is(w, n, "CRC") || word_is(w, n, "CRC32") || word_is(w, n, "UFS") ||
                   word_is(w, n, "SD") || word_is(w, n, "DTB") || word_is(w, n, "FDT") ||
                   word_is(w, n, "XNU") || word_is(w, n, "UEFI") || word_is(w, n, "OS") ||
                   word_is(w, n, "RAM") || word_is(w, n, "ID") || word_is(w, n, "ACPM") ||
                   word_is(w, n, "EL1");
        if (word_is(w, n, "LINUX"))
            rep = "Linux";
        else if (word_is(w, n, "ANDROID"))
            rep = "Android";
        else if (word_is(w, n, "WINDOWS"))
            rep = "Windows";
        else if (word_is(w, n, "IOS"))
            rep = "iOS";
        else if (word_is(w, n, "MICROSD"))
            rep = "microSD";
        for (unsigned k = 0; k < n && o + 1 < cap; ++k) {
            char c = rep ? rep[k] : w[k];
            if (!rep && !keep)
                c = (first && k == 0) ? c : lower(c);
            d[o++] = c;
        }
        first = 0;
        i += n;
    }
    d[o] = 0;
}

/* ================================================================================================
 * text measurement (glass's arithmetic), cached by content
 * ============================================================================================== */

#define TWC 512
static struct { uint32_t h; uint16_t font, n; float track, w; } TWc[TWC];

static uint32_t fnv(const char *s, unsigned n)
{
    uint32_t h = 2166136261U;
    for (unsigned k = 0; k < n; ++k)
        h = (h ^ (uint8_t)s[k]) * 16777619U;
    return h;
}

static float tw_n(unsigned font, const char *s, unsigned n, float track)
{
    if (!n)
        return 0;
    uint32_t h = fnv(s, n) ^ font * 0x9e3779b9U;
    unsigned i = h & (TWC - 1);
    if (TWc[i].h == h && TWc[i].font == font && TWc[i].n == n && TWc[i].track == track)
        return TWc[i].w;
    float w = g7_text_w(font, s, n, track);
    TWc[i].h = h;
    TWc[i].font = (uint16_t)font;
    TWc[i].n = (uint16_t)n;
    TWc[i].track = track;
    TWc[i].w = w;
    return w;
}

static float tw(unsigned font, const char *s, float track) { return tw_n(font, s, slen(s), track); }

/* line breaks as CSS normal wrapping (at spaces); returns the line count, starts and lengths */
#define WRAP_MAX 6
static unsigned wrap(unsigned font, const char *s, float track, float maxw, unsigned *st, unsigned *ln)
{
    unsigned n = slen(s), pos = 0, lines = 0;
    while (pos < n && lines < WRAP_MAX) {
        while (s[pos] == ' ')
            ++pos;
        if (pos >= n)
            break;
        unsigned best = 0, k = pos;
        for (;;) {
            while (k < n && s[k] != ' ')
                ++k;
            if (!best || tw_n(font, s + pos, k - pos, track) <= maxw + 0.01f)
                best = k - pos;
            else
                break;
            while (k < n && s[k] == ' ')
                ++k;
            if (k >= n)
                break;
        }
        st[lines] = pos;
        ln[lines] = best;
        ++lines;
        pos += best;
    }
    return lines;
}

/* ================================================================================================
 * the catalog as provider sections (doc 4.2.2; the concept's P, PB, EB)
 * ============================================================================================== */

static unsigned sec_cat_at(unsigned k)
{
    switch (k) {
    case 0: return CAT_APPLE;
    case 1: return CAT_MICROSOFT;
    case 2: return CAT_ANDROID;
    case 3: return CAT_LINUX;
    case 4: return CAT_STARBITE;
    case 5: return CAT_OTHER;
    default: return CAT_LAB;
    }
}

static const char *cat_title(unsigned c)
{
    switch (c) {
    case CAT_APPLE: return "Apple";
    case CAT_MICROSOFT: return "Microsoft";
    case CAT_ANDROID: return "Google";
    case CAT_LINUX: return "Linux";
    case CAT_STARBITE: return "Starbite";
    case CAT_LAB: return "Lab";
    default: return "Other";
    }
}

static unsigned cat_glyph(unsigned c)
{
    switch (c) {
    case CAT_APPLE: return SB7_P_APPLE;
    case CAT_MICROSOFT: return SB7_P_MICROSOFT;
    case CAT_ANDROID: return SB7_P_GOOGLE;
    case CAT_LINUX: return SB7_P_LINUX;
    case CAT_STARBITE: return SB7_P_STARBITE;
    case CAT_LAB: return SB7_P_LAB;
    default: return SB7_P_OTHER;
    }
}

static void set4(float *t, float a, float b, float c, float d)
{
    t[0] = a;
    t[1] = b;
    t[2] = c;
    t[3] = d;
}

static void cat_tint(unsigned c, float *t)
{
    switch (c) {
    case CAT_APPLE: set4(t, M7_TINT_APPLE); break;
    case CAT_MICROSOFT: set4(t, M7_TINT_MS); break;
    case CAT_ANDROID: set4(t, M7_TINT_GOOGLE); break;
    case CAT_LINUX: set4(t, M7_TINT_LINUX); break;
    case CAT_STARBITE: set4(t, M7_TINT_SB); break;
    case CAT_LAB: set4(t, M7_TINT_LAB); break;
    default: set4(t, M7_TINT_OTHER); break;
    }
}

/* Tags (Daily, Beta, ...) hidden on every row, sheet and launch card (the user, 2026-11-05: "we
 * might add those back later"). The catalog keeps them; set M7_SHOW_TAGS 1 to show them again. */
#ifndef M7_SHOW_TAGS
#define M7_SHOW_TAGS 0
#endif

static const char *tag_title(unsigned t)
{
    if (!M7_SHOW_TAGS)
        return 0;
    switch (t) {
    case TAG_DAILY: return "Daily";
    case TAG_BETA: return "Beta";
    case TAG_EXPERIMENTAL: return "Experimental";
    case TAG_LAB: return "Lab";
    case TAG_STABLE: return "Stable";
    default: return 0;
    }
}

static const char *row_key(int r)
{
    if (r < 0 || (unsigned)r >= NR)
        return "";
    return R[r].cat < 0 ? "@lab" : cat_get((unsigned)R[r].cat)->key;
}

/* readiness (UFS / SD reads happen here) */
static void row_state(struct row *w)
{
    if (w->cat < 0) {
        w->st = ST_READY;
        scopy(w->det, sizeof(w->det), "Display · touch · keys · UART · memory · clocks");
        return;
    }
    const struct cat_ent *e = cat_get((unsigned)w->cat);
    const char *why = 0;
    if (!e->file[0][0]) {
        w->st = ST_COMING;
        cat_utf8(w->det, sizeof(w->det), e->version);
    } else if (star_entry_ready((unsigned)w->cat, &why)) {
        w->st = ST_READY;
        cat_utf8(w->det, sizeof(w->det), e->version);
    } else {
        w->st = ST_MISSING;
        sentence(w->det, sizeof(w->det), why ? why : "NOT INSTALLED");
    }
}

static void rows_build(void)
{
    NS = 0;
    NR = 0;
    for (unsigned k = 0; k < 7 && NS < M7_MAX_SECTIONS; ++k) {
        unsigned c = sec_cat_at(k), vis = 0;
        for (unsigned i = 0; i < cat_count(); ++i) {
            const struct cat_ent *e = cat_get(i);
            if (e->category == c && !(e->flags & CF_HIDDEN))
                ++vis;
        }
        if (c == CAT_OTHER && !vis)
            continue;
        struct sec *s = &SEC[NS];
        s->cat = (uint8_t)c;
        s->r0 = (uint8_t)NR;
        s->glyph = (uint16_t)cat_glyph(c);
        unsigned n = 0;
        if (c == CAT_LAB && NR < NROW) {
            struct row *w = &R[NR++];
            w->cat = -1;
            w->sec = (uint8_t)NS;
            w->j = (uint8_t)n++;
            w->tag = TAG_LAB;
            scopy(w->name, sizeof(w->name), "Starbite Lab");
            w->tile = (uint16_t)sb7_tile_for("@lab", CAT_LAB);
        }
        for (unsigned i = 0; i < cat_count() && NR < NROW; ++i) {
            const struct cat_ent *e = cat_get(i);
            if (e->category != c || (e->flags & CF_HIDDEN))
                continue;
            struct row *w = &R[NR++];
            w->cat = (int16_t)i;
            w->sec = (uint8_t)NS;
            w->j = (uint8_t)n++;
            w->tag = e->tag;
            cat_utf8(w->name, sizeof(w->name), e->name);
            w->tile = (uint16_t)sb7_tile_for(e->key, e->category);
        }
        s->n = (uint8_t)n;
        ++NS;
    }
    for (unsigned r = 0; r < NR; ++r)
        row_state(&R[r]);
}

static int row_by_key(const char *key)
{
    if (!key || !key[0])
        return -1;
    for (unsigned r = 0; r < NR; ++r)
        if (seq(row_key((int)r), key))
            return (int)r;
    return -1;
}

static int row_by_cat(int cat)
{
    for (unsigned r = 0; r < NR; ++r)
        if (R[r].cat == cat)
            return (int)r;
    return -1;
}

static int sec_by_cat(unsigned c)
{
    for (unsigned i = 0; i < NS; ++i)
        if (SEC[i].cat == c)
            return (int)i;
    return -1;
}

static uint32_t rkey(int r) { return FK(F_R, R[r].sec, R[r].j); }

static int key_row(uint32_t f)
{
    if (FKIND(f) != F_R || FA(f) >= NS || FB(f) >= SEC[FA(f)].n)
        return -1;
    return SEC[FA(f)].r0 + (int)FB(f);
}

/* ================================================================================================
 * transforms and layout (the CSS: #content padding 64 72 260, #cards gap 32, .card / .rows)
 * ============================================================================================== */

static struct xf xf_id(void)
{
    struct xf x = { 1, 0, 0 };
    return x;
}

/* translate(tx, ty) scale(s) with transform-origin (ox, oy) */
static struct xf xf_about(float s, float ox, float oy, float tx, float ty)
{
    struct xf x;
    x.s = s;
    x.bx = ox - s * ox + tx;
    x.by = oy - s * oy + ty;
    return x;
}

static struct xf xf_cat(struct xf o, struct xf i)          /* o(i(p)) */
{
    struct xf x;
    x.s = o.s * i.s;
    x.bx = o.s * i.bx + o.bx;
    x.by = o.s * i.by + o.by;
    return x;
}

static struct rc xf_rc(struct xf x, struct rc r)
{
    struct rc o;
    o.x = x.s * r.x + x.bx;
    o.y = x.s * r.y + x.by;
    o.w = r.w * x.s;
    o.h = r.h * x.s;
    return o;
}

static struct rc rcv(float x, float y, float w, float h)
{
    struct rc r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

/* press springs (the concept's press Map): 1 when the element has none */
static float press_of(uint32_t k)
{
    for (unsigned i = 0; i < NPRESS; ++i)
        if (press_key[i] == k)
            return SP.press[i].x;
    return 1;
}

static float rshake_of(int r)
{
    for (unsigned i = 0; i < NSHAKE; ++i)
        if (rshake_row[i] == r + 1)
            return SP.rshake[i].x;
    return 0;
}

static struct lay {
    const struct m7_xf *ix;         /* the intro's transforms, or 0 */
    float off, cy, cards_h, foot_y, content_h;
    float card_y[M7_MAX_SECTIONS], rows_h[M7_MAX_SECTIONS];
    struct xf cxf[M7_MAX_SECTIONS];
} L;

#define C_BRANDY 64.0f               /* from #content's top */
#define C_TITLE 146.0f
#define C_SUB 316.0f
#define C_CARDS 459.0f
#define FOOT_H 117.8f               /* two lines of 38 px at line-height 1.55 */

static float rows_full(unsigned i) { return (float)SEC[i].n * M7_ROW_H + M7_ROWS_PAD; }

static void layout(void)
{
    L.off = SP.scroll.x - SP.room.x;
    L.cy = M7_VP_Y0 - L.off;
    float y = L.cy + C_CARDS, y0 = y;
    for (unsigned i = 0; i < NS; ++i) {
        float rh = maxf(0, SP.open[i].x * rows_full(i));
        L.card_y[i] = y;
        L.rows_h[i] = rh;
        if (L.ix)
            L.cxf[i] = xf_about(L.ix->card_s[i], 720, y, 0, L.ix->card_dy[i]);
        else
            L.cxf[i] = xf_about(press_of(FK(F_P, i, 0)), 720, y, 0, 0);
        y += M7_CARD_HEAD + rh + M7_CARD_GAP;
    }
    L.cards_h = NS ? y - M7_CARD_GAP - y0 : 0;
    L.foot_y = y0 + L.cards_h + 64;
    L.content_h = C_CARDS + L.cards_h + 64 + FOOT_H + M7_CONTENT_PAD_BOT;
}

static float max_scroll(void) { return maxf(0, L.content_h - (M7_VP_Y1 - M7_VP_Y0)); }

/* a row's opacity as the section opens (update(): rows fade and rise 24 px, staggered) */
static float row_alpha(int r)
{
    unsigned i = R[r].sec, n = SEC[i].n;
    float o = SP.open[i].x;
    return c01((o * 1.25f - (float)R[r].j * 0.07f / maxf(1, (float)n / 7)) / 0.45f);
}

static struct rc row_box(int r)
{
    return rcv(M7_X0, L.card_y[R[r].sec] + M7_CARD_HEAD + (float)R[r].j * M7_ROW_H, M7_CW, M7_ROW_H);
}

/* translate(shake, (1 - a) * -24) scale(press) about the row's centre, inside its card */
static struct xf row_xf(int r)
{
    struct rc b = row_box(r);
    float a = row_alpha(r);
    struct xf x = xf_about(press_of(rkey(r)), b.x + b.w / 2, b.y + b.h / 2, rshake_of(r), (1 - a) * -24);
    return xf_cat(L.cxf[R[r].sec], x);
}

#define TB_W (1016.0f / 3.0f)       /* .tb: (1044 - 2 * 14) / 3 */

static float dock_dy(void) { return L.ix ? L.ix->dock_dy : 0; }

static struct rc tb_box(unsigned b)
{
    if (b == T_POWER)
        return rcv(M7_DOCK_POWER_X, M7_DOCK_Y + dock_dy(), M7_DOCK_H, M7_DOCK_H);
    return rcv(86 + (float)b * TB_W, M7_DOCK_Y + 14 + dock_dy(), TB_W, 184);
}

static struct xf tb_xf(unsigned b)
{
    struct rc r = tb_box(b);
    return xf_about(press_of(FK(F_T, 0, b)), r.x + r.w / 2, r.y + r.h / 2, 0, 0);
}

/* ---- overlay geometry ---- */

#define CTX_W 940.0f
#define CTX_H 804.0f
#define CTX_X 250.0f

static float ctx_top(void)
{
    return S.ctx_below ? S.ctx_mtop : S.ctx_mtop - CTX_H;
}

static struct xf ctx_xf(void)
{
    float s = 0.25f + 0.75f * SP.ctx.x;
    float oy = S.ctx_below ? ctx_top() : ctx_top() + CTX_H;
    return xf_about(s, CTX_X + CTX_W / 2, oy, 0, 0);
}

/* the preview: translateY(-shift * lift) scale(1 + .04 lift) about its centre */
static struct xf prev_xf(void)
{
    float l = SP.lift.x;
    return xf_about(1 + 0.04f * l, S.ctx_r.x + S.ctx_r.w / 2, S.ctx_r.y + S.ctx_r.h / 2, 0,
                    -S.ctx_shift * l);
}

/* an options menu item's box inside #ctxMenu (22 px padding; .mi 148, .msep 2 + 2 x 8, .mlab 96,
 * .seg 18 + 104 + 26, .msep.thick 16 + 2 x 10) */
static struct rc ctx_item(unsigned b)
{
    float t = ctx_top();
    switch (b) {
    case MI_START: return rcv(CTX_X, t + 22, CTX_W, 148);
    case MI_DEFAULT: return rcv(CTX_X, t + 188, CTX_W, 148);
    case MI_DETAILS: return rcv(CTX_X, t + 634, CTX_W, 148);
    default: return rcv(CTX_X + 44 + (float)(b - MI_AUTO0) * 216.5f, t + 468, 202.5f, 104);
    }
}

static float alert_s(void) { return 1.12f - 0.12f * clampf(SP.alert.x, 0, 1.05f); }

static struct xf alert_xf(void)
{
    return xf_about(alert_s(), 720, S.alert_top + S.alert_h / 2, 0, 0);
}

static unsigned alert_nb(void) { return S.alert == AL_POWER ? 3 : 2; }

static unsigned alert_act(unsigned i)
{
    if (S.alert == AL_POWER)
        return i == 0 ? A_OFF : i == 1 ? A_RESTART : A_CANCEL;
    return i == 0 ? (S.alert == AL_RECOVERY ? A_RECOVERY : A_DOWNLOAD) : A_CANCEL;
}

static float alert_btn_top(void);

static struct rc alert_btn(unsigned i)
{
    return rcv(M7_ALERT_X + 72, S.alert_top + alert_btn_top() + (float)i * 186, M7_ALERT_W - 144, 160);
}

static float sheet_ty(void) { return (1 - SP.sheet.x) * 1960; }

static unsigned sheet_nrows(void);

static struct rc sheet_el(unsigned b)
{
    float ty = sheet_ty();
    if (b == SH_CLOSE)
        return rcv(1220, 1132 + ty, 136, 136);
    if (b == SH_DEF)
        return rcv(96, 1470 + (float)(sheet_nrows() - 2) * 138 + ty, 1248, 138);
    return rcv(96, 2688 + ty, 1248, 176);
}

/* #lnErr: scale(1.06 - .06 a) about the screen's centre, #lnC shifted by .6 of the shake */
static struct xf err_xf(void)
{
    return xf_about(1.06f - 0.06f * SP.err_a.x, 720, 1480, SP.shake.x * 0.6f, 0);
}

static struct xf run_xf(void)
{
    return xf_about(0.94f + 0.06f * SP.run_a.x, 720, 1480, SP.shake.x * 0.6f, 0);
}

static struct rc err_btn(unsigned b) { return rcv(230, b == E_RETRY ? 2330 : 2540, 980, 176); }

/* fel(key) + rectP: the element's box after every transform; menu elements with #menu's
 * scale(menuSc) about (720, 1332) when menu is set */
static int rect_of(uint32_t k, struct rc *o, int menu)
{
    unsigned a = FA(k), b = FB(k);
    struct rc r;
    switch (FKIND(k)) {
    case F_P:
        if (a >= NS)
            return 0;
        r = xf_rc(L.cxf[a], rcv(M7_X0, L.card_y[a], M7_CW, M7_CARD_HEAD));
        break;
    case F_R: {
        int w = key_row(k);
        if (w < 0)
            return 0;
        r = xf_rc(row_xf(w), row_box(w));
        break;
    }
    case F_T:
        if (b > T_POWER)
            return 0;
        r = xf_rc(tb_xf(b), tb_box(b));
        break;
    case F_M: {
        if (!S.ctx_vis)
            return 0;
        struct rc m = ctx_item(b);
        r = xf_rc(ctx_xf(), m);
        *o = r;
        return 1;
    }
    case F_A: {
        if (!S.alert_vis)
            return 0;
        for (unsigned i = 0; i < alert_nb(); ++i)
            if (alert_act(i) == b) {
                struct rc m = alert_btn(i);
                struct xf p = xf_about(press_of(k), m.x + m.w / 2, m.y + m.h / 2, 0, 0);
                *o = xf_rc(xf_cat(alert_xf(), p), m);
                return 1;
            }
        return 0;
    }
    case F_SH:
        if (!S.sheet_vis || (b == SH_DEF && R[S.sheet_row].st != ST_READY))
            return 0;
        *o = sheet_el(b);
        return 1;
    case F_E: {
        if (!S.lnerr_vis)
            return 0;
        struct rc m = err_btn(b);
        struct xf p = xf_about(press_of(k), m.x + m.w / 2, m.y + m.h / 2, 0, 0);
        *o = xf_rc(xf_cat(err_xf(), p), m);
        return 1;
    }
    default:
        return 0;
    }
    if (menu && S.menu_sc != 1)
        r = xf_rc(xf_about(S.menu_sc, 720, 1332, 0, 0), r);
    *o = r;
    return 1;
}

/* ================================================================================================
 * focus lenses (mkLens, lensTarget, lensStep, lensShape)
 * ============================================================================================== */

struct ltarget { struct rc r; float rad; int clip; };

static int lens_target(uint32_t k, struct ltarget *T)
{
    struct rc r;
    if (!k || !rect_of(k, &r, 1) || r.w < 2)
        return 0;
    float ix = 0, iy = 0, rad = r.h / 2;
    unsigned kind = FKIND(k), b = FB(k);
    if (kind == F_P) {
        ix = 16;
        iy = 16;
        rad = 80;
    } else if (kind == F_R) {
        ix = 16;
        iy = 8;
        rad = 62;
    } else if ((kind == F_T && b == T_POWER) || (kind == F_SH && b == SH_CLOSE)) {
        ix = 12;
        iy = 12;
        rad = (r.h - 24) / 2;
    } else if (kind == F_T) {
        rad = r.h / 2;
    } else if (kind == F_M && b >= MI_AUTO0 && b <= MI_AUTO10) {
        rad = r.h / 2;
    } else if (kind == F_M) {
        ix = 16;
        iy = 4;
        rad = 58;
    } else if (kind == F_SH && b == SH_DEF) {
        ix = 10;
        iy = 8;
        rad = 56;
    }
    T->r = rcv(r.x + ix, r.y + iy, r.w - 2 * ix, r.h - 2 * iy);
    T->rad = rad;
    T->clip = kind == F_P || kind == F_R;
    return 1;
}

static void lens_step(struct m7_spring *L, struct lensx *m, uint32_t key, int show, int snap)
{
    struct ltarget T;
    int have = show && key && lens_target(key, &T);
    if (have) {
        if (!m->init || snap) {
            m7_spring_snap(&L[LX_], T.r.x);
            m7_spring_snap(&L[LY_], T.r.y);
            m7_spring_snap(&L[LW_], T.r.w);
            m7_spring_snap(&L[LH_], T.r.h);
            m7_spring_snap(&L[LR_], T.rad);
            m->init = 1;
        } else {
            L[LX_].t = T.r.x;
            L[LY_].t = T.r.y;
            L[LW_].t = T.r.w;
            L[LH_].t = T.r.h;
            L[LR_].t = T.rad;
        }
        m->clip = T.clip;
    }
    L[LA_].t = have ? 1 : 0;
    if (snap)
        m7_spring_snap(&L[LA_], L[LA_].t);
}

/* ================================================================================================
 * scrolling (measureScroll, ensureVisible)
 * ============================================================================================== */

static void ensure_visible(void)
{
    uint32_t f = S.focus;
    struct rc r;
    if (!f || FKIND(f) == F_T || !rect_of(f, &r, 0))
        return;
    float top = r.y - L.cy, bot = r.y + r.h - L.cy, vh = 2450 - SP.room.t, t = SP.scroll.t;
    if (top < t + 150)
        t = top - 150;
    else if (bot > t + vh - 200)
        t = bot - vh + 200;
    SP.scroll.t = clampf(t, 0, max_scroll());
}

static void ensure_visible_now(void)
{
    layout();
    ensure_visible();
    m7_spring_snap(&SP.scroll, SP.scroll.t);
    layout();
}

/* ================================================================================================
 * timers (the concept's setTimeout calls)
 * ============================================================================================== */

static void tmr_add(double ms, int kind, int a, int b)
{
    for (unsigned i = 0; i < NTMR; ++i)
        if (TM[i].kind == T_NONE_) {
            TM[i].due = S.now + ms;
            TM[i].kind = kind;
            TM[i].a = a;
            TM[i].b = b;
            return;
        }
}

/* ================================================================================================
 * press and shake (pressEl, pulse, shakeRow)
 * ============================================================================================== */

static struct m7_spring *press_slot(uint32_t k)
{
    int free = -1;
    for (unsigned i = 0; i < NPRESS; ++i) {
        if (press_key[i] == k)
            return &SP.press[i];
        if (free < 0 && (!press_key[i] || (!sp_moving(&SP.press[i]) && SP.press[i].x == 1)))
            free = (int)i;
    }
    if (free < 0)
        return 0;
    press_key[free] = k;
    m7_spring_init(&SP.press[free], 1, 0.3f, 0.55f, 0.0005f);
    return &SP.press[free];
}

static void press_el(uint32_t k, int down)
{
    if (!k)
        return;
    struct m7_spring *s = press_slot(k);
    if (!s)
        return;
    if (down) {
        m7_spring_cfg(s, 0.18f, 1);
        s->t = 0.965f;
    } else {
        m7_spring_cfg(s, 0.3f, 0.55f);
        s->t = 1;
    }
}

static void pulse(uint32_t k)
{
    press_el(k, 1);
    tmr_add(90, T_PRESS_UP, (int)k, 0);
}

/* the element a press acts on: a section's key presses its card (the same key here) */
static uint32_t press_target(uint32_t f) { return f; }

static void shake_row(int r)
{
    int slot = -1;
    for (unsigned i = 0; i < NSHAKE; ++i)
        if (rshake_row[i] == r + 1)
            slot = (int)i;
    for (unsigned i = 0; slot < 0 && i < NSHAKE; ++i)
        if (!rshake_row[i] || !sp_moving(&SP.rshake[i])) {
            slot = (int)i;
            rshake_row[i] = r + 1;
            m7_spring_init(&SP.rshake[i], 0, 0.28f, 0.32f, 0.05f);
        }
    if (slot >= 0)
        m7_spring_kick(&SP.rshake[slot], -2600);
}

/* ================================================================================================
 * settings (doc 4.2.3): the record in the UFS store, through menu.c
 * ============================================================================================== */

static void settings_write(void)
{
    int valid = 0;
    struct star_settings *st = star_menu_settings(&valid);
    const char *k = S.def >= 0 ? row_key(S.def) : "";
    unsigned i = 0;
    for (; i < 23 && k[i]; ++i)
        st->default_key[i] = k[i];
    for (; i < 24; ++i)
        st->default_key[i] = 0;
    st->timeout_s = (uint32_t)S.autos;
    if (!star_menu_settings_save())
        S.set_valid = 1;
}

static void refresh_rows(void)
{
    if (SP.back.x > 0.002f)
        S.refreeze = 1;                 /* the frozen menu behind the overlay shows the new tags */
}

/* setDefault: the first one on a phone without a record turns auto-start on at 5 s */
static void set_default(int r)
{
    S.def = r;
    if (!S.set_valid && !S.autos)
        S.autos = 5;
    settings_write();
    refresh_rows();
}

/* ================================================================================================
 * the island (pillCountdown, pillMsg, startCountdown, hidePill, cancelCountdown) and the notice
 * ============================================================================================== */

enum { IC_CHECK, IC_TIMER, IC_INFO, IC_FLASK, IC_STARF };

static void pill_msg(int icon, const char *t1, const char *t2)
{
    S.pill_kind = 1;
    S.pill_icon = icon;
    scopy(S.pill1, sizeof(S.pill1), t1);
    scopy(S.pill2, sizeof(S.pill2), t2);
}

static int def_ready(void) { return S.def >= 0 && R[S.def].st == ST_READY && R[S.def].cat >= 0; }

static void pill_countdown(void) { S.pill_kind = 0; }

static void hide_pill(void)
{
    SP.pill_s.t = 0.6f;
    SP.pill_a.t = 0;
}

static void start_countdown(void)
{
    if (S.autos <= 0 || !def_ready()) {
        pill_msg(IC_CHECK, "Ready", "Auto-start is off");
        SP.pill_a.t = 1;
        SP.pill_s.t = 1;
        tmr_add(1700, T_PILL_HIDE, 0, 0);
        return;
    }
    S.cd = 1;
    S.cd_t0 = S.now;
    S.cd_dur = S.autos * 1000.0;
    pill_countdown();
    SP.pill_a.t = 1;
    SP.pill_s.t = 1;
    SP.pill_y.t = 0;
}

static void cancel_countdown(void)
{
    if (!S.cd)
        return;
    S.cd = 0;
    pill_msg(IC_TIMER, "Auto-start Off", "Choose a system");
    m7_spring_snap(&SP.pill_s, 1);
    m7_spring_kick(&SP.pill_s, -1.2f);
    tmr_add(1100, T_PILL_HIDE, 0, 0);
}

static void toast(int icon, const char *t, const char *s)
{
    S.toast_icon = icon;
    scopy(S.toast1, sizeof(S.toast1), t);
    scopy(S.toast2, sizeof(S.toast2), s);
    S.toast_vis = 1;
    SP.toast.t = 1;
    if (SP.toast.x < 0.05f) {
        m7_spring_snap(&SP.toast, 0);
        SP.toast.t = 1;
    }
    S.toast_t = S.now + 2100;
    if (!S.cd && SP.pill_a.t > 0) {
        m7_spring_snap(&SP.pill_a, 0);
        m7_spring_snap(&SP.pill_s, 0.6f);
    }
}

/* ================================================================================================
 * menu behaviour (menuKeys, setFocus, moveFocus, openProv, closeProv, toggleProv, ...)
 * ============================================================================================== */

static unsigned menu_keys(uint32_t *k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NS; ++i) {
        k[n++] = FK(F_P, i, 0);
        if (S.open == (int)i)
            for (unsigned j = 0; j < SEC[i].n; ++j)
                k[n++] = FK(F_R, i, j);
    }
    for (unsigned b = 0; b < 4; ++b)
        k[n++] = FK(F_T, 0, b);
    return n;
}

static void set_focus(uint32_t k, int snap)
{
    S.focus = k;
    S.ensure_until = S.now + 900;
    if (snap) {
        layout();
        lens_step(SP.lb, &LBm, k, 1, 1);
    }
}

static void move_focus(int d)
{
    uint32_t ks[M7_MAX_SECTIONS + NROW + 4];
    unsigned n = menu_keys(ks);
    int i = 0;
    for (unsigned j = 0; j < n; ++j)
        if (ks[j] == S.focus)
            i = (int)j;
    int j = i + d;
    if (j < 0 || j >= (int)n) {
        m7_spring_kick(&SP.lb[LY_], (float)d * 900);
        return;
    }
    set_focus(ks[j], 0);
}

static void open_prov(int i, uint32_t focus_row)
{
    S.open = i;
    for (unsigned c = 0; c < NS; ++c)
        SP.open[c].t = (int)c == i ? 1 : 0;
    if (focus_row)
        set_focus(focus_row, 0);
    S.ensure_until = S.now + 900;
}

static void close_prov(void)
{
    int i = S.open;
    S.open = -1;
    for (unsigned c = 0; c < NS; ++c)
        SP.open[c].t = 0;
    if (i >= 0)
        set_focus(FK(F_P, i, 0), 0);
}

static void toggle_prov(int i)
{
    if (S.open == i)
        close_prov();
    else
        open_prov(i, 0);
    set_focus(FK(F_P, i, 0), 0);
}

/* device: a section without systems does not open */
static int prov_empty(int i)
{
    if (SEC[i].n)
        return 0;
    char b[96];
    struct sbuf s;
    sb_init(&s, b, sizeof(b));
    sb_s(&s, cat_title(SEC[i].cat));
    sb_s(&s, " systems are coming soon");
    toast(IC_INFO, "Nothing Here Yet", b);
    return 1;
}

static void close_overlays(int keep_back);
static void launch(int cat, int row, int src, uint32_t src_key, const struct rc *src_rect);
static void open_alert(int kind);
static void open_ctx(int r);
static void open_sheet(int r);

static int preview_lab;             /* m7_preview_state("lab"): the concept's goLab */

static int lab_os;                  /* this Lab request may start Engineer OS (go_lab_os) */

static void go_lab(void)
{
    if (S.mode != MD_MENU)
        return;
    close_overlays(0);
    if (preview_lab) {
        int i = sec_by_cat(CAT_LAB);
        int d = row_by_key("display");  /* the concept's openProv('lab','r:lab:display') */
        if (i >= 0 && SEC[i].n)
            open_prov(i, d >= 0 && R[d].sec == i ? rkey(d) : FK(F_R, i, 0));
        toast(IC_FLASK, "Lab", "Engineering tools for Galaxy S9");
        return;
    }
    S.result = RES_LAB;             /* D10: EngineerMode, run by menu7_main */
    lab_os = 0;
}

/* Engineer OS (docs/ENGINEER-OS.md): the Lab row and the dock's Lab button start the "engineer"
 * catalog entry when it is installed (run_lab); holding Bixby (go_lab) always opens Engineer Lite,
 * the loader's EngineerMode, which also stays the fallback when the entry is missing. */
static void go_lab_os(void)
{
    go_lab();
    lab_os = S.result == RES_LAB;
}

static void start_entry(int r, uint32_t src)
{
    if (R[r].st != ST_READY) {
        shake_row(r);
        char b[96];
        struct sbuf s;
        sb_init(&s, b, sizeof(b));
        sb_s(&s, R[r].name);
        sb_s(&s, R[r].st == ST_COMING ? " Is Coming Soon" : " Isn’t Installed");
        toast(IC_INFO, b, R[r].st == ST_COMING ? "Not available in this build" : "Add it with USB Toolkit");
        return;
    }
    if (R[r].cat < 0) {
        go_lab_os();                /* the Lab row: Engineer OS when installed */
        return;
    }
    launch(R[r].cat, r, src ? SRC_ROW : SRC_NONE, src, 0);
}

static void menu_activate(void)
{
    uint32_t k = S.focus;
    switch (FKIND(k)) {
    case F_P:
        pulse(k);
        if (!prov_empty((int)FA(k)))
            toggle_prov((int)FA(k));
        break;
    case F_R: {
        int r = key_row(k);
        pulse(k);
        if (r >= 0)
            start_entry(r, k);
        break;
    }
    case F_T:
        pulse(k);
        if (FB(k) == T_LAB)
            go_lab_os();            /* the dock's Lab button: Engineer OS when installed */
        else
            open_alert(FB(k) == T_POWER ? AL_POWER : FB(k) == T_RECOVERY ? AL_RECOVERY : AL_DOWNLOAD);
        break;
    default:
        break;
    }
}

static void menu_back(void)
{
    uint32_t k = S.focus;
    if (FKIND(k) == F_R)
        close_prov();
    else if (FKIND(k) == F_P && S.open >= 0)
        close_prov();
    else if (FKIND(k) == F_T)
        set_focus(S.open >= 0 ? FK(F_P, S.open, 0) : FK(F_P, 0, 0), 0);
    else
        toast(IC_FLASK, "Hold Bixby for Lab", "Bixby goes back in menus");
}

/* ================================================================================================
 * options menu (openCtx, ctxKeys, ctxActivate, closeCtx)
 * ============================================================================================== */

static int ctx_ok(void) { return R[S.ctx_row].st == ST_READY; }
static int ctx_def_ok(void) { return ctx_ok() && R[S.ctx_row].cat >= 0; }    /* device: not Lab */
static int ctx_is_def(void) { return S.ctx_row == S.def; }

static int auto_val(unsigned b)
{
    return b == MI_AUTO3 ? 3 : b == MI_AUTO5 ? 5 : b == MI_AUTO10 ? 10 : 0;
}

static unsigned auto_item(int v)
{
    return v == 3 ? MI_AUTO3 : v == 5 ? MI_AUTO5 : v == 10 ? MI_AUTO10 : MI_AUTO0;
}

static void open_ctx(int r)
{
    if (S.mode != MD_MENU)
        return;
    struct rc rr;
    layout();
    if (!rect_of(rkey(r), &rr, 1))
        return;
    S.ov = OV_CTX;
    S.ctx_row = r;
    S.ctx_vis = 1;
    S.ctx_closing = 0;
    int ok = ctx_ok(), cur = ctx_is_def() ? S.autos : 0;
    float mh = CTX_H;
    int below = rr.y + rr.h + 28 + mh < 2860;
    S.ctx_shift = 0;
    if (!below && rr.y - 28 - mh < 120) {
        S.ctx_shift = (rr.y + rr.h + 28 + mh) - 2860;
        below = 1;
    }
    S.ctx_below = below;
    S.ctx_mtop = below ? rr.y + rr.h + 28 - S.ctx_shift : rr.y - 28;
    S.ctx_r = rr;
    S.seg_on = ok && ctx_def_ok() ? (int)auto_item(cur) - MI_AUTO0 : -1;
    S.seg_prev = S.seg_on;
    S.seg_t = -1e9;
    m7_spring_snap(&SP.ctx, 0);
    SP.ctx.t = 1;
    SP.lift.t = 1;
    m7_spring_snap(&SP.sub, 0);
    SP.back.t = 1;
    S.ofocus = ok ? FK(F_M, 0, MI_START) : FK(F_M, 0, MI_DETAILS);
    LOm.init = 0;
}

static int ctx_item_on(unsigned b)
{
    if (b == MI_START)
        return ctx_ok();
    if (b == MI_DEFAULT)
        return ctx_def_ok() && !ctx_is_def();
    if (b >= MI_AUTO0 && b <= MI_AUTO10)
        return ctx_def_ok();
    return 1;
}

static unsigned ctx_keys(uint32_t *k)
{
    unsigned n = 0;
    for (unsigned b = MI_START; b <= MI_DETAILS; ++b)
        if (ctx_item_on(b))
            k[n++] = FK(F_M, 0, b);
    return n;
}

static void close_ctx(int keep_back, int to_sheet)
{
    if (S.ov != OV_CTX)
        return;
    S.ov = OV_NONE;
    SP.ctx.t = 0;
    SP.lift.t = 0;
    SP.sub.t = 0;
    if (!keep_back && !to_sheet)
        SP.back.t = 0;
    S.ctx_closing = 1;
}

static void ctx_activate(uint32_t k)
{
    if (S.ov != OV_CTX)
        return;
    int r = S.ctx_row;
    unsigned b = FB(k);
    if (!ctx_item_on(b))
        return;
    pulse(k);
    char t[96], u[96];
    struct sbuf s1, s2;
    sb_init(&s1, t, sizeof(t));
    sb_init(&s2, u, sizeof(u));
    if (b == MI_START) {
        close_ctx(1, 0);
        tmr_add(40, T_CTX_START, r, 0);
    } else if (b == MI_DEFAULT) {
        set_default(r);
        close_ctx(0, 0);
        sb_s(&s1, R[r].name);
        sb_s(&s1, " Is the Default");
        if (S.autos) {
            sb_s(&s2, "Starts automatically after ");
            sb_u(&s2, (unsigned)S.autos);
            sb_s(&s2, " s");
        } else {
            sb_s(&s2, "Auto-start is off");
        }
        toast(IC_STARF, t, u);
    } else if (b >= MI_AUTO0 && b <= MI_AUTO10) {
        int v = auto_val(b);
        S.autos = v;
        if (v)
            S.def = r;
        settings_write();
        refresh_rows();
        if (S.seg_on != (int)(b - MI_AUTO0)) {
            S.seg_prev = S.seg_on;
            S.seg_on = (int)(b - MI_AUTO0);
            S.seg_t = S.now;
        }
        tmr_add(380, T_CTX_AUTO, v, r);
    } else if (b == MI_DETAILS) {
        close_ctx(0, 1);
        open_sheet(r);
    }
}

/* ================================================================================================
 * alerts (ALERTS, openAlert, alertKeys, alertActivate, closeAlert)
 * ============================================================================================== */

static const char *alert_title(void)
{
    return S.alert == AL_POWER ? "Power Off?" : S.alert == AL_RECOVERY ? "Restart to TWRP?" :
           "Enter Download Mode?";
}

static const char *alert_msg(void)
{
    return S.alert == AL_POWER ? "Galaxy S9 will shut down. Hold Power to turn it on again." :
           S.alert == AL_RECOVERY ?
           "Galaxy S9 restarts into TWRP recovery. Starbite comes back on the next normal start." :
           "Odin can flash Galaxy S9 over USB. Keep the cable connected until it finishes.";
}

static const char *alert_btn_text(unsigned act)
{
    switch (act) {
    case A_OFF: return "Power Off";
    case A_RESTART: return "Restart";
    case A_RECOVERY: return "Restart to TWRP";
    case A_DOWNLOAD: return "Enter Download Mode";
    default: return "Cancel";
    }
}

#define AL_MSG_W (M7_ALERT_W - 144)
#define AL_LH 66.24f
static unsigned alert_lines(void)
{
    unsigned st[WRAP_MAX], ln[WRAP_MAX];
    return wrap(SB7_F_IN400_48, alert_msg(), 0, AL_MSG_W, st, ln);
}

/* padding 76, icon 150 + 46, title 75.6, message 26 + lines x 66.24, buttons 64 + ... */
static float alert_btn_top(void) { return 76 + 150 + 46 + 75.6f + 26 + (float)alert_lines() * AL_LH + 64; }

static void open_alert(int kind)
{
    S.ov = OV_ALERT;
    S.alert = kind;
    S.alert_vis = 1;
    unsigned nb = alert_nb();
    S.alert_h = alert_btn_top() + (float)nb * 160 + (float)(nb - 1) * 26 + 66;
    S.alert_top = (PY - S.alert_h) / 2 - 40;
    m7_spring_snap(&SP.alert, 0);
    SP.alert.t = 1;
    SP.back.t = 1;
    S.ofocus = FK(F_A, 0, alert_act(nb - 1));
    LOm.init = 0;
}

static unsigned alert_keys(uint32_t *k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < alert_nb(); ++i)
        k[n++] = FK(F_A, 0, alert_act(i));
    return n;
}

static void close_alert(int keep)
{
    if (S.ov != OV_ALERT)
        return;
    S.ov = OV_NONE;
    SP.alert.t = 0;
    if (!keep)
        SP.back.t = 0;
}

static void alert_activate(uint32_t k)
{
    unsigned act = FB(k);
    pulse(k);
    close_alert(act != A_CANCEL);
    if (act != A_CANCEL)
        tmr_add(120, T_ALERT_END, (int)act, 0);
}

/* ================================================================================================
 * Details sheet (openSheet, sheetKeys, sheetActivate, closeSheet)
 * ============================================================================================== */

static void sheet_info(int r);

static void open_sheet(int r)
{
    sheet_info(r);
    S.ov = OV_SHEET;
    S.sheet_row = r;
    S.sheet_vis = 1;
    S.sw_on = r == S.def;
    S.sw_t = -1e9;
    m7_spring_snap(&SP.sheet, 0);
    SP.sheet.t = 1;
    SP.back.t = 1;
    S.ofocus = R[r].st == ST_READY ? FK(F_SH, 0, SH_START) : FK(F_SH, 0, SH_CLOSE);
    LOm.init = 0;
}

static unsigned sheet_keys(uint32_t *k)
{
    unsigned n = 0;
    k[n++] = FK(F_SH, 0, SH_CLOSE);
    if (R[S.sheet_row].st == ST_READY && R[S.sheet_row].cat >= 0)
        k[n++] = FK(F_SH, 0, SH_DEF);
    k[n++] = FK(F_SH, 0, SH_START);
    return n;
}

static void close_sheet(int keep)
{
    if (S.ov != OV_SHEET)
        return;
    S.ov = OV_NONE;
    SP.sheet.t = 0;
    if (!keep)
        SP.back.t = 0;
}

static void sheet_activate(uint32_t k)
{
    int r = S.sheet_row;
    pulse(k);
    if (FB(k) == SH_CLOSE) {
        close_sheet(0);
    } else if (FB(k) == SH_DEF) {
        if (S.def != r && R[r].cat >= 0) {
            set_default(r);
            S.sw_on = 1;
            S.sw_t = S.now;
        }
    } else if (FB(k) == SH_START && R[r].st == ST_READY) {
        close_sheet(1);
        tmr_add(60, T_SHEET_START, r, 0);
    }
}

static void close_overlays(int keep_back)
{
    if (S.ov == OV_CTX)
        close_ctx(keep_back, 0);
    if (S.ov == OV_ALERT)
        close_alert(keep_back);
    if (S.ov == OV_SHEET)
        close_sheet(keep_back);
}

/* ================================================================================================
 * launch and error cards (launch, launchError, launchRetry, launchBack) - doc 4.2.5
 * ============================================================================================== */

/* the stage the card shows before the loader reports one (device: the method's first stage) */
static void file_title(char *d, unsigned cap, const char *field)
{
    char alt[64];
    if (!cat_alt(field, 0, alt)) {
        d[0] = 0;
        return;
    }
    const char *a = alt;
    if (a[0] == 'u' && a[1] == 'f' && a[2] == 's' && a[3] == ':')
        a += 4;
    else if (a[0] == 's' && a[1] == 'd' && a[2] == ':')
        a += 3;
    if (seq(a, "@boot"))
        a = "BOOT payload";
    else if (seq(a, "@bundle"))
        a = "BOOT UEFI bundle";
    scopy(d, cap, a);
}

static void stage0(int cat)
{
    const struct cat_ent *e = cat_get((unsigned)cat);
    unsigned f = e->method == M_LINUX || e->method == M_ANDROID ? 1 : 0;
    const char *t = "Reading image", *fixed = 0;
    char alt[64], b[96];
    struct store_ent se;
    int found = 0;
    for (unsigned k = 0; !found && cat_alt(e->file[f], k, alt); ++k) {
        if (seq(alt, "@boot") || seq(alt, "@bundle")) {
            fixed = alt[1] == 'b' && alt[2] == 'o' ? "BOOT payload" : "BOOT bundle";
            found = 1;
        } else if (store_find(alt, &se)) {
            found = 2;
        }
    }
    switch (e->method) {
    case M_LINUX: t = "Loading device tree"; break;
    case M_ANDROID: t = "Loading ramdisk"; break;
    case M_XNU: t = found == 1 ? "Verifying image" : "Reading kernelcache"; break;
    case M_UEFI: t = found == 1 ? "Verifying image" : "Reading firmware"; break;
    default: break;
    }
    scopy(S.run_stage, sizeof(S.run_stage), t);
    struct sbuf s;
    sb_init(&s, b, sizeof(b));
    if (found == 2) {
        sb_s(&s, se.name);
        sb_s(&s, " · ");
        sb_mb(&s, se.size);
    } else if (found == 1) {
        sb_s(&s, fixed);
    }
    scopy(S.run_det, sizeof(S.run_det), b);
    S.bar = 0;
    S.bar_from = 0;
    S.bar_to = 0;
}

static void pill_rect(struct rc *r);

static void launch(int cat, int row, int src, uint32_t src_key, const struct rc *src_rect)
{
    close_overlays(1);
    S.mode = MD_LAUNCH;
    S.run_on = 1;
    S.run_cat = cat;
    S.run_row = row;
    S.run_err = 0;
    S.run_src = src;
    S.run_src_key = src_key;
    S.returned = 0;
    /* the concept sets S.last here but redraws the rows only in refreshRows: the "Last Used" label
     * moves on the next boot (star_menu_remember when the pick is returned) */
    struct rc r = rcv(72, 1200, 1296, 180);
    float rad = 62;
    layout();
    if (src == SRC_ROW && src_key && rect_of(src_key, &r, 1)) {
        r = rcv(r.x + 16, r.y + 6, r.w - 32, r.h - 12);
    } else if (src == SRC_PILL) {
        pill_rect(&r);
        rad = 68;
    } else if (src == SRC_RECT && src_rect) {
        r = *src_rect;
    }
    S.from[0] = r.x;
    S.from[1] = r.y;
    S.from[2] = r.w;
    S.from[3] = r.h;
    S.from[4] = rad;
    m7_spring_snap(&SP.lx, r.x);
    SP.lx.t = 0;
    m7_spring_snap(&SP.ly, r.y);
    SP.ly.t = 0;
    m7_spring_snap(&SP.lw, r.w);
    SP.lw.t = PX;
    m7_spring_snap(&SP.lh, r.h);
    SP.lh.t = PY;
    m7_spring_snap(&SP.lr, rad);
    SP.lr.t = 118;
    m7_spring_snap(&SP.la, 1);
    S.launch_vis = 1;
    S.launch_closing = 0;
    m7_spring_snap(&SP.err_tint, 0);
    stage0(cat);
    S.lnerr_vis = 0;
    m7_spring_snap(&SP.err_a, 0);
    m7_spring_snap(&SP.run_a, 0);
    m7_spring_snap(&SP.hint, 1);
    tmr_add(180, T_RUN_IN, 0, 0);
    SP.back.t = 1;
    S.ofocus = 0;
    LOm.init = 0;
    if (src == SRC_PILL)
        m7_spring_snap(&SP.pill_a, 0);
}

/* the error card's texts from the loader's reason (doc 4.2.5) */
static int has_word(const char *s, const char *w)
{
    unsigned n = slen(w);
    for (unsigned i = 0; s && s[i]; ++i) {
        unsigned k = 0;
        while (k < n && s[i + k] == w[k])
            ++k;
        if (k == n)
            return 1;
    }
    return 0;
}

static void error_texts(const char *why)
{
    const struct cat_ent *e = cat_get((unsigned)S.run_cat);
    char name[64];
    struct sbuf s;
    cat_utf8(name, sizeof(name), e->name);
    if (!why)
        why = "UNKNOWN ERROR";
    sb_init(&s, S.err_title, sizeof(S.err_title));
    sb_s(&s, "Couldn’t Start ");
    sb_s(&s, e->category == CAT_LINUX ? "Linux" : e->category == CAT_ANDROID ? "Android" :
             e->category == CAT_MICROSOFT ? "Windows" : name);
    sentence(S.err_why, sizeof(S.err_why), why);
    /* the evidence under the message (the concept's "expected 0x... · read 0x..."), not the pill
     * again: the CRCs of a CRC failure, else the file the loader was reading and its size */
    {
        char fnm[32];
        uint64_t fsz;
        uint32_t want, got;
        int lr = store_last_load(fnm, sizeof(fnm), &fsz, &want, &got);
        sb_init(&s, S.err_code, sizeof(S.err_code));
        if (lr == -2) {
            sb_s(&s, "expected ");
            sb_x8(&s, want);
            sb_s(&s, " · read ");
            sb_x8(&s, got);
        } else if (lr != 1 && fnm[0]) {
            sb_s(&s, fnm);
            sb_s(&s, " · ");
            sb_mb(&s, fsz);
            sb_s(&s, lr == -1 ? " · read error" : lr == -3 ? " · too large" : "");
        } else {
            sb_s(&s, e->key);
            if (e->file[0][0]) {
                sb_s(&s, " · ");
                sb_s(&s, e->file[0]);
            }
        }
    }
    sb_init(&s, S.err_msg, sizeof(S.err_msg));
    if (has_word(why, "CRC")) {
        const char *kind = has_word(why, "DTB") ? "device tree" : has_word(why, "INITRD") ? "initramfs" :
                           has_word(why, "RAMDISK") ? "ramdisk" : has_word(why, "CMDLINE") ? "command line" :
                           has_word(why, "PAYLOAD") ? "payload" : has_word(why, "UEFI") ? "UEFI image" :
                           has_word(why, "KERNEL") ? "kernel image" : "image";
        sb_s(&s, "The ");
        sb_s(&s, kind);
        sb_s(&s, " for ");
        sb_s(&s, name);
        sb_s(&s, " didn’t pass verification. Reflash it with USB Toolkit, or try again.");
    } else if (why[0] == 'N' && why[1] == 'O' && why[2] == ' ') {
        unsigned f = has_word(why, "DTB") || has_word(why, "RAMDISK") ? 1 : has_word(why, "INITRD") ? 2 : 0;
        char fn[64];
        file_title(fn, sizeof(fn), e->file[f]);
        sb_s(&s, name);
        sb_s(&s, " needs ");
        sb_s(&s, fn[0] ? fn : "a file");
        sb_s(&s, ". Add it with USB Toolkit, or try again.");
    } else {
        sb_s(&s, "Starbite couldn’t start ");
        sb_s(&s, name);
        sb_s(&s, ". Try again, or choose another system.");
    }
}

static void launch_error(const char *why)
{
    S.run_err = 1;
    m7_spring_kick(&SP.shake, -3200);
    SP.err_tint.t = 1;
    error_texts(why);
    tmr_add(260, T_ERR_SHOW, 0, 0);
}

static void launch_retry(void)
{
    if (!S.run_on)
        return;
    S.run_err = 0;
    S.returned = 0;
    SP.err_tint.t = 0;
    SP.err_a.t = 0;
    S.ofocus = 0;
    stage0(S.run_cat);
    SP.hint.t = 1;
    tmr_add(220, T_RETRY_SHOW, 0, 0);
}

static void launch_back(void)
{
    if (!S.run_on)
        return;
    float f[5];
    for (unsigned i = 0; i < 5; ++i)
        f[i] = S.from[i];
    struct rc r;
    layout();
    if (S.run_src == SRC_ROW && S.run_src_key && rect_of(S.run_src_key, &r, 1)) {
        f[0] = r.x + 16;
        f[1] = r.y + 6;
        f[2] = r.w - 32;
        f[3] = r.h - 12;
        f[4] = 60;
    }
    SP.lx.t = f[0];
    SP.ly.t = f[1];
    SP.lw.t = f[2];
    SP.lh.t = f[3];
    SP.lr.t = f[4];
    SP.run_a.t = 0;
    SP.err_a.t = 0;
    SP.err_tint.t = 0;
    SP.back.t = 0;
    S.run_on = 0;
    S.mode = MD_MENU;
    S.ofocus = 0;
    S.launch_closing = 1;
    if (S.run_src == SRC_ROW && S.run_src_key)
        set_focus(S.run_src_key, 0);
}

/* ================================================================================================
 * end screens (endScreen) - D12: the real action follows
 * ============================================================================================== */

static void end_screen(int kind)
{
    S.mode = MD_END;
    S.end_kind = kind;
    S.cd = 0;
    S.end_vis = 1;
    S.end_t = S.now;
    if (kind != EN_HANDOFF)
        tmr_add(600, T_END_ACT, kind, 0);
}

/* ================================================================================================
 * overlays common (closeOverlays, hardReset, toMenu) and the boot (prepMenuForBoot, finishIntro)
 * ============================================================================================== */

static void hard_reset(void)
{
    S.ov = OV_NONE;
    S.run_on = 0;
    S.cd = 0;
    S.ctx_closing = 0;
    S.launch_closing = 0;
    S.ctx_vis = S.sheet_vis = S.alert_vis = S.launch_vis = S.toast_vis = 0;
    S.lnerr_vis = 0;
    m7_spring_snap(&SP.ctx, 0);
    m7_spring_snap(&SP.lift, 0);
    m7_spring_snap(&SP.sub, 0);
    m7_spring_snap(&SP.alert, 0);
    m7_spring_snap(&SP.sheet, 0);
    m7_spring_snap(&SP.toast, 0);
    m7_spring_snap(&SP.la, 0);
    m7_spring_snap(&SP.run_a, 0);
    m7_spring_snap(&SP.err_a, 0);
    m7_spring_snap(&SP.back, 0);
    m7_spring_snap(&SP.err_tint, 0);
    S.end_vis = 0;
}

static uint32_t first_focus(void)
{
    int r = S.def >= 0 ? S.def : S.last;
    return r >= 0 ? rkey(r) : FK(F_P, 0, 0);
}

static void to_menu(void)
{
    hard_reset();
    S.mode = MD_MENU;
    m7_spring_snap(&SP.pill_a, 0);
    if (!S.focus)
        set_focus(first_focus(), 1);
}

static void prep_menu_for_boot(void)
{
    int r = S.def >= 0 ? S.def : S.last;
    S.open = r >= 0 ? R[r].sec : -1;
    for (unsigned c = 0; c < NS; ++c)
        m7_spring_snap(&SP.open[c], (int)c == S.open ? 1 : 0);
    m7_spring_snap(&SP.scroll, 0);
    m7_spring_snap(&SP.room, 0);
    S.focus = first_focus();
}

static void enter_menu(void)
{
    prep_menu_for_boot();
    to_menu();
    m7_spring_snap(&SP.pill_a, 0);
    S.cd = 0;
    m7_spring_snap(&SP.lb[LA_], 1);
    ensure_visible_now();
    lens_step(SP.lb, &LBm, S.focus, 1, 1);
}

static struct { float pill_a, pill_dy, lens_a; } IX;    /* the intro's last transforms */

void m7_prepare_boot(void)
{
    hard_reset();
    S.mode = MD_INTRO;
    prep_menu_for_boot();
    ensure_visible_now();
    if (S.autos > 0 && def_ready())
        pill_countdown();
    else
        pill_msg(IC_CHECK, "Ready", "Auto-start is off");
    m7_spring_snap(&SP.pill_a, 0);
    m7_spring_snap(&SP.pill_s, 1);
    m7_spring_snap(&SP.pill_y, 0);
    /* device: the lens sits on the focused row while the intro fades it in (the concept leaves it
     * unpositioned until finishIntro, so it pops in at 2350 ms) */
    LBm.init = 0;
    lens_step(SP.lb, &LBm, S.focus, 1, 1);
    m7_spring_snap(&SP.lb[LA_], 0);
    IX.pill_a = 0;
    IX.pill_dy = I7_ISLAND_DY;
    IX.lens_a = 0;
}

void m7_finish_intro(int skipped)
{
    if (S.mode != MD_INTRO && S.mode != MD_SPLASH)
        return;
    S.mode = MD_MENU;
    m7_spring_snap(&SP.pill_a, IX.pill_a);
    m7_spring_snap(&SP.pill_s, 1);
    m7_spring_snap(&SP.pill_y, IX.pill_dy);
    SP.pill_y.t = 0;
    if (skipped)
        m7_spring_snap(&SP.pill_y, 0);
    m7_spring_snap(&SP.lb[LA_], IX.lens_a);
    SP.lb[LA_].t = 1;
    set_focus(S.focus ? S.focus : first_focus(), 0);
    layout();
    lens_step(SP.lb, &LBm, S.focus, 1, 1);
    SP.lb[LA_].t = 1;
    start_countdown();
}

/* ================================================================================================
 * keys (key, keyDown, keyUp)
 * ============================================================================================== */

enum { K_UP_, K_DOWN_, K_POWER_, K_BIXBY_, K_POWERHOLD_, K_LAB_ };

static void key(int k)
{
    if (S.mode == MD_BOOT || S.mode == MD_SPLASH || S.mode == MD_INTRO)
        return;                     /* intro7_run handles the intro's keys */
    if (S.mode == MD_END)
        return;                     /* D12: the end screens act, they do not return */
    if (S.cd) {
        if (k == K_POWER_) {
            S.cd = 0;
            if (def_ready())
                launch(R[S.def].cat, S.def, SRC_PILL, 0, 0);
        } else {
            cancel_countdown();
        }
        return;
    }
    if (S.mode == MD_LAUNCH) {
        if (!S.run_on)
            return;
        if (S.run_err) {
            int i = S.ofocus == FK(F_E, 0, E_BACK) ? 1 : 0;
            if (k == K_UP_ || k == K_DOWN_) {
                int j = (int)clampf((float)(i + (k == K_DOWN_ ? 1 : -1)), 0, 1);
                S.ofocus = FK(F_E, 0, j);
            } else if (k == K_POWER_) {
                pulse(S.ofocus);
                if (S.ofocus == FK(F_E, 0, E_BACK))
                    launch_back();
                else
                    launch_retry();
            } else if (k == K_BIXBY_) {
                launch_back();
            }
        } else if (k == K_BIXBY_ && !S.returned) {
            launch_back();          /* D9: Bixby cancels until the card settles */
        }
        return;
    }
    if (S.ov) {
        uint32_t ks[8];
        unsigned n = S.ov == OV_CTX ? ctx_keys(ks) : S.ov == OV_ALERT ? alert_keys(ks) : sheet_keys(ks);
        if (k == K_UP_ || k == K_DOWN_) {
            int i = -1;
            for (unsigned j = 0; j < n; ++j)
                if (ks[j] == S.ofocus)
                    i = (int)j;
            int j = i + (k == K_DOWN_ ? 1 : -1);
            if (j < 0 || j >= (int)n) {
                m7_spring_kick(&SP.lo[LY_], (k == K_DOWN_ ? 1.0f : -1.0f) * 700);
                return;
            }
            S.ofocus = ks[j];
            return;
        }
        if (k == K_POWER_) {
            if (S.ov == OV_CTX)
                ctx_activate(S.ofocus);
            else if (S.ov == OV_ALERT)
                alert_activate(S.ofocus);
            else
                sheet_activate(S.ofocus);
            return;
        }
        if (k == K_BIXBY_) {
            close_overlays(0);
            return;
        }
        if (k == K_LAB_) {
            close_overlays(0);
            go_lab();
        }
        return;
    }
    if (k == K_UP_)
        move_focus(-1);
    else if (k == K_DOWN_)
        move_focus(1);
    else if (k == K_POWER_)
        menu_activate();
    else if (k == K_POWERHOLD_) {
        int r = key_row(S.focus);
        if (r >= 0)
            open_ctx(r);
        else
            menu_activate();
    } else if (k == K_BIXBY_)
        menu_back();
    else if (k == K_LAB_)
        go_lab();
}

static uint32_t focus_press_el(void) { return S.focus ? press_target(S.focus) : 0; }

static void key_event(const struct m7_ev *ev)
{
    switch (ev->type) {
    case M7_EV_UP: key(K_UP_); break;
    case M7_EV_DOWN: key(K_DOWN_); break;
    case M7_EV_POWER_DOWN:
        if (S.mode == MD_MENU && !S.ov && !S.cd)
            press_el(focus_press_el(), 1);
        break;
    case M7_EV_POWER_HOLD:
        press_el(focus_press_el(), 0);
        key(K_POWERHOLD_);
        break;
    case M7_EV_POWER:
        press_el(focus_press_el(), 0);
        key(K_POWER_);
        break;
    case M7_EV_BIXBY: key(K_BIXBY_); break;
    case M7_EV_BIXBY_HOLD: key(K_LAB_); break;
    default: break;
    }
}

/* ================================================================================================
 * touch (pointerdown / move / up, tap)
 * ============================================================================================== */

static int in_rc(struct rc r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void pill_rect(struct rc *r)
{
    float s = SP.pill_s.x;
    float cx = M7_ISLAND_X + M7_ISLAND_W / 2.0f, cy = M7_ISLAND_Y + M7_ISLAND_H / 2.0f;
    struct xf x = xf_about(s, cx, cy, 0, SP.pill_y.x);
    *r = xf_rc(x, rcv(M7_ISLAND_X, M7_ISLAND_Y, M7_ISLAND_W, M7_ISLAND_H));
}

/* the element under (x, y) with a data-f key, 0 for none; *in_list as closest('#vp') */
static uint32_t hit(float x, float y, int *in_list)
{
    struct rc r;
    *in_list = 0;
    if (S.mode == MD_LAUNCH) {
        for (unsigned b = 0; b < 2; ++b)
            if (S.launch_vis && rect_of(FK(F_E, 0, b), &r, 0) && in_rc(r, x, y))
                return FK(F_E, 0, b);
        return 0;
    }
    if (S.ov == OV_CTX) {
        for (unsigned b = MI_START; b <= MI_DETAILS; ++b)
            if (rect_of(FK(F_M, 0, b), &r, 0) && in_rc(r, x, y))
                return FK(F_M, 0, b);
        return 0;
    }
    if (S.ov == OV_ALERT) {
        for (unsigned i = 0; i < alert_nb(); ++i)
            if (rect_of(FK(F_A, 0, alert_act(i)), &r, 0) && in_rc(r, x, y))
                return FK(F_A, 0, alert_act(i));
        return 0;
    }
    if (S.ov == OV_SHEET) {
        for (unsigned b = 0; b < 3; ++b)
            if (rect_of(FK(F_SH, 0, b), &r, 0) && in_rc(r, x, y))
                return FK(F_SH, 0, b);
        return 0;
    }
    if (SP.pill_a.x >= 0.01f) {
        pill_rect(&r);
        if (in_rc(r, x, y))
            return 0;
    }
    for (unsigned b = 0; b < 4; ++b)
        if (rect_of(FK(F_T, 0, b), &r, 1) && in_rc(r, x, y))
            return FK(F_T, 0, b);
    if (y < M7_VP_Y0 || y >= M7_VP_Y1)
        return 0;
    *in_list = 1;
    for (unsigned i = 0; i < NS; ++i) {
        struct rc rows = xf_rc(L.cxf[i], rcv(M7_X0, L.card_y[i] + M7_CARD_HEAD, M7_CW, L.rows_h[i]));
        if (in_rc(rows, x, y))
            for (unsigned j = 0; j < SEC[i].n; ++j)
                if (rect_of(FK(F_R, i, j), &r, 1) && in_rc(r, x, y))
                    return FK(F_R, i, j);
        if (rect_of(FK(F_P, i, 0), &r, 1) && in_rc(r, x, y))
            return FK(F_P, i, 0);
    }
    return 0;
}

static void tap(uint32_t f)
{
    if (S.mode == MD_LAUNCH) {
        if (f == FK(F_E, 0, E_RETRY))
            launch_retry();
        else if (f == FK(F_E, 0, E_BACK))
            launch_back();
        return;
    }
    if (S.ov) {
        if (!f) {
            close_overlays(0);
            return;
        }
        S.ofocus = f;
        if (S.ov == OV_CTX && FKIND(f) == F_M)
            ctx_activate(f);
        else if (S.ov == OV_ALERT && FKIND(f) == F_A)
            alert_activate(f);
        else if (S.ov == OV_SHEET && FKIND(f) == F_SH)
            sheet_activate(f);
        return;
    }
    if (S.mode != MD_MENU || !f)
        return;
    set_focus(f, 0);
    if (FKIND(f) == F_P) {
        if (!prov_empty((int)FA(f)))
            toggle_prov((int)FA(f));
    } else if (FKIND(f) == F_R) {
        int r = key_row(f);
        if (r >= 0)
            start_entry(r, f);
    } else if (f == FK(F_T, 0, T_LAB)) {
        go_lab_os();                /* a tap on the dock's Lab button */
    } else if (FKIND(f) == F_T) {
        open_alert(FB(f) == T_POWER ? AL_POWER : FB(f) == T_RECOVERY ? AL_RECOVERY : AL_DOWNLOAD);
    }
}

static void ptr_down(float x, float y)
{
    if (S.mode == MD_END || S.mode == MD_INTRO || S.mode == MD_SPLASH)
        return;
    if (S.cd) {
        cancel_countdown();
        return;
    }
    layout();
    int in_list;
    uint32_t t = hit(x, y, &in_list);
    P.on = 1;
    P.x = x;
    P.y = y;
    P.t0 = S.now;
    P.s0 = SP.scroll.x;
    P.tgt = t;
    P.drag = 0;
    P.lng = 0;
    P.nv = 0;
    P.inlist = in_list && !S.ov && S.mode == MD_MENU;
    P.pe = 0;
    P.lt_armed = 0;
    if (t) {
        P.pe = press_target(t);
        press_el(P.pe, 1);
    }
    if (t && FKIND(t) == F_R && !S.ov && S.mode == MD_MENU) {
        P.lt_armed = 1;
        P.lt_due = S.now + M7_LONG_PRESS_MS;
    }
}

static void ptr_long(void)
{
    if (!P.on || P.drag || !P.lt_armed || S.now < P.lt_due)
        return;
    P.lt_armed = 0;
    P.lng = 1;
    press_el(P.pe, 0);
    set_focus(P.tgt, 0);
    int r = key_row(P.tgt);
    if (r >= 0)
        open_ctx(r);
}

static void ptr_move(float x, float y)
{
    (void)x;
    if (!P.on)
        return;
    float dy = y - P.y;
    if (!P.drag && absf(dy) > M7_DRAG_PX && P.inlist) {
        P.drag = 1;
        P.lt_armed = 0;
        press_el(P.pe, 0);
    }
    if (P.drag) {
        layout();
        float ms = max_scroll(), s = P.s0 - dy;
        if (s < 0)
            s *= 0.5f;
        else if (s > ms)
            s = ms + (s - ms) * 0.5f;
        m7_spring_snap(&SP.scroll, s);
        if (P.nv == 6) {
            for (unsigned i = 0; i < 5; ++i) {
                P.vt[i] = P.vt[i + 1];
                P.vs[i] = P.vs[i + 1];
            }
            P.nv = 5;
        }
        P.vt[P.nv] = S.now;
        P.vs[P.nv] = s;
        ++P.nv;
        S.ensure_until = 0;
    }
}

static void ptr_up(float x, float y)
{
    (void)x;
    (void)y;
    if (!P.on)
        return;
    P.on = 0;
    P.lt_armed = 0;
    press_el(P.pe, 0);
    if (P.drag) {
        float v = 0;
        if (P.nv > 1) {
            double dt = P.vt[P.nv - 1] - P.vt[0];
            v = (P.vs[P.nv - 1] - P.vs[0]) / (float)(dt > 1 ? dt : 1) * 1000;
        }
        layout();
        SP.scroll.t = clampf(SP.scroll.x + v * 0.32f, 0, max_scroll());
        return;
    }
    if (P.lng)
        return;
    tap(P.tgt);
}

static void dispatch(const struct m7_ev *ev)
{
    if (ev->type >= M7_EV_TOUCH_DOWN) {
        if (ev->type == M7_EV_TOUCH_DOWN)
            ptr_down(ev->x, ev->y);
        else if (ev->type == M7_EV_TOUCH_MOVE)
            ptr_move(ev->x, ev->y);
        else
            ptr_up(ev->x, ev->y);
    } else {
        key_event(ev);
    }
}

/* ================================================================================================
 * the Details sheet's rows (the concept's INFO; device: from the catalog and the store)
 * ============================================================================================== */

struct srow { const char *l; char v[64]; int mono; };
static struct srow SRW[8];
static unsigned SRn;

static void sr_add(const char *l, const char *v, int mono)
{
    if (SRn >= 6)
        return;
    SRW[SRn].l = l;
    scopy(SRW[SRn].v, sizeof(SRW[SRn].v), v);
    SRW[SRn].mono = mono;
    ++SRn;
}

/* "<file> · 31.4 MB" for a catalog file field; *size accumulates; the medium into loc */
static void sr_file(const char *field, char *out, unsigned cap, uint64_t *size, char *loc, unsigned lcap)
{
    char alt[64], t[64];
    struct sbuf s;
    struct store_ent se;
    sb_init(&s, out, cap);
    file_title(t, sizeof(t), field);
    sb_s(&s, t);
    if (cat_alt(field, 0, alt) && alt[0] != '@' && store_find(alt, &se)) {
        sb_s(&s, " · ");
        sb_mb(&s, se.size);
        *size += se.size;
        if (loc && !loc[0]) {
            struct sbuf l;
            sb_init(&l, loc, lcap);
            sb_s(&l, se.medium == STORE_SD ? "sd:" : "ufs:");
            sb_s(&l, se.name);
        }
    } else if (loc && !loc[0] && alt[0] == '@') {
        scopy(loc, lcap, "BOOT partition");
    }
}

static void sheet_info(int r)
{
    SRn = 0;
    if (R[r].cat < 0) {
        sr_add("Runs", "In the loader", 0);
        sr_add("Tools", "Display · touch · keys · UART", 0);
        sr_add("Memory", "RAM test, clocks, PMIC", 0);
        sr_add("Exit", "Bixby or Power", 0);
        return;
    }
    const struct cat_ent *e = cat_get((unsigned)R[r].cat);
    if (R[r].st == ST_COMING) {
        sr_add("Status", "Coming soon", 0);
        sr_add("Detail", R[r].det, 0);
        return;
    }
    if (R[r].st == ST_MISSING) {
        sr_add("Status", "Not installed", 0);
        sr_add("Detail", R[r].det, 0);
        sr_add("Install", "USB Toolkit → Add System", 0);
        return;
    }
    char a[64], b[64], loc[64], sz[32];
    uint64_t size = 0;
    loc[0] = 0;
    /* the catalog's version to its second " · " field ("26.04 LTS · Phosh · Linux 7.1" ->
     * "26.04 LTS · Phosh", the concept's value); the row's subtitle keeps all of it */
    {
        char v[64];
        cat_utf8(v, sizeof(v), e->version);
        for (unsigned i = 0, f = 0; v[i]; ++i)
            if (v[i] == ' ' && v[i + 1] == '\xc2' && v[i + 2] == '\xb7' && v[i + 3] == ' ' && ++f == 2) {
                v[i] = 0;
                break;
            }
        sr_add("Version", v, 0);
    }
    sr_file(e->file[0], a, sizeof(a), &size, loc, sizeof(loc));
    switch (e->method) {
    case M_XNU: sr_add("Image", a, 0); break;
    case M_UEFI: sr_add("Firmware", a, 0); break;
    case M_LINUX:
        sr_add("Kernel", a, 0);
        sr_file(e->file[1], b, sizeof(b), &size, 0, 0);
        sr_add("Device Tree", b, 0);
        if (e->file[2][0])
            sr_file(e->file[2], b, sizeof(b), &size, 0, 0);
        break;
    case M_ANDROID:
        sr_add("Kernel", a, 0);
        sr_file(e->file[1], b, sizeof(b), &size, 0, 0);
        sr_add("Ramdisk", b, 0);
        break;
    default: sr_add("Image", a, 0); break;
    }
    sr_add("Location", loc[0] ? loc : "—", 1);
    if (size) {
        struct sbuf s;
        sb_init(&s, sz, sizeof(sz));
        sb_mb(&s, size);
        sr_add("Size", sz, 0);
    }
    sr_add("Handoff", e->method == M_XNU ? "EL1 · XNU" : e->method == M_UEFI ? "EL1 · UEFI" :
           "EL1 · 0x92000000", 1);
}

static unsigned sheet_ok(void) { return R[S.sheet_row].st == ST_READY && R[S.sheet_row].cat >= 0; }
static unsigned sheet_nrows(void) { return SRn + (sheet_ok() ? 2 : 0); }

/* ================================================================================================
 * timers' callbacks
 * ============================================================================================== */

static void end_action(int kind);

static void tmr_fire(const struct tmr *t)
{
    char a[96], b[96];
    struct sbuf s1, s2;
    switch (t->kind) {
    case T_PRESS_UP:
        press_el((uint32_t)t->a, 0);
        break;
    case T_CTX_START:
    case T_SHEET_START:
        if (S.mode == MD_MENU)
            start_entry(t->a, rkey(t->a));
        break;
    case T_CTX_AUTO:
        if (S.ov != OV_CTX)
            break;
        close_ctx(0, 0);
        sb_init(&s1, a, sizeof(a));
        sb_init(&s2, b, sizeof(b));
        if (t->a) {
            sb_s(&s1, "Start ");
            sb_s(&s1, R[t->b].name);
            sb_s(&s1, " Automatically");
            sb_s(&s2, "After ");
            sb_u(&s2, (unsigned)t->a);
            sb_s(&s2, " seconds at power on");
        } else {
            sb_s(&s1, "Auto-start Off");
            sb_s(&s2, "Starbite waits for you");
        }
        toast(IC_TIMER, a, b);
        break;
    case T_ALERT_END:
        end_screen(t->a == A_OFF ? EN_OFF : t->a == A_RESTART ? EN_RESTART :
                   t->a == A_RECOVERY ? EN_RECOVERY : EN_DOWNLOAD);
        break;
    case T_RUN_IN:
        if (S.run_on && !S.run_err)
            SP.run_a.t = 1;
        break;
    case T_ERR_SHOW:
        if (!S.run_on || !S.run_err)
            break;
        SP.run_a.t = 0;
        S.lnerr_vis = 1;
        SP.err_a.t = 1;
        S.ofocus = FK(F_E, 0, E_RETRY);
        LOm.init = 0;
        break;
    case T_RETRY_SHOW:
        if (S.run_on && !S.run_err) {
            S.lnerr_vis = 0;
            SP.run_a.t = 1;
        }
        break;
    case T_PILL_HIDE:
        if (!S.cd)
            hide_pill();
        break;
    case T_END_ACT:
        end_action(t->a);
        break;
    default:
        break;
    }
}

static void tmr_run(void)
{
    for (unsigned i = 0; i < NTMR; ++i)
        if (TM[i].kind != T_NONE_ && TM[i].due <= S.now) {
            struct tmr t = TM[i];
            TM[i].kind = T_NONE_;
            tmr_fire(&t);
        }
}

/* ================================================================================================
 * per-frame update (update(): springs -> geometry, the timed checks)
 * ============================================================================================== */

static void update(void)
{
    float soft = SP.back.x;
    S.menu_sc = soft > 0.002f ? 1 - 0.035f * soft : 1;
    layout();
    if (S.now < S.ensure_until && S.mode == MD_MENU && !P.on)
        ensure_visible();
    /* in the intro the island's alpha is the intro's (pillA.snap in introAt), so the list makes room
     * while the island rises, as in the concept */
    float pa = S.mode == MD_INTRO ? IX.pill_a : SP.pill_a.t;
    SP.room.t = (pa > 0.5f || S.cd || (SP.toast.t > 0.5f && S.mode == MD_MENU)) ? M7_ROOM : 0;
    layout();
    float ds = (L.off - S.last_scroll) * S.menu_sc;
    if (ds != 0 && LBm.init && LBm.clip)
        SP.lb[LY_].x -= ds;
    S.last_scroll = L.off;
    int menu_lens = (S.mode == MD_MENU || S.mode == MD_LAUNCH) && S.focus;
    if (S.mode != MD_INTRO)
        lens_step(SP.lb, &LBm, S.focus, menu_lens, 0);
    else if (LBm.init) {
        /* the lens fading in during the intro stays on the focused row while the list makes room
         * for the island (its alpha is the intro's) */
        float a = SP.lb[LA_].x, at = SP.lb[LA_].t;
        lens_step(SP.lb, &LBm, S.focus, 1, 1);
        m7_spring_snap(&SP.lb[LA_], a);
        SP.lb[LA_].t = at;
    }
    if (S.cd) {
        double rem = S.cd_dur - (S.now - S.cd_t0);
        if (rem <= 0) {
            S.cd = 0;
            if (def_ready())
                launch(R[S.def].cat, S.def, SRC_PILL, 0, 0);
        }
    }
    if (S.ctx_vis && S.ov != OV_CTX && SP.ctx.x < 0.01f && SP.lift.x < 0.01f) {
        S.ctx_vis = 0;
        S.ctx_closing = 0;
    }
    if (S.alert_vis && S.ov != OV_ALERT && SP.alert.x < 0.01f)
        S.alert_vis = 0;
    if (S.sheet_vis && S.ov != OV_SHEET && SP.sheet.x < 0.01f)
        S.sheet_vis = 0;
    if (S.toast_vis) {
        if (S.now > S.toast_t)
            SP.toast.t = 0;
        if (SP.toast.t == 0 && SP.toast.x < 0.01f)
            S.toast_vis = 0;
    }
    if (S.launch_vis && S.launch_closing &&
        absf(SP.lw.x - SP.lw.t) < 2 && absf(SP.lh.x - SP.lh.t) < 2) {
        S.launch_vis = 0;
        S.launch_closing = 0;
    }
    int ov_lens = S.ofocus && (S.ov || S.mode == MD_LAUNCH);
    lens_step(SP.lo, &LOm, S.ofocus, ov_lens, !LOm.init);
}

static uint64_t t_base;

static double now_ms(void) { return (double)(m7_now_us() - t_base) / 1000.0; }

#define NSPRING (sizeof(SP) / sizeof(struct m7_spring))

static void springs_step(float dt)
{
    struct m7_spring *a = (struct m7_spring *)&SP;
    for (unsigned i = 0; i < NSPRING; ++i)
        m7_spring_step(&a[i], dt);
}

#ifdef M7_PREVIEW
static void snap_all(void)
{
    struct m7_spring *a = (struct m7_spring *)&SP;
    for (unsigned i = 0; i < NSPRING; ++i)
        m7_spring_snap(&a[i], a[i].t);
}
#endif

static int springs_moving(void)
{
    struct m7_spring *a = (struct m7_spring *)&SP;
    for (unsigned i = 0; i < NSPRING; ++i)
        if (sp_moving(&a[i]))
            return 1;
    return 0;
}

/* The battery (D8): batt.c's MAX77705 reader, read when the menu starts and then every
 * M7_BATT_MS, never per frame. A change (percentage, cable, known / unknown) costs one frame;
 * while an overlay is up, the frozen base behind it is taken again. */
#define M7_BATT_MS 10000.0
static struct batt_info BAT;
static double bat_due;              /* S.now of the next reading (0: at the first step) */

static void bat_poll(void)
{
    if (bat_due > 0 && S.now < bat_due)
        return;
    bat_due = S.now + M7_BATT_MS;
    struct batt_info b;
    (void)batt_read(&b);
    if (b.valid != BAT.valid || b.pct != BAT.pct || b.plugged != BAT.plugged)
        S.refreeze = 1;             /* also makes run_loop draw a frame when nothing is frozen */
    BAT = b;
}

void m7_step(float dt)
{
    S.now = now_ms();
    tmr_run();
    bat_poll();
    springs_step(dt);
    ptr_long();
    update();
}

void m7_advance(float dt)
{
    dt = clampf(dt, 0, M7_DT_CATCHUP);
    for (;;) {
        float h = minf(dt, M7_DT_MAX);
        m7_step(h);
        dt -= h;
        if (dt <= 0)
            break;
    }
}

/* ================================================================================================
 * the frame: shapes and items (render(), collect(), and the DOM each layer paints)
 * ============================================================================================== */

static struct g7_layer LAYB, LAYO;
static struct g7_layer *E;          /* the layer being filled */
static struct xf EX;                /* the element's transform */
static float EA;                    /* the element's opacity (ancestors') */
static unsigned EC;                 /* its clip */
static uint16_t EF;                 /* G7_I_FADE inside #vp */

#define KEY(ns, i, p) ((uint32_t)(ns) << 24 | (uint32_t)(i) << 12 | (uint32_t)(p))
enum { NS_STATUS = 1, NS_TOP, NS_FOOT, NS_DOCK, NS_PILL, NS_CARD = 0x10, NS_ROW = 0x20,
       NS_CTX = 0x40, NS_PREV, NS_SHEET, NS_ALERT, NS_TOAST, NS_RUN, NS_ERR, NS_END };

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BRAND 0xc7ffffffU         /* rgba(255,255,255,.78) */
#define C_CHEV 0x80ebebf5U          /* rgba(235,235,245,.5) */
#define C_SD_GLOW 0x8030d158U
#define C_BAT_CHG 0xff30d158U         /* battery fill while a cable is in (#30d158) */
#define C_BAT_LOW 0xffff453aU         /* battery fill under 15 % (#ff453a) */
#define C_SD_MISS 0x4debebf5U       /* rgba(235,235,245,.3) */
#define C_SD_COME 0xd9ff9f0aU       /* rgba(255,159,10,.85) */
#define C_STAR16 0x296fb3ffU        /* rgba(111,179,255,.16) */
#define C_RED16 0x29ff453aU
#define C_WHITE16 0x29ffffffU
#define C_TEXT_OFF 0x4dffffffU      /* .mi.off */
#define C_LAB_OFF 0x42ffffffU       /* .mlab.off */
#define C_MSEP 0x1affffffU
#define C_THICK 0x38000000U
#define C_SG 0x1affffffU
#define C_SG_ON_TX 0xff0b0d12U
#define C_SG_OFF_TX 0x47ffffffU
#define C_SG_OFF 0x0dffffffU
#define C_KF 0xf26fb3ffU
#define C_GRAB 0x47ffffffU
#define C_GRP 0x13ffffffU
#define C_SW_OFF 0x5c787880U
#define C_SHADOW25 0x40000000U
#define C_SHADOW35 0x59000000U
#define C_SHADOW45 0x73000000U
#define C_START_OFF 0x66ffffffU
#define C_BAR 0x33ffffffU
#define C_WARN13 0x21ffd60aU
#define C_WARN18 0x2effd60aU
#define C_CHIP 0x2eff453aU
#define C_CHIP_TX 0xffff8a80U

static struct g7_item *item(uint32_t key, unsigned kind, uint32_t color, float a)
{
    a *= EA;
    if (a <= 0.002f)
        return 0;
    struct g7_item *it = g7_add_item(E, kind);
    if (!it)
        return 0;
    it->key = key;
    it->clip = (uint8_t)EC;
    it->flags = EF;
    it->a = a;
    it->color = color;
    return it;
}

static float tx_(float x) { return EX.s * x + EX.bx; }
static float ty_(float y) { return EX.s * y + EX.by; }

static void t_text(uint32_t key, unsigned font, const char *s, float x, float base, float track,
                   uint32_t color, float a, unsigned flags, float max_w)
{
    if (!s || !s[0])
        return;
    struct g7_item *it = item(key, G7_TEXT, color, a);
    if (!it)
        return;
    struct g7_text *t = &it->u.text;
    t->s = s;
    t->n = (uint16_t)slen(s);
    t->font = (uint16_t)font;
    t->x = tx_(x);
    t->y = ty_(base);
    t->track = track;
    t->scale = EX.s;
    t->max_w = max_w;
    t->color2 = color;
    t->gy0 = t->gy1 = t->gstop = 0;
    it->flags |= (uint16_t)flags;
}

/* text centred on cx (CSS text-align: center; the width includes the trailing letter-spacing) */
static void t_center(uint32_t key, unsigned font, const char *s, float cx, float base, float track,
                     uint32_t color, float a)
{
    t_text(key, font, s, cx - tw(font, s, track) / 2, base, track, color, a, 0, 0);
}

static void t_rect(uint32_t key, float x, float y, float w, float h, float r, float ring, uint32_t color,
                   float a, unsigned flags)
{
    if (w <= 0 || h <= 0)
        return;
    struct g7_item *it = item(key, G7_RECT, color, a);
    if (!it)
        return;
    it->u.rect.x = tx_(x);
    it->u.rect.y = ty_(y);
    it->u.rect.w = w * EX.s;
    it->u.rect.h = h * EX.s;
    it->u.rect.r = r * EX.s;
    it->u.rect.ring = ring * EX.s;
    it->flags |= (uint16_t)flags;
}

static void t_shadow(uint32_t key, float x, float y, float w, float h, float r, float dx, float dy,
                     float blur, float spread, uint32_t color, float a)
{
    struct g7_item *it = item(key, G7_SHADOW, color, a);
    if (!it)
        return;
    struct g7_boxshadow *b = &it->u.shadow;
    b->x = tx_(x);
    b->y = ty_(y);
    b->w = w * EX.s;
    b->h = h * EX.s;
    b->r = r * EX.s;
    b->dx = dx * EX.s;
    b->dy = dy * EX.s;
    b->blur = blur * EX.s;
    b->spread = spread * EX.s;
}

static void t_img(uint32_t key, unsigned id, float x, float y, float w, float h, uint32_t color, float a)
{
    struct g7_item *it = item(key, G7_IMAGE, color, a);
    if (!it)
        return;
    it->u.image.id = (uint16_t)id;
    it->u.image.pad = 0;
    it->u.image.x = tx_(x);
    it->u.image.y = ty_(y);
    it->u.image.w = w * EX.s;
    it->u.image.h = h * EX.s;
    it->u.image.sat = 1;
}

static void t_tile(uint32_t key, unsigned tile, unsigned size, float x, float y, float w, float sat, float a)
{
    struct g7_item *it = item(key, G7_TILE, C_WHITE, a);
    if (!it)
        return;
    it->u.tile.tile = (uint16_t)tile;
    it->u.tile.size = (uint16_t)size;
    it->u.tile.x = tx_(x);
    it->u.tile.y = ty_(y);
    it->u.tile.w = w * EX.s;
    it->u.tile.sat = sat;
}

static void t_arc(uint32_t key, float cx, float cy, float r, float width, float a0, float sweep,
                  uint32_t color, float a)
{
    struct g7_item *it = item(key, G7_ARC, color, a);
    if (!it)
        return;
    it->u.arc.cx = tx_(cx);
    it->u.arc.cy = ty_(cy);
    it->u.arc.r = r * EX.s;
    it->u.arc.width = width * EX.s;
    it->u.arc.a0 = a0;
    it->u.arc.sweep = sweep;
}

static void t_stroke(uint32_t key, unsigned n, const float *x, const float *y, float width,
                     uint32_t color, float a)
{
    struct g7_item *it = item(key, G7_STROKE, color, a);
    if (!it)
        return;
    for (unsigned i = 0; i < 4; ++i) {
        it->u.stroke.x[i] = i < n ? tx_(x[i]) : 0;
        it->u.stroke.y[i] = i < n ? ty_(y[i]) : 0;
    }
    it->u.stroke.n = (uint8_t)n;
    it->u.stroke.width = width * EX.s;
}

static void shape(uint32_t key, unsigned flags, struct rc r, float rad, float a, float s, const float *tint)
{
    if (a <= 0.002f || r.w < 1 || r.h < 1)
        return;
    struct g7_shape *g = g7_add_shape(E);
    if (!g)
        return;
    g->key = key;
    g->flags = flags;
    g->x = r.x;
    g->y = r.y;
    g->w = r.w;
    g->h = r.h;
    g->r = rad;
    g->a = a;
    g->s = s;
    for (unsigned i = 0; i < 4; ++i)
        g->tint[i] = tint[i];
}

static const float TINT0[4] = { M7_TINT_PANE };   /* floats only: no relocation */

static unsigned clip_and(float x0, float y0, float x1, float y1, unsigned with)
{
    const struct g7_clip *c = &E->clip[with];
    x0 = maxf(x0, c->x0);
    y0 = maxf(y0, c->y0);
    x1 = minf(x1, c->x1);
    y1 = minf(y1, c->y1);
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    return g7_add_clip(E, x0, y0, x1, y1, 0);
}

static void lens_shape(uint32_t key, const struct m7_spring *L, const struct lensx *m, float a)
{
    if (a < 0.01f)
        return;
    float st = minf(absf(L[LY_].v) * 0.03f, 46), sq = minf(absf(L[LY_].v) * 0.008f, 22);
    float t[4] = { M7_TINT_LENS };
    shape(key, G7_S_LENS | (m->clip ? G7_S_CLIP : 0),
          rcv(L[LX_].x + sq, L[LY_].x - st / 2, maxf(10, L[LW_].x - 2 * sq), L[LH_].x + st),
          L[LR_].x, a, 1.4f, t);
}

static unsigned icon_54(int ic)
{
    switch (ic) {
    case IC_CHECK: return SB7_I_CHECK_54;
    case IC_TIMER: return SB7_I_TIMER_54;
    case IC_FLASK: return SB7_I_FLASK_54;
    case IC_STARF: return SB7_I_STARF_54;
    default: return SB7_I_INFO_54;
    }
}

/* ---- the base layer: #menu ---- */

static unsigned ps_text(int i, char *b, unsigned cap, unsigned *ready)
{
    struct sbuf s;
    unsigned nready = 0, ncome = 0, n = SEC[i].n;
    sb_init(&s, b, cap);
    for (unsigned j = 0; j < n; ++j) {
        const struct row *w = &R[SEC[i].r0 + j];
        if (w->st == ST_READY) {
            if (SEC[i].cat != CAT_LAB) {
                if (nready)
                    sb_s(&s, ", ");
                sb_s(&s, w->name);
            }
            ++nready;
        } else if (w->st == ST_COMING) {
            ++ncome;
        }
    }
    *ready = nready;
    if (SEC[i].cat == CAT_LAB) {
        sb_u(&s, nready);
        sb_s(&s, nready == 1 ? " Tool Ready" : " Tools Ready");
    } else if (!nready) {
        if (ncome) {
            sb_u(&s, ncome);
            sb_s(&s, " Coming Soon");
        } else if (n) {
            sb_u(&s, n);
            sb_s(&s, n == 1 ? " System" : " Systems");
        } else {
            sb_s(&s, "Coming Soon");
        }
    }
    return n;
}

struct tagv { const char *s; uint32_t bg, fg; int star; float w, h; };

static unsigned row_tags(int r, struct tagv *t)
{
    unsigned n = 0;
    if (r == S.def) {
        t[n].s = "Default";
        t[n].bg = M7_TAG_DEF_BG;
        t[n].fg = M7_TAG_DEF_FG;
        t[n].star = 1;
        ++n;
    }
    const char *tg = tag_title(R[r].tag);
    if (R[r].st == ST_READY && tg) {
        t[n].s = tg;
        t[n].star = 0;
        if (R[r].tag == TAG_EXPERIMENTAL) {
            t[n].bg = M7_TAG_EXP_BG;
            t[n].fg = M7_TAG_EXP_FG;
        } else if (R[r].tag == TAG_STABLE || R[r].tag == TAG_DAILY) {
            t[n].bg = M7_TAG_OK_BG;
            t[n].fg = M7_TAG_OK_FG;
        } else {
            t[n].bg = M7_TAG_BG;
            t[n].fg = M7_TAG_FG;
        }
        ++n;
    }
    if (R[r].st != ST_READY) {
        t[n].s = R[r].st == ST_COMING ? "Coming Soon" : "Not Installed";
        t[n].bg = 0;
        t[n].fg = M7_L3;
        t[n].star = -1;             /* .st */
        ++n;
    }
    for (unsigned i = 0; i < n; ++i) {
        if (t[i].star < 0) {
            t[i].w = tw(SB7_F_IN400_40, t[i].s, 0);
            t[i].h = 49;
        } else {
            t[i].w = 26 + (t[i].star ? 42 : 0) + tw(SB7_F_IN600_33, t[i].s, 0.005f) + 26;
            t[i].h = M7_TAG_H;
        }
    }
    return n;
}

/* a system row's DOM (rowHTML) with its top-left at (x, y): .mk .rt .ra */
static void row_items(uint32_t ns, int r, float x, float y)
{
    const struct row *w = &R[r];
    int ready = w->st == ST_READY;
    uint32_t base = KEY(ns, (unsigned)r, 0);
    t_tile(base + 1, w->tile, SB7_TILE_124, x + 72, y + 32, 124, ready ? 1 : 0.3f, ready ? 1 : 0.46f);
    struct tagv tg[3];
    unsigned nt = row_tags(r, tg);
    float raw = 0, rah = 0;
    for (unsigned i = 0; i < nt; ++i) {
        raw = maxf(raw, tg[i].w);
        rah += tg[i].h + (i ? 12 : 0);
    }
    float rtw = M7_CW - 268 - 52 - 24 - raw;
    t_text(base + 2, SB7_F_IT600_66, w->name, x + 268, y + M7_ROW_NAME_BASE, -0.02f,
           ready ? M7_L1 : M7_DIM_ROW, 1, G7_I_ELLIP, rtw);
    /* .rd: dot, detail, Last Used */
    float sdx = x + 268, sdy = y + 125.6f;
    if (w->st == ST_READY) {
        t_shadow(base + 3, sdx, sdy, 18, 18, 9, 0, 0, 12, 0, C_SD_GLOW, 1);
        t_rect(base + 4, sdx, sdy, 18, 18, 9, 0, M7_GREEN, 1, 0);
    } else {
        t_rect(base + 4, sdx, sdy, 18, 18, 9, 3.5f, w->st == ST_COMING ? C_SD_COME : C_SD_MISS, 1,
               w->st == ST_COMING ? G7_I_DASH : 0);
    }
    float luw = 0, dotw = 0, lastw = 0;
    int lu = r == S.last;
    if (lu) {
        dotw = tw(SB7_F_IN400_42, "·", 0);
        lastw = tw(SB7_F_IN400_42, "Last Used", 0);
        luw = dotw + 10 + 36 + 10 + lastw;
    }
    float ddmax = rtw - 18 - 14 - (lu ? 14 + luw : 0);
    float ddw = minf(tw(SB7_F_IN400_42, w->det, 0), maxf(0, ddmax));
    float base_y = y + M7_ROW_SUB_BASE;
    t_text(base + 5, SB7_F_IN400_42, w->det, x + 300, base_y, 0, ready ? M7_L2 : M7_L3, 1, G7_I_ELLIP,
           maxf(0, ddmax));
    if (lu) {
        float lx = x + 300 + ddw + 14;
        t_text(base + 6, SB7_F_IN400_42, "·", lx, base_y, 0, M7_STAR, 1, 0, 0);
        t_img(base + 7, SB7_I_CLOCK_36, lx + dotw + 10, y + 109.1f + 7.5f, 36, 36, M7_STAR, 1);
        t_text(base + 8, SB7_F_IN400_42, "Last Used", lx + dotw + 56, base_y, 0, M7_STAR, 1, 0, 0);
    }
    /* .ra: tags stacked, right-aligned at the row's right padding */
    float right = x + M7_CW - M7_ROW_RIGHT, ty = y + (M7_ROW_H - rah) / 2;
    for (unsigned i = 0; i < nt; ++i) {
        float tx0 = right - tg[i].w;
        if (tg[i].star < 0) {
            t_text(base + 9 + 4 * i, SB7_F_IN400_40, tg[i].s, tx0, ty + 39, 0, tg[i].fg, 1, 0, 0);
        } else {
            t_rect(base + 9 + 4 * i, tx0, ty, tg[i].w, M7_TAG_H, 31, 0, tg[i].bg, 1, 0);
            if (tg[i].star)
                t_img(base + 10 + 4 * i, SB7_I_STARF_32, tx0 + 26, ty + 15, 32, 32, tg[i].fg, 1);
            t_text(base + 11 + 4 * i, SB7_F_IN600_33, tg[i].s, tx0 + 26 + (tg[i].star ? 42 : 0), ty + 43,
                   0.005f, tg[i].fg, 1, 0, 0);
        }
        ty += tg[i].h + 12;
    }
}

static void build_cards(unsigned vp, float ma)
{
    const struct m7_xf *ix = L.ix;
    for (unsigned i = 0; i < NS; ++i) {
        float ca = ix ? ix->card_a[i] : 1, cy = L.card_y[i];
        EX = L.cxf[i];
        EA = ma * ca;
        EC = vp;
        EF = G7_I_FADE;
        uint32_t k = KEY(NS_CARD, i, 0);
        t_img(k + 1, SEC[i].glyph, M7_X0 + 88, cy + 100, 96, 96, C_WHITE, 1);
        /* .pn: white with the sheen to #d9e2f0 over its box */
        struct g7_item *it = 0;
        t_text(k + 2, SB7_F_IT700_128, cat_title(SEC[i].cat), M7_X0 + M7_PNAME_X, cy + M7_PNAME_BASE,
               -0.035f, C_WHITE, 1, G7_I_GRAD, 0);
        if (E->nitem && E->item[E->nitem - 1].key == k + 2)
            it = &E->item[E->nitem - 1];
        if (it) {
            it->u.text.color2 = M7_SHEEN_END;
            it->u.text.gy0 = ty_(cy + 47.5f);
            it->u.text.gy1 = ty_(cy + 47.5f + 133.12f);
            it->u.text.gstop = 0.48f;
        }
        /* .ps */
        char *b = ARENA + arena_n;
        unsigned nready, n;
        if (arena_n + 160 > sizeof(ARENA))
            continue;
        n = ps_text((int)i, b, 160, &nready);
        arena_n += slen(b) + 1;
        float px = M7_X0 + M7_PNAME_X, pb = cy + M7_PSUB_BASE;
        unsigned pc = EC;
        float textw = tw(SB7_F_IN400_44, b, 0);
        char *b2 = 0;
        float w2 = 0;
        if (nready && SEC[i].cat != CAT_LAB) {
            struct sbuf s;
            b2 = ARENA + arena_n;
            if (arena_n + 32 <= sizeof(ARENA)) {
                sb_init(&s, b2, 32);
                sb_s(&s, "· ");
                sb_u(&s, n);
                sb_s(&s, n == 1 ? " System" : " Systems");
                arena_n += slen(b2) + 1;
                w2 = tw(SB7_F_IN400_44, b2, 0);
            } else {
                b2 = 0;
            }
        }
        float total = (nready ? 36 : 0) + textw + (b2 ? 16 + w2 : 0);
        if (total > 858) {
            struct rc pr = xf_rc(EX, rcv(px, cy + 194.5f, 858, 54));
            EC = clip_and(pr.x, pr.y, pr.x + pr.w, pr.y + pr.h, vp);
        }
        float x = px;
        if (nready) {
            t_rect(k + 3, x, cy + 211.5f, 20, 20, 10, 0, M7_GREEN, 1, 0);
            x += 36;
        }
        t_text(k + 4, SB7_F_IN400_44, b, x, pb, 0, M7_L2, 1, 0, 0);
        if (b2)
            t_text(k + 5, SB7_F_IN400_44, b2, x + textw + 16, pb, 0, M7_L3, 1, 0, 0);
        EC = pc;
        /* chevron, rotated 0 -> 90 degrees about its box's centre as the section opens */
        float ang = clampf(SP.open[i].x, 0, 1.1f) * 90 * PI_F / 180, cs = cosf_(ang), sn = sinf_(ang);
        float ccx = M7_X0 + 1172 + 30, ccy = cy + 118 + 30, px3[3], py3[3];
        float qx[3] = { -7.5f, 10, -7.5f }, qy[3] = { -17.5f, 0, 17.5f };
        for (unsigned p = 0; p < 3; ++p) {
            px3[p] = ccx + qx[p] * cs - qy[p] * sn;
            py3[p] = ccy + qx[p] * sn + qy[p] * cs;
        }
        t_stroke(k + 6, 3, px3, py3, 6.5f, C_CHEV, 1);
        /* rows */
        if (L.rows_h[i] <= 0.5f)
            continue;
        struct rc rb = xf_rc(EX, rcv(M7_X0, cy + M7_CARD_HEAD, M7_CW, L.rows_h[i]));
        unsigned rc_clip = clip_and(rb.x, rb.y, rb.x + rb.w, rb.y + rb.h, vp);
        const struct g7_clip *cl = &E->clip[rc_clip];
        for (unsigned j = 0; j < SEC[i].n; ++j) {
            int r = SEC[i].r0 + (int)j;
            float a = row_alpha(r);
            if (a <= 0.002f || (S.ctx_vis && S.ctx_row == r))
                continue;
            struct xf rx = row_xf(r);
            struct rc box = xf_rc(rx, row_box(r));
            if (box.y >= cl->y1 || box.y + box.h <= cl->y0)
                continue;
            EX = rx;
            EA = ma * ca * a;
            EC = rc_clip;
            struct rc b0 = row_box(r);
            t_rect(KEY(NS_ROW, (unsigned)r, 0), b0.x + 268, b0.y, M7_CW - 268 - 52, 2, 0, 0, M7_SEP, 1, 0);
            row_items(NS_ROW, r, b0.x, b0.y);
        }
        EX = L.cxf[i];
    }
}

static void pill_xf(struct xf *x, float *ga, float *ca)
{
    const struct m7_xf *ix = L.ix;
    float s = ix ? 1 : SP.pill_s.x, dy = ix ? ix->pill_dy : SP.pill_y.x;
    *x = xf_about(s, M7_ISLAND_X + M7_ISLAND_W / 2.0f, M7_ISLAND_Y + M7_ISLAND_H / 2.0f, 0, dy);
    *ga = ix ? ix->pill_a : SP.pill_a.x;
    *ca = ix ? ix->pill_in_a : SP.pill_a.x;
}

static void build_pill_items(float ma)
{
    struct xf px;
    float ga, ca;
    pill_xf(&px, &ga, &ca);
    if (ga < 0.01f)
        return;                     /* visibility: hidden */
    EX = px;
    EA = ma * ca;
    EC = 0;
    EF = 0;
    uint32_t k = KEY(NS_PILL, 0, 0);
    if (S.pill_kind == 0 && S.def >= 0) {
        const struct row *w = &R[S.def];
        unsigned n = (unsigned)S.autos;
        float frac = 1;
        if (S.cd) {
            double rem = S.cd_dur - (S.now - S.cd_t0);
            n = rem > 0 ? (unsigned)ceilf_((float)(rem / 1000)) : 0;
            frac = c01((float)(rem / S.cd_dur));
        }
        char b[64];
        struct sbuf s;
        sb_init(&s, b, sizeof(b));
        sb_s(&s, "Starting in ");
        sb_u(&s, n);
        sb_s(&s, " s · Tap to cancel");
        t_tile(k + 1, w->tile, SB7_TILE_92, 274, 198, 92, 1, 1);
        t_text(k + 2, SB7_F_IT600_46, w->name, 394, 234.8f, -0.012f, M7_L1, 1, G7_I_ELLIP, 636);
        t_text(k + 3, SB7_F_IN400_36, arena(b), 394, 285.1f, 0, M7_L2, 1, 0, 0);
        t_arc(k + 4, 1104, 244, 39, 7, 0, 360, C_WHITE16, 1);
        t_arc(k + 5, 1104, 244, 39, 7, 0, 360 * frac, M7_STAR, 1);
        sb_init(&s, b, sizeof(b));
        sb_u(&s, n);
        t_center(k + 6, SB7_F_IT700_40, arena(b), 1104, 258, 0, M7_L1, 1);
    } else {
        t_rect(k + 7, 274, 198, 92, 92, 46, 0, C_STAR16, 1, 0);
        t_img(k + 8, icon_54(S.pill_icon), 293, 217, 54, 54, M7_STAR, 1);
        t_text(k + 9, SB7_F_IT600_46, S.pill1, 394, 234.8f, -0.012f, M7_L1, 1, G7_I_ELLIP, 756);
        t_text(k + 10, SB7_F_IN400_36, S.pill2, 394, 285.1f, 0, M7_L2, 1, 0, 0);
    }
}

static const char *foot1(void) { return "Volume to move · Power to choose · Hold Power for options"; }
static const char *foot2(void) { return "Bixby goes back · Hold Bixby for Lab"; }

static void build_base(void)
{
    const struct m7_xf *ix = L.ix;
    E = &LAYB;
    g7_layer_reset(E);
    if (S.no_menu)
        return;
    float ma = ix ? ix->menu_a : 1, t[4];
    /* glass: collect(baseEls) in DOM order, then the lens */
    for (unsigned i = 0; i < NS; ++i) {
        float ga = ma * (ix ? ix->card_a[i] : 1);
        struct rc c = xf_rc(L.cxf[i], rcv(M7_X0, L.card_y[i], M7_CW, M7_CARD_HEAD + L.rows_h[i]));
        if (c.y > M7_VP_Y1 + 150 || c.y + c.h < M7_VP_Y0 - 150)
            continue;
        shape(KEY(0, 0x100 + i, 0), G7_S_CLIP, c, M7_CARD_R * L.cxf[i].s, ga, 1, TINT0);
        cat_tint(SEC[i].cat, t);
        shape(KEY(0, 0x200 + i, 0), G7_S_CLIP,
              xf_rc(L.cxf[i], rcv(M7_X0 + M7_PTILE_X, L.card_y[i] + 62, M7_PTILE, M7_PTILE)),
              M7_PTILE_R * L.cxf[i].s, ga, 0.9f, t);
    }
    float da = ma * (ix ? ix->dock_a : 1);
    shape(KEY(0, 0x300, 0), 0, rcv(M7_X0, M7_DOCK_Y + dock_dy(), M7_DOCK_W, M7_DOCK_H), 106, da, 1, TINT0);
    struct xf pw = tb_xf(T_POWER);
    shape(KEY(0, 0x301, 0), 0, xf_rc(pw, tb_box(T_POWER)), 106 * pw.s, da, 1, TINT0);
    struct xf px;
    float pga, pca;
    pill_xf(&px, &pga, &pca);
    float ti[4] = { M7_TINT_ISLAND };
    shape(KEY(0, 0x302, 0), 0, xf_rc(px, rcv(M7_ISLAND_X, M7_ISLAND_Y, M7_ISLAND_W, M7_ISLAND_H)),
          M7_ISLAND_R * px.s, pga * ma, 1, ti);
    lens_shape(KEY(0, 0x303, 0), SP.lb, &LBm, ix ? ix->lens_a : SP.lb[LA_].x);

    /* #status */
    EX = xf_id();
    EA = ma * (ix ? ix->status_a : 1);
    EC = 0;
    EF = 0;
    t_text(KEY(NS_STATUS, 0, 1), SB7_F_IT600_46, "Galaxy S9", M7_STATUS_X0, M7_STATUS_BASE, -0.01f, C_WHITE, 1, 0, 0);
    /* D8: the battery as the concept's #i-bat (viewBox 44 x 20 at 84 x 40, ending where the
     * concept's glyph ends, x 1348): the 45 % outline and nub, the fill (the concept's rect at
     * x 3.8, 27 wide for 87 %, so 31.4 at 100 %) and "87%" ending at 1250 (gap 14), from batt.c.
     * Green with a bolt while a cable is in, red under 15 % on battery; unknown (no answer from
     * the MAX77705): the outline only. */
    {
        const float u = 84.f / 44.f;
        t_rect(KEY(NS_STATUS, 0, 2), 1264 + 0.2f * u, 92 + 0.2f * u, 38.6f * u, 19.6f * u, 6.3f * u, 1.6f * u,
               C_WHITE, 0.45f, 0);
        t_rect(KEY(NS_STATUS, 0, 3), 1264 + 40.5f * u, 92 + 7 * u, 2 * u, 6 * u, 1 * u, 0, C_WHITE, 0.45f, 0);
        if (BAT.valid) {
            float fw = maxf(31.4f * (float)BAT.pct / 100.f, 1.5f);
            uint32_t fc = BAT.plugged ? C_BAT_CHG : BAT.pct < 15 ? C_BAT_LOW : C_WHITE;
            t_rect(KEY(NS_STATUS, 0, 4), 1264 + 3.8f * u, 92 + 3.8f * u, fw * u, 12.4f * u,
                   minf(3, fw / 2) * u, 0, fc, 1, 0);
            char pc[5];
            unsigned n = 0, p = BAT.pct;
            if (p >= 100)
                pc[n++] = '1';
            if (p >= 10)
                pc[n++] = (char)('0' + p / 10 % 10);
            pc[n++] = (char)('0' + p % 10);
            pc[n++] = '%';
            pc[n] = 0;
            const char *ps = arena(pc);
            t_text(KEY(NS_STATUS, 0, 5), SB7_F_IT600_46, ps, 1250 - tw(SB7_F_IT600_46, ps, -0.01f),
                   M7_STATUS_BASE, -0.01f, C_WHITE, 1, 0, 0);
            if (BAT.plugged)        /* the bolt centred on the body (x 1 .. 38, y 1 .. 19) */
                t_img(KEY(NS_STATUS, 0, 6), SB7_I_BOLT_30, 1264 + 19.5f * u - 9, 92 + 10 * u - 15, 18, 30,
                      C_WHITE, 1);
        }
    }

    /* #vp > #content */
    unsigned vp = g7_add_clip(E, 0, M7_VP_Y0, G7_W, M7_VP_Y1, 0);
    EC = vp;
    EF = G7_I_FADE;
    float cy = L.cy;
    EX = xf_about(1, 0, 0, 0, ix ? ix->brand_dy : 0);
    EA = ma * (ix ? ix->brand_a : 1);
    float bw = tw(SB7_F_IT300_56, "starbite", -0.01f);
    t_text(KEY(NS_TOP, 0, 1), SB7_F_IT300_56, "starbite", 82, cy + C_BRANDY + 48, -0.01f, C_BRAND, 1, 0, 0);
    t_img(KEY(NS_TOP, 0, 2), SB7_I_GSTAR_56, 82 + bw + 1.68f, cy + C_BRANDY + 1.12f, 23.52f, 23.52f, M7_STAR, 1);
    t_text(KEY(NS_TOP, 0, 3), SB7_F_IT700_142, "Choose a System", 76, cy + C_TITLE + 123, -0.036f, C_WHITE, 1, 0, 0);
    EX = xf_about(1, 0, 0, 0, ix ? ix->sub_dy : 0);
    EA = ma * (ix ? ix->sub_a : 1);
    if (S.def >= 0) {
        char b[96];
        struct sbuf s;
        sb_init(&s, b, sizeof(b));
        sb_s(&s, R[S.def].name);
        sb_s(&s, " starts by default");
        t_img(KEY(NS_TOP, 0, 4), SB7_I_STARF_40, 82, cy + C_SUB + 9.5f, 40, 40, M7_STAR, 1);
        t_text(KEY(NS_TOP, 0, 5), SB7_F_IN400_48, arena(b), 136, cy + C_SUB + 47, -0.004f, M7_L2, 1, 0, 0);
    } else {
        t_img(KEY(NS_TOP, 0, 6), SB7_I_INFO_54, 82, cy + C_SUB + 9.5f, 40, 40, M7_L2, 1);
        t_text(KEY(NS_TOP, 0, 7), SB7_F_IN400_48, "Hold Power on a system for options", 136, cy + C_SUB + 47,
               -0.004f, M7_L2, 1, 0, 0);
    }
    build_cards(vp, ma);
    EX = xf_id();
    EA = ma * (ix ? ix->foot_a : 1);
    EC = vp;
    EF = G7_I_FADE;
    t_center(KEY(NS_FOOT, 0, 1), SB7_F_IN400_38, foot1(), 720, L.foot_y + 43, 0, M7_L3, 1);
    t_center(KEY(NS_FOOT, 0, 2), SB7_F_IN400_38, foot2(), 720, L.foot_y + 43 + 58.9f, 0, M7_L3, 1);

    /* #toolbar: translateY(dock_dy); its content keeps full opacity in the intro (only the glass fades) */
    EC = 0;
    EF = 0;
    for (unsigned b = 0; b < 3; ++b) {
        struct rc r = tb_box(b);
        EX = tb_xf(b);
        EA = ma;
        float cx = r.x + r.w / 2;
        unsigned ic = b == T_RECOVERY ? SB7_I_LIFE_74 : b == T_DOWNLOAD ? SB7_I_DOWN_74 : SB7_I_FLASK_74;
        const char *lb = b == T_RECOVERY ? "Recovery" : b == T_DOWNLOAD ? "Download" : "Lab";
        t_img(KEY(NS_DOCK, b, 1), ic, cx - 37, r.y + 32.5f, 74, 74, C_WHITE, 1);
        t_center(KEY(NS_DOCK, b, 2), SB7_F_IN600_33, lb, cx, r.y + 146.5f, 0.005f, C_WHITE, 1);
    }
    EX = pw;
    struct rc pr = tb_box(T_POWER);
    t_img(KEY(NS_DOCK, 3, 1), SB7_I_POWER_84D, pr.x + 64, pr.y + 64, 84, 84, C_WHITE, 1);

    build_pill_items(ma);
}

/* ---- the over layer: #over (and #end) ---- */

static float toast_w(void)
{
    return 26 + 92 + 28 + maxf(tw(SB7_F_IT600_46, S.toast1, -0.01f), tw(SB7_F_IN400_36, S.toast2, 0)) + 64;
}

static void build_toast_shape(void)
{
    float a = SP.toast.x, sx = 0.72f + 0.28f * a, sy = 0.8f + 0.2f * a, w = toast_w();
    float cy = 176 + 68 + (1 - a) * -46;
    float t[4] = { M7_TINT_NOTICE };
    shape(KEY(0, 10, 0), G7_S_FROST, rcv(720 - w * sx / 2, cy - 68 * sy, w * sx, 136 * sy), 68 * sx,
          c01(a * 1.5f), 1, t);
}

static void build_toast_items(void)
{
    float a = SP.toast.x, sx = 0.72f + 0.28f * a, sy = 0.8f + 0.2f * a, w = toast_w(), x0 = 720 - w / 2;
    /* translate(-50%, (1 - a) * -46) scale(sx, sy) about its centre (720, 244): glyphs scale by sy,
     * x positions are squeezed by sx */
    EX.s = sy;
    EX.bx = 720 - sy * 720;
    EX.by = 244 - sy * 244 + (1 - a) * -46;
    EA = c01(a * 1.5f);
    EC = 0;
    EF = 0;
    float k2 = sx / sy, cx = 720 + (x0 + 72 - 720) * k2, tx0 = 720 + (x0 + 146 - 720) * k2;
    uint32_t k = KEY(NS_TOAST, 0, 0);
    t_rect(k + 1, cx - 46, 198, 92, 92, 46, 0, C_STAR16, 1, 0);
    t_img(k + 2, icon_54(S.toast_icon), cx - 27, 217, 54, 54, M7_STAR, 1);
    t_text(k + 3, SB7_F_IT600_46, S.toast1, tx0, 234.2f, -0.01f, M7_L1, 1, 0, 0);
    t_text(k + 4, SB7_F_IN400_36, S.toast2, tx0, 285.3f, 0, M7_L2, 1, 0, 0);
}

static void ctx_items(void)
{
    /* #ctxPrev: the row's innerHTML, padding-right 52, no hairline */
    EX = prev_xf();
    EA = 1;
    EC = 0;
    EF = 0;
    row_items(NS_PREV, S.ctx_row, S.ctx_r.x, S.ctx_r.y);
    /* #ctxMenu */
    EX = ctx_xf();
    EA = c01(SP.ctx.x * 1.6f);
    uint32_t k = KEY(NS_CTX, 0, 0);
    float t = ctx_top();
    int ok = ctx_ok(), dok = ctx_def_ok(), isdef = ctx_is_def();
    uint32_t c_start = ok ? C_WHITE : C_TEXT_OFF;
    t_text(k + 1, SB7_F_IN500_58, "Start", 314, t + 22 + 95, -0.01f, c_start, 1, 0, 0);
    t_img(k + 2, SB7_I_PLAY_58, 1072, t + 22 + 45, 58, 58, c_start, 1);
    t_rect(k + 3, CTX_X, t + 178, CTX_W, 2, 0, 0, C_MSEP, 1, 0);
    uint32_t c_def = dok && !isdef ? C_WHITE : C_TEXT_OFF;
    t_text(k + 4, SB7_F_IN500_58, isdef ? "Default System" : "Set as Default", 314, t + 188 + 95, -0.01f, c_def, 1, 0, 0);
    t_img(k + 5, isdef ? SB7_I_CHECK_58 : SB7_I_STAR_58, 1072, t + 188 + 45, 58, 58, c_def, 1);
    t_rect(k + 6, CTX_X, t + 344, CTX_W, 2, 0, 0, C_MSEP, 1, 0);
    uint32_t c_lab = dok ? M7_L2 : C_LAB_OFF;
    t_text(k + 7, SB7_F_IN400_42, "Start Automatically", 314, t + 354 + 76, 0, c_lab, 1, 0, 0);
    t_img(k + 8, SB7_I_TIMER_46, 1084, t + 354 + 38, 46, 46, c_lab, 1);
    for (unsigned g = 0; g < 4; ++g) {
        struct rc r = ctx_item(MI_AUTO0 + g);
        uint32_t f = FK(F_M, 0, MI_AUTO0 + g);
        int on = dok && S.seg_on == (int)g, kf = on && S.ofocus == f;
        /* background transition .25 s */
        float p = css_ease(c01((float)((S.now - S.seg_t) / 250)));
        float onv = on ? (S.seg_prev == (int)g ? 1 : p) : (S.seg_prev == (int)g && S.seg_on != (int)g ? 1 - p : 0);
        if (kf) {
            t_rect(k + 10 + g * 8, r.x - 12, r.y - 12, r.w + 24, r.h + 24, 64, 0, C_KF, 1, 0);
            t_rect(k + 11 + g * 8, r.x - 6, r.y - 6, r.w + 12, r.h + 12, 58, 0, C_SG_ON_TX, 1, 0);
        }
        if (dok) {
            t_rect(k + 12 + g * 8, r.x, r.y, r.w, r.h, 52, 0, C_SG, 1 - onv, 0);
            t_rect(k + 13 + g * 8, r.x, r.y, r.w, r.h, 52, 0, C_WHITE, onv, 0);
        } else {
            t_rect(k + 12 + g * 8, r.x, r.y, r.w, r.h, 52, 0, C_SG_OFF, 1, 0);
        }
        const char *s = g == 0 ? "Off" : g == 1 ? "3 s" : g == 2 ? "5 s" : "10 s";
        uint32_t c = !dok ? C_SG_OFF_TX : onv > 0.5f ? C_SG_ON_TX : C_WHITE;
        t_center(k + 14 + g * 8, SB7_F_IN600_46, s, r.x + r.w / 2, r.y + 69, -0.005f, c, 1);
    }
    t_rect(k + 50, CTX_X, t + 608, CTX_W, 16, 0, 0, C_THICK, 1, 0);
    t_text(k + 51, SB7_F_IN500_58, "Details", 314, t + 634 + 95, -0.01f, C_WHITE, 1, 0, 0);
    t_img(k + 52, SB7_I_INFO_58, 1072, t + 634 + 45, 58, 58, C_WHITE, 1);
}

static void sheet_items(void)
{
    int r = S.sheet_row, ok = R[r].st == ST_READY;
    float ty = sheet_ty();
    EX = xf_about(1, 0, 0, 0, ty);
    EA = 1;
    EC = 0;
    EF = 0;
    uint32_t k = KEY(NS_SHEET, 0, 0);
    t_rect(k + 1, 645, 1106, 150, 14, 7, 0, C_GRAB, 1, 0);
    t_img(k + 2, SB7_I_X_54, 1261, 1173, 54, 54, C_WHITE, 1);
    /* .shHead */
    t_shadow(k + 3, 96, 1184, 220, 220, 220 * 0.22f, 0, 18, 30, 0, C_SHADOW35, 1);
    t_tile(k + 4, R[r].tile, SB7_TILE_220, 96, 1184, 220, 1, 1);
    t_text(k + 5, SB7_F_IT700_92, R[r].name, 368, 1289, -0.028f, C_WHITE, 1, G7_I_ELLIP, 816);
    char b[96];
    struct sbuf s;
    sb_init(&s, b, sizeof(b));
    sb_s(&s, cat_title(SEC[R[r].sec].cat));
    sb_s(&s, " ·");
    const char *p1 = arena(b);
    float x = 368 + tw(SB7_F_IN400_46, p1, 0) + 14;
    t_text(k + 6, SB7_F_IN400_46, p1, 368, 1367, 0, M7_L2, 1, 0, 0);
    sb_init(&s, b, sizeof(b));
    if (ok) {
        t_rect(k + 7, x, 1340, 20, 20, 10, 0, M7_GREEN, 1, 0);
        x += 34;
        sb_s(&s, "Ready");
        const char *tg = tag_title(R[r].tag);
        if (tg) {
            sb_s(&s, " · ");
            sb_s(&s, tg);
        }
    } else {
        sb_s(&s, R[r].st == ST_COMING ? "Coming Soon" : "Not Installed");
    }
    t_text(k + 8, SB7_F_IN400_46, arena(b), x, 1367, 0, M7_L2, 1, 0, 0);
    /* .grp */
    unsigned n = sheet_nrows();
    t_rect(k + 9, 96, 1470, 1248, (float)n * 138, 64, 0, C_GRP, 1, 0);
    for (unsigned i = 0; i < n; ++i) {
        float y = 1470 + (float)i * 138;
        uint32_t kk = k + 16 + i * 8;
        if (i)
            t_rect(kk, 148, y, 1196, 2, 0, 0, M7_SEP, 1, 0);
        if (i < SRn) {
            t_text(kk + 1, SB7_F_IN400_50, SRW[i].l, 148, y + 87, 0, C_WHITE, 1, 0, 0);
            unsigned f = SRW[i].mono ? SB7_F_JB500_40 : SB7_F_IN400_46;
            t_text(kk + 2, f, SRW[i].v, 1292 - tw(f, SRW[i].v, 0), y + (SRW[i].mono ? 83.5f : 86), 0, M7_L2, 1, 0, 0);
        } else if (i == SRn) {
            t_text(kk + 1, SB7_F_IN400_50, "Default System", 148, y + 87, 0, C_WHITE, 1, 0, 0);
            float p = S.sw_on ? cubic_bezier(0.3f, 1.4f, 0.5f, 1, c01((float)((S.now - S.sw_t) / 380))) : 0;
            float pb = S.sw_on ? css_ease(c01((float)((S.now - S.sw_t) / 250))) : 0;
            t_rect(kk + 2, 1104, y + 12, 188, 114, 57, 0, C_SW_OFF, 1 - pb, 0);
            t_rect(kk + 3, 1104, y + 12, 188, 114, 57, 0, M7_GREEN, pb, 0);
            t_shadow(kk + 4, 1112 + 74 * p, y + 20, 98, 98, 49, 0, 6, 16, 0, C_SHADOW25, 1);
            t_rect(kk + 5, 1112 + 74 * p, y + 20, 98, 98, 49, 0, C_WHITE, 1, 0);
        } else {
            t_text(kk + 1, SB7_F_IN400_50, "Start Automatically", 148, y + 87, 0, C_WHITE, 1, 0, 0);
            const char *v = "Off";
            if (r == S.def && S.autos) {
                sb_init(&s, b, sizeof(b));
                sb_s(&s, "After ");
                sb_u(&s, (unsigned)S.autos);
                sb_s(&s, " s");
                v = arena(b);
            }
            t_text(kk + 2, SB7_F_IN400_46, v, 1292 - tw(SB7_F_IN400_46, v, 0), y + 86, 0, M7_L2, 1, 0, 0);
        }
    }
    /* #shStart */
    sb_init(&s, b, sizeof(b));
    if (ok) {
        sb_s(&s, "Start ");
        sb_s(&s, R[r].name);
    } else {
        sb_s(&s, R[r].st == ST_COMING ? "Coming Soon" : "Not Installed");
    }
    t_center(k + 100, SB7_F_IN600_58, arena(b), 720, 2797, -0.008f, ok ? C_WHITE : C_START_OFF, 1);
}

static void alert_items(void)
{
    struct xf ax = alert_xf();
    EX = ax;
    EA = c01(SP.alert.x * 1.4f);
    EC = 0;
    EF = 0;
    uint32_t k = KEY(NS_ALERT, 0, 0);
    float t = S.alert_top, lx = M7_ALERT_X + 72;
    int red = S.alert == AL_POWER;
    t_rect(k + 1, lx, t + 76, 150, 150, 75, 0, red ? C_RED16 : C_STAR16, 1, 0);
    unsigned ic = S.alert == AL_POWER ? SB7_I_POWER_84 : S.alert == AL_RECOVERY ? SB7_I_LIFE_84 : SB7_I_DOWN_84;
    t_img(k + 2, ic, lx + 33, t + 76 + 33, 84, 84, red ? M7_RED : M7_STAR, 1);
    t_text(k + 3, SB7_F_IT700_70, alert_title(), lx, t + 272 + 63, -0.02f, C_WHITE, 1, 0, 0);
    unsigned st[WRAP_MAX], ln[WRAP_MAX];
    const char *m = alert_msg();
    unsigned nl = wrap(SB7_F_IN400_48, m, 0, AL_MSG_W, st, ln);
    for (unsigned i = 0; i < nl; ++i) {
        char b[160];
        unsigned n = ln[i] < sizeof(b) - 1 ? ln[i] : sizeof(b) - 1;
        for (unsigned c = 0; c < n; ++c)
            b[c] = m[st[i] + c];
        b[n] = 0;
        t_text(k + 4 + i, SB7_F_IN400_48, arena(b), lx, t + 373.6f + 50 + (float)i * AL_LH, 0, M7_L2, 1, 0, 0);
    }
    for (unsigned i = 0; i < alert_nb(); ++i) {
        struct rc r = alert_btn(i);
        unsigned act = alert_act(i);
        EX = xf_cat(ax, xf_about(press_of(FK(F_A, 0, act)), r.x + r.w / 2, r.y + r.h / 2, 0, 0));
        t_center(k + 16 + i, SB7_F_IN600_56, alert_btn_text(act), r.x + r.w / 2, r.y + 100, -0.008f, C_WHITE, 1);
    }
}

static void wrap_center(uint32_t k, unsigned font, const char *s, float track, float maxw, float base,
                        float lh, uint32_t c)
{
    unsigned st[WRAP_MAX], ln[WRAP_MAX], n = wrap(font, s, track, maxw, st, ln);
    for (unsigned i = 0; i < n; ++i) {
        char b[200];
        unsigned m = ln[i] < sizeof(b) - 1 ? ln[i] : sizeof(b) - 1;
        for (unsigned j = 0; j < m; ++j)
            b[j] = s[st[i] + j];
        b[m] = 0;
        t_center(k + i, font, arena(b), 720, base + (float)i * lh, track, c, 1);
    }
}

static void launch_items(unsigned clip)
{
    const struct cat_ent *e = cat_get((unsigned)S.run_cat);
    int row = S.run_row;
    unsigned tile = row >= 0 ? R[row].tile : sb7_tile_for(e->key, e->category);
    uint32_t k = KEY(NS_RUN, 0, 0);
    /* #lnRun */
    EX = run_xf();
    EA = SP.run_a.x;
    EC = clip;
    EF = 0;
    t_shadow(k + 1, 530, 980, 380, 380, 380 * 0.22f, 0, 40, 70, 0, C_SHADOW45, 1);
    t_tile(k + 2, tile, SB7_TILE_380, 530, 980, 380, 1, 1);
    char b[160], nm[64];
    struct sbuf s;
    cat_utf8(nm, sizeof(nm), e->name);
    t_center(k + 3, SB7_F_IT700_92, arena(nm), 720, 1549, -0.028f, C_WHITE, 1);
    cat_utf8(b, sizeof(b), e->version);
    sb_init(&s, b + slen(b), sizeof(b) - slen(b));
    const char *tg = tag_title(e->tag);
    if (tg) {
        sb_s(&s, " · ");
        sb_s(&s, tg);
    }
    t_center(k + 4, SB7_F_IN400_48, arena(b), 720, 1637, 0, M7_L2, 1);
    t_rect(k + 5, 400, 1790, 640, 16, 8, 0, C_BAR, 1, 0);
    float fw = 640 * c01(S.bar);
    t_rect(k + 6, 400, 1790, fw, 16, minf(8, fw / 2), 0, C_WHITE, 1, 0);
    t_center(k + 7, SB7_F_IN400_40, S.run_stage, 720, 1905, 0, M7_L2, 1);
    t_center(k + 8, SB7_F_JB500_34, S.run_det, 720, 1959, 0, M7_L3, 1);
    t_center(k + 9, SB7_F_IN400_38, "Bixby to cancel", 720, 2737, 0, M7_L3, SP.hint.x);
    if (!S.lnerr_vis)
        return;
    /* #lnErr */
    struct xf ex = err_xf();
    EX = ex;
    EA = SP.err_a.x;
    uint32_t q = KEY(NS_ERR, 0, 0);
    t_rect(q + 1, 580, 800, 280, 280, 140, 0, C_WARN13, 1, 0);
    t_rect(q + 2, 580, 800, 280, 280, 140, 2, C_WARN18, 1, 0);
    t_img(q + 3, SB7_I_WARN_150, 645, 865, 150, 150, C_WHITE, 1);
    wrap_center(q + 4, SB7_F_IT700_100, S.err_title, -0.03f, 1240, 1248, 104, C_WHITE);
    float cw = 40 + tw(SB7_F_IN600_46, S.err_why, 0) + 40;
    t_rect(q + 10, 720 - cw / 2, 1310, cw, 88, 44, 0, C_CHIP, 1, 0);
    t_text(q + 11, SB7_F_IN600_46, S.err_why, 720 - cw / 2 + 40, 1371, 0, C_CHIP_TX, 1, 0, 0);
    wrap_center(q + 12, SB7_F_IN400_50, S.err_msg, 0, 1100, 1513, 70, M7_L2);
    t_center(q + 20, SB7_F_JB500_36, S.err_code, 720, 1776, 0, M7_L3, 1);
    for (unsigned b2 = 0; b2 < 2; ++b2) {
        struct rc r = err_btn(b2);
        EX = xf_cat(ex, xf_about(press_of(FK(F_E, 0, b2)), r.x + r.w / 2, r.y + r.h / 2, 0, 0));
        t_center(q + 21 + b2, SB7_F_IN600_58, b2 == E_RETRY ? "Try Again" : "Back", 720, r.y + 109, 0, C_WHITE, 1);
    }
}

static void end_items(void)
{
    float a = css_ease(c01((float)((S.now - S.end_t) / 500)));
    EX = xf_id();
    EA = 1;
    EC = 0;
    EF = 0;
    uint32_t k = KEY(NS_END, 0, 0);
    t_rect(k + 1, 0, 0, G7_W, G7_H, 0, 0, C_BLACK, a, 0);
    int spin = S.end_kind != EN_OFF;
    float y0 = spin ? 1480 : 1300;
    if (spin)
        for (unsigned i = 0; i < 8; ++i) {
            float an = (float)i * 45 * PI_F / 180, cs = cosf_(an), sn = sinf_(an);
            float xs[2], ys[2], d0 = -54, d1 = -30;
            xs[0] = 720 - d0 * sn;
            ys[0] = 1360 + d0 * cs;
            xs[1] = 720 - d1 * sn;
            ys[1] = 1360 + d1 * cs;
            t_stroke(k + 2 + i, 2, xs, ys, 12, C_WHITE, a * (0.25f + 0.1f * (float)i));
        }
    const char *t = "", *sub = "";
    char b[96];
    struct sbuf s;
    switch (S.end_kind) {
    case EN_HANDOFF:
        sb_init(&s, b, sizeof(b));
        sb_s(&s, "Starting");
        if (S.run_cat >= 0) {
            char nm[64];
            cat_utf8(nm, sizeof(nm), cat_get((unsigned)S.run_cat)->name);
            sb_s(&s, " ");
            sb_s(&s, nm);
        }
        t = arena(b);
        break;
    case EN_OFF: t = "Galaxy S9 is off"; sub = "Press Power to turn it on."; break;
    case EN_RESTART: t = "Restarting"; break;
    case EN_RECOVERY: t = "Restarting to TWRP"; break;
    case EN_DOWNLOAD: t = "Entering Download Mode"; break;
    default: t = "Couldn’t Power Off"; sub = "A charger keeps Galaxy S9 on. Restarting…"; break;
    }
    t_center(k + 12, SB7_F_IT600_54, t, 720, y0 + 54, -0.01f, C_WHITE, a * (S.end_kind == EN_OFF ? 0.4f : 1));
    if (S.end_kind == EN_HANDOFF)
        t_center(k + 13, SB7_F_JB500_34, S.end_det, 720, y0 + 70.2f + 22 + 44, 0, M7_END_SUB, a);
    else
        wrap_center(k + 13, SB7_F_IN400_40, sub, 0, 1440, y0 + 70.2f + 22 + 44, 60, M7_END_SUB);
}

static void build_over(void)
{
    E = &LAYO;
    g7_layer_reset(E);
    float t[4];
    /* glass: collect(overEls) in DOM order, then the overlay lens */
    if (S.ctx_vis) {
        struct xf p = prev_xf();
        shape(KEY(0, 1, 0), 0, xf_rc(p, S.ctx_r), 64 * p.s, c01(SP.lift.x * 4 + (S.ov == OV_CTX ? 1 : 0)), 1, TINT0);
        struct xf m = ctx_xf();
        set4(t, M7_TINT_MENU);
        shape(KEY(0, 2, 0), G7_S_FROST, xf_rc(m, rcv(CTX_X, ctx_top(), CTX_W, CTX_H)), M7_MENU_R * m.s,
              c01(SP.ctx.x * 1.6f), 1, t);
    }
    if (S.sheet_vis) {
        float ty = sheet_ty();
        set4(t, M7_TINT_SHEET);
        shape(KEY(0, 3, 0), G7_S_FROST, rcv(M7_SHEET_X, M7_SHEET_Y + ty, M7_SHEET_W, M7_SHEET_H), M7_SHEET_R, 1, 1, t);
        shape(KEY(0, 4, 0), 0, sheet_el(SH_CLOSE), 68, 1, 1, TINT0);
        if (R[S.sheet_row].st == ST_READY)
            set4(t, M7_TINT_BLUE);
        else
            set4(t, M7_TINT_OFF);
        shape(KEY(0, 5, 0), 0, sheet_el(SH_START), 88, 1, 1, t);
    }
    if (S.alert_vis) {
        struct xf ax = alert_xf();
        float a = c01(SP.alert.x * 1.4f);
        set4(t, M7_TINT_ALERT);
        shape(KEY(0, 6, 0), G7_S_FROST, xf_rc(ax, rcv(M7_ALERT_X, S.alert_top, M7_ALERT_W, S.alert_h)),
              118 * ax.s, a, 1, t);
        for (unsigned i = 0; i < alert_nb(); ++i) {
            unsigned act = alert_act(i);
            struct rc r;
            if (act == A_OFF)
                set4(t, M7_TINT_RED);
            else if (act == A_RECOVERY || act == A_DOWNLOAD)
                set4(t, M7_TINT_BLUE);
            else
                set4(t, M7_TINT_GRAY);
            if (rect_of(FK(F_A, 0, act), &r, 0))
                shape(KEY(0, 7 + i, 0), 0, r, 80 * r.w / (M7_ALERT_W - 144), a, 1, t);
        }
    }
    if (S.toast_vis)
        build_toast_shape();
    float lx = SP.lx.x + SP.shake.x, ly = SP.ly.x, lw = SP.lw.x, lh = SP.lh.x, lr = SP.lr.x;
    if (S.launch_vis) {
        float et = SP.err_tint.x, la = 1;
        t[0] = lerpf(0.02f, 0.14f, et);
        t[1] = lerpf(0.035f, 0.03f, et);
        t[2] = lerpf(0.08f, 0.05f, et);
        t[3] = lerpf(0.34f, 0.44f, et);
        if (S.launch_closing)
            la = minf(1, c01((SP.lw.x - SP.lw.t) / 400) * 1.5f);
        shape(KEY(0, 11, 0), 0, rcv(lx, ly, lw, lh), lr, la, 1.15f, t);
        if (S.lnerr_vis)
            for (unsigned b = 0; b < 2; ++b) {
                struct rc r;
                if (b == E_RETRY)
                    set4(t, M7_TINT_BLUE);
                else
                    set4(t, M7_TINT_BACK);
                if (rect_of(FK(F_E, 0, b), &r, 0))
                    shape(KEY(0, 12 + b, 0), 0, r, 88 * r.w / 980, SP.err_a.x, 1, t);
            }
    }
    lens_shape(KEY(0, 14, 0), SP.lo, &LOm, SP.lo[LA_].x);

    /* items, in #over's DOM order */
    if (S.ctx_vis)
        ctx_items();
    if (S.sheet_vis)
        sheet_items();
    if (S.alert_vis)
        alert_items();
    if (S.toast_vis)
        build_toast_items();
    if (S.launch_vis) {
        unsigned c = g7_add_clip(E, lx, ly, lx + lw, ly + lh, lr);
        launch_items(c);
    }
    if (S.end_vis)
        end_items();
}

static int M_inited;
static void core_init(void);

void m7_build(struct g7_frame *f, const struct m7_xf *xf)
{
#ifdef M7_PREVIEW
    if (!M_inited) {                /* h7 --state intro: intro7_frame without m7_preview_state */
        core_init();
        M_inited = 1;
        m7_prepare_boot();
    }
#endif
    arena_n = 0;
    L.ix = xf;
    layout();
    if (xf) {
        IX.pill_a = xf->pill_a;
        IX.pill_dy = xf->pill_dy;
        IX.lens_a = xf->lens_a;
    }
    build_base();
    build_over();
    f->base = &LAYB;
    f->over = &LAYO;
    f->wall.sx = G7_WALL_FINAL_SX;
    f->wall.sy = G7_WALL_FINAL_SY;
    f->wall.si = G7_WALL_FINAL_SI;
    f->wall.sr = G7_WALL_FINAL_SR;
    f->wall.sw = G7_WALL_FINAL_SW;
    f->wall.rr = G7_WALL_FINAL_RR;
    f->wall.rf = G7_WALL_FINAL_RF;
    f->wall.amb = G7_WALL_FINAL_AMB;
    f->word.tm = 0;
    f->word.tf = 0;
    f->word.ta = 0;
    f->word.cap[0] = 720;
    f->word.cap[1] = 1380;
    f->word.cap[2] = 840;
    f->word.cap[3] = 124;
    f->dim = 0.8f;
    f->back = SP.back.x;
    f->lx = 0;
    f->ly = 0;
    f->flags = (S.full ? G7_F_FULL : 0) | (S.refreeze ? G7_F_REFREEZE : 0);
    S.full = 0;
    S.refreeze = 0;
    L.ix = 0;
}

/* ================================================================================================
 * input (doc 4.2.6): keys debounced 20 ms, Power hold 520 ms, Bixby hold 700 ms, Volume repeat,
 * touch finger 0 in panel px
 * ============================================================================================== */

uint64_t m7_now_us(void)
{
    uint64_t t = hw_ticks(), f = hw_freq();
    if (!f)
        f = 26000000U;
    return t / f * 1000000U + t % f * 1000000U / f;
}

static struct inp {
    int inited;
    int render;                     /* g7's poll hook samples the keys (the menu or the intro runs) */
    unsigned stable, raw, ignore;
    uint64_t raw_t, praw_t, braw_t, pdown, bdown, rep_up, rep_dn, vol_t, hook_t;
    int pheld, bheld, vol;
    struct m7_ev q[16];
    unsigned qh, qn;
} IN;

static void q_push(unsigned type, int x, int y, uint64_t t)
{
    if (IN.qn >= 16)
        return;
    struct m7_ev *e = &IN.q[(IN.qh + IN.qn) % 16];
    e->type = (uint8_t)type;
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->t_us = t;
    ++IN.qn;
}

/* keys down now are ignored until they are released (a key held through the intro or Lab) */
static void in_ignore_held(void)
{
    IN.stable = IN.raw = hw_keys();
    IN.ignore = IN.stable;
    IN.raw_t = m7_now_us();
}

/* The keys at now: debounce, holds (timed from when the key went down, and only while it is still
 * down: a release seen late is a press, not a hold), repeat, and both Volume keys held: power off
 * at once, with no drawing (v5 / v6 K_VOL_HOLD), so the way out never waits for a frame. */
static void in_keys(uint64_t now)
{
    if (!IN.inited) {
        IN.inited = 1;
        in_ignore_held();
    }
    unsigned k = hw_keys();
    if (k != IN.raw) {
        unsigned ch = k ^ IN.raw;
        if (ch & K_POWER)
            IN.praw_t = now;
        if (ch & K_BIXBY)
            IN.braw_t = now;
        IN.raw = k;
        IN.raw_t = now;
    }
    if (IN.raw != IN.stable && now - IN.raw_t >= M7_DEBOUNCE_MS * 1000U) {
        unsigned ch = IN.raw ^ IN.stable, dn = ch & IN.raw, up = ch & IN.stable;
        IN.stable = IN.raw;
        unsigned ign_up = up & IN.ignore;
        IN.ignore &= ~up;
        up &= ~ign_up;
        dn &= ~IN.ignore;
        if (dn & K_UP) {
            q_push(M7_EV_UP, 0, 0, now);
            IN.rep_up = now + M7_REPEAT_MS * 1000U;
        }
        if (dn & K_DOWN) {
            q_push(M7_EV_DOWN, 0, 0, now);
            IN.rep_dn = now + M7_REPEAT_MS * 1000U;
        }
        if (dn & K_POWER) {
            q_push(M7_EV_POWER_DOWN, 0, 0, now);
            IN.pdown = IN.praw_t;
            IN.pheld = 0;
        }
        if (dn & K_BIXBY) {
            IN.bdown = IN.braw_t;
            IN.bheld = 0;
        }
        if ((up & K_POWER) && !IN.pheld)
            q_push(M7_EV_POWER, 0, 0, now);
        if ((up & K_BIXBY) && !IN.bheld)
            q_push(M7_EV_BIXBY, 0, 0, now);
    }
    unsigned live = IN.stable & ~IN.ignore;
    if ((live & IN.raw & K_POWER) && !IN.pheld && now - IN.pdown >= M7_HOLD_POWER_MS * 1000U) {
        IN.pheld = 1;
        q_push(M7_EV_POWER_HOLD, 0, 0, now);
    }
    if ((live & IN.raw & K_BIXBY) && !IN.bheld && now - IN.bdown >= M7_HOLD_BIXBY_MS * 1000U) {
        IN.bheld = 1;
        q_push(M7_EV_BIXBY_HOLD, 0, 0, now);
    }
    if ((live & K_UP) && now >= IN.rep_up) {
        q_push(M7_EV_UP, 0, 0, now);
        IN.rep_up += M7_REPEAT_EVERY_MS * 1000U;
    }
    if ((live & K_DOWN) && now >= IN.rep_dn) {
        q_push(M7_EV_DOWN, 0, 0, now);
        IN.rep_dn += M7_REPEAT_EVERY_MS * 1000U;
    }
    if ((live & IN.raw & (K_UP | K_DOWN)) == (K_UP | K_DOWN)) {
        if (!IN.vol) {
            IN.vol = 1;
            IN.vol_t = now;
        } else if (now - IN.vol_t >= M7_VOL_HOLD_MS * 1000U) {
            hw_power_off();
            /* still on: a charger keeps the PMIC up */
            hw_reboot(INFORM_NORMAL);
            IN.vol_t = now;
        }
    } else {
        IN.vol = 0;
    }
}

/* g7_set_poll: the keys between rows of a frame (rate-limited to every 2 ms); touch stays in the
 * controller's FIFO until the frame ends */
static void in_render_poll(void)
{
    if (!IN.render)
        return;
    uint64_t now = m7_now_us();
    if (now - IN.hook_t < 2000)
        return;
    IN.hook_t = now;
    in_keys(now);
}

static void in_poll(void)
{
    uint64_t now = m7_now_us();
    in_keys(now);
    int st, x, y;
    while (IN.qn < 16 && hw_tp_event(&st, &x, &y))
        q_push(st == 1 ? M7_EV_TOUCH_DOWN : st == 2 ? M7_EV_TOUCH_MOVE : M7_EV_TOUCH_UP, x, y, now);
}

int m7_input(struct m7_ev *ev)
{
    in_poll();
    if (!IN.qn)
        return 0;
    const struct m7_ev *e = &IN.q[IN.qh];
    ev->type = e->type;
    ev->x = e->x;
    ev->y = e->y;
    ev->t_us = e->t_us;
    IN.qh = (IN.qh + 1) % 16;
    --IN.qn;
    return 1;
}

/* ================================================================================================
 * statistics (doc 2.3): log slots 120..127
 * ============================================================================================== */

#ifndef M7_PREVIEW
#define M7_LOG 0x91fff000UL
static void log_put(unsigned slot, uint64_t v)
{
    ((volatile uint64_t *)M7_LOG)[slot] = v;
}
#else
static void log_put(unsigned slot, uint64_t v)
{
    (void)slot;
    (void)v;
}
#endif

static struct { uint32_t menu_frames, menu_worst, expand_worst, launch_worst, intro_frames, intro_worst, flags; } ST;

/* One line in the pmsg ring (TWRP: /sys/fs/pstore/pmsg-ramoops-0 after the next reboot): why v7
 * stopped or what it picked, the bake, and log slots 119..139. The log page itself is not readable
 * from TWRP or Linux. */
#ifndef M7_PREVIEW
void star_pmsg(const char *s);
static void rep_hex(char *b, unsigned *n, uint64_t v)
{
    int d = 15;
    while (d > 0 && !((v >> (4 * d)) & 15))
        --d;
    for (; d >= 0; --d)
        b[(*n)++] = "0123456789abcdef"[(v >> (4 * d)) & 15];
}
static void rep_str(char *b, unsigned *n, const char *s)
{
    while (*s && *n < 900)
        b[(*n)++] = *s++;
}
/* Micro-benchmarks for the report (us each): a 20M-iteration subs/b.ne loop (about 1 cycle each:
 * MHz = 20e6 / us), a 5M dependent fmadd chain, 2 MiB WB -> WB copy in ui.c's scratch (unused by
 * v7), 2 MiB canvas -> framebuffer (the same pixels it already shows), and SCTLR_EL1. */
static uint64_t bench_us(int k)
{
    uint64_t t0 = m7_now_us();
    if (k == 0) {
        uint64_t c = 20000000;
        __asm__ volatile("1: subs %0, %0, #1\n b.ne 1b" : "+r"(c));
    } else if (k == 1) {
        float x = 1.f, m = .999f, a = .001f;
        for (int i = 0; i < 5000000; ++i)
            __asm__ volatile("fmadd %s0, %s0, %s1, %s2" : "+w"(x) : "w"(m), "w"(a));
    } else if (k == 2) {
        const volatile uint64_t *src = (const volatile uint64_t *)0x88800000UL;
        volatile uint64_t *dst = (volatile uint64_t *)0x88a00000UL;
        for (unsigned i = 0; i < (2U << 20) / 8; ++i)
            dst[i] = src[i];
    } else {
        const volatile uint64_t *src = (const volatile uint64_t *)G7_CANVAS;
        volatile uint64_t *dst = (volatile uint64_t *)0xcc000000UL;
        for (unsigned i = 0; i < (2U << 20) / 8; ++i)
            dst[i] = src[i];
        __asm__ volatile("dsb st" ::: "memory");
    }
    return m7_now_us() - t0;
}

void m7_report(const char *why)
{
    static char b[1024];
    unsigned n = 0;
    struct g7_stats g;
    g7_get_stats(&g);
    rep_str(b, &n, "STARBITE v7: ");
    rep_str(b, &n, why);
    rep_str(b, &n, " flags ");
    rep_hex(b, &n, ST.flags);
    rep_str(b, &n, " bake_us ");
    rep_hex(b, &n, g.bake_us);
    for (unsigned i = 0; i < 4; ++i) {
        rep_str(b, &n, i ? "," : " parts ");
        rep_hex(b, &n, g.bake_part[i]);
    }
    rep_str(b, &n, " bench alu,fma,wbcopy,fbcopy_us ");
    for (int k = 0; k < 4; ++k) {
        if (k)
            rep_str(b, &n, ",");
        uint64_t us = bench_us(k);
        log_put(140U + (unsigned)k, us);   /* also in the log page (readable from Linux /dev/mem) */
        rep_hex(b, &n, us);
    }
    uint64_t sc;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sc));
    log_put(144, sc);
    /* glass.c's phase totals (glass.c PROF: 0 setup (diff, the shift), 1 base glass, 2 base items
     * (the freeze only), 3 frozen crossfade, 4 over glass (and the curtain), 5, 6 unused, 7 present
     * (the 2x, the content items, the edge), 8 freeze, 9 the intro's wallpaper; ticks -> ms;
     * 10, 11 rows shifted / shaded again, counts), slots 145..156 */
    extern uint64_t g7_prof[12];
    rep_str(b, &n, " prof_ms");
    for (int k = 0; k < 12; ++k) {
        uint64_t v = k < 10 ? g7_prof[k] / 26000U : g7_prof[k];
        log_put(145U + (unsigned)k, v);
        rep_str(b, &n, k ? "," : " ");
        rep_hex(b, &n, v);
    }
    rep_str(b, &n, " sctlr ");
    rep_hex(b, &n, sc);
    rep_str(b, &n, " slots");
    for (unsigned k = 119; k <= 139 && n < 990 - 20; ++k) {
        rep_str(b, &n, " ");
        rep_hex(b, &n, ((volatile uint64_t *)M7_LOG)[k]);
    }
    b[n++] = '\n';
    b[n] = 0;
    star_pmsg(b);
}
#else
void m7_report(const char *why)
{
    (void)why;
}
#endif

static void stats_log(void)
{
    struct g7_stats g;
    g7_get_stats(&g);
    log_put(120, (uint64_t)ST.intro_frames << 32 | ST.intro_worst);
    log_put(121, (uint64_t)ST.menu_frames << 32 | ST.menu_worst);
    log_put(122, ST.expand_worst);
    log_put(123, ST.launch_worst);
    log_put(124, g.freeze_us / 1000U);
    log_put(125, g.bake_us / 1000U);
    log_put(127, ST.flags);
}

/* ================================================================================================
 * the main loop (doc 4.2.3)
 * ============================================================================================== */

/* per-class frame statistics: log slots 128 + class, frames << 48 | worst ms << 32 | total ms (the
 * total kept in us: no per-frame rounding). Slot 139: the renderer's stamp M7_RSTAMP << 32 | the
 * panel pixels presented so far / 1024 (with slots 128.. and 152 (present ms), ms per Mpx). */
#define M7_RSTAMP 0x4d563401ULL      /* "MV4", 1: canvas 720 x 1480, 2x present, items at panel resolution */
int m7_phase_last;
static struct { uint32_t n, worst; uint64_t total_us; } PHS[M7_PH_N];
static uint64_t px_total;
static int phase_sync = -1;          /* menu7_progress / menu7_handoff frames */

uint32_t m7_frame_timed(const struct g7_frame *f, int ph)
{
    if (ph < 0 || ph >= M7_PH_N)
        ph = M7_PH_IDLE;
    m7_phase_last = ph;
    uint64_t t0 = m7_now_us();
    uint32_t px = g7_frame(f);
    if (!px)
        return 0;
    uint64_t us = m7_now_us() - t0;
    uint32_t ms = (uint32_t)((us + 500) / 1000);
    PHS[ph].n++;
    PHS[ph].total_us += us;
    if (ms > PHS[ph].worst)
        PHS[ph].worst = ms;
    uint64_t tot = (PHS[ph].total_us + 500) / 1000;
    log_put(128U + (unsigned)ph, (uint64_t)(PHS[ph].n > 0xffffU ? 0xffffU : PHS[ph].n) << 48 |
                                     (uint64_t)(PHS[ph].worst > 0xffffU ? 0xffffU : PHS[ph].worst) << 32 |
                                     (tot > 0xffffffffULL ? 0xffffffffULL : tot));
    px_total += px;
    log_put(139, M7_RSTAMP << 32 | ((px_total >> 10) & 0xffffffffULL));
    return px;
}

static int moving_any(const struct m7_spring *s, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        if (sp_moving(&s[i]))
            return 1;
    return 0;
}

/* what this menu frame shows (M7_PH_*) */
static int menu_phase(void)
{
    if (phase_sync >= 0)
        return phase_sync;
    if (S.end_vis)
        return M7_PH_END;
    if (S.mode == MD_LAUNCH || S.launch_vis)
        return M7_PH_LAUNCH;
    if (sp_moving(&SP.back) || sp_moving(&SP.ctx) || sp_moving(&SP.sub) || sp_moving(&SP.alert) ||
        sp_moving(&SP.sheet) || sp_moving(&SP.lift))
        return M7_PH_OVER_MOVE;
    if (S.ctx_vis || S.alert_vis || S.sheet_vis)
        return M7_PH_OVER_REST;
    for (unsigned i = 0; i < NS; ++i)
        if (sp_moving(&SP.open[i]))
            return M7_PH_EXPAND;
    if (sp_moving(&SP.scroll) || sp_moving(&SP.room) || sp_moving(&SP.pill_y) || sp_moving(&SP.pill_s))
        return M7_PH_LIST;
    if (moving_any(SP.lb, 6))
        return M7_PH_LENS;
    return M7_PH_IDLE;
}

static const struct star_info *M_in;

int m7_rd_range(uint64_t *start, uint64_t *end)
{
    if (!M_in)
        return 0;
    *start = M_in->rd_start;
    *end = M_in->rd_end;
    return 1;
}

static char *M_cmd;
static unsigned M_len;
static int M_inited;
static double anim_until;           /* time-based transitions (end fade, segments, the switch) */

static void frame_out(void)
{
    struct g7_frame f;
    int expanding = 0, launching = 0;
    for (unsigned i = 0; i < NS; ++i)
        expanding |= sp_moving(&SP.open[i]);
    launching = S.launch_vis && (sp_moving(&SP.lw) || sp_moving(&SP.lh));
    m7_build(&f, 0);
    uint64_t t0 = m7_now_us();
    (void)m7_frame_timed(&f, menu_phase());
    uint32_t ms = (uint32_t)((m7_now_us() - t0 + 500) / 1000);
    ++ST.menu_frames;
    if (ms > ST.menu_worst)
        ST.menu_worst = ms;
    if (expanding && ms > ST.expand_worst)
        ST.expand_worst = ms;
    if (launching && ms > ST.launch_worst)
        ST.launch_worst = ms;
    if (ST.menu_frames == 1) {
        uint64_t f2 = hw_freq() ? hw_freq() : 26000000U;
        log_put(126, hw_ticks() * 1000U / f2);
    }
    stats_log();
}

static int timers_pending(void)
{
    for (unsigned i = 0; i < NTMR; ++i)
        if (TM[i].kind != T_NONE_)
            return 1;
    return 0;
}

/* frame-budget mode: a full-panel frame takes longer than M7_SLOW_MS (the intro's first frame
 * already says) */
static int m7_slow(void)
{
    struct g7_stats g;
    g7_get_stats(&g);
    return g.full_us > M7_SLOW_MS * 1000U;
}

/* the launch card where its springs are going (frame-budget mode: no growing over many frames) */
static void launch_snap(void)
{
    m7_spring_snap(&SP.lx, SP.lx.t);
    m7_spring_snap(&SP.ly, SP.ly.t);
    m7_spring_snap(&SP.lw, SP.lw.t);
    m7_spring_snap(&SP.lh, SP.lh.t);
    m7_spring_snap(&SP.lr, SP.lr.t);
    m7_spring_snap(&SP.la, SP.la.t);
    m7_spring_snap(&SP.back, SP.back.t);
    m7_spring_snap(&SP.run_a, 1);
    ST.flags |= 32;
}

static int launch_settled(void)
{
    return !sp_moving(&SP.lx) && !sp_moving(&SP.ly) && !sp_moving(&SP.lw) && !sp_moving(&SP.lh) &&
           !sp_moving(&SP.lr) && SP.run_a.t == 1 && !sp_moving(&SP.run_a);
}

static void frame_wait(double t0)
{
    while (now_ms() - t0 < 1000.0 / M7_FPS) {
        in_poll();
        hw_wait_us(2000);
    }
}

/* Lab (dock Lab, Hold Bixby, the Starbite Lab row): EngineerMode, then the menu again */
static void run_lab(void)
{
    ST.flags |= 4;
    /* Engineer OS when the Lab row or button asked and the entry is ready: it is launched as a
     * picked entry below (r = entry + 1); otherwise Engineer Lite (em_main) as before. */
    int eng = lab_os ? cat_find("engineer") : -1;
    lab_os = 0;
    int r = eng >= 0 && star_entry_ready((unsigned)eng, 0) ? eng + 1 : em_main(M_in, M_cmd, M_len);
    for (unsigned i = 0; i < NR; ++i)
        row_state(&R[i]);           /* Lab may have mounted the SD store */
    in_ignore_held();
    g7_present_all();
    S.full = 1;
    S.now = now_ms();
    if (r > 0 && cat_get((unsigned)(r - 1))) {
        struct rc lr = rcv(72, 1200, 1296, 180);
        int lab_row = -1;
        for (unsigned i = 0; i < NR; ++i)
            if (R[i].cat < 0)
                lab_row = (int)i;
        layout();
        if (lab_row >= 0 && rect_of(rkey(lab_row), &lr, 1))
            lr = rcv(lr.x + 16, lr.y + 6, lr.w - 32, lr.h - 12);
        S.mode = MD_MENU;
        launch(r - 1, row_by_cat(r - 1), SRC_RECT, 0, &lr);
    }
}

static void end_action(int kind)
{
    switch (kind) {
    case EN_OFF:
        hw_power_off();
        /* still on: a charger keeps the PMIC up */
        S.end_kind = EN_OFF_FAILED;
        S.end_t = S.now - 1000;
        anim_until = S.now + 600;
        tmr_add(1500, T_END_ACT, EN_RESTART, 0);
        break;
    case EN_RESTART:
        hw_reboot(INFORM_NORMAL);
        break;
    case EN_RECOVERY:
        hw_reboot(INFORM_RECOVERY);
        break;
    case EN_DOWNLOAD:
        S.result = RES_DOWNLOAD;
        break;
    default:
        break;
    }
}

static int run_loop(void)
{
    double last = now_ms();
    int first = 1;
    IN.render = 1;
    for (;;) {
        double t0 = now_ms();
        struct m7_ev ev;
        int had = 0;
        while (m7_input(&ev)) {
            S.now = t0;
            dispatch(&ev);
            had = 1;
        }
        float dt = (float)((t0 - last) / 1000.0);
        last = t0;
        m7_advance(dt);
        if (S.result == RES_LAB) {
            S.result = RES_NONE;
            run_lab();
            last = now_ms();
            continue;
        }
        if (S.result == RES_DOWNLOAD) {
            IN.render = 0;
            star_fb_mute(1);
            return 0;
        }
        if (S.mode == MD_LAUNCH && S.run_on && !S.run_err && !S.returned && !launch_settled() && m7_slow())
            launch_snap();
        if (S.mode == MD_LAUNCH && S.run_on && !S.run_err && !S.returned && launch_settled()) {
            S.returned = 1;
            SP.hint.t = 0;          /* "Bixby to cancel" no longer applies */
            star_menu_remember(S.run_cat);
            star_fb_mute(1);
            hw_tp_clear();
            store_last_clear();     /* the error card's evidence is this start's */
            IN.render = 0;
            return S.run_cat + 1;
        }
        if (S.end_t > anim_until - 600)
            anim_until = S.end_t + 600;
        if (S.seg_t > anim_until - 400)
            anim_until = S.seg_t + 400;
        if (S.sw_t > anim_until - 400)
            anim_until = S.sw_t + 400;
        if (first || had || S.full || S.refreeze || S.cd || springs_moving() || timers_pending() ||
            S.now < anim_until)
            frame_out();
        else
            (void)g7_idle();        /* at rest: motion frames' half-resolution glass to full */
        first = 0;
        frame_wait(t0);
    }
}

static void springs_init(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct m7_spring *L = i ? SP.lo : SP.lb;
        m7_spring_init(&L[LX_], 0, 0.38f, 0.72f, 0.05f);
        m7_spring_init(&L[LY_], 0, 0.38f, 0.72f, 0.05f);
        m7_spring_init(&L[LW_], 0, 0.38f, 0.76f, 0.05f);
        m7_spring_init(&L[LH_], 0, 0.38f, 0.76f, 0.05f);
        m7_spring_init(&L[LR_], 40, 0.38f, 0.8f, 0.05f);
        m7_spring_init(&L[LA_], 0, 0.3f, 1, 0.002f);
    }
    m7_spring_init(&SP.scroll, 0, 0.55f, 1, 0.05f);
    m7_spring_init(&SP.room, 0, 0.5f, 0.86f, 0.05f);
    m7_spring_init(&SP.back, 0, 0.4f, 1, 0.002f);
    m7_spring_init(&SP.ctx, 0, 0.42f, 0.8f, 0.002f);
    m7_spring_init(&SP.lift, 0, 0.35f, 0.7f, 0.002f);
    m7_spring_init(&SP.sub, 0, 0.38f, 0.86f, 0.002f);
    m7_spring_init(&SP.alert, 0, 0.34f, 0.8f, 0.002f);
    m7_spring_init(&SP.sheet, 0, 0.5f, 0.9f, 0.002f);
    m7_spring_init(&SP.toast, 0, 0.42f, 0.75f, 0.002f);
    m7_spring_init(&SP.pill_y, 0, 0.55f, 0.8f, 0.05f);
    m7_spring_init(&SP.pill_s, 1, 0.4f, 0.8f, 0.002f);
    m7_spring_init(&SP.pill_a, 0, 0.3f, 1, 0.002f);
    m7_spring_init(&SP.lx, 0, 0.52f, 0.88f, 0.05f);
    m7_spring_init(&SP.ly, 0, 0.52f, 0.88f, 0.05f);
    m7_spring_init(&SP.lw, PX, 0.52f, 0.88f, 0.05f);
    m7_spring_init(&SP.lh, PY, 0.52f, 0.88f, 0.05f);
    m7_spring_init(&SP.lr, 118, 0.52f, 0.88f, 0.05f);
    m7_spring_init(&SP.la, 0, 0.25f, 1, 0.002f);
    m7_spring_init(&SP.run_a, 0, 0.3f, 1, 0.002f);
    m7_spring_init(&SP.err_a, 0, 0.3f, 1, 0.002f);
    m7_spring_init(&SP.shake, 0, 0.28f, 0.32f, 0.05f);
    m7_spring_init(&SP.err_tint, 0, 0.4f, 1, 0.002f);
    m7_spring_init(&SP.hint, 1, 0.3f, 1, 0.002f);
    for (unsigned i = 0; i < M7_MAX_SECTIONS; ++i)
        m7_spring_init(&SP.open[i], 0, 0.5f, 0.86f, 0.002f);
}

/* the catalog, readiness and settings; no drawing */
static void core_init(void)
{
    springs_init();
    t_base = m7_now_us();
    S.now = 0;
    S.open = -1;
    S.def = S.last = -1;
    S.run_cat = -1;
    S.menu_sc = 1;
    S.mode = MD_MENU;
    star_menu_begin();
    rows_build();
    int valid = 0;
    struct star_settings *st = star_menu_settings(&valid);
    S.set_valid = valid;
    S.def = row_by_key(st->default_key);
    if (S.def >= 0 && R[S.def].cat < 0)
        S.def = -1;
    S.last = row_by_key(st->last_key);
    S.autos = valid ? (int)st->timeout_s : 0;
    if (S.autos < 0 || S.autos > 60)
        S.autos = 0;
    bat_poll();                     /* the first reading, before the intro reveals the status bar */
}

/* ================================================================================================
 * the loader's entry points (menu.h)
 * ============================================================================================== */

static int ready_state = -1;

int menu7_ready(void)
{
    if (ready_state >= 0)
        return ready_state;
    uint64_t s, e;
    ready_state = star_font_blob(&s, &e) && sb7_open((uintptr_t)(s & ~(uint64_t)(SB7_ALIGN - 1)), (uintptr_t)e);
    return ready_state;
}

/* boot-args key=N (the first digit run), or dflt */
static int cmd_num(const char *c, const char *key, int dflt)
{
    unsigned n = slen(key);
    for (unsigned i = 0; c && c[i]; ++i) {
        if (i && c[i - 1] != ' ')
            continue;
        unsigned k = 0;
        while (k < n && c[i + k] == key[k])
            ++k;
        if (k < n || c[i + n] != '=' || c[i + n + 1] < '0' || c[i + n + 1] > '9')
            continue;
        int v = 0;
        for (unsigned j = i + n + 1; c[j] >= '0' && c[j] <= '9' && v < 1000; ++j)
            v = v * 10 + (c[j] - '0');
        return v;
    }
    return dflt;
}

static int M_off;                   /* v7 declined to run (speed check, starmenu7=0): v6 runs */

int menu7_main(const struct star_info *in, char *cmdline, unsigned len)
{
    const char *fname, *fwhy;
    M_in = in;
    M_cmd = cmdline;
    M_len = len;
    if (M_off)
        return -1;
    if (!M_inited) {
        int mode = cmd_num(cmdline, "starmenu7", 1);
        if (!mode || !menu7_ready() || !g7_init()) {
            M_off = 1;
            m7_report(!mode ? "off (starmenu7=0)" : !menu7_ready() ? "no assets (SB7ASSET/fonts)" : "g7_init failed");
            return -1;              /* nothing drawn: menu v6 runs */
        }
        g7_bake();
        /* Speed check (nothing drawn yet: S-Boot's splash is still on the panel, so v6 can take
         * over with its own intro). The bake is per-pixel float work like a frame's glass. */
        struct g7_stats gb;
        g7_get_stats(&gb);
        if (mode < 2 && gb.bake_us > M7_BAKE_MAX_MS * 1000U) {
            ST.flags |= 16;
            stats_log();
            M_off = 1;
            m7_report("too slow (bake over 400 ms)");
            return -1;
        }
        core_init();
        M_inited = 1;
        ST.flags |= 1;
        g7_set_poll(in_render_poll);
        hw_tp_start();
        in_ignore_held();
        int next = star_menu_next();
        if (next >= 0) {
            /* D15: a one-shot start goes straight to the launch card, over the wallpaper */
            ST.flags |= 2;
            enter_menu();
            S.no_menu = 1;
            S.full = 1;                 /* the first frame replaces S-Boot's splash */
            struct rc full = rcv(0, 0, PX, PY);
            launch(next, row_by_cat(next), SRC_RECT, 0, &full);
            S.from[4] = 118;
            m7_spring_snap(&SP.lr, 118);
            m7_spring_snap(&SP.back, 1);
            int row = row_by_cat(next);
            struct rc rr;
            layout();
            if (row >= 0 && rect_of(rkey(row), &rr, 1)) {
                S.run_src = SRC_ROW;
                S.run_src_key = rkey(row);
            }
            return run_loop();
        }
        struct g7_stats g0, g1;
        g7_get_stats(&g0);
        IN.render = 1;
        int r = intro7_run();
        g7_get_stats(&g1);
        ST.intro_frames = g1.frames - g0.frames;
        ST.intro_worst = g1.worst_us / 1000U;
        S.now = now_ms();
        if (r == I7_LAB)
            S.result = RES_LAB;
        in_ignore_held();
        return run_loop();
    }
    if (star_menu_failure(&fname, &fwhy)) {
        star_fb_mute(0);
        ST.flags |= 8;
        in_ignore_held();
        S.now = now_ms();
        if (S.no_menu) {
            S.no_menu = 0;
            S.refreeze = 1;
        }
        if (S.run_on && S.run_cat >= 0) {
            S.mode = MD_LAUNCH;
            S.returned = 0;
            launch_error(fwhy);
        } else {
            to_menu();
        }
        return run_loop();
    }
    star_fb_mute(0);
    S.now = now_ms();
    if (S.mode != MD_MENU)
        to_menu();
    return run_loop();
}

/* synchronous frames while the loader works (no input: the pick is made) */
static void sync_frames(double until)
{
    double last = now_ms();
    if (m7_slow()) {
        /* frame-budget mode: one frame, the bar at its target (only the bar and texts change) */
        m7_advance(0);
        S.bar = S.bar_to;
        frame_out();
        return;
    }
    for (;;) {
        double t0 = now_ms();
        float dt = (float)((t0 - last) / 1000.0);
        last = t0;
        m7_advance(dt);
        if (S.bar_t + 120 > S.now) {
            float p = c01((float)((S.now - S.bar_t) / 120));
            S.bar = lerpf(S.bar_from, S.bar_to, m7_ease(p));
        } else {
            S.bar = S.bar_to;
        }
        frame_out();
        if (t0 >= until)
            break;
        while (now_ms() - t0 < 1000.0 / M7_FPS)
            hw_wait_us(1000);
    }
}

void menu7_progress(unsigned stage, unsigned n, const char *title, const char *detail)
{
    if (!M_inited || !S.run_on)
        return;
    scopy(S.run_stage, sizeof(S.run_stage), title);
    scopy(S.run_det, sizeof(S.run_det), detail);
    S.now = now_ms();
    S.bar_from = S.bar;
    S.bar_to = n ? c01((float)(stage + 1) / (float)n) : 1;
    S.bar_t = S.now;
    phase_sync = M7_PH_PROGRESS;
    sync_frames(S.now + 120);
    phase_sync = -1;
}

void menu7_handoff(const char *detail)
{
    if (!M_inited || !S.run_on)
        return;
    if (m7_slow())
        return;                     /* frame-budget mode: the card at 100 % is the last frame */
    S.now = now_ms();
    S.bar = S.bar_to = 1;
    scopy(S.end_det, sizeof(S.end_det), detail);
    end_screen(EN_HANDOFF);
    phase_sync = M7_PH_END;
    sync_frames(S.now + 520);
    phase_sync = -1;    m7_report("handoff");
}

/* ================================================================================================
 * preview harness (tools/menu7/preview): the concept's go(st), then snapAll()
 * ============================================================================================== */

#ifdef M7_PREVIEW
static int prow(const char *a, const char *b)
{
    int r = row_by_key(a);
    if (r < 0 && b)
        r = row_by_key(b);
    if (r < 0) {
        int s = sec_by_cat(CAT_LINUX);
        for (unsigned j = 0; s >= 0 && j < SEC[s].n && r < 0; ++j)
            if (R[SEC[s].r0 + j].st == ST_READY)
                r = SEC[s].r0 + (int)j;
    }
    return r;
}

static void flush_timers(double horizon)
{
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < NTMR; ++i)
            if (TM[i].kind != T_NONE_ && TM[i].due <= horizon && (best < 0 || TM[i].due < TM[best].due))
                best = (int)i;
        if (best < 0)
            return;
        struct tmr t = TM[best];
        TM[best].kind = T_NONE_;
        if (t.due > S.now)
            S.now = t.due;
        tmr_fire(&t);
    }
}

static void preview_stage(float p)
{
    unsigned n = 5, si = (unsigned)(p * 5) > 4 ? 4 : (unsigned)(p * 5);
    S.bar = ((float)si + m7_ease(c01(p * (float)n - (float)si))) / (float)n;
    switch (si) {
    case 0: scopy(S.run_stage, 64, "Reading boot image"); scopy(S.run_det, 64, "Image.gz · 31.4 MB"); break;
    case 1: scopy(S.run_stage, 64, "Verifying kernel CRC32"); scopy(S.run_det, 64, "0x9A3F1C07"); break;
    case 2: scopy(S.run_stage, 64, "Loading device tree"); scopy(S.run_det, 64, "exynos9810-starlte"); break;
    case 3: scopy(S.run_stage, 64, "Loading initramfs"); scopy(S.run_det, 64, "12.8 MB"); break;
    default: scopy(S.run_stage, 64, "Handing off to kernel"); scopy(S.run_det, 64, "EL1 · 0x80080000"); break;
    }
}

int m7_preview_state(const char *st, float p)
{
    if (!M_inited) {
        core_init();
        M_inited = 1;
    }
    preview_lab = 0;
    S.now = now_ms();
    double t0 = S.now;
    int lin = sec_by_cat(CAT_LINUX), phosh = prow("phosh", "linux-ubuntu");
    if (S.mode == MD_INTRO)
        m7_finish_intro(1);
    if (seq(st, "intro")) {
        m7_prepare_boot();
        return 1;
    }
    enter_menu();
    if (seq(st, "menu")) {
    } else if (seq(st, "countdown")) {
        m7_spring_snap(&SP.pill_y, 0);
        start_countdown();
        m7_spring_snap(&SP.pill_a, 1);
    } else if (seq(st, "collapsed")) {
        S.open = -1;
        for (unsigned c = 0; c < NS; ++c)
            SP.open[c].t = 0;
        if (lin >= 0)
            set_focus(FK(F_P, lin, 0), 0);
        SP.scroll.t = 0;
    } else if (seq(st, "apple")) {
        int a = sec_by_cat(CAT_APPLE), r = prow("ios26", 0);
        if (a >= 0)
            open_prov(a, r >= 0 && R[r].sec == a ? rkey(r) : FK(F_P, a, 0));
    } else if (seq(st, "ctx") || seq(st, "submenu") || seq(st, "sheet") || seq(st, "launch") ||
               seq(st, "error")) {
        if (phosh < 0)
            return 0;
        open_prov(R[phosh].sec, rkey(phosh));
        ensure_visible_now();
        if (seq(st, "ctx") || seq(st, "submenu")) {
            open_ctx(phosh);
            if (seq(st, "submenu"))
                S.ofocus = FK(F_M, 0, auto_item(S.autos));
        } else if (seq(st, "sheet")) {
            open_sheet(phosh);
        } else {
            launch(R[phosh].cat, phosh, SRC_ROW, rkey(phosh), 0);
            if (seq(st, "error"))
                launch_error("KERNEL CRC MISMATCH");
        }
    } else if (seq(st, "alert") || seq(st, "recovery")) {
        int al = seq(st, "alert");
        set_focus(FK(F_T, 0, al ? T_POWER : T_RECOVERY), 0);
        open_alert(al ? AL_POWER : AL_RECOVERY);
    } else if (seq(st, "handoff")) {
        if (phosh < 0)
            return 0;
        S.run_on = 1;
        S.run_cat = R[phosh].cat;
        S.run_row = phosh;
        scopy(S.end_det, sizeof(S.end_det), "EL1 · 0x80080000");
        end_screen(EN_HANDOFF);
        S.end_t = S.now - 1000;
    } else if (seq(st, "missing")) {
        int r = prow("arch", "linux-arch");
        if (r < 0)
            return 0;
        open_prov(R[r].sec, rkey(r));
        start_entry(r, rkey(r));
    } else if (seq(st, "lab")) {
        preview_lab = 1;
        go_lab();
        preview_lab = 0;
    } else {
        return 0;
    }
    flush_timers(t0 + 400);
    for (unsigned i = 0; i < 4; ++i) {
        update();
        snap_all();
    }
    update();
    if (S.run_on && p > 0)
        preview_stage(p);
    return 1;
}
#endif
