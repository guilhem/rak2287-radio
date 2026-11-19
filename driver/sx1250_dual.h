#ifndef SX1250_DUAL_H
#define SX1250_DUAL_H
#include "sx1250_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receive independently on both RAK2287 packet engines using the SPI clock.
 * Reset and exclusively own the board as for sx1250_rx_open. Profiles are
 * indexed by RF chain. Serialize all operations. This is a separate session
 * mode from sx1250_rx_open; close it with sx1250_dual_close. */
int sx1250_dual_open(const char *spi_path, const struct sx1250_rx_config profiles[2]);

/* Poll once. Return 1 for a validated packet or 0 when none is available. */
int sx1250_dual_next(unsigned rf, struct sx1250_rx_packet *packet);
/* Return 1 for a receive in progress or a pending packet, or 0 for idle. */
int sx1250_dual_activity(unsigned rf);

/* Pause for shared-front-end operations. EAGAIN means a receive is pending
 * and there was no handoff. Pending FIFO data remains available to next().
 * A late preamble can still race a hardware standby command. */
int sx1250_dual_pause(void);
/* Restore both original receive profiles and rearm after successful pause.
 * Includes one board recovery attempt on a failed restoration. */
int sx1250_dual_resume(void);

/* Close even after an I/O fault. Other calls return -1 with errno on error. */
int sx1250_dual_close(void);

#ifdef __cplusplus
}
#endif
#endif
