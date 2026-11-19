#define _POSIX_C_SOURCE 200809L
#include "usp_scheduler.h"
#include "rac_radio.h"
#include "smtc_core_hal.h"
#include "smtc_rac_api.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_flag running = ATOMIC_FLAG_INIT;

static struct {
    void (*timer_callback)(void *);
    void *timer_ctx;
    uint64_t timer_us;
    void (*radio_callback)(void *);
    void *radio_ctx;
    rp_status_t lease_status[RP_NB_HOOKS];
} linux_hal;

enum { poll_ms = 5, max_slot_ms = 120000 };

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("CLOCK_MONOTONIC");
        abort();
    }
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

uint32_t smtc_modem_hal_get_time_in_ms(void)
{
    return (uint32_t)(monotonic_us() / 1000);
}

void smtc_modem_hal_start_timer(uint32_t ms, void (*callback)(void *), void *context)
{
    linux_hal.timer_callback = callback;
    linux_hal.timer_ctx = context;
    linux_hal.timer_us = monotonic_us() + (uint64_t)ms * 1000;
}

void smtc_modem_hal_stop_timer(void)
{
    linux_hal.timer_callback = NULL;
}

void smtc_modem_hal_irq_config_radio_irq(void (*callback)(void *), void *context)
{
    linux_hal.radio_callback = callback;
    linux_hal.radio_ctx = context;
}

/* All callbacks run on the event loop, including the synthetic lease IRQ. */
void smtc_modem_hal_protect_api_call(void) {}
void smtc_modem_hal_unprotect_api_call(void) {}
void smtc_modem_hal_user_lbm_irq(void) {}
bool smtc_modem_external_stack_currently_use_radio(void) { return false; }
void smtc_modem_hal_stop_radio_tcxo(void) {}

void smtc_modem_hal_set_ant_switch(bool tx)
{
    if (tx) abort();
}

void smtc_modem_hal_on_panic(uint8_t *function, uint32_t line, const char *format, ...)
{
    fprintf(stderr, "USP RAC panic in %s:%u: ", function, line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

void smtc_modem_hal_print_trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

ral_status_t rak2287_rac_idle(const void *context)
{
    (void)context;
    return RAL_STATUS_OK;
}
ral_status_t rak2287_rac_sleep(const void *context, bool retain)
{
    (void)retain;
    return rak2287_rac_idle(context);
}
ral_status_t rak2287_rac_standby(const void *context, ral_standby_cfg_t mode)
{
    (void)mode;
    return rak2287_rac_idle(context);
}
ral_status_t rak2287_rac_clear_irq(const void *context, ral_irq_t irq)
{
    (void)irq;
    return rak2287_rac_idle(context);
}

static void lease_started(void)
{
    linux_hal.radio_callback(linux_hal.radio_ctx);
}

static void lease_event(rp_status_t status)
{
    linux_hal.lease_status[smtc_rac_get_callback_radio_id()] = status;
}

static void rac_tick(void)
{
    if (linux_hal.timer_callback && monotonic_us() >= linux_hal.timer_us) {
        void (*callback)(void *) = linux_hal.timer_callback;
        linux_hal.timer_callback = NULL;
        callback(linux_hal.timer_ctx);
    }
    smtc_rac_run_engine();
}

static int wait_status(uint8_t client, rp_status_t expected)
{
    const uint64_t limit = monotonic_us() + 200000;
    do {
        rac_tick();
        if (linux_hal.lease_status[client] == expected) return 0;
        if (linux_hal.lease_status[client] == RP_STATUS_TASK_ABORTED) break;
        const struct timespec delay = { .tv_nsec = 1000000 };
        nanosleep(&delay, NULL);
    } while (monotonic_us() < limit);
    errno = ETIMEDOUT;
    return -1;
}

static int submit_slot(uint8_t client, unsigned ms)
{
    linux_hal.lease_status[client] = RP_STATUS_TASK_INIT;
    smtc_rac_scheduler_config_t schedule = {
        .start_time_ms = smtc_modem_hal_get_time_in_ms(),
        .duration_time_ms = ms,
        .scheduling = SMTC_RAC_ASAP_TRANSACTION,
        .callback_pre_radio_transaction = lease_started,
        .callback_post_radio_transaction = lease_event,
    };
    if (smtc_rac_lock_radio_access(client, schedule) != SMTC_RAC_SUCCESS) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int request_release(uint8_t client)
{
    linux_hal.lease_status[client] = RP_STATUS_TASK_INIT;
    if (smtc_rac_unlock_radio_access(client) != SMTC_RAC_SUCCESS) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int receive(const struct usp_rx_hooks *hooks, void *ctx, const struct usp_rx_profile *profile,
                   int ms, bool drain)
{
    struct sx1250_rx_packet packet;
    const int rc = drain ? hooks->drain(ctx, &packet) : hooks->next(ctx, &packet, ms);
    if (rc < 0 && errno == EINTR && hooks->stop(ctx)) return 0;
    if (rc > 0 && hooks->packet) hooks->packet(ctx, profile, monotonic_us(), &packet);
    return rc;
}

static int listen_until(const struct usp_rx_hooks *hooks, void *ctx, const struct usp_rx_profile *profile,
                        uint64_t deadline_us)
{
    while (!hooks->stop(ctx)) {
        const uint64_t now = monotonic_us();
        if (now >= deadline_us) break;
        const unsigned remaining_ms = (deadline_us - now + 999) / 1000;
        if (receive(hooks, ctx, profile, remaining_ms < poll_ms ? remaining_ms : poll_ms, false) < 0) return -1;
    }
    return 0;
}

struct slot {
    const struct usp_rx_profile *profile;
    uint64_t armed_at, switch_us, nominal_end, limit;
};

static void report_slot(const struct usp_rx_hooks *hooks, void *ctx, const struct slot *slot, uint64_t end)
{
    if (hooks->slot && slot->profile) {
        hooks->slot(ctx, slot->profile, end, end - slot->armed_at, slot->switch_us);
    }
}

static int protect_reception(const struct usp_rx_hooks *hooks, void *ctx, const struct slot *slot)
{
    while (!hooks->stop(ctx)) {
        const uint64_t now = monotonic_us();
        if (now >= slot->limit) break;
        const int busy = hooks->activity(ctx);
        if (busy <= 0) return busy;
        const uint64_t next_sample = now + poll_ms * 1000;
        const uint64_t deadline = next_sample < slot->limit ? next_sample : slot->limit;
        if (listen_until(hooks, ctx, slot->profile, deadline) < 0) return -1;
    }
    return 0;
}

static int tune_next(const struct usp_rx_hooks *hooks, void *ctx, struct slot *slot,
                     const struct usp_rx_profile *profile)
{
    bool drained_at_limit = false;
    uint64_t frozen_at = 0;
    for (;;) {
        if (hooks->stop(ctx)) return 1;
        uint64_t begin = monotonic_us();
        if (slot->profile == profile) {
            report_slot(hooks, ctx, slot, begin);
            slot->switch_us = 0;
            slot->armed_at = begin;
            break;
        }
        const bool force = !slot->profile || begin >= slot->limit;
        if (hooks->tune(ctx, &profile->radio, force) == 0) {
            const uint64_t armed = monotonic_us();
            if (drained_at_limit) begin = frozen_at;
            report_slot(hooks, ctx, slot, begin);
            slot->armed_at = armed;
            slot->switch_us = armed - begin;
            break;
        }
        if (!slot->profile || errno != EAGAIN) return -1;
        if (drained_at_limit) {
            errno = EIO;
            return -1;
        }
        if (force) {
            frozen_at = begin;
            drained_at_limit = true;
        }
        if (receive(hooks, ctx, slot->profile, force ? 0 : poll_ms, force) < 0) return -1;
    }
    slot->profile = profile;
    slot->nominal_end = slot->armed_at + (uint64_t)profile->duration_ms * 1000;
    slot->limit = slot->nominal_end + (uint64_t)profile->max_extension_ms * 1000;
    return 0;
}

int usp_rx_run(const struct usp_rx_profile *profiles, size_t count, const struct usp_rx_hooks *hooks, void *ctx)
{
    if (!profiles || !count || count > 2 || !hooks || !hooks->tune || !hooks->next ||
        (count > 1 && !hooks->drain) || !hooks->activity || !hooks->stop) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        if (!profiles[i].name || !profiles[i].duration_ms || profiles[i].duration_ms > max_slot_ms ||
            profiles[i].max_extension_ms > max_slot_ms || profiles[i].priority > 4) {
            errno = EINVAL;
            return -1;
        }
    }
    if (atomic_flag_test_and_set(&running)) {
        errno = EBUSY;
        return -1;
    }
    size_t order[] = {0, 1};
    if (count == 2 && profiles[1].priority < profiles[0].priority) {
        order[0] = 1;
        order[1] = 0;
    }
    linux_hal.timer_callback = NULL;
    smtc_rac_init();
    /* The Linux loop owns the lease bounds. A dwell plus its extension may
     * exceed the upstream failsafe, which includes the in-flight unlock. */
    rp_disable_failsafe(smtc_rac_get_rp(), true);
    uint8_t clients[2];
    size_t registered = 0;
    int error = 0;
    struct slot slot = {0};
    for (; registered < count; registered++) {
        unsigned priority = profiles[order[registered]].priority;
        if (registered && priority == profiles[order[0]].priority) priority = priority == 4 ? 3 : priority + 1;
        clients[registered] = smtc_rac_open_radio(RAC_VERY_HIGH_PRIORITY + priority);
        if (clients[registered] == RAC_INVALID_RADIO_ID) {
            error = EIO;
            goto cleanup;
        }
    }
    if (!hooks->stop(ctx) && submit_slot(clients[0], profiles[order[0]].duration_ms) < 0) {
        error = errno;
        goto cleanup;
    }
    for (size_t i = 0; !hooks->stop(ctx); i = (i + 1) % count) {
        const struct usp_rx_profile *profile = &profiles[order[i]];
        if (wait_status(clients[i], RP_STATUS_RADIO_LOCKED) < 0) {
            error = errno;
            break;
        }
        const int tuned = tune_next(hooks, ctx, &slot, profile);
        if (tuned < 0 || (tuned == 0 &&
            (listen_until(hooks, ctx, profile, slot.nominal_end) < 0 ||
             (count > 1 && protect_reception(hooks, ctx, &slot) < 0)))) {
            error = errno ? errno : EIO;
        }
        const size_t next = (i + 1) % count;
        const bool advance = !error && !hooks->stop(ctx);
        if (request_release(clients[i]) < 0) {
            if (!error) error = errno;
            break;
        }
        /* RAC completes the unlock on its next engine tick. A queued release
         * IRQ prevents the incoming ASAP request from evicting the old owner. */
        if (advance && count > 1 &&
            submit_slot(clients[next], profiles[order[next]].duration_ms) < 0) error = errno;
        if (wait_status(clients[i], RP_STATUS_RADIO_UNLOCKED) < 0 && !error) error = errno;
        if (error || !advance) break;
        if (count == 1 && submit_slot(clients[0], profile->duration_ms) < 0) {
            error = errno;
            break;
        }
    }
cleanup:
    if (!error) report_slot(hooks, ctx, &slot, monotonic_us());
    while (registered) {
        if (smtc_rac_close_radio(clients[--registered]) != SMTC_RAC_SUCCESS && !error) error = EIO;
    }
    linux_hal.timer_callback = NULL;
    atomic_flag_clear(&running);
    if (!error) return 0;
    errno = error;
    return -1;
}
