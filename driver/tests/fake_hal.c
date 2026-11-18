#define _POSIX_C_SOURCE 200809L
/* A packet-engine model for protocol and output tests. Never touches SPI. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fake_hal.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1250.h"
#include "loragw_sx1302.h"

static bool connected;
static unsigned mode, packet_type, irq, arms;
static uint8_t regs[65536], tag, configured_cr, rf1_mod[4], rf1_packet[6];

static int32_t lines[LGW_TOTALREGS];
static unsigned tx_mode, tx_type, tx_irq, tx_size;
static uint8_t tx_regs[65536], tx_fifo[256], sent[255], sent_size;
static uint8_t injected[255], injected_size;
static bool restoring, tx_started;

static bool fault(const char *name) {
    const char *f = getenv("FAKE_TX_FAULT");
    return f && !strcmp(f, name);
}

size_t fake_last_tx(uint8_t *data, size_t capacity) {
    if (!data || capacity < sent_size) return 0;
    memcpy(data, sent, sent_size);
    return sent_size;
}

bool fake_air(uint8_t payload_tag, unsigned flags) {
    if (mode != 5) return false;
    tag = payload_tag;
    irq = flags;
    mode = 2;
    return true;
}

bool fake_inject_rx(const uint8_t *data, uint8_t size) {
    if (!getenv("FAKE_TX") || !data || !size || !fake_air(0, 0x12)) return false;
    memcpy(injected, data, size);
    injected_size = size;
    return true;
}

static int tx_write(sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(getenv("FAKE_TX") && n && n <= 253);
    switch (op) {
    case SET_STANDBY: tx_mode = 2 + p[0]; break;
    case CALIBRATE: assert(tx_mode == 2); break;
    case SET_RF_FREQUENCY: memcpy(tx_regs + 0x088b, p, 4); break;
    case SET_PACKET_TYPE: tx_type = p[0]; break;
    case WRITE_REGISTER: memcpy(tx_regs + ((p[0] << 8) | p[1]), p + 2, n - 2); break;
    case SET_MODULATION_PARAMS: assert(tx_mode == 3 && n == 4 && !memcmp(p, rf1_mod, 4)); break;
    case SET_PACKET_PARAMS:
        assert(tx_mode == 3 && n == 6 && !memcmp(p, rf1_packet, 3) && !memcmp(p+4, rf1_packet+4, 2));
        tx_size = p[3]; break;
    case SET_PA_CONFIG: assert(n == 4 && p[0] == 2 && p[1] == 2 && !p[2] && p[3] == 1); break;
    case SET_TX_PARAMS: assert(n == 2 && (int8_t)p[0] == -9 && p[1] == SET_RAMP_200U); break;
    case SET_BUFFER_BASE_ADDRESS: case SET_DIO_IRQ_PARAMS: break;
    case CLR_IRQ_STATUS: tx_irq = 0; break;
    case WRITE_BUFFER: assert((size_t)p[0] + n - 1 <= sizeof tx_fifo);
        memcpy(tx_fifo + p[0], p + 1, n - 1); break;
    case SET_TX:
        assert(n == 3 && (p[0] || p[1] || p[2]) && tx_mode == 3 && mode == 3 && tx_size);
        assert(!lines[SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN] && !lines[SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN]);
        assert(!lines[SX1302_REG_AGC_MCU_RF_EN_A_PA_EN] && !lines[SX1302_REG_AGC_MCU_RF_EN_B_PA_EN]);
        assert(!lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL] && tx_regs[0x0587] == regs[0x0587]);
        assert(tx_type == PACKET_TYPE_LORA && !memcmp(tx_regs + 0x088b, regs + 0x088b, 4) &&
               !memcmp(tx_regs + 0x0740, regs + 0x0740, 2));
        tx_started = true;
        if (fault("write")) return -1;
        tx_mode = 6; break;
    default: assert(!"unexpected RF0 write");
    }
    return 0;
}

static int tx_read(sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(getenv("FAKE_TX") && n <= 253);
    uint8_t status = 0x82 | (tx_mode << 4);
    if ((unsigned)op == 0x11) { p[0] = status; p[1] = tx_type; return 0; }
    switch (op) {
    case GET_STATUS: p[0] = status; break;
    case GET_IRQ_STATUS:
        if (tx_mode == 6 && !fault("deadline")) {
            tx_mode = 2; tx_irq = fault("conflict") ? 0x201 : fault("timeout") ? 0x200 : 1;
            status = tx_irq == 1 ? 0xac : 0xa6;
            if (tx_irq == 1) { memcpy(sent, tx_fifo, tx_size); sent_size = tx_size; }
            if (fault("late_done")) {
                struct timespec delay = {.tv_nsec = 20000000};
                while (nanosleep(&delay, &delay)) { }
            }
            if (fault("status")) status = 0xa8;
        }
        p[0] = status; p[1] = tx_irq >> 8; p[2] = tx_irq; break;
    case READ_REGISTER: { unsigned addr = (p[0] << 8) | p[1];
        p[2] = status; memcpy(p + 3, tx_regs + addr, n - 3); break; }
    case READ_BUFFER: { unsigned offset = p[0];
        p[1] = status; memcpy(p + 2, tx_fifo + offset, n - 2);
        if (fault("readback")) p[2] ^= 1;
        break; }
    default: assert(!"unexpected RF0 read");
    }
    return 0;
}

void *lgw_com_target(void) { return connected ? regs : NULL; }
int lgw_connect(lgw_com_type_t t, const char *p) {
    assert(t == LGW_COM_SPI && p);
    connected = true; regs[0x0587] = 0x20; arms = 0; irq = 0; sent_size = injected_size = 0;
    restoring = tx_started = false; memset(lines, 0, sizeof lines);
    puts("HAL connected");
    return getenv("FAIL_CONNECT") ? -1 : 0;
}
int lgw_disconnect(void) { assert(connected); connected = false; puts("HAL disconnected"); return 0; }
int lgw_reg_r(uint16_t reg, int32_t *v) {
    if (reg == SX1302_REG_COMMON_VERSION_VERSION) *v = 0x10;
    else if (reg == SX1302_REG_AGC_MCU_CTRL_MCU_CLEAR || reg == SX1302_REG_ARB_MCU_CTRL_MCU_CLEAR)
        *v = !getenv("BUSY_FW");
    else *v = lines[reg];
    return 0;
}
int lgw_reg_w(uint16_t reg, int32_t val) {
    if (reg == SX1302_REG_AGC_MCU_RF_EN_A_PA_EN || reg == SX1302_REG_AGC_MCU_RF_EN_B_PA_EN)
        assert(val == 0);
    lines[reg] = val;
    return 0;
}
int sx1302_radio_host_ctrl(bool b) { (void)b; return 0; }
int sx1302_radio_reset(uint8_t c, lgw_radio_type_t t) { assert(c <= 1 && t == LGW_RADIO_TYPE_SX1250);
    lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL] = 0;
    if (!c) { tx_mode = 2; tx_irq = 0; tx_regs[0x0587] = fault("pinmode") ? 0 : regs[0x0587]; restoring = tx_started; }
    return 0; }
int sx1302_radio_set_mode(uint8_t c, lgw_radio_type_t t) { assert(c <= 1 && t == LGW_RADIO_TYPE_SX1250); return 0; }
int sx1302_radio_clock_select(uint8_t c) {
    assert(!c && tx_regs[0x0587] == 0x0b);
    if (restoring && fault("clock")) return -1;
    lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL] = 1;
    lines[SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_A_SEL] = 1;
    lines[SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_B_SEL] = 0;
    return 0;
}
int sx1302_pa_lna_lut_configure(struct lgw_conf_board_s *b) { assert(!b->full_duplex); return 0; }
int sx1250_setup(uint8_t c, uint32_t f, bool single) { assert(!c && f && !single); tx_regs[0x0587] = 0x0b; tx_mode = 5; return 0; }
int sx1250_calibrate(uint8_t c, uint32_t f) { assert(f && ((c == 1 && mode == 2) || (!c && tx_mode == 2 && getenv("FAKE_TX")))); return 0; }
void wait_ms(unsigned long t) { (void)t; }

int sx1250_reg_w(sx1250_op_code_t op, uint8_t *p, uint16_t n, uint8_t c) {
    if (!c) return tx_write(op, p, n);
    assert(c == 1 && n);
    switch (op) {
    case SET_STANDBY:
        if (irq && arms == 1 && !getenv("EARLY") && !getenv("FAKE_TX")) irq |= 0x40; /* CRC error becomes visible on freeze. */
        mode = 2 + p[0]; break;
    case CALIBRATE: assert(mode == 2); break;
    case SET_RF_FREQUENCY: memcpy(regs + 0x088b, p, 4); break;
    case SET_PACKET_TYPE: packet_type = p[0]; break;
    case WRITE_REGISTER: memcpy(regs + ((p[0] << 8) | p[1]), p + 2, n - 2); break;
    case CLR_IRQ_STATUS: irq = 0; break;
    case SET_RX:
        assert(n == 3 && !p[0] && !p[1] && !p[2]);
        if (restoring && fault("rearm")) return -1;
        if (restoring) {
            assert(lines[SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN] && lines[SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN]);
            assert(lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL]);
        }
        mode = 5; ++arms; injected_size = 0; restoring = tx_started = false;
        if (arms == 1 && getenv("EARLY")) assert(fake_air(7, 0x12));
        if (getenv("FALSE_STANDBY")) mode = 2;
        break;
    case SET_MODULATION_PARAMS:
        assert(n == sizeof rf1_mod); memcpy(rf1_mod, p, n);
        configured_cr = p[2];
        if (getenv("EXPECTED_BW")) assert(p[1] == atoi(getenv("EXPECTED_BW")));
        if (getenv("ALT_PROFILE")) assert(n == 4 && p[0] == 12 && p[1] == 5 && p[2] == 1 && p[3] == 1);
        break;
    case SET_PACKET_PARAMS:
        assert(n == sizeof rf1_packet); memcpy(rf1_packet, p, n);
        if (getenv("IMPLICIT")) assert(p[2] == 1 && p[3] == 7 && p[4] == 1);
        if (getenv("ALT_PROFILE")) assert(n == 6 && !p[0] && p[1]==12 && !p[2] && p[3]==255 && !p[4] && p[5]);
        break;
    case SET_BUFFER_BASE_ADDRESS: case SET_DIO_IRQ_PARAMS: break;
    default: assert(!"unexpected radio write opcode");
    }
    return 0;
}

int sx1250_reg_r(sx1250_op_code_t op, uint8_t *p, uint16_t n, uint8_t c) {
    if (!c) return tx_read(op, p, n);
    assert(c == 1 && n <= 253);
    uint8_t status = 0x82 | (mode << 4);
    if ((unsigned)op == 0x11) { p[0] = status; p[1] = packet_type; return 0; }
    switch (op) {
    case GET_STATUS: p[0] = status; break;
    case READ_REGISTER: {
        unsigned addr = (p[0] << 8) | p[1];
        if (getenv("IMPLICIT")) assert(addr != 0x0749 && addr != 0x076b);
        if (addr == 0x0702) {
            assert(getenv("IMPLICIT") && n == 4);
            regs[0x0702] = arms == 2 ? 8 : 7;
        }
        /* Synthetic RX API vectors use CR4/6; injected frames use the RF1 profile. */
        regs[0x0749] = injected_size ? configured_cr << 4 : 0x20;
        regs[0x076b] = arms == 2 && !injected_size ? 0 : 0x10;
        p[2] = status; memcpy(p + 3, regs + addr, n - 3); break;
    }
    case GET_IRQ_STATUS:
        if (mode == 5 && arms <= 3 && !getenv("IDLE")) fake_air(0, 0x12);
        p[0] = status; p[1] = irq >> 8; p[2] = irq; break;
    case GET_RX_BUFFER_STATUS: p[0] = status; p[1] = getenv("IMPLICIT") ? (arms == 2 ? 7 : 193) : injected_size ? injected_size : 255; p[2] = 230; break;
    case GET_PACKET_STATUS: p[0] = status; p[1] = 161; p[2] = 249; break;
    case READ_BUFFER: {
        if (getenv("IMPLICIT")) assert(arms == 3 && n == 9);
        unsigned off = p[0]; p[1] = status;
        for (unsigned i = 2; i < n; i++) p[i] = injected_size ? injected[(uint8_t)(off + i - 2 - 230)] : (off + i - 2 + tag) & 255;
        break;
    }
    default: assert(!"unexpected radio read opcode");
    }
    return 0;
}
