#define _POSIX_C_SOURCE 200809L
/* The real RF0 TX path with independently injectable pause/resume outcomes. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "fake_hal.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1250.h"
#include "loragw_sx1302.h"
#include "sx1250_dual.h"
#include "sx1250_rx.h"
#include "sx1250_tx.h"

static unsigned pauses, resumes;
static bool busy, fail_resume, recover_cold, integrated;

int __real_sx1250_dual_pause(void);
int __real_sx1250_dual_resume_report(bool force_cold, bool *cold_recovery);

int __wrap_sx1250_dual_pause(void) {
    if (integrated) return __real_sx1250_dual_pause();
    ++pauses;
    if (busy) { errno = EAGAIN; return -1; }

    /* Replace the fixture's initial RF0 I/Q mode with packet standby. */
    assert(!sx1302_radio_reset(0, LGW_RADIO_TYPE_SX1250));
    assert(!sx1250_reg_w(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 0));
    assert(!sx1250_reg_w(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 1));
    assert(!sx1250_reg_w(WRITE_REGISTER, (uint8_t[]){0x08, 0xd8, 0x40}, 3, 0));
    return 0;
}

int __wrap_sx1250_dual_resume_report(bool force_cold, bool *cold_recovery) {
    if (integrated) return __real_sx1250_dual_resume_report(force_cold, cold_recovery);
    ++resumes;
    int32_t enabled;
    uint8_t status = 0, clamp[] = {0x08, 0xd8, 0, 0};
    /* Restoring RX starts only after abort/standby and trim restoration. */
    assert(!force_cold);
    assert(!sx1250_reg_r(GET_STATUS, &status, 1, 0) && ((status >> 4) & 7) == 3);
    assert(!lgw_reg_r(SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN, &enabled) && !enabled);
    assert(!lgw_reg_r(SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN, &enabled) && !enabled);
    assert(!sx1250_reg_r(READ_REGISTER, clamp, sizeof clamp, 0) && clamp[3] == 0x40);
    if (cold_recovery) *cold_recovery = recover_cold;
    if (fail_resume) { errno = EIO; return -1; }
    return 0;
}

static void test_both_receivers(const char *device, const struct sx1250_rx_config *narrow,
                                bool cross_band, bool restore_failure) {
    integrated = true;
    assert(!setenv("FAKE_DUAL_RX", "1", 1));
    assert(!setenv("FAKE_DUAL_TX", "1", 1));
    assert(!setenv("IDLE", "1", 1));
    assert(!setenv("FAKE_TX_FAULT", restore_failure ? "dual_tx_trim_restore" : "", 1));
    const struct sx1250_rx_config profiles[] = {
        *narrow, {cross_band ? 915525000 : 869525000, 250000, 16, 9, 5, 0x2b, true, false, 0}
    };
    const uint8_t to_air[] = {0xaa, 0xbb, 0xcc};
    const uint8_t from_air_0[] = {0x10, 0x20, 0x30, 0x40};
    const uint8_t from_air_1[] = {0x70, 0x80, 0x90};
    struct sx1250_handoff_report report;
    struct sx1250_rx_packet packet;
    uint8_t sent[255], trim[] = {0x08, 0xe7, 0, 0};
    unsigned rf0_calibrations = 1;
    unsigned rf1_calibrations = 1;
    const uint32_t rf1_band = cross_band ? 902000000 : 863000000;

    assert(!sx1250_dual_open(device, profiles));
    assert(fake_image_band(0) == 863000000 && fake_image_band(1) == rf1_band);
    assert(fake_image_calibrations(0) == 1 && fake_image_calibrations(1) == 1);
    if (cross_band && !restore_failure) {
        fake_fail_image_calibration(0);
        assert(sx1250_dual_send(&profiles[1], to_air, sizeof to_air, 100, &report) < 0 && errno == EIO);
        assert(!report.tx_started && report.cold_recovery && report.rf0_rearmed && report.rf1_rearmed);
        assert(!strcmp(report.failed_stage, "rf0-image-calibration"));
        rf0_calibrations += 3;
        ++rf1_calibrations;
        assert(fake_image_band(0) == 863000000 && fake_image_calibrations(0) == rf0_calibrations);
    }
    int result;
    if (cross_band) {
        fake_expect_tx_power(0x16);
        result = sx1250_dual_send_with_power(&profiles[1], SX1250_RF0_TX_POWER_MAX_DBM,
                                           to_air, sizeof to_air, 100, &report);
        rf0_calibrations += 2;
    } else {
        result = sx1250_dual_send(&profiles[1], to_air, sizeof to_air, 100, &report);
        rf0_calibrations += restore_failure;
    }
    rf1_calibrations += restore_failure;
    assert(restore_failure ? result == -1 && errno == EIO : !result);
    assert(report.tx_done && report.rf0_rearmed && report.rf1_rearmed);
    assert(report.cold_recovery == restore_failure);
    assert(fake_last_tx_image_band() == rf1_band);
    assert(fake_image_band(0) == 863000000 && fake_image_band(1) == rf1_band);
    assert(fake_image_calibrations(0) == rf0_calibrations && fake_image_calibrations(1) == rf1_calibrations);
    for (unsigned rf = 0; rf < 2; ++rf) {
        const uint32_t pll = SX1250_FREQ_TO_REG(profiles[rf].frequency_hz);
        const uint8_t expected[] = {pll >> 24, pll >> 16, pll >> 8, pll};
        uint8_t readback[] = {0x08, 0x8b, 0, 0, 0, 0, 0};
        assert(!sx1250_reg_r(READ_REGISTER, readback, sizeof readback, rf));
        assert(!memcmp(readback + 3, expected, sizeof expected));
    }
    assert(!sx1250_reg_r(READ_REGISTER, trim, sizeof trim, 0) && trim[3] == 0x5a);
    assert(fake_last_tx(sent, sizeof sent) == sizeof to_air && !memcmp(sent, to_air, sizeof to_air));
    assert(fake_inject_rf0(from_air_0, sizeof from_air_0) && fake_inject_rx(from_air_1, sizeof from_air_1));
    assert(sx1250_dual_next(0, &packet) == 1 && packet.size == sizeof from_air_0 &&
           packet.coding_rate == 8 && !memcmp(packet.payload, from_air_0, sizeof from_air_0));
    assert(sx1250_dual_next(1, &packet) == 1 && packet.size == sizeof from_air_1 &&
           packet.coding_rate == 5 && !memcmp(packet.payload, from_air_1, sizeof from_air_1));
    assert(!sx1250_dual_close());
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("FAKE_TX", "1", 1));
    assert(!setenv("FAKE_SINGLE_TX", "1", 1));
    const struct sx1250_rx_config profile = {869618000, 62500, 16, 8, 8, 0x12, true, false, 0};
    const uint8_t payload[] = {0x07, 0x31, 0x9b};
    struct sx1250_handoff_report report;
    uint8_t sent[255];

    assert(!sx1250_rx_open(argv[1], &profile));
    assert(sx1250_tx_power_supported(-9));
    assert(sx1250_tx_power_supported(22));
    assert(!sx1250_tx_power_supported(-10));
    assert(!sx1250_tx_power_supported(23));
    assert(sx1250_dual_send_with_power(&profile, -10, payload, sizeof payload, 100, &report) == -1 &&
           errno == EINVAL);
    assert(sx1250_dual_send_with_power(&profile, 23, payload, sizeof payload, 100, &report) == -1 &&
           errno == EINVAL);
    assert(!pauses && !resumes && !report.tx_started && !strcmp(report.failed_stage, "validate"));
    assert(sx1250_dual_send(&profile, payload, 0, 100, &report) == -1 && errno == EINVAL);
    assert(!pauses && !resumes && !report.tx_started);
    busy = true;
    assert(sx1250_dual_send(&profile, payload, sizeof payload, 100, &report) == -1 && errno == EAGAIN);
    assert(pauses == 1 && !resumes && !report.tx_started);
    busy = false;

    const char *faults[] = {"", "readback", "write", "timeout", "", ""};
    const int errors[] = {0, EIO, EIO, ETIMEDOUT, EIO, 0};
    for (unsigned i = 0; i < sizeof errors / sizeof *errors; ++i) {
        fail_resume = i == 4;
        recover_cold = i >= 4;
        assert(!setenv("FAKE_TX_FAULT", faults[i], 1));
        int result = sx1250_dual_send(&profile, payload, sizeof payload, 100, &report);
        assert(result == (errors[i] ? -1 : 0) && (!result || errno == errors[i]));
        assert(pauses == i + 2 && resumes == i + 1);
        assert(report.tx_started == (i != 1));
        assert(report.tx_done == (i == 0 || recover_cold));
        assert(report.rf0_rearmed == !fail_resume && report.rf1_rearmed == !fail_resume);
        assert(report.cold_recovery == recover_cold);
        assert((report.downtime_ns > 0) == !fail_resume);
        assert((report.failed_stage == NULL) == (!errors[i] && !recover_cold));
        size_t size = fake_last_tx(sent, sizeof sent);
        assert(size == (report.tx_done ? sizeof payload : 0));
        if (size) assert(!memcmp(sent, payload, size));
        assert(!sx1250_rx_close());
        if (i + 1 < sizeof errors / sizeof *errors) assert(!sx1250_rx_open(argv[1], &profile));
    }
    test_both_receivers(argv[1], &profile, false, false);
    test_both_receivers(argv[1], &profile, false, true);
    test_both_receivers(argv[1], &profile, true, false);
    test_both_receivers(argv[1], &profile, true, true);
    return 0;
}
