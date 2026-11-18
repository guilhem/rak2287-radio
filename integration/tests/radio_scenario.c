/* Drive the actual repeater through the C driver and the HAL model. The wrapper
 * schedules air events around polls; every poll goes through the real driver. */
#define _POSIX_C_SOURCE 200809L
#include "sx1250_rx.h"
#include "tests/fake_hal.h"
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
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

/* Inject a preamble only once the real dispatcher has a send due. Each stage
 * advances through its ordinary recvRaw/isReceiving/startSendRaw callbacks. */
static bool loaded, queried, pending, handled, vetoed, sent, in_tx;
static unsigned busy_checks, attempts;
static int64_t first_busy_ms;
static const uint8_t ack[] = {0x0e,0,1,2,3,4}; // Direct ACK: received but not retransmitted.
static const uint8_t malformed[] = {0xff};      // PHY-valid, invalid MeshCore envelope.

static bool scenario(const char* name) {
    const char* current = getenv("ACTIVITY_SCENARIO");
    return current && strcmp(current, name) == 0;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

extern int __real_sx1250_rx_activity(void);
int __wrap_sx1250_rx_activity(void) {
    // The driver's independent last-moment guard also calls this symbol.
    if (in_tx || !getenv("ACTIVITY_SCENARIO")) return __real_sx1250_rx_activity();
    assert(loaded && !sent); // TX eligibility was reached after receiving the first flood.
    if (scenario("error")) {
        assert(!setenv("FAKE_RX_IRQ_ERROR","1",1));
        int rc = __real_sx1250_rx_activity();
        int saved = errno;
        assert(!unsetenv("FAKE_RX_IRQ_ERROR") && rc == -1);
        errno = saved;
        return rc;
    }
    if ((!queried || (scenario("force") && !vetoed)) &&
        !scenario("race") && !scenario("idle")) {
        if (scenario("crc"))
            assert(fake_air(0, 0x52)); // RxDone + HeaderValid + CRC error
        else if (scenario("terminal"))
            assert(fake_inject_rx(ack, sizeof ack));
        else
            assert(fake_rx_progress(0x04));
        pending = true;
    }
    queried = true;
    int rc = __real_sx1250_rx_activity();
    assert(rc >= 0);
    if (rc) {
        if (!busy_checks) first_busy_ms = monotonic_ms();
        ++busy_checks;
    }
    return rc;
}

static int activity_next(struct sx1250_rx_packet* pkt, int timeout_ms) {
    if (sent) {
        assert(saw_tx(first, sizeof first));
        printf("AIR ordered busy=%u veto=%u handled=%u attempts=%u elapsed=%lld\n",
               busy_checks,(unsigned)vetoed,(unsigned)handled,attempts,
               (long long)(first_busy_ms ? monotonic_ms() - first_busy_ms : 0));
        raise(SIGTERM);
    }
    if (!loaded) {
        assert(fake_inject_rx(first, sizeof first));
        loaded = true;
    } else if (pending && !scenario("lost")) {
        const uint8_t *frame = scenario("reject") ? malformed : ack;
        uint8_t size = scenario("reject") ? sizeof malformed : sizeof ack;
        if (!scenario("terminal") && !scenario("crc")) assert(fake_inject_rx(frame, size));
        int rc = __real_sx1250_rx_next(pkt, timeout_ms);
        if (scenario("crc")) assert(rc == 0);
        else assert(rc == 1 && pkt->size == size && !memcmp(pkt->payload, frame, size));
        pending = false;
        handled = true;
        return rc;
    }
    return __real_sx1250_rx_next(pkt, timeout_ms);
}

extern int __real_sx1250_tx_send(const uint8_t *payload, size_t len, unsigned timeout_ms);
int __wrap_sx1250_tx_send(const uint8_t *payload, size_t len, unsigned timeout_ms) {
    static bool injected;
    if (getenv("ACTIVITY_SCENARIO")) {
        assert(queried && !sent);
        ++attempts;
        if (scenario("race") && !vetoed) {
            assert(fake_rx_progress(0x04)); // Between dispatcher check and driver guard.
            pending = true;
        }
        in_tx = true;
        int rc = __real_sx1250_tx_send(payload, len, timeout_ms);
        in_tx = false;
        if (rc == -1 && errno == EAGAIN) {
            assert(!vetoed && (scenario("race") || scenario("force")));
            vetoed = true;
            return rc;
        }
        assert(rc == 0 && (handled || scenario("lost") || scenario("idle")));
        sent = true;
        return rc;
    }
    if (!injected && getenv("INJECT_BEFORE_TX")) {
        assert(fake_inject_rx(first, sizeof first));
        injected = true;
    }
    return __real_sx1250_tx_send(payload, len, timeout_ms);
}

int __wrap_sx1250_rx_next(struct sx1250_rx_packet *pkt, int timeout_ms) {
    static unsigned phase;
    if (getenv("ACTIVITY_SCENARIO")) return activity_next(pkt, timeout_ms);
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
