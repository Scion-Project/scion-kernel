/* SPDX-License-Identifier: GPL-2.0-only
 * Loader fastboot (FB1) and the USB console (UO1): docs/USB-FASTBOOT.md.
 *
 * Three layers:
 *   fastboot.c      the fastboot protocol subset and the console's line shell. Pure: no
 *                   registers, no fixed addresses, so tools/ios27/usb-fastboot/host_test.c runs it on
 *                   the Mac. The hardware it needs goes through the fbp_* functions below.
 *   fastboot_dev.c  the loader side: fastboot_run() (the USB loop), fastboot_hook() (the
 *                   loader-start check), the fbp_* functions on the phone.
 *   usb_dwc3.c,     the DWC3 device controller, the USBCON PHY and the MAX77705 D+/D- switch.
 *   usb_phy.c,
 *   max77705_muic.c
 * No pointer tables anywhere: the Image must have no absolute relocations (build.sh).
 */
#pragma once
#include <stdint.h>

/* ---- text builder (shared by every USB file) ---------------------------------------------- */
struct tb { char *b; unsigned n, cap; };
void tb_init(struct tb *t, char *buf, unsigned cap);
void tb_c(struct tb *t, char c);
void tb_s(struct tb *t, const char *s);
void tb_u(struct tb *t, uint64_t v);
void tb_hex(struct tb *t, uint64_t v, unsigned digits);     /* "0x" and digits */
void tb_reg(struct tb *t, const char *name, uint32_t v);    /* "name 0x12345678\n" */
int fb_streq(const char *a, const char *b);
int fb_prefix(const char *s, const char *prefix);          /* 1 if s starts with prefix */
unsigned fb_strlen(const char *s);

/* ---- Android boot image (what `fastboot boot` sends) -------------------------------------- */
/* `fastboot boot <file>` wraps any file that is not already a boot image as the kernel of a
 * header v0 boot image (page 2048); `fastboot boot kernel ramdisk` adds the ramdisk, --dtb (with
 * --header-version 2) the DTB, --cmdline the command line. */
#define FB_CMDLINE_MAX 1536U
struct fb_image {
    uintptr_t kernel, ramdisk, dtb;
    uint64_t kernel_size, ramdisk_size, dtb_size;
    uint32_t header_version;   /* 0xffffffff: raw (no boot image header) */
    char cmdline[FB_CMDLINE_MAX + 1];
};
/* img: size bytes at addr. 1 and *out filled, or 0 with *why. */
int fb_image_parse(uintptr_t addr, uint64_t size, struct fb_image *out, const char **why);

/* What the kernel part is */
#define FB_K_UNKNOWN 0U
#define FB_K_XNU     1U   /* "SWEETXNU" payload (mkpayload.py) */
#define FB_K_ARM64   2U   /* arm64 Image header ("ARMd" at +56) */
#define FB_K_BUNDLE  3U   /* "SBUEFI!!" bundle page (tools/store/bundle.py), the UEFI after it */
unsigned fb_kernel_kind(uintptr_t k, uint64_t size);
/* 1 if an EDK2 firmware volume ("_FVH" at +0x28 of a 4 KiB-aligned header) is in the first
 * 8 MiB: the woa UEFI's bootshim + FD look like an arm64 Image. */
int fb_has_fv(uintptr_t k, uint64_t size);
/* The boot method (catalog.h M_*) for this image: forced (0 = auto) or guessed; 0 if none. */
unsigned fb_pick_method(const struct fb_image *img, unsigned forced);
const char *fb_method_name(unsigned m);

/* ---- protocol ------------------------------------------------------------------------------ */
#define FB_ACT_NONE      0
#define FB_ACT_MENU      1   /* `fastboot continue`: back to the boot menu */
#define FB_ACT_BOOT      2   /* `fastboot boot`: the prepared image starts */
#define FB_ACT_REBOOT    3   /* value: PMU INFORM3 (hw.h INFORM_*) */

#define FB_RESP_MAX 64U      /* "OKAY"/"FAIL"/"INFO"/"DATA" + 60 characters */
#define FB_CMD_MAX  512U

/* dl: download buffer (dl_max bytes, DMA-able); scratch / stage: text buffers (getvar all,
 * oem log; `fastboot get_staged` uploads the stage buffer). */
void fb_init(uintptr_t dl, uint32_t dl_max, char *scratch, unsigned scratch_cap,
             char *stage, unsigned stage_cap);
void fb_command(const uint8_t *cmd, unsigned len);
/* Download data still expected (0: the next OUT transfer is a command), and where it goes. */
uint32_t fb_rx_want(void);
uintptr_t fb_rx_ptr(void);
void fb_rx_data(uint32_t n);
/* The next bulk IN transfer (a response, or upload data) into out (at most max bytes, max >=
 * FB_RESP_MAX): its length, 0 when nothing is queued. */
uint32_t fb_tx_next(uint8_t *out, uint32_t max);
int fb_tx_was_msg(void);   /* the last piece was a response (not upload data) */
int fb_tx_idle(void);
/* The action the last command asked for, once everything queued went out: FB_ACT_* (taken). */
int fb_action(uint32_t *value);
/* The host went away (bus reset, cable): drop a download half done and the queue. */
void fb_usb_reset(void);
/* Downloaded bytes (0 after a reset or a failed download) and the forced method. */
uint32_t fb_dl_size(void);
unsigned fb_forced(void);

/* ---- console (CDC-ACM): a line shell ------------------------------------------------------- */
void con_init(char *ring, unsigned cap, char *scratch, unsigned scratch_cap);
void con_hello(void);                               /* banner + prompt (the port was opened) */
void con_input(const uint8_t *b, unsigned n);
unsigned con_tx_take(uint8_t *out, unsigned cap);  /* bytes for the bulk IN endpoint */
unsigned con_pending(void);
int con_action(uint32_t *value);                    /* FB_ACT_*, as fb_action */

/* ---- the loader side (fastboot_dev.c) --------------------------------------------------------
 * fastboot_run: brings up USB (MAX77705 D+/D- switch, PHY, DWC3) and serves fastboot and the
 * console until the host or a key says otherwise. Returns FASTBOOT_MENU (`fastboot continue`,
 * `oem menu`, console `menu`, Bixby or Power pressed, USB failed to start: USB is stopped, go
 * on to the menu) or FASTBOOT_BOOT (`fastboot boot`: the image is placed, USB is stopped, return
 * fastboot_entry() from star_prepare_xnu). Reboots do not return.
 * fastboot_wanted: 1 if the loader should enter fastboot now: boot-next mailbox "fastboot"
 * (consumed) or Volume Up held alone. fastboot_hook: fastboot_wanted, then fastboot_run; the
 * entry to start, or 0 for the menu (the one call star_prepare_xnu makes). */
#define FASTBOOT_MENU 0
#define FASTBOOT_BOOT 1
int fastboot_run(void);
uintptr_t fastboot_entry(void);
int fastboot_wanted(void);
uintptr_t fastboot_hook(void);

/* ---- platform (fastboot_dev.c on the phone, tools/ios27/usb-fastboot/host_test.c on the Mac) ---- */
const char *fbp_serial(void);
int fbp_battery(unsigned *mv, unsigned *pct);                   /* 1 if the fuel gauge answered */
unsigned fbp_cat_count(void);
/* entry i: key, name, catalog.h method, ready (every file present) */
int fbp_cat_entry(unsigned i, const char **key, const char **name, unsigned *method, int *ready);
int fbp_cat_find(const char *key);                              /* index or -1 */
/* A log as text into out: "pmsg" (default), "dmesg", "loader", "usb". Bytes written. */
unsigned fbp_log(const char *which, char *out, unsigned cap);
/* Put the image where its OS wants it, exactly as the catalog's start paths do (xnu_boot.c
 * star_boot_ram); 1 when it is ready to start, 0 with *why. */
int fbp_boot_prepare(unsigned method, const struct fb_image *img, const char **why);
void fbp_set_next(const char *key);                             /* boot-next mailbox */
const char *fbp_speed(void);                                    /* "high", "full", "none" */
void fbp_status(const char *line);                              /* the screen's last-command line */
