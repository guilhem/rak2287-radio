#include "target.h"

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>

MonitorBoard board;
VolatileRTCClock rtc_clock;
SensorManager sensors;

bool validEnvelope(const uint8_t* data, int len) {
    if (len < 2) return false;
    mesh::Packet p;
    p.header = data[0];
    int pathPos = p.hasTransportCodes() ? 5 : 1;
    if (p.getPayloadVer() != PAYLOAD_VER_1 || pathPos >= len) return false;
    p.path_len = data[pathPos];
    if (!mesh::Packet::isValidPathLen(p.path_len)) return false;
    int payload = len - pathPos - 1 - p.getPathByteLen();
    if (payload <= 0 || payload > MAX_PACKET_PAYLOAD) return false;
    // The native direct trace and multipart handlers read these prefixes.
    if (p.getPayloadType() == PAYLOAD_TYPE_TRACE) {
        if (payload < 9) return false;
        unsigned hashShift = data[len - payload + 8] & 3;
        return (payload - 9) % (1 << hashShift) == 0;
    }
    if (p.getPayloadType() == PAYLOAD_TYPE_MULTIPART) return payload >= 5;
    if (p.getPayloadType() == PAYLOAD_TYPE_ACK) return payload >= 4;
    return true;
}

uint32_t ExternalRadio::getEstAirtimeFor(int len) {
    // EU Narrow SF8, BW62500, CR4/8, explicit header, CRC, 16-symbol preamble.
    double symbols = 16 + 4.25 + 8 + std::ceil((8.0 * len + 12) / 32) * 8;
    return static_cast<uint32_t>(symbols * 4.096);
}

float ExternalRadio::packetScore(float value, int len) {
    // Same SNR/length score as the native SX1262 target.
    float quality = (std::clamp(value, -20.0f, 10.0f) + 20.0f) / 30.0f;
    return quality * (1.0f - 0.6f * std::min(1.f, len / 255.f));
}

void KernelRandom::random(uint8_t* p, size_t n) {
    while (n) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) throw std::runtime_error("getrandom failed");
        p += got;
        n -= got;
    }
}
