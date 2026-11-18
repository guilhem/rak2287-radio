/* RF0 packet/IQ transitions, shared by the transmitter and timing bench. */
/* Retain the pre-TX values changed by the LoRa IQ, sync and PA workarounds.
 * OCP can change during SetPaConfig. Store RF0 values, never RF1 trim values. */
static const struct sx1250_reg_span bridge_spans[] = {
    {0x0736, 1}, {0x0740, 2}, {0x08d8, 1}, {0x0889, 1}, {0x08e7, 1}
};
#define SPANS(a) (sizeof(a) / sizeof((a)[0]))
static uint8_t bridge_regs[SPANS(bridge_spans)][4];
static uint8_t bridge_setup[SX1250_RF0_RESET_SPAN_COUNT][4];

static int snapshot(const struct sx1250_reg_span *spans, size_t count, uint8_t values[][4]) {
    for (size_t i = 0; i < count; i++) {
        uint16_t addr = spans[i].address;
        if (bank_read(tx_radio, READ_REGISTER, (uint8_t[]){addr >> 8, addr}, 2,
                      values[i], spans[i].size)) {
            fprintf(stderr, "RF0 snapshot failed at 0x%04x\n", addr);
            return -1;
        }
    }
    return 0;
}

/* Both uses operate only while RF0 is in the indicated standby mode and RIF
 * is SPI-clocked. Check each readback before moving on to another field. */
static int write_snapshot(const struct sx1250_reg_span *spans, size_t count,
                          const uint8_t values[][4], unsigned mode) {
    for (size_t i = 0; i < count; i++) {
        uint16_t addr = spans[i].address;
        size_t size = spans[i].size;
        uint8_t cmd[6] = {addr >> 8, addr}, check[4];
        memcpy(cmd + 2, values[i], size);
        if (bank_write(tx_radio, WRITE_REGISTER, cmd, size + 2, mode) ||
            bank_read(tx_radio, READ_REGISTER, cmd, 2, check, size) ||
            memcmp(check, values[i], size)) {
            fprintf(stderr, "RF0 register restore failed at 0x%04x\n", addr);
            return -1;
        }
    }
    return 0;
}

static int bridge_setup_matches(void) {
    uint8_t current[SX1250_RF0_RESET_SPAN_COUNT][4] = {0};
    if (snapshot(sx1250_rf0_reset_spans, SX1250_RF0_RESET_SPAN_COUNT, current)) return -1;
    for (size_t i = 0; i < SX1250_RF0_RESET_SPAN_COUNT; i++) {
        if (memcmp(current[i], bridge_setup[i], sx1250_rf0_reset_spans[i].size)) {
            fprintf(stderr, "RF0 restored bridge differs at 0x%04x\n", sx1250_rf0_reset_spans[i].address);
            return -1;
        }
    }
    return 0;
}

static int capture_bridge(struct sx1250_handoff_report *report) {
    uint8_t bridge, packet_type;
    stage(report, "rf0-bridge-state");
    if (bank_status(tx_radio, GET_STATUS, 5) ||
        bank_read(tx_radio, READ_REGISTER, (uint8_t[]){5, 0x87}, 2, &bridge, 1) ||
        bank_read(tx_radio, (sx1250_op_code_t)0x11, NULL, 0, &packet_type, 1)) return -1;
    if (bridge != 0x0b || packet_type != PACKET_TYPE_GFSK) {
        fprintf(stderr, "RF0 bridge pin mode 0x%02x and packet type 0x%02x\n", bridge, packet_type);
        return -1;
    }
    stage(report, "rf0-bridge-snapshot");
    return bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, 2) ||
           snapshot(bridge_spans, SPANS(bridge_spans), bridge_regs) ||
           snapshot(sx1250_rf0_reset_spans, SX1250_RF0_RESET_SPAN_COUNT, bridge_setup);
}

static int restore_bridge(uint32_t hz, int64_t *set_rx_ns,
                          struct sx1250_handoff_report *report, bool counter_active) {
    /* One warm attempt; no retries. Return to cold recovery when a check fails
     * or restoration takes over 500 ms before rearming RF1. No SetTx here. */
    int64_t started = now_ns();
    uint8_t type, irq[2];
    stage(report, "rf0-restore-packet-state");
    if (started < 0 || pa_off() || lnas(0) ||
        line(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, 0) ||
        bank_write(tx_radio, SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        bank_write(tx_radio, SET_DIO_IRQ_PARAMS, (uint8_t[8]){0}, 8, 3) ||
        bank_write(tx_radio, CLR_IRQ_STATUS, (uint8_t[]){0xff, 0xff}, 2, 3) ||
        bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, irq, 2) || irq[0] || irq[1] ||
        bank_write(tx_radio, SET_PACKET_TYPE, (uint8_t[]){PACKET_TYPE_GFSK}, 1, 3) ||
        bank_read(tx_radio, (sx1250_op_code_t)0x11, NULL, 0, &type, 1) || type != PACKET_TYPE_GFSK ||
        write_snapshot(bridge_spans, SPANS(bridge_spans), bridge_regs, 3)) return -1;
    stage(report, "rf0-restore-bridge-clock");
    if (sx1250_setup(tx_radio, hz, false) || sx1302_radio_clock_select(tx_radio) ||
        bank_status(tx_radio, GET_STATUS, 5) || bridge_setup_matches() ||
        bank_read(tx_radio, GET_IRQ_STATUS, NULL, 0, irq, 2) || irq[0] || irq[1]) return -1;
    if (counter_active && !clock_counter_running()) return -1;
    int64_t now = now_ns();
    if (now < started || now - started > INT64_C(500000000)) {
        stage(report, "rf0-restore-deadline");
        return -1;
    }
    /* After successful rearm, an arriving frame belongs to the caller even
     * if host scheduling has taken the total past the recovery budget. */
    stage(report, "rf1-warm-rearm");
    return enable_verified_rx(set_rx_ns);
}
