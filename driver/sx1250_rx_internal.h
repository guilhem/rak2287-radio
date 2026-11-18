#ifndef SX1250_RX_INTERNAL_H
#define SX1250_RX_INTERNAL_H
#include "sx1250_rx.h"
const struct sx1250_rx_config *sx1250_rx_profile(void);
int sx1250_rx_rearm_at(int64_t *set_rx_ns);
void sx1250_rx_fault(void);
#endif
