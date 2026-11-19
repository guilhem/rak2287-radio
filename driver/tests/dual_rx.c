#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "fake_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1250.h"
#include "sx1250_dual.h"
#include "sx1250_rx_internal.h"
#include "sx1250_usp.h"

static void expect_frequency(unsigned rf, uint32_t hz) {
    uint32_t pll = SX1250_FREQ_TO_REG(hz);
    uint8_t expected[] = {pll >> 24, pll >> 16, pll >> 8, pll}, actual[4];
    assert(!sx126x_read_register(&sx1250_usp_radios[rf], 0x088b, actual, sizeof actual));
    assert(!memcmp(expected, actual, sizeof actual));
}

static void test_reconfigure(const struct sx1250_rx_config profiles[2]) {
    struct sx1250_rx_config updated = profiles[0];
    updated.frequency_hz = 915600000;
    updated.sync_word = 0x2b;
    const uint8_t payload[] = {0xc0, 0xdb, 0x12};
    const unsigned rf0_calibrations = fake_image_calibrations(0);
    const unsigned rf1_calibrations = fake_image_calibrations(1);
    struct sx1250_rx_packet packet;

    assert(sx1250_dual_reconfigure(2, &updated) < 0 && errno == EINVAL);
    assert(sx1250_dual_reconfigure(1, NULL) < 0 && errno == EINVAL);
    struct sx1250_rx_config invalid = updated;
    invalid.frequency_hz = 870000000;
    assert(sx1250_dual_reconfigure(1, &invalid) < 0 && errno == EINVAL);
    assert(fake_image_calibrations(1) == rf1_calibrations);

    assert(fake_rx_progress(4));
    assert(sx1250_dual_reconfigure(1, &updated) < 0 && errno == EAGAIN);
    assert(fake_inject_rx(payload, sizeof payload));
    assert(sx1250_dual_reconfigure(1, &updated) < 0 && errno == EAGAIN);
    assert(sx1250_dual_next(1, &packet) == 1 && packet.coding_rate == profiles[1].coding_rate);
    assert(packet.size == sizeof payload && !memcmp(packet.payload, payload, sizeof payload));

    assert(!setenv("FAKE_RX_DONE_ON_STANDBY", "1", 1));
    assert(sx1250_dual_reconfigure(1, &updated) < 0 && errno == EAGAIN);
    assert(!unsetenv("FAKE_RX_DONE_ON_STANDBY"));
    expect_frequency(1, profiles[1].frequency_hz);
    assert(sx1250_dual_next(1, &packet) == 1 && packet.crc_present);

    /* A ready frame on the other receiver survives the reconfiguration. */
    assert(fake_inject_rf0(payload, sizeof payload));
    assert(!sx1250_dual_reconfigure(1, &updated));
    expect_frequency(0, profiles[0].frequency_hz);
    expect_frequency(1, updated.frequency_hz);
    assert(fake_image_band(1) == 902000000 && fake_image_calibrations(1) == rf1_calibrations + 1);
    assert(sx1250_dual_next(0, &packet) == 1 && packet.coding_rate == profiles[0].coding_rate);
    assert(packet.size == sizeof payload && !memcmp(packet.payload, payload, sizeof payload));

    assert(fake_inject_rx(payload, sizeof payload));
    assert(!sx1250_dual_reconfigure(0, &profiles[1]));
    expect_frequency(0, profiles[1].frequency_hz);
    assert(sx1250_dual_next(1, &packet) == 1 && packet.coding_rate == updated.coding_rate);
    assert(packet.size == sizeof payload && !memcmp(packet.payload, payload, sizeof payload));

    assert(!sx1250_dual_pause());
    assert(sx1250_dual_reconfigure(1, &profiles[1]) < 0 && errno == EBUSY);
    assert(!sx1250_dual_resume());
    expect_frequency(0, profiles[1].frequency_hz);
    expect_frequency(1, updated.frequency_hz);
    assert(fake_image_calibrations(0) == rf0_calibrations);
    assert(fake_image_calibrations(1) == rf1_calibrations + 1);
    uint8_t sync[2];
    assert(!sx126x_read_register(&sx1250_usp_radios[1], 0x0740, sync, sizeof sync));
    assert(sync[0] == 0x24 && sync[1] == 0xb4);

    /* If a latched error prevents warm rollback, recover both old profiles. */
    fake_fail_image_calibration(1);
    assert(sx1250_dual_reconfigure(1, &profiles[1]) < 0 && errno == EIO);
    expect_frequency(0, profiles[1].frequency_hz);
    expect_frequency(1, updated.frequency_hz);
    assert(fake_image_band(0) == 863000000 && fake_image_calibrations(0) == rf0_calibrations + 1);
    assert(fake_image_band(1) == 902000000 && fake_image_calibrations(1) == rf1_calibrations + 4);
    assert(fake_inject_rf0(payload, sizeof payload));
    assert(sx1250_dual_next(0, &packet) == 1 && packet.coding_rate == profiles[1].coding_rate);
    assert(fake_inject_rx(payload, sizeof payload));
    assert(sx1250_dual_next(1, &packet) == 1 && packet.coding_rate == updated.coding_rate);

    /* Roll back both the frequency and PHY if rearming the new profile fails. */
    fake_fail_rf1_rearm(1);
    assert(sx1250_dual_reconfigure(1, &profiles[1]) < 0 && errno == EIO);
    expect_frequency(1, updated.frequency_hz);
    assert(fake_image_band(1) == 902000000 && fake_image_calibrations(1) == rf1_calibrations + 6);
    assert(fake_inject_rx(payload, sizeof payload));
    assert(sx1250_dual_next(1, &packet) == 1 && packet.coding_rate == updated.coding_rate);
    assert(packet.size == sizeof payload && !memcmp(packet.payload, payload, sizeof payload));

    fake_fail_rf1_rearm(3);
    assert(sx1250_dual_reconfigure(1, &profiles[1]) < 0 && errno == EIO);
    assert(sx1250_dual_next(1, &packet) < 0 && errno == EIO);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("FAKE_DUAL_RX", "1", 1));
    assert(!setenv("FAKE_TX", "1", 1)); /* enables test injection into RF1 */
    assert(!setenv("IDLE", "1", 1));
    struct sx1250_rx_config profiles[2] = {
        {869618000, 62500, 16, 8, 8, 0x12, true, false, 0},
        {869475000, 250000, 16, 8, 5, 0x12, true, false, 0},
    };
    struct sx1250_rx_packet packet;
    assert(sx1250_dual_reconfigure(0, profiles) < 0 && errno == ENODEV);
    assert(!sx1250_dual_open(argv[1], profiles));
    assert(sx1250_rx_open(argv[1], profiles) < 0 && errno == EBUSY);
    assert(fake_image_calibrations(0) == 1 && fake_image_calibrations(1) == 1);
    expect_frequency(0, profiles[0].frequency_hz);
    expect_frequency(1, profiles[1].frequency_hz);

    /* RF1 finishes as it enters standby. RF0 must resume and the RF1 FIFO
     * must remain readable without completing a pause. */
    assert(!setenv("FAKE_RX_DONE_ON_STANDBY", "1", 1));
    assert(sx1250_dual_pause() < 0 && errno == EAGAIN);
    assert(!unsetenv("FAKE_RX_DONE_ON_STANDBY"));
    assert(sx1250_dual_activity(1) == 1);
    assert(sx1250_dual_next(1, &packet) == 1 && packet.crc_present);

    const uint8_t first[] = {0x10, 0x20, 0x30}, second[] = {0x40, 0x50, 0x60, 0x70};
    assert(fake_inject_rf0(first, sizeof first));
    assert(fake_inject_rx(second, sizeof second));
    assert(sx1250_dual_activity(0) == 1 && sx1250_dual_activity(1) == 1);
    assert(sx1250_dual_pause() < 0 && errno == EAGAIN);
    assert(sx1250_dual_next(0, &packet) == 1 && packet.coding_rate == 8 && packet.crc_present);
    assert(packet.size == sizeof first && !memcmp(first, packet.payload, sizeof first));
    assert(sx1250_dual_next(1, &packet) == 1 && packet.coding_rate == 5 && packet.crc_present);
    assert(packet.size == sizeof second && !memcmp(second, packet.payload, sizeof second));
    assert(!sx1250_dual_next(0, &packet) && !sx1250_dual_next(1, &packet));

    assert(!sx1250_dual_pause());
    assert(sx1250_dual_next(0, &packet) < 0 && errno == EBUSY);
    assert(!sx126x_set_rf_freq_in_pll_steps(&sx1250_usp_radios[0], SX1250_FREQ_TO_REG(869500000)));
    assert(!lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN, 0));
    assert(!lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN, 0));
    bool recovered;
    assert(!sx1250_dual_resume_report(false, &recovered) && !recovered);
    expect_frequency(0, profiles[0].frequency_hz);
    expect_frequency(1, profiles[1].frequency_hz);

    assert(!sx1250_dual_pause());
    assert(!lgw_reg_w(SX1302_REG_COMMON_CTRL0_CLK32_RIF_CTRL, 1));
    assert(!sx1250_dual_resume_report(false, &recovered) && recovered);
    assert(fake_image_calibrations(0) == 2 && fake_image_calibrations(1) == 2);
    assert(fake_inject_rf0(first, sizeof first));
    assert(sx1250_dual_next(0, &packet) == 1 && packet.crc_present);
    test_reconfigure(profiles);
    assert(!sx1250_dual_close());
    assert(!sx1250_rx_open(argv[1], profiles));
    assert(!sx1250_rx_close());
    return 0;
}
