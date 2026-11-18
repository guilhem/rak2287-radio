/* Private packet-engine operations, also used by the diagnostics. */
#ifndef SX1250_PACKET_H
#define SX1250_PACKET_H
#include "sx1250_rx.h"
enum { rx_done = 2, preamble_detected = 4, header_valid = 0x10, header_error = 0x20, crc_error = 0x40, rx_timeout = 0x200 };

static inline int freeze_rx_irq(unsigned *irq) {
    const unsigned terminal = header_error | crc_error | rx_done | rx_timeout;
    uint8_t flags[2];
    if (write_cmd(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2)) return -1;
    unsigned frozen = ((unsigned)flags[0] << 8) | flags[1];
    if (radio_verbose && frozen != *irq) printf("IRQ before standby=0x%04x frozen=0x%04x\n", *irq, frozen);
    /* Missing latched terminal bits or contradictory completion forbid using
     * the frozen FIFO as evidence for the originally observed event. */
    if (((frozen & *irq & terminal) != (*irq & terminal)) ||
        ((frozen & (rx_done | rx_timeout)) == (rx_done | rx_timeout))) {
        if (radio_verbose) puts("Discarded an inconsistent IRQ snapshot.");
        *irq = 0;
    } else {
        *irq = frozen;
    }
    return 0;
}
#endif
