#ifndef SX1250_BOARD_H
#define SX1250_BOARD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Match the pinned SX1250 HAL's open image-calibration intervals. */
static inline uint32_t sx1250_image_band(uint32_t hz) {
    if (hz > 430000000 && hz < 440000000) return 430000000;
    if (hz > 470000000 && hz < 510000000) return 470000000;
    if (hz > 779000000 && hz < 787000000) return 779000000;
    if (hz > 863000000 && hz < 870000000) return 863000000;
    if (hz > 902000000 && hz < 928000000) return 902000000;
    return 0;
}

/* RAK2287 SPI board, RF0 supplies the clock, RF1 is the receiver.
 * Stop other concentrator software and externally reset the board before acquire.
 * The stock HAL has process-global state. Serialize I/O and release on one owner.
 * On success RF1 is tuned, calibrated and in XOSC standby. No packet type is set.
 * Returns 0 on success, -1 with errno on failure. Release only after a successful
 * acquisition. This lock coordinates users of this API, not other SPI software. */
int sx1250_board_acquire(const char *spi_path, uint32_t frequency_hz);
int sx1250_board_release(void);

/* Both chips start in packet-mode XOSC standby with the RIF on the SPI clock. */
int sx1250_board_acquire_dual(const char *spi_path, const uint32_t frequency_hz[2]);
int sx1250_board_recover_dual(const uint32_t frequency_hz[2]);
/* After both radios are confirmed in standby, restore the verified RX front end. */
int sx1250_board_enable_dual_rx(void);
/* Call in XOSC standby. A successful calibration returns the chain to XOSC standby. */
int sx1250_board_calibrate_for_frequency(unsigned rf, uint32_t hz);

enum { SX1250_RF0_RESET_SPAN_COUNT = 6 };
struct sx1250_reg_span { uint16_t address; uint8_t size; };
extern const struct sx1250_reg_span sx1250_rf0_reset_spans[SX1250_RF0_RESET_SPAN_COUNT];
/* Private board-start capture for restoring the RF0 I/Q pins to packet mode. */
const uint8_t (*sx1250_board_rf0_reset(void))[4];
#ifdef __cplusplus
}
#endif
#endif
