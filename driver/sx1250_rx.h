#ifndef SX1250_RX_H
#define SX1250_RX_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Independent LoRa packet reception on RF1 of the RAK2287 EU868.
 * Supported RF center 863000001..869999999 Hz, BW 7812/10417/15625/20833/31250/41667/
 * 62500/125000/250000/500000 Hz (Semtech integer values), SF5..12, CR denominator 5..8, any 8-bit sync word. Preamble is >= 8
 * symbols (>= 12 for SF5/6). Bandwidth/SF combinations outside the tested
 * EU Narrow profile follow Semtech command encodings and need RF validation. */
struct sx1250_rx_config {
    uint32_t frequency_hz, bandwidth_hz;
    uint16_t preamble_symbols;
    uint8_t spreading_factor, coding_rate, sync_word;
    bool require_crc, inverted_iq;
    uint8_t implicit_length; /* 0: explicit header. 1..255: fixed payload length. */
};

struct sx1250_rx_packet {
    uint8_t payload[255];
    uint8_t size, coding_rate; /* coding rate denominator */
    bool crc_present;
    bool implicit_header; /* true: CR/CRC come from configuration; false: received header */
    float rssi_dbm, snr_db;
};

/* Validate the RF1 LoRa profile without opening the board. */
bool sx1250_lora_profile_valid(const struct sx1250_rx_config *config);
/* LoRa packet airtime in milliseconds, rounded up. Returns 0 for an invalid
 * profile or payload size. No board connection required. */
uint32_t sx1250_lora_airtime_ms(const struct sx1250_rx_config *config, uint8_t size);

/* Serialize all operations. Call open after stopping other radio software
 * and resetting the board. Call close once after a successful open, including
 * after next() errors. Before starting the stock forwarder, reset as usual.
 * open/close return 0 or -1 with errno. The stock HAL may log to stdout. */
int sx1250_rx_open(const char *spi_path, const struct sx1250_rx_config *config);
/* Change only RF1 and rearm RX under the existing board lock. EAGAIN leaves
 * terminal events for next(). Without force, partial RX activity also gives
 * EAGAIN. On EIO after a hardware write, close and reopen the board. */
int sx1250_rx_reconfigure(const struct sx1250_rx_config *config, bool force);
/* After a forced reconfiguration returns EAGAIN, drain its frozen terminal
 * event. Returns 1 for a complete packet, 0 for an invalid reception, or
 * -1 with errno. The pending event is acknowledged and RF1 stays in standby.
 * Retry the forced reconfiguration before calling next() or activity(). */
int sx1250_rx_drain_for_switch(struct sx1250_rx_packet *packet);
/* Returns 1 for a complete packet, 0 at the timeout, -1 on error with errno.
 * timeout_ms >= 0. Zero polls once. No packet is copied on timeout/error.
 * One reception latches the FIFO until next() reads and rearms the radio.
 * Arrivals during that interval can be missed when the caller polls slowly. */
int sx1250_rx_next(struct sx1250_rx_packet *packet, int timeout_ms);
/* Return 1 while RF1 reports a LoRa preamble, an explicit header, or a terminal
 * event awaiting next(). Return 0 if no such activity is observed, -1/errno on
 * error. No FIFO consumption, standby or CAD. Poll next() to drain terminals.
 * The local deadline starts when activity() first observes a partial IRQ.
 * A previously latched indication can therefore defer TX for a full window
 * on that first call. Its radio arrival time is unknown. The preamble budget
 * assumes a sender using the configured preamble length.
 * Unrelated energy, activity in another PHY, and arrivals after the sample may
 * go undetected. In implicit mode only the preamble predicts a pending frame. */
int sx1250_rx_activity(void);
int sx1250_rx_close(void);
#ifdef __cplusplus
}
#endif
#endif
