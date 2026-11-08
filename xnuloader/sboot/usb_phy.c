/* SPDX-License-Identifier: GPL-2.0-only
 * Exynos 9810 USB 2.0 PHY bring-up for the loader (bare metal, MMU on: registers are Device
 * memory, mmu.c).
 *
 * The sequence is tools/linux/port/src/usb/phy-exynos9810-usbdrd.c (s9p_usbphy_init), which is
 * the vendor CAL for EXYNOS_USBCON_VER_03_0_0 (phy_exynos_usb_v3p1_enable) and is what our Linux
 * port runs when its ECM + ACM gadget enumerates on the Mac; the woa UEFI's polling driver
 * (UsbDebugHid.c UsbEnablePhy, BSD-2-Clause-Patent) does the same steps without the OCD, ReWA
 * and tune ones. Clocks: only the three gates the Linux port opens (exynos9810-star2lte.dts
 * s9p,clock-gates): GATE_CLKCMU_FSYS0_USB30DRD 0x1a242064, FSYS0 USB30DRD_REF_CLK 0x1100206c,
 * FSYS0 USBDPPHY_REF_SOC_PLL 0x11002008, MANUAL | CG_VAL (bits 20, 21) set. Muxes, dividers and
 * Q-channels are only read (the woa driver's UsbValidateClocks list), never written; MIF/INT are
 * never touched. PMU: USB2 PHY isolation 0x1406072c and USB3/DP 0x14060704, bit 0 = enabled.
 *
 * Nothing here is undone before an OS starts: every OS here runs its own PHY init (Linux this
 * same sequence; the woa UEFI saves and restores its registers), and S-Boot leaves the PHY in an
 * unknown state anyway.
 */
#include "usb_phy.h"
#include "hw.h"

#define USBCON 0x11100000UL
#define  LINK_CTRL     0x04U
#define   LC_PIPE3_FORCE_RX_ELEC_IDLE (1U << 18)
#define   LC_PIPE3_FORCE_PHY_STATUS   (1U << 17)
#define   LC_PIPE3_FORCE_EN           (1U << 16)
#define   LC_DIS_QACT_LINKGATE        (1U << 12)
#define   LC_DIS_QACT_ID0             (1U << 11)
#define   LC_DIS_QACT_VBUS_VALID      (1U << 10)
#define   LC_DIS_QACT_BVALID          (1U << 9)
#define   LC_FORCE_QACT               (1U << 8)
#define   LC_BUS_FILTER_BYPASS        (0xfU << 4)
#define  LINK_PORT     0x08U
#define   LP_HUB_PORT_SEL_OCD_U3      (1U << 3)
#define   LP_HUB_PORT_SEL_OCD_U2      (1U << 2)
#define  CLKRST        0x20U
#define   CR_PHY_SW_RST               (1U << 3)
#define   CR_PHY_RST_SEL              (1U << 2)
#define   CR_PORT_RST                 (1U << 1)
#define   CR_LINK_SW_RST              (1U << 0)
#define  PWR           0x24U
#define  COMBO_PMA     0x48U
#define   PMA_LOW_PWRN                (1U << 4)
#define  UTMI          0x50U
#define   UTMI_FORCE_VBUSVALID        (1U << 5)
#define   UTMI_FORCE_BVALID           (1U << 4)
#define   UTMI_DP_PULLDOWN            (1U << 3)
#define   UTMI_DM_PULLDOWN            (1U << 2)
#define   UTMI_FORCE_SUSPEND          (1U << 1)
#define   UTMI_FORCE_SLEEP            (1U << 0)
#define  HSP           0x54U
#define   HSP_VBUSVLDEXTSEL           (1U << 13)
#define   HSP_VBUSVLDEXT              (1U << 12)
#define   HSP_EN_UTMISUSPEND          (1U << 9)
#define   HSP_COMMONONN               (1U << 8)
#define  HSP_TUNE      0x58U
#define  HSP_TEST      0x5cU
#define   HSP_TEST_SIDDQ              (1U << 24)
#define  REWA_ENABLE   0x100U
#define  HSREWA_CTRL   0x108U
#define  HSREWA_REFTO  0x10cU
#define  HSREWA_HSTK   0x110U
#define  HSREWA_INT1_MASK 0x11cU

/* stock star2lte device-mode HS tuning: tx_vref 0xd, tx_rise 1, tx_res 3, tx_pre_emp 3,
 * rx_sqrx 4, compdis 3 (phy-exynos9810-usbdrd.c S9P_HSP_TUNE_*) */
#define TUNE_MASK ((0xfU << 28) | (0x3U << 24) | (0x3U << 21) | (0x3U << 18) | (0x7U << 8) | 0x7U)
#define TUNE_VAL  ((0xdU << 28) | (0x1U << 24) | (0x3U << 21) | (0x3U << 18) | (0x4U << 8) | 0x3U)

#define PMU_USB20_PHY 0x1406072cUL
#define PMU_USB3DP_PHY 0x14060704UL

#define GATE_TOP_USB30DRD  0x1a242064UL
#define GATE_FSYS0_REF     0x1100206cUL
#define GATE_FSYS0_DPPHY   0x11002008UL
#define GATE_MANUAL_CGVAL  ((1U << 20) | (1U << 21))

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static void rmw(uintptr_t a, uint32_t clear, uint32_t set)
{
    wr(a, (rd(a) & ~clear) | set);
}

/* read-only snapshot: the woa driver's clock checks, the gates, the PMU and USBCON */
#define SNAP_N 20U
static uint32_t snap_before[SNAP_N], snap_after[SNAP_N];
static int snapped;

static uintptr_t snap_addr(unsigned i)
{
    switch (i) {
    case 0: return 0x1a241060UL;   /* CMU_TOP mux CLKCMU_FSYS0_USB30DRD */
    case 1: return 0x1a241858UL;   /* CMU_TOP div */
    case 2: return GATE_TOP_USB30DRD;
    case 3: return 0x110001e0UL;   /* CMU_FSYS0 mux */
    case 4: return 0x11000228UL;   /* CMU_FSYS0 mux autogate */
    case 5: return GATE_FSYS0_REF;
    case 6: return GATE_FSYS0_DPPHY;
    case 7: return 0x11003000UL;   /* FSYS0 QCH: CMU */
    case 8: return 0x1100304cUL;   /* QCH: USB30DRD ctrl */
    case 9: return 0x11003050UL;   /* QCH: link */
    case 10: return 0x11003054UL;  /* QCH: PHY */
    case 11: return 0x11003058UL;  /* QCH: PCS */
    case 12: return PMU_USB20_PHY;
    case 13: return PMU_USB3DP_PHY;
    case 14: return USBCON + 0x00;   /* CTRL_VER */
    case 15: return USBCON + LINK_CTRL;
    case 16: return USBCON + CLKRST;
    case 17: return USBCON + UTMI;
    case 18: return USBCON + HSP;
    default: return USBCON + HSP_TEST;
    }
}

static const char *snap_name(unsigned i)
{
    switch (i) {
    case 0: return "top.mux";
    case 1: return "top.div";
    case 2: return "top.gate";
    case 3: return "fsys0.mux";
    case 4: return "fsys0.mux_ag";
    case 5: return "fsys0.ref_gate";
    case 6: return "fsys0.dpphy_gate";
    case 7: return "qch.cmu";
    case 8: return "qch.ctrl";
    case 9: return "qch.link";
    case 10: return "qch.phy";
    case 11: return "qch.pcs";
    case 12: return "pmu.usb20";
    case 13: return "pmu.usb3dp";
    case 14: return "usbcon.ver";
    case 15: return "usbcon.link";
    case 16: return "usbcon.clkrst";
    case 17: return "usbcon.utmi";
    case 18: return "usbcon.hsp";
    default: return "usbcon.hsp_test";
    }
}

static void snap(uint32_t *s)
{
    for (unsigned i = 0; i < SNAP_N; ++i)
        s[i] = rd(snap_addr(i));
}

const char *usb_phy_line(void)
{
    unsigned ls = (rd(USBCON + HSP_TEST) >> 20) & 3U;   /* HSP_TEST LINESTATE (Scion's glue) */
    return ls == 0 ? "SE0" : ls == 1 ? "J" : ls == 2 ? "K" : "SE1";
}

void usb_phy_diag(struct tb *t)
{
    if (!snapped) {
        tb_s(t, "phy: not started\n");
        return;
    }
    tb_s(t, "phy registers (as found -> after init):\n");
    for (unsigned i = 0; i < SNAP_N; ++i) {
        tb_s(t, snap_name(i));
        tb_c(t, ' ');
        tb_hex(t, snap_before[i], 8);
        tb_s(t, " -> ");
        tb_hex(t, snap_after[i], 8);
        tb_c(t, '\n');
    }
    tb_s(t, "usbcon.linestate ");
    tb_s(t, usb_phy_line());
    tb_s(t, (rd(USBCON + HSP_TEST) & HSP_TEST_SIDDQ) ? " (PHY powered down)\n" : "\n");
    tb_reg(t, "usbcon.hsp_tune", rd(USBCON + HSP_TUNE));
    tb_reg(t, "usbcon.link_port", rd(USBCON + LINK_PORT));
    tb_reg(t, "usbcon.pma", rd(USBCON + COMBO_PMA));
}

/* exynos_cal_usbphy_q_ch(enable): the Q-channel forced active, with the vendor's delays */
static void q_ch_on(void)
{
    uintptr_t r = USBCON + LINK_CTRL;
    uint32_t v = rd(r);
    v |= LC_DIS_QACT_ID0 | LC_DIS_QACT_VBUS_VALID | LC_DIS_QACT_BVALID | LC_DIS_QACT_LINKGATE;
    v &= ~LC_FORCE_QACT;
    hw_wait_us(500);
    wr(r, v);
    hw_wait_us(500);
    v = rd(r) | LC_FORCE_QACT;
    hw_wait_us(500);
    wr(r, v);
}

int usb_phy_start(const char **why)
{
    const uintptr_t r = USBCON;
    snap(snap_before);
    snapped = 1;

    wr(GATE_TOP_USB30DRD, rd(GATE_TOP_USB30DRD) | GATE_MANUAL_CGVAL);
    wr(GATE_FSYS0_REF, rd(GATE_FSYS0_REF) | GATE_MANUAL_CGVAL);
    wr(GATE_FSYS0_DPPHY, rd(GATE_FSYS0_DPPHY) | GATE_MANUAL_CGVAL);
    wr(PMU_USB20_PHY, rd(PMU_USB20_PHY) | 1U);
    wr(PMU_USB3DP_PHY, rd(PMU_USB3DP_PHY) | 1U);
    __asm__ volatile("dsb sy" : : : "memory");
    hw_wait_us(10);

    /* phy_exynos_usb_v3p1_enable(), VER_03_0_0 */
    q_ch_on();
    rmw(r + CLKRST, 0, CR_LINK_SW_RST);                        /* link reset */
    hw_wait_us(10);
    rmw(r + CLKRST, CR_LINK_SW_RST, 0);
    rmw(r + CLKRST, 0, CR_PHY_SW_RST | CR_PHY_RST_SEL);        /* PHY reset high */
    rmw(r + HSP_TEST, HSP_TEST_SIDDQ, 0);                      /* PHY power: SIDDQ off */
    rmw(r + UTMI, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN, 0);
    rmw(r + HSP, 0, HSP_EN_UTMISUSPEND | HSP_COMMONONN);       /* common_block_disable = 1 */
    hw_wait_us(100);
    rmw(r + CLKRST, CR_PHY_SW_RST | CR_PORT_RST, CR_PHY_RST_SEL);   /* PHY reset low */
    /* no VBUS pad on this board: VBUS forced valid, bus filter bypassed */
    rmw(r + LINK_CTRL, 0, LC_BUS_FILTER_BYPASS);
    rmw(r + UTMI, 0, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
    rmw(r + HSP, 0, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);
    rmw(r + LINK_PORT, 0, LP_HUB_PORT_SEL_OCD_U3 | LP_HUB_PORT_SEL_OCD_U2);
    /* phy_exynos_usb3p1_rewa_ready(): ReWA parked */
    rmw(r + REWA_ENABLE, 1U, 0);
    rmw(r + HSREWA_CTRL, 1U << 24, 1U << 28);
    wr(r + HSREWA_HSTK, 0x1U);
    wr(r + HSREWA_REFTO, 0xff00U);
    rmw(r + HSREWA_INT1_MASK, 0, (1U << 18) | (1U << 17) | (1U << 16) | 7U);
    /* phy_exynos_usb_v3p1_pipe_ovrd(): the USB3 side forced idle, its PMA powered down */
    rmw(r + LINK_CTRL, LC_PIPE3_FORCE_PHY_STATUS, LC_PIPE3_FORCE_EN | LC_PIPE3_FORCE_RX_ELEC_IDLE);
    rmw(r + COMBO_PMA, 0, PMA_LOW_PWRN);
    /* phy_exynos_usb_v3p1_tune(), device column */
    rmw(r + HSP_TUNE, TUNE_MASK, TUNE_VAL);
    __asm__ volatile("dsb sy" : : : "memory");
    hw_wait_us(100);
    snap(snap_after);

    /* the block answers: the PHY is out of SIDDQ and the UTMI overrides stuck */
    if ((rd(r + HSP_TEST) & HSP_TEST_SIDDQ) ||
        (rd(r + UTMI) & (UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID)) != (UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID)) {
        *why = "USBCON did not take the PHY settings";
        return -1;
    }
    *why = 0;
    return 0;
}
