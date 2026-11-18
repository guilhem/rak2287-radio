#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <time.h>
#include "sx1250_rx.h"
#include "sx1250_board.h"
static const bool radio_verbose = false;
#include "sx1250_io.h"
#include "sx1250_packet.h"
#include "sx1250_lora.h"
#include "sx1250_rx_internal.h"
#include "sx1250_usp.h"

static bool active, failed;
static struct sx1250_rx_config profile;
static const void *const radio = &sx1250_usp_rf1;
static const unsigned terminal_irq = rx_done | crc_error | header_error | rx_timeout;
static sx126x_irq_mask_t pending_switch_irq;
/* IRQ bits remain latched until acknowledged. Repeated samples of the same
 * phase must never extend its budget. A header stays latched for next(). */
static struct {
    int64_t deadline;
    unsigned ignored;
    bool payload, repeated_preamble;
} activity;

static int arm_at(int64_t *set_rx_ns) {
    memset(&activity, 0, sizeof activity);
    pending_switch_irq = 0;
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
    if (sx1250_reg_r(GET_STATUS, &status, 1, chain) || !status_ok(status)) return -1;
    unsigned mode = (status >> 4) & 7;
    if (mode == 5) return 0;
    /* SX1261/2 table 13-9: an immediate reception returns the single-shot
     * radio to STBY_RC. Leave the latched event and FIFO for the next poll. */
    if (mode != 2 || sx126x_get_irq_status(radio, &irq)) return -1;
    return !(irq & rx_timeout) && (irq & (rx_done | header_error | crc_error)) ? 0 : -1;
}

static int arm(void) { return arm_at(NULL); }

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

static int configure_rx(const struct sx1250_rx_config *c) {
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
           sx126x_set_dio_irq_params(radio, 0x027e, 0, 0, 0) || arm();
}

int sx1250_rx_open(const char *path, const struct sx1250_rx_config *c) {
    if (!sx1250_lora_profile_valid(c)) { errno = EINVAL; return -1; }
    if (sx1250_board_acquire(path, c->frequency_hz)) return -1;
    if (configure_rx(c)) {
        sx1250_board_release(); errno = EIO; return -1;
    }
    profile = *c;
    failed = false;
    active = true;
    return 0;
}

static int receive_irq(unsigned *irq) {
    uint8_t raw[3] = {0}; /* GET_IRQ_STATUS: command status then two IRQ bytes. */
    if (sx1250_reg_r(GET_IRQ_STATUS, raw, sizeof raw, chain)) return -1;
    unsigned mode = (raw[0] >> 4) & 7, code = (raw[0] >> 1) & 7;
    *irq = (raw[1] << 8) | raw[2];
    bool standby = mode == 2 || mode == 3;
    /* A hardware timeout may set the previous-command status to timeout.
     * It requires an independently latched terminal bit and a stopped modem. */
    if (standby && code == 3 && (*irq & rx_timeout)) return 0;
    if (!status_ok(raw[0])) return -1;
    return mode == 5 || (standby && (*irq & terminal_irq)) ? 0 : -1;
}

int sx1250_rx_reconfigure(const struct sx1250_rx_config *c, bool force) {
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
        if (force) pending_switch_irq = irq;
        errno = EAGAIN;
        return -1;
    }
    if (c->frequency_hz != profile.frequency_hz) {
        /* The board uses the same SX1250 image calibration band throughout EU868.
         * Keep the exact PLL step convention used by the RF0 transmit path. */
        uint32_t pll = SX1250_FREQ_TO_REG(c->frequency_hz);
        uint8_t check[4];
        if (sx126x_set_rf_freq_in_pll_steps(radio, pll) ||
            sx126x_read_register(radio, 0x088b, check, sizeof check) ||
            (((uint32_t)check[0] << 24) | ((uint32_t)check[1] << 16) |
             ((uint32_t)check[2] << 8) | check[3]) != pll) goto fault;
    }
    if (configure_rx(c)) goto fault;
    profile = *c;
    return 0;
fault:
    sx1250_rx_fault();
    errno = EIO;
    return -1;
}

static int read_rx_fifo(struct sx1250_rx_packet *packet) {
    sx126x_rx_buffer_status_t desc;
    uint8_t metrics[3];
    /* USP reports whole dB. Keep the raw metrics for half/quarter dB resolution. */
    if (sx126x_get_rx_buffer_status(radio, &desc) ||
        read_cmd(GET_PACKET_STATUS, NULL, 0, metrics, 3)) return -1;
    packet->implicit_header = profile.implicit_length != 0;
    uint8_t size = desc.pld_len_in_bytes;
    if (packet->implicit_header) {
        /* LoRaMac-node SX126xGetRxBufferStatus uses REG_LR_PAYLOADLENGTH for
         * fixed-length LoRa. The descriptor still supplies the start offset. */
        if (sx126x_read_register(radio, 0x0702, &size, 1)) return -1;
        if (size != profile.implicit_length) {
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
        packet->coding_rate = profile.coding_rate;
        packet->crc_present = profile.require_crc;
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

static int read_packet(unsigned irq, struct sx1250_rx_packet *packet) {
    bool valid = (irq & rx_done) && !(irq & (header_error | crc_error | rx_timeout)) &&
                 (profile.implicit_length || (irq & header_valid));
    if (valid) {
        if (read_rx_fifo(packet)) return -1;
        valid = packet->size && packet->coding_rate >= 5 && packet->coding_rate <= 8 &&
                (!profile.require_crc || packet->crc_present);
    }
    return valid;
}

static int poll_packet(struct sx1250_rx_packet *out) {
    unsigned irq;
    if (receive_irq(&irq)) return -1;
    if (!(irq & terminal_irq)) return 0;
    if (freeze_rx_irq(&irq)) return -1;
    struct sx1250_rx_packet packet = {0};
    int rc = read_packet(irq, &packet);
    if (rc < 0 || arm()) return -1;
    if (rc > 0) *out = packet;
    return rc;
}

int sx1250_rx_drain_for_switch(struct sx1250_rx_packet *out) {
    if (!sx1250_rx_profile()) return -1;
    if (!out || !pending_switch_irq) { errno = EINVAL; return -1; }
    struct sx1250_rx_packet packet = {0};
    int rc = read_packet(pending_switch_irq, &packet);
    if (rc < 0 || sx126x_clear_irq_status(radio, UINT16_MAX)) {
        sx1250_rx_fault();
        errno = EIO;
        return -1;
    }
    pending_switch_irq = 0;
    if (rc > 0) *out = packet;
    return rc;
}

const struct sx1250_rx_config *sx1250_rx_profile(void) {
    if (!active || failed) { errno = active ? EIO : ENODEV; return NULL; }
    return &profile;
}

/* Conservative symbol bound derived from Semtech's LoRa time-on-air formula.
 * 24 symbols cover the preamble tail (up to 6.25), header and timing margin.
 * Explicit headers can announce up to 255 bytes, CR 4/8 and a payload CRC.
 * Use 64-bit arithmetic even for SF12/BW7.8 and a 65535-symbol preamble. */
static int64_t activity_budget_ns(bool payload, bool include_preamble) {
    const bool fixed = profile.implicit_length != 0;
    unsigned symbols = 24 + (include_preamble ? profile.preamble_symbols : 0);
    if (fixed || payload) {
        unsigned sf = profile.spreading_factor;
        int bits = 8 * (fixed ? profile.implicit_length : 255) +
                   (!fixed || profile.require_crc ? 16 : 0) + (fixed ? 0 : 20) +
                   (sf >= 7 ? 8 : 0) - 4 * (int)sf;
        unsigned block = 4 * (sf - 2 * lora_ldro(&profile));
        if (bits > 0)
            symbols += (((unsigned)bits + block - 1) / block) * (fixed ? profile.coding_rate : 8);
    }
    uint64_t chips = (uint64_t)symbols << profile.spreading_factor;
    return (chips * UINT64_C(1000000000) + profile.bandwidth_hz - 1) / profile.bandwidth_hz;
}

int sx1250_rx_activity(void) {
    if (!sx1250_rx_profile()) return -1;
    bool acknowledged_preamble = false;
    for (;;) {
        unsigned irq;
        if (receive_irq(&irq)) { errno = EIO; return -1; }
        if (irq & terminal_irq) return 1;
        /* SX1261/2 table 8-4 reserves SyncWordValid for FSK. An implicit
         * LoRa frame has no transmitted header. */
        unsigned phase = irq & (preamble_detected | (profile.implicit_length ? 0 : header_valid));
        activity.ignored &= phase;
        if (!(phase & preamble_detected)) activity.repeated_preamble = false;
        else if (acknowledged_preamble && !activity.repeated_preamble) {
            /* Immediate reassertion may be a new detection or a stuck bit.
             * Allow one more window until an actual low level is observed. */
            activity.ignored &= ~preamble_detected;
            activity.repeated_preamble = true;
        }
        if (!(phase & ~activity.ignored)) return 0;
        struct timespec t;
        if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
        int64_t now = (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
        /* A new preamble with a previous expired header still latched needs
         * a full packet window: another header has no observable rising edge. */
        bool payload = (phase & header_valid) != 0;
        if (!activity.deadline || (payload && !activity.payload)) {
            activity.payload = payload;
            activity.deadline = now + activity_budget_ns(payload, !payload || (activity.ignored & header_valid));
        }
        if (now < activity.deadline) return 1;

        /* W1C only the preamble. RxDone or HeaderValid can latch between the
         * sample and this command, including while the radio enters standby. */
        if ((phase & preamble_detected) && sx126x_clear_irq_status(radio, preamble_detected)) {
            errno = EIO;
            return -1;
        }
        acknowledged_preamble = (phase & preamble_detected) != 0;
        activity.ignored |= phase;
        activity.deadline = 0;
        /* Sample again to pick up concurrent progress and note deasserted bits. */
    }
}

int sx1250_rx_rearm_at(int64_t *set_rx_ns) { return arm_at(set_rx_ns); }

void sx1250_rx_fault(void) { failed = true; }

int sx1250_rx_next(struct sx1250_rx_packet *packet, int timeout_ms) {
    if (!active) { errno = ENODEV; return -1; }
    if (failed) { errno = EIO; return -1; }
    if (!packet || timeout_ms < 0) { errno = EINVAL; return -1; }
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (;;) {
        int rc = poll_packet(packet);
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
    if (!active) { errno = ENODEV; return -1; }
    active = false;
    return sx1250_board_release();
}
