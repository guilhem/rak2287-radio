#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sx1250_dual.h"
#include "sx1250_tx.h"

static struct sx1250_rx_config active[2];
static bool configured[2], delivered[2];
static atomic_bool receiving;

int sx1250_dual_open(const char *path, const struct sx1250_rx_config profiles[2]) {
    assert(!strcmp(path, "/simulated-spi"));
    for (unsigned rf = 0; rf < 2; ++rf) {
        assert(sx1250_lora_profile_valid(&profiles[rf]));
        active[rf] = profiles[rf];
    }
    receiving = true;
    return 0;
}

int sx1250_dual_close(void) {
    assert(receiving);
    receiving = false;
    return 0;
}

int sx1250_dual_reconfigure(unsigned rf, const struct sx1250_rx_config *profile) {
    assert(receiving && rf < 2);
    if (!sx1250_lora_profile_valid(profile)) {
        errno = EINVAL;
        return -1;
    }
    if (getenv("FAKE_RECONFIG_ERROR") && profile->frequency_hz == 868800000) {
        errno = EIO;
        return -1;
    }
    active[rf] = *profile;
    configured[rf] = true;
    fprintf(stderr, "RECONFIG rf=%u hz=%u bw=%u sf=%u cr=%u sync=%02x preamble=%u\n",
            rf, profile->frequency_hz, profile->bandwidth_hz, profile->spreading_factor,
            profile->coding_rate, profile->sync_word, profile->preamble_symbols);
    return 0;
}

int sx1250_dual_next(unsigned rf, struct sx1250_rx_packet *packet) {
    assert(receiving && rf < 2);
    if (!getenv("FAKE_RX") || !configured[rf] || delivered[rf]) return 0;
    delivered[rf] = true;
    const uint8_t bytes[] = {0x15, 0xc0, 0xdb, (uint8_t)rf};
    *packet = (struct sx1250_rx_packet){
        .size = sizeof bytes, .coding_rate = active[rf].coding_rate, .crc_present = true,
        .rssi_dbm = -95 - (int)rf, .snr_db = rf ? -3.5f : 4.25f,
    };
    memcpy(packet->payload, bytes, sizeof bytes);
    return 1;
}

int sx1250_dual_activity(unsigned rf) {
    assert(receiving && rf < 2);
    const char *gate = getenv("FAKE_TX_GATE");
    return gate && access(gate, F_OK) != 0;
}

uint32_t sx1250_lora_airtime_ms(const struct sx1250_rx_config *profile, uint8_t size) {
    if (!sx1250_lora_profile_valid(profile) || !size) return 0;
    if (!getenv("FAKE_LONG_TX")) return 10;
    const uint64_t preamble_chips = (uint64_t)profile->preamble_symbols << profile->spreading_factor;
    return (uint32_t)((preamble_chips * 1000 + profile->bandwidth_hz - 1) / profile->bandwidth_hz);
}

bool sx1250_tx_power_supported(int8_t power_dbm) {
    return power_dbm >= SX1250_RF0_TX_POWER_MIN_DBM && power_dbm <= SX1250_RF0_TX_POWER_MAX_DBM;
}

int sx1250_dual_send_with_power(const struct sx1250_rx_config *profile, int8_t power_dbm,
                                const uint8_t *payload, size_t length, unsigned timeout,
                                struct sx1250_handoff_report *report) {
    assert(receiving && timeout >= 500 && length > 0);
    if (timeout > SX1250_TX_TIMEOUT_MAX_MS || !sx1250_tx_power_supported(power_dbm)) {
        *report = (struct sx1250_handoff_report){0};
        errno = EINVAL;
        return -1;
    }
    if (sx1250_dual_activity(0)) {
        errno = EAGAIN;
        return -1;
    }
    receiving = false;
    fprintf(stderr, "SET_TX hz=%u bw=%u sf=%u cr=%u sync=%02x preamble=%u power_dbm=%d payload=",
            profile->frequency_hz, profile->bandwidth_hz, profile->spreading_factor,
            profile->coding_rate, profile->sync_word, profile->preamble_symbols, power_dbm);
    for (size_t i = 0; i < length; ++i) fprintf(stderr, "%02x", payload[i]);
    fputc('\n', stderr);
    const char *gate = getenv("FAKE_TX_IN_FLIGHT");
    while (gate && access(gate, F_OK) != 0)
        nanosleep(&(struct timespec){.tv_nsec = 2000000}, NULL);
    nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL);
    *report = (struct sx1250_handoff_report){
        .tx_started = true, .tx_done = !getenv("FAKE_AMBIGUOUS"),
        .rf0_rearmed = true, .rf1_rearmed = true,
    };
    receiving = true;
    if (!report->tx_done) {
        errno = EIO;
        return -1;
    }
    return 0;
}
