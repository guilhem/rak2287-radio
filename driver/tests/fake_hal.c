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
static unsigned mode, packet_type, irq, arms, irq_mask;
static uint8_t regs[65536], tag, configured_cr, rf1_mod[4], rf1_packet[6];

static int32_t lines[LGW_TOTALREGS];
static unsigned tx_mode, tx_type, tx_irq, tx_size;
static uint8_t tx_regs[65536], tx_fifo[256], sent[255], sent_size;
static uint8_t injected[255], injected_size;
static bool restoring, tx_started;
static unsigned tx_commands;
static bool injected_warm_fault;

#ifdef HANDOVER_BENCH
/* The handover executable links the pinned reset, setup, calibration and SPI
 * implementations; only their hardware-facing register effects are modeled. */
extern unsigned bench_due_tx_irq(void);
/* Synthetic, deliberately distinct register defaults. These values exercise
 * snapshot/restore, not assumptions about silicon factory reset contents. */
static const struct { uint16_t addr; uint8_t value; } bench_reset_fields[] = {
    {0x0580,0x41}, {0x0582,0x42}, {0x0583,0x43}, {0x0584,0x44}, {0x0585,0x45},
    {0x06a1,0x51}, {0x06a2,0x52}, {0x06a3,0x53}, {0x08b6,0x61},
    {0x088f,0x71}, {0x0890,0x72}, {0x0891,0x73}
};
#endif

static bool fault(const char *name) {
    const char *f = getenv("FAKE_TX_FAULT");
    return f && !strcmp(f, name);
}

#ifdef HANDOVER_BENCH
/* Advance the radio independently of the host's next IRQ read. Once the
 * first terminal event has latched, later polls cannot change its outcome. */
void fake_bench_latch_tx(void) {
    if (tx_mode != 6 || fault("deadline")) return;
    unsigned event = bench_due_tx_irq();
    if (!event) return;
    tx_mode = 2;
    tx_irq = fault("conflict") ? 0x201 : fault("timeout") ? 0x200 : event;
    if (tx_irq == 1) {
        memcpy(sent, tx_fifo, tx_size);
        sent_size = tx_size;
    }
}

static const struct { uint16_t addr; uint8_t value; } bench_bridge_fields[] = {
    {0x0736,0x91}, {0x0740,0x92}, {0x0741,0x93}, {0x08d8,0x94}, {0x0889,0x91}, {0x08e7,0x96}
};
#endif

size_t fake_last_tx(uint8_t *data, size_t capacity) {
    if (!data || capacity < sent_size) return 0;
    memcpy(data, sent, sent_size);
    return sent_size;
}

bool fake_rx_progress(uint16_t flags) {
    if (!connected || mode != 5 || !flags || (flags & ~0x1cu)) return false;
    irq |= flags & irq_mask;
    return true;
}

bool fake_air(uint8_t payload_tag, unsigned flags) {
    if (mode != 5) return false;
    tag = payload_tag;
    irq |= flags & irq_mask;
    mode = 2;
    return true;
}

bool fake_inject_rx(const uint8_t *data, uint8_t size) {
    if (!getenv("FAKE_TX") || !data || !size || !fake_air(0, rf1_packet[2] ? 0x02 : 0x12)) return false;
    memcpy(injected, data, size);
    injected_size = size;
    return true;
}

static int tx_write(sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(getenv("FAKE_TX") && n && n <= 253);
    switch (op) {
    case SET_STANDBY: tx_mode = 2 + p[0]; break;
    case CALIBRATE: assert(tx_mode == 2); break;
#ifdef HANDOVER_BENCH
    case CALIBRATE_IMAGE: assert(tx_mode == 2 && n == 2); break;
    case SET_RX:
        assert(n == 3 && p[0] == 255 && p[1] == 255 && p[2] == 255);
        for (size_t i = 0; i < sizeof bench_bridge_fields / sizeof *bench_bridge_fields; ++i)
            assert(tx_regs[bench_bridge_fields[i].addr] == bench_bridge_fields[i].value);
        tx_mode = 5; break;
#endif
    case SET_RF_FREQUENCY: memcpy(tx_regs + 0x088b, p, 4); break;
    case SET_PACKET_TYPE: tx_type = p[0]; break;
    case WRITE_REGISTER: memcpy(tx_regs + ((p[0] << 8) | p[1]), p + 2, n - 2); break;
    case SET_MODULATION_PARAMS: assert(tx_mode == 3 && n == 4 && !memcmp(p, rf1_mod, 4)); break;
    case SET_PACKET_PARAMS:
        assert(tx_mode == 3 && n == 6 && !memcmp(p, rf1_packet, 3) && !memcmp(p+4, rf1_packet+4, 2));
        tx_size = p[3]; break;
    case SET_PA_CONFIG:
        assert(n == 4 && p[0] == 2 && p[1] == 2 && !p[2] && p[3] == 1);
#ifdef HANDOVER_BENCH
        tx_regs[0x08e7] = 0xa7; /* Synthetic OCP side effect, not a silicon value. */
#endif
        break;
    case SET_TX_PARAMS: assert(n == 2 && (int8_t)p[0] == -9 && p[1] == SET_RAMP_200U); break;
    case SET_BUFFER_BASE_ADDRESS: case SET_DIO_IRQ_PARAMS: break;
    case CLR_IRQ_STATUS: tx_irq = 0; break;
    case WRITE_BUFFER: assert((size_t)p[0] + n - 1 <= sizeof tx_fifo);
        memcpy(tx_fifo + p[0], p + 1, n - 1); break;
    case SET_TX:
        assert(!getenv("FAKE_FORBID_TX"));
        ++tx_commands;
        assert(!getenv("FAKE_SINGLE_TX") || tx_commands == 1);
        assert(n == 3 && (p[0] || p[1] || p[2]) && tx_mode == 3 && mode == 3 && tx_size);
        assert(!lines[SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN] && !lines[SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN]);
        assert(!lines[SX1302_REG_AGC_MCU_RF_EN_A_PA_EN] && !lines[SX1302_REG_AGC_MCU_RF_EN_B_PA_EN]);
        assert(!lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL] && tx_regs[0x0587] == regs[0x0587]);
        assert(tx_type == PACKET_TYPE_LORA && !memcmp(tx_regs + 0x088b, regs + 0x088b, 4) &&
               !memcmp(tx_regs + 0x0740, regs + 0x0740, 2));
#ifdef HANDOVER_BENCH
        for (size_t i = 0; i < sizeof bench_bridge_fields / sizeof *bench_bridge_fields; ++i)
            assert(tx_regs[bench_bridge_fields[i].addr] != bench_bridge_fields[i].value);
        for (size_t i = 0; i < sizeof bench_reset_fields / sizeof *bench_reset_fields; ++i)
            assert(tx_regs[bench_reset_fields[i].addr] == bench_reset_fields[i].value);
#endif
        tx_started = true;
        if (fault("write")) return -1;
        tx_mode = 6; break;
    default: assert(!"unexpected RF0 write");
    }
    return 0;
}

static int tx_read(sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(n <= 253);
    /* The receive-only startup can snapshot the reset-state RF0 pins. */
    assert(getenv("FAKE_TX") || (tx_mode == 2 && (op == GET_STATUS || op == READ_REGISTER)));
    uint8_t status = 0x82 | (tx_mode << 4);
    if ((unsigned)op == 0x11) { p[0] = status; p[1] = tx_type; return 0; }
    switch (op) {
    case GET_STATUS: p[0] = status; break;
#ifdef HANDOVER_BENCH
    case GET_DEVICE_ERRORS: memset(p, 0, n); break;
#endif
    case GET_IRQ_STATUS:
#ifdef HANDOVER_BENCH
        if (tx_irq) status = tx_irq == 1 ? 0xac : 0xa6;
#else
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
#endif
        p[0] = status; p[1] = tx_irq >> 8; p[2] = tx_irq;
        if (fault("warm_restore") && tx_started && tx_mode == 3 && !tx_irq && !injected_warm_fault) {
            p[2] |= 0x40;
            injected_warm_fault = true;
        }
        break;
    case READ_REGISTER: { unsigned addr = (p[0] << 8) | p[1];
        p[2] = status; memcpy(p + 3, tx_regs + addr, n - 3);
        if (addr == 0x0587 && tx_mode == 2 && tx_regs[addr] != 0x0b && fault("pinmode")) p[3] ^= 1;
        if (addr == 0x0587 && tx_mode == 5 && fault("warm_prepare") && !injected_warm_fault) {
            p[3] ^= 1;
            injected_warm_fault = true;
        }
        break; }
    case READ_BUFFER: { unsigned offset = p[0];
        p[1] = status; memcpy(p + 2, tx_fifo + offset, n - 2);
        if (fault("readback")) p[2] ^= 1;
        break; }
    default: assert(!"unexpected RF0 read");
    }
    return 0;
}

void *lgw_com_target(void) {
    static int fake_fd = -1;
    return connected ? &fake_fd : NULL;
}
int lgw_connect(lgw_com_type_t t, const char *p) {
    assert(t == LGW_COM_SPI && p);
    connected = true; regs[0x0587] = 0x20; arms = 0; irq = 0; sent_size = injected_size = 0;
    restoring = tx_started = false; memset(lines, 0, sizeof lines);
    tx_commands = 0;
    injected_warm_fault = false;
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
int lgw_reg_rb(uint16_t reg, uint8_t *data, uint16_t size) {
    assert(reg == SX1302_REG_TIMESTAMP_TIMESTAMP_PPS_MSB2_TIMESTAMP_PPS && size == 8);
    static uint32_t counter;
    counter += 32000;
    memset(data, 0, 4);
    for (unsigned i = 0; i < 4; ++i) data[4 + i] = (uint8_t)(counter >> (24 - 8 * i));
    return 0;
}
int lgw_reg_w(uint16_t reg, int32_t val) {
    if (reg == SX1302_REG_AGC_MCU_RF_EN_A_PA_EN || reg == SX1302_REG_AGC_MCU_RF_EN_B_PA_EN)
        assert(val == 0);
#ifdef HANDOVER_BENCH
    if (reg == SX1302_REG_AGC_MCU_RF_EN_A_RADIO_RST && val == 1) {
        tx_mode = 2; tx_irq = 0; tx_type = PACKET_TYPE_GFSK;
        memset(tx_regs, 0, sizeof tx_regs);
        for (size_t i = 0; i < sizeof bench_reset_fields / sizeof *bench_reset_fields; ++i)
            tx_regs[bench_reset_fields[i].addr] = bench_reset_fields[i].value;
        for (size_t i = 0; i < sizeof bench_bridge_fields / sizeof *bench_bridge_fields; ++i)
            tx_regs[bench_bridge_fields[i].addr] = bench_bridge_fields[i].value;
        tx_regs[0x0587] = regs[0x0587];
        restoring = tx_started;
    }
#endif
    lines[reg] = val;
    return 0;
}
#ifndef HANDOVER_BENCH
int sx1302_radio_host_ctrl(bool b) { (void)b; return 0; }
int sx1302_radio_reset(uint8_t c, lgw_radio_type_t t) { assert(c <= 1 && t == LGW_RADIO_TYPE_SX1250);
    lines[SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL] = 0;
    if (!c) { tx_mode = 2; tx_irq = 0; tx_type = PACKET_TYPE_GFSK;
        tx_regs[0x0587] = fault("pinmode") ? 0 : regs[0x0587]; restoring = tx_started; }
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
int sx1250_setup(uint8_t c, uint32_t f, bool single) { assert(!c && f && !single);
    tx_regs[0x0587] = 0x0b; tx_mode = 5; restoring = tx_started; return 0; }
int sx1250_calibrate(uint8_t c, uint32_t f) { assert(f && ((c == 1 && mode == 2) || (!c && tx_mode == 2 && getenv("FAKE_TX")))); return 0; }
void wait_ms(unsigned long t) { (void)t; }
#else
#define sx1250_reg_w bench_radio_w
#define sx1250_reg_r bench_radio_r
#endif

int sx1250_reg_w(sx1250_op_code_t op, uint8_t *p, uint16_t n, uint8_t c) {
    if (!c) return tx_write(op, p, n);
    assert(c == 1 && n);
    switch (op) {
    case SET_STANDBY:
        if (mode == 5 && getenv("FAKE_RX_DONE_ON_STANDBY")) assert(fake_air(7, 0x12));
        if (irq && arms == 1 && !getenv("EARLY") && !getenv("FAKE_TX")) irq |= 0x40; /* CRC error becomes visible on freeze. */
        mode = 2 + p[0]; break;
    case CALIBRATE: assert(mode == 2); break;
#ifdef HANDOVER_BENCH
    case CALIBRATE_IMAGE: assert(mode == 2 && n == 2); break;
#endif
    case SET_RF_FREQUENCY: memcpy(regs + 0x088b, p, 4); break;
    case SET_PACKET_TYPE: packet_type = p[0]; break;
    case WRITE_REGISTER: memcpy(regs + ((p[0] << 8) | p[1]), p + 2, n - 2); break;
    case CLR_IRQ_STATUS: {
        assert(n == 2);
        unsigned mask = (p[0] << 8) | p[1];
        if (mask == 4 && getenv("FAKE_RX_DONE_ON_CLEAR")) assert(fake_air(7, 0x12));
        if (mask == 4 && getenv("FAKE_RX_CLEAR_ERROR")) return -1;
        irq &= ~mask;
        if (mask == 4 && getenv("FAKE_RX_STUCK_PREAMBLE")) irq |= 4;
        break;
    }
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
    case SET_DIO_IRQ_PARAMS:
        assert(n == 8);
        irq_mask = (p[0] << 8) | p[1]; break;
    case SET_BUFFER_BASE_ADDRESS: break;
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
#ifdef HANDOVER_BENCH
    case GET_DEVICE_ERRORS: memset(p, 0, n); break;
#endif
    case READ_REGISTER: {
        unsigned addr = (p[0] << 8) | p[1];
        if (getenv("IMPLICIT")) assert(addr != 0x0749 && addr != 0x076b);
        if (addr == 0x0702) {
            assert(rf1_packet[2] && n == 4);
            regs[0x0702] = getenv("IMPLICIT") ? (arms == 2 ? 8 : 7) : rf1_packet[3];
        }
        /* Synthetic RX API vectors use CR4/6; injected frames use the RF1 profile. */
        regs[0x0749] = injected_size ? configured_cr << 4 : 0x20;
        regs[0x076b] = arms == 2 && !injected_size ? 0 : 0x10;
        p[2] = status; memcpy(p + 3, regs + addr, n - 3); break;
    }
    case GET_IRQ_STATUS:
        if (getenv("FAKE_RX_IRQ_ERROR")) return -1;
        if (getenv("FAKE_RX_IRQ_STATUS")) status = strtoul(getenv("FAKE_RX_IRQ_STATUS"), NULL, 0);
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
