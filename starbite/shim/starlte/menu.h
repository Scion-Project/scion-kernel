/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite boot menu and Starbite Engineer mode (menu.c). */
#pragma once
#include <stdint.h>

struct star_info {
    uint64_t el, sctlr, fdt, rd_start, rd_end;
    uint64_t phys_base, mem_size, entry_va, nsegs;
};

/*
 * Boot menu under Samsung's logo: the catalog's OS rows (catalog.h), Recovery (TWRP), Download,
 * and Starbite Engineer. Returns the catalog entry to start + 1 (0: Download mode), with
 * `cmdline` (BOOT's iOS boot-args) edited in place by Engineer; Recovery and Download reboot
 * from here. If the start fails, star_menu_failed says why and star_menu is called again: it
 * comes back as it was, with the reason on screen.
 */
int star_menu(const struct star_info *in, char *cmdline, unsigned len);
void star_menu_failed(const char *name, const char *why);
/* Catalog entry + 1 of iOS from BOOT (what Engineer's "boot iOS" starts), 0 if there is none. */
int star_ios_pick(void);

/* xnu_boot.c: draw the Starbite overlay again (over black). */
void star_overlay_redraw(void);

/* xnu_boot.c: the v5 menu's sprites ("SBMENU01" in the BOOT ramdisk slot,
 * tools/brand/menu.mjs). star_sprite draws every sprite with that id and
 * returns how many it drew (0 without assets, -1 if malformed). */
int star_menu_assets(void);
int star_sprite(unsigned id);
/* Rectangle of the first sprite with this id; 0 if there is none. */
int star_sprite_rect(unsigned id, unsigned *x, unsigned *y, unsigned *w, unsigned *h);
/* Sprite id into dst (0 = the panel; panel stride), at (x, y) when at, else at its own place,
 * faded to fade/256. An opaque copy, alpha included (ui_sprite blends). */
int star_sprite_to(unsigned id, uint32_t *dst, int x, int y, int at, unsigned fade);

/* Boot menu v6 (menu6.c): full screen, categories, run-time labels, touch. Used when the BOOT
 * ramdisk has its sprites (V6=1 tools/brand/menu.mjs: icon V6_ICON_SPRITE) and the font atlas. */
#define V6_ICON_SPRITE 97U
struct star_settings;
int menu_v6(const struct star_info *in, char *cmdline, unsigned len);
/* menu.c, for menu v6: the settings record, one-shot next, last choice, a failed start. */
void star_menu_begin(void);
struct star_settings *star_menu_settings(int *valid);
int star_menu_settings_save(void);
int star_menu_ready_key(const char *key);
int star_menu_next(void);
void star_menu_remember(int idx);
int star_menu_failure(const char **name, const char **why);
/* Boot menu v7 (menu7.c, the approved concept docs/concepts/ios27-boot.html; docs/MENU7-PORT.md):
 * the default when the BOOT ramdisk has its SB7ASSET container. menu7_ready: 1 when it does.
 * menu7_main: as menu_v6 (entry + 1, or 0 for Download mode), or -1 when v7 cannot run (nothing
 * drawn: v6 follows). menu7_progress / menu7_handoff: the launch card's bar and stage text while
 * the loader loads the pick, and the last frame before the jump. */
int menu7_ready(void);
int menu7_main(const struct star_info *in, char *cmdline, unsigned len);
void m7_report(const char *why);
void menu7_progress(unsigned stage, unsigned n, const char *title, const char *detail);
void menu7_handoff(const char *detail);
/* menu.c: forward to menu v7 while it is the active menu, else nothing (xnu_boot.c calls these at
 * the real stages of each boot method). */
void star_menu_progress(unsigned stage, unsigned n, const char *title, const char *detail);
void star_menu_handoff(const char *detail);
/* anim.c: rectangles (x, y, w, h each) of a composed screen src slide up and fade in, one after
 * another, over what the off-screen buffer holds; then src is the panel. */
void star_reveal(const uint32_t *src, const int *rects, unsigned n);
