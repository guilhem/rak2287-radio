#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include "sx1250_rx.h"
#include "sx1250_usp.h"
#include "sx126x_hal.h"
#include "fake_hal.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    const struct sx1250_rx_config narrow = {
        .frequency_hz = 869618000, .bandwidth_hz = 62500, .preamble_symbols = 16,
        .spreading_factor = 8, .coding_rate = 8, .sync_word = 0x12, .require_crc = true,
    };
    struct sx1250_rx_config wide = narrow;
    wide.frequency_hz = 869525000;
    wide.bandwidth_hz = 250000;
    wide.spreading_factor = 11;
    wide.coding_rate = 5;
    wide.sync_word = 0x2b;
    assert(sx1250_lora_airtime_ms(&narrow, 32) == 411);
    assert(sx1250_lora_airtime_ms(&narrow, 53) == 575);
    assert(sx1250_lora_airtime_ms(&narrow, 0) == 0);
    assert(sx1250_rx_reconfigure(&narrow, false) < 0 && errno == ENODEV);

    setenv("IDLE", "1", 1);
    setenv("FAKE_TX", "1", 1); /* Enables RX frame injection in the model. */
    assert(!sx1250_rx_open(argv[1], &narrow));
    const void *radio = &sx1250_usp_rf1;
    assert(sx126x_set_tx(radio, 1) != SX126X_STATUS_OK);
    assert(sx126x_set_tx_cw(radio) != SX126X_STATUS_OK);
    assert(sx126x_set_tx_infinite_preamble(radio) != SX126X_STATUS_OK);
    assert(sx126x_write_register(radio, 0x0587, (uint8_t[]){0x0b}, 1) != SX126X_STATUS_OK);
    assert(sx126x_set_rx(NULL, 0) != SX126X_STATUS_OK);
    uint8_t readback[3];
    assert(sx126x_hal_read(radio, (uint8_t[]){0x83, 0}, 2, readback, sizeof readback) != SX126X_HAL_STATUS_OK);
    assert(sx1250_rx_activity() == 0);

    assert(fake_rx_progress(4));
    assert(sx1250_rx_reconfigure(&wide, false) < 0 && errno == EAGAIN);
    setenv("EXPECTED_BW", "5", 1);
    assert(sx1250_rx_reconfigure(&wide, true) == 0);
    unsetenv("EXPECTED_BW");
    uint8_t data[] = {1, 2, 3};
    struct sx1250_rx_packet packet;
    assert(fake_inject_rx(data, sizeof data));
    assert(sx1250_rx_reconfigure(&narrow, true) < 0 && errno == EAGAIN);
    assert(sx1250_rx_next(&packet, 0) == 1 && packet.coding_rate == 5 && packet.size == sizeof data);
    for (size_t i = 0; i < sizeof data; ++i) assert(packet.payload[i] == data[i]);

    /* A completed packet can latch after the last live status check. */
    setenv("FAKE_RX_DONE_ON_STANDBY", "1", 1);
    assert(sx1250_rx_reconfigure(&narrow, false) < 0 && errno == EAGAIN);
    unsetenv("FAKE_RX_DONE_ON_STANDBY");
    assert(sx1250_rx_next(&packet, 0) == 1);
    setenv("EXPECTED_BW", "3", 1);
    assert(sx1250_rx_reconfigure(&narrow, false) == 0);
    assert(fake_inject_rx(data, sizeof data));
    assert(sx1250_rx_next(&packet, 0) == 1 && packet.coding_rate == 8);
    unsetenv("EXPECTED_BW");

    assert(fake_inject_rx(data, sizeof data));
    assert(sx1250_rx_reconfigure(&wide, true) < 0 && errno == EAGAIN);
    assert(sx1250_rx_drain_for_switch(&packet) == 1);
    assert(packet.coding_rate == 8 && packet.size == sizeof data);
    for (size_t i = 0; i < sizeof data; ++i) assert(packet.payload[i] == data[i]);
    assert(!fake_inject_rx(data, sizeof data)); /* RF1 stayed in standby. */
    assert(sx1250_rx_drain_for_switch(&packet) < 0 && errno == EINVAL);
    assert(sx1250_rx_reconfigure(&wide, true) == 0);
    assert(sx1250_rx_next(&packet, 0) == 0);

    assert(fake_air(7, 0x52)); /* RxDone with CRC error. */
    assert(sx1250_rx_reconfigure(&narrow, true) < 0 && errno == EAGAIN);
    packet.size = 0x55;
    assert(sx1250_rx_drain_for_switch(&packet) == 0 && packet.size == 0x55);
    assert(!fake_inject_rx(data, sizeof data));
    assert(sx1250_rx_reconfigure(&narrow, true) == 0);
    assert(sx1250_rx_next(&packet, 0) == 0);
    assert(!sx1250_rx_close());
}
