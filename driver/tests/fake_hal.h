#ifndef SX1250_TEST_HAL_H
#define SX1250_TEST_HAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Test-only, FAKE_TX=1, IDLE=1. Never accesses a radio device. */
bool fake_inject_rx(const uint8_t *data, uint8_t size);
bool fake_inject_rf0(const uint8_t *data, uint8_t size);
/* Reject the next count RF1 SetRx commands. */
void fake_fail_rf1_rearm(unsigned count);
/* Model an image-calibration error that stays latched until reset. */
void fake_fail_image_calibration(unsigned rf);
uint32_t fake_image_band(unsigned rf);
unsigned fake_image_calibrations(unsigned rf);
uint32_t fake_last_tx_image_band(void);
/* Latch a phase (0x04 preamble, 0x08 sync, 0x10 header) in an active RF1
 * reception. Repeating a phase does not produce another hardware edge. */
bool fake_rx_progress(uint16_t flags);
/* Complete single-shot reception with a synthetic payload and terminal IRQ. */
bool fake_air(uint8_t payload_tag, unsigned flags);
size_t fake_last_tx(uint8_t *data, size_t capacity);
void fake_expect_tx_power(uint8_t power);
#ifdef __cplusplus
}
#endif
#endif
