/* Offline only: real SX1250 SPI encoding and wait calls, simulated register bus,
 * packet airtime, and continuously-receiving frame acquisition. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/spi/spidev.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include "fake_hal.h"
#include "loragw_hal.h"
#include "loragw_spi.h"
#include "loragw_sx1250.h"
#include "loragw_sx1302.h"
#include "sx1250_spi.h"
#include "sx1250_rx.h"
#include "sx1250_tx.h"

#include "sx1250_tx_warm.h"
int bench_radio_w(sx1250_op_code_t, uint8_t*, uint16_t, uint8_t);
int bench_radio_r(sx1250_op_code_t, uint8_t*, uint16_t, uint8_t);
static bool warm, wall, listening, tracking, acquiring, captured, active, skip_reset;
static int64_t us, start_us, wall_origin, air_us, tx_us, wall_tx_us;
static int64_t arrival = INT64_MAX, finish = INT64_MAX;
static unsigned resets, transactions, transmissions, captures;
static bool injected_fault;
static unsigned programmed_ticks;
static int64_t observed_timeout_us;
void fake_bench_latch_tx(void);
static const char *fault;
static const uint8_t rx_frame[] = {0x15,0x00,0xb5,0x09,0x0a,0x08,0x06,0x04,0x02,0x01};
static int64_t wall_now(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
enum marker { STANDBY, TRANSMIT, DONE, REARM, RETURNED, MARKERS };
static int64_t marks[MARKERS], wall_marks[MARKERS];
static void mark(enum marker m) {
    if (tracking && !marks[m]) {
        marks[m] = us - start_us;
        wall_marks[m] = wall_now() - wall_origin;
    }
}
/* SF8/BW62500/CR4/8, 16-symbol preamble, explicit header and CRC. */
static int64_t airtime(size_t n) {
    int blocks = ((int)n * 8 + 12 + 31) / 32;
    return 4096 * (28 + 8 * blocks) + 1024;
}
static void advance(int64_t duration) {
    assert(duration >= 0);
    if (wall && active && duration) {
        struct timespec d = {.tv_sec = duration / 1000000, .tv_nsec = (duration % 1000000) * 1000};
        while (nanosleep(&d, &d)) assert(errno == EINTR);
    }
    int64_t end = us + duration;
    if (arrival <= end) {
        assert(arrival >= us);
        acquiring = listening;
        finish = arrival + airtime(sizeof rx_frame);
        arrival = INT64_MAX;
    }
    if (finish <= end) {
        captured = acquiring && listening && fake_inject_rx(rx_frame, sizeof rx_frame);
        acquiring = false;
        finish = INT64_MAX;
    }
    us = end;
    fake_bench_latch_tx();
}
void wait_ms(unsigned long ms) { advance((int64_t)ms * 1000); }
int bench_clock_gettime(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC);
    int64_t t = wall ? wall_now() : us;
    *out = (struct timespec){.tv_sec=t/1000000, .tv_nsec=(t%1000000)*1000};
    return 0;
}
int bench_nanosleep(const struct timespec *d, struct timespec *remaining) {
    (void)remaining;
    advance(d->tv_sec * INT64_C(1000000) + d->tv_nsec / 1000);
    return 0;
}
unsigned bench_due_tx_irq(void) {
    int64_t elapsed_ns = 1000 * (wall ? wall_now() - wall_tx_us : us - tx_us);
    int64_t timeout_ns = INT64_C(15625) * programmed_ticks;
    int64_t air_ns = air_us * 1000;
    if (programmed_ticks && timeout_ns <= air_ns)
        return elapsed_ns >= timeout_ns ? 0x200u : 0u;
    return elapsed_ns >= air_ns ? 1u : 0u;
}
lgw_com_type_t lgw_com_type(void) { return LGW_COM_SPI; }
int sx1250_com_w(lgw_com_type_t type, void *target, uint8_t mux, sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(type == LGW_COM_SPI); return sx1250_spi_w(target,mux,op,p,n);
}
int sx1250_com_r(lgw_com_type_t type, void *target, uint8_t mux, sx1250_op_code_t op, uint8_t *p, uint16_t n) {
    assert(type == LGW_COM_SPI); return sx1250_spi_r(target,mux,op,p,n);
}
/* Only the pinned SPI translation unit calls this substitute for ioctl. */
int bench_ioctl(int fd, unsigned long request, ...) {
    assert(fd == -1 && request == SPI_IOC_MESSAGE(1));
    va_list args;
    va_start(args, request);
    const struct spi_ioc_transfer *k = va_arg(args, struct spi_ioc_transfer *);
    va_end(args);
    uint8_t *out = (uint8_t *)(uintptr_t)k->tx_buf, *in = (uint8_t *)(uintptr_t)k->rx_buf;
    assert(k->len > 2 && (out[0] == LGW_SPI_MUX_TARGET_RADIOA || out[0] == LGW_SPI_MUX_TARGET_RADIOB));
    uint8_t rf = out[0] == LGW_SPI_MUX_TARGET_RADIOB;
    sx1250_op_code_t op = out[1];
    int rc;
    if (tracking) ++transactions;
    if (in) {
        memcpy(in, out, k->len);
        rc = bench_radio_r(op, in+2, k->len-2, rf);
        if (!rc && tracking && !rf && fault && !injected_fault) {
            if (!strcmp(fault, "warm_entry") && op == READ_REGISTER &&
                out[2] == 5 && out[3] == 0x87 && !transmissions) {
                in[5] ^= 1; // First bridge-pin readback disagrees with the entry contract.
                injected_fault = true;
            } else if (!strcmp(fault, "slow_restore") && marks[DONE] && op == GET_IRQ_STATUS) {
                advance(501000); // Delayed response inside the warm recovery guard.
                injected_fault = true;
            } else if (!strcmp(fault, "warm_restore") && marks[DONE] && op == GET_IRQ_STATUS) {
                in[4] |= 0x40; // One stale IRQ after the warm sequence cleared all flags.
                injected_fault = true;
            }
        }
        if (!rc && rf == 0 && op == GET_IRQ_STATUS) {
            if (in[4] & 1) mark(DONE);
            if ((in[3] & 2) && !observed_timeout_us) observed_timeout_us = us - tx_us;
        }
    } else {
        rc = bench_radio_w(op, out+2, k->len-2, rf);
        if (!rc && rf == 0 && op == SET_TX) {
            assert(k->len == 5);
            programmed_ticks = ((unsigned)out[2] << 16) | ((unsigned)out[3] << 8) | out[4];
            ++transmissions; mark(TRANSMIT); tx_us = us; wall_tx_us = wall_now();
        }
        if (!rc && rf == 1 && op == SET_STANDBY) {
            listening = acquiring = false;
            mark(STANDBY);
        }
        if (!rc && rf == 1 && op == SET_RX) {
            listening = true;
            mark(REARM);
        }
    }
    return rc ? -1 : (int)k->len;
}
int __real_sx1302_radio_reset(uint8_t, lgw_radio_type_t);
int __wrap_sx1302_radio_reset(uint8_t rf, lgw_radio_type_t type) {
    if (tracking && !rf) {
        ++resets;
        if (skip_reset) return 0;
    }
    return __real_sx1302_radio_reset(rf, type);
}
int __real_sx1250_setup(uint8_t, uint32_t, bool);
int __wrap_sx1250_setup(uint8_t rf, uint32_t hz, bool single) {
    static bool initial = true;
    if (warm && initial) {
        assert(!rf && !tracking);
        ++captures;
        if (sx1250_tx_warm_capture_reset_sim()) return -1;
    }
    initial = false;
    return __real_sx1250_setup(rf, hz, single);
}
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    bool repeat = !strcmp(argv[1], "warm-reuse");
    warm = repeat || !strcmp(argv[1], "warm");
    fault = getenv("BENCH_FAULT");
    skip_reset = !strcmp(argv[1], "skip");
    wall = !strcmp(argv[2], "wall");
    size_t size = strtoul(argv[3], NULL, 10);
    assert(size >= 1 && size <= 255);
    uint8_t payload[255], sent[255];
    memset(payload, 0x4a, size);
    air_us = airtime(size);
    assert(setenv("FAKE_TX","1",1) == 0 && setenv("IDLE","1",1) == 0);
    struct sx1250_rx_config cfg = {869618000,62500,16,8,8,0x12,true,false,0};
    assert(!sx1250_rx_open(argv[5], &cfg));
    int64_t offset = strtoll(argv[4], NULL, 10);
    if (fault && !strcmp(fault, "tx_timeout")) assert(setenv("FAKE_TX_FAULT","timeout",1) == 0);
    start_us = us + 2 * airtime(sizeof rx_frame);
    if (strcmp(argv[4], "none")) arrival = start_us + offset;
    advance(start_us - us);
    tracking = active = true;
    wall_origin = wall_now();
    unsigned timeout_ms = getenv("BENCH_TIMEOUT_MS") ? (unsigned)strtoul(getenv("BENCH_TIMEOUT_MS"), NULL, 10) : 3000;
    int rc = warm ? sx1250_tx_send_warm_sim(payload,size,timeout_ms) : sx1250_tx_send(payload,size,timeout_ms);
    int call_errno = errno;
    mark(RETURNED);
    tracking = active = false;
    bool short_timer = timeout_ms * INT64_C(1000) < air_us;
    if (transmissions) assert(programmed_ticks == 64u * timeout_ms);
    if (short_timer || (fault && !strcmp(fault, "tx_timeout"))) {
        assert(rc == -1 && call_errno == ETIMEDOUT && transmissions == 1 && resets == (warm ? 1u : 2u) &&
               !marks[DONE] && marks[REARM] && marks[RETURNED] > marks[REARM]);
        assert(fake_inject_rx(rx_frame, sizeof rx_frame));
    } else if (skip_reset || (fault && !strcmp(fault, "warm_entry"))) {
        assert(rc == -1 && !marks[TRANSMIT] && transmissions == 0);
        assert(resets == (skip_reset ? 2u : 1u));
        if (!skip_reset) assert(injected_fault && fake_inject_rx(rx_frame, sizeof rx_frame));
    } else {
        assert(!rc && transmissions == 1 && marks[STANDBY] && marks[TRANSMIT] > marks[STANDBY] &&
               marks[DONE] > marks[TRANSMIT] && marks[REARM] > marks[DONE] && marks[RETURNED] > marks[REARM]);
        assert(resets == (warm ? (unsigned)!!fault : 2u));
        assert(fake_last_tx(sent, sizeof sent) == size && !memcmp(sent, payload, size));
        if (fault) assert(injected_fault && fake_inject_rx(rx_frame, sizeof rx_frame));
    }
    if (arrival != INT64_MAX) advance(arrival - us);
    if (finish != INT64_MAX) advance(finish - us);
    struct sx1250_rx_packet pkt;
    if (!skip_reset) {
        int received = sx1250_rx_next(&pkt, 0);
        if (fault || short_timer) assert(received == 1);
        else assert(received == (int)captured);
        if (received) assert(pkt.size == sizeof rx_frame && !memcmp(pkt.payload,rx_frame,pkt.size));
    }
    int64_t *stamp = wall ? wall_marks : marks;
    printf("BENCH standby=%"PRId64" set_tx=%"PRId64" done=%"PRId64" set_rx=%"PRId64" returned=%"PRId64
           " rx=%u resets=%u transfers=%u air=%"PRId64" timeout_irq=%"PRId64"\n",stamp[STANDBY],stamp[TRANSMIT],stamp[DONE],
           stamp[REARM],stamp[RETURNED],(unsigned)captured,resets,transactions,air_us,observed_timeout_us);
    if (repeat) {
        tracking = true;
        payload[0] ^= 0xff;
        assert(!sx1250_tx_send_warm_sim(payload,size,3000));
        tracking = false;
        assert(transmissions == 2 && !resets && captures == 1);
        assert(fake_last_tx(sent, sizeof sent) == size && !memcmp(sent, payload, size));
        assert(fake_inject_rx(rx_frame, sizeof rx_frame));
        assert(sx1250_rx_next(&pkt,0) == 1 && pkt.size == sizeof rx_frame && !memcmp(pkt.payload,rx_frame,pkt.size));
    }
    assert(!sx1250_rx_close());
}
