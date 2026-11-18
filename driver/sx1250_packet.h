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

static inline int read_rx_fifo(struct sx1250_rx_packet *packet, unsigned *start_address,
                               const struct sx1250_rx_config *config) {
    uint8_t desc[2], metrics[3];
    if (read_cmd(GET_RX_BUFFER_STATUS, NULL, 0, desc, 2) ||
        read_cmd(GET_PACKET_STATUS, NULL, 0, metrics, 3)) return -1;
    packet->implicit_header = config && config->implicit_length;
    uint8_t size = desc[0];
    if (packet->implicit_header) {
        /* LoRaMac-node SX126xGetRxBufferStatus uses REG_LR_PAYLOADLENGTH for
         * fixed-length LoRa. The descriptor still supplies the start offset. */
        if (reg_read(0x0702, &size, 1)) return -1;
        if (size != config->implicit_length) {
            packet->size = 0; /* Discard this event before any FIFO read. */
            return 0;
        }
    }
    unsigned start = desc[1];
    for (unsigned i = 0; i < size;) {
        uint8_t offset = start + i;
        size_t n = size - i;
        if (n > 128) n = 128;
        if (read_cmd(READ_BUFFER, &offset, 1, packet->payload + i, n)) return -1;
        i += n;
    }
    packet->size = size;
    if (packet->implicit_header) {
        packet->coding_rate = config->coding_rate;
        packet->crc_present = config->require_crc;
    } else {
        uint8_t cr, crc;
        if (reg_read(0x0749, &cr, 1) || reg_read(0x076b, &crc, 1)) return -1;
        packet->coding_rate = 4 + ((cr >> 4) & 7);
        packet->crc_present = (crc & 0x10) != 0;
    }
    packet->rssi_dbm = -metrics[0] / 2.0f;
    packet->snr_db = (int8_t)metrics[1] / 4.0f;
    *start_address = start;
    return 0;
}
#endif
