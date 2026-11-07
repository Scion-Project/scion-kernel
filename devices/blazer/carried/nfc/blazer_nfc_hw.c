/* Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, research/ios26-nfc-r1/receipts/0001-hk-nfc1.patch), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md. */
#if defined(BLAZER)
/* SPDX-License-Identifier: MIT
 * Written here. Facts: FDT st21nfc@8 and its pins (research/ios26-dualboot-r8/truth/fdt.dts), the bus votes and I2C block exactly as
 * blazer_haptic_hw.c (physical, haptic-1-r1 and later; same controller), the pad page layout of blazer_pcie1.c/blazer_modem.c (physical),
 * and ST's Android NFC HAL (hardware/st/nfc, Apache-2.0; facts only): 3-byte header read then the payload as a second read, 0x7E idle
 * bytes, 30 ms reset pulse, IRQ active high, CORE_RESET_NTF offsets. Notes: research/ios26-nfc-r1/DESIGN.md.
 *
 * Pins (FDT gpio-line-names index = pad page, as for the Wi-Fi pads):
 *   chip enable  reset-gpios <hsion 1>   AP2NFC_CHIP_EN_1V2    pinctrl@C580000 page 1 (stock sleep state output-high: high = on)
 *   power state  pidle-gpios <hsion 3>   NFC2AP_PWR_STATE_1V2  page 3, read only
 *   host IRQ     irq-gpios   <gdmc 3>    NFC2AP_HOST_IRQ_1V2   pinctrl@6480000 page 3, read only; used only if its input is enabled
 * Never touched: the eSE (SPI, its own enable on hsion 6), the 19.2 MHz clock pads (CPM CLKOUT0, OPEN), the I2C pads, any other address.
 * The bus carries the USB repeater (0x3e) and the haptics (0x43): address 0x08 only, no bus clear, no controller reset, the haptics' lock.
 */
#include "blazer_nfc_hw.h"

#define MBA_MAGIC_PA    0x05250000ULL
#define MBA_MAGIC       0x004d4241u
#define MBA_PAGE        0x05272000ULL
#define MBA_GO          0x00u
#define MBA_CTL         0x04u
#define MBA_ST          0x08u
#define MBA_MSG         0x100u
#define MBA_CTL_BIT24   0x01000000u
#define MBA_REPLY_BIT   0x00400000u
#define MBA_POLLS       1001u
#define MBA_POLL_US     200u

/* The haptics' votes, frame for frame (blazer_haptic_hw.c votes()) */
#define LPB_GET         0x05000003u
#define LPB_ID_LSIOE    28u
#define LPB_ST_ON       2u
#define LPCM_HDR        0x08000000u
#define CFG_CLK_PREPARE 0x00040202u
#define I2C_CLK_PREPARE 0x00140202u
#define I2C_CLK_RATE    0x03140202u
#define I2C_RST_DEASSERT 0x010c0204u
#define CLK_GATE        1u
#define I2C_CLK_HZ      200000000u
#define SIP_PD_CONTROL  0x82000701u
#define SIP_CLK_CONTROL 0x82000702u
#define SSWRP_LSIOE     0x1cu
#define PD_ON           1u
#define LPCM_LSIOE      2u
#define GPIO_CLK        0u

#define P_PARAM         0x00u
#define P_DMUX          0x04u
#define P_TX            0x08u
#define P_RX            0x10u
#define PARAM_IE        (1u << 8)
#define TX_PUPD_MASK    3u
#define TX_DOUT         4u
#define TX_OE           8u
#define PAD_STRIDE      0x1000u
#define LSIOE_PADS      0x3a200000ULL
#define PAD_SCL         16u
#define PAD_SDA         17u
#define DMUX_CLI10      2u
#define HSION_PADS      0x0c580000ULL   /* FDT hsion_pinctrl, 12 pages; csr_access_pd HSIO_N (USB's, on-at-init) */
#define PAD_EN          1u
#define PAD_PWR         3u
#define GDMC_PADS       0x06480000ULL   /* FDT gdmc_pinctrl, 21 pages, no power domain */
#define PAD_IRQ         3u
#define DMUX_GPIO       1u              /* f0 hsion_gpio, as lsioe_gpio and hsios_gpio (INFERENCE for this block; stage 1 reads it) */
#define EN_OFF_US       30000u          /* ST HAL: "a 30ms pulse on RESET line" */
#define EN_BOOT_US      10000u

#define CLI_CONF        0x3a330000ULL
#define CLI_IPTYPE_I2C  1u
#define CLI_PAD_PD      1u
#define CLI_PAD_SPU     3u
#define I2C             0x3a331000ULL
#define IC_CON          0x00u
#define IC_TAR          0x04u
#define IC_DATA_CMD     0x10u
#define IC_FS_HCNT      0x1cu
#define IC_FS_LCNT      0x20u
#define IC_INTR_MASK    0x30u
#define IC_RAW_INTR     0x34u
#define IC_RX_TL        0x38u
#define IC_TX_TL        0x3cu
#define IC_CLR_TX_ABRT  0x54u
#define IC_CLR_STOP     0x60u
#define IC_ENABLE       0x6cu
#define IC_STATUS       0x70u
#define IC_ABRT_SOURCE  0x80u
#define IC_EN_STATUS    0x9cu
#define IC_COMP_VERSION 0xf8u
#define IC_COMP_TYPE    0xfcu
#define COMP_TYPE_DW    0x44570140u
#define CON_MASTER_FAST 0x65u
#define FS_HCNT_400K    152u
#define FS_LCNT_400K    339u
#define INTR_ABRT       (1u << 6)
#define INTR_STOP       (1u << 9)
#define ST_ACTIVITY     1u
#define ST_TFNF         2u
#define ST_RFNE         8u
#define ST_MASTER       0x20u
#define CMD_READ        0x100u
#define CMD_STOP        0x200u
#define BUS_POLLS       2000u
#define RD_OUTSTANDING  8u
#define CHIP_ADDR       0x08u   /* FDT st21nfc@8 */
#define ABRT_ADDR_NOACK 1u

#define NCI_IDLE        0x7eu
#define MT_RSP          2u
#define MT_NTF          3u
#define RSP_TIMEOUT_MS  1000u
#define NTF_TIMEOUT_MS  1000u         /* the HAL allows 10 s for the whole open */
#define PKT_BOUND       64u           /* packets per wait, so a chatty chip cannot hold the bus forever */
#define IRQ_POLL_US     1000u
#define READ_POLL_US    5000u
#define WRITE_TRIES     3u
#define WRITE_RETRY_US  4000u

/* The only bytes ever sent to the chip, and the stage each needs */
enum { C_CORE_RESET, C_CORE_INIT, C_MODE_ON, C_DISCOVER, C_DEACTIVATE, C_COUNT };
static const struct { uint32_t stage, len; uint8_t b[12]; const char *name; } cmds[C_COUNT] = {
	[C_CORE_RESET] = { NF_STAGE_INIT, 4, { 0x20, 0x00, 0x01, 0x01 }, "CORE_RESET" },                       /* reset config */
	[C_CORE_INIT] = { NF_STAGE_INIT, 5, { 0x20, 0x01, 0x02, 0x00, 0x00 }, "CORE_INIT" },                    /* NCI 2.0 form */
	[C_MODE_ON] = { NF_STAGE_DISCOVER, 5, { 0x2f, 0x02, 0x02, 0x02, 0x01 }, "ST_NFC_MODE_ON" },            /* HAL propNfcModeSetCmdOn */
	[C_DISCOVER] = { NF_STAGE_DISCOVER, 12, { 0x21, 0x03, 0x09, 0x04, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x06, 0x01 }, "RF_DISCOVER" },
	[C_DEACTIVATE] = { NF_STAGE_DISCOVER, 4, { 0x21, 0x06, 0x01, 0x00 }, "RF_DEACTIVATE" },                /* to idle */
};

static int
halt(struct nf *n, uint32_t code, uint32_t detail)
{
	if (n->stop == NF_OK) {
		n->stop = code;
		n->stop_detail = detail;
		n->ops.log(n->ops.ctx, "stop %s, detail 0x%x\n", nf_stop_name(code), detail);
	}
	return (int)n->stop;
}

const char *
nf_stop_name(uint32_t stop)
{
	static const char *const names[] = {
		"ok", "refused-pa", "refused-cmd", "refused-frame", "fault", "mba", "lsioe-off", "smc", "lpcm", "rate", "cli", "not-designware",
		"pad-not-i2c", "enable-pad", "bus-busy", "bus", "nak", "no-ntf", "bad-packet", "nci-status", "loader-mode", "stage",
	};
	return stop < sizeof(names) / sizeof(names[0]) ? names[stop] : "?";
}

static uint64_t pad(uint64_t base, uint32_t p, uint32_t reg) { return base + (uint64_t)p * PAD_STRIDE + reg; }

static bool
pa_allowed(uint64_t pa, bool write)
{
	if (pa >= MBA_PAGE && pa < MBA_PAGE + MBA_MSG + 16) {
		return true;
	}
	if (pa >= I2C && pa < I2C + 0x100) {
		return true;
	}
	if (pa == CLI_CONF + 4 || pa == CLI_CONF + 8) {
		return true;
	}
	if (pa == pad(HSION_PADS, PAD_EN, P_DMUX) || pa == pad(HSION_PADS, PAD_EN, P_TX)) {
		return true;
	}
	if (write) {
		return false;
	}
	return pa == MBA_MAGIC_PA || pa == CLI_CONF || pa == pad(LSIOE_PADS, PAD_SCL, P_DMUX) || pa == pad(LSIOE_PADS, PAD_SDA, P_DMUX) ||
	       pa == pad(HSION_PADS, PAD_EN, P_PARAM) || pa == pad(HSION_PADS, PAD_EN, P_RX) ||
	       pa == pad(HSION_PADS, PAD_PWR, P_PARAM) || pa == pad(HSION_PADS, PAD_PWR, P_DMUX) || pa == pad(HSION_PADS, PAD_PWR, P_TX) ||
	       pa == pad(HSION_PADS, PAD_PWR, P_RX) || pa == pad(GDMC_PADS, PAD_IRQ, P_PARAM) || pa == pad(GDMC_PADS, PAD_IRQ, P_DMUX) ||
	       pa == pad(GDMC_PADS, PAD_IRQ, P_TX) || pa == pad(GDMC_PADS, PAD_IRQ, P_RX);
}

static int
rd(struct nf *n, uint64_t pa, uint32_t *v)
{
	if (n->stop != NF_OK) {
		return (int)n->stop;
	}
	if (!pa_allowed(pa, false)) {
		return halt(n, NF_STOP_REFUSED_PA, (uint32_t)pa);
	}
	if (n->ops.read32(n->ops.ctx, pa, v)) {
		return halt(n, NF_STOP_FAULT, (uint32_t)pa);
	}
	return 0;
}

static int
wr(struct nf *n, uint64_t pa, uint32_t v)
{
	if (n->stop != NF_OK) {
		return (int)n->stop;
	}
	if (!pa_allowed(pa, true)) {
		return halt(n, NF_STOP_REFUSED_PA, (uint32_t)pa);
	}
	if (n->ops.write32(n->ops.ctx, pa, v)) {
		return halt(n, NF_STOP_FAULT, (uint32_t)pa);
	}
	return 0;
}

/* ---- stage 1: the pads, reads only. The HSIO_N block first, so a fault in the GDMC block is told apart ---- */

static int
pads_read(struct nf *n)
{
	uint32_t pdmux = 0, ptx = 0, iparam = 0, idmux = 0, itx = 0, pparam = 0;

	if (rd(n, pad(HSION_PADS, PAD_EN, P_PARAM), &n->en_param) || rd(n, pad(HSION_PADS, PAD_EN, P_DMUX), &n->en_dmux) ||
	    rd(n, pad(HSION_PADS, PAD_EN, P_TX), &n->en_tx) || rd(n, pad(HSION_PADS, PAD_EN, P_RX), &n->en_rx)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "pad chip-enable (hsion 1) param 0x%x dmux 0x%x tx 0x%x rx 0x%x\n", n->en_param, n->en_dmux, n->en_tx, n->en_rx);
	if (rd(n, pad(HSION_PADS, PAD_PWR, P_PARAM), &pparam) || rd(n, pad(HSION_PADS, PAD_PWR, P_DMUX), &pdmux) ||
	    rd(n, pad(HSION_PADS, PAD_PWR, P_TX), &ptx) || rd(n, pad(HSION_PADS, PAD_PWR, P_RX), &n->pwr_rx)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "pad power-state (hsion 3) param 0x%x dmux 0x%x tx 0x%x rx 0x%x\n", pparam, pdmux, ptx, n->pwr_rx);
	n->ops.log(n->ops.ctx, "next gdmc pad 3 at 0x%llx\n", (unsigned long long)pad(GDMC_PADS, PAD_IRQ, 0));
	if (rd(n, pad(GDMC_PADS, PAD_IRQ, P_PARAM), &iparam) || rd(n, pad(GDMC_PADS, PAD_IRQ, P_DMUX), &idmux) ||
	    rd(n, pad(GDMC_PADS, PAD_IRQ, P_TX), &itx) || rd(n, pad(GDMC_PADS, PAD_IRQ, P_RX), &n->irq_rx)) {
		return (int)n->stop;
	}
	n->irq_param = iparam;
	n->irq_usable = (iparam & PARAM_IE) != 0 && !(itx & TX_OE);
	/* On if driven high, or (not driven) the line reads high through a pull: either way it gets the 30 ms off first */
	n->en_was_on = ((n->en_tx & TX_OE) && (n->en_tx & TX_DOUT)) || (!(n->en_tx & TX_OE) && (n->en_param & PARAM_IE) && (n->en_rx & 1u));
	n->ops.log(n->ops.ctx, "pad host-irq (gdmc 3) param 0x%x dmux 0x%x tx 0x%x rx 0x%x: irq %s\n", iparam, idmux, itx, n->irq_rx,
	    n->irq_usable ? "readable" : "not readable, polled reads instead");
	n->ops.log(n->ops.ctx, "chip enable %s\n", (n->en_tx & TX_OE) ? (n->en_was_on ? "driven high (on)" : "driven low (off)") :
	    n->en_was_on ? "not driven, reads high (on)" : "not driven (off)");
	return 0;
}

/* ---- votes: blazer_haptic_hw.c's, unchanged ---- */

static bool
frame_allowed(uint32_t w0, uint32_t w1, uint32_t w2)
{
	if (w0 == LPB_GET) {
		return w1 == LPB_ID_LSIOE && w2 == 0;
	}
	if (w0 != LPCM_HDR) {
		return false;
	}
	return ((w1 == CFG_CLK_PREPARE || w1 == I2C_CLK_PREPARE) && w2 == CLK_GATE) || ((w1 == I2C_CLK_RATE || w1 == I2C_RST_DEASSERT) && w2 == 0);
}

static int
mba_frame(struct nf *n, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t r[4])
{
	uint32_t v = 0, ctl = 0;
	unsigned k = 0;

	if (wr(n, MBA_PAGE + MBA_ST, 1) || rd(n, MBA_PAGE + MBA_ST, &v) || rd(n, MBA_PAGE + MBA_CTL, &ctl) || wr(n, MBA_PAGE + MBA_CTL, ctl | MBA_CTL_BIT24) ||
	    wr(n, MBA_PAGE + MBA_MSG, w0) || wr(n, MBA_PAGE + MBA_MSG + 4, w1) || wr(n, MBA_PAGE + MBA_MSG + 8, w2) || wr(n, MBA_PAGE + MBA_MSG + 12, 0) ||
	    wr(n, MBA_PAGE + MBA_GO, 1) || rd(n, MBA_PAGE + MBA_ST, &v)) {
		return (int)n->stop;
	}
	while (!(v & 1u)) {
		if (++k >= MBA_POLLS) {
			return halt(n, NF_STOP_MBA, w1);
		}
		n->ops.delay_us(n->ops.ctx, MBA_POLL_US);
		if (rd(n, MBA_PAGE + MBA_ST, &v)) {
			return (int)n->stop;
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		if (rd(n, MBA_PAGE + MBA_MSG + 4 * i, &r[i])) {
			return (int)n->stop;
		}
	}
	if (wr(n, MBA_PAGE + MBA_ST, 1) || rd(n, MBA_PAGE + MBA_ST, &v)) {
		return (int)n->stop;
	}
	return r[0] != (w0 | MBA_REPLY_BIT) ? halt(n, NF_STOP_MBA, r[0]) : 0;
}

static int
mba(struct nf *n, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t r[4])
{
	int rc;

	if (!frame_allowed(w0, w1, w2)) {
		return halt(n, NF_STOP_REFUSED_FRAME, w1);
	}
	if (n->ops.cpm_enter != NULL) {
		n->ops.cpm_enter(n->ops.ctx);
	}
	rc = mba_frame(n, w0, w1, w2, r);
	if (n->ops.cpm_exit != NULL) {
		n->ops.cpm_exit(n->ops.ctx);
	}
	return rc;
}

static int
lpcm_ok(struct nf *n, uint32_t w1, uint32_t w2)
{
	uint32_t r[4] = {0};

	if (mba(n, LPCM_HDR, w1, w2, r)) {
		return (int)n->stop;
	}
	return r[1] != 0 ? halt(n, NF_STOP_LPCM, w1) : 0;
}

static int
smc_vote(struct nf *n, uint32_t fn, uint32_t a1, uint32_t a2, uint32_t a3)
{
	uint64_t r[2] = { ~0ull, ~0ull };

	n->ops.smc(n->ops.ctx, fn, a1, a2, a3, r);
	return (r[0] != 0 || r[1] != 0) ? halt(n, NF_STOP_SMC, fn) : 0;
}

static int
cli_pads(struct nf *n)
{
	uint32_t w[3] = {0}, v = 0;

	for (unsigned i = 0; i < 3; i++) {
		if (rd(n, CLI_CONF + 4 * i, &w[i])) {
			return (int)n->stop;
		}
	}
	n->ops.log(n->ops.ctx, "cli was %u,%u,%u\n", w[0], w[1], w[2]);
	if (w[0] != CLI_IPTYPE_I2C) {
		return halt(n, NF_STOP_CLI, w[0]);
	}
	for (unsigned i = 1; i < 3; i++) {
		if (w[i] == CLI_PAD_SPU) {
			continue;
		}
		if (w[i] != CLI_PAD_PD) {
			return halt(n, NF_STOP_CLI, i << 16 | w[i]);
		}
		if (wr(n, CLI_CONF + 4 * i, CLI_PAD_SPU) || rd(n, CLI_CONF + 4 * i, &v)) {
			return (int)n->stop;
		}
		if (v != CLI_PAD_SPU) {
			return halt(n, NF_STOP_CLI, i << 16 | v);
		}
	}
	return 0;
}

static int
votes(struct nf *n)
{
	uint32_t v = 0, r[4] = {0};

	if (rd(n, MBA_MAGIC_PA, &v)) {
		return (int)n->stop;
	}
	if (v != MBA_MAGIC) {
		return halt(n, NF_STOP_MBA, v);
	}
	if (mba(n, LPB_GET, LPB_ID_LSIOE, 0, r)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "lsio_e %u (2 on)\n", r[1]);
	if (r[1] != LPB_ST_ON) {
		return halt(n, NF_STOP_LSIOE_OFF, r[1]);
	}
	if (smc_vote(n, SIP_PD_CONTROL, SSWRP_LSIOE, 0, PD_ON) || smc_vote(n, SIP_CLK_CONTROL, LPCM_LSIOE, GPIO_CLK, 1) ||
	    lpcm_ok(n, CFG_CLK_PREPARE, CLK_GATE) || lpcm_ok(n, I2C_CLK_PREPARE, CLK_GATE) || cli_pads(n)) {
		return (int)n->stop;
	}
	if (mba(n, LPCM_HDR, I2C_CLK_RATE, 0, r)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "i2c clock %u Hz\n", r[1]);
	if ((int32_t)r[1] <= 0 || r[1] > I2C_CLK_HZ) {
		return halt(n, NF_STOP_RATE, r[1]);
	}
	return lpcm_ok(n, I2C_RST_DEASSERT, 0);
}

static int
block_check(struct nf *n)
{
	uint32_t v = 0, w = 0;

	if (rd(n, I2C + IC_COMP_TYPE, &v) || rd(n, I2C + IC_COMP_VERSION, &w)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "controller type 0x%x version 0x%x\n", v, w);
	if (v != COMP_TYPE_DW) {
		return halt(n, NF_STOP_NOT_DW, v);
	}
	if (rd(n, pad(LSIOE_PADS, PAD_SCL, P_DMUX), &v) || rd(n, pad(LSIOE_PADS, PAD_SDA, P_DMUX), &w)) {
		return (int)n->stop;
	}
	return (v != DMUX_CLI10 || w != DMUX_CLI10) ? halt(n, NF_STOP_PAD_NOT_I2C, v << 8 | w) : 0;
}

/* ---- chip enable: TX only (dout before oe), the mux only from "none" to GPIO, and back to the found words at the end ---- */

static int
en_set(struct nf *n, bool high)
{
	uint32_t tx = (n->en_tx & ~(TX_PUPD_MASK | TX_DOUT)) | (high ? TX_DOUT : 0);

	if (!n->en_driven && n->en_dmux != DMUX_GPIO) {
		if (n->en_dmux != 0) {
			return halt(n, NF_STOP_EN_PAD, n->en_dmux);    /* another function owns the pad: not ours */
		}
		if (wr(n, pad(HSION_PADS, PAD_EN, P_DMUX), DMUX_GPIO)) {
			return (int)n->stop;
		}
		n->en_dmux_set = true;
	}
	if (wr(n, pad(HSION_PADS, PAD_EN, P_TX), tx) || wr(n, pad(HSION_PADS, PAD_EN, P_TX), tx | TX_OE)) {
		return (int)n->stop;
	}
	n->en_driven = true;
	if (n->en_param & PARAM_IE) {
		uint32_t rx = 0;

		if (rd(n, pad(HSION_PADS, PAD_EN, P_RX), &rx)) {
			return (int)n->stop;
		}
		if ((rx & 1u) != (high ? 1u : 0u)) {
			return halt(n, NF_STOP_EN_PAD, 0x100u | rx);
		}
	}
	return 0;
}

/* Power-up as the HAL's open: off for 30 ms if it was on, then on. The chip then sends CORE_RESET_NTF by itself */
static int
power_up(struct nf *n)
{
	if (n->en_was_on) {
		if (en_set(n, false)) {
			return (int)n->stop;
		}
		n->ops.delay_us(n->ops.ctx, EN_OFF_US);
	}
	if (en_set(n, true)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "chip enable high%s\n", n->en_was_on ? " after 30 ms low" : "");
	n->ops.delay_us(n->ops.ctx, EN_BOOT_US);
	return 0;
}

static void
en_restore(struct nf *n)
{
	uint32_t saved = n->stop, detail = n->stop_detail;

	if (!n->en_driven || n->cfg.keep == 1) {
		return;
	}
	n->stop = NF_OK;
	if (n->en_was_on) {
		(void)wr(n, pad(HSION_PADS, PAD_EN, P_TX), n->en_tx);   /* as found: on */
	} else {
		(void)wr(n, pad(HSION_PADS, PAD_EN, P_TX), n->en_tx);
		if (n->en_dmux_set) {
			(void)wr(n, pad(HSION_PADS, PAD_EN, P_DMUX), n->en_dmux);
		}
	}
	n->ops.log(n->ops.ctx, "chip enable restored to tx 0x%x (%s)\n", n->en_tx, n->stop == NF_OK ? "ok" : "failed");
	n->en_driven = false;
	if (saved != NF_OK || n->stop == NF_OK) {
		n->stop = saved;
		n->stop_detail = detail;
	}
}

/* ---- the bus ---- */

static int
wait_disabled(struct nf *n)
{
	uint32_t v = 1;

	for (unsigned i = 0; i < BUS_POLLS; i++) {
		if (rd(n, I2C + IC_EN_STATUS, &v)) {
			return (int)n->stop;
		}
		if (!(v & 1u)) {
			return 0;
		}
		n->ops.delay_us(n->ops.ctx, 10);
	}
	return halt(n, NF_STOP_BUS, v);
}

static int
bus_up(struct nf *n)
{
	uint32_t st = 0, en = 0, con = 0, tar = 0;

	if (rd(n, I2C + IC_ENABLE, &en) || rd(n, I2C + IC_STATUS, &st) || rd(n, I2C + IC_CON, &con) || rd(n, I2C + IC_TAR, &tar)) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "bus found: enable 0x%x status 0x%x con 0x%x tar 0x%x\n", en, st, con, tar);
	if (st & (ST_ACTIVITY | ST_MASTER)) {
		return halt(n, NF_STOP_BUS_BUSY, st);
	}
	if (wr(n, I2C + IC_ENABLE, 0) || wait_disabled(n) || wr(n, I2C + IC_CON, CON_MASTER_FAST) || wr(n, I2C + IC_TAR, CHIP_ADDR) ||
	    wr(n, I2C + IC_FS_HCNT, FS_HCNT_400K) || wr(n, I2C + IC_FS_LCNT, FS_LCNT_400K) || wr(n, I2C + IC_INTR_MASK, 0) || wr(n, I2C + IC_RX_TL, 0) ||
	    wr(n, I2C + IC_TX_TL, 0) || wr(n, I2C + IC_ENABLE, 1) || rd(n, I2C + IC_CLR_TX_ABRT, &st) || rd(n, I2C + IC_CLR_STOP, &st)) {
		return (int)n->stop;
	}
	n->bus_on = true;
	return 0;
}

static void
bus_down(struct nf *n)
{
	uint32_t saved = n->stop, detail = n->stop_detail;

	if (!n->bus_on) {
		return;
	}
	n->stop = NF_OK;
	(void)wr(n, I2C + IC_ENABLE, 0);
	(void)wait_disabled(n);
	n->bus_on = false;
	if (saved != NF_OK || n->stop == NF_OK) {
		n->stop = saved;
		n->stop_detail = detail;
	}
}

/* 0 done, -1 aborted (abort_source says why: bit 0 the address was not acked), else stopped */
static int
xfer_end(struct nf *n)
{
	uint32_t raw = 0, st = 0, junk = 0;

	for (unsigned i = 0; i < BUS_POLLS; i++) {
		if (rd(n, I2C + IC_RAW_INTR, &raw)) {
			return (int)n->stop;
		}
		if (raw & (INTR_STOP | INTR_ABRT)) {
			if (raw & INTR_ABRT) {
				if (rd(n, I2C + IC_ABRT_SOURCE, &n->abort_source) || rd(n, I2C + IC_CLR_TX_ABRT, &junk)) {
					return (int)n->stop;
				}
			}
			if (rd(n, I2C + IC_CLR_STOP, &junk)) {
				return (int)n->stop;
			}
			return (raw & INTR_ABRT) ? -1 : 0;
		}
		n->ops.delay_us(n->ops.ctx, 10);
	}
	(void)rd(n, I2C + IC_STATUS, &st);
	return halt(n, NF_STOP_BUS, st);
}

static int
push(struct nf *n, uint32_t word)
{
	uint32_t st = 0;

	for (unsigned i = 0; i < BUS_POLLS; i++) {
		if (rd(n, I2C + IC_STATUS, &st)) {
			return (int)n->stop;
		}
		if (st & ST_TFNF) {
			return wr(n, I2C + IC_DATA_CMD, word);
		}
		n->ops.delay_us(n->ops.ctx, 10);
	}
	return halt(n, NF_STOP_BUS, st);
}

/* One read transaction of count bytes: at most RD_OUTSTANDING read commands ahead of the RX FIFO. 0, -1 aborted, or stopped */
static int
read_bytes(struct nf *n, uint8_t *buf, uint32_t count)
{
	uint32_t pushed = 0, got = 0, st = 0, raw = 0, b = 0, junk = 0;
	unsigned idle = 0;

	while (got < count) {
		while (pushed < count && pushed - got < RD_OUTSTANDING) {
			if (push(n, CMD_READ | (pushed + 1 == count ? CMD_STOP : 0))) {
				return (int)n->stop;
			}
			pushed++;
		}
		if (rd(n, I2C + IC_STATUS, &st)) {
			return (int)n->stop;
		}
		if (st & ST_RFNE) {
			if (rd(n, I2C + IC_DATA_CMD, &b)) {
				return (int)n->stop;
			}
			buf[got++] = (uint8_t)b;
			idle = 0;
			continue;
		}
		if (rd(n, I2C + IC_RAW_INTR, &raw)) {
			return (int)n->stop;
		}
		if (raw & INTR_ABRT) {
			if (rd(n, I2C + IC_ABRT_SOURCE, &n->abort_source) || rd(n, I2C + IC_CLR_TX_ABRT, &junk) || rd(n, I2C + IC_CLR_STOP, &junk)) {
				return (int)n->stop;
			}
			return -1;
		}
		if (++idle >= BUS_POLLS) {
			return halt(n, NF_STOP_BUS, st);
		}
		n->ops.delay_us(n->ops.ctx, 10);
	}
	return xfer_end(n);
}

static int
send(struct nf *n, unsigned c)
{
	if (c >= C_COUNT || n->cfg.stage < cmds[c].stage) {
		return halt(n, NF_STOP_REFUSED_CMD, c);
	}
	for (unsigned t = 0; t < WRITE_TRIES; t++) {
		int rc;

		for (uint32_t i = 0; i < cmds[c].len; i++) {
			if (push(n, cmds[c].b[i] | (i + 1 == cmds[c].len ? CMD_STOP : 0))) {
				return (int)n->stop;
			}
		}
		rc = xfer_end(n);
		if (rc == 0) {
			n->ops.log(n->ops.ctx, "tx %s (%u bytes)\n", cmds[c].name, cmds[c].len);
			return 0;
		}
		if (n->stop != NF_OK) {
			return (int)n->stop;
		}
		n->ops.delay_us(n->ops.ctx, WRITE_RETRY_US);
	}
	return halt(n, NF_STOP_NAK, n->abort_source);
}

static void
log_pkt(struct nf *n)
{
	static const char hx[] = "0123456789abcdef";
	char s[3 * 40 + 8];
	uint32_t k = 0, show = n->pkt.len < 40 ? n->pkt.len : 40;

	for (uint32_t i = 0; i < show; i++) {
		s[k++] = ' ';
		s[k++] = hx[n->pkt.b[i] >> 4];
		s[k++] = hx[n->pkt.b[i] & 15];
	}
	if (show < n->pkt.len) {
		s[k++] = ' ';
		s[k++] = '.';
		s[k++] = '.';
	}
	s[k] = 0;
	n->ops.log(n->ops.ctx, "rx %u bytes:%s\n", n->pkt.len, s);
}

/* One NCI packet into n->pkt. 0 got one, 1 none in timeout_ms, else stopped. With a readable IRQ the bus is read only when it is high */
static int
read_packet(struct nf *n, uint32_t timeout_ms)
{
	uint64_t waited = 0, limit = (uint64_t)timeout_ms * 1000u;
	uint8_t h[3];

	for (;;) {
		bool ready = true;
		int rc;

		if (n->irq_usable) {
			uint32_t rx = 0;

			if (rd(n, pad(GDMC_PADS, PAD_IRQ, P_RX), &rx)) {
				return (int)n->stop;
			}
			ready = (rx & 1u) != 0;
		}
		if (ready) {
			rc = read_bytes(n, h, 3);
			if (rc > 0) {
				return rc;
			}
			if (rc == 0) {
				uint32_t have = 3, skip = 0;

				n->addr_ack = true;
				if (h[0] == NCI_IDLE) {
					skip = h[1] != NCI_IDLE ? 1 : h[2] != NCI_IDLE ? 2 : 3;       /* the HAL's shift: idle bytes before a header */
				}
				if (skip < 3) {
					for (uint32_t i = skip; i < 3; i++) {
						n->pkt.b[i - skip] = h[i];
					}
					have = 3 - skip;
					if (skip && read_bytes(n, n->pkt.b + have, skip) != 0) {
						return n->stop != NF_OK ? (int)n->stop : halt(n, NF_STOP_BAD_PACKET, skip);
					}
					n->pkt.len = 3;
					if (n->pkt.b[2] != 0) {
						rc = read_bytes(n, n->pkt.b + 3, n->pkt.b[2]);
						if (rc != 0) {
							return n->stop != NF_OK ? (int)n->stop : halt(n, NF_STOP_BAD_PACKET, n->abort_source);
						}
						n->pkt.len += n->pkt.b[2];
					}
					log_pkt(n);
					return 0;
				}
			} else if (n->irq_usable && n->irq_naks++ < 3) {
				n->ops.log(n->ops.ctx, "irq high, read not acked (abort 0x%x)\n", n->abort_source);
			}
		}
		if (waited >= limit) {
			return 1;
		}
		n->ops.delay_us(n->ops.ctx, n->irq_usable ? IRQ_POLL_US : READ_POLL_US);
		waited += n->irq_usable ? IRQ_POLL_US : READ_POLL_US;
	}
}

static uint32_t mt(const struct nf *n) { return n->pkt.b[0] >> 5; }
static uint32_t gid(const struct nf *n) { return n->pkt.b[0] & 0x0fu; }
static uint32_t oid(const struct nf *n) { return n->pkt.b[1] & 0x3fu; }

static void
hexs(char *s, const uint8_t *b, uint32_t len)
{
	static const char hx[] = "0123456789abcdef";

	for (uint32_t i = 0; i < len; i++) {
		s[2 * i] = hx[b[i] >> 4];
		s[2 * i + 1] = hx[b[i] & 15];
	}
	s[2 * len] = 0;
}

/* RF technology-specific parameters (NCI 2.0 table 54-ish): an ID for poll A (NFCID1) and poll V (UID); others by raw bytes only */
static void
tag_params(struct nf *n, uint32_t mode, const uint8_t *p, uint32_t plen)
{
	char s[2 * 10 + 1];

	n->uid_len = 0;
	if (mode == 0x00 && plen >= 3 && p[2] <= 10 && 3u + p[2] <= plen) {
		n->uid_len = p[2];
		memcpy(n->uid, p + 3, p[2]);
	} else if (mode == 0x06 && plen >= 10) {
		n->uid_len = 8;
		for (unsigned i = 0; i < 8; i++) {
			n->uid[i] = p[9 - i];   /* UID is sent least significant byte first */
		}
	} else if (mode == 0x01 && plen >= 5 && p[0] >= 5 && 1u + p[0] <= plen) {
		n->uid_len = 4;         /* NFCID0 in SENSB_RES bytes 1..4 */
		memcpy(n->uid, p + 2, 4);
	}
	hexs(s, n->uid, n->uid_len);
	n->ops.log(n->ops.ctx, "tag: %s, protocol 0x%x, id %s\n", mode == 0x00 ? "nfc-a" : mode == 0x01 ? "nfc-b" : mode == 0x02 ? "nfc-f" : mode == 0x06 ? "nfc-v" : "other",
	    n->last_proto, n->uid_len ? s : "(not parsed)");
}

/* Notifications wherever they come. 0, or stopped */
static int
on_ntf(struct nf *n)
{
	const uint8_t *b = n->pkt.b;
	uint32_t len = n->pkt.len;

	n->ntf_count++;
	if (gid(n) == 0 && oid(n) == 0) {      /* CORE_RESET_NTF */
		if (len >= 4 && ((b[2] == 0x39 && b[3] == 0xa1) || (b[2] == 0x41 && b[3] == 0xa2))) {
			n->ops.log(n->ops.ctx, "chip in loader mode (reset ntf length 0x%x, trigger 0x%x): no NCI\n", b[2], b[3]);
			return halt(n, NF_STOP_LOADER, b[3]);
		}
		if (len < 8) {
			return halt(n, NF_STOP_BAD_PACKET, len);
		}
		n->reset_trigger = b[3];
		n->nci_version = b[5];
		n->manuf = b[6];
		if (len >= 14) {
			n->hw_version = b[8];
			n->fw_version = (uint32_t)b[10] << 24 | (uint32_t)b[11] << 16 | (uint32_t)b[12] << 8 | b[13];
		}
		if (len >= 17) {
			n->loader_version = (uint32_t)b[14] << 16 | (uint32_t)b[15] << 8 | b[16];
		}
		n->ops.log(n->ops.ctx, "chip st21nfc fw 0x%08x hw 0x%02x loader 0x%06x nci 0x%02x manuf 0x%02x trigger 0x%02x config 0x%02x\n",
		    n->fw_version, n->hw_version, n->loader_version, n->nci_version, n->manuf, n->reset_trigger, b[4]);
		return 0;
	}
	if (gid(n) == 0 && (oid(n) == 7 || oid(n) == 8)) {
		n->ops.log(n->ops.ctx, "core %s error ntf, status 0x%x\n", oid(n) == 7 ? "generic" : "interface", len > 3 ? b[3] : 0);
		return 0;
	}
	if (gid(n) == 1) {
		n->rf_ntfs++;
		if (oid(n) == 5 && len >= 10) {        /* RF_INTF_ACTIVATED_NTF */
			uint32_t plen = b[9];

			n->tags++;
			n->last_proto = b[5];
			n->last_tech = b[6];
			n->ops.log(n->ops.ctx, "rf activated: id %u interface 0x%x protocol 0x%x mode 0x%x\n", b[3], b[4], b[5], b[6]);
			tag_params(n, b[6], b + 10, 10u + plen <= len ? plen : len - 10);
			return 0;
		}
		if (oid(n) == 3 && len >= 7) {         /* RF_DISCOVER_NTF */
			uint32_t plen = b[6];

			n->tags++;
			n->last_proto = b[4];
			n->last_tech = b[5];
			n->ops.log(n->ops.ctx, "rf discovered: id %u protocol 0x%x mode 0x%x\n", b[3], b[4], b[5]);
			tag_params(n, b[5], b + 7, 7u + plen <= len ? plen : len - 7);
			return 0;
		}
		if (oid(n) == 6) {
			n->ops.log(n->ops.ctx, "rf deactivated ntf, type 0x%x reason 0x%x\n", len > 3 ? b[3] : 0, len > 4 ? b[4] : 0);
			return 0;
		}
		if (oid(n) == 7) {
			n->ops.log(n->ops.ctx, "rf field info: %s\n", len > 3 && (b[3] & 1u) ? "field on" : "field off");
			return 0;
		}
	}
	return 0;
}

/* A command and its response; notifications on the way are taken as they come. 0 with *status, or stopped */
static int
transact(struct nf *n, unsigned c, uint32_t *status)
{
	uint64_t waited = 0;
	uint32_t pkts = 0;

	if (send(n, c)) {
		return (int)n->stop;
	}
	while (waited < RSP_TIMEOUT_MS && pkts < PKT_BOUND) {
		int rc = read_packet(n, 50);

		if (rc > 0 && n->stop != NF_OK) {
			return (int)n->stop;
		}
		if (rc == 1) {
			waited += 50;
			continue;
		}
		pkts++;
		if (mt(n) == MT_NTF) {
			if (on_ntf(n)) {
				return (int)n->stop;
			}
			continue;
		}
		if (mt(n) == MT_RSP && gid(n) == (cmds[c].b[0] & 0x0fu) && oid(n) == (cmds[c].b[1] & 0x3fu)) {
			*status = n->pkt.len > 3 ? n->pkt.b[3] : 0xffu;
			n->ops.log(n->ops.ctx, "rsp %s status 0x%x\n", cmds[c].name, *status);
			return *status == 0 ? 0 : halt(n, NF_STOP_NCI_STATUS, c << 8 | *status);
		}
		n->ops.log(n->ops.ctx, "unexpected packet mt %u gid %u oid %u while waiting for %s\n", mt(n), gid(n), oid(n), cmds[c].name);
	}
	return halt(n, NF_STOP_NO_NTF, 0x100u | c);
}

/* Notifications only, for up to ms: until want_oid (gid/oid) arrives if given. 0 got it (or none wanted), 1 not, else stopped */
static int
drain(struct nf *n, uint32_t ms, int want_gid, int want_oid)
{
	uint64_t waited = 0;
	uint32_t pkts = 0;

	while (waited < ms && pkts < PKT_BOUND) {
		int rc = read_packet(n, 50);

		if (rc > 0 && n->stop != NF_OK) {
			return (int)n->stop;
		}
		if (rc == 1) {
			waited += 50;
			continue;
		}
		pkts++;
		if (mt(n) == MT_NTF) {
			if (on_ntf(n)) {
				return (int)n->stop;
			}
			if ((int)gid(n) == want_gid && (int)oid(n) == want_oid) {
				return 0;
			}
		}
	}
	return want_gid < 0 ? 0 : 1;
}

static int
probe(struct nf *n)
{
	int rc;

	if (power_up(n) || bus_up(n)) {
		return (int)n->stop;
	}
	rc = drain(n, NTF_TIMEOUT_MS, 0, 0);
	if (rc > 1 || n->stop != NF_OK) {
		return (int)n->stop;
	}
	n->ops.log(n->ops.ctx, "address 0x%02x %s, power-up reset ntf %s\n", CHIP_ADDR, n->addr_ack ? "acked" : "never acked", rc == 0 ? "received" : "not received");
	if (!n->addr_ack) {
		return halt(n, NF_STOP_NAK, n->abort_source);
	}
	return rc == 0 ? 0 : halt(n, NF_STOP_NO_NTF, 0);
}

static int
nci_init(struct nf *n)
{
	uint32_t st = 0;

	if (transact(n, C_CORE_RESET, &st)) {
		return (int)n->stop;
	}
	if (drain(n, NTF_TIMEOUT_MS, 0, 0)) {
		return n->stop != NF_OK ? (int)n->stop : halt(n, NF_STOP_NO_NTF, 0x200u);
	}
	if (transact(n, C_CORE_INIT, &st)) {
		return (int)n->stop;
	}
	n->init_status = st;
	n->ops.log(n->ops.ctx, "nci init ok: nci 0x%02x, fw 0x%08x, init rsp %u bytes\n", n->nci_version, n->fw_version, n->pkt.len);
	return 0;
}

static int
discover(struct nf *n)
{
	uint32_t st = 0, listen = n->cfg.listen_s;

	if (transact(n, C_MODE_ON, &st) || transact(n, C_DISCOVER, &st)) {
		return (int)n->stop;
	}
	n->rf_on = true;
	n->ops.log(n->ops.ctx, "discovery on (poll a/b/f/v), listening %u s\n", listen);
	for (uint32_t s = 0; s < listen && n->tags == 0; s++) {
		if (drain(n, 1000, -1, -1) || n->stop != NF_OK) {
			break;
		}
	}
	n->ops.log(n->ops.ctx, "discovery: %s after %u rf notifications\n", n->tags ? "tag found" : "no tag", n->rf_ntfs);
	return 0;
}

/* RF off before the bus or the pad are let go, whatever failed */
static void
rf_off(struct nf *n)
{
	uint32_t saved = n->stop, detail = n->stop_detail, st = 0;

	if (!n->rf_on || !n->bus_on) {
		return;
	}
	n->stop = NF_OK;
	if (transact(n, C_DEACTIVATE, &st) == 0) {
		(void)drain(n, 200, 1, 6);
		n->rf_on = false;
	}
	if (saved != NF_OK || n->stop == NF_OK) {
		n->stop = saved;
		n->stop_detail = detail;
	}
}

void
nf_init(struct nf *n, const struct nf_ops *ops, const struct nf_cfg *cfg)
{
	memset(n, 0, sizeof(*n));
	n->ops = *ops;
	n->cfg = *cfg;
	if (n->cfg.listen_s == 0 || n->cfg.listen_s > 60u) {
		n->cfg.listen_s = 20u;
	}
}

int
nf_run(struct nf *n)
{
	if (n->cfg.stage < NF_STAGE_PADS || n->cfg.stage > NF_STAGE_DISCOVER) {
		return halt(n, NF_STOP_STAGE, n->cfg.stage);
	}
	if (pads_read(n) || n->cfg.stage == NF_STAGE_PADS) {
		return (int)n->stop;
	}
	if (n->ops.bus_enter != NULL) {
		n->ops.bus_enter(n->ops.ctx);
		n->bus_held = true;
	}
	if (!votes(n) && !block_check(n) && !probe(n) && n->cfg.stage >= NF_STAGE_INIT && !nci_init(n) && n->cfg.stage >= NF_STAGE_DISCOVER) {
		(void)discover(n);
	}
	rf_off(n);
	bus_down(n);
	en_restore(n);
	if (n->bus_held) {
		n->ops.bus_exit(n->ops.ctx);
		n->bus_held = false;
	}
	return (int)n->stop;
}
#endif /* BLAZER */
