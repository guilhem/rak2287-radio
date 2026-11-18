#ifndef SX1250_RX_INTERNAL_H
#define SX1250_RX_INTERNAL_H
#include "sx1250_rx.h"
const struct sx1250_rx_config *sx1250_rx_profile(void);
int sx1250_rx_rearm(void);
void sx1250_rx_fault(void);
#endif
