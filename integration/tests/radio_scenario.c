/* Drive the actual repeater through the C driver and the HAL model. The wrapper
 * schedules air events around polls; every poll goes through the real driver. */
#include "sx1250_rx.h"
#include "tests/fake_hal.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int __real_sx1250_rx_next(struct sx1250_rx_packet *pkt, int timeout_ms);

static const uint8_t first[] = {0x15, 0x40, 0xaa, 1, 2, 3, 4, 5, 6, 7};
static const uint8_t second[] = {0x15, 0x40, 0xbb, 9, 8, 7, 6, 5, 4, 3, 2};

static bool saw_tx(const uint8_t *original, size_t size) {
    uint8_t fifo[255];
    size_t n = fake_last_tx(fifo, sizeof fifo);
    if (n != size + 2 || fifo[0] != original[0] || fifo[1] != 0x41 ||
        memcmp(fifo + 4, original + 2, size - 2)) return false;
    printf("FAKE RF0 TxDone path=%02x%02x payload=", fifo[2], fifo[3]);
    for (size_t i = 4; i < n; i++) printf("%02x", fifo[i]);
    puts("");
    return true;
}

extern int __real_sx1250_tx_send(const uint8_t *payload, size_t len, unsigned timeout_ms);
int __wrap_sx1250_tx_send(const uint8_t *payload, size_t len, unsigned timeout_ms) {
    static bool injected;
    if (!injected && getenv("INJECT_BEFORE_TX")) {
        assert(fake_inject_rx(first, sizeof first));
        injected = true;
    }
    return __real_sx1250_tx_send(payload, len, timeout_ms);
}

int __wrap_sx1250_rx_next(struct sx1250_rx_packet *pkt, int timeout_ms) {
    static unsigned phase;
    switch (phase) {
    case 0:
        assert(fake_inject_rx(first, sizeof first));
        ++phase;
        break;
    case 1:
        if (saw_tx(first, sizeof first)) {
            assert(fake_inject_rx(second, sizeof second));
            ++phase;
        }
        break;
    case 2:
        if (saw_tx(second, sizeof second)) {
            assert(fake_inject_rx(first, sizeof first));
            ++phase;
        }
        break;
    case 3:
        /* Let the dispatcher process the duplicate before stopping. */
        ++phase;
        break;
    case 4:
        raise(SIGTERM);
        ++phase;
    }
    return __real_sx1250_rx_next(pkt, timeout_ms);
}
