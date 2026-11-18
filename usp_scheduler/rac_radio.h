#ifndef USP_SCHEDULER_RAC_RADIO_H
#define USP_SCHEDULER_RAC_RADIO_H

#include "ralf_sx126x.h"

ral_status_t rak2287_rac_idle(const void *context);
ral_status_t rak2287_rac_sleep(const void *context, bool retain);
ral_status_t rak2287_rac_standby(const void *context, ral_standby_cfg_t mode);
ral_status_t rak2287_rac_clear_irq(const void *context, ral_irq_t irq);

/* RAC schedules exclusive receive leases. Board operations stay in the
 * receiver hooks so a lease ends without resetting or putting RF0 to sleep. */
#undef RALF_SX126X_INSTANTIATE
#define RALF_SX126X_INSTANTIATE(ctx) { .ral = { .context = (ctx), .driver = { \
    .init = rak2287_rac_idle, .reset = rak2287_rac_idle, .set_sleep = rak2287_rac_sleep, \
    .set_standby = rak2287_rac_standby, .clear_irq_status = rak2287_rac_clear_irq, \
} } }

#endif
