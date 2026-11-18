#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sx1250_rx.h"
#include "sx1250_tx.h"
#include "fake_hal.h"

static int64_t ms;
static bool clock_fault;
int __wrap_clock_gettime(clockid_t id, struct timespec *out) {
    assert(id == CLOCK_MONOTONIC);
    if (clock_fault) { errno = EIO; return -1; }
    *out = (struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    return 0;
}

static void busy(void) { assert(sx1250_rx_activity() == 1); }
static void idle(void) { assert(sx1250_rx_activity() == 0); }
static void completed(const uint8_t *payload, uint8_t size) {
    struct sx1250_rx_packet pkt;
    busy();
    assert(sx1250_rx_next(&pkt, 0) == 1 && pkt.size == size);
    assert(!memcmp(payload, pkt.payload, size));
    idle();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("FAKE_TX", "1", 1);
    setenv("IDLE", "1", 1);
    struct sx1250_rx_config c = {869618000,62500,16,8,8,0x12,true,false,0};
    struct sx1250_rx_packet pkt;
    const uint8_t data[] = {0xbe, 0, 0xef};
    uint8_t maximum[255];
    for (size_t i = 0; i < sizeof maximum; i++) maximum[i] = (uint8_t)(3 * i);
    assert(sx1250_rx_activity() < 0 && errno == ENODEV);
    assert(!sx1250_rx_open(argv[1], &c));
    idle();
    const unsigned errors[] = {0x20, 0x42, 0x0200};
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        assert(fake_air(0, errors[i]));
        if (errors[i] == 0x200) setenv("FAKE_RX_IRQ_STATUS", "0xa6", 1);
        busy(); busy();
        unsetenv("FAKE_RX_IRQ_STATUS");
        assert(sx1250_rx_next(&pkt, 0) == 0);
        idle();
    }
    setenv("FAKE_RX_IRQ_STATUS", "0xa6", 1);
    assert(sx1250_rx_activity() < 0 && errno == EIO); /* timeout status without bit */
    setenv("FAKE_RX_IRQ_STATUS", "0xb2", 1);
    assert(sx1250_rx_activity() < 0 && errno == EIO); /* standby without event */
    unsetenv("FAKE_RX_IRQ_STATUS");
    assert(fake_rx_progress(0x08)); /* FSK sync alone is not a LoRa event. */
    idle();
    assert(fake_rx_progress(4)); busy();
    ms = 80; busy();
    assert(sx1250_tx_send(data, sizeof data, 10) < 0 && errno == EAGAIN);
    assert(sx1250_rx_next(&pkt, 0) == 0);
    ms = 163; busy();
    assert(fake_rx_progress(0x10)); busy();
    ms += 2200; busy(); /* SF8/BW62.5 255 bytes/CR4/8 max frame takes 2.246 s. */
    assert(fake_inject_rx(maximum, sizeof maximum));
    ms += 10000;
    completed(maximum, sizeof maximum);

    assert(fake_rx_progress(4)); busy();
    ms += 164;
    setenv("FAKE_RX_CLEAR_ERROR", "1", 1);
    assert(sx1250_rx_activity() < 0 && errno == EIO);
    unsetenv("FAKE_RX_CLEAR_ERROR"); idle();
    assert(fake_rx_progress(4)); busy();
    ms += 164;
    setenv("FAKE_RX_STUCK_PREAMBLE", "1", 1);
    busy(); /* Immediate reassertion earns exactly one bounded window. */
    ms += 164; idle(); ms += 10000; idle();
    unsetenv("FAKE_RX_STUCK_PREAMBLE");
    assert(fake_inject_rx(data, sizeof data)); completed(data, sizeof data);

    assert(fake_rx_progress(0x14)); busy();
    ms += 10000;
    setenv("FAKE_RX_DONE_ON_CLEAR", "1", 1);
    busy(); /* An RxDone racing selective W1C keeps both header and FIFO. */
    unsetenv("FAKE_RX_DONE_ON_CLEAR");
    assert(sx1250_rx_next(&pkt, 0) == 1 && pkt.size == 255 && pkt.payload[0] == 237);
    idle();
    setenv("FAKE_RX_DONE_ON_STANDBY", "1", 1);
    assert(sx1250_tx_send(data, sizeof data, 10) < 0 && errno == EAGAIN);
    unsetenv("FAKE_RX_DONE_ON_STANDBY");
    assert(sx1250_rx_next(&pkt, 0) == 1 && pkt.payload[0] == 237);
    idle();
    setenv("FAKE_RX_IRQ_ERROR", "1", 1);
    assert(sx1250_rx_activity() < 0 && errno == EIO);
    assert(sx1250_tx_send(data, sizeof data, 10) < 0 && errno == EIO);
    unsetenv("FAKE_RX_IRQ_ERROR");
    assert(fake_rx_progress(4));
    clock_fault = true; assert(sx1250_rx_activity() < 0 && errno == EIO);
    clock_fault = false; busy();
    assert(!sx1250_rx_close());
    assert(sx1250_rx_activity() < 0 && errno == ENODEV);

    /* When HeaderValid from an expired frame remains latched, a new long
     * preamble needs the full receive window even without a new header edge. */
    c.preamble_symbols = 2048;
    assert(!sx1250_rx_open(argv[1], &c));
    assert(fake_rx_progress(0x10)); busy(); ms += 3000; idle();
    assert(fake_rx_progress(4)); busy(); ms += 9000; busy();
    assert(fake_inject_rx(data, sizeof data)); completed(data, sizeof data);
    assert(!sx1250_rx_close());

    c.preamble_symbols = 16; c.implicit_length = sizeof data;
    assert(!sx1250_rx_open(argv[1], &c));
    assert(fake_rx_progress(0x10)); idle();
    assert(fake_rx_progress(4)); busy();
    ms += 150; busy();
    assert(fake_inject_rx(data, sizeof data)); completed(data, sizeof data);
    assert(fake_rx_progress(4)); busy(); ms += 300; idle();
    assert(fake_inject_rx(data, sizeof data)); completed(data, sizeof data);
    assert(!sx1250_rx_close());

    c.preamble_symbols = 65535; c.spreading_factor = 12; c.bandwidth_hz = 7812;
    assert(!sx1250_rx_open(argv[1], &c));
    assert(fake_rx_progress(4)); busy();
    ms += 9 * 3600000; busy(); ms += 3600000; idle();
    assert(!sx1250_rx_close());
}
