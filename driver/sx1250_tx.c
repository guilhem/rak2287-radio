#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1302.h"
#include "sx1250_rx_internal.h"
#include "sx1250_tx.h"
static const bool radio_verbose = false;
#include "sx1250_io.h"
#include "sx1250_lora.h"

enum { tx_radio = 0, tx_done = 1, timer_irq = 0x200 };

static int line(uint16_t reg, int value) {
    int32_t actual;
    return lgw_reg_w(reg, value) || lgw_reg_r(reg, &actual) || actual != value;
}

/* Do both writes, including if the first fails. */
static int pa_off(void) {
    int result = line(SX1302_REG_AGC_MCU_RF_EN_A_PA_EN, 0);
    result |= line(SX1302_REG_AGC_MCU_RF_EN_B_PA_EN, 0);
    return result;
}

static int lnas(int value) {
    int result = line(SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN, value);
    result |= line(SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN, value);
    return result;
}

static int enable_verified_rx(void) {
    uint8_t bridge;
    int32_t rif, clock_a, clock_b;
    return bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) || bridge != 0x0b ||
        lgw_reg_r(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, &rif) || rif != 1 ||
        lgw_reg_r(SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_A_SEL, &clock_a) || clock_a != 1 ||
        lgw_reg_r(SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_B_SEL, &clock_b) || clock_b != 0 ||
        lnas(1) || sx1250_rx_rearm();
}

static int restore(uint32_t frequency_hz) {
    int muted = pa_off();
    /* Attempt an immediate RF abort; a verified hardware reset follows even if
     * this command failed or the status of the triggering SetTx is unknown. */
    (void)sx1250_reg_w(SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, tx_radio);
    /* Reset also switches RIF to SPI and stops any in-progress RF0 packet.
     * Restore the radio's FPGA_MODE_RX clock before re-enabling the RF path. */
    if (sx1302_radio_reset(tx_radio, LGW_RADIO_TYPE_SX1250) || muted ||
        sx1302_radio_set_mode(tx_radio, LGW_RADIO_TYPE_SX1250) ||
        sx1250_setup(tx_radio, frequency_hz, false) || sx1302_radio_clock_select(tx_radio) ||
        enable_verified_rx()) {
        pa_off();
        lnas(0);
        sx1250_rx_fault();
        return -1;
    }
    return 0;
}

#ifdef SX1250_WARM_SIMULATION
static int64_t now_ns(void);
#include "tests/sx1250_tx_warm_impl.h"
#endif

static int prepare_rf0(const struct sx1250_rx_config *c, uint8_t length, uint8_t packet_pin_mode) {
    uint32_t frf = SX1250_FREQ_TO_REG(c->frequency_hz);
    uint8_t f[] = {frf >> 24, frf >> 16, frf >> 8, frf}, actual[4], bridge, clamp, quality;
    if (pa_off() || lnas(0) ||
        line(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, 0) ||
#ifdef SX1250_WARM_SIMULATION
        leave_bridge_sim(packet_pin_mode) ||
#else
        sx1302_radio_reset(tx_radio, LGW_RADIO_TYPE_SX1250) ||
        sx1302_radio_set_mode(tx_radio, LGW_RADIO_TYPE_SX1250) ||
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, 2) ||
#endif
        bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) ||
        bridge != packet_pin_mode || /* Compare with the working RF1 packet engine. */
        bank_write(tx_radio, CALIBRATE, (uint8_t[]){0x7f}, 1, 2) ||
        sx1250_calibrate(tx_radio, c->frequency_hz) ||
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        bank_write(tx_radio, SET_RF_FREQUENCY, f, sizeof f, 3) ||
        bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0x8b}, 2, actual, 4) ||
        memcmp(f, actual, sizeof f) || configure_phy(tx_radio, c, length)) return -1;

    /* SX1262 low-output PA table and errata 15.1 / 15.2. The SX1250 output
     * stage and FEM bypass on this board have no autonomous-mode calibration. */
    return bank_write(tx_radio, SET_PA_CONFIG, (uint8_t[]){2, 2, 0, 1}, 4, 3) ||
           bank_write(tx_radio, SET_TX_PARAMS, (uint8_t[]){(uint8_t)-9, SET_RAMP_200U}, 2, 3) ||
           bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0xd8}, 2, &clamp, 1) ||
           bank_write(tx_radio, WRITE_REGISTER, (uint8_t[]){8, 0xd8, clamp | 0x1e}, 3, 3) ||
           bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0x89}, 2, &quality, 1) ||
           bank_write(tx_radio, WRITE_REGISTER, (uint8_t[]){8, 0x89,
                      c->bandwidth_hz == 500000 ? quality & ~4 : quality | 4}, 3, 3) ||
           bank_write(tx_radio, SET_DIO_IRQ_PARAMS, (uint8_t[]){2,1,0,0,0,0,0,0}, 8, 3);
}

static int fill_fifo(const uint8_t *data, size_t length) {
    uint8_t cmd[129], check[128];
    for (size_t offset = 0; offset < length;) {
        size_t n = length - offset;
        if (n > sizeof check) n = sizeof check;
        cmd[0] = offset;
        memcpy(cmd + 1, data + offset, n);
        if (bank_write(tx_radio, WRITE_BUFFER, cmd, n + 1, 3) ||
            bank_read(tx_radio, READ_BUFFER, cmd, 1, check, n) ||
            memcmp(data + offset, check, n)) return -1;
        offset += n;
    }
    return 0;
}

static int64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
}

static int transmit(unsigned timeout_ms) {
    uint8_t flags[2];
    if (bank_write(tx_radio, CLR_IRQ_STATUS, (uint8_t[]){0xff, 0xff}, 2, 3) ||
        bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, flags, 2) || flags[0] || flags[1]) return EIO;
    uint32_t ticks = timeout_ms * 64u;
    int64_t start = now_ns();
    if (start < 0) return EIO;
    int64_t deadline = start + (int64_t)timeout_ms * 1000000;
    if (sx1250_reg_w(SET_TX, (uint8_t[]){ticks >> 16, ticks >> 8, ticks}, 3, tx_radio)) return EIO;
    for (;;) {
        uint8_t raw[3] = {0};
        if (sx1250_reg_r(GET_IRQ_STATUS, raw, sizeof raw, tx_radio)) return EIO;
        unsigned irq = (raw[1] << 8) | raw[2], mode = (raw[0] >> 4) & 7, code = (raw[0] >> 1) & 7;
        if ((irq & timer_irq) && (irq & tx_done)) return EIO;
        if (irq & timer_irq) return ETIMEDOUT;
        if ((mode != 2 && mode != 3 && mode != 6) || (code >= 3 && code <= 5)) return EIO;
        /* IRQ is latched at the radio. A delayed host read still confirms
         * completion within the independently programmed hardware timeout. */
        if ((irq & tx_done) && mode != 6) return 0;
        int64_t now = now_ns();
        if (now < 0) return EIO;
        if (now >= deadline) return ETIMEDOUT;
        if (nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL) && errno == EINTR) return EINTR;
    }
}

#ifdef SX1250_WARM_SIMULATION
int sx1250_tx_send_warm_sim(const uint8_t *payload, size_t length, unsigned timeout_ms) {
    if (!fake_warm_simulation_active() || !reset_captured) { errno = EPERM; return -1; }
#else
int sx1250_tx_send(const uint8_t *payload, size_t length, unsigned timeout_ms) {
#endif
    const struct sx1250_rx_config *c = sx1250_rx_profile();
    if (!c) return -1;
    if (!payload || !length || length > 255 || !timeout_ms || timeout_ms > 120000 ||
        (c->implicit_length && length != c->implicit_length)) { errno = EINVAL; return -1; }

    /* Preserve a completed RF1 frame; the caller drains it with next().
     * Standby prevents another reception between this check and the RF switch. */
    uint8_t flags[2], packet_pin_mode;
    if (reg_read(0x0587, &packet_pin_mode, 1) || packet_pin_mode == 0x0b) {
        errno = EIO;
        return -1;
    }
    /* Last live sample before stopping RF1. A preamble starting between
     * this sample and standby can still be cut short. The frozen IRQ read
     * below protects a terminal/FIFO that latched in that interval. */
    int busy = sx1250_rx_activity();
    if (busy < 0) return -1;
    if (busy) { errno = EAGAIN; return -1; }
    if (write_cmd(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2)) {
        sx1250_rx_fault(); errno = EIO; return -1;
    }
    if ((flags[0] & 2) || (flags[1] & 0x62)) { errno = EAGAIN; return -1; }
    int err = EIO;
    if (!prepare_rf0(c, length, packet_pin_mode) && !fill_fifo(payload, length)) err = transmit(timeout_ms);
#ifdef SX1250_WARM_SIMULATION
    /* A successful TxDone permits verified warm recovery. Any other result
     * goes directly to one cold recovery; an ambiguous TX is never resent. */
    if (err || restore_bridge_sim(c->frequency_hz)) {
        /* Warm recovery might already have reached RF1 SetRx. Cold recovery
         * expects RF1 in standby, including if this confirmation fails. */
        int standby = write_cmd(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3);
        if (restore(c->frequency_hz) || standby) {
            pa_off();
            lnas(0);
            sx1250_rx_fault();
            err = EIO;
        }
    }
#else
    if (restore(c->frequency_hz)) err = EIO;
#endif
    if (err) { errno = err; return -1; }
    return 0;
}
