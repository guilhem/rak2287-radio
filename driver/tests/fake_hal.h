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
size_t fake_last_tx(uint8_t *data, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
