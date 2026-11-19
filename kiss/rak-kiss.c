#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "sx1250_dual.h"
#include "sx1250_tx.h"

enum { N_PORTS = 2, TX_WAIT_MS = 2000, TX_MARGIN_MS = 500, MAX_PACKET = 255 };
enum { FEND = 0xc0, FESC = 0xdb, TFEND = 0xdc, TFESC = 0xdd };
enum { DATA = 0x00, TX_DELAY = 0x01, SET_HARDWARE = 0x06 };
enum {
    SET_RADIO = 0x09, SET_TX_POWER, GET_RADIO, GET_TX_POWER,
    IS_CHANNEL_BUSY = 0x0e, GET_AIRTIME, GET_VERSION = 0x11, GET_STATS,
    GET_DEVICE_NAME = 0x16, PING,
    SET_SIGNAL_REPORT = 0x19, GET_SIGNAL_REPORT,
    SET_SYNC_WORD, SET_PREAMBLE, GET_PHY_EXTRA,
    OK = 0xf0, ERROR, TX_DONE = 0xf8, RX_META,
};
enum { BAD_LENGTH = 1, BAD_PARAMETER, UNAVAILABLE, UNKNOWN_COMMAND = 5, TX_BUSY = 7 };
enum { MAX_CONTROL_BYTES = 64, TX_DONE_BYTES = 7 };

struct port {
    const char *name;
    int fd;
    bool published, connected, configured, signal_report;
    unsigned tx_delay_ms;
    struct sx1250_rx_config profile;
    int8_t power_dbm;
    uint16_t preamble_override;
    uint32_t received, sent, tx_errors;
    uint8_t input[256], frame[512], output[4096];
    size_t input_start, input_end, frame_size, output_start, output_end;
    bool in_frame, escaped;
    uint8_t pending[MAX_PACKET];
    size_t pending_size;
    int8_t pending_power_dbm;
    int64_t earliest_tx, tx_deadline;
};

static struct port ports[N_PORTS] = {{.name = "rf0", .fd = -1}, {.name = "rf1", .fd = -1}};

static struct transmission {
    pthread_t thread;
    bool active;
    unsigned rf, timeout_ms;
    uint32_t airtime_ms;
    struct sx1250_rx_config profile;
    int8_t power_dbm;
    uint8_t payload[MAX_PACKET];
    size_t size;
    struct sx1250_handoff_report report;
    int result, error;
} transmission;

static volatile sig_atomic_t stopping;

static void stop(int signal_number) { (void)signal_number; stopping = 1; }

static int64_t clock_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool previous_pty_link(int directory, const char *name) {
    struct stat attributes;
    if (fstatat(directory, name, &attributes, AT_SYMLINK_NOFOLLOW) ||
        !S_ISLNK(attributes.st_mode) || attributes.st_uid != geteuid()) return false;
    char target[PATH_MAX];
    const char prefix[] = "/dev/pts/";
    ssize_t length = readlinkat(directory, name, target, sizeof target);
    if (length < (ssize_t)sizeof prefix || length == (ssize_t)sizeof target ||
        memcmp(target, prefix, sizeof prefix - 1)) return false;
    for (size_t i = sizeof prefix - 1; i < (size_t)length; ++i)
        if (target[i] < '0' || target[i] > '9') return false;
    return true;
}

static int open_port(struct port *port, int directory) {
    port->fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (port->fd < 0 || grantpt(port->fd) || unlockpt(port->fd)) return -1;
    char slave_path[PATH_MAX];
    int error = ptsname_r(port->fd, slave_path, sizeof slave_path);
    if (error) { errno = error; return -1; }
    int slave = open(slave_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (slave < 0) return -1;
    struct termios mode;
    int rc = tcgetattr(slave, &mode);
    if (rc == 0) {
        cfmakeraw(&mode);
        mode.c_cflag |= CLOCAL;
        rc = cfsetspeed(&mode, B115200) || tcsetattr(slave, TCSANOW, &mode);
    }
    error = errno;
    close(slave);
    if (rc) { errno = error; return -1; }
    if (symlinkat(slave_path, directory, port->name)) {
        if (errno != EEXIST) return -1;
        /* The directory lock excludes a live modem using these port names. */
        if (!previous_pty_link(directory, port->name)) { errno = EEXIST; return -1; }
        if (unlinkat(directory, port->name, 0) || symlinkat(slave_path, directory, port->name)) return -1;
    }
    port->published = true;
    return 0;
}

static void disconnect(struct port *port) {
    port->connected = port->in_frame = port->escaped = false;
    port->input_start = port->input_end = port->frame_size = 0;
    port->output_start = port->output_end = port->pending_size = 0;
    tcflush(port->fd, TCIOFLUSH);
}

static size_t output_room(const struct port *port) {
    return sizeof port->output - (port->output_end - port->output_start);
}

static void encode(struct port *port, uint8_t type, const uint8_t *payload, size_t length) {
    if (port->output_end + 2 * length + 3 > sizeof port->output) {
        port->output_end -= port->output_start;
        memmove(port->output, port->output + port->output_start, port->output_end);
        port->output_start = 0;
    }
    uint8_t *out = port->output + port->output_end;
    *out++ = FEND;
    *out++ = type;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = payload[i];
        if (byte == FEND || byte == FESC) {
            *out++ = FESC;
            *out++ = byte == FEND ? TFEND : TFESC;
        } else *out++ = byte;
    }
    *out++ = FEND;
    port->output_end = (size_t)(out - port->output);
}

static void reply(struct port *port, uint8_t subcommand, const void *data, size_t length) {
    uint8_t payload[(MAX_CONTROL_BYTES - 3) / 2] = {subcommand};
    if (length >= sizeof payload) abort();
    if (length) memcpy(payload + 1, data, length);
    encode(port, SET_HARDWARE, payload, length + 1);
}

static void send_error(struct port *port, uint8_t code) { reply(port, ERROR, &code, 1); }

static void tx_done(struct port *port, uint8_t success) {
    reply(port, TX_DONE, &success, 1);
    if (success) ++port->sent;
    else ++port->tx_errors;
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static bool expect(struct port *port, size_t actual, size_t expected) {
    if (actual == expected) return true;
    send_error(port, BAD_LENGTH);
    return false;
}

static int channel_busy(void) {
    if (transmission.active) return 1;
    for (unsigned rf = 0; rf < N_PORTS; ++rf) {
        int busy = sx1250_dual_activity(rf);
        if (busy) return busy;
    }
    return 0;
}

static void apply_profile(unsigned rf, struct sx1250_rx_config profile, uint16_t preamble,
                          bool configure) {
    struct port *port = &ports[rf];
    profile.preamble_symbols = preamble ? preamble : (profile.spreading_factor <= 8 ? 32 : 16);
    if (transmission.active || port->pending_size) { send_error(port, TX_BUSY); return; }
    if (sx1250_dual_reconfigure(rf, &profile)) {
        const int error = errno;
        if (error == EINVAL) send_error(port, BAD_PARAMETER);
        else if (error == EAGAIN || error == EBUSY) send_error(port, TX_BUSY);
        else send_error(port, UNAVAILABLE);
        return;
    }
    port->profile = profile;
    port->preamble_override = preamble;
    if (configure) port->configured = true;
    reply(port, OK, NULL, 0);
}

static void hardware_command(unsigned rf, const uint8_t *command, size_t length) {
    struct port *port = &ports[rf];
    if (length == 0) { send_error(port, BAD_LENGTH); return; }
    const uint8_t op = command[0];
    const uint8_t *data = command + 1;
    const size_t size = length - 1;
    struct sx1250_rx_config profile = port->profile;
    uint8_t out[12];
    const unsigned set_size = op == SET_RADIO ? 10 : op == SET_PREAMBLE ? 2 : 1;
    switch (op) {
    case SET_RADIO:
    case SET_SYNC_WORD:
    case SET_PREAMBLE: {
        if (!expect(port, size, set_size)) return;
        uint16_t preamble = port->preamble_override;
        if (op == SET_RADIO) {
            profile.frequency_hz = read_u32(data);
            profile.bandwidth_hz = read_u32(data + 4);
            profile.spreading_factor = data[8];
            profile.coding_rate = data[9];
        } else if (op == SET_SYNC_WORD) profile.sync_word = data[0];
        else preamble = data[0] | (uint16_t)data[1] << 8;
        apply_profile(rf, profile, preamble, op == SET_RADIO);
        return;
    }
    case SET_TX_POWER: {
        if (!expect(port, size, 1)) return;
        const int8_t power_dbm = data[0] <= INT8_MAX ? data[0] : (int)data[0] - (UINT8_MAX + 1);
        if (!sx1250_tx_power_supported(power_dbm)) { send_error(port, BAD_PARAMETER); return; }
        port->power_dbm = power_dbm;
        reply(port, OK, NULL, 0);
        return;
    }
    case SET_SIGNAL_REPORT:
        if (!expect(port, size, 1)) return;
        port->signal_report = data[0] != 0;
        reply(port, OK, NULL, 0);
        return;
    case GET_AIRTIME:
        if (!expect(port, size, 1)) return;
        write_u32(out, sx1250_lora_airtime_ms(&profile, data[0]));
        reply(port, op | 0x80, out, 4);
        return;
    }
    if (!expect(port, size, 0)) return;
    switch (op) {
    case GET_RADIO:
        write_u32(out, profile.frequency_hz);
        write_u32(out + 4, profile.bandwidth_hz);
        out[8] = profile.spreading_factor;
        out[9] = profile.coding_rate;
        reply(port, op | 0x80, out, 10);
        break;
    case GET_PHY_EXTRA:
        out[0] = profile.sync_word;
        out[1] = (uint8_t)profile.preamble_symbols;
        out[2] = (uint8_t)(profile.preamble_symbols >> 8);
        reply(port, op | 0x80, out, 3);
        break;
    case GET_TX_POWER:
        out[0] = (uint8_t)port->power_dbm;
        reply(port, op | 0x80, out, 1);
        break;
    case GET_SIGNAL_REPORT:
        out[0] = port->signal_report;
        reply(port, op | 0x80, out, 1);
        break;
    case IS_CHANNEL_BUSY: {
        int busy = channel_busy();
        if (busy < 0) send_error(port, UNAVAILABLE);
        else {
            out[0] = busy;
            reply(port, op | 0x80, out, 1);
        }
        break;
    }
    case GET_VERSION:
        reply(port, op | 0x80, (const uint8_t[]){2, 0}, 2);
        break;
    case GET_DEVICE_NAME:
        reply(port, op | 0x80, rf == 0 ? "RAK2287 RF0" : "RAK2287 RF1", sizeof "RAK2287 RF0" - 1);
        break;
    case GET_STATS:
        write_u32(out, port->received);
        write_u32(out + 4, port->sent);
        write_u32(out + 8, port->tx_errors);
        reply(port, op | 0x80, out, sizeof out);
        break;
    case PING:
        reply(port, op | 0x80, NULL, 0);
        break;
    default:
        send_error(port, op > 0 && op <= GET_PHY_EXTRA ? UNAVAILABLE : UNKNOWN_COMMAND);
        break;
    }
}

static void handle_frame(unsigned rf, size_t length, bool allow_tx, int64_t now) {
    struct port *port = &ports[rf];
    const size_t size = length - 1;
    switch (port->frame[0]) {
    case DATA:
        if (size == 0 || size > MAX_PACKET) send_error(port, BAD_LENGTH);
        else if (port->pending_size || (transmission.active && transmission.rf == rf))
            send_error(port, TX_BUSY);
        else if (!allow_tx || !port->configured) tx_done(port, 0);
        else {
            memcpy(port->pending, port->frame + 1, size);
            port->pending_size = size;
            port->pending_power_dbm = port->power_dbm;
            port->earliest_tx = now + port->tx_delay_ms;
            port->tx_deadline = port->earliest_tx + TX_WAIT_MS;
        }
        break;
    case TX_DELAY:
        if (size == 1) port->tx_delay_ms = 10U * port->frame[1];
        break;
    case SET_HARDWARE:
        hardware_command(rf, port->frame + 1, size);
        break;
    }
}

static void decode_byte(unsigned rf, uint8_t byte, bool allow_tx, int64_t now) {
    struct port *port = &ports[rf];
    if (byte == FEND) {
        if (port->in_frame && !port->escaped && port->frame_size)
            handle_frame(rf, port->frame_size, allow_tx, now);
        port->in_frame = true;
        port->escaped = false;
        port->frame_size = 0;
        return;
    }
    if (!port->in_frame) return;
    if (port->escaped) {
        port->escaped = false;
        if (byte == TFEND) byte = FEND;
        else if (byte == TFESC) byte = FESC;
        else { port->in_frame = false; return; }
    } else if (byte == FESC) {
        port->escaped = true;
        return;
    }
    if (port->frame_size == sizeof port->frame) port->in_frame = false;
    else port->frame[port->frame_size++] = byte;
}

static int flush_port(struct port *port) {
    if (port->output_start == port->output_end) return 0;
    ssize_t count = write(port->fd, port->output + port->output_start,
                         port->output_end - port->output_start);
    if (count > 0) port->output_start += (size_t)count;
    else if (count < 0) {
        if (errno == EIO) disconnect(port);
        else if (errno != EAGAIN && errno != EINTR) return -1;
    }
    return 0;
}

static int service_port(unsigned rf, short events, bool allow_tx, int64_t now) {
    struct port *port = &ports[rf];
    if (events & (POLLERR | POLLHUP)) { disconnect(port); return 0; }
    if (events & POLLNVAL) { errno = EBADF; return -1; }
    if ((events & POLLOUT) && flush_port(port)) return -1;
    if (!port->connected) return 0;
    if (events & POLLIN) {
        ssize_t length = read(port->fd, port->input, sizeof port->input);
        if (length == 0 || (length < 0 && errno == EIO)) { disconnect(port); return 0; }
        if (length < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (length > 0) {
            port->input_start = 0;
            port->input_end = (size_t)length;
        }
    }
    while (port->input_start < port->input_end &&
           output_room(port) >= MAX_CONTROL_BYTES + TX_DONE_BYTES)
        decode_byte(rf, port->input[port->input_start++], allow_tx, now);
    return 0;
}

static uint8_t signed_byte(float signal) {
    return (uint8_t)lroundf(fmaxf(-128.0f, fminf(127.0f, signal)));
}

static int forward_rx(unsigned rf) {
    struct port *port = &ports[rf];
    struct sx1250_rx_packet packet;
    int rc = sx1250_dual_next(rf, &packet);
    if (rc <= 0) return rc;
    if (!port->connected || !port->configured) return 0;
    ++port->received;
    const size_t metadata = port->signal_report ? 9 : 0;
    /* Reserve space for a control reply and a pending TX's final status. */
    if (output_room(port) < 2U * packet.size + 3 + metadata + MAX_CONTROL_BYTES + TX_DONE_BYTES)
        return 0;
    encode(port, DATA, packet.payload, packet.size);
    if (port->signal_report) {
        uint8_t metrics[] = {signed_byte(packet.snr_db * 4), signed_byte(packet.rssi_dbm)};
        reply(port, RX_META, metrics, sizeof metrics);
    }
    return 0;
}

static void *send_on_rf0(void *data) {
    struct transmission *job = data;
    job->result = sx1250_dual_send_with_power(&job->profile, job->power_dbm, job->payload, job->size,
                                             job->timeout_ms, &job->report);
    job->error = job->result < 0 ? errno : 0;
    return NULL;
}

static int start_transmission(void) {
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    /* Handle shutdown on the serial loop. A signal must not interrupt the
     * worker's radio wait and turn a completed send into an uncertain one. */
    int error = pthread_sigmask(SIG_BLOCK, &signals, &previous);
    if (error) return error;
    error = pthread_create(&transmission.thread, NULL, send_on_rf0, &transmission);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    transmission.active = error == 0;
    return error;
}

static int collect_transmission(unsigned *turn, int64_t *next_tx) {
    if (!transmission.active) return 0;
    int error = pthread_tryjoin_np(transmission.thread, NULL);
    if (error == EBUSY) return 0;
    if (error) { errno = error; return -1; }
    transmission.active = false;
    const struct sx1250_handoff_report *report = &transmission.report;
    if (transmission.error == EAGAIN && !report->tx_started) return 0;
    struct port *port = &ports[transmission.rf];
    /* Hangup clears the slot before collection. Until then, it belongs to
     * this submission and cannot be reused by a reconnecting client. */
    if (port->pending_size) {
        port->pending_size = 0;
        tx_done(port, transmission.result == 0);
        if (flush_port(port)) return -1;
    }
    *turn = (transmission.rf + 1) % N_PORTS;
    if (report->tx_started) {
        const int64_t ended = clock_ms();
        if (ended < 0) return -1;
        *next_tx = ended + 9 * (int64_t)transmission.airtime_ms;
    }
    if (transmission.result < 0 && (!report->rf0_rearmed || !report->rf1_rearmed)) {
        errno = transmission.error;
        return -1;
    }
    return 0;
}

static int schedule_tx(unsigned turn, int64_t next_tx, int64_t now) {
    if (stopping || transmission.active || now < next_tx) return 0;
    for (unsigned i = 0; i < N_PORTS; ++i) {
        const unsigned rf = (turn + i) % N_PORTS;
        struct port *port = &ports[rf];
        if (!port->pending_size || now < port->earliest_tx) continue;
        const uint32_t airtime = sx1250_lora_airtime_ms(&port->profile, (uint8_t)port->pending_size);
        if (!airtime || airtime > SX1250_TX_TIMEOUT_MAX_MS - TX_MARGIN_MS) {
            port->pending_size = 0;
            tx_done(port, 0);
            continue;
        }
        int busy = channel_busy();
        if (busy) return busy > 0 ? 0 : -1;
        transmission = (struct transmission){
            .rf = rf, .profile = port->profile, .size = port->pending_size,
            .power_dbm = port->pending_power_dbm,
            .airtime_ms = airtime, .timeout_ms = 2 * airtime + TX_MARGIN_MS,
        };
        if (transmission.timeout_ms > SX1250_TX_TIMEOUT_MAX_MS)
            transmission.timeout_ms = SX1250_TX_TIMEOUT_MAX_MS;
        memcpy(transmission.payload, port->pending, port->pending_size);
        int error = start_transmission();
        if (error) {
            fprintf(stderr, "Start RF0 transmission: %s\n", strerror(error));
            port->pending_size = 0;
            tx_done(port, 0);
        }
        return 0;
    }
    return 0;
}

static int run(bool allow_tx) {
    unsigned turn = 0;
    int64_t next_tx = 0;
    while (!stopping) {
        struct pollfd descriptors[N_PORTS];
        for (unsigned i = 0; i < N_PORTS; ++i) {
            struct port *port = &ports[i];
            /* A master without a slave reports HUP continuously. Probe it without
             * waking the main poll until a client opens that slave. */
            struct pollfd probe = {.fd = port->fd};
            if (!port->connected && poll(&probe, 1, 0) == 0) port->connected = true;
            short events = 0;
            if (port->input_start == port->input_end &&
                output_room(port) >= MAX_CONTROL_BYTES + TX_DONE_BYTES) events |= POLLIN;
            if (port->output_start != port->output_end) events |= POLLOUT;
            descriptors[i] = (struct pollfd){.fd = port->connected ? port->fd : -1,
                                            .events = events};
        }
        if (poll(descriptors, N_PORTS, 2) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        const int64_t now = clock_ms();
        if (now < 0) return -1;
        for (unsigned i = 0; i < N_PORTS; ++i)
            if (service_port(i, descriptors[i].revents, allow_tx, now)) return -1;
        if (collect_transmission(&turn, &next_tx)) return -1;
        for (unsigned i = 0; !transmission.active && i < N_PORTS; ++i)
            if (forward_rx(i) < 0) return -1;
        const int64_t transmit_time = clock_ms();
        if (transmit_time < 0) return -1;
        for (unsigned i = 0; i < N_PORTS; ++i) {
            struct port *port = &ports[i];
            if (transmission.active && transmission.rf == i) continue;
            if (port->pending_size && transmit_time >= port->tx_deadline) {
                port->pending_size = 0;
                tx_done(port, 0);
            }
        }
        if (schedule_tx(turn, next_tx, transmit_time)) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || (argc == 4 && strcmp(argv[3], "--enable-transmit"))) {
        fputs("Usage: rak-kiss MODEM_DIRECTORY SPI_DEVICE [--enable-transmit]\n", stderr);
        return 2;
    }
    int directory = argv[1][0] == '/' ? open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat attributes;
    if (directory < 0 || fstat(directory, &attributes) || attributes.st_uid != geteuid() ||
        (attributes.st_mode & 077)) {
        if (directory >= 0) close(directory);
        fputs("Modem directory must be an absolute, private directory owned by the modem user.\n", stderr);
        return 1;
    }
    if (flock(directory, LOCK_EX | LOCK_NB)) {
        perror("lock modem directory");
        close(directory);
        return 1;
    }
    const bool allow_tx = argc == 4;
    struct sx1250_rx_config profiles[N_PORTS];
    umask(0077);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    int rc = 1;
    for (unsigned i = 0; i < N_PORTS; ++i) {
        profiles[i] = (struct sx1250_rx_config){
            .frequency_hz = 868100000, .bandwidth_hz = 125000, .preamble_symbols = 32,
            .spreading_factor = 7, .coding_rate = 5, .sync_word = 0x12, .require_crc = true,
        };
        ports[i].profile = profiles[i];
        ports[i].power_dbm = SX1250_RF0_TX_SETTING_DBM;
        ports[i].signal_report = true;
        if (open_port(&ports[i], directory)) { perror("open KISS port"); goto cleanup; }
    }
    if (sx1250_dual_open(argv[2], profiles)) { perror("sx1250_dual_open"); goto cleanup; }
    printf("RAK_KISS_READY rf0=%s/rf0 rf1=%s/rf1 tx_enabled=%d\n", argv[1], argv[1], allow_tx);
    fflush(stdout);
    rc = run(allow_tx) < 0;
    if (rc) perror("rak-kiss");
    if (transmission.active) pthread_join(transmission.thread, NULL);
    if (sx1250_dual_close()) { perror("sx1250_dual_close"); rc = 1; }
cleanup:
    for (unsigned i = 0; i < N_PORTS; ++i) {
        if (ports[i].published) unlinkat(directory, ports[i].name, 0);
        if (ports[i].fd >= 0) close(ports[i].fd);
    }
    close(directory);
    return rc;
}
