#ifndef SX1250_TX_H
#define SX1250_TX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional experimental half-duplex RF0 TX for the EU868 RAK2287.
 * Requires an antenna or suitable load and a serialized, open LoRa RX context.
 * The physical connector power is uncalibrated. Uses the current LoRa profile.
 * timeout_ms (1..120000) limits SetTx airtime/wait, in addition to the HAL's
 * bounded reset/setup/recovery delays. Length is 1..255, or the exact implicit
 * length. Returns 0 only for TxDone followed by successful RF1 rearming.
 * EAGAIN: RF0 did not start. Keep the TX queued, service sx1250_rx_next(),
 * and retry after sx1250_rx_activity() reports idle.
 * After other I/O failures transmission may already have occurred; do not retry
 * blindly. If RX recovery failed, close and reset the board before reopening.
 * No driver-level listen-before-talk or airtime accounting is performed. */
int sx1250_tx_send(const uint8_t *payload, size_t length, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
