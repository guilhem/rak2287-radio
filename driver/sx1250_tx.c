#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1302.h"
#include "sx1250_board.h"
#include "sx1250_dual.h"
#include "sx1250_rx_internal.h"
#include "sx1250_tx.h"
#include "sx1250_tx_handoff.h"
static const bool radio_verbose = false;
#include "sx1250_io.h"
#include "sx1250_lora.h"

enum { tx_radio = 0, tx_done = 1, timer_irq = 0x200 };

/* Add levels once their output at the connector has been measured with this PA setting. */
static const struct rf0_power_level {
    int8_t power_dbm, chip_setting;
} rf0_power_levels[] = {
    {SX1250_RF0_TX_SETTING_DBM, SX1250_RF0_TX_SETTING_DBM},
};

static const struct rf0_power_level *find_rf0_power(int8_t power_dbm) {
    for (size_t i = 0; i < sizeof rf0_power_levels / sizeof *rf0_power_levels; ++i)
        if (rf0_power_levels[i].power_dbm == power_dbm) return &rf0_power_levels[i];
    return NULL;
}

bool sx1250_tx_power_supported(int8_t power_dbm) {
    return find_rf0_power(power_dbm) != NULL;
}

static void stage(struct sx1250_handoff_report *report, const char *name) {
    if (report) report->failed_stage = name;
}

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

static int enable_verified_rx(int64_t *set_rx_ns) {
    uint8_t bridge;
    int32_t rif, clock_a, clock_b;
    return bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) || bridge != 0x0b ||
        lgw_reg_r(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, &rif) || rif != 1 ||
        lgw_reg_r(SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_A_SEL, &clock_a) || clock_a != 1 ||
        lgw_reg_r(SX1302_REG_CLK_CTRL_CLK_SEL_CLK_RADIO_B_SEL, &clock_b) || clock_b != 0 ||
        lnas(1) || sx1250_rx_rearm_at(set_rx_ns);
}

static int restore(uint32_t frequency_hz, int64_t *set_rx_ns) {
    int muted = pa_off();
    /* Attempt an immediate RF abort; a verified hardware reset follows even if
     * this command failed or the status of the triggering SetTx is unknown. */
    (void)sx1250_reg_w(SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, tx_radio);
    /* Reset also switches RIF to SPI and stops any in-progress RF0 packet.
     * Restore the radio's FPGA_MODE_RX clock before re-enabling the RF path. */
    if (sx1302_radio_reset(tx_radio, LGW_RADIO_TYPE_SX1250) || muted ||
        sx1302_radio_set_mode(tx_radio, LGW_RADIO_TYPE_SX1250) ||
        sx1250_setup(tx_radio, frequency_hz, false) || sx1302_radio_clock_select(tx_radio) ||
        enable_verified_rx(set_rx_ns)) {
        pa_off();
        lnas(0);
        sx1250_rx_fault();
        return -1;
    }
    return 0;
}

/* This counter can be stopped when no concentrator firmware is running.
 * Require progression after switching back only when the baseline advances. */
static bool clock_counter_running(void) {
    uint8_t first[8], second[8];
    if (lgw_reg_rb(SX1302_REG_TIMESTAMP_TIMESTAMP_PPS_MSB2_TIMESTAMP_PPS, first, sizeof first))
        return false;
    wait_ms(1);
    if (lgw_reg_rb(SX1302_REG_TIMESTAMP_TIMESTAMP_PPS_MSB2_TIMESTAMP_PPS, second, sizeof second))
        return false;
    return memcmp(first + 4, second + 4, 4) != 0;
}

static int64_t now_ns(void);
#include "sx1250_tx_warm_impl.h"

static int configure_rf0_tx(const struct sx1250_rx_config *c, uint8_t length, int8_t chip_power,
                            struct sx1250_handoff_report *report) {
    uint32_t frf = SX1250_FREQ_TO_REG(c->frequency_hz);
    uint8_t f[] = {frf >> 24, frf >> 16, frf >> 8, frf}, actual[4], clamp, quality;
    stage(report, "rf0-packet-configuration");
    if (bank_write(tx_radio, SET_RF_FREQUENCY, f, sizeof f, 3) ||
        bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0x8b}, 2, actual, sizeof actual) ||
        memcmp(f, actual, sizeof actual) || configure_phy(tx_radio, c, length)) return -1;

    /* SX1262 low-output PA table and errata 15.1 / 15.2. The SX1250 output
     * stage and FEM bypass on this board have no autonomous-mode calibration. */
    stage(report, "rf0-low-output-pa");
    return bank_write(tx_radio, SET_PA_CONFIG, (uint8_t[]){2, 2, 0, 1}, 4, 3) ||
           bank_write(tx_radio, SET_TX_PARAMS, (uint8_t[]){(uint8_t)chip_power, SET_RAMP_200U}, 2, 3) ||
           bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0xd8}, 2, &clamp, 1) ||
           bank_write(tx_radio, WRITE_REGISTER, (uint8_t[]){8, 0xd8, clamp | 0x1e}, 3, 3) ||
           bank_read(tx_radio, READ_REGISTER, (uint8_t[]){8, 0x89}, 2, &quality, 1) ||
           bank_write(tx_radio, WRITE_REGISTER, (uint8_t[]){8, 0x89,
                      c->bandwidth_hz == 500000 ? quality & ~4 : quality | 4}, 3, 3) ||
           bank_write(tx_radio, SET_DIO_IRQ_PARAMS, (uint8_t[]){2,1,0,0,0,0,0,0}, 8, 3);
}

static int prepare_rf0(const struct sx1250_rx_config *c, uint8_t length, uint8_t packet_pin_mode,
                       unsigned flags, struct sx1250_handoff_report *report) {
    uint8_t bridge;
    stage(report, "rf0-spi-clock");
    if (pa_off() || lnas(0) || line(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, 0)) return -1;
    if ((flags & (SX1250_HANDOFF_WARM_PREPARE | SX1250_HANDOFF_WARM_RESTORE)) &&
        capture_bridge(report)) return -1;
    if (flags & SX1250_HANDOFF_WARM_PREPARE) {
        const uint8_t (*reset_regs)[4] = sx1250_board_rf0_reset();
        stage(report, "rf0-reset-snapshot");
        if (!reset_regs) return -1;
        if (reset_regs[0][0] != packet_pin_mode) {
            fprintf(stderr, "RF0 reset pin mode 0x%02x and RF1 packet mode 0x%02x\n",
                    reset_regs[0][0], packet_pin_mode);
            return -1;
        }
        stage(report, "rf0-packet-pins");
        if (write_snapshot(sx1250_rf0_reset_spans, SX1250_RF0_RESET_SPAN_COUNT, reset_regs, 2))
            return -1;
    } else {
        stage(report, "rf0-reset");
        if (sx1302_radio_reset(tx_radio, LGW_RADIO_TYPE_SX1250) ||
            sx1302_radio_set_mode(tx_radio, LGW_RADIO_TYPE_SX1250) ||
            bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, 2)) return -1;
    }
    stage(report, "rf0-packet-configuration");
    if (bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) ||
        bridge != packet_pin_mode || /* Compare with the working RF1 packet engine. */
        bank_write(tx_radio, CALIBRATE, (uint8_t[]){0x7f}, 1, 2) ||
        sx1250_calibrate(tx_radio, c->frequency_hz) ||
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3)) return -1;
    return configure_rf0_tx(c, length, SX1250_RF0_TX_SETTING_DBM, report);
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

static int clear_tx_irqs(void) {
    uint8_t flags[2];
    return bank_write(tx_radio, CLR_IRQ_STATUS, (uint8_t[]){0xff, 0xff}, 2, 3) ||
           bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, flags, 2) || flags[0] || flags[1];
}

static int transmit(unsigned timeout_ms, struct sx1250_handoff_report *report) {
    uint32_t ticks = timeout_ms * 64u;
    int64_t start = now_ns();
    if (start < 0) return EIO;
    int64_t deadline = start + (int64_t)timeout_ms * 1000000;
    stage(report, "rf0-set-tx");
    if (report) report->tx_started = true;
    if (sx1250_reg_w(SET_TX, (uint8_t[]){ticks >> 16, ticks >> 8, ticks}, 3, tx_radio)) return EIO;
    stage(report, "rf0-tx-done");
    for (;;) {
        uint8_t raw[3] = {0};
        if (sx1250_reg_r(GET_IRQ_STATUS, raw, sizeof raw, tx_radio)) return EIO;
        unsigned irq = (raw[1] << 8) | raw[2], mode = (raw[0] >> 4) & 7, code = (raw[0] >> 1) & 7;
        if ((irq & timer_irq) && (irq & tx_done)) return EIO;
        if (irq & timer_irq) return ETIMEDOUT;
        if ((mode != 2 && mode != 3 && mode != 6) || (code >= 3 && code <= 5)) return EIO;
        /* IRQ is latched at the radio. A delayed host read still confirms
         * completion within the independently programmed hardware timeout. */
        if ((irq & tx_done) && mode != 6) {
            if (report) report->tx_done = true;
            return 0;
        }
        int64_t now = now_ns();
        if (now < 0) return EIO;
        if (now >= deadline) return ETIMEDOUT;
        if (nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL) && errno == EINTR) return EINTR;
    }
}

static int load_and_send(const uint8_t *payload, size_t length, unsigned timeout_ms,
                         bool dry_run, struct sx1250_handoff_report *report) {
    stage(report, "rf0-fifo");
    if (fill_fifo(payload, length)) return EIO;
    stage(report, "rf0-pre-transmit-irq");
    if (clear_tx_irqs()) return EIO;
    return dry_run ? 0 : transmit(timeout_ms, report);
}

static bool valid_tx(const struct sx1250_rx_config *c, const uint8_t *payload,
                     size_t length, unsigned timeout_ms) {
    return payload && length && length <= 255 && timeout_ms && timeout_ms <= SX1250_TX_TIMEOUT_MAX_MS &&
           (!c->implicit_length || length == c->implicit_length);
}

static int handoff(const uint8_t *payload, size_t length, unsigned timeout_ms, unsigned flags,
                   bool strict, struct sx1250_handoff_report *report) {
    stage(report, "validate");
    const struct sx1250_rx_config *c = sx1250_rx_profile();
    if (!c) return -1;
    if (!valid_tx(c, payload, length, timeout_ms)) { errno = EINVAL; return -1; }

    /* Preserve a completed RF1 frame; the caller drains it with next().
     * Standby prevents another reception between this check and the RF switch. */
    uint8_t irq[2], packet_pin_mode;
    stage(report, "rf1-packet-pins");
    if (reg_read(0x0587, &packet_pin_mode, 1) || packet_pin_mode == 0x0b) {
        errno = EIO;
        return -1;
    }
    bool counter_active = false;
    if (flags & SX1250_HANDOFF_WARM_RESTORE) {
        counter_active = clock_counter_running();
        if (strict)
            fprintf(stderr, "RF0 clock counter baseline advancing=%u\n", (unsigned)counter_active);
    }
    /* Last live sample before stopping RF1. A preamble starting between
     * this sample and standby can still be cut short. The frozen IRQ read
     * below protects a terminal/FIFO that latched in that interval. */
    stage(report, "rf1-activity");
    int busy = sx1250_rx_activity();
    if (busy < 0) return -1;
    if (busy) { errno = EAGAIN; return -1; }
    stage(report, "rf1-standby");
    int standby = sx1250_reg_w(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, chain);
    int64_t standby_ns = !standby && report ? now_ns() : -1;
    if (!standby) wait_ms(10);
    if (standby || radio_status(SET_STANDBY, 3) ||
        read_cmd(GET_IRQ_STATUS, NULL, 0, irq, 2)) {
        sx1250_rx_fault(); errno = EIO; return -1;
    }
    if ((irq[0] & 2) || (irq[1] & 0x62)) { errno = EAGAIN; return -1; }
    int err = prepare_rf0(c, length, packet_pin_mode, flags, report) ? EIO :
        load_and_send(payload, length, timeout_ms, flags & SX1250_HANDOFF_DRY_RUN, report);

    int64_t set_rx_ns = -1;
    int64_t *stamp = report ? &set_rx_ns : NULL;
    bool rearmed = false;
    if (!err && (flags & SX1250_HANDOFF_WARM_RESTORE)) {
        rearmed = restore_bridge(c->frequency_hz, stamp, report, counter_active) == 0;
        if (!rearmed && strict) err = EIO;
    }
    bool fallback = !rearmed && (flags & (SX1250_HANDOFF_WARM_PREPARE | SX1250_HANDOFF_WARM_RESTORE)) &&
                    (err || (flags & SX1250_HANDOFF_WARM_RESTORE));
    if (!rearmed) {
        standby = 0;
        if (fallback) {
            if (report) report->cold_recovery = true;
            /* Warm restoration may already have reached SetRx. Freeze RF1 before
             * touching its clock again, even if the mode check fails. */
            standby = write_cmd(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3);
        } else if (!err) {
            stage(report, "rf0-cold-restore");
        }
        rearmed = restore(c->frequency_hz, stamp) == 0 && !standby;
        if (fallback && !rearmed) {
            fprintf(stderr, "RF0 cold recovery failed\n");
            pa_off(); lnas(0); sx1250_rx_fault();
        }
    }
    if (!rearmed) err = EIO;
    if (report) {
        report->rf1_rearmed = rearmed;
        if (rearmed && standby_ns >= 0 && set_rx_ns >= standby_ns)
            report->downtime_ns = set_rx_ns - standby_ns;
        if (!err && !fallback) report->failed_stage = NULL;
    }
    if (err) { errno = err; return -1; }
    return 0;
}

int sx1250_tx_send_report(const uint8_t *payload, size_t length, unsigned timeout_ms,
                          struct sx1250_handoff_report *report) {
    if (report) *report = (struct sx1250_handoff_report){0};
    return handoff(payload, length, timeout_ms,
                   SX1250_HANDOFF_WARM_PREPARE | SX1250_HANDOFF_WARM_RESTORE, false, report);
}

int sx1250_tx_send(const uint8_t *payload, size_t length, unsigned timeout_ms) {
    return sx1250_tx_send_report(payload, length, timeout_ms, NULL);
}

int sx1250_dual_send_with_power(const struct sx1250_rx_config *profile, int8_t power_dbm,
                                const uint8_t *payload, size_t length, unsigned timeout_ms,
                                struct sx1250_handoff_report *report) {
    if (report) *report = (struct sx1250_handoff_report){0};
    stage(report, "validate");
    const struct rf0_power_level *power = find_rf0_power(power_dbm);
    if (!power || !sx1250_lora_profile_valid(profile) || !valid_tx(profile, payload, length, timeout_ms)) {
        errno = EINVAL;
        return -1;
    }

    stage(report, "dual-pause");
    int64_t pause_start = report ? now_ns() : -1;
    if (sx1250_dual_pause()) return -1;

    stage(report, "rf0-packet-snapshot");
    uint8_t saved[SPANS(bridge_spans)][4];
    bool saved_valid = !pa_off() && !lnas(0) &&
        !snapshot(bridge_spans, SPANS(bridge_spans), saved);
    /* Both banks are already in packet mode and calibrated for this EU band. */
    int err = !saved_valid || configure_rf0_tx(profile, length, power->chip_setting, report) ? EIO :
        load_and_send(payload, length, timeout_ms, false, report);

    bool force_cold = false;
    if (saved_valid) {
        /* Explicitly abort on an uncertain SetTx or TxDone before touching RF0.
         * Do not re-enable the LNAs until the dual receiver verifies standby. */
        int cleanup = pa_off();
        cleanup |= bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
            write_snapshot(bridge_spans, SPANS(bridge_spans), saved, 3);
        force_cold = cleanup != 0;
        if (force_cold && !err) {
            stage(report, "rf0-restore-packet-state");
            err = EIO;
        }
    }
    if (sx1250_dual_resume_report(force_cold, report ? &report->cold_recovery : NULL)) {
        stage(report, "dual-resume");
        errno = EIO;
        return -1;
    }

    if (report) {
        int64_t resumed_at = now_ns();
        if (pause_start >= 0 && resumed_at >= pause_start)
            report->downtime_ns = resumed_at - pause_start;
        report->rf0_rearmed = true;
        report->rf1_rearmed = true;
        if (!err) report->failed_stage = report->cold_recovery ? "dual-warm-resume" : NULL;
    }
    if (err) { errno = err; return -1; }
    return 0;
}

int sx1250_dual_send(const struct sx1250_rx_config *profile, const uint8_t *payload, size_t length,
                     unsigned timeout_ms, struct sx1250_handoff_report *report) {
    return sx1250_dual_send_with_power(profile, SX1250_RF0_TX_SETTING_DBM, payload, length,
                                      timeout_ms, report);
}

int sx1250_tx_probe_handoff(const uint8_t *payload, size_t length, unsigned timeout_ms,
                           unsigned flags, struct sx1250_handoff_report *report) {
    if (!report) { errno = EINVAL; return -1; }
    *report = (struct sx1250_handoff_report){0};
    if (flags & ~(SX1250_HANDOFF_WARM_PREPARE | SX1250_HANDOFF_WARM_RESTORE | SX1250_HANDOFF_DRY_RUN)) {
        stage(report, "validate"); errno = EINVAL; return -1;
    }
    return handoff(payload, length, timeout_ms, flags, true, report);
}
