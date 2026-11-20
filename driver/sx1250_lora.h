/* Shared packet-engine profile encoding for either radio. */
#ifndef SX1250_LORA_H
#define SX1250_LORA_H
#include "sx1250_rx.h"
static inline int bw_code(uint32_t hz) {
    /* Semtech sx126x_lora_bw_t and sx126x_get_lora_bw_in_hz() encodings. */
    switch (hz) {
    case 7812: return 0;
    case 10417: return 8;
    case 15625: return 1;
    case 20833: return 9;
    case 31250: return 2;
    case 41667: return 10;
    case 62500: return 3;
    case 125000: return 4;
    case 250000: return 5;
    case 500000: return 6;
    default: return -1;
    }
}

static inline bool lora_ldro(const struct sx1250_rx_config *c) {
    return (1u << c->spreading_factor) * 1000u >= 16u * c->bandwidth_hz;
}
#endif
