#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../sx1250_rx.h"
#include "../sx1250_tx.h"
#include "../sx1250_tx_handoff.h"

static volatile sig_atomic_t stopped;

static void stop(int signal) {
    (void)signal;
    stopped = 1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *hex, uint8_t *payload, size_t capacity) {
    size_t length = strlen(hex);
    if ((length & 1) || length / 2 > capacity) return 0;
    for (size_t i = 0; i < length / 2; ++i) {
        int high = hex_digit(hex[2 * i]);
        int low = hex_digit(hex[2 * i + 1]);
        if (high < 0 || low < 0) return 0;
        payload[i] = (high << 4) | low;
    }
    return length / 2;
}

static int64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static int listen_until(int64_t deadline) {
    unsigned received = 0;
    while (!stopped) {
        int64_t now = monotonic_ns();
        if (now < 0) {
            perror("clock_gettime");
            return 1;
        }
        if (now >= deadline) break;
        int64_t remaining_ms = (deadline - now + 999999) / 1000000;
        struct sx1250_rx_packet packet;
        int result = sx1250_rx_next(&packet, remaining_ms < 100 ? remaining_ms : 100);
        if (result < 0 && errno == EINTR && stopped) break;
        if (result < 0) {
            perror("sx1250_rx_next");
            return 1;
        }
        if (result == 0) continue;
        printf("RX\t%.1f\t%.2f\t", packet.rssi_dbm, packet.snr_db);
        for (unsigned i = 0; i < packet.size; ++i) printf("%02x", (unsigned)packet.payload[i]);
        putchar('\n');
        ++received;
    }
    printf("RX_PROBE_DONE packets=%u\n", received);
    return 0;
}

static void usage(FILE *out) {
    fputs("Usage: sx1250-tx-probe --enable-transmit DEVICE HEX [LISTEN_MS]\n"
          "       sx1250-tx-probe --enable-cold-transmit DEVICE HEX [LISTEN_MS]\n"
          "       sx1250-tx-probe --enable-warm-transmit[=prepare|=restore] DEVICE HEX [LISTEN_MS]\n"
          "       sx1250-tx-probe --warm-dry-run[=prepare|=restore] DEVICE [LISTEN_MS]\n"
          "The dry run switches RF0 and returns to RF1 without issuing SetTx.\n", out);
    fputs("Send one raw MeshCore EU Narrow LoRa payload, then listen on RF1.\n"
          "Provide 1 to 255 bytes as hex. Listen for 5000 ms by default (0 to 60000 ms).\n"
          "Stop any other concentrator software, attach an antenna or RF load, and reset the board.\n",
          out);
}

static int parse_options(const char *option, unsigned *flags, const char **mode) {
    static const char dry[] = "--warm-dry-run";
    static const char tx[] = "--enable-warm-transmit";
    const char *suffix;
    if (!strcmp(option, "--enable-transmit")) {
        *mode = "warm";
        return 0;
    }
    if (!strcmp(option, "--enable-cold-transmit")) {
        *mode = "cold";
        return 0;
    }
    if (!strncmp(option, dry, sizeof dry - 1)) {
        *flags = SX1250_HANDOFF_DRY_RUN;
        suffix = option + sizeof dry - 1;
    } else if (!strncmp(option, tx, sizeof tx - 1)) {
        suffix = option + sizeof tx - 1;
    } else {
        return -1;
    }
    if (!*suffix) {
        *flags |= SX1250_HANDOFF_WARM_PREPARE | SX1250_HANDOFF_WARM_RESTORE;
        *mode = "warm";
    } else if (!strcmp(suffix, "=prepare")) {
        *flags |= SX1250_HANDOFF_WARM_PREPARE;
        *mode = "warm-prepare";
    } else if (!strcmp(suffix, "=restore")) {
        *flags |= SX1250_HANDOFF_WARM_RESTORE;
        *mode = "warm-restore";
    } else {
        return -1;
    }
    return 0;
}

static void print_handoff(const char *mode, bool dry_run, const struct sx1250_handoff_report *report) {
    printf("HANDOFF mode=%s dry_run=%u rf1_downtime_ms=", mode, (unsigned)dry_run);
    if (report->downtime_ns > 0) printf("%.3f", report->downtime_ns / 1000000.0);
    else fputs("unknown", stdout);
    printf(" tx_started=%u tx_done=%u rf1_rearmed=%u cold_recovery=%u failed_stage=%s\n",
           (unsigned)report->tx_started, (unsigned)report->tx_done, (unsigned)report->rf1_rearmed,
           (unsigned)report->cold_recovery, report->failed_stage ? report->failed_stage : "none");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    unsigned flags = 0;
    const char *mode;
    if (argc < 2 || parse_options(argv[1], &flags, &mode)) { usage(stderr); return 2; }
    bool dry_run = (flags & SX1250_HANDOFF_DRY_RUN) != 0;
    bool default_send = strcmp(argv[1], "--enable-transmit") == 0;
    int listen_arg = dry_run ? 3 : 4;
    if (argc < listen_arg || argc > listen_arg + 1) { usage(stderr); return 2; }
    uint8_t payload[255] = {0};
    size_t length = dry_run ? 1 : parse_hex(argv[3], payload, sizeof payload);
    if (!length) { usage(stderr); return 2; }
    unsigned listen_ms = 5000;
    if (argc > listen_arg) {
        char *end;
        errno = 0;
        const char *duration = argv[listen_arg];
        unsigned long parsed = strtoul(duration, &end, 10);
        if (errno || *duration < '0' || *duration > '9' || *end || parsed > 60000) {
            usage(stderr);
            return 2;
        }
        listen_ms = parsed;
    }

    const struct sx1250_rx_config profile = {
        .frequency_hz = 869618000, .bandwidth_hz = 62500, .preamble_symbols = 16,
        .spreading_factor = 8, .coding_rate = 8, .sync_word = 0x12, .require_crc = true,
    };
    unsigned airtime_ms = sx1250_lora_airtime_ms(&profile, length);
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction");
        return 1;
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) {
        perror("sigaction SIGPIPE");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (sx1250_rx_open(argv[2], &profile)) {
        perror("sx1250_rx_open");
        return 1;
    }

    int result = 1;
    int64_t start = monotonic_ns();
    if (start < 0) perror("clock_gettime");
    else if (!stopped) {
        struct sx1250_handoff_report report = {0};
        unsigned timeout_ms = 2 * airtime_ms + 500;
        int sent = default_send ? sx1250_tx_send_report(payload, length, timeout_ms, &report) :
                                  sx1250_tx_probe_handoff(payload, length, timeout_ms, flags, &report);
        int tx_error = errno;
        int64_t end = monotonic_ns();
        print_handoff(mode, dry_run, &report);
        if (sent && tx_error == EAGAIN) {
            fputs("TX_DEFERRED rf1_busy=1 set_tx_called=0\n", stderr);
            result = 3;
        } else if (dry_run) {
            if (sent || report.cold_recovery) {
                fprintf(stderr, "DRY_RUN_FAILED %s\n", strerror(sent ? tx_error : EIO));
                if (report.rf1_rearmed && end >= 0)
                    (void)listen_until(end + (int64_t)listen_ms * 1000000);
            } else if (end < 0) {
                perror("clock_gettime after RF1 rearm");
            } else {
                printf("DRY_RUN_OK set_tx_called=0 rf1_rearmed=1 handover_ms=%.3f\n", (end - start) / 1000000.0);
                result = listen_until(end + (int64_t)listen_ms * 1000000);
            }
        } else if (sent && !report.tx_started) {
            fprintf(stderr, "TX_NOT_STARTED %s\n", strerror(tx_error));
            if (report.rf1_rearmed && end >= 0)
                (void)listen_until(end + (int64_t)listen_ms * 1000000);
        } else if (sent && report.tx_done) {
            fprintf(stderr, "TX_HANDOFF_FAILED %s\n", strerror(tx_error));
            if (report.rf1_rearmed && end >= 0)
                (void)listen_until(end + (int64_t)listen_ms * 1000000);
        } else if (sent) {
            fprintf(stderr, "TX_UNCONFIRMED %s\n", strerror(tx_error));
        } else if (end < 0) {
            perror("clock_gettime after TxDone and RF1 rearm");
        } else {
            printf("TX_OK bytes=%zu airtime_ms=%u handover_ms=%.3f tx_done=1 rf1_rearmed=1\n",
                   length, airtime_ms, (end - start) / 1000000.0);
            result = listen_until(end + (int64_t)listen_ms * 1000000);
        }
    }
    if (sx1250_rx_close()) {
        perror("sx1250_rx_close");
        return 1;
    }
    return result;
}
