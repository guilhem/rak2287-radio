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

static inline int configure_phy(unsigned rf, const struct sx1250_rx_config *c, uint8_t payload_length) {
    uint8_t iq, check[2], type;
    uint8_t sync[] = {0x07, 0x40, (c->sync_word & 0xf0) | 4, (c->sync_word << 4) | 4};
    bool ldro = ((1u << c->spreading_factor) * 1000u >= 16u * c->bandwidth_hz);
    uint8_t mod[] = {c->spreading_factor, bw_code(c->bandwidth_hz), c->coding_rate - 4, ldro};
    uint8_t pkt[] = {c->preamble_symbols >> 8, c->preamble_symbols, c->implicit_length != 0,
                     payload_length, c->require_crc, c->inverted_iq};
    return bank_write(rf, SET_PACKET_TYPE, (uint8_t[]){PACKET_TYPE_LORA}, 1, 3) ||
        bank_read(rf, (sx1250_op_code_t)0x11, NULL, 0, &type, 1) || type != PACKET_TYPE_LORA ||
        bank_write(rf, SET_MODULATION_PARAMS, mod, sizeof mod, 3) ||
        bank_write(rf, SET_PACKET_PARAMS, pkt, sizeof pkt, 3) ||
        bank_read(rf, READ_REGISTER, (uint8_t[]){0x07,0x36}, 2, &iq, 1) ||
        bank_write(rf, WRITE_REGISTER, (uint8_t[]){0x07, 0x36, c->inverted_iq ? iq & ~4 : iq | 4}, 3, 3) ||
        bank_write(rf, WRITE_REGISTER, sync, sizeof sync, 3) || bank_read(rf, READ_REGISTER, (uint8_t[]){0x07,0x40}, 2, check, 2) ||
        memcmp(check, sync + 2, 2) ||
        bank_write(rf, SET_BUFFER_BASE_ADDRESS, (uint8_t[]){0, 0}, 2, 3);
}
#endif
