#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sx1250_rx.h"

extern bool fake_air(uint8_t payload_tag, unsigned flags);

int main(int argc, char **argv) {
    assert(argc == 2);
    struct sx1250_rx_config c = {869618000, 62500, 16, 8, 8, 0x12, true, false, 0};
    struct sx1250_rx_packet p = {0};
    assert(sx1250_rx_next(&p, 0) < 0 && errno == ENODEV);
    c.bandwidth_hz = 50001;
    assert(sx1250_rx_open(argv[1], &c) < 0 && errno == EINVAL);
    c.bandwidth_hz = 62500;
    setenv("BUSY_FW", "1", 1);
    assert(sx1250_rx_open(argv[1], &c) < 0 && errno == EBUSY);
    unsetenv("BUSY_FW");
    setenv("FAIL_CONNECT", "1", 1);
    assert(sx1250_rx_open(argv[1], &c) < 0 && errno == EIO);
    unsetenv("FAIL_CONNECT");
    assert(!sx1250_rx_open(argv[1], &c));
    assert(sx1250_rx_open(argv[1], &c) < 0 && errno == EBUSY);
    assert(sx1250_rx_next(&p, 1000) == 1);
    assert(p.size == 255 && p.coding_rate == 6 && p.crc_present && !p.implicit_header);
    assert(p.rssi_dbm == -80.5f && p.snr_db == -1.75f);
    for (unsigned i = 0; i < p.size; i++) assert(p.payload[i] == (uint8_t)(230+i));
    memset(&p, 0x5A, sizeof p);
    struct sx1250_rx_packet saved;
    memcpy(&saved, &p, sizeof p);
    assert(!sx1250_rx_next(&p, 15) && !memcmp(&p, &saved, sizeof p));
    assert(!sx1250_rx_close());
    c.bandwidth_hz=250000; c.spreading_factor=12; c.coding_rate=5;
    c.require_crc=false; c.inverted_iq=true; c.preamble_symbols=12;
    setenv("ALT_PROFILE", "1", 1);
    assert(!sx1250_rx_open(argv[1], &c));
    assert(sx1250_rx_next(&p, 1000) == 1 && !p.crc_present);
    assert(!sx1250_rx_close());
    unsetenv("ALT_PROFILE");
    setenv("IDLE", "1", 1);
    setenv("FALSE_STANDBY", "1", 1);
    assert(sx1250_rx_open(argv[1], &c) < 0 && errno == EIO);
    unsetenv("FALSE_STANDBY");
    setenv("EARLY", "1", 1);
    assert(!sx1250_rx_open(argv[1], &c)); /* Arrived before GetStatus. */
    assert(!fake_air(42, 0x52)); /* A slow reader still owns the first payload/IRQ. */
    assert(sx1250_rx_next(&p, 0) == 1);
    for (unsigned i = 0; i < p.size; i++) assert(p.payload[i] == (uint8_t)(230+i+7));
    assert(fake_air(23, 0x12));
    assert(!fake_air(91, 0x12));
    assert(sx1250_rx_next(&p, 0) == 1);
    for (unsigned i = 0; i < p.size; i++) assert(p.payload[i] == (uint8_t)(230+i+23));
    assert(!sx1250_rx_close());
    unsetenv("EARLY");
    c.implicit_length = 7;
    c.require_crc = true;
    setenv("IMPLICIT", "1", 1);
    assert(!sx1250_rx_open(argv[1], &c));
    assert(fake_air(2, 0x42)); /* RxDone + CrcErr. */
    assert(!sx1250_rx_next(&p, 0));
    assert(fake_air(3, 0x02)); /* Descriptor says 7, register says 8: discard. */
    assert(!sx1250_rx_next(&p, 0));
    assert(fake_air(5, 0x02)); /* Stale descriptor says 193; register says 7. */
    assert(sx1250_rx_next(&p, 0) == 1 && p.size == 7 && p.implicit_header);
    assert(p.crc_present && p.coding_rate == 5 && p.payload[0] == 235);
    assert(!sx1250_rx_close());
    unsetenv("IMPLICIT");
    c.implicit_length = 0;
    unsetenv("IDLE");
    static const uint32_t bandwidths[] = {7812,10417,15625,20833,31250,41667,62500,125000,250000,500000};
    static const char *codes[] = {"0","8","1","9","2","10","3","4","5","6"};
    for (unsigned i = 0; i < sizeof bandwidths / sizeof bandwidths[0]; ++i) {
        c.bandwidth_hz = bandwidths[i];
        setenv("EXPECTED_BW", codes[i], 1);
        assert(!sx1250_rx_open(argv[1], &c));
        assert(!sx1250_rx_close());
    }
}
