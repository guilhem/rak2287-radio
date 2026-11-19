#ifndef SX1250_TX_HANDOFF_H
#define SX1250_TX_HANDOFF_H
#include "sx1250_tx.h"

enum {
    SX1250_HANDOFF_WARM_PREPARE = 1,
    SX1250_HANDOFF_WARM_RESTORE = 2,
    SX1250_HANDOFF_DRY_RUN = 4,
};

/* Diagnostic entry point. Flags zero selects the measured cold
 * path. A failed warm transition returns an error even after cold recovery. */
int sx1250_tx_probe_handoff(const uint8_t *payload, size_t length, unsigned timeout_ms,
                           unsigned flags, struct sx1250_handoff_report *report);
#endif
