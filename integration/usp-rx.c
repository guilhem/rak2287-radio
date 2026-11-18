#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usp_scheduler.h"

enum { max_profiles = 2, name_capacity = 32 };

struct receiver {
    const char *device;
    FILE *output;
    bool opened;
};

static volatile sig_atomic_t stopping;

static void stop_on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

static int read_profiles(const char *path, struct usp_rx_profile profiles[],
                         char names[][name_capacity]) {
    FILE *config = fopen(path, "r");
    if (!config) {
        perror(path);
        return -1;
    }
    char *line = NULL;
    size_t capacity = 0;
    unsigned lineno = 0, count = 0;
    int result = -1;
    while (getline(&line, &capacity, config) >= 0) {
        ++lineno;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *token_state;
        const char *name = strtok_r(line, " \t\r\n", &token_state);
        if (!name) continue;
        if (count == max_profiles || strlen(name) >= name_capacity ||
            strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(name) ||
            (count && !strcmp(name, names[0]))) goto invalid;
        const unsigned upper[] = {869999999, 500000, 12, 8, 255, 65535, 1, 1, 255, 120000, 120000, 4};
        const unsigned lower[] = {863000001, 7812, 5, 5, 0, 8, 0, 0, 0, 1, 0, 0};
        unsigned v[sizeof upper / sizeof upper[0]];
        for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
            const char *value = strtok_r(NULL, " \t\r\n", &token_state);
            if (!value || !isdigit((unsigned char)*value)) goto invalid;
            char *end;
            errno = 0;
            unsigned long n = strtoul(value, &end, i == 4 &&
                 value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 16 : 10);
            if (errno || *end || n < lower[i] || n > upper[i]) goto invalid;
            v[i] = (unsigned)n;
        }
        if (strtok_r(NULL, " \t\r\n", &token_state)) goto invalid;
        strcpy(names[count], name);
        profiles[count] = (struct usp_rx_profile){
            .name = names[count],
            .radio = {
                .frequency_hz = v[0], .bandwidth_hz = v[1], .spreading_factor = v[2],
                .coding_rate = v[3], .sync_word = v[4], .preamble_symbols = v[5],
                .require_crc = v[6], .inverted_iq = v[7], .implicit_length = v[8],
            },
            .duration_ms = v[9], .max_extension_ms = v[10], .priority = v[11],
        };
        if (!sx1250_lora_profile_valid(&profiles[count].radio)) goto invalid;
        ++count;
    }
    if (ferror(config)) perror(path);
    else if (!count) fprintf(stderr, "%s: at least one profile is required\n", path);
    else result = (int)count;
    goto done;
invalid:
    fprintf(stderr, "%s:%u: invalid profile or more than %d profiles\n", path, lineno, max_profiles);
done:
    free(line);
    fclose(config);
    return result;
}

static int tune(void *ctx, const struct sx1250_rx_config *profile, bool force) {
    struct receiver *receiver = ctx;
    if (receiver->opened) return sx1250_rx_reconfigure(profile, force);
    int rc = sx1250_rx_open(receiver->device, profile);
    receiver->opened = rc == 0;
    return rc;
}

static int receive(void *ctx, struct sx1250_rx_packet *packet, int timeout_ms) {
    (void)ctx;
    return sx1250_rx_next(packet, timeout_ms);
}

static int drain(void *ctx, struct sx1250_rx_packet *packet) {
    (void)ctx;
    return sx1250_rx_drain_for_switch(packet);
}

static int activity(void *ctx) {
    (void)ctx;
    return sx1250_rx_activity();
}

static bool should_stop(void *ctx) {
    const struct receiver *receiver = ctx;
    return stopping || ferror(receiver->output);
}

static void packet_record(void *ctx, const struct usp_rx_profile *profile, uint64_t time_us,
                          const struct sx1250_rx_packet *packet) {
    struct receiver *receiver = ctx;
    FILE *out = receiver->output;
    fprintf(out, "RX\t%s\t%" PRIu64 "\t%.1f\t%.2f\t", profile->name, time_us,
            packet->rssi_dbm, packet->snr_db);
    for (size_t i = 0; i < packet->size; ++i) fprintf(out, "%02x", packet->payload[i]);
    fputc('\n', out);
}

static void slot_record(void *ctx, const struct usp_rx_profile *profile, uint64_t time_us,
                        uint64_t dwell_us, uint64_t switch_us) {
    struct receiver *receiver = ctx;
    fprintf(receiver->output, "SLOT profile=%s monotonic_us=%" PRIu64 " dwell_us=%" PRIu64
            " switch_us=%" PRIu64 "\n", profile->name, time_us, dwell_us, switch_us);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: %s CONFIG [SPI_DEVICE]\n", argv[0]);
        return argc == 2 ? 0 : 2;
    }
    struct usp_rx_profile profiles[max_profiles];
    char names[max_profiles][name_capacity];
    int count = read_profiles(argv[1], profiles, names);
    if (count < 0) return 2;
    /* Keep stock HAL startup diagnostics out of the packet stream. */
    int output_fd = dup(STDOUT_FILENO);
    FILE *out = output_fd < 0 ? NULL : fdopen(output_fd, "w");
    if (!out || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        perror("receiver output");
        return 1;
    }
    setvbuf(out, NULL, _IOLBF, 0);
    struct sigaction action = {.sa_handler = stop_on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
    struct receiver receiver = {.device = argc == 3 ? argv[2] : "/dev/spidev0.0", .output = out};
    const struct usp_rx_hooks hooks = {
        .tune = tune, .next = receive, .drain = drain, .activity = activity, .stop = should_stop,
        .packet = packet_record, .slot = slot_record,
    };
    int rc = usp_rx_run(profiles, (size_t)count, &hooks, &receiver);
    if (rc) perror("USP receive scheduler");
    if (receiver.opened && sx1250_rx_close()) {
        perror("sx1250_rx_close");
        rc = -1;
    }
    if (ferror(out)) rc = -1;
    if (fclose(out)) rc = -1;
    return rc ? 1 : 0;
}
