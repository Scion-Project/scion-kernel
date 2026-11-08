/* SPDX-License-Identifier: GPL-2.0-only
 * The battery for the boot menus (batt.h): MAX77705 on HSI2C 0x14360000 (USI_CMGP03), read only.
 *
 * The bus code is EngineerMode's (em.c "MAX77705 on HSI2C 0x14360000", em-research-battery.md),
 * copied so em.c can move on its own: the same platform writes (CMU_CMGP gates and QCH, USI mode
 * I2C, GPM14/15 function 2), the timing registers S-Boot left snapshotted before the first
 * controller reset and written back, register reads with a repeated start falling back to STOP
 * between the halves. Differences: every wait is bounded by the timer (a message gives up after
 * BATT_MSG_US, a STOP after BATT_STOP_US), a register gets one try of each kind, and after
 * BATT_FAILS readings in a row fail the reader stops touching the bus for the rest of the boot,
 * so a dead bus can never stall a menu.
 *
 * Never writes a MAX77705 register (S-Boot's charger watchdog is not fed: see batt.h); never reads
 * the clear-on-read interrupt registers (PMIC 0x22/0x24, charger 0xB0).
 */
#include <stdint.h>
#include "batt.h"
#include "hw.h"

#define BI 0x14360000UL
#define BI_PMIC 0x66U
#define BI_FG 0x36U
#define BI_CHG 0x69U

#define BATT_MSG_US 3000U       /* one message (a 3-byte read is ~75 us at 400 kHz) */
#define BATT_STOP_US 2000U      /* the bus going idle after STOP */
#define BATT_SETTLE_US 200U     /* a write without STOP: TX FIFO empty this long = sent */
#define BATT_FAILS 3U           /* readings failing in a row before the reader gives up */

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

static uint64_t tpu;            /* timer ticks per us */

static uint64_t now_us(void)
{
    return hw_ticks() / tpu;
}

static uint32_t fs1, fs2, fs3;
static int state;               /* 0 untouched, 1 up, -1 given up */
static unsigned fails;

static void bus_reset(void)
{
    wr(BI + 0x20, 0);
    wr(BI + 0xc4, 0);
    wr(BI + 0x00, rd(BI + 0x00) | 0x80000000U);
    wr(BI + 0x00, rd(BI + 0x00) & ~0x80000000U);
    wr(BI + 0x68, fs3);
    wr(BI + 0x64, fs2);
    wr(BI + 0x60, fs1);
    wr(BI + 0x00, 0x8U);
    wr(BI + 0x08, 0x00ffffffU);
    wr(BI + 0x48, rd(BI + 0x48) & ~0x80000000U);
    wr(BI + 0x40, rd(BI + 0x40) | 0x80000000U);
    wr(BI + 0x24, rd(BI + 0x24) & 0xfffU);
}

/* One auto-mode message (count <= 8), with or without STOP; 0 on NACK, error or timeout (the
 * controller is reset). */
static int msg(unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop)
{
    int xfer_done = 0;
    unsigned done = 0;
    uint64_t t0 = now_us(), settle = 0;
    wr(BI + 0x04, 3U | (count << 4) | (count << 16));
    wr(BI + 0x70, addr << 10);
    wr(BI + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    wr(BI + 0x20, 0x80U);
    wr(BI + 0x44, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    wr(BI + 0x44, rd(BI + 0x44) | 0x80000000U);
    for (;;) {
        uint64_t t = now_us();
        if (t - t0 > BATT_MSG_US)
            break;
        uint32_t irq = rd(BI + 0x24);
        if (irq)
            wr(BI + 0x24, irq);
        if (irq & 0xf00U)
            break;              /* NACK, lost arbitration, timeout */
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(rd(BI + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)rd(BI + 0x38);
        } else {
            while (done < count && !(rd(BI + 0x30) & (1U << 7)))
                wr(BI + 0x34, bytes[done++]);
        }
        /* without STOP the controller may not flag TRANSFER_DONE: accept a TX FIFO that stayed
         * empty for BATT_SETTLE_US */
        if (!receive && !stop && done == count && (rd(BI + 0x30) & (1U << 8))) {
            if (!settle)
                settle = t + 1;
        } else {
            settle = 0;
        }
        if (done == count && (xfer_done || (receive && stop) || (settle && t + 1 - settle > BATT_SETTLE_US))) {
            wr(BI + 0x20, 0);
            if (!stop)
                return 1;
            uint64_t s0 = now_us();
            while (now_us() - s0 < BATT_STOP_US)
                if (!(rd(BI + 0x50) & (1U << 17)))
                    return 1;
            break;
        }
        hw_wait_us(1);
    }
    wr(BI + 0x20, 0);
    bus_reset();
    return 0;
}

static int reg_read(unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    if (msg(addr, 0, &reg, 1, 0) && msg(addr, 1, data, len, 1))
        return 1;
    bus_reset();
    if (msg(addr, 0, &reg, 1, 1) && msg(addr, 1, data, len, 1))
        return 1;
    bus_reset();
    return 0;
}

static int fg_word(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    if (!reg_read(BI_FG, reg, b, 2))
        return 0;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

int batt_init(void)
{
    if (state)
        return state > 0;
    state = -1;
    uint64_t f = hw_freq();
    tpu = f >= 1000000U ? f / 1000000U : 26U;
    /* CMU_CMGP gates (MANUAL + CG_VAL) and QCH for USI_CMGP03 (S-Boot set them already) */
    static const uint32_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };
    for (unsigned i = 0; i < 4; ++i)
        wr(0x14200000UL + gates[i], rd(0x14200000UL + gates[i]) | 0x00300000U);
    wr(0x14203050UL, rd(0x14203050UL) | 3U);
    wr(0x14212030UL, 4U);                              /* USI mode: I2C */
    for (uintptr_t g = 0x14220180UL; g <= 0x142201a0UL; g += 0x20) {   /* GPM14/15[0]: func 2, no pull */
        wr(g, (rd(g) & ~0xfU) | 0x2U);
        wr(g + 8, rd(g + 8) & ~0xfU);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    fs1 = rd(BI + 0x60);
    fs2 = rd(BI + 0x64);
    fs3 = rd(BI + 0x68);
    if (!((fs3 >> 16) & 0xffU)) {                      /* nothing from S-Boot: 200 MHz, 400 kHz */
        fs3 = (fs3 & ~0x00ff0000U) | (0x21U << 16);
        fs2 = (fs2 & ~0xffU) | 0xe0U;
        fs1 = (fs1 & ~0x00ff0000U) | (0xf0U << 16);
    }
    bus_reset();
    uint8_t id = 0;
    uint16_t st = 0, ir = 0;
    if (!reg_read(BI_PMIC, 0x00, &id, 1))
        return 0;                                      /* no answer: given up for this boot */
    /* the fuel gauge: Status bit 3 (Bst) = no battery; 0xB2 high byte 0xFF = its init failed */
    if (!fg_word(0x00, &st) || (st & 8U) || (fg_word(0xb2, &ir) && (ir >> 8) == 0xffU))
        return 0;
    state = 1;
    return 1;
}

static int read_all(struct batt_info *b)
{
    uint16_t v;
    uint8_t c;
    if (!fg_word(0x06, &v))                            /* RepSOC: % = raw / 256 */
        return 0;
    b->pct = (uint8_t)((v >> 8) > 100U ? 100U : (v >> 8));
    if (!reg_read(BI_CHG, 0xb2, &c, 1))                /* CHG_INT_OK */
        return 0;
    b->plugged = (c & 0x60U) ? 1 : 0;                  /* b6 CHGIN_OK, b5 WCIN_OK */
    if (reg_read(BI_CHG, 0xb4, &c, 1))                 /* CHG_DETAILS_01 [3:0] CHG_DTLS */
        b->charging = (c & 0xfU) <= 2U ? 1 : 0;
    if (fg_word(0x09, &v))                             /* VCELL: 78.125 uV per bit */
        b->mv = (uint16_t)((uint32_t)v * 5U / 64U);
    if (fg_word(0x0a, &v))                             /* Current: 0.3125 mA per bit (5 mOhm) */
        b->ma = (int16_t)((int32_t)(int16_t)v * 5 / 16);
    return 1;
}

int batt_read(struct batt_info *b)
{
    b->valid = b->pct = b->plugged = b->charging = 0;
    b->mv = 0;
    b->ma = 0;
    if (!batt_init())
        return 0;
    if (!read_all(b)) {
        b->pct = b->plugged = b->charging = 0;
        if (++fails >= BATT_FAILS)
            state = -1;
        return 0;
    }
    fails = 0;
    b->valid = 1;
    return 1;
}

int batt_percent(void)
{
    uint16_t v;
    if (!batt_init() || !fg_word(0x06, &v))
        return -1;
    return (v >> 8) > 100U ? 100 : (int)(v >> 8);
}

int batt_mv(void)
{
    uint16_t v;
    if (!batt_init() || !fg_word(0x09, &v))
        return -1;
    return (int)((uint32_t)v * 5U / 64U);
}

int batt_plugged(void)
{
    uint8_t c;
    if (!batt_init() || !reg_read(BI_CHG, 0xb2, &c, 1))
        return -1;
    return (c & 0x60U) ? 1 : 0;
}
