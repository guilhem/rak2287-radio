#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/file.h>
#include <unistd.h>

#include "sx1250_board.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1302.h"

static const bool radio_verbose = false;
#include "sx1250_io.h"

static atomic_flag owner = ATOMIC_FLAG_INIT;
static int lock_fd = -1;
static bool connected, configured;
static bool rf0_reset_valid;
static uint8_t rf0_reset_values[SX1250_RF0_RESET_SPAN_COUNT][4];

/* The pinned HAL changes these registers when entering its I/Q bridge mode. */
const struct sx1250_reg_span sx1250_rf0_reset_spans[SX1250_RF0_RESET_SPAN_COUNT] = {
    {0x0587, 1}, {0x0580, 1}, {0x0582, 4}, {0x06a1, 3}, {0x08b6, 1}, {0x088f, 3}
};

static void capture_rf0_reset(void) {
    rf0_reset_valid = false;
    if (bank_status(0, GET_STATUS, 2)) return;
    for (size_t i = 0; i < SX1250_RF0_RESET_SPAN_COUNT; ++i) {
        struct sx1250_reg_span span = sx1250_rf0_reset_spans[i];
        if (bank_read(0, READ_REGISTER, (uint8_t[]){span.address >> 8, span.address}, 2,
                      rf0_reset_values[i], span.size)) return;
    }
    rf0_reset_valid = rf0_reset_values[0][0] != 0x0b;
}

const uint8_t (*sx1250_board_rf0_reset(void))[4] {
    return rf0_reset_valid ? rf0_reset_values : NULL;
}

int sx1250_board_release(void) {
    if (lock_fd < 0) { errno = EINVAL; return -1; }
    int error = 0;
    if (configured) {
        error |= lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_A_PA_EN, 0);
        error |= lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_B_PA_EN, 0);
        error |= sx1302_radio_reset(chain, LGW_RADIO_TYPE_SX1250);
        error |= sx1302_radio_reset(0, LGW_RADIO_TYPE_SX1250);
        error |= lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN, 0);
        error |= lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN, 0);
        error |= lgw_reg_w(SX1302_REG_AGC_MCU_CTRL_FORCE_HOST_FE_CTRL, 0);
        error |= sx1302_radio_host_ctrl(false);
    }
    if (connected) error |= lgw_disconnect();
    error |= close(lock_fd);
    lock_fd = -1;
    connected = configured = rf0_reset_valid = false;
    atomic_flag_clear(&owner);
    if (error) { errno = EIO; return -1; }
    return 0;
}

int sx1250_board_acquire(const char *path, uint32_t hz) {
    /* The stock image calibrator uses strict inequalities for the EU band. */
    if (!path || !*path || hz <= 863000000 || hz >= 870000000) { errno = EINVAL; return -1; }
    if (atomic_flag_test_and_set(&owner)) { errno = EBUSY; return -1; }
    lock_fd = open(path, O_RDWR | O_CLOEXEC);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        int err = errno;
        if (lock_fd >= 0) close(lock_fd);
        lock_fd = -1;
        atomic_flag_clear(&owner);
        errno = err == EWOULDBLOCK ? EBUSY : err;
        return -1;
    }
    int err = EIO;
    if (lgw_com_target()) { err = EBUSY; goto fail; }
    if (lgw_connect(LGW_COM_SPI, path)) {
        connected = lgw_com_target() != NULL;
        goto fail;
    }
    connected = true;
    int32_t version, agc_reset, arb_reset;
    if (lgw_reg_r(SX1302_REG_COMMON_VERSION_VERSION, &version) ||
        lgw_reg_r(SX1302_REG_AGC_MCU_CTRL_MCU_CLEAR, &agc_reset) ||
        lgw_reg_r(SX1302_REG_ARB_MCU_CTRL_MCU_CLEAR, &arb_reset)) goto fail;
    if (version != 0x10) { err = ENODEV; goto fail; }
    if (!agc_reset || !arb_reset) { err = EBUSY; goto fail; }
    configured = true;
    struct lgw_conf_board_s board = {.clksrc = 0, .full_duplex = false};
    uint32_t frf = SX1250_FREQ_TO_REG(hz);
    uint8_t freq[] = {frf >> 24, frf >> 16, frf >> 8, frf}, check[4];
    if (sx1302_radio_host_ctrl(true) || sx1302_radio_reset(0, LGW_RADIO_TYPE_SX1250) ||
        sx1302_radio_set_mode(0, LGW_RADIO_TYPE_SX1250)) goto fail;
    capture_rf0_reset();
    if (sx1250_setup(0, hz, false) ||
        sx1302_radio_reset(chain, LGW_RADIO_TYPE_SX1250) ||
        sx1302_radio_set_mode(chain, LGW_RADIO_TYPE_SX1250) || sx1302_radio_clock_select(0) ||
        sx1302_pa_lna_lut_configure(&board) ||
        lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_A_PA_EN, 0) ||
        lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_B_PA_EN, 0) ||
        lgw_reg_w(SX1302_REG_AGC_MCU_CTRL_FORCE_HOST_FE_CTRL, 1) ||
        lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_A_LNA_EN, 1) ||
        lgw_reg_w(SX1302_REG_AGC_MCU_RF_EN_B_LNA_EN, 1) ||
        write_cmd(SET_STANDBY, (uint8_t[]){STDBY_RC}, 1, 2) ||
        write_cmd(CALIBRATE, (uint8_t[]){0x7f}, 1, 2) || sx1250_calibrate(chain, hz) ||
        write_cmd(SET_STANDBY, (uint8_t[]){STDBY_XOSC}, 1, 3) ||
        write_cmd(SET_RF_FREQUENCY, freq, sizeof freq, 3) ||
        reg_read(0x088b, check, sizeof check) || memcmp(check, freq, sizeof freq)) goto fail;
    return 0;
fail:
    sx1250_board_release();
    errno = err;
    return -1;
}
