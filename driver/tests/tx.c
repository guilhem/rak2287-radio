#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sx1250_rx.h"
#include "sx1250_tx.h"
#include "fake_hal.h"

static void check_rx(void) {
    const uint8_t bytes[] = {0xda, 0x00, 0xff, 0x52};
    struct sx1250_rx_packet packet;
    assert(fake_inject_rx(bytes, sizeof bytes));
    assert(sx1250_rx_next(&packet, 100) == 1 && packet.size == sizeof bytes);
    assert(!memcmp(bytes, packet.payload, sizeof bytes));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("FAKE_TX", "1", 1); setenv("IDLE", "1", 1);
    const struct sx1250_rx_config c = {869618000, 62500, 16, 8, 8, 0x12, true, false, 0};
    uint8_t payload[255], out[255];
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(17 * i + 1);
    assert(sx1250_tx_send(payload, sizeof payload, 100) < 0 && errno == ENODEV);
    assert(!sx1250_rx_open(argv[1], &c));
    assert(sx1250_tx_send(payload, 256, 100) < 0 && errno == EINVAL);
    assert(sx1250_tx_send(payload, 1, 0) < 0 && errno == EINVAL);
    assert(fake_inject_rx(payload, 32));
    assert(sx1250_tx_send(payload, sizeof payload, 100) < 0 && errno == EAGAIN);
    assert(!fake_last_tx(out, sizeof out));
    struct sx1250_rx_packet packet;
    assert(sx1250_rx_next(&packet, 100) == 1 && packet.size == 32);
    assert(!memcmp(packet.payload, payload, 32));
    assert(!sx1250_tx_send(payload, sizeof payload, 100));
    assert(fake_last_tx(out, sizeof out) == sizeof payload && !memcmp(out, payload, sizeof out));
    check_rx();
    setenv("FAKE_TX_FAULT", "late_done", 1); /* IRQ latched; host read delayed 20 ms. */
    assert(!sx1250_tx_send(payload, 1, 5));
    check_rx();
    const char *normal_errors[] = {"timeout", "deadline", "conflict", "write", "readback", "pinmode", "status"};
    for (unsigned i=0; i < sizeof normal_errors / sizeof *normal_errors; ++i) {
        setenv("FAKE_TX_FAULT", normal_errors[i], 1);
        assert(sx1250_tx_send(payload, 1, 10) < 0 && errno == (i < 2 ? ETIMEDOUT : EIO));
        check_rx();
    }
    assert(!sx1250_rx_close());
    const char *restore_errors[] = {"rearm", "clock"};
    for (unsigned i=0; i < sizeof restore_errors / sizeof *restore_errors; ++i) {
        setenv("FAKE_TX_FAULT", restore_errors[i], 1);
        assert(!sx1250_rx_open(argv[1], &c));
        assert(sx1250_tx_send(payload, 1, 100) < 0 && errno == EIO);
        assert(fake_last_tx(out, sizeof out) == 1 && out[0] == payload[0]);
        assert(sx1250_rx_activity() < 0 && errno == EIO);
        assert(sx1250_rx_next(&packet, 0) < 0 && errno == EIO);
        assert(sx1250_tx_send(payload, 1, 100) < 0 && errno == EIO);
        assert(!sx1250_rx_close());
    }
    unsetenv("FAKE_TX_FAULT");
    assert(!sx1250_rx_open(argv[1], &c)); check_rx(); assert(!sx1250_rx_close());
}
