/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7: the Liquid Glass renderer (glass.c, glass_*.c). docs/MENU7-PORT.md section 4.1.
 *
 * Every pixel of menu v7 and its intro is drawn here; menu7.c and intro7.c only describe frames.
 * A frame is two layers, as in the concept (docs/concepts/ios27-boot.html):
 *
 *   base   the wallpaper and its glass (cvBase: GLASS_FS with uOpaque = 1), then the base
 *          layer's content items (the #menu DOM: text, tiles, icons, tags, fills)
 *   over   the overlay glass (cvOver: GLASS_FS with uOpaque = 0, premultiplied over the base),
 *          then the overlay's content items (#over, #splash and #end, in that order)
 *
 * then the curved-edge shade (.edge) over everything. Glass refracts the wallpaper planes only,
 * never content. The shading is the concept's GLASS_FS / WALL_FS / BLUR_FS, ported exactly (the
 * spec panel's "Glass shader" table restates it), with K = 1 (the canvas is the panel).
 *
 * glass.c keeps the previous frame's lists and draws only what changed: every shape and item
 * carries a stable key, and a change of an item's key, geometry or content dirties its old and new
 * bounds (a shape's with its 110 x 150 px shadow margin, as GLASS_FS's own cull); a change of a
 * frame-wide value (dim, back, light, wall, word) dirties the panel. A frame that changes nothing
 * costs nothing.
 *
 * Resolution (MV4 route 2): everything below the content items (wallpaper, glass, the frozen base,
 * the over glass) is composed in G7_CANVAS on the half-resolution grid, 720 x 1480 (canvas pixel
 * (i, j) is the panel point (2i + 1, 2j + 1)). The present scales the dirty rects 2x to the
 * framebuffer (bilinear) and draws the content items over them at panel resolution, so text, icons
 * and tiles keep their full sharpness; a change of an item alone is only presented again. Behind an
 * overlay (frozen) the base layer's items are part of the frozen canvas. All coordinates in this
 * interface stay panel px.
 *
 * Compiled with FP/SIMD (not -mgeneral-regs-only): see build.sh and section 2 of the doc.
 * Units: panel px (1440 x 2960), y down; colours are sRGB-encoded values blended as such (the
 * browser's gamma-space blending), 0xAARRGGBB with straight alpha unless noted.
 * Frozen interface: builders implement against it; a change goes through the integrator.
 */
#pragma once
#include <stdint.h>

#define G7_W 1440
#define G7_H 2960

/* ---- memory (docs/MENU7-PORT.md section 3): write-back RAM, mmu.c, free while the menu runs ---- */
#ifndef G7_HOSTOFF
#define G7_HOSTOFF 0UL              /* the preview harness may move everything up (macOS) */
#endif
#define G7_CANVAS     (0x84000000UL + G7_HOSTOFF)  /* 17 MiB: the canvas (720 x 1480 ARGB), the present's band buffer */
#define G7_SHARP      (0x85100000UL + G7_HOSTOFF)  /* 17 MiB: the base glass (GLB) / the frozen base layer (canvas size) */
#define G7_BASE       (0x86200000UL + G7_HOSTOFF)  /* 17 MiB: wallpaper x .8 (canvas size), the over glass plane */
#define G7_POOL       (0x87300000UL + G7_HOSTOFF)  /* 18 MiB: glass's planes, tables, row buffers */
#define G7_POOL_SIZE  0x01200000UL
#define G7_CACHE      (0x88500000UL + G7_HOSTOFF)  /* 3 MiB: decoded images and rendered tiles */
#define G7_CACHE_SIZE 0x00300000UL
#define G7_RAM_END    (0x88800000UL + G7_HOSTOFF)  /* ui.c's UI_SCRATCH (EngineerMode) starts here */
#define G7_FRAME_BYTES (4UL * G7_W * G7_H)          /* 17 049 600 */

/* ---- shapes: GLASS_FS's uR / uA / uT, one per [data-glass] element (and the focus lenses) ---- */

#define G7_MAX_SHAPES 20            /* per layer (GLASS_FS N) */

#define G7_S_CLIP  1U               /* faded by the scroll viewport: alpha *= smoothstep(170, 246, y)
                                     * * (1 - smoothstep(2480, 2620, y)), per pixel (uClip) */
#define G7_S_LENS  2U               /* focus lens: no shadow, SZ = 0, passes 82 % of what is under it */
#define G7_S_FROST 4U               /* frosted overlay pane (FR = 1) */

struct g7_shape {
    uint32_t key;                   /* stable identity for dirty tracking, unique in its layer */
    uint32_t flags;                 /* G7_S_* */
    float x, y, w, h;               /* the box after transforms (getBoundingClientRect) */
    float r;                        /* CSS border-radius x the element's scale (sdSq adds the
                                     * continuous corner: n 2.6 -> 2, radius x 1.18 -> 1) */
    float a;                        /* alpha (the element's _ga x its layer's opacity) */
    float s;                        /* refraction strength: panes 1, tiles .9, launch 1.15, lens 1.4 */
    float tint[4];                  /* r, g, b, a in 0..1 (data-tint; default .02, .035, .08, .22) */
};

/* ---- content items: everything the DOM draws above a layer's glass, in paint order ---- */

#define G7_MAX_ITEMS 448            /* per layer */
#define G7_MAX_CLIPS 32             /* per layer; clip 0 is always the whole panel */

enum {
    G7_TEXT = 1,                    /* a run of one font */
    G7_IMAGE,                       /* an SB7 image (icon, provider glyph, SAMSUNG) */
    G7_TILE,                        /* an OS tile (sb7_tile), drawn by glass at that size */
    G7_RECT,                        /* rounded rect: filled, or a ring (border) */
    G7_SHADOW,                      /* CSS box-shadow (outer) of a rounded rect */
    G7_ARC,                         /* circular arc stroke, round caps (countdown ring) */
    G7_STROKE,                      /* polyline stroke, round caps and joins (chevron, spinner) */
};

#define G7_I_FADE  1U               /* inside #vp: coverage x the viewport's edge fade (as G7_S_CLIP) */
#define G7_I_GRAD  2U               /* text: vertical gradient (provider names' sheen) */
#define G7_I_DASH  4U               /* rect ring: dashed (border-style: dashed) */
#define G7_I_ELLIP 8U               /* text: cut to max_w with "…" (text-overflow: ellipsis) */

struct g7_text {
    const char *s;                  /* UTF-8; read only during g7_frame */
    uint16_t n;                     /* bytes */
    uint16_t font;                  /* SB7_F_* */
    float x, y;                     /* pen start and baseline, after transforms */
    float track;                    /* letter-spacing in em (CSS), added after every character */
    float scale;                    /* 1 = the atlas size; glyphs scale about (x, y) */
    float max_w;                    /* G7_I_ELLIP or a clip: advance limit in px at scale 1 */
    uint32_t color2;                /* G7_I_GRAD: colour at gy1 */
    float gy0, gy1, gstop;          /* G7_I_GRAD: color from gy0 to gy0 + gstop * (gy1 - gy0),
                                     * then to color2 at gy1 (.pn: #fff 0 % / 48 %, #d9e2f0 100 %) */
};
struct g7_image {
    uint16_t id, pad;               /* SB7_I_ / SB7_P_ / SB7_S_ */
    float x, y, w, h;               /* where the image's nominal box goes (bilinear when scaled) */
    float sat;                      /* saturation (CSS saturate()), 1 = as is */
};
struct g7_tile {
    uint16_t tile;                  /* tile id (sb7_tile_for) */
    uint16_t size;                  /* SB7_TILE_*: which glyph layer and cached render to use */
    float x, y, w;                  /* box (square), w may differ from the size (press, lift) */
    float sat;                      /* .3 for systems that cannot start (CSS saturate(.3)) */
};
struct g7_rect {
    float x, y, w, h, r;
    float ring;                     /* 0 filled; > 0 a border of that width inside the box */
};
struct g7_boxshadow {
    float x, y, w, h, r;            /* the casting box */
    float dx, dy, blur, spread;     /* CSS: offset, blur radius (sigma = blur / 2), spread */
};
struct g7_arc {
    float cx, cy, r, width;
    float a0, sweep;                /* degrees; 0 = 12 o'clock, clockwise (the ring's -90 deg turn) */
};
struct g7_stroke {
    float x[4], y[4];
    uint8_t n;                      /* 2 .. 4 points */
    float width;
};

struct g7_item {
    uint32_t key;                   /* stable identity for dirty tracking, unique in its layer */
    uint8_t kind;                   /* G7_TEXT .. */
    uint8_t clip;                   /* index into the layer's clips */
    uint16_t flags;                 /* G7_I_* */
    float a;                        /* opacity (the product of the element's ancestors') */
    uint32_t color;                 /* text, A8 image, rect, shadow, arc, stroke colour */
    union {
        struct g7_text text;
        struct g7_image image;
        struct g7_tile tile;
        struct g7_rect rect;
        struct g7_boxshadow shadow;
        struct g7_arc arc;
        struct g7_stroke stroke;
    } u;
};

struct g7_clip {
    float x0, y0, x1, y1;           /* overflow: hidden boxes (x1, y1 exclusive) */
    float r;                        /* > 0: rounded (the launch card's 118 -> its spring) */
};

struct g7_layer {
    uint16_t nshape, nitem, nclip, pad;
    struct g7_shape shape[G7_MAX_SHAPES];
    struct g7_item item[G7_MAX_ITEMS];
    struct g7_clip clip[G7_MAX_CLIPS];
};

/* ---- the wallpaper (WALL_FS) and the intro's glass wordmark (GLASS_FS uText/uCap/uTxP) ---- */

struct g7_wall {                    /* WALL_FS: uStar (sx, sy, si, sr), uStarW (sw), uRev (rr, rf, amb) */
    float sx, sy, si, sr, sw;
    float rr, rf, amb;
};
/* After the intro (and whenever the wallpaper is static): the concept's W at toMenu(). */
#define G7_WALL_FINAL_SX 1290.0f
#define G7_WALL_FINAL_SY 232.0f
#define G7_WALL_FINAL_SI 1.0f
#define G7_WALL_FINAL_SR 54.0f
#define G7_WALL_FINAL_SW 0.72f
#define G7_WALL_FINAL_RR 1e5f
#define G7_WALL_FINAL_RF 800.0f
#define G7_WALL_FINAL_AMB 1.0f

struct g7_word {                    /* the SAMSUNG height field as glass, base layer, above its shapes */
    float tm;                       /* uTxP.x: letters melt into the capsule 0 -> 1 */
    float tf;                       /* uTxP.y: white -> glass 1 -> 0 */
    float ta;                       /* uTxP.z: alpha; 0 = not drawn */
    float cap[4];                   /* uCap: capsule centre x, y, w, h */
};

/* ---- a frame ---- */

#define G7_F_FULL     1U            /* redraw and present the whole panel */
#define G7_F_REFREEZE 2U            /* while frozen (back > 0): take the base layer again */

struct g7_frame {
    const struct g7_layer *base, *over;
    struct g7_wall wall;            /* != the final wall: the wallpaper and BASE are rebuilt (intro) */
    struct g7_word word;
    float dim;                      /* uDim: 1 -> .8 in the intro, .8 after */
    float back;                     /* the concept's back.x: base glass softens (uSoft), the base
                                     * layer's content blurs 26 px, darkens 25 % and zooms to .965
                                     * about (720, 1332); the overlay backdrop is 45 % (uBack) */
    float lx, ly;                   /* uLight tilt; 0, 0 on the device (fixed light) */
    uint32_t flags;                 /* G7_F_* */
};

/*
 * Behind an overlay (back > 0) the base layer is frozen: on the first frame with back > .002,
 * glass renders the base layer as it is into G7_SHARP and, once, its soft version (base glass
 * at uSoft = 1, content blurred / darkened / zoomed, half resolution) into G7_POOL; until back
 * is 0 again, frames show mix(SHARP, SOFT, back) under the over layer and ignore the base list
 * (G7_F_REFREEZE takes it again). This is the deliberate difference D4 of the doc.
 */

/* ---- API ---- */

/* Assets (sb7_open done), tables, buffers. 0 on failure: the caller falls back to menu v6. */
int g7_init(void);
/* The static wallpaper: WALL_FS at 720 x 1480 for the final wall, blur A (360 x 740) and B
 * (180 x 370) exactly as the concept's renderWall, the 5-tap frost plane, G7_BASE at dim .8.
 * Also the intro's static field. Call once before the first frame (~0.3 s). */
void g7_bake(void);
/* Draw what changed and present it. Returns the panel pixels presented (0: nothing changed). */
uint32_t g7_frame(const struct g7_frame *f);
/* Nothing to draw this frame: a motion frame's half-resolution glass, again at full resolution, a
 * band at a time (a few ms). Returns 1 while there is more of it (call again next frame). */
int g7_idle(void);
/* G7_CANVAS to the panel again (after Lab drew on it). */
void g7_present_all(void);
/* A function g7_frame and g7_bake call between rows (every few ms of drawing), so input is
 * sampled while a slow frame draws (menu7: the keys). It must not draw. 0: none. Set at run time
 * (no static pointer: no absolute relocation). */
void g7_set_poll(void (*fn)(void));

/* Text measurement, the same arithmetic g7_frame draws with (kerning, letter-spacing after every
 * character as CSS does, so the result matches the concept's tw). */
float g7_text_w(unsigned font, const char *s, unsigned n, float track);
/* How many bytes of s fit in max_w (whole UTF-8 characters); for wrapping and ellipsis. */
unsigned g7_text_fit(unsigned font, const char *s, unsigned n, float track, float max_w);
/* hhea ascent and descent (> 0) in px: CSS line boxes (line-height: normal uses + line gap). */
void g7_font_metrics(unsigned font, float *ascent, float *descent, float *gap);
/* Nominal box of an image (sb7_image nw, nh); 0 if it does not exist. */
int g7_image_size(unsigned id, unsigned *w, unsigned *h);

struct g7_stats {
    uint32_t frames;                /* presented frames */
    uint32_t last_us, worst_us;     /* g7_frame time */
    uint32_t last_px;               /* pixels presented by the last frame */
    uint32_t freeze_us;             /* the last freeze (SHARP + SOFT) */
    uint32_t bake_us;               /* g7_bake */
    uint32_t full_us;               /* a full-panel frame's time, scaled from the last frame that
                                     * drew at least 1/8 of the panel (0: none yet) */
    uint32_t bake_part[4];          /* g7_bake: field, wallpaper, blur planes, BG8 (us) */
};
void g7_get_stats(struct g7_stats *s);

/* ---- list helpers (no libc: plain loops) ---- */

static inline void g7_layer_reset(struct g7_layer *l)
{
    l->nshape = 0;
    l->nitem = 0;
    l->nclip = 1;
    l->clip[0].x0 = 0;
    l->clip[0].y0 = 0;
    l->clip[0].x1 = G7_W;
    l->clip[0].y1 = G7_H;
    l->clip[0].r = 0;
}
/* The next free shape / item / clip (zeroed key and flags), or 0 when the layer is full. */
static inline struct g7_shape *g7_add_shape(struct g7_layer *l)
{
    if (l->nshape >= G7_MAX_SHAPES)
        return 0;
    struct g7_shape *s = &l->shape[l->nshape++];
    s->key = 0;
    s->flags = 0;
    return s;
}
static inline struct g7_item *g7_add_item(struct g7_layer *l, unsigned kind)
{
    if (l->nitem >= G7_MAX_ITEMS)
        return 0;
    struct g7_item *it = &l->item[l->nitem++];
    it->key = 0;
    it->kind = (uint8_t)kind;
    it->clip = 0;
    it->flags = 0;
    it->a = 1;
    it->color = 0xffffffffU;
    return it;
}
static inline unsigned g7_add_clip(struct g7_layer *l, float x0, float y0, float x1, float y1, float r)
{
    if (l->nclip >= G7_MAX_CLIPS)
        return 0;
    struct g7_clip *c = &l->clip[l->nclip];
    c->x0 = x0;
    c->y0 = y0;
    c->x1 = x1;
    c->y1 = y1;
    c->r = r;
    return l->nclip++;
}
