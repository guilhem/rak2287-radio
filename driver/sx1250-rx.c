#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sx1250_rx.h"

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }

static void usage(FILE *out) {
    fputs("Usage: sx1250-rx --frequency HZ --bandwidth HZ --sf 5..12 --cr 5..8 --sync BYTE\n"
          "                 [--implicit-length 1..255] [--preamble SYMBOLS] [--crc 0|1] [--invert-iq 0|1] [--device PATH]\n"
          "BW in Hz: 7812, 10417, 15625, 20833, 31250, 41667, 62500, 125000, 250000, 500000.\n"
          "Defaults: preamble 16, CRC required,\n"
          "explicit header, normal IQ, /dev/spidev0.0. Reset the idle RAK2287 before startup.\n"
          "Output: RX<TAB>RSSI_dBm<TAB>SNR_dB<TAB>hex followed by a newline.\n", out);
}

static int parse(int argc, char **argv, const char **path, struct sx1250_rx_config *c) {
    unsigned flags = 0;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return -1;
        const char *key = argv[i], *value = argv[i+1];
        if (!strcmp(key, "--device")) { *path = value; continue; }
        char *end;
        errno = 0;
        unsigned long v = strtoul(value, &end, strncmp(value, "0x", 2) == 0 ? 16 : 10);
        if (*value < '0' || *value > '9' || *end || errno || v > UINT32_MAX) return -1;
        if (!strcmp(key, "--frequency")) { c->frequency_hz = v; flags |= 1; }
        else if (!strcmp(key, "--bandwidth")) { c->bandwidth_hz = v; flags |= 2; }
        else if (!strcmp(key, "--sf") && v <= 12) { c->spreading_factor = v; flags |= 4; }
        else if (!strcmp(key, "--cr") && v <= 8) { c->coding_rate = v; flags |= 8; }
        else if (!strcmp(key, "--sync") && v <= 255) { c->sync_word = v; flags |= 16; }
        else if (!strcmp(key, "--implicit-length") && v >= 1 && v <= 255) c->implicit_length = v;
        else if (!strcmp(key, "--preamble") && v <= UINT16_MAX) c->preamble_symbols = v;
        else if (!strcmp(key, "--crc") && v <= 1) c->require_crc = v;
        else if (!strcmp(key, "--invert-iq") && v <= 1) c->inverted_iq = v;
        else return -1;
    }
    return flags == 31 ? 0 : -1;
}

/* Keep a whole record within Linux PIPE_BUF and flush it immediately. */
static int send_packet(int output, const struct sx1250_rx_packet *p) {
    static const char digits[] = "0123456789abcdef";
    char line[576];
    int n = snprintf(line, sizeof line, "RX\t%.1f\t%.2f\t", p->rssi_dbm, p->snr_db);
    if (n < 0 || (size_t)n + p->size * 2 + 1 > sizeof line) return -1;
    for (unsigned i = 0; i < p->size; i++) {
        line[n++] = digits[p->payload[i] >> 4];
        line[n++] = digits[p->payload[i] & 15];
    }
    line[n++] = '\n';
    for (int offset = 0; offset < n && !stopped;) {
        struct pollfd fd = {.fd = output, .events = POLLOUT};
        int ready = poll(&fd, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (!ready) continue;
        ssize_t w = write(output, line + offset, n - offset);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        offset += w;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    const char *path = "/dev/spidev0.0";
    struct sx1250_rx_config config = {.preamble_symbols = 16, .require_crc = true};
    if (parse(argc, argv, &path, &config)) { usage(stderr); return 2; }
    /* The stock HAL itself prints to stdout. Save the protocol fd before its first call. */
    int output = dup(STDOUT_FILENO);
    if (output < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) { perror("output"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
    if (sx1250_rx_open(path, &config)) { perror("sx1250_rx_open"); close(output); return 1; }
    int rc = 0;
    while (!stopped) {
        struct pollfd sink = {.fd = output};
        int result = poll(&sink, 1, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (sink.revents & POLLNVAL)) { rc = 1; break; }
        if (sink.revents & (POLLERR | POLLHUP)) break;
        struct sx1250_rx_packet p;
        result = sx1250_rx_next(&p, 100);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) { perror("sx1250_rx_next"); rc = 1; break; }
        if (result && send_packet(output, &p)) {
            if (errno != EPIPE) { perror("write packet"); rc = 1; }
            break;
        }
    }
    if (sx1250_rx_close()) { perror("sx1250_rx_close"); rc = 1; }
    if (close(output)) { perror("close output"); rc = 1; }
    return rc;
}
