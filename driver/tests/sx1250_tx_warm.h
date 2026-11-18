#ifndef SX1250_TX_WARM_H
#define SX1250_TX_WARM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Provided only by the simulation harness. The candidate deliberately cannot
 * link to the production HAL and radio driver on their own. */
bool fake_warm_simulation_active(void);
/* Bench hook between the normal board-acquire RF0 reset and first bridge setup.
 * Run at each fresh board acquisition; do not repeat it during a transaction. */
int sx1250_tx_warm_capture_reset_sim(void);
int sx1250_tx_send_warm_sim(const uint8_t *payload, size_t length, unsigned timeout_ms);
#endif
