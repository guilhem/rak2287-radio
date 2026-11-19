#define _POSIX_C_SOURCE 200809L
#include "usp_scheduler.h"
#include "smtc_rac_api.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t simulated_us;
static unsigned queued_handoffs;
static bool previously_pending;

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    assert(clock == CLOCK_MONOTONIC);
    const radio_planner_t *planner = smtc_rac_get_rp();
    bool pending = false;
    if (planner->tasks[planner->radio_task_id].state == RP_TASK_STATE_RUNNING &&
        planner->tasks[planner->radio_task_id].type == RP_TASK_TYPE_UNLOCK_RADIO_ACCESS) {
        for (size_t i = 0; i < RP_NB_HOOKS; i++) {
            const rp_task_t *task = &planner->tasks[i];
            if (task->state == RP_TASK_STATE_ASAP && task->type == RP_TASK_TYPE_LOCK_RADIO_ACCESS) pending = true;
        }
    }
    if (pending && !previously_pending) queued_handoffs++;
    previously_pending = pending;
    ts->tv_sec = simulated_us / 1000000;
    ts->tv_nsec = simulated_us % 1000000 * 1000;
    return 0;
}

int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining)
{
    (void)remaining;
    simulated_us += (uint64_t)request->tv_sec * 1000000 + request->tv_nsec / 1000;
    return 0;
}

static const uint64_t epoch = 1000000;
static const unsigned meshcore_hz = 869618000;
static const unsigned meshtastic_hz = 869300000;

struct air_frame {
    unsigned freq, begin_ms, end_ms;
    uint8_t id;
};

struct model {
    const struct air_frame *frames;
    size_t frame_count;
    unsigned freq, tunes, slots, packet_count;
    uint64_t armed_at, until;
    unsigned forced_drains, terminal_rearms;
    bool force[8], pending_at_switch, frozen, read_failure, interrupt_on_read;
    uint8_t packets[8];
    unsigned slot_freq[8];
    uint64_t dwell_us[8], switch_us[8];
};

static int reconfigure(void *ctx, const struct sx1250_rx_config *radio, bool force)
{
    struct model *m = ctx;
    if (m->pending_at_switch && m->tunes == 1) {
        m->frozen = force;
        if (force) simulated_us += 1000;
        errno = EAGAIN;
        return -1;
    }
    assert(m->tunes < 8);
    m->force[m->tunes++] = force;
    simulated_us += 2000;
    m->armed_at = simulated_us;
    m->freq = radio->frequency_hz;
    m->frozen = false;
    return 0;
}

static int read_rf(void *ctx, struct sx1250_rx_packet *packet, int ms)
{
    struct model *m = ctx;
    if (m->read_failure) {
        errno = EIO;
        return -1;
    }
    if (m->interrupt_on_read) {
        m->until = simulated_us;
        errno = EINTR;
        return -1;
    }
    if (m->pending_at_switch && ms == 0) {
        /* Immediate RxDone after every rearm would keep this event pending. */
        m->terminal_rearms++;
        *packet = (struct sx1250_rx_packet){.size = 1, .payload = {0xee}};
        return 1;
    }
    const uint64_t until = simulated_us + (unsigned)ms * 1000;
    for (size_t i = 0; i < m->frame_count; i++) {
        const struct air_frame *f = &m->frames[i];
        const uint64_t begin = epoch + f->begin_ms * 1000;
        const uint64_t end = epoch + f->end_ms * 1000;
        if (f->freq == m->freq && begin >= m->armed_at && end > simulated_us && end <= until) {
            simulated_us = end;
            *packet = (struct sx1250_rx_packet){.size = 1, .payload = {f->id}};
            return 1;
        }
    }
    simulated_us = until;
    return 0;
}

static int drain_terminal(void *ctx, struct sx1250_rx_packet *packet)
{
    struct model *m = ctx;
    assert(m->pending_at_switch && m->frozen);
    simulated_us += 1000;
    m->forced_drains++;
    m->pending_at_switch = false;
    *packet = (struct sx1250_rx_packet){.size = 1, .payload = {0xee}};
    return 1;
}

static int rx_activity(void *ctx)
{
    const struct model *m = ctx;
    for (size_t i = 0; i < m->frame_count; i++) {
        const struct air_frame *f = &m->frames[i];
        const uint64_t begin = epoch + f->begin_ms * 1000;
        const uint64_t end = epoch + f->end_ms * 1000;
        if (f->freq == m->freq && begin >= m->armed_at && begin <= simulated_us && end > simulated_us) {
            return 1;
        }
    }
    return 0;
}

static bool should_stop(void *ctx)
{
    return simulated_us >= ((struct model *)ctx)->until;
}

static void saw_packet(void *ctx, const struct usp_rx_profile *profile, uint64_t at,
                       const struct sx1250_rx_packet *packet)
{
    struct model *m = ctx;
    assert(m->packet_count < 8);
    assert(at == simulated_us && profile->radio.frequency_hz == m->freq);
    m->packets[m->packet_count++] = packet->payload[0];
}

static void saw_slot(void *ctx, const struct usp_rx_profile *profile, uint64_t at,
                     uint64_t dwell, uint64_t tune)
{
    struct model *m = ctx;
    assert(m->slots < 8);
    assert(at <= simulated_us);
    m->slot_freq[m->slots] = profile->radio.frequency_hz;
    m->dwell_us[m->slots] = dwell;
    m->switch_us[m->slots++] = tune;
}

static const struct usp_rx_hooks hooks = {
    .tune = reconfigure, .next = read_rf, .drain = drain_terminal,
    .activity = rx_activity, .stop = should_stop,
    .packet = saw_packet, .slot = saw_slot,
};

int main(void)
{
    const struct usp_rx_profile meshcore = {
        .name = "meshcore", .radio.frequency_hz = meshcore_hz,
        .duration_ms = 20, .max_extension_ms = 15, .priority = 0,
    };
    const struct usp_rx_profile meshtastic = {
        .name = "meshtastic", .radio.frequency_hz = meshtastic_hz,
        .duration_ms = 30, .max_extension_ms = 3, .priority = 4,
    };
    const struct usp_rx_profile profiles[] = {meshtastic, meshcore};
    const struct air_frame frames[] = {
        {meshcore_hz, 7, 8, 1}, {meshtastic_hz, 10, 13, 2},
        {meshcore_hz, 20, 26, 3}, {meshtastic_hz, 31, 33, 4},
        {meshcore_hz, 34, 35, 5}, {meshtastic_hz, 54, 190, 6},
    };
    simulated_us = epoch;
    struct model alternating = {.frames = frames, .frame_count = sizeof frames / sizeof frames[0],
                                .until = epoch + 75000};
    assert(usp_rx_run(profiles, 2, &hooks, &alternating) == 0);
    assert(queued_handoffs == 2);
    assert(alternating.slots == 3 && alternating.tunes == 3);
    assert(alternating.slot_freq[0] == meshcore_hz);
    assert(alternating.slot_freq[1] == meshtastic_hz);
    assert(alternating.slot_freq[2] == meshcore_hz);
    assert(alternating.dwell_us[0] == 25000);
    assert(alternating.dwell_us[1] == 33000);
    assert(alternating.switch_us[0] == 2000 && alternating.switch_us[1] == 2000);
    assert(!alternating.force[1] && alternating.force[2]);
    assert(alternating.packet_count == 3);
    assert(memcmp(alternating.packets, (const uint8_t[]){1, 3, 4}, 3) == 0);

    simulated_us = epoch;
    struct model failing = {.read_failure = true, .until = epoch + 47000};
    assert(usp_rx_run(&meshcore, 1, &hooks, &failing) == -1 && errno == EIO);

    simulated_us = epoch;
    struct model stopping = {.interrupt_on_read = true, .until = epoch + 47000};
    assert(usp_rx_run(&meshcore, 1, &hooks, &stopping) == 0);
    assert(stopping.slots == 1);

    simulated_us = epoch;
    struct model single = {.until = epoch + 47000};
    assert(usp_rx_run(&meshcore, 1, &hooks, &single) == 0);
    assert(single.tunes == 1 && single.slots == 3 && single.switch_us[1] == 0);

    simulated_us = epoch;
    struct usp_rx_profile tied[] = {meshcore, meshtastic};
    tied[0].priority = tied[1].priority = 4;
    tied[0].max_extension_ms = 0;
    struct model racing = {.pending_at_switch = true, .until = epoch + 48000};
    assert(usp_rx_run(tied, 2, &hooks, &racing) == 0);
    assert(racing.packet_count == 1 && racing.packets[0] == 0xee);
    assert(racing.tunes == 2 && racing.force[1]);
    assert(racing.slot_freq[0] == meshcore_hz && racing.slot_freq[1] == meshtastic_hz);
    assert(racing.forced_drains == 1 && racing.terminal_rearms == 0);
    assert(racing.dwell_us[0] == 20000 && racing.switch_us[1] == 4000);

    puts("USP RAC leases, RX profile routing and bounded activity extension passed");
    return 0;
}
