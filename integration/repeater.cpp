#include "MyMesh.h"
#include "repeater_queue.h"
#include "sx1250_rx.h"
#include "sx1250_tx.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile std::sig_atomic_t stopping;
static void stop(int) { stopping = 1; }

class Radio : public ExternalRadio {
    bool txDone = false;
    bool deferred = false;

public:
    int failure = 0;
    unsigned busyDeferrals = 0;
    bool takeDeferred() {
        bool result = deferred;
        deferred = false;
        return result;
    }
    bool isReceiving() override {
        if (failure || stopping) return true;
        int activity = sx1250_rx_activity();
        if (activity < 0) {
            failure = errno;
            std::perror("sx1250_rx_activity");
        }
        return activity != 0;
    }

    int recvRaw(uint8_t* dest, int capacity) override {
        if (failure || stopping) return 0;
        sx1250_rx_packet p{};
        int n = sx1250_rx_next(&p, 0);
        if (n < 0) {
            failure = errno;
            std::perror("sx1250_rx_next");
            return 0;
        }
        if (!n) return 0;
        if (p.size > capacity || p.coding_rate != 8 || !p.crc_present ||
            p.implicit_header || !validEnvelope(p.payload, p.size)) {
            reject();
            return 0;
        }
        std::memcpy(dest, p.payload, p.size);
        rssi = p.rssi_dbm;
        snr = p.snr_db;
        ++received;
        return p.size;
    }

    bool startSendRaw(const uint8_t* data, int len) override {
        if (failure || stopping || len <= 0 || len > MAX_TRANS_UNIT) return false;
        // Serialized driver operation: success means RF0 TxDone AND RF1 rearmed.
        const unsigned timeout = 2 * getEstAirtimeFor(len) + 500;
        int rc = sx1250_tx_send(data, len, timeout);
        if (rc < 0 && errno == EAGAIN) {
            deferred = true;
            ++busyDeferrals;
            return false;
        }
        if (rc < 0) {
            failure = errno;
            std::perror("sx1250_tx_send");
            return false;
        }
        txDone = true;
        ++sent;
        return true;
    }

    bool isSendComplete() override { return txDone; }
    void onSendFinished() override { txDone = false; }
    bool isInRecvMode() const override { return !failure; }
};

static Radio physical;
ExternalRadio& radio_driver = physical;

// Forwarding is handled by simple_repeater. This target exposes no remote CLI.
class Repeater : public MyMesh {
    RepeaterQueue queue;
public:
    Repeater(mesh::MainBoard& b, mesh::Radio& r, mesh::MillisecondClock& clock,
             mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
        : MyMesh(b, r, clock, rng, rtc, tables), queue(*_mgr) { _mgr = &queue; }

protected:
    void logTxFail(mesh::Packet* packet, int len) override {
        if (physical.takeDeferred()) {
            queue.defer(packet, futureMillis(getCADFailRetryDelay()));
        } else {
            MyMesh::logTxFail(packet, len);
        }
    }
    void onAnonDataRecv(mesh::Packet*, const uint8_t*, const mesh::Identity&, uint8_t*, size_t) override {}
    void onPeerDataRecv(mesh::Packet*, uint8_t, int, const uint8_t*, uint8_t*, size_t) override {}
};

// Use the native firmware's 96-byte public/private identity file. Serialize
// all access to state. Atomic rename preserves an existing identity on failure.
static bool identity(int dir, mesh::LocalIdentity& id, KernelRandom& rng) {
    IdentityStore store(LittleFS, "");
    const char* name = "repeater.id";
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 077) || st.st_size != PUB_KEY_SIZE + PRV_KEY_SIZE) return false;
        if (!store.load("repeater", id)) return false;
        static const uint8_t message[] = "MeshCore identity check";
        uint8_t signature[SIGNATURE_SIZE];
        id.sign(signature, message, sizeof message);
        return id.verify(signature, message, sizeof message);
    }
    if (errno != ENOENT) return false;
    const char* pending = "repeater.pending.id";
    if (unlinkat(dir, pending, 0) < 0 && errno != ENOENT) return false;
    do { id = mesh::LocalIdentity(&rng); }
    while (id.pub_key[0] == 0 || id.pub_key[0] == 255);
    if (!store.save("repeater.pending", id)) return false;
    int fd = openat(dir, pending, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    int rc = fsync(fd);
    int closed = close(fd);
    return !rc && !closed && renameat(dir, pending, dir, name) == 0 && fsync(dir) == 0;
}

int main(int argc, char** argv) {
    if (argc != 4 || std::strcmp(argv[1], "--enable-transmit") ||
        std::strcmp(argv[2], "--device") || !*argv[3]) {
        std::fputs("Usage: meshcore-radio-repeater --enable-transmit --device SPI_PATH\n", stderr);
        return 2;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    umask(0077);
    const char* state = std::getenv("MESHCORE_DATA_DIR");
    int dir = state ? open(state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat st;
    if (dir < 0 || fstat(dir, &st) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || flock(dir, LOCK_EX | LOCK_NB)) {
        std::fputs("MESHCORE_DATA_DIR must be a private, owned, idle directory.\n", stderr);
        return 1;
    }
    static ArduinoMillis clock;
    static KernelRandom rng;
    static SimpleMeshTables tables;
    static Repeater mesh(board, physical, clock, rng, rtc_clock, tables);
    if (!identity(dir, mesh.self_id, rng)) {
        std::fputs("Cannot load or create the private repeater identity.\n", stderr);
        return 1;
    }
    std::printf("Repeater public key: ");
    for (unsigned n : mesh.self_id.pub_key) std::printf("%02x", n);
    std::puts("");
    std::fflush(stdout);
    sx1250_rx_config profile{869618000, 62500, 16, 8, 8, 0x12, true, false, 0};
    if (sx1250_rx_open(argv[3], &profile) < 0) {
        std::perror("sx1250_rx_open");
        return 1;
    }
    rtc_clock.setCurrentTime(std::time(nullptr));
    mesh.begin(&LittleFS);
    mesh.getNodePrefs()->freq = profile.frequency_hz / 1000000.0f;
    mesh.getNodePrefs()->bw = profile.bandwidth_hz / 1000.0f;
    mesh.getNodePrefs()->sf = profile.spreading_factor;
    mesh.getNodePrefs()->cr = profile.coding_rate;
    float& silence = mesh.getNodePrefs()->airtime_factor;
    if (std::isnan(silence) || silence < 9.0f) silence = 9.0f;
    // Dispatcher::futureMillis takes a signed int. Reject unrepresentable
    // preferences instead of silently shortening a requested quiet period.
    if (!std::isfinite(silence) || silence > INT_MAX / 120000) {
        std::fputs("Airtime factor exceeds the dispatcher timing range.\n", stderr);
        physical.failure = EINVAL;
    }
    std::printf("Post-send silence factor: %g\n", silence);
    while (!stopping && !physical.failure) {
        rtc_clock.tick();
        mesh.loop();
        usleep(2000);
    }
    int closed = sx1250_rx_close();
    if (closed) std::perror("sx1250_rx_close");
    std::printf("SUMMARY received=%u flood=%u direct=%u flood_dups=%u direct_dups=%u "
                "rejected=%u sent=%u flood_tx=%u direct_tx=%u busy_deferrals=%u\n",
                physical.getPacketsRecv(), mesh.getNumRecvFlood(), mesh.getNumRecvDirect(),
                tables.getNumFloodDups(), tables.getNumDirectDups(), physical.getPacketsRecvErrors(),
                physical.getPacketsSent(), mesh.getNumSentFlood(), mesh.getNumSentDirect(), physical.busyDeferrals);
    close(dir);
    return (closed || physical.failure) ? 1 : 0;
}
