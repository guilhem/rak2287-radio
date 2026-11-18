/* RAK2287 RF1 packet-engine experiment. Start with an externally reset board.
 * Profile adapted from Lora-net/SWSD003, sx126x/common/apps_configuration.h.
 * The test sender uses 869.618 MHz, BT 0.5, and a seven-byte "GFSK-RX" payload.
 * Each run opens one bounded, single-shot RX window. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../sx1250_board.h"
static const bool radio_verbose = false;
#include "../sx1250_io.h"
#include "../sx1250_packet.h"

enum { frequency_hz = 869618000, max_payload = 7, preamble_irq = 4, sync_irq = 8 };
static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }

static int setup_gfsk(void) {
    /* Src: sx126x_set_gfsk_mod_params / sx126x_set_gfsk_pkt_params in the
     * Semtech sx126x driver. Bitrate = 32*32 MHz/50000, deviation = 25 kHz.
     * 32-bit preamble, 16-bit detector, 40-bit sync, variable-length packet,
     * inverted one-byte CRC, no address filter and no whitening. */
    uint8_t mod[] = {0x00,0x50,0x00, 0x09,0x0b, 0x00,0x66,0x66};
    uint8_t pkt[] = {0,32, 5,40, 0,1,max_payload, 4,0};
    /* Consecutive registers: CRC seed, polynomial, then the eight sync bytes.
     * SWSD003 passes the low 16 bits of 0x01234567 for both CRC settings. */
    uint8_t pattern[] = {0x06,0xbc, 0x45,0x67,0x45,0x67, 0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
    uint8_t actual[sizeof pattern - 2], type, flags[2];
    unsigned mask = rx_done | preamble_irq | sync_irq | crc_error | header_error | rx_timeout;
    if (write_cmd(SET_PACKET_TYPE, (uint8_t[]){PACKET_TYPE_GFSK}, 1, 3) ||
        read_cmd((sx1250_op_code_t)0x11, NULL, 0, &type, 1) || type != PACKET_TYPE_GFSK ||
        write_cmd(SET_MODULATION_PARAMS, mod, sizeof mod, 3) ||
        write_cmd(SET_PACKET_PARAMS, pkt, sizeof pkt, 3) ||
        write_cmd(WRITE_REGISTER, pattern, sizeof pattern, 3) ||
        reg_read(0x06bc, actual, sizeof actual) || memcmp(actual, pattern + 2, sizeof actual) ||
        write_cmd(SET_BUFFER_BASE_ADDRESS, (uint8_t[]){0,0}, 2, 3) ||
        write_cmd(SET_DIO_IRQ_PARAMS, (uint8_t[]){mask >> 8, mask, 0,0,0,0,0,0}, 8, 3) ||
        write_cmd(CLR_IRQ_STATUS, (uint8_t[]){0xff,0xff}, 2, 3) ||
        read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2) || flags[0] || flags[1]) {
        fputs("GFSK configuration, readback or IRQ clear failed.\n", stderr);
        return -1;
    }
    puts("RF1 GFSK type, CRC seed/poly 0x4567 and sync register readback passed.");
    return 0;
}

static int print_fifo(void) {
    uint8_t desc[2], metrics[3], data[max_payload];
    if (read_cmd(GET_RX_BUFFER_STATUS, NULL, 0, desc, 2) ||
        read_cmd(GET_PACKET_STATUS, NULL, 0, metrics, 3)) return -1;
    printf("RxDone: FIFO length=%u offset=%u GFSK status=0x%02x RSSI_sync=%.1f RSSI_avg=%.1f dBm\n",
           desc[0], desc[1], metrics[0], -metrics[1]/2.0, -metrics[2]/2.0);
    /* GetPacketStatus: bits 5..2 are address, CRC, length and abort errors.
     * Bit 1 means received, bit 0 means sent. */
    if (desc[0] == 0 || desc[0] > sizeof data || (metrics[0] & 0x3f) != 0x02) {
        puts("Rejected packet metadata; no verified payload.");
        return 0;
    }
    if (read_cmd(READ_BUFFER, &desc[1], 1, data, desc[0])) return -1;
    printf("RF1 GFSK packet payload=");
    for (unsigned i = 0; i < desc[0]; ++i) printf("%02x", data[i]);
    printf(" reference_match=%s\n", desc[0] == max_payload && !memcmp(data, "GFSK-RX", max_payload) ? "yes" : "no");
    return 0;
}

static int receive(unsigned seconds) {
    struct timespec start, now;
    uint32_t ticks = (seconds + 2) * 64000u;  /* hardware backup beyond the host deadline */
    uint8_t status_byte;
    if (clock_gettime(CLOCK_MONOTONIC, &start) ||
        write_cmd(SET_RX, (uint8_t[]){ticks>>16, ticks>>8, ticks}, 3, 0) ||
        sx1250_reg_r(GET_STATUS, &status_byte, 1, chain) || !status_ok(status_byte)) return -1;
    printf("RF1 status after SetRx=0x%02x; listening %u s at %u Hz, 50 kb/s, deviation 25 kHz, RX BW 117.3 kHz.\n",
           status_byte, seconds, frequency_hz);
    fflush(stdout);
    unsigned last = 0;
    const unsigned terminal_mask = rx_done | crc_error | header_error | rx_timeout;
    bool first_poll = true;
    while (!interrupted) {
        uint8_t flags[2];
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9;
        bool expired = elapsed >= seconds;
        unsigned irq = last;
        if (!expired) {
            if (read_cmd(GET_IRQ_STATUS, NULL, 0, flags, 2)) return -1;
            irq = ((unsigned)flags[0] << 8) | flags[1];
        }
        if (first_poll && !expired && ((status_byte >> 4) & 7) != 5 && !(irq & terminal_mask)) {
            fputs("RF1 did not enter receive mode.\n", stderr);
            return -1;
        }
        first_poll = false;
        if (irq != last) { printf("IRQ=0x%04x after %.3f s\n", irq, elapsed); last = irq; }
        if (expired || (irq & terminal_mask)) {
            /* A packet may complete between the last poll and the deadline. */
            if (freeze_rx_irq(&irq)) return -1;
            if (interrupted) break;
            if ((irq & rx_done) && !(irq & (header_error | crc_error | rx_timeout))) return print_fifo();
            if (expired && !(irq & terminal_mask)) {
                printf("Window elapsed. Frozen IRQ=0x%04x; no GFSK packet observed.\n", irq);
                return 0;
            }
            printf("Terminal IRQ=0x%04x; no verified GFSK payload.\n", irq);
            return 0;
        }
        wait_ms(10);
    }
    printf("Window interrupted. Last polled IRQ=0x%04x.\n", last);
    return -1;
}

int main(int argc, char **argv) {
    char *end;
    long seconds = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (seconds < 1 || seconds > 120 || *end) {
        fprintf(stderr, "Usage: %s SECONDS (1..120)\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    if (sx1250_board_acquire("/dev/spidev0.0", frequency_hz)) {
        perror("RF1 acquisition");
        return 1;
    }
    int failed = setup_gfsk() || interrupted || receive((unsigned)seconds);
    if (sx1250_board_release()) { perror("RF1 release"); failed = 1; }
    return failed;
}
