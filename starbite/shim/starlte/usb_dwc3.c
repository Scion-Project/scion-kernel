/* SPDX-License-Identifier: GPL-2.0-only
 * Polling DWC3 device-mode driver (usb_dwc3.h) for the Exynos 9810 USB DRD at 0x10c00000.
 *
 * Follows the DWC3 programming model as Linux drivers/usb/dwc3 uses it (core.c dwc3_core_init,
 * gadget.c dwc3_gadget_start / reset / conndone, ep0.c), cross-checked against the woa UEFI's
 * polling driver (build/woa/fw/.../UsbDebugHid.c, BSD-2-Clause-Patent) and our XNU ep0 patch
 * (xnu/series-12377/0073): where they differ, the Linux order wins because it is the one that
 * enumerates on this phone (the ECM + ACM gadget). The DT quirks of our Linux port apply
 * (exynos9810-star2lte.dts usb@10c00000): dis_u2_susphy, dis_u3_susphy, dis-u2-freeclk-exists,
 * frame-length-adjustment 0x20, maximum-speed high-speed.
 *
 * ep0 (USB 2.0 control transfers on DWC3): a SETUP TRB on physical endpoint 0; a data stage TRB
 * on 1 (IN) or 0 (OUT) started at once; the status stage TRB (STATUS2 for two-stage requests,
 * STATUS3 after a data stage) only when the controller reports XferNotReady(Status) on the
 * direction the host chose. SET_ADDRESS goes into DCFG during the setup stage (the core applies
 * it after the status stage), SET_CONFIGURATION configures the endpoints before the status stage.
 * Bulk: one TRB per transfer (normal, LST, IOC; ISP on OUT), restarted by the class layer.
 */
#include "usb_dwc3.h"
#include "hw.h"

#define DWC 0x10c00000UL

#define GSBUSCFG0    0xc100U
#define  GSBUSCFG0_INCRBRSTEN   (1U << 0)
#define  GSBUSCFG0_INCR16BRSTEN (1U << 3)
#define  GSBUSCFG0_REQINFO      ((2U << 16) | (2U << 20) | (2U << 24) | (2U << 28))
#define GSBUSCFG1    0xc104U
#define GCTL         0xc110U
#define  GCTL_PRTCAP_MASK  (3U << 12)
#define  GCTL_PRTCAP_DEV   (2U << 12)
#define  GCTL_SCALEDOWN    (3U << 4)
#define  GCTL_DSBLCLKGTNG  (1U << 0)
#define  GCTL_SOFITPSYNC   (1U << 10)
#define GSTS         0xc118U
#define GSNPSID      0xc120U
#define GUCTL        0xc12cU
#define GHWPARAMS(n) (0xc140U + 4U * (n))
#define GUSB2PHYCFG  0xc200U
#define  U2_PHYSOFTRST     (1U << 31)
#define  U2_FREECLK_EXISTS (1U << 30)
#define  U2_ENBLSLPM       (1U << 8)
#define  U2_SUSPHY         (1U << 6)
#define GUSB3PIPECTL 0xc2c0U
#define  U3_SUSPHY         (1U << 17)
#define GTXFIFOSIZ(n) (0xc300U + 4U * (n))
#define GRXFIFOSIZ0  0xc380U
#define GEVNTADRLO   0xc400U
#define GEVNTADRHI   0xc404U
#define GEVNTSIZ     0xc408U
#define  GEVNTSIZ_INTMASK  (1U << 31)
#define GEVNTCOUNT   0xc40cU
#define GFLADJ       0xc630U
#define DCFG         0xc700U
#define  DCFG_SPEED_MASK   7U
#define  DCFG_ADDR_MASK    (0x7fU << 3)
#define  DCFG_LPM_CAP      (1U << 22)
#define DCTL         0xc704U
#define  DCTL_RUN_STOP     (1U << 31)
#define  DCTL_CSFTRST      (1U << 30)
#define  DCTL_ULSTCHNG_MASK (0xfU << 5)
#define DEVTEN       0xc708U
#define  DEVTEN_DISCONN    (1U << 0)
#define  DEVTEN_USBRST     (1U << 1)
#define  DEVTEN_CONNDONE   (1U << 2)
#define DSTS         0xc70cU
#define  DSTS_DEVCTRLHLT   (1U << 22)
#define DALEPENA     0xc720U
#define DEP(n)       (0xc800U + 0x10U * (n))   /* +0 PAR2, +4 PAR1, +8 PAR0, +c DEPCMD */

#define CMD_SETEPCONFIG   1U
#define CMD_SETXFERRES    2U
#define CMD_SETSTALL      4U
#define CMD_CLEARSTALL    5U
#define CMD_STARTXFER     6U
#define CMD_ENDXFER       8U
#define CMD_STARTCFG      9U
#define CMD_IOC           (1U << 8)
#define CMD_ACT           (1U << 10)
#define CMD_FORCERM       (1U << 11)
#define CMD_PARAM(x)      ((uint32_t)(x) << 16)

#define CFG_ACTION_INIT   0U
#define CFG_ACTION_MODIFY 2U
#define CFG_XFERCOMPLETE  (1U << 8)
#define CFG_XFERNOTREADY  (1U << 10)

#define TRB_HWO  (1U << 0)
#define TRB_LST  (1U << 1)
#define TRB_ISP  (1U << 10)
#define TRB_IOC  (1U << 11)
#define TRB_NORMAL  (1U << 4)
#define TRB_SETUP   (2U << 4)
#define TRB_STATUS2 (3U << 4)
#define TRB_STATUS3 (4U << 4)
#define TRB_CDATA   (5U << 4)

#define TRBSTS_SETUP_PENDING 2U

#define EV_XFERCOMPLETE 1U
#define EV_XFERNOTREADY 3U

/* DMA layout (USB_DMA_BASE, non-cacheable) */
#define EVT_BUF   (USB_DMA_BASE + 0x0000UL)
#define EVT_SIZE  4096U
#define TRB(n)    (USB_DMA_BASE + 0x1000UL + 16UL * (n))
#define SETUP_BUF (USB_DMA_BASE + 0x1200UL)
#define EP0_BUF   (USB_DMA_BASE + 0x1400UL)   /* 512 bytes */

#define EPN 8U    /* physical endpoints used: 0..7 (ep0, 0x81, 0x02/0x82, 0x03/0x83) */
#define QN 16U

#define E0_SETUP  0
#define E0_DATA   1
#define E0_WAIT   2   /* waiting for XferNotReady(Status) */
#define E0_STATUS 3

struct epst {
    uint8_t on, busy, res;
    uintptr_t buf;
    uint32_t len;
};

static struct {
    int started, connected;
    uint32_t id;
    unsigned evt_pos;
    struct usb_dev_state st;
    struct epst ep[EPN];
    int e0, e0_three, e0_phys, e0_zlp, e0_pending;
    uint8_t setup[8];
    uint64_t e0_t0;
    unsigned speed;
    struct usbd_ev q[QN];
    unsigned qh, qn;
    /* diagnostics */
    uint32_t nev, nreset, nconn, ndisc, nstall, nsetup, nwatchdog, ncfg, cmd_fail;
    uint32_t last_cmd_fail, last_ev[16];
    unsigned last_i;
    const char *why;
} d;

static struct ch9_res res;

static uint32_t rd(uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(DWC + off);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uint32_t off, uint32_t v)
{
    __asm__ volatile("dsb st" : : : "memory");   /* TRB and buffer stores (Normal NC) first */
    *(volatile uint32_t *)(DWC + off) = v;
}

static void dctl(uint32_t v)
{
    wr(DCTL, v & ~DCTL_ULSTCHNG_MASK);           /* never request a link state by accident */
}

static int wait_bits(uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
    for (unsigned i = 0; i < us; i += 10) {
        if ((rd(off) & mask) == want)
            return 1;
        hw_wait_us(10);
    }
    return (rd(off) & mask) == want;
}

static void push(int type, unsigned ep, uint32_t value, uint32_t status)
{
    if (d.qn >= QN)
        return;
    struct usbd_ev *e = &d.q[(d.qh + d.qn++) % QN];
    e->type = type;
    e->ep = ep;
    e->value = value;
    e->status = status;
}

static int depcmd(unsigned phys, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    uint32_t b = DEP(phys);
    wr(b + 0x0, p2);
    wr(b + 0x4, p1);
    wr(b + 0x8, p0);
    wr(b + 0xc, cmd | CMD_ACT);
    for (unsigned i = 0; i < 5000; ++i) {
        uint32_t v = rd(b + 0xc);
        if (!(v & CMD_ACT)) {
            unsigned status = (v >> 12) & 0xfU;
            if (status) {
                ++d.cmd_fail;
                d.last_cmd_fail = phys << 24 | (cmd & 0xffU) << 16 | (v & 0xffffU);
                return -(int)status;
            }
            return 0;
        }
        hw_wait_us(1);
    }
    ++d.cmd_fail;
    d.last_cmd_fail = 0x80000000U | phys << 24 | (cmd & 0xffU) << 16;
    return -16;
}

static int ep_cfg(unsigned phys, unsigned type, unsigned mps, unsigned action, unsigned interval)
{
    uint32_t p0 = type << 1 | mps << 3 | action << 30;
    uint32_t p1 = CFG_XFERCOMPLETE | phys << 25;
    if (phys & 1U)
        p0 |= (phys >> 1) << 17;                 /* TX FIFO = endpoint number */
    if (phys < 2)
        p1 |= CFG_XFERNOTREADY;
    if (interval)
        p1 |= (interval - 1U) << 16;
    return depcmd(phys, CMD_SETEPCONFIG, p0, p1, 0);
}

static int ep_start(unsigned phys, uintptr_t buf, uint32_t len, uint32_t type)
{
    volatile uint32_t *t = (volatile uint32_t *)TRB(phys);
    uint32_t ctl = type | TRB_HWO | TRB_LST | TRB_IOC;
    if (!(phys & 1U))
        ctl |= TRB_ISP;
    t[0] = (uint32_t)buf;
    t[1] = (uint32_t)((uint64_t)buf >> 32);
    t[2] = len & 0x00ffffffU;
    t[3] = ctl;
    uint64_t ta = TRB(phys);
    int r = depcmd(phys, CMD_STARTXFER, (uint32_t)(ta >> 32), (uint32_t)ta, 0);
    if (r)
        return r;
    d.ep[phys].res = (uint8_t)((rd(DEP(phys) + 0xc) >> 16) & 0x7fU);
    d.ep[phys].busy = 1;
    d.ep[phys].buf = buf;
    d.ep[phys].len = len;
    return 0;
}

static void ep_end(unsigned phys)
{
    if (!d.ep[phys].busy)
        return;
    (void)depcmd(phys, CMD_ENDXFER | CMD_IOC | CMD_FORCERM | CMD_PARAM(d.ep[phys].res), 0, 0, 0);
    hw_wait_us(100);
    d.ep[phys].busy = 0;
}

/* bytes moved by the finished TRB of phys */
static uint32_t ep_done_len(unsigned phys, uint32_t *trbsts)
{
    __asm__ volatile("dsb sy" : : : "memory");
    uint32_t sz = ((volatile uint32_t *)TRB(phys))[2];
    uint32_t left = sz & 0x00ffffffU;
    *trbsts = sz >> 28;
    return left <= d.ep[phys].len ? d.ep[phys].len - left : 0;
}

/* ---- ep0 ------------------------------------------------------------------------------------ */

static void e0_arm_setup(void)
{
    d.e0 = E0_SETUP;
    d.e0_pending = 0;
    if (!d.ep[0].busy && ep_start(0, SETUP_BUF, 8, TRB_SETUP))
        d.why = "ep0 setup start failed";
}

static void e0_stall(void)
{
    ep_end(0);
    ep_end(1);
    (void)depcmd(0, CMD_SETSTALL, 0, 0, 0);
    ++d.nstall;
    e0_arm_setup();
}

static void e0_status(unsigned phys)
{
    if (ep_start(phys, EP0_BUF, 0, d.e0_three ? TRB_STATUS3 : TRB_STATUS2)) {
        e0_stall();
        return;
    }
    d.e0 = E0_STATUS;
}

/* to the status stage: at once if its XferNotReady already came */
static void e0_wait_status(void)
{
    d.e0 = E0_WAIT;
    if (d.e0_pending) {
        unsigned p = (unsigned)d.e0_pending - 1U;
        d.e0_pending = 0;
        e0_status(p);
    }
}

static void configure(unsigned cfg)
{
    static const uint8_t list[5] = { USB_EP_ACM_NOTIFY, USB_EP_ACM_IN, USB_EP_ACM_OUT,
                                     USB_EP_FB_IN, USB_EP_FB_OUT };
    for (unsigned p = 2; p < EPN; ++p) {
        ep_end(p);
        d.ep[p].on = 0;
    }
    wr(DALEPENA, 3);
    if (cfg == 1) {
        (void)depcmd(0, CMD_STARTCFG | CMD_PARAM(2), 0, 0, 0);
        uint32_t ena = 3;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned a = list[i], p = USB_PHYS(a);
            unsigned intr = a == USB_EP_ACM_NOTIFY;
            if (ep_cfg(p, intr ? 3U : 2U, usb_ep_mps(a, d.st.full_speed), CFG_ACTION_INIT, intr ? 9U : 0U) ||
                depcmd(p, CMD_SETXFERRES, 1, 0, 0)) {
                d.why = "endpoint configuration failed";
                continue;
            }
            d.ep[p].on = 1;
            ena |= 1U << p;
        }
        wr(DALEPENA, ena);
    }
    ++d.ncfg;
    push(USBD_EV_CONFIGURED, 0, cfg, 0);
}

static void clear_halt(unsigned addr)
{
    unsigned p = USB_PHYS(addr);
    if (p < 2 || p >= EPN || !d.ep[p].on)
        return;
    if (d.ep[p].busy) {
        /* end the transfer, clear, start it again (Linux __dwc3_gadget_ep_set_halt) */
        uintptr_t buf = d.ep[p].buf;
        uint32_t len = d.ep[p].len;
        ep_end(p);
        (void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0);
        (void)ep_start(p, buf, len, TRB_NORMAL);
    } else {
        (void)depcmd(p, CMD_CLEARSTALL, 0, 0, 0);
    }
}

static void e0_setup(void)
{
    const volatile uint8_t *s = (const volatile uint8_t *)SETUP_BUF;
    for (unsigned i = 0; i < 8; ++i)
        d.setup[i] = s[i];
    ++d.nsetup;
    d.e0_t0 = hw_ticks();
    usb_ch9_setup(&d.st, d.setup, &res);
    if (res.set_address >= 0)
        wr(DCFG, (rd(DCFG) & ~DCFG_ADDR_MASK) | (uint32_t)res.set_address << 3);
    if (res.set_config >= 0)
        configure((unsigned)res.set_config);
    if (res.clear_halt >= 0)
        clear_halt((unsigned)res.clear_halt);
    if (res.line_changed)
        push(USBD_EV_LINE, 0, d.st.line_state, 0);
    unsigned wlen = (unsigned)d.setup[6] | (unsigned)d.setup[7] << 8;
    d.e0_pending = 0;
    if (res.action == CH9_ACK) {
        d.e0_three = 0;
        d.e0 = E0_WAIT;
    } else if (res.action == CH9_IN) {
        volatile uint8_t *b = (volatile uint8_t *)EP0_BUF;
        for (unsigned i = 0; i < res.len; ++i)
            b[i] = res.data[i];
        d.e0_three = 1;
        d.e0_phys = 1;
        d.e0_zlp = res.len < wlen && !(res.len % USB_EP0_MPS);
        d.e0 = E0_DATA;
        if (ep_start(1, EP0_BUF, res.len, TRB_CDATA))
            e0_stall();
    } else if (res.action == CH9_OUT) {
        d.e0_three = 1;
        d.e0_phys = 0;
        d.e0_zlp = 0;
        d.e0 = E0_DATA;
        if (ep_start(0, EP0_BUF, (res.len + USB_EP0_MPS - 1) / USB_EP0_MPS * USB_EP0_MPS, TRB_CDATA))
            e0_stall();
    } else {
        e0_stall();
    }
}

static void e0_event(unsigned phys, unsigned type, unsigned status)
{
    if (type == EV_XFERCOMPLETE) {
        if (!d.ep[phys].busy)
            return;                                  /* ended by us: stale */
        uint32_t trbsts, n = ep_done_len(phys, &trbsts);
        d.ep[phys].busy = 0;
        if (d.e0 == E0_SETUP) {
            if (phys == 0)
                e0_setup();
        } else if (d.e0 == E0_DATA && phys == (unsigned)d.e0_phys) {
            if (trbsts == TRBSTS_SETUP_PENDING) {
                e0_arm_setup();                      /* the host gave up on it and sent a new SETUP */
                return;
            }
            if (phys == 1 && d.e0_zlp) {
                d.e0_zlp = 0;
                if (ep_start(1, EP0_BUF, 0, TRB_CDATA))
                    e0_stall();
                return;
            }
            if (phys == 0) {
                uint8_t b[64];
                const volatile uint8_t *s = (const volatile uint8_t *)EP0_BUF;
                if (n > sizeof(b))
                    n = sizeof(b);
                for (unsigned i = 0; i < n; ++i)
                    b[i] = s[i];
                usb_ch9_out(&d.st, d.setup, b, n);
            }
            e0_wait_status();
        } else if (d.e0 == E0_STATUS) {
            e0_arm_setup();
        }
        return;
    }
    if (type == EV_XFERNOTREADY) {
        unsigned phase = status & 3U;
        if (phase == 2U) {                           /* the host is in the status stage */
            if (d.e0 == E0_WAIT)
                e0_status(phys);
            else if (d.e0 == E0_DATA)
                d.e0_pending = (int)phys + 1;
        } else if (phase == 1U && d.e0 == E0_DATA && phys != (unsigned)d.e0_phys) {
            e0_stall();                              /* data stage in the other direction */
        }
    }
}

/* ---- events --------------------------------------------------------------------------------- */

static void on_reset(void)
{
    ++d.nreset;
    for (unsigned p = 2; p < EPN; ++p) {
        ep_end(p);
        d.ep[p].on = 0;
    }
    wr(DALEPENA, 3);
    wr(DCFG, rd(DCFG) & ~DCFG_ADDR_MASK);
    d.st.config = 0;
    if (d.e0 != E0_SETUP) {
        ep_end(0);
        ep_end(1);
    }
    e0_arm_setup();
    push(USBD_EV_RESET, 0, 0, 0);
}

static void on_conndone(void)
{
    ++d.nconn;
    d.speed = rd(DSTS) & 7U;
    d.st.full_speed = d.speed == 1U || d.speed == 3U;
    d.connected = 1;
    /* ep0's max packet for the link (64 at high and full speed) */
    (void)ep_cfg(0, 0, USB_EP0_MPS, CFG_ACTION_MODIFY, 0);
    (void)ep_cfg(1, 0, USB_EP0_MPS, CFG_ACTION_MODIFY, 0);
    push(USBD_EV_CONNECT, 0, d.speed, 0);
}

static void dev_event(unsigned type)
{
    if (type == 0) {                                 /* disconnect */
        ++d.ndisc;
        d.connected = 0;
        d.st.config = 0;
        push(USBD_EV_DISCONNECT, 0, 0, 0);
    } else if (type == 1) {
        on_reset();
    } else if (type == 2) {
        on_conndone();
    }
}

static void ep_event(unsigned phys, unsigned type, unsigned status)
{
    if (phys < 2) {
        e0_event(phys, type, status);
        return;
    }
    if (phys >= EPN || type != EV_XFERCOMPLETE || !d.ep[phys].busy)
        return;
    uint32_t trbsts, n = ep_done_len(phys, &trbsts);
    d.ep[phys].busy = 0;
    push(USBD_EV_DONE, phys, n, trbsts);
}

static void hw_events(void)
{
    uint32_t cnt = rd(GEVNTCOUNT) & 0xfffcU;
    if (!cnt)
        return;
    __asm__ volatile("dsb sy" : : : "memory");
    while (cnt >= 4 && d.qn + 4 <= QN) {
        uint32_t ev = *(volatile uint32_t *)(EVT_BUF + d.evt_pos);
        d.evt_pos = (d.evt_pos + 4U) & (EVT_SIZE - 1U);
        cnt -= 4;
        wr(GEVNTCOUNT, 4);
        ++d.nev;
        d.last_ev[d.last_i++ & 15U] = ev;
        if (ev & 1U) {
            if (!((ev >> 1) & 0x7fU))
                dev_event((ev >> 8) & 0xfU);
        } else {
            ep_event((ev >> 1) & 0x1fU, (ev >> 6) & 0xfU, (ev >> 12) & 0xfU);
        }
    }
}

int usbd_poll(struct usbd_ev *ev)
{
    if (!d.started)
        return 0;
    if (!d.qn)
        hw_events();
    /* a control transfer the host abandoned (or a status stage we missed): start over */
    if (d.e0 != E0_SETUP && hw_ticks() - d.e0_t0 > hw_freq()) {
        ++d.nwatchdog;
        e0_stall();
    }
    if (!d.qn)
        return 0;
    *ev = d.q[d.qh];
    d.qh = (d.qh + 1) % QN;
    --d.qn;
    return 1;
}

int usbd_queue(unsigned phys, uintptr_t buf, uint32_t len)
{
    if (phys < 2 || phys >= EPN || !d.ep[phys].on || d.ep[phys].busy)
        return -1;
    return ep_start(phys, buf, len, TRB_NORMAL) ? -1 : 0;
}

int usbd_busy(unsigned phys) { return phys < EPN && d.ep[phys].busy; }
int usbd_configured(void) { return d.started && d.st.config == 1; }
int usbd_full_speed(void) { return d.st.full_speed; }
int usbd_connected(void) { return d.connected; }

/* ---- start / stop --------------------------------------------------------------------------- */

static int fail(int stage, const char *why, const char **out)
{
    d.why = why;
    *out = why;
    return stage;
}

int usbd_start(const char *serial, const char **why)
{
    for (uintptr_t a = USB_DMA_BASE; a < USB_DMA_BASE + USB_DMA_DRIVER; a += 8)
        *(volatile uint64_t *)a = 0;
    {
        uint8_t *p = (uint8_t *)&d;
        for (unsigned i = 0; i < sizeof(d); ++i)
            p[i] = 0;
    }
    usb_ch9_init(&d.st, serial);
    d.id = rd(GSNPSID);
    if ((d.id & 0xffff0000U) != 0x55330000U && (d.id & 0xffff0000U) != 0x33310000U)
        return fail(-1, "no DWC3 at 0x10c00000 (GSNPSID)", why);

    /* core soft reset: completes only with the PHY's clock running */
    dctl(DCTL_CSFTRST);
    if (!wait_bits(DCTL, DCTL_CSFTRST, 0, 200000))
        return fail(-2, "DWC3 soft reset stuck (no PHY clock?)", why);
    hw_wait_ms(10);

    /* USB 2.0 PHY interface: no PHY suspend, no LPM sleep signals, no free-running clock
     * (the DT quirks; Scion's driver, which runs on this phone, clears the same three) */
    uint32_t v = rd(GUSB2PHYCFG);
    v &= ~(U2_SUSPHY | U2_FREECLK_EXISTS | U2_ENBLSLPM | U2_PHYSOFTRST);
    wr(GUSB2PHYCFG, v);
    wr(GUSB3PIPECTL, rd(GUSB3PIPECTL) & ~U3_SUSPHY);

    unsigned rev = d.id & 0xffffU;
    v = rd(GCTL) & ~(GCTL_SCALEDOWN | GCTL_PRTCAP_MASK | GCTL_DSBLCLKGTNG);
    if ((d.id >> 16) == 0x5533U && rev >= 0x210aU && rev <= 0x250aU)
        v |= GCTL_DSBLCLKGTNG;                       /* Linux's 2.10a..2.50a workaround */
    wr(GCTL, v | GCTL_PRTCAP_DEV | GCTL_SOFITPSYNC);
    /* the bus as Samsung's dwc3_core_config sets it (Scion and the woa UEFI do the same):
     * INCR bursts up to 16, AXI cache hints 2 (from 2.20a), burst request limit 3; GUCTL
     * REFCLKPER 0x14 and USBHSTINAUTORETRYEN, the values both of them write (the period field
     * only matters for SuperSpeed and LPM timing, neither used here) */
    v = rd(GSBUSCFG0) | GSBUSCFG0_INCRBRSTEN | GSBUSCFG0_INCR16BRSTEN;
    if ((d.id >> 16) == 0x3331U || d.id >= 0x5533220aU)
        v |= GSBUSCFG0_REQINFO;
    wr(GSBUSCFG0, v);
    wr(GSBUSCFG1, (rd(GSBUSCFG1) & ~(0xfU << 8)) | (3U << 8));
    wr(GUCTL, (rd(GUCTL) & ~(0x3ffU << 22)) | (0x14U << 22) | (1U << 14));
    if ((d.id >> 16) != 0x5533U || rev >= 0x250aU) {
        v = rd(GFLADJ);
        if ((v & 0x3fU) != 0x20U)
            wr(GFLADJ, (v & ~0x3fU) | (1U << 7) | 0x20U);
    }
    wr(DCFG, (rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_ADDR_MASK | DCFG_LPM_CAP)) | 0U);   /* high speed */

    /* one event buffer, its interrupt masked: polled */
    wr(GEVNTADRLO, (uint32_t)EVT_BUF);
    wr(GEVNTADRHI, (uint32_t)((uint64_t)EVT_BUF >> 32));
    wr(GEVNTSIZ, GEVNTSIZ_INTMASK | EVT_SIZE);
    wr(GEVNTCOUNT, rd(GEVNTCOUNT) & 0xfffcU);
    d.evt_pos = 0;
    wr(DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNDONE);

    /* ep0: transfer resources from 0, both directions at 512 until Connect Done says 64 */
    if (depcmd(0, CMD_STARTCFG | CMD_PARAM(0), 0, 0, 0) ||
        ep_cfg(0, 0, 512, CFG_ACTION_INIT, 0) || depcmd(0, CMD_SETXFERRES, 1, 0, 0) ||
        ep_cfg(1, 0, 512, CFG_ACTION_INIT, 0) || depcmd(1, CMD_SETXFERRES, 1, 0, 0))
        return fail(-3, "ep0 configuration refused (DEPCMD)", why);
    d.ep[0].on = d.ep[1].on = 1;
    wr(DALEPENA, 3);
    d.started = 1;
    e0_arm_setup();
    if (!d.ep[0].busy)
        return fail(-4, "ep0 setup TRB refused", why);

    dctl(rd(DCTL) | DCTL_RUN_STOP);
    if (!wait_bits(DSTS, DSTS_DEVCTRLHLT, 0, 100000))
        return fail(-5, "DWC3 did not leave halt (run/stop)", why);
    d.why = 0;
    *why = 0;
    return 0;
}

void usbd_stop(void)
{
    if (!d.started)
        return;
    for (unsigned p = 2; p < EPN; ++p)
        ep_end(p);
    ep_end(0);
    ep_end(1);
    dctl(rd(DCTL) & ~DCTL_RUN_STOP);
    /* the core halts once its event buffer is drained */
    for (unsigned i = 0; i < 1000 && !(rd(DSTS) & DSTS_DEVCTRLHLT); ++i) {
        uint32_t c = rd(GEVNTCOUNT) & 0xfffcU;
        if (c)
            wr(GEVNTCOUNT, c);
        hw_wait_us(100);
    }
    wr(DEVTEN, 0);
    wr(GEVNTSIZ, GEVNTSIZ_INTMASK | EVT_SIZE);
    {
        uint32_t c = rd(GEVNTCOUNT) & 0xfffcU;
        if (c)
            wr(GEVNTCOUNT, c);
    }
    wr(DALEPENA, 0);
    dctl(DCTL_CSFTRST);
    (void)wait_bits(DCTL, DCTL_CSFTRST, 0, 100000);
    wr(GEVNTADRLO, 0);
    wr(GEVNTADRHI, 0);
    wr(GEVNTSIZ, GEVNTSIZ_INTMASK);
    wr(GEVNTCOUNT, 0);
    d.started = 0;
    d.connected = 0;
    d.st.config = 0;
}

void usbd_brief(struct tb *t)
{
    tb_s(t, "RST ");
    tb_u(t, d.nreset);
    tb_s(t, " SETUP ");
    tb_u(t, d.nsetup);
    tb_s(t, " CFG ");
    tb_u(t, d.ncfg);
    tb_s(t, " STALL ");
    tb_u(t, d.nstall);
}

void usbd_diag(struct tb *t)
{
    tb_s(t, "dwc3: ");
    tb_s(t, d.why ? d.why : d.started ? "running" : "stopped");
    tb_c(t, '\n');
    tb_reg(t, "gsnpsid", rd(GSNPSID));
    tb_reg(t, "gctl", rd(GCTL));
    tb_reg(t, "gsts", rd(GSTS));
    tb_reg(t, "gusb2phycfg", rd(GUSB2PHYCFG));
    tb_reg(t, "gusb3pipectl", rd(GUSB3PIPECTL));
    tb_reg(t, "gsbuscfg0", rd(GSBUSCFG0));
    tb_reg(t, "guctl", rd(GUCTL));
    for (unsigned i = 0; i < 4; ++i) {
        char n[16];
        struct tb w;
        tb_init(&w, n, sizeof(n));
        tb_s(&w, "ghwparams");
        tb_u(&w, i);
        tb_reg(t, n, rd(GHWPARAMS(i)));
    }
    for (unsigned i = 0; i < 4; ++i) {
        char n[16];
        struct tb w;
        tb_init(&w, n, sizeof(n));
        tb_s(&w, "gtxfifosiz");
        tb_u(&w, i);
        tb_reg(t, n, rd(GTXFIFOSIZ(i)));
    }
    tb_reg(t, "grxfifosiz0", rd(GRXFIFOSIZ0));
    tb_reg(t, "dcfg", rd(DCFG));
    tb_reg(t, "dctl", rd(DCTL));
    tb_reg(t, "dsts", rd(DSTS));
    tb_reg(t, "dalepena", rd(DALEPENA));
    tb_reg(t, "gevntcount", rd(GEVNTCOUNT));
    tb_s(t, "dwc3: events ");
    tb_u(t, d.nev);
    tb_s(t, " resets ");
    tb_u(t, d.nreset);
    tb_s(t, " conndone ");
    tb_u(t, d.nconn);
    tb_s(t, " disconnects ");
    tb_u(t, d.ndisc);
    tb_s(t, " setups ");
    tb_u(t, d.nsetup);
    tb_s(t, " stalls ");
    tb_u(t, d.nstall);
    tb_s(t, " ep0 watchdog ");
    tb_u(t, d.nwatchdog);
    tb_s(t, " set-configs ");
    tb_u(t, d.ncfg);
    tb_s(t, "\ndwc3: speed ");
    tb_u(t, d.speed);
    tb_s(t, " config ");
    tb_u(t, d.st.config);
    tb_s(t, " line state ");
    tb_u(t, d.st.line_state);
    tb_s(t, " ep0 state ");
    tb_u(t, (unsigned)d.e0);
    tb_s(t, " cmd failures ");
    tb_u(t, d.cmd_fail);
    tb_c(t, ' ');
    tb_hex(t, d.last_cmd_fail, 8);
    tb_s(t, "\ndwc3: last setup");
    for (unsigned i = 0; i < 8; ++i) {
        tb_c(t, ' ');
        tb_hex(t, d.setup[i], 2);
    }
    tb_s(t, "\ndwc3: last events (oldest first):");
    unsigned n = d.last_i < 16 ? d.last_i : 16;
    for (unsigned i = 0; i < n; ++i) {
        if (!(i & 3))
            tb_s(t, "\n ");
        tb_c(t, ' ');
        tb_hex(t, d.last_ev[(d.last_i - n + i) & 15U], 8);
    }
    tb_c(t, '\n');
}
