#ifndef SX1250_TX_H
#define SX1250_TX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct sx1250_handoff_report {
    /* Failed transition, including one recovered after TxDone, or NULL. */
    const char *failed_stage;
    /* Host monotonic time from RF1 SetStandby completion to verified SetRx
     * completion, or zero if a timestamp is unavailable. */
    int64_t downtime_ns;
    bool tx_started; /* SetTx was attempted, even if SPI failed. */
    bool tx_done;
    bool rf1_rearmed;
    bool cold_recovery;
};

/* Optional experimental half-duplex RF0 TX for the EU868 RAK2287.
 * Requires an antenna or suitable load and a serialized, open LoRa RX context.
 * The physical connector power is uncalibrated. Uses the current LoRa profile.
 * timeout_ms (1..120000) limits SetTx airtime/wait. Setup and any recovery add
 * their own delays. Length is 1..255, or the exact implicit length.
 * Normal handoff leaves RF0 powered across packets. One cold recovery can
 * restore RF1 after a failed transition, without repeating SetTx.
 * Returns 0 only for TxDone followed by successful RF1 rearming.
 * EAGAIN: RF0 did not start. Keep the TX queued, service sx1250_rx_next(),
 * and retry after sx1250_rx_activity() reports idle.
 * After other I/O failures transmission may already have occurred; do not retry
 * blindly. If RX recovery failed, close and reset the board before reopening.
 * No driver-level listen-before-talk or airtime accounting is performed. */
int sx1250_tx_send(const uint8_t *payload, size_t length, unsigned timeout_ms);
/* Use the same send transaction and optionally return its transition report. */
int sx1250_tx_send_report(const uint8_t *payload, size_t length, unsigned timeout_ms,
                          struct sx1250_handoff_report *report);

#ifdef __cplusplus
}
#endif
#endif
