/* SPDX-License-Identifier: GPL-2.0-only
 * MAX77705 (HSI2C 0x14360000, USI_CMGP03) for the loader's USB device mode.
 *
 * The MAX77705's embedded MCU ("USBC", I2C 0x25) owns the D+/D- switches between the USB-C
 * connector and the AP. Samsung's MUIC driver sets them with opcode CONTROL1_WRITE (0x06) and
 * COM_USB (0x09: D+ and D- to the AP) whenever a host is attached, and its shutdown path resets
 * the MCU, so after a reboot from TWRP (or any vendor kernel) the switches are open and the Mac
 * sees nothing however well the DWC3 is set up. Same fix as our Linux port
 * (tools/linux/port/src/soc/s9p-max77705-muic.c): send COM_USB, and again whenever VBUS returns.
 * Opcode protocol: opcode + data into AP_DATAOUT0.. (0x21..), a write to AP_DATAOUT32 (0x41)
 * ends the command, the answer is in AP_DATAIN0.. (0x51..).
 *
 * The HSI2C driver is EngineerMode's (em.c bi_*: the same bus, the same S-Boot timing snapshot),
 * copied rather than shared so neither file depends on the other's state; this copy also writes.
 * Never touches the clear-on-read interrupt registers.
 */
#include "max77705_muic.h"
#include "hw.h"

#define BI 0x14360000UL
#define A_USBC 0x25U
#define A_FG   0x36U

#define R_HW_REV       0x00U
#define R_FW_REV       0x01U
#define R_USBC_STATUS1 0x06U
#define R_BC_STATUS    0x08U
#define  BC_VBUSDET    0x80U
#define R_CC_STATUS0   0x0aU
#define R_AP_DATAOUT0  0x21U
#define R_AP_DATAOUT32 0x41U
#define R_AP_DATAIN0   0x51U
#define OP_CONTROL1_READ  0x05U
#define OP_CONTROL1_WRITE 0x06U
#define COM_USB 0x09U
#define NOT_IN 0xffffffffU

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

static uint32_t fs1, fs2, fs3;
static int bus_state;           /* 0 untouched, 1 up, -1 no answer */

static struct {
    int routed, written, ctrl1, last_vbus, reroutes;
    unsigned lows, since_in;
    uint8_t hw, fw, bc, usbc1, cc0, answer[4];
    uint32_t errors;
} m;

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

/* One auto-mode message (count <= 32), with or without STOP (em.c bi_msg). */
static int msg(unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop)
{
    int xfer_done = 0;
    unsigned done = 0, settle = 0;
    unsigned trig = count < 8 ? count : 8;
    wr(BI + 0x04, 3U | (trig << 4) | (trig << 16));
    wr(BI + 0x70, addr << 10);
    wr(BI + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    wr(BI + 0x20, 0x80U);
    wr(BI + 0x44, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    wr(BI + 0x44, rd(BI + 0x44) | 0x80000000U);
    for (unsigned us = 0; us < 50000; ++us) {
        uint32_t irq = rd(BI + 0x24);
        if (irq)
            wr(BI + 0x24, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(rd(BI + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)rd(BI + 0x38);
        } else {
            while (done < count && !(rd(BI + 0x30) & (1U << 7)))
                wr(BI + 0x34, bytes[done++]);
        }
        if (!receive && !stop && done == count && (rd(BI + 0x30) & (1U << 8)))
            ++settle;
        if (done == count && (xfer_done || (receive && stop) || settle > 200)) {
            wr(BI + 0x20, 0);
            if (!stop)
                return 1;
            for (unsigned n = 0; n < 10000; ++n) {
                if (!(rd(BI + 0x50) & (1U << 17)))
                    return 1;
                hw_wait_us(1);
            }
            break;
        }
        hw_wait_us(1);
    }
    wr(BI + 0x20, 0);
    bus_reset();
    ++m.errors;
    return 0;
}

static int i2c_read(unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    for (unsigned a = 0; a < 2; ++a) {
        if (msg(addr, 0, &reg, 1, 0) && msg(addr, 1, data, len, 1))
            return 1;
        bus_reset();
        if (msg(addr, 0, &reg, 1, 1) && msg(addr, 1, data, len, 1))
            return 1;
        bus_reset();
        hw_wait_us(300);
    }
    return 0;
}

/* reg, then n data bytes, one message with STOP */
static int i2c_write(unsigned addr, uint8_t reg, const uint8_t *data, unsigned n)
{
    uint8_t b[33];
    if (n > 32)
        return 0;
    b[0] = reg;
    for (unsigned i = 0; i < n; ++i)
        b[1 + i] = data[i];
    for (unsigned a = 0; a < 2; ++a) {
        if (msg(addr, 0, b, n + 1, 1))
            return 1;
        hw_wait_us(300);
    }
    return 0;
}

static int bus_init(void)
{
    if (bus_state > 0)
        return 1;
    /* CMU_CMGP gates (MANUAL + CG_VAL) and QCH for USI_CMGP03, USI mode I2C, GPM14/15 (em.c) */
    static const uint32_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };
    for (unsigned i = 0; i < 4; ++i)
        wr(0x14200000UL + gates[i], rd(0x14200000UL + gates[i]) | 0x00300000U);
    wr(0x14203050UL, rd(0x14203050UL) | 3U);
    wr(0x14212030UL, 4U);
    for (uintptr_t g = 0x14220180UL; g <= 0x142201a0UL; g += 0x20) {
        wr(g, (rd(g) & ~0xfU) | 0x2U);
        wr(g + 8, rd(g + 8) & ~0xfU);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    if (!bus_state) {
        fs1 = rd(BI + 0x60);
        fs2 = rd(BI + 0x64);
        fs3 = rd(BI + 0x68);
        if (!((fs3 >> 16) & 0xffU)) {                  /* nothing from S-Boot: 200 MHz, 400 kHz */
            fs3 = (fs3 & ~0x00ff0000U) | (0x21U << 16);
            fs2 = (fs2 & ~0xffU) | 0xe0U;
            fs1 = (fs1 & ~0x00ff0000U) | (0xf0U << 16);
        }
    }
    bus_reset();
    bus_state = i2c_read(A_USBC, R_HW_REV, &m.hw, 1) ? 1 : -1;
    if (bus_state > 0)
        (void)i2c_read(A_USBC, R_FW_REV, &m.fw, 1);
    return bus_state > 0;
}

/* max77705_i2c_opcode_write(): opcode + data, then the end marker */
static int opcode_write(uint8_t op, const uint8_t *data, unsigned n)
{
    uint8_t b[4];
    uint8_t z = 0;
    b[0] = op;
    for (unsigned i = 0; i < n && i < 3; ++i)
        b[1 + i] = data[i];
    if (!i2c_write(A_USBC, R_AP_DATAOUT0, b, 1 + (n < 3 ? n : 3)))
        return 0;
    return i2c_write(A_USBC, R_AP_DATAOUT32, &z, 1);
}

/* CONTROL1 as the MCU reports it (opcode 0x05; the answer is the opcode, then the value), or
 * -1 without an answer in 100 ms (Scion's hakios.max77705.c usbsw_read) */
static int control1_read(void)
{
    if (!opcode_write(OP_CONTROL1_READ, 0, 0))
        return -1;
    for (unsigned i = 0; i < 10; ++i) {
        hw_wait_ms(10);
        if (i2c_read(A_USBC, R_AP_DATAIN0, m.answer, 4) && m.answer[0] == OP_CONTROL1_READ)
            return m.answer[1];
    }
    return -1;
}

/* COM_USB written and read back, four tries (the MCU's own detection may still be running) */
static int close_switch(void)
{
    uint8_t v = COM_USB;
    for (unsigned a = 0; a < 4; ++a) {
        if (opcode_write(OP_CONTROL1_WRITE, &v, 1)) {
            m.written = 1;
            hw_wait_ms(50);
            m.ctrl1 = control1_read();
            if (m.ctrl1 == COM_USB)
                return 1;
        }
        hw_wait_ms(200);
    }
    return 0;
}

int muic_route_usb(void)
{
    if (bus_state < 0)
        bus_state = 0;                                 /* retried each call */
    if (!bus_init())
        return 0;
    (void)i2c_read(A_USBC, R_BC_STATUS, &m.bc, 1);
    (void)i2c_read(A_USBC, R_USBC_STATUS1, &m.usbc1, 1);
    (void)i2c_read(A_USBC, R_CC_STATUS0, &m.cc0, 1);
    m.last_vbus = !!(m.bc & BC_VBUSDET);
    m.since_in = m.last_vbus ? 1U : NOT_IN;            /* re-close on the schedule below too */
    m.ctrl1 = -1;
    m.routed = close_switch();
    return m.routed || m.written;
}

/* polls (500 ms each) after VBUS came in at which the switch is closed again: 0, 0.5, 1.5, 3, 6 s */
static int reclose_at(unsigned n)
{
    return n == 0 || n == 1 || n == 3 || n == 6 || n == 12;
}

int muic_poll(void)
{
    uint8_t bc;
    int back = 0;
    if (bus_state <= 0)
        return -1;
    if (!i2c_read(A_USBC, R_BC_STATUS, &bc, 1))
        return -1;
    m.bc = bc;
    if (bc & BC_VBUSDET) {
        m.lows = 0;
        if (!m.last_vbus) {
            m.since_in = 0;
            back = 1;
        }
        m.last_vbus = 1;
    } else if (++m.lows >= 2) {                         /* two readings in a row: the cable is out */
        m.last_vbus = 0;
        m.since_in = NOT_IN;
    }
    if (m.since_in != NOT_IN) {
        if (reclose_at(m.since_in)) {
            if (close_switch())
                m.routed = 1;
            ++m.reroutes;
        }
        m.since_in = m.since_in >= 12 ? NOT_IN : m.since_in + 1;
    }
    return back ? 2 : m.last_vbus;
}

int muic_battery(unsigned *mv, unsigned *pct)
{
    uint8_t b[2];
    if (bus_state <= 0 && !bus_init())
        return 0;
    if (!i2c_read(A_FG, 0x06, b, 2))                  /* RepSOC: 1/256 % */
        return 0;
    *pct = b[1];
    if (!i2c_read(A_FG, 0x09, b, 2))                  /* VCELL: 78.125 uV */
        return 0;
    *mv = (unsigned)((b[0] | (unsigned)b[1] << 8) * 5U / 64U);
    return 1;
}

void muic_diag(struct tb *t)
{
    tb_s(t, "max77705: bus ");
    tb_s(t, bus_state > 0 ? "up" : bus_state < 0 ? "no answer" : "untouched");
    tb_s(t, ", usbc hw ");
    tb_hex(t, m.hw, 2);
    tb_s(t, " fw ");
    tb_hex(t, m.fw, 2);
    tb_s(t, "\nmax77705: bc_status ");
    tb_hex(t, m.bc, 2);
    tb_s(t, m.bc & BC_VBUSDET ? " (vbus)" : " (no vbus)");
    tb_s(t, " usbc_status1 ");
    tb_hex(t, m.usbc1, 2);
    tb_s(t, " cc_status0 ");
    tb_hex(t, m.cc0, 2);
    tb_s(t, "\nmax77705: d+/d- ");
    tb_s(t, m.routed ? "routed to the AP (CONTROL1 read back 0x09)" :
            m.written ? "COM_USB written, NOT read back" : "NOT routed");
    tb_s(t, ", CONTROL1 ");
    if (m.ctrl1 < 0)
        tb_s(t, "no answer");
    else
        tb_hex(t, (unsigned)m.ctrl1, 2);
    tb_s(t, ", last answer ");
    for (unsigned i = 0; i < 4; ++i) {
        tb_hex(t, m.answer[i], 2);
        tb_c(t, ' ');
    }
    tb_s(t, "re-routes ");
    tb_u(t, (unsigned)m.reroutes);
    tb_s(t, " i2c errors ");
    tb_u(t, m.errors);
    tb_c(t, '\n');
}
