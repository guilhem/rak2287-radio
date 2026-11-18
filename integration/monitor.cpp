#include "MyMesh.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <unistd.h>

class StdinRadio : public ExternalRadio {
    uint8_t frame[MAX_TRANS_UNIT]{};
    int size = 0;
public:
    bool load(const std::string& line);
    int recvRaw(uint8_t* dest, int capacity) override;
    bool isInRecvMode() const override { return true; }
    bool startSendRaw(const uint8_t*, int) override { ++blocked; return false; }
    bool isSendComplete() override { return false; }
    void onSendFinished() override {}
    uint32_t blocked = 0;
};
static StdinRadio input;
ExternalRadio& radio_driver = input;
static SimpleMeshTables tables;

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decimal(std::string_view field, float& value) {
    if (!field.empty() && field.front() == '+') {
        field.remove_prefix(1);
        if (!field.empty() && field.front() == '-') return false;
    }
    if (field.empty()) return false;
    auto result = std::from_chars(field.data(), field.data() + field.size(), value);
    return result.ec == std::errc{} && result.ptr == field.data() + field.size()
        && std::isfinite(value);
}

bool StdinRadio::load(const std::string& line) {
    std::array<std::string_view, 3> fields;
    std::string_view tail(line);
    for (auto& field : fields) {
        auto end = tail.find('\t');
        if (end == tail.npos) return false;
        field = tail.substr(0, end);
        tail.remove_prefix(end + 1);
    }
    float newRssi, newSnr;
    if (fields[0] != "RX" || !decimal(fields[1], newRssi) || !decimal(fields[2], newSnr)
        || newRssi < -256 || newRssi > 127 || newSnr < -32 || newSnr > 31.75f
        || tail.empty() || tail.size() % 2 || tail.size() > 2 * sizeof(frame)) return false;
    for (size_t i = 0; i < tail.size(); i += 2) {
        int hi = hexDigit(tail[i]), lo = hexDigit(tail[i + 1]);
        if (hi < 0 || lo < 0) return false;
        frame[i / 2] = static_cast<uint8_t>(hi * 16 + lo);
    }
    int len = static_cast<int>(tail.size() / 2);
    if (!validEnvelope(frame, len)) return false;
    size = len;
    rssi = newRssi;
    snr = newSnr;
    return true;
}

int StdinRadio::recvRaw(uint8_t* dest, int capacity) {
    if (!size) return 0;
    int n = size;
    size = 0;
    if (n > capacity) { reject(); return 0; }
    std::memcpy(dest, frame, n);
    ++received;
    return n;
}

class Monitor : public MyMesh {
public:
    using MyMesh::MyMesh;
    uint64_t routed = 0, repeat = 0;

protected:
    int calcRxDelay(float, uint32_t) const override { return 0; }

    mesh::DispatcherAction onRecvPacket(mesh::Packet* p) override {
        auto dupCount = [] { return tables.getNumDirectDups() + tables.getNumFloodDups(); };
        auto before = dupCount();
        uint8_t hash[MAX_HASH_SIZE];
        p->calculatePacketHash(hash);
        unsigned type = p->getPayloadType(), hops = p->getPathHashCount();
        bool flood = p->isRouteFlood();
        auto action = MyMesh::onRecvPacket(p);
        bool wouldRepeat = action != ACTION_RELEASE && action != ACTION_MANUAL_HOLD;
        ++routed;
        repeat += wouldRepeat;
        std::cout << "ROUTE hash=";
        for (unsigned n : hash) std::cout << std::hex << std::setw(2) << std::setfill('0') << n;
        std::cout << std::dec << " type=" << type << " route=" << (flood ? "flood" : "direct")
                  << " hops=" << hops << " rssi=" << radio_driver.getLastRSSI()
                  << " snr=" << radio_driver.getLastSNR() << " duplicate=" << (dupCount() != before)
                  << " repeat_requested=" << wouldRepeat << std::endl;
        return wouldRepeat ? ACTION_RELEASE : action;
    }

    // An observer never accepts remote administration or creates login sessions.
    void onAnonDataRecv(mesh::Packet*, const uint8_t*, const mesh::Identity&, uint8_t*, size_t) override {}
    void onPeerDataRecv(mesh::Packet*, uint8_t, int, const uint8_t*, uint8_t*, size_t) override {}
};

static volatile std::sig_atomic_t stopping;
static void stop(int) { stopping = 1; }

int main() {
    const char* dir = std::getenv("MESHCORE_DATA_DIR");
    if (!dir || !std::filesystem::is_directory(dir)) {
        std::cerr << "Set MESHCORE_DATA_DIR to an existing dedicated directory.\n";
        return 1;
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    static ArduinoMillis clock;
    static KernelRandom random;
    static Monitor mesh(board, radio_driver, clock, random, rtc_clock, tables);
    rtc_clock.setCurrentTime(std::time(nullptr));
    mesh.self_id = mesh::LocalIdentity(&random);
    mesh.begin(&LittleFS);
    mesh.getNodePrefs()->advert_interval = mesh.getNodePrefs()->flood_advert_interval = 0;
    mesh.updateAdvertTimer();
    mesh.updateFloodAdvertTimer();

    char buffer[4096];
    std::string line;
    bool oversized = false;
    pollfd in{STDIN_FILENO, POLLIN, 0};
    while (!stopping) {
        rtc_clock.tick();
        mesh.loop();
        int ready = poll(&in, 1, 20);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (in.revents & (POLLERR | POLLNVAL))) { perror("stdin poll"); return 1; }
        if (!ready) continue;
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { perror("stdin read"); return 1; }
        if (n == 0) {
            if (!line.empty() || oversized) radio_driver.reject();
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            if (buffer[i] == '\n') {
                if (oversized || !input.load(line)) radio_driver.reject();
                else mesh.loop();
                line.clear();
                oversized = false;
            } else if (line.size() >= 576) {
                oversized = true;
            } else {
                line += buffer[i];
            }
        }
    }
    std::cout << "SUMMARY routed=" << mesh.routed << " flood=" << mesh.getNumRecvFlood()
              << " direct=" << mesh.getNumRecvDirect() << " flood_dups=" << tables.getNumFloodDups()
              << " direct_dups=" << tables.getNumDirectDups() << " repeat_requested=" << mesh.repeat
              << " rejected=" << radio_driver.getPacketsRecvErrors()
              << " send_blocked=" << input.blocked << " sent=0" << std::endl;
}
