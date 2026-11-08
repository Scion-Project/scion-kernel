/* SPDX-License-Identifier: GPL-2.0-only
 * Polling DWC3 device-mode driver for the loader (usb_dwc3.c): Exynos 9810 USB 3.0 DRD at
 * 0x10c00000 run as a USB 2.0 high-speed device. One TRB per endpoint, no interrupts.
 *
 * DMA memory: USB_DMA_BASE .. + USB_DMA_SIZE, in the loader's non-cacheable window (mmu.c
 * 0x8c000000..: Normal NC, so no cache maintenance) and used by nothing else while the loader
 * runs (docs/USB-FASTBOOT.md section 3). usb_dwc3.c owns the first USB_DMA_DRIVER bytes; the
 * class buffers (fastboot_dev.c) follow.
 */
#pragma once
#include <stdint.h>
#include "usb_desc.h"
#include "fastboot.h"

#define USB_DMA_BASE   0x91000000UL
#define USB_DMA_SIZE   0x00100000UL
#define USB_DMA_DRIVER 0x00004000UL

#define USBD_EV_RESET      1   /* bus reset: every endpoint but ep0 is gone */
#define USBD_EV_CONNECT    2   /* value: DSTS connect speed (0 high, 1/3 full) */
#define USBD_EV_DISCONNECT 3
#define USBD_EV_CONFIGURED 4   /* value: the configuration (0: deconfigured) */
#define USBD_EV_DONE       5   /* ep: physical endpoint; value: bytes moved; status: TRB status */
#define USBD_EV_LINE       6   /* value: ACM control line state (bit 0 DTR) */
struct usbd_ev {
    int type;
    unsigned ep;
    uint32_t value, status;
};

/* Core up, ep0 armed, pulled up (the host sees the device once D+/D- reach the connector).
 * 0, or a negative stage with *why. */
int usbd_start(const char *serial, const char **why);
/* Soft disconnect, end every transfer, core soft reset: the state an OS's own DWC3 driver
 * expects. Safe to call twice. */
void usbd_stop(void);
/* Service the controller; 1 with an event in *ev. Call often (no interrupts). */
int usbd_poll(struct usbd_ev *ev);
/* One transfer on a configured endpoint: buf (DMA memory), len (OUT: a multiple of the max
 * packet size). 0 queued, -1 busy or not configured. */
int usbd_queue(unsigned phys, uintptr_t buf, uint32_t len);
int usbd_busy(unsigned phys);
int usbd_configured(void);
int usbd_full_speed(void);
int usbd_connected(void);
void usbd_diag(struct tb *t);
/* one screen line: bus resets, setups, set-configurations, stalls */
void usbd_brief(struct tb *t);
