/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 (menu7.c, menu7_*.c): "Choose a System", the approved concept
 * docs/concepts/ios27-boot.html ported exactly. docs/MENU7-PORT.md section 4.2.
 *
 * menu7.c is the concept's <script> in C, function for function (the doc's table 4.2.1): the
 * state machine (mode splash/intro/menu/launch/end, overlays ctx/alert/sheet, focus keys), the
 * springs, the input (keys + touch), the catalog mapped onto provider sections, the settings,
 * boot-next and fail-back through menu.c's star_menu_* API, and the layout (the concept's CSS,
 * checked against docs/concepts/boot-geometry/<state>.json). It never touches a pixel: each frame
 * it fills a struct g7_frame (glass.h) and calls g7_frame().
 *
 * Compiled with FP/SIMD (single-precision floats are fine everywhere here).
 * Frozen interface: builders implement against it; a change goes through the integrator.
 */
#pragma once
#include <stdint.h>
#include "glass.h"

struct star_info;

/* ---- entry points for the loader (also declared in menu.h, without floats) ------------------ */

/* 1 when the BOOT ramdisk holds a valid SB7 container (sb7_open). Cheap; no drawing. */
int menu7_ready(void);
void m7_report(const char *why);   /* pmsg diagnostics: why v7 stopped, or the pick and its timings */
/* The menu, as menu_v6: returns the catalog entry to start + 1, or 0 for Download mode
 * (Recovery, Restart and Power Off happen inside). The first call runs the intro (intro7_run);
 * a call after star_menu_failed resumes on the error card ("Couldn't Start ..."). Returns -1 if
 * v7 cannot run (g7_init failed): nothing was drawn, the caller falls back to v6. */
int menu7_main(const struct star_info *in, char *cmdline, unsigned len);
/* The loader is loading the chosen entry: stage (0-based) of n, what and a detail ("Image.gz
 * · 31.4 MB"). Draws the launch card's bar, stage and detail (a short eased step, <= 120 ms). */
void menu7_progress(unsigned stage, unsigned n, const char *title, const char *detail);
/* The last frame before the jump: the concept's "handoff" end screen (detail: "EL1 · 0x..."). */
void menu7_handoff(const char *detail);

/* ---- palette (spec "Palette"), 0xAARRGGBB straight alpha ---------------------------------- */

#define M7_L1        0xffffffffU    /* label */
#define M7_L2        0x9eebebf5U    /* rgba(235,235,245,.62) secondary */
#define M7_L3        0x57ebebf5U    /* rgba(235,235,245,.34) tertiary */
#define M7_SEP       0x1cffffffU    /* rgba(255,255,255,.11) separator */
#define M7_STAR      0xff6fb3ffU    /* the starbite star, focus accent, default badge */
#define M7_BLUE      0xff0a84ffU
#define M7_GREEN     0xff30d158U
#define M7_ORANGE    0xffff9f0aU
#define M7_RED       0xffff453aU
#define M7_YELLOW    0xffffd60aU
#define M7_SHEEN_END 0xffd9e2f0U    /* provider names: #fff 0..48 %, then to this at 100 % */
#define M7_DIM_ROW   0x75ffffffU    /* rgba(255,255,255,.46): names of systems that cannot start */
#define M7_TAG_BG    0x1fffffffU    /* .tag           rgba(255,255,255,.12) / text rgba(255,255,255,.88) */
#define M7_TAG_FG    0xe0ffffffU
#define M7_TAG_EXP_BG 0x29ff9f0aU   /* .tag.exp       rgba(255,159,10,.16) / #ffb340 */
#define M7_TAG_EXP_FG 0xffffb340U
#define M7_TAG_OK_BG 0x2630d158U    /* .tag.stable    rgba(48,209,88,.15) / #5ee07f */
#define M7_TAG_OK_FG 0xff5ee07fU
#define M7_TAG_DEF_BG 0x336fb3ffU   /* .tag.def       rgba(111,179,255,.2) / #a9d2ff */
#define M7_TAG_DEF_FG 0xffa9d2ffU
#define M7_SPLASH_BY 0xff8d8d92U    /* "Powered by" */
#define M7_END_SUB   0xff6c6c72U    /* end screen subtitle */

/* Glass tints {r, g, b, a} (spec "Glass tint", the concept's data-tint attributes) */
#define M7_TINT_PANE    0.02f, 0.035f, 0.08f, 0.22f     /* default: sections, dock, preview */
#define M7_TINT_ISLAND  0.01f, 0.015f, 0.03f, 0.7f
#define M7_TINT_MENU    0.13f, 0.14f, 0.18f, 0.56f      /* options menu (frost) */
#define M7_TINT_SHEET   0.09f, 0.10f, 0.13f, 0.62f      /* details sheet (frost) */
#define M7_TINT_ALERT   0.12f, 0.13f, 0.17f, 0.6f       /* alert (frost) */
#define M7_TINT_NOTICE  0.08f, 0.09f, 0.12f, 0.55f      /* toast (frost) */
#define M7_TINT_BLUE    0.04f, 0.48f, 1.0f, 0.72f       /* sheet Start, Try Again, blue alert button */
#define M7_TINT_RED     1.0f, 0.22f, 0.18f, 0.78f       /* Power Off */
#define M7_TINT_GRAY    0.42f, 0.44f, 0.5f, 0.3f        /* other alert buttons */
#define M7_TINT_BACK    0.55f, 0.57f, 0.62f, 0.45f      /* error Back */
#define M7_TINT_OFF     0.5f, 0.5f, 0.55f, 0.12f        /* sheet Start when the system cannot start */
#define M7_TINT_LENS    0.75f, 0.85f, 1.0f, 0.06f
#define M7_TINT_APPLE   0.62f, 0.66f, 0.74f, 0.5f       /* provider tiles */
#define M7_TINT_MS      0.0f, 0.47f, 0.95f, 0.8f
#define M7_TINT_GOOGLE  0.2f, 0.75f, 0.4f, 0.78f
#define M7_TINT_LINUX   1.0f, 0.6f, 0.1f, 0.82f
#define M7_TINT_SB      0.43f, 0.7f, 1.0f, 0.78f
#define M7_TINT_LAB     0.68f, 0.38f, 0.95f, 0.8f
#define M7_TINT_OTHER   0.42f, 0.44f, 0.5f, 0.6f        /* not in the concept */
/* launch card: lerp(.02, .14), lerp(.035, .03), lerp(.08, .05), lerp(.34, .44) by errTint */

/* ---- layout (spec "Layout on 1440 x 2960" + the CSS; docs/concepts/boot-geometry is normative) --- */

#define M7_EDGE_SAFE 72             /* no text, rim or highlight at x < 72 or x > 1368 */
#define M7_STATUS_X0 92             /* "Galaxy S9" pen x, baseline 129, SB7_F_IT600_46, -.01 em */
#define M7_STATUS_X1 1348           /* right end: battery icon 84 x 40 at (1264, 92), "87%" ends at 1250 */
#define M7_STATUS_BASE 129
#define M7_ISLAND_X 250             /* island x 250..1190, y 176..312, r 68 */
#define M7_ISLAND_Y 176
#define M7_ISLAND_W 940
#define M7_ISLAND_H 136
#define M7_ISLAND_R 68
#define M7_ROOM 176                 /* the list moves down this much while the island shows */
#define M7_VP_Y0 170                /* scroll viewport y 170..2620 */
#define M7_VP_Y1 2620
#define M7_VP_FADE_TOP 76           /* mask: transparent at 170, opaque from 246 ... */
#define M7_VP_FADE_BOT 140          /* ... to 2480, transparent at 2620 */
#define M7_CONTENT_PAD_TOP 64       /* #content padding 64 72 260 (scroll y 0: brand at 234) */
#define M7_CONTENT_PAD_BOT 260
#define M7_X0 72                    /* margins: content 72..1368 (1296) */
#define M7_CW 1296
#define M7_BRAND_Y 234              /* wordmark box (56 high), pen x 82, baseline 282 */
#define M7_TITLE_Y 316              /* title box (142), pen x 76, baseline 439 */
#define M7_SUB_Y 486                /* subtitle box (59), star 40 at x 82, text x 136, baseline 533 */
#define M7_CARDS_Y 629              /* first section top at scroll 0 */
#define M7_CARD_HEAD 296            /* section header height */
#define M7_CARD_R 96
#define M7_CARD_GAP 32
#define M7_PTILE 172                /* provider tile at card x + 50, y + 62, r 46, strength .9 */
#define M7_PTILE_X 50
#define M7_PTILE_R 46
#define M7_PNAME_X 268              /* name / subtitle / row text at card x + 268 */
#define M7_PNAME_BASE 158.5f        /* provider name baseline from the card top (128 / 700 / -.035 em) */
#define M7_PSUB_BASE 237.5f         /* provider subtitle baseline (44): dot 20 at x 268, text at 304 */
#define M7_CHEV 60                  /* chevron box 60 at x 1172 of the card (right padding 64), stroke 2.6 */
#define M7_ROW_H 188                /* system row: OS tile 124 centred in the 268 column */
#define M7_ROWS_PAD 24              /* .rowsIn bottom padding: open height = n * 188 + 24 */
#define M7_ROW_NAME_BASE 85.8f      /* from the row top: name 66 / 600 / -.02 em */
#define M7_ROW_SUB_BASE 150.1f      /* detail 42: dot 18 at x 268, text at x 300 */
#define M7_ROW_RIGHT 52             /* tags right edge = card right - 52; hairline 268 .. right - 52 */
#define M7_TAG_H 62                 /* tags: 62 high, r 31, padding 26, 33 / 600 / +.005 em, gap 12 */
#define M7_DOCK_Y 2652              /* capsule x 72 w 1044 h 212 r 106; Power circle x 1156, 212 */
#define M7_DOCK_W 1044
#define M7_DOCK_H 212
#define M7_DOCK_POWER_X 1156
#define M7_MENU_W 940               /* options menu, centred, 28 px from the lifted preview */
#define M7_MENU_GAP 28
#define M7_MENU_ITEM 148
#define M7_MENU_R 80
#define M7_ALERT_X 150              /* alert 1140 wide, r 118, buttons 160 high r 80, gap 26 */
#define M7_ALERT_W 1140
#define M7_SHEET_X 32               /* sheet x 32..1408, y 1080..2928, r 104 */
#define M7_SHEET_Y 1080
#define M7_SHEET_W 1376
#define M7_SHEET_H 1848
#define M7_SHEET_R 104
#define M7_LN_ICON_Y 980            /* launch: tile 380 at (530, 980), name baseline box y 1470, */
#define M7_LN_NAME_Y 1470           /* bar 640 x 16 at (400, 1790), stage y 1866, detail y 1930, */
#define M7_LN_BAR_Y 1790            /* hint y 2700; error buttons 980 x 176 at x 230, y 2330, 2540 */
#define M7_ER_RETRY_Y 2330
#define M7_ER_BACK_Y 2540

/* ---- motion (spec "Motion": response s / damping ratio) ---------------------------------- */

#define M7_SPRING_H 0.004f          /* semi-implicit Euler sub-step */
#define M7_DT_MAX 0.05f             /* the concept's frame() clamps dt to this: one m7_step */
#define M7_FPS 30                   /* frame pacing target */
/* A frame later than M7_DT_MAX (the A55 can take seconds for a full-panel glass frame) advances
 * the springs by its real dt in M7_DT_MAX steps (m7_advance), up to this much: animations end on
 * time however few frames are drawn. */
#define M7_DT_CATCHUP 0.5f
/* Frame-budget mode: a full-panel frame takes longer than this (g7_stats.full_us; VM ~100-180
 * ms after the motion-frame work, x12 ~0.3-0.6 s). The launch card's springs snap and the pick is returned at once, a progress
 * stage draws one frame, the handoff screen is skipped. */
#define M7_SLOW_MS 300
/* Speed check before anything is drawn: g7_bake slower than this (VM: ~19 ms, x12 ~230 ms) and v7 does not
 * run (menu v6 does; log slot 127 bit 16). Boot-args starmenu7=0 turns v7 off, starmenu7=2 skips
 * the check. */
#define M7_BAKE_MAX_MS 400
/* lens .38/.72 (x, y), .38/.76 (w, h), .38/.8 (r), .3/1 (a); eps .05; bounce +-900 px/s */
/* section open .50/.86 eps .002; scroll .55/1 eps .05; room .50/.86; press down .18/1 to .965,
 * release .30/.55; options .42/.80, lift .35/.70, backdrop (back) .40/1; alert .34/.80;
 * sheet .50/.90; toast .42/.75; island y .55/.80, scale .40/.80, alpha .30/1; launch x y w h r
 * .52/.88, LA .25/1; content .30/1; shake .28/.32 kick -3200 (rows: -2600); errTint .40/1 */

/* ---- input (spec "Inputs" / "Key mapping") ------------------------------------------------ */

#define M7_HOLD_POWER_MS 520
#define M7_HOLD_BIXBY_MS 700
#define M7_LONG_PRESS_MS 480
#define M7_DRAG_PX 32               /* a touch that moves this far in the list scrolls */
#define M7_REPEAT_MS 450            /* Volume held: repeat after 450 ms, every 110 ms */
#define M7_REPEAT_EVERY_MS 110
#define M7_DEBOUNCE_MS 20
#define M7_VOL_HOLD_MS 700          /* both Volume keys held: power off now (v5 / v6 K_VOL_HOLD) */

enum {
    M7_EV_NONE,
    M7_EV_UP,                       /* Volume Up pressed (and repeats) */
    M7_EV_DOWN,
    M7_EV_POWER_DOWN,               /* Power went down (press visual starts) */
    M7_EV_POWER,                    /* Power released before the hold */
    M7_EV_POWER_HOLD,               /* Power held M7_HOLD_POWER_MS (no M7_EV_POWER follows) */
    M7_EV_BIXBY,                    /* Bixby released before the hold */
    M7_EV_BIXBY_HOLD,               /* Bixby held M7_HOLD_BIXBY_MS: Lab */
    M7_EV_TOUCH_DOWN,               /* x, y in panel px (finger 0) */
    M7_EV_TOUCH_MOVE,
    M7_EV_TOUCH_UP,
};
struct m7_ev {
    uint8_t type;
    int16_t x, y;
    uint64_t t_us;                  /* when it happened (m7_now_us) */
};
/* Poll keys and touch once and return the next event (0: none). Debounce, holds, repeat. */
int m7_input(struct m7_ev *ev);
uint64_t m7_now_us(void);
/* The payload appended to the BOOT Image (menu7_main's star_info rd_start .. rd_end, from
 * xnu_boot.c's boot_rd_*): 1 and the range, or 0 before menu7_main / in the preview. */
int m7_rd_range(uint64_t *start, uint64_t *end);

/* Frame timing by what the frame shows (on the device: log slots 128 + class, each
 * frames << 48 | worst ms << 32 | total ms; h7 --run prints the class's name). */
enum { M7_PH_INTRO_WALL,            /* intro, the wallpaper still revealing (t < 1600 ms) */
       M7_PH_INTRO_GLASS,           /* intro, the menu's glass arriving */
       M7_PH_IDLE,                  /* menu: nothing but items (countdown ring, a press, a notice) */
       M7_PH_LENS,                  /* menu: the focus lens moving */
       M7_PH_LIST,                  /* menu: the list moving (scroll, the island's room, the island) */
       M7_PH_EXPAND,                /* a section opening or closing */
       M7_PH_OVER_MOVE,             /* an overlay opening or closing (backdrop, options, sheet, alert) */
       M7_PH_OVER_REST,             /* an overlay at rest (its lens, switch, segments) */
       M7_PH_LAUNCH,                /* the launch card growing or the error card */
       M7_PH_PROGRESS,              /* menu7_progress frames */
       M7_PH_END,                   /* end screens, handoff */
       M7_PH_N };
/* g7_frame, timed into the class's statistics (intro7_run uses it too) */
uint32_t m7_frame_timed(const struct g7_frame *f, int phase);
extern int m7_phase_last;           /* the class of the last m7_frame_timed */

/* ---- springs (spec "Springs": m = 1, k = (2 pi / response)^2, c = 4 pi zeta / response) ---- */

struct m7_spring {
    float x, t, v;
    float k, c, eps;
};
void m7_spring_init(struct m7_spring *s, float x, float response, float damping, float eps);
void m7_spring_cfg(struct m7_spring *s, float response, float damping);
void m7_spring_to(struct m7_spring *s, float t);
void m7_spring_snap(struct m7_spring *s, float t);          /* x = t = target, v = 0 */
void m7_spring_kick(struct m7_spring *s, float v);
/* n = ceil(dt / 4 ms) sub-steps: v += a h, x += v h; settles when |x - t| < eps, |v| < 8 eps */
void m7_spring_step(struct m7_spring *s, float dt);
/* The concept's spA(t, r, z): closed-form step response of a spring released at t = 0 (s). */
float m7_spa(float t, float response, float damping);
float m7_ease(float t);             /* cubic in-out */
float m7_eout(float t);             /* cubic out */
float m7_clamp(float x, float a, float b);

/* ---- what intro7.c needs from the menu (the concept's prepMenuForBoot / introAt / finishIntro) ---- */

#define M7_MAX_SECTIONS 8

/* The intro's transforms over the menu's own state; 1 / 0 = as the menu has it. */
struct m7_xf {
    float menu_a;                   /* #menu opacity (content and base glass) */
    float status_a;                 /* #status opacity */
    float brand_dy, brand_a;        /* #brand and #title: translateY, opacity */
    float sub_dy, sub_a;            /* #subtitle */
    float foot_a;                   /* #foot */
    float card_dy[M7_MAX_SECTIONS]; /* section i: translateY(card_dy) scale(card_s) about its */
    float card_s[M7_MAX_SECTIONS];  /*   top centre (transform-origin 50% 0), opacity and glass */
    float card_a[M7_MAX_SECTIONS];  /*   alpha (card and its tile) card_a */
    float dock_dy, dock_a;          /* #toolbar translateY; tbCap / tbPow glass alpha */
    float lens_a;                   /* LB.a (snapped) */
    float pill_dy, pill_a;          /* #pill translateY (pillY), glass alpha (_ga) */
    float pill_in_a;                /* #pillIn opacity */
};
/* prepMenuForBoot: open the default's section, focus its row, scroll so it shows, island
 * content (countdown or "Ready"). Done before the intro's first frame. */
void m7_prepare_boot(void);
/* The menu's layers for its current state into f (base and over reset first; f->wall, word,
 * dim, back filled with the menu's values). xf: the intro's transforms, 0 = none. */
void m7_build(struct g7_frame *f, const struct m7_xf *xf);
/* Advance every menu spring by dt seconds (the concept steps all springs every frame). */
void m7_step(float dt);
/* m7_step for a frame's real dt: clamped to M7_DT_CATCHUP, in steps of at most M7_DT_MAX. */
void m7_advance(float dt);
/* finishIntro(skipped): lens on, island spring home (snapped if skipped), countdown starts. */
void m7_finish_intro(int skipped);
/* The splash star's centre and radius (#spStar), where the intro's star flight starts. */
#define M7_STAR0_X 841.85f
#define M7_STAR0_Y 2556.15f
#define M7_STAR0_R 13.85f

/* ---- preview harness hooks (tools/menu7/preview, built with -DM7_PREVIEW) ------------------ */
#ifdef M7_PREVIEW
/* Put the menu in a concept state exactly as the concept's go(st) does, then snapAll():
 * menu countdown collapsed apple ctx submenu sheet alert recovery launch error handoff
 * missing lab. p: launch progress 0..1 (the concept's &p=). 0 if unknown. */
int m7_preview_state(const char *state, float p);
#endif
