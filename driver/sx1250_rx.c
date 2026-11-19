#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <time.h>
#include "sx1250_rx.h"
#include "sx1250_dual.h"
#include "sx1250_board.h"
static const bool radio_verbose = false;
#include "sx1250_io.h"
#include "sx1250_packet.h"
#include "sx1250_lora.h"
#include "sx1250_rx_internal.h"
#include "sx1250_usp.h"

static enum { CLOSED, SINGLE, DUAL } session;
static bool failed, paused;
static const unsigned terminal_irq = rx_done | crc_error | header_error | rx_timeout;
static struct receiver {
    struct sx1250_rx_config profile;
    sx126x_irq_mask_t pending_switch_irq;
    /* IRQs remain latched. Repeated samples of the same phase must not extend
     * its budget. A header stays latched for the packet reader. */
    struct {
        int64_t deadline;
        unsigned ignored;
        bool payload, repeated_preamble;
    } activity;
} receivers[2];

static int arm_at(unsigned rf, int64_t *set_rx_ns) {
    struct receiver *rx = &receivers[rf];
    const void *radio = &sx1250_usp_radios[rf];
    memset(&rx->activity, 0, sizeof rx->activity);
    rx->pending_switch_irq = 0;
    sx126x_irq_mask_t irq;
    uint8_t status = 0;
    if (sx126x_clear_irq_status(radio, UINT16_MAX) ||
        sx126x_get_irq_status(radio, &irq) || irq ||
        sx126x_set_rx(radio, 0)) return -1;
    if (set_rx_ns) {
        struct timespec t;
        *set_rx_ns = clock_gettime(CLOCK_MONOTONIC, &t) ? -1 :
                    (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
    }
    wait_ms(10);
    if (sx1250_reg_r(GET_STATUS, &status, 1, rf) || !status_ok(status)) return -1;
    unsigned mode = (status >> 4) & 7;
    if (mode == 5) return 0;
    /* SX1261/2 table 13-9: an immediate reception returns the single-shot
     * radio to STBY_RC. Leave the latched event and FIFO for the next poll. */
    if (mode != 2 || sx126x_get_irq_status(radio, &irq)) return -1;
    return !(irq & rx_timeout) && (irq & (rx_done | header_error | crc_error)) ? 0 : -1;
}

static int arm(unsigned rf) { return arm_at(rf, NULL); }

bool sx1250_lora_profile_valid(const struct sx1250_rx_config *c) {
    return c && c->spreading_factor >= 5 && c->spreading_factor <= 12 &&
           c->coding_rate >= 5 && c->coding_rate <= 8 && bw_code(c->bandwidth_hz) >= 0 &&
           c->preamble_symbols >= (c->spreading_factor <= 6 ? 12 : 8) &&
           c->frequency_hz > 863000000 && c->frequency_hz < 870000000;
}

static sx126x_mod_params_lora_t mod_params(const struct sx1250_rx_config *c) {
    return (sx126x_mod_params_lora_t){
        .sf = c->spreading_factor, .bw = bw_code(c->bandwidth_hz),
        .cr = c->coding_rate - 4, .ldro = lora_ldro(c),
    };
}

static sx126x_pkt_params_lora_t packet_params(const struct sx1250_rx_config *c, uint8_t size) {
    return (sx126x_pkt_params_lora_t){
        .preamble_len_in_symb = c->preamble_symbols,
        .header_type = c->implicit_length ? SX126X_LORA_PKT_IMPLICIT : SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes = size,
        .crc_is_on = c->require_crc, .invert_iq_is_on = c->inverted_iq,
    };
}

uint32_t sx1250_lora_airtime_ms(const struct sx1250_rx_config *c, uint8_t size) {
    if (!sx1250_lora_profile_valid(c) || !size || (c->implicit_length && size != c->implicit_length)) return 0;
    sx126x_mod_params_lora_t mod = mod_params(c);
    sx126x_pkt_params_lora_t pkt = packet_params(c, size);
    /* USP's millisecond wrapper uses a 32-bit intermediate. */
    uint64_t scaled = UINT64_C(1000) * sx126x_get_lora_time_on_air_numerator(&pkt, &mod);
    return (scaled + c->bandwidth_hz - 1) / c->bandwidth_hz;
}

static int configure_rx(unsigned rf, const struct sx1250_rx_config *c) {
    const void *radio = &sx1250_usp_radios[rf];
    sx126x_mod_params_lora_t mod = mod_params(c);
    sx126x_pkt_params_lora_t pkt = packet_params(c, c->implicit_length ? c->implicit_length : 255);
    /* Program the known-good low nibbles rather than trusting reset defaults. */
    uint8_t sync[] = {(c->sync_word & 0xf0) | 4, (c->sync_word << 4) | 4}, check[2];
    sx126x_pkt_type_t type;
    return sx126x_set_pkt_type(radio, SX126X_PKT_TYPE_LORA) ||
           sx126x_get_pkt_type(radio, &type) || type != SX126X_PKT_TYPE_LORA ||
           sx126x_set_lora_mod_params(radio, &mod) ||
           sx126x_set_lora_pkt_params(radio, &pkt) ||
           sx126x_write_register(radio, 0x0740, sync, sizeof sync) ||
           sx126x_read_register(radio, 0x0740, check, sizeof check) ||
           memcmp(check, sync, sizeof sync) ||
           sx126x_set_buffer_base_address(radio, 0, 0) ||
           sx126x_set_dio_irq_params(radio, 0x027e, 0, 0, 0) || arm(rf);
}

int sx1250_rx_open(const char *path, const struct sx1250_rx_config *c) {
    if (!sx1250_lora_profile_valid(c)) { errno = EINVAL; return -1; }
    if (sx1250_board_acquire(path, c->frequency_hz)) return -1;
    if (configure_rx(chain, c)) {
        sx1250_board_release(); errno = EIO; return -1;
    }
    receivers[chain].profile = *c;
    failed = false;
    session = SINGLE;
    return 0;
}

static int receive_irq(unsigned rf, unsigned *irq) {
    uint8_t raw[3] = {0}; /* GET_IRQ_STATUS: command status then two IRQ bytes. */
    if (sx1250_reg_r(GET_IRQ_STATUS, raw, sizeof raw, rf)) return -1;
    unsigned mode = (raw[0] >> 4) & 7, code = (raw[0] >> 1) & 7;
    *irq = (raw[1] << 8) | raw[2];
    bool standby = mode == 2 || mode == 3;
    /* A hardware timeout may set the previous-command status to timeout.
     * It requires an independently latched terminal bit and a stopped modem. */
    if (standby && code == 3 && (*irq & rx_timeout)) return 0;
    if (!status_ok(raw[0])) return -1;
    return mode == 5 || (standby && (*irq & terminal_irq)) ? 0 : -1;
}

static int set_frequency(unsigned rf, uint32_t hz) {
    const void *radio = &sx1250_usp_radios[rf];
    uint32_t pll = SX1250_FREQ_TO_REG(hz);
    uint8_t expected[] = {pll >> 24, pll >> 16, pll >> 8, pll}, check[4];
    return sx126x_set_rf_freq_in_pll_steps(radio, pll) ||
           sx126x_read_register(radio, 0x088b, check, sizeof check) ||
           memcmp(expected, check, sizeof check);
}

int sx1250_rx_reconfigure(const struct sx1250_rx_config *c, bool force) {
    struct receiver *rx = &receivers[chain];
    const void *radio = &sx1250_usp_radios[chain];
    if (!sx1250_rx_profile()) return -1;
    if (!sx1250_lora_profile_valid(c)) { errno = EINVAL; return -1; }
    if (!force) {
        int busy = sx1250_rx_activity();
        if (busy < 0) return -1;
        if (busy) { errno = EAGAIN; return -1; }
    }
    sx126x_irq_mask_t irq;
    if (sx126x_set_standby(radio, SX126X_STANDBY_CFG_XOSC) ||
        sx126x_get_irq_status(radio, &irq)) goto fault;
    /* Do not erase a terminal IRQ or FIFO latched before standby. */
    if (irq & terminal_irq) {
        if (force) rx->pending_switch_irq = irq;
        errno = EAGAIN;
        return -1;
    }
    /* Reuse the current image calibration anywhere in the EU868 range. */
    if ((c->frequency_hz != rx->profile.frequency_hz && set_frequency(chain, c->frequency_hz)) ||
        configure_rx(chain, c)) goto fault;
    rx->profile = *c;
    return 0;
fault:
    sx1250_rx_fault();
    errno = EIO;
    return -1;
}

static int read_rx_fifo(unsigned rf, struct sx1250_rx_packet *packet) {
    const void *radio = &sx1250_usp_radios[rf];
    const struct sx1250_rx_config *profile = &receivers[rf].profile;
    sx126x_rx_buffer_status_t desc;
    uint8_t metrics[3];
    /* USP reports whole dB. Keep the raw metrics for half/quarter dB resolution. */
    if (sx126x_get_rx_buffer_status(radio, &desc) ||
        bank_read(rf, GET_PACKET_STATUS, NULL, 0, metrics, 3)) return -1;
    packet->implicit_header = profile->implicit_length != 0;
    uint8_t size = desc.pld_len_in_bytes;
    if (packet->implicit_header) {
        /* LoRaMac-node SX126xGetRxBufferStatus uses REG_LR_PAYLOADLENGTH for
         * fixed-length LoRa. The descriptor still supplies the start offset. */
        if (sx126x_read_register(radio, 0x0702, &size, 1)) return -1;
        if (size != profile->implicit_length) {
            packet->size = 0; /* Discard this event before any FIFO read. */
            return 0;
        }
    }
    unsigned start = desc.buffer_start_pointer;
    for (unsigned i = 0; i < size;) {
        uint8_t offset = start + i;
        size_t n = size - i;
        if (n > 128) n = 128;
        if (sx126x_read_buffer(radio, offset, packet->payload + i, n)) return -1;
        i += n;
    }
    packet->size = size;
    if (packet->implicit_header) {
        packet->coding_rate = profile->coding_rate;
        packet->crc_present = profile->require_crc;
    } else {
        uint8_t cr, crc;
        if (sx126x_read_register(radio, 0x0749, &cr, 1) ||
            sx126x_read_register(radio, 0x076b, &crc, 1)) return -1;
        packet->coding_rate = 4 + ((cr >> 4) & 7);
        packet->crc_present = (crc & 0x10) != 0;
    }
    packet->rssi_dbm = -metrics[0] / 2.0f;
    packet->snr_db = (int8_t)metrics[1] / 4.0f;
    return 0;
}

static int read_packet(unsigned rf, unsigned irq, struct sx1250_rx_packet *packet) {
    const struct sx1250_rx_config *profile = &receivers[rf].profile;
    bool valid = (irq & rx_done) && !(irq & (header_error | crc_error | rx_timeout)) &&
                 (profile->implicit_length || (irq & header_valid));
    if (valid) {
        if (read_rx_fifo(rf, packet)) return -1;
        valid = packet->size && packet->coding_rate >= 5 && packet->coding_rate <= 8 &&
                (!profile->require_crc || packet->crc_present);
    }
    return valid;
}

static int poll_packet(unsigned rf, struct sx1250_rx_packet *out) {
    unsigned irq;
    if (receive_irq(rf, &irq)) return -1;
    if (!(irq & terminal_irq)) return 0;
    if (freeze_rx_irq_on(rf, &irq)) return -1;
    struct sx1250_rx_packet packet = {0};
    int rc = read_packet(rf, irq, &packet);
    if (rc < 0 || arm(rf)) return -1;
    if (rc > 0) *out = packet;
    return rc;
}

int sx1250_rx_drain_for_switch(struct sx1250_rx_packet *out) {
    struct receiver *rx = &receivers[chain];
    const void *radio = &sx1250_usp_radios[chain];
    if (!sx1250_rx_profile()) return -1;
    if (!out || !rx->pending_switch_irq) { errno = EINVAL; return -1; }
    struct sx1250_rx_packet packet = {0};
    int rc = read_packet(chain, rx->pending_switch_irq, &packet);
    if (rc < 0 || sx126x_clear_irq_status(radio, UINT16_MAX)) {
        sx1250_rx_fault();
        errno = EIO;
        return -1;
    }
    rx->pending_switch_irq = 0;
    if (rc > 0) *out = packet;
    return rc;
}

const struct sx1250_rx_config *sx1250_rx_profile(void) {
    if (session != SINGLE || failed) { errno = session == SINGLE ? EIO : ENODEV; return NULL; }
    return &receivers[chain].profile;
}

/* Conservative symbol bound derived from Semtech's LoRa time-on-air formula.
 * 24 symbols cover the preamble tail (up to 6.25), header and timing margin.
 * Explicit headers can announce up to 255 bytes, CR 4/8 and a payload CRC.
 * Use 64-bit arithmetic even for SF12/BW7.8 and a 65535-symbol preamble. */
static int64_t activity_budget_ns(const struct sx1250_rx_config *profile,
                                  bool payload, bool include_preamble) {
    const bool fixed = profile->implicit_length != 0;
    unsigned symbols = 24 + (include_preamble ? profile->preamble_symbols : 0);
    if (fixed || payload) {
        unsigned sf = profile->spreading_factor;
        int bits = 8 * (fixed ? profile->implicit_length : 255) +
                   (!fixed || profile->require_crc ? 16 : 0) + (fixed ? 0 : 20) +
                   (sf >= 7 ? 8 : 0) - 4 * (int)sf;
        unsigned block = 4 * (sf - 2 * lora_ldro(profile));
        if (bits > 0)
            symbols += (((unsigned)bits + block - 1) / block) * (fixed ? profile->coding_rate : 8);
    }
    uint64_t chips = (uint64_t)symbols << profile->spreading_factor;
    return (chips * UINT64_C(1000000000) + profile->bandwidth_hz - 1) / profile->bandwidth_hz;
}

static int receive_activity(unsigned rf) {
    struct receiver *rx = &receivers[rf];
    const struct sx1250_rx_config *profile = &rx->profile;
    const void *radio = &sx1250_usp_radios[rf];
    bool acknowledged_preamble = false;
    for (;;) {
        unsigned irq;
        if (receive_irq(rf, &irq)) { errno = EIO; return -1; }
        if (irq & terminal_irq) return 1;
        /* SX1261/2 table 8-4 reserves SyncWordValid for FSK. An implicit
         * LoRa frame has no transmitted header. */
        unsigned phase = irq & (preamble_detected | (profile->implicit_length ? 0 : header_valid));
        rx->activity.ignored &= phase;
        if (!(phase & preamble_detected)) rx->activity.repeated_preamble = false;
        else if (acknowledged_preamble && !rx->activity.repeated_preamble) {
            /* Immediate reassertion may be a new detection or a stuck bit.
             * Allow one more window until an actual low level is observed. */
            rx->activity.ignored &= ~preamble_detected;
            rx->activity.repeated_preamble = true;
        }
        if (!(phase & ~rx->activity.ignored)) return 0;
        struct timespec t;
        if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
        int64_t now = (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
        /* A new preamble with a previous expired header still latched needs
         * a full packet window: another header has no observable rising edge. */
        bool payload = (phase & header_valid) != 0;
        if (!rx->activity.deadline || (payload && !rx->activity.payload)) {
            rx->activity.payload = payload;
            rx->activity.deadline = now + activity_budget_ns(profile, payload, !payload || (rx->activity.ignored & header_valid));
        }
        if (now < rx->activity.deadline) return 1;

        /* W1C only the preamble. RxDone or HeaderValid can latch between the
         * sample and this command, including while the radio enters standby. */
        if ((phase & preamble_detected) && sx126x_clear_irq_status(radio, preamble_detected)) {
            errno = EIO;
            return -1;
        }
        acknowledged_preamble = (phase & preamble_detected) != 0;
        rx->activity.ignored |= phase;
        rx->activity.deadline = 0;
        /* Sample again to pick up concurrent progress and note deasserted bits. */
    }
}

int sx1250_rx_activity(void) {
    return sx1250_rx_profile() ? receive_activity(chain) : -1;
}

int sx1250_rx_rearm_at(int64_t *set_rx_ns) { return arm_at(chain, set_rx_ns); }

void sx1250_rx_fault(void) { failed = true; }

int sx1250_rx_next(struct sx1250_rx_packet *packet, int timeout_ms) {
    if (session != SINGLE) { errno = ENODEV; return -1; }
    if (failed) { errno = EIO; return -1; }
    if (!packet || timeout_ms < 0) { errno = EINVAL; return -1; }
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        int rc = poll_packet(chain, packet);
        if (rc < 0) errno = EIO;
        if (rc || !timeout_ms) return rc;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        int64_t left = (int64_t)timeout_ms * 1000000 -
            ((now.tv_sec - start.tv_sec) * INT64_C(1000000000) + now.tv_nsec - start.tv_nsec);
        if (left <= 0) return 0;
        if (nanosleep(&(struct timespec){.tv_nsec = left < 10000000 ? left : 10000000}, NULL))
            return -1;
    }
}

int sx1250_rx_close(void) {
    if (session != SINGLE) { errno = ENODEV; return -1; }
    session = CLOSED;
    return sx1250_board_release();
}

int sx1250_dual_open(const char *path, const struct sx1250_rx_config profiles[2]) {
    if (!profiles || !sx1250_lora_profile_valid(&profiles[0]) ||
        !sx1250_lora_profile_valid(&profiles[1])) { errno = EINVAL; return -1; }
    uint32_t frequencies[] = {profiles[0].frequency_hz, profiles[1].frequency_hz};
    if (sx1250_board_acquire_dual(path, frequencies)) return -1;
    for (unsigned rf = 0; rf < 2; ++rf) {
        if (configure_rx(rf, &profiles[rf])) {
            sx1250_board_release();
            errno = EIO;
            return -1;
        }
        receivers[rf].profile = profiles[rf];
    }
    session = DUAL;
    failed = paused = false;
    return 0;
}

static int check_dual(void) {
    if (session != DUAL) { errno = ENODEV; return -1; }
    if (failed) { errno = EIO; return -1; }
    return 0;
}

static int dual_fault(void) {
    failed = true;
    errno = EIO;
    return -1;
}

int sx1250_dual_next(unsigned rf, struct sx1250_rx_packet *packet) {
    if (rf > 1 || !packet) { errno = EINVAL; return -1; }
    if (check_dual()) return -1;
    if (paused) { errno = EBUSY; return -1; }
    int rc = poll_packet(rf, packet);
    return rc < 0 ? dual_fault() : rc;
}

int sx1250_dual_activity(unsigned rf) {
    if (rf > 1) { errno = EINVAL; return -1; }
    if (check_dual()) return -1;
    if (paused) { errno = EBUSY; return -1; }
    int rc = receive_activity(rf);
    return rc < 0 ? dual_fault() : rc;
}

static bool receive_pending(const struct receiver *rx, sx126x_irq_mask_t flags) {
    unsigned phase = preamble_detected | (rx->profile.implicit_length ? 0 : header_valid);
    return (flags & (terminal_irq | (phase & ~rx->activity.ignored))) != 0;
}

int sx1250_dual_reconfigure(unsigned rf, const struct sx1250_rx_config *config) {
    if (rf > 1 || !sx1250_lora_profile_valid(config)) { errno = EINVAL; return -1; }
    int busy = sx1250_dual_activity(rf);
    if (busy < 0) return -1;
    if (busy) { errno = EAGAIN; return -1; }

    struct receiver *rx = &receivers[rf];
    const void *radio = &sx1250_usp_radios[rf];
    sx126x_irq_mask_t flags;
    if (sx126x_set_standby(radio, SX126X_STANDBY_CFG_XOSC) ||
        sx126x_get_irq_status(radio, &flags)) return dual_fault();
    if (receive_pending(rx, flags)) {
        /* Keep a terminal IRQ and its FIFO for the next poll. */
        if (!(flags & terminal_irq) && arm(rf)) return dual_fault();
        errno = EAGAIN;
        return -1;
    }

    /* These profiles all use the same EU868 image calibration. */
    bool retune = config->frequency_hz != rx->profile.frequency_hz;
    if ((retune && set_frequency(rf, config->frequency_hz)) || configure_rx(rf, config)) {
        if (sx126x_set_standby(radio, SX126X_STANDBY_CFG_XOSC) ||
            (retune && set_frequency(rf, rx->profile.frequency_hz)) ||
            configure_rx(rf, &rx->profile)) return dual_fault();
        errno = EIO;
        return -1;
    }
    rx->profile = *config;
    return 0;
}

int sx1250_dual_pause(void) {
    if (check_dual()) return -1;
    if (paused) { errno = EBUSY; return -1; }
    for (unsigned rf = 0; rf < 2; ++rf) {
        int busy = sx1250_dual_activity(rf);
        if (busy < 0) return -1;
        if (busy) { errno = EAGAIN; return -1; }
    }
    sx126x_irq_mask_t flags[2];
    unsigned frozen = 0;
    while (frozen < 2) {
        int busy = sx1250_dual_activity(frozen);
        if (busy < 0) return -1;
        if (busy) break;
        const void *radio = &sx1250_usp_radios[frozen];
        if (sx126x_set_standby(radio, SX126X_STANDBY_CFG_XOSC) ||
            sx126x_get_irq_status(radio, &flags[frozen])) return dual_fault();
        bool defer = receive_pending(&receivers[frozen], flags[frozen]);
        ++frozen;
        if (defer) goto defer;
    }
    if (frozen == 2) {
        paused = true;
        return 0;
    }
defer:
    /* Leave any terminal IRQ and its FIFO in standby for the normal reader. */
    for (unsigned rf = 0; rf < frozen; ++rf)
        if (!(flags[rf] & terminal_irq) && arm(rf)) return dual_fault();
    errno = EAGAIN;
    return -1;
}

static int configure_both(void) {
    for (unsigned rf = 0; rf < 2; ++rf)
        if (configure_rx(rf, &receivers[rf].profile)) return -1;
    return 0;
}

static int restore_dual_rx(const uint32_t frequencies[2]) {
    for (unsigned rf = 0; rf < 2; ++rf)
        if (sx126x_set_standby(&sx1250_usp_radios[rf], SX126X_STANDBY_CFG_XOSC) ||
            set_frequency(rf, frequencies[rf])) return -1;
    return sx1250_board_enable_dual_rx() || configure_both();
}

int sx1250_dual_resume_report(bool force_cold, bool *cold_recovery) {
    if (cold_recovery) *cold_recovery = false;
    if (check_dual()) return -1;
    if (!paused) { errno = EINVAL; return -1; }
    uint32_t frequencies[] = {receivers[0].profile.frequency_hz, receivers[1].profile.frequency_hz};
    if (force_cold || restore_dual_rx(frequencies)) {
        if (cold_recovery) *cold_recovery = true;
        if (sx1250_board_recover_dual(frequencies) || configure_both()) return dual_fault();
    }
    paused = false;
    return 0;
}

int sx1250_dual_resume(void) { return sx1250_dual_resume_report(false, NULL); }

int sx1250_dual_close(void) {
    if (session != DUAL) { errno = ENODEV; return -1; }
    session = CLOSED;
    return sx1250_board_release();
}
