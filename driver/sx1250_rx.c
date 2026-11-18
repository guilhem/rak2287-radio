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

static bool active, failed;
static struct sx1250_rx_config profile;
static int arm(void) {
    uint8_t flags[2], status = 0;
    if (write_cmd(CLR_IRQ_STATUS, (uint8_t[]){0xff, 0xff}, 2, 3) ||
        read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2) || flags[0] || flags[1] ||
        sx1250_reg_w(SET_RX, (uint8_t[]){0, 0, 0}, 3, chain)) return -1;
    wait_ms(10);
    if (sx1250_reg_r(GET_STATUS, &status, 1, chain) || !status_ok(status)) return -1;
    unsigned mode = (status >> 4) & 7;
    if (mode == 5) return 0;
    /* SX1261/2 table 13-9: an immediate reception returns the single-shot
     * radio to STBY_RC. Leave the latched event and FIFO for the next poll. */
    if (mode != 2 || read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2)) return -1;
    unsigned irq = ((unsigned)flags[0] << 8) | flags[1];
    return !(irq & rx_timeout) && (irq & (rx_done | header_error | crc_error)) ? 0 : -1;
}

int sx1250_rx_open(const char *path, const struct sx1250_rx_config *c) {
    if (!c || c->spreading_factor < 5 || c->spreading_factor > 12 ||
        c->coding_rate < 5 || c->coding_rate > 8 || bw_code(c->bandwidth_hz) < 0 ||
        c->preamble_symbols < (c->spreading_factor <= 6 ? 12 : 8)) {
        errno = EINVAL; return -1;
    }
    if (sx1250_board_acquire(path, c->frequency_hz)) return -1;
    if (configure_phy(chain, c, c->implicit_length ? c->implicit_length : 255) ||
        write_cmd(SET_DIO_IRQ_PARAMS, (uint8_t[]){2, 0x7e, 0, 0, 0, 0, 0, 0}, 8, 3) || arm()) {
        sx1250_board_release(); errno = EIO; return -1;
    }
    profile = *c;
    failed = false;
    active = true;
    return 0;
}

static int poll_packet(struct sx1250_rx_packet *out) {
    uint8_t flags[2];
    if (read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2)) return -1;
    unsigned irq = (flags[0] << 8) | flags[1];
    if (!(irq & (rx_done | crc_error | header_error | rx_timeout))) return 0;
    if (freeze_rx_irq(&irq)) return -1;
    bool valid = (irq & rx_done) && !(irq & (header_error | crc_error | rx_timeout)) &&
                 (profile.implicit_length || (irq & header_valid));
    struct sx1250_rx_packet packet = {0};
    if (valid) {
        unsigned offset;
        if (read_rx_fifo(&packet, &offset, &profile)) return -1;
        valid = packet.size && packet.coding_rate >= 5 && packet.coding_rate <= 8 &&
                (!profile.require_crc || packet.crc_present);
    }
    if (arm()) return -1;
    if (valid) *out = packet;
    return valid;
}

const struct sx1250_rx_config *sx1250_rx_profile(void) {
    if (!active || failed) { errno = active ? EIO : ENODEV; return NULL; }
    return &profile;
}

int sx1250_rx_rearm(void) { return arm(); }

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
