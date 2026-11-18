#ifndef SX1250_BOARD_H
#define SX1250_BOARD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RAK2287 868 MHz SPI board, RF0 supplies the clock, RF1 is the receiver.
 * Stop other concentrator software and externally reset the board before acquire.
 * The stock HAL has process-global state. Serialize I/O and release on one owner.
 * On success RF1 is tuned, calibrated and in XOSC standby. No packet type is set.
 * Returns 0 on success, -1 with errno on failure. Release only after a successful
 * acquisition. This lock coordinates users of this API, not other SPI software. */
int sx1250_board_acquire(const char *spi_path, uint32_t frequency_hz);
int sx1250_board_release(void);
#ifdef __cplusplus
}
#endif
#endif
