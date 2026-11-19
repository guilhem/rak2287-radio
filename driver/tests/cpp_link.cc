#include "sx1250_board.h"
#include "sx1250_rx.h"
#include "sx1250_tx.h"

#include <cerrno>

int main() {
    sx1250_rx_config config{};
    sx1250_rx_packet packet{};
    if (sx1250_board_acquire(nullptr, 0) != -1 || errno != EINVAL) return 1;
    if (sx1250_rx_open(nullptr, &config) != -1 || errno != EINVAL) return 1;
    if (sx1250_rx_next(&packet, 0) != -1 || errno != ENODEV) return 1;
    if (sx1250_rx_activity() != -1 || errno != ENODEV) return 1;
    if (sx1250_tx_send(nullptr, 0, 1) != -1 || errno != ENODEV) return 1;
    return 0;
}
