/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader: see sd.h. A polling, read-only driver for the Exynos 9810's
 * card controller (dwmmc2, DesignWare MSHC 2.70a at 0x11500000, Exynos7 type: CLKSEL at 0xa8,
 * 64-bit FIFO at 0x200), after what mainline Linux needed on this phone (NOTES.md 2026-10-27):
 *
 *   clocks   CMU gates opened by hand (bit 20 manual, bit 21 on, as s9p-clkgate.c): CMU top
 *            GATE_CLKCMU_FSYS1_MMC_CARD 0x1a242070, FSYS1 ACLK 0x11402030 and SDCLKIN 0x11402034.
 *            PLL_MMC (~806 MHz) / (DIV_CLKCMU_FSYS1_MMC_CARD 0x1a241864 + 1) / (CLKSEL div 3 + 1)
 *            = the controller's bus clock (~200 MHz when the CMU divider is /1, as Linux sets it;
 *            the loader leaves the divider alone and computes with it)
 *   pins     GPF1-0..5 function 2 (pinctrl_fsys1 0x11430000), no pulls, drive 4 (clock) / 3
 *   card     detect GPA1-5 active low; rails S2MPS18 LDO31 2.8 V + LDO2 3.3 V (hw_sd_rails,
 *            which power-cycles the card), 3.3 V signalling only: no UHS
 *   TMOUT    always ~0: with the default every read ended in a data read timeout
 *
 * Init at 400 kHz: CMD0, CMD8, ACMD41 until ready, CMD2, CMD3, CMD7, ACMD6 (4 bit), then 25 MHz.
 * Reads: CMD17 / CMD18 with auto stop, the FIFO emptied by the CPU (no DMA, no interrupts), every
 * wait bounded by the generic timer. A failed read is followed by sd_recover() (controller + FIFO
 * reset, clock reload, CMD12 with stop_abort, CMD13 until tran; Linux dw_mci_reset +
 * send_stop_abort) before the retry, so no error leaves the controller inside the CMD18. CLKSEL is
 * changed in its timing fields only (Linux dw_mci_exynos_set_clksel_timing); a CMU gate that did
 * not open stops the bring-up before the controller is read. Nothing here ever writes the card.
 */
#include <stdint.h>
#include "sd.h"
#include "hw.h"

#define SD        0x11500000UL
#define CTRL      0x00
#define PWREN     0x04
#define CLKDIV    0x08
#define CLKSRC    0x0c
#define CLKENA    0x10
#define TMOUT     0x14
#define CTYPE     0x18
#define BLKSIZ    0x1c
#define BYTCNT    0x20
#define INTMASK   0x24
#define CMDARG    0x28
#define CMD       0x2c
#define RESP0     0x30
#define RINTSTS   0x44
#define STATUS    0x48
#define FIFOTH    0x4c
#define VERID     0x6c
#define HCON      0x70
#define BMOD      0x80
#define CLKSEL64  0xa8
#define DATA      0x200

#define C_START    (1U << 31)
#define C_HOLD     (1U << 29)
#define C_UPD_CLK  (1U << 21)
#define C_INIT     (1U << 15)
#define C_STOP     (1U << 14)           /* stop_abort_cmd (Linux SDMMC_CMD_STOP) */
#define C_PRV_WAIT (1U << 13)
#define C_AUTOSTOP (1U << 12)
#define C_DATA     (1U << 9)
#define C_CRC      (1U << 8)
#define C_LONG     (1U << 7)
#define C_RESP     (1U << 6)
#define R1         (C_RESP | C_CRC)

#define I_RE   (1U << 1)
#define I_CD   (1U << 2)
#define I_DTO  (1U << 3)
#define I_RCRC (1U << 6)
#define I_DCRC (1U << 7)
#define I_RTO  (1U << 8)
#define I_DRTO (1U << 9)
#define I_HTO  (1U << 10)
#define I_FRUN (1U << 11)
#define I_HLE  (1U << 12)
#define I_SBE  (1U << 13)
#define I_ACD  (1U << 14)
#define I_EBE  (1U << 15)
#define I_DERR (I_DCRC | I_DRTO | I_HTO | I_FRUN | I_SBE | I_EBE)

#define PLL_MMC_HZ 806000000ULL
#define PINCTRL_FSYS1 0x11430000UL
#define ALIVE_GPA1_DAT 0x14050044UL

struct sd_report sd_rep;
static int allowed;

void sd_allow(void)
{
    allowed = 1;
}

int sd_allowed(void)
{
    return allowed;
}

static uint32_t rd(uintptr_t off)
{
    uint32_t v = *(volatile uint32_t *)(SD + off);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t off, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)(SD + off) = v;
}

static uint32_t mrd(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}

static void mwr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

/* Deadline helpers on the generic timer. */
static uint64_t deadline(unsigned ms)
{
    return hw_ticks() + hw_freq() / 1000U * ms;
}

static int expired(uint64_t t)
{
    return hw_ticks() > t;
}

/* Wait until (reg & mask) == want; 0 or -1 on timeout. */
static int wait_reg(uintptr_t off, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t t = deadline(ms);
    while ((rd(off) & mask) != want)
        if (expired(t))
            return -1;
    return 0;
}

/* prv_wait: after the last data transfer (clock changes); without it right after a controller
 * reset, as Linux dw_mci_reset's SDMMC_CMD_UPD_CLK. */
static int update_clock(int prv_wait)
{
    wr(CMDARG, 0);
    wr(CMD, C_START | C_HOLD | C_UPD_CLK | (prv_wait ? C_PRV_WAIT : 0));
    return wait_reg(CMD, C_START, 0, 100);
}

static int set_clock(uint64_t hz)
{
    uint64_t div = (sd_rep.bus_hz + 2 * hz - 1) / (2 * hz);
    if (div > 255)
        div = 255;
    wr(CLKENA, 0);
    if (update_clock(1))
        return -1;
    wr(CLKDIV, (uint32_t)div);
    wr(CLKSRC, 0);
    if (update_clock(1))
        return -1;
    wr(CLKENA, 1);
    if (update_clock(1))
        return -1;
    sd_rep.card_hz = div ? sd_rep.bus_hz / (2 * div) : sd_rep.bus_hz;
    return 0;
}

/* One command; 0, -1 controller stuck, -2 no response, -3 response CRC, -4 response error.
 * A stop (C_STOP) goes out at once, as Linux sends it: no busy wait, no PRV_WAIT. */
static int cmd(unsigned idx, uint32_t arg, uint32_t flags)
{
    int stop = (flags & C_STOP) != 0;
    if (!stop && wait_reg(STATUS, 1U << 9, 0, 500))   /* card busy (R1b of the last command) */
        return -1;
    wr(RINTSTS, 0xffffffffU);
    wr(CMDARG, arg);
    wr(CMD, C_START | C_HOLD | (stop ? 0 : C_PRV_WAIT) | flags | idx);
    if (wait_reg(CMD, C_START, 0, 100))
        return -1;
    uint64_t t = deadline(500);
    uint32_t s;
    while (!((s = rd(RINTSTS)) & (I_CD | I_HLE)))
        if (expired(t))
            return -1;
    sd_rep.last_rintsts = s;
    if (s & I_HLE)
        return -1;
    if (s & I_RTO)
        return -2;
    if ((flags & C_CRC) && (s & I_RCRC))
        return -3;
    if (s & I_RE)
        return -4;
    return 0;
}

static int fail(const char *why)
{
    sd_rep.why = why;
    sd_rep.state = -1;
    sd_rep.last_status = rd(STATUS);
    return 0;
}

static int acmd(unsigned idx, uint32_t arg, uint32_t flags)
{
    int r = cmd(55, sd_rep.rca << 16, R1);
    return r ? r : cmd(idx, arg, flags);
}

int sd_up(void)
{
    if (sd_rep.state)
        return sd_rep.state > 0;
    if (!allowed)
        return 0;
    sd_rep.state = -1;
    sd_rep.present = !(mrd(ALIVE_GPA1_DAT) & (1U << 5));
    if (!sd_rep.present) {
        sd_rep.why = "no card (detect GPA1-5 high)";
        return 0;
    }
    /* clocks, then pins, then power: the controller is touched only after its clocks run */
    for (unsigned i = 0; i < 3; ++i) {
        uintptr_t a = i == 0 ? 0x1a242070UL : i == 1 ? 0x11402030UL : 0x11402034UL;
        mwr(a, mrd(a) | (1U << 20) | (1U << 21));
        sd_rep.gate[i] = mrd(a);
    }
    sd_rep.cmu_div = mrd(0x1a241864UL);
    sd_rep.bus_hz = PLL_MMC_HZ / ((sd_rep.cmu_div & 0xfU) + 1U) / 4U;
    for (unsigned i = 0; i < 3; ++i)               /* an unclocked controller can stall the bus */
        if ((sd_rep.gate[i] & (3U << 20)) != (3U << 20)) {
            sd_rep.why = "clock gates did not open";
            return 0;
        }
    mwr(PINCTRL_FSYS1 + 0x0, (mrd(PINCTRL_FSYS1 + 0x0) & 0xff000000U) | 0x00222222U);
    mwr(PINCTRL_FSYS1 + 0x8, mrd(PINCTRL_FSYS1 + 0x8) & 0xff000000U);
    mwr(PINCTRL_FSYS1 + 0xc, (mrd(PINCTRL_FSYS1 + 0xc) & 0xff000000U) | 0x00333334U);
    sd_rep.rails = hw_sd_rails(&sd_rep.l31, &sd_rep.l2);
    if (sd_rep.rails <= 0)
        return fail("card power (PMIC) refused");
    hw_wait_ms(10);
    sd_rep.verid = rd(VERID);
    sd_rep.hcon = rd(HCON);
    sd_rep.clksel_was = rd(CLKSEL64);
    if ((sd_rep.verid & 0xffffU) != 0x270aU)
        return fail("controller not answering (VERID)");
    if (((sd_rep.hcon >> 7) & 7U) != 2U)
        return fail("FIFO is not 64 bit (HCON)");

    wr(CTRL, rd(CTRL) | 7U);                   /* controller, FIFO and DMA reset */
    if (wait_reg(CTRL, 7U, 0, 100))
        return fail("controller reset");
    wr(BMOD, 0);                               /* no internal DMA */
    wr(CTRL, rd(CTRL) & ~((1U << 25) | (1U << 4)));   /* no IDMAC, no interrupts */
    wr(INTMASK, 0);
    wr(RINTSTS, 0xffffffffU);
    wr(TMOUT, 0xffffffffU);
    wr(FIFOTH, (2U << 28) | (31U << 16) | 32U);
    /* ciu div 3, drive phase 2, sample phase 0; the bits outside SDMMC_CLKSEL_TIMING_MASK stay */
    wr(CLKSEL64, (rd(CLKSEL64) & ~0x07070007U) | (3U << 24) | (2U << 16));
    sd_rep.clksel = rd(CLKSEL64);
    wr(CTYPE, 0);
    wr(PWREN, 1);
    hw_wait_ms(5);
    if (set_clock(400000))
        return fail("clock update (400 kHz)");
    hw_wait_ms(2);

    sd_rep.rca = 0;
    if (cmd(0, 0, C_INIT))
        return fail("CMD0 (go idle)");
    int r = cmd(8, 0x1aa, R1);
    sd_rep.r7 = r ? 0 : rd(RESP0);
    if (r || (sd_rep.r7 & 0xfffU) != 0x1aaU)
        return fail("CMD8 (not an SD 2.0 card at 3.3 V)");
    uint64_t t = deadline(1500);
    for (;;) {
        r = acmd(41, 0x40ff8000U, C_RESP);     /* HCS, 2.7-3.6 V; R3 has no CRC */
        sd_rep.ocr = r ? 0 : rd(RESP0);
        if (!r && (sd_rep.ocr & (1U << 31)))
            break;
        if (expired(t))
            return fail("ACMD41 (card never ready)");
        hw_wait_ms(10);
    }
    sd_rep.high_capacity = (sd_rep.ocr >> 30) & 1U;
    if (cmd(2, 0, C_RESP | C_LONG | C_CRC))
        return fail("CMD2 (CID)");
    for (unsigned i = 0; i < 4; ++i)
        sd_rep.cid[i] = rd(RESP0 + 4 * i);
    if (cmd(3, 0, R1))
        return fail("CMD3 (address)");
    sd_rep.rca = rd(RESP0) >> 16;
    if (cmd(7, sd_rep.rca << 16, R1))
        return fail("CMD7 (select)");
    if (!sd_rep.high_capacity && cmd(16, 512, R1))
        return fail("CMD16 (block length)");
    if (acmd(6, 2, R1))
        return fail("ACMD6 (4-bit bus)");
    wr(CTYPE, 1);
    if (set_clock(25000000))
        return fail("clock update (25 MHz)");
    wr(BLKSIZ, 512);
    sd_rep.why = 0;
    sd_rep.state = 1;
    return 1;
}

/* Up to 256 sectors with one CMD17/18 (auto stop), drained from the FIFO by the CPU. */
static int read_chunk(uint64_t sector, uint32_t n, uintptr_t dst)
{
    wr(CTRL, rd(CTRL) | 2U);                   /* FIFO reset */
    if (wait_reg(CTRL, 2U, 0, 50))
        return -1;
    wr(BLKSIZ, 512);
    wr(BYTCNT, n * 512U);
    uint32_t arg = (uint32_t)(sd_rep.high_capacity ? sector : sector * 512U);
    int r = cmd(n > 1 ? 18 : 17, arg, R1 | C_DATA | (n > 1 ? C_AUTOSTOP : 0));
    if (r)
        return r;
    volatile uint64_t *out = (volatile uint64_t *)dst;
    uint64_t want = (uint64_t)n * 64U, got = 0;   /* 64-bit FIFO words */
    uint64_t t = deadline(200 + n * 2U);
    for (;;) {
        uint32_t s = rd(RINTSTS);
        uint32_t c = (rd(STATUS) >> 17) & 0x1fffU;
        while (c-- && got < want)
            out[got++] = *(volatile uint64_t *)(SD + DATA);
        if (s & I_DERR) {
            sd_rep.last_rintsts = s;
            return -5;
        }
        if ((s & I_DTO) && got >= want)
            break;
        if ((s & I_DTO) && !((rd(STATUS) >> 17) & 0x1fffU)) {
            sd_rep.last_rintsts = s;
            return -6;                         /* transfer over, data short */
        }
        if (expired(t)) {
            sd_rep.last_rintsts = s;
            return -7;
        }
    }
    if (n > 1 && wait_reg(RINTSTS, I_ACD, I_ACD, 100))
        return -8;
    return 0;
}

/*
 * After a failed read_chunk the controller can still be inside the CMD18 (the FIFO full, the card
 * clock stopped by flow control), and the next command would wait for it forever. As Linux after a
 * data error: controller + FIFO reset, RINTSTS cleared, UPD_CLK; CMD12 with stop_abort (a card
 * already in tran does not answer: ignored); CMD13 until tran (4), at most 250 ms. Returns the
 * last card state or -1.
 */
static int sd_recover(void)
{
    sd_rep.recoveries++;
    wr(CTRL, rd(CTRL) | 3U);
    (void)wait_reg(CTRL, 3U, 0, 100);
    wr(RINTSTS, 0xffffffffU);
    (void)update_clock(0);
    (void)cmd(12, 0, R1 | C_STOP);
    (void)wait_reg(STATUS, 1U << 9, 0, 500);   /* R1b busy */
    int state = -1, restopped = 0;
    uint64_t t = deadline(250);
    for (;;) {
        if (cmd(13, sd_rep.rca << 16, R1) == 0) {
            state = (int)((rd(RESP0) >> 9) & 0xfU);
            if (state == 4)
                break;
            if (state == 5 && !restopped) {
                restopped = 1;
                (void)cmd(12, 0, R1 | C_STOP);
                (void)wait_reg(STATUS, 1U << 9, 0, 500);
            }
        }
        if (expired(t))
            break;
        hw_wait_ms(1);
    }
    wr(RINTSTS, 0xffffffffU);
    sd_rep.card_state = state;
    return state;
}

int sd_read(uint64_t sector, uint32_t count, uintptr_t dst)
{
    if (!sd_up())
        return -1;
    while (count) {
        uint32_t n = count > 256U ? 256U : count;
        int r = -1;
        for (int a = 0; a < 3 && r; ++a) {
            r = read_chunk(sector, n, dst);
            if (r) {                               /* the last failure too: the next read starts clean */
                uint32_t ri = sd_rep.last_rintsts;
                sd_rep.last_error = r;
                (void)sd_recover();
                sd_rep.last_rintsts = ri;
            }
        }
        if (r)
            return r;
        sector += n;
        count -= n;
        dst += (uintptr_t)n * 512U;
    }
    return 0;
}

int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (count > 0xffffffffULL / 8U)
        return -1;
    return sd_read(lba * 8U, (uint32_t)(count * 8U), dst);
}

/* Sector 0: the card's MBR. */
static uint64_t mbr[64];

int sd_store_area(uint64_t *first, uint64_t *blocks)
{
    *first = *blocks = 0;
    if (!allowed || sd_read(0, 1, (uintptr_t)mbr))
        return 0;
    const volatile uint8_t *b = (const volatile uint8_t *)mbr;
    if (b[510] != 0x55 || b[511] != 0xaa)
        return 0;
    for (unsigned i = 0; i < 4; ++i) {
        const volatile uint8_t *e = b + 446 + 16 * i;
        uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if (e[4] != 0xda || !lba || (lba & 7U) || len < 64)
            continue;
        *first = lba / 8U;
        *blocks = len / 8U;
        return 1;
    }
    return 0;
}
