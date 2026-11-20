#include "sx1250_board.h"
#include "sx1250_lora.h"

bool sx1250_lora_profile_valid(const struct sx1250_rx_config *c) {
    return c && c->spreading_factor >= 5 && c->spreading_factor <= 12 &&
           c->coding_rate >= 5 && c->coding_rate <= 8 && bw_code(c->bandwidth_hz) >= 0 &&
           c->preamble_symbols >= (c->spreading_factor <= 6 ? 12 : 8) &&
           sx1250_image_band(c->frequency_hz);
}
