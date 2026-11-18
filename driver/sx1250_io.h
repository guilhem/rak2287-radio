/* Private SPI helpers shared by the receiver and hardware diagnostics. */
#ifndef SX1250_IO_H
#define SX1250_IO_H
#include <string.h>
#include <stdio.h>
#include "loragw_aux.h"
#include "loragw_sx1250.h"
enum { chain = 1 };
static inline int status_ok(uint8_t s) {
    unsigned mode = (s >> 4) & 7, code = (s >> 1) & 7;
    /* The SX1250 HAL masks bits 6:4. RF1 on the tested RAK2287 returns 0xa2 in standby RC.
     * The outer bits are reserved on SX126x and are not validity bits. */
    return mode >= 2 && mode <= 5 && !(code >= 3 && code <= 5);
}

static inline int bank_status(unsigned rf, sx1250_op_code_t after, unsigned expected_mode) {
    uint8_t s = 0;
    if (sx1250_reg_r(GET_STATUS, &s, 1, rf)) return -1;
    if (radio_verbose || !status_ok(s) || (expected_mode && ((s >> 4) & 7u) != expected_mode))
        fprintf(radio_verbose ? stdout : stderr, "After opcode 0x%02x: status=%02x mode=%u command=%u\n",
           after, s, (s >> 4) & 7, (s >> 1) & 7);
    return status_ok(s) && (!expected_mode || ((s >> 4) & 7u) == expected_mode) ? 0 : -1;
}

static inline int bank_write(unsigned rf, sx1250_op_code_t op, uint8_t *data, size_t n, unsigned mode) {
    if (sx1250_reg_w(op, data, n, rf)) return -1;
    if (op == SET_STANDBY || op == SET_RX || op == CALIBRATE) wait_ms(10);
    return bank_status(rf, op, mode);
}

/* Data begins after address (if any) and a NOP. HAL's SPI length is uint8_t. */
static inline int bank_read(unsigned rf, sx1250_op_code_t op, const uint8_t *addr, size_t a,
                    uint8_t *out, size_t n) {
    uint8_t b[253] = {0};
    if (a + 1 + n > sizeof b) return -1;
    if (a) memcpy(b, addr, a);
    if (sx1250_reg_r(op, b, a + 1 + n, rf)) return -1;
    if (!status_ok(b[a])) {
        fprintf(stderr, "Read opcode 0x%02x returned status=%02x\n", op, b[a]);
        return -1;
    }
    memcpy(out, b + a + 1, n);
    return 0;
}

static inline int radio_status(sx1250_op_code_t op, unsigned mode) {
    return bank_status(chain, op, mode);
}

static inline int write_cmd(sx1250_op_code_t op, uint8_t *data, size_t n, unsigned mode) {
    return bank_write(chain, op, data, n, mode);
}

static inline int read_cmd(sx1250_op_code_t op, const uint8_t *addr, size_t a,
                           uint8_t *out, size_t n) {
    return bank_read(chain, op, addr, a, out, n);
}

static inline int reg_read(uint16_t addr, uint8_t *out, size_t n) {
    return read_cmd(READ_REGISTER, (uint8_t[]){addr >> 8, addr}, 2, out, n);
}

#endif
