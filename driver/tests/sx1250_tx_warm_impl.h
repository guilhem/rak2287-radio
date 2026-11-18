/* Simulation experiment only. Included only in the separate candidate object. */
#ifndef SX1250_WARM_SIMULATION
#error "This sequence requires the fake-radio timing harness"
#endif
#include "sx1250_tx_warm.h"

struct reg_span { uint16_t address; uint8_t size; };
/* Captured from the modeled RF0 after its normal board-acquire reset.
 * The published HAL mutates all of these while entering I/Q bridge mode. */
static const struct reg_span reset_spans[] = {
    {0x0587, 1}, {0x0580, 1}, {0x0582, 4}, {0x06a1, 3}, {0x08b6, 1}, {0x088f, 3}
};
/* Retain the pre-TX values changed by the LoRa IQ, sync and PA workarounds.
 * OCP can change during SetPaConfig. Store RF0 values, never RF1 trim values. */
static const struct reg_span bridge_spans[] = {
    {0x0736, 1}, {0x0740, 2}, {0x08d8, 1}, {0x0889, 1}, {0x08e7, 1}
};
#define SPANS(a) (sizeof(a) / sizeof((a)[0]))
static uint8_t reset_regs[SPANS(reset_spans)][4], bridge_regs[SPANS(bridge_spans)][4];
static uint8_t bridge_setup[SPANS(reset_spans)][4];
static bool reset_captured;

static int snapshot(const struct reg_span *spans, size_t count, uint8_t values[][4]) {
    for (size_t i = 0; i < count; i++) {
        uint16_t addr = spans[i].address;
        if (bank_read(tx_radio, READ_REGISTER, (uint8_t[]){addr >> 8, addr}, 2,
                      values[i], spans[i].size)) return -1;
    }
    return 0;
}

/* Both uses operate only while RF0 is in the indicated standby mode and RIF
 * is SPI-clocked. Check each readback before moving on to another field. */
static int write_snapshot(const struct reg_span *spans, size_t count,
                          uint8_t values[][4], unsigned mode) {
    for (size_t i = 0; i < count; i++) {
        uint16_t addr = spans[i].address;
        size_t size = spans[i].size;
        uint8_t cmd[6] = {addr >> 8, addr}, check[4];
        memcpy(cmd + 2, values[i], size);
        if (bank_write(tx_radio, WRITE_REGISTER, cmd, size + 2, mode) ||
            bank_read(tx_radio, READ_REGISTER, cmd, 2, check, size) ||
            memcmp(check, values[i], size)) return -1;
    }
    return 0;
}

int sx1250_tx_warm_capture_reset_sim(void) {
    reset_captured = false;
    if (!fake_warm_simulation_active() || bank_status(tx_radio, GET_STATUS, 2) ||
        snapshot(reset_spans, SPANS(reset_spans), reset_regs)) return -1;
    reset_captured = reset_regs[0][0] != 0x0b;
    return reset_captured ? 0 : -1;
}

static int bridge_setup_matches(void) {
    uint8_t current[SPANS(reset_spans)][4] = {0};
    return snapshot(reset_spans, SPANS(reset_spans), current) ||
           memcmp(current, bridge_setup, sizeof current);
}

static int leave_bridge_sim(uint8_t rf1_pin_mode) {
    uint8_t bridge, packet_type;
    if (!reset_captured || reset_regs[0][0] != rf1_pin_mode ||
        bank_status(tx_radio, GET_STATUS, 5) ||
        bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) || bridge != 0x0b ||
        bank_read(tx_radio, (sx1250_op_code_t)0x11, NULL, 0, &packet_type, 1) ||
        packet_type != PACKET_TYPE_GFSK || /* Only a known initial modem can be restored. */
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, 2) ||
        snapshot(bridge_spans, SPANS(bridge_spans), bridge_regs) ||
        snapshot(reset_spans, SPANS(reset_spans), bridge_setup)) return -1;
    return write_snapshot(reset_spans, SPANS(reset_spans), reset_regs, 2);
}

static int restore_bridge_sim(uint32_t hz) {
    /* One warm attempt; no retries. Return to cold recovery when a check fails
     * or restoration takes over 500 ms before rearming RF1. No SetTx here. */
    int64_t started = now_ns();
    uint8_t type, irq[2];
    if (started < 0 || pa_off() || lnas(0) ||
        line(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, 0) ||
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        bank_write(tx_radio, SET_DIO_IRQ_PARAMS, (uint8_t[8]){0}, 8, 3) ||
        bank_write(tx_radio, CLR_IRQ_STATUS, (uint8_t[]){0xff, 0xff}, 2, 3) ||
        bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, irq, 2) || irq[0] || irq[1] ||
        bank_write(tx_radio, SET_PACKET_TYPE, (uint8_t[]){PACKET_TYPE_GFSK}, 1, 3) ||
        bank_read(tx_radio, (sx1250_op_code_t)0x11, NULL, 0, &type, 1) || type != PACKET_TYPE_GFSK ||
        write_snapshot(bridge_spans, SPANS(bridge_spans), bridge_regs, 3) ||
        sx1250_setup(tx_radio, hz, false) || sx1302_radio_clock_select(tx_radio) ||
        bank_status(tx_radio, GET_STATUS, 5) || bridge_setup_matches() ||
        bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, irq, 2) || irq[0] || irq[1]) return -1;
    int64_t now = now_ns();
    if (now < started || now - started > INT64_C(500000000)) return -1;
    /* After successful rearm, an arriving frame belongs to the caller even
     * if host scheduling has taken the total past the recovery budget. */
    return enable_verified_rx();
}
