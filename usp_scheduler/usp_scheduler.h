#ifndef USP_SCHEDULER_H
#define USP_SCHEDULER_H

#include <stddef.h>
#include "sx1250_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

struct usp_rx_profile {
    const char *name;
    struct sx1250_rx_config radio;
    unsigned duration_ms, max_extension_ms, priority;
};

struct usp_rx_hooks {
    int (*tune)(void *ctx, const struct sx1250_rx_config *radio, bool force);
    int (*next)(void *ctx, struct sx1250_rx_packet *pkt, int timeout_ms);
    int (*drain)(void *ctx, struct sx1250_rx_packet *pkt);
    int (*activity)(void *ctx);
    bool (*stop)(void *ctx);
    void (*packet)(void *ctx, const struct usp_rx_profile *profile, uint64_t monotonic_us,
                   const struct sx1250_rx_packet *pkt);
    void (*slot)(void *ctx, const struct usp_rx_profile *profile, uint64_t monotonic_us,
                 uint64_t dwell_us, uint64_t switch_us);
};

/* Owns one or two receive profiles in a single synchronous RAC event loop.
 * The caller owns opening on the first tune and closing after return. On
 * EAGAIN, tune preserves the old profile and its pending packet. After a
 * forced tune returns EAGAIN, RF1 is in standby and drain() reads the
 * terminal event without rearming. drain() returns 1 for a frame, 0 for an
 * invalid event, or -1/errno, and is required when running two profiles.
 * A forced tune can discard an incomplete reception.
 * Smaller priority values run first in each cycle. Ties use config order.
 * slot timestamps mark the end of each slot and switch_us measures the
 * tune transaction that armed it, including a terminal drain at handoff.
 * Dwell spans that arm to begin of next tune or shutdown and includes
 * ordinary packet drains. Returns 0 at stop or -1/errno. */
int usp_rx_run(const struct usp_rx_profile *profiles, size_t count, const struct usp_rx_hooks *hooks, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
