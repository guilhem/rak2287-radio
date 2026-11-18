#include <stdbool.h>
#include "sx1250_usp.h"
#include "sx126x_hal.h"
static const bool radio_verbose = false;
#include "sx1250_io.h"

const uint8_t sx1250_usp_rf1 = 1;

static bool rx_command(const uint8_t *command, uint16_t command_length, uint16_t data_length) {
    switch (command[0]) {
    case SET_STANDBY: return command_length == 2 && !data_length && command[1] == STDBY_XOSC;
    case SET_PACKET_TYPE: return command_length == 2 && !data_length && command[1] == PACKET_TYPE_LORA;
    case SET_MODULATION_PARAMS: return command_length == 5 && !data_length;
    case SET_PACKET_PARAMS: return command_length == 7 && !data_length;
    case SET_RF_FREQUENCY: return command_length == 5 && !data_length;
    case SET_DIO_IRQ_PARAMS:
        return command_length == 9 && !data_length && !memcmp(command + 3, (uint8_t[6]){0}, 6);
    case SET_BUFFER_BASE_ADDRESS: return command_length == 3 && !data_length;
    case CLR_IRQ_STATUS: return command_length == 3 && !data_length;
    case SET_RX: return command_length == 4 && !data_length && !memcmp(command + 1, (uint8_t[3]){0}, 3);
    case WRITE_REGISTER:
        if (command_length != 3) return false;
        switch ((command[1] << 8) | command[2]) {
        case 0x0736: /* IQ polarity */
        case 0x0889: /* SX126x modulation quality workaround */
            return data_length == 1;
        case 0x0740: /* LoRa sync word */
            return data_length == 2;
        default:
            return false;
        }
    default:
        return false;
    }
}

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command,
                                    uint16_t command_length, const uint8_t *data, uint16_t data_length) {
    if (context != &sx1250_usp_rf1 || !command || !command_length ||
        (data_length && !data) || !rx_command(command, command_length, data_length))
        return SX126X_HAL_STATUS_ERROR;
    uint8_t frame[8];
    size_t parameters = command_length - 1;
    memcpy(frame, command + 1, parameters);
    if (data_length) memcpy(frame + parameters, data, data_length);
    sx1250_op_code_t opcode = (sx1250_op_code_t)command[0];
    /* Status immediately after SetRx may already report completed RX. The
     * caller checks that transition along with the latched IRQ. */
    if (opcode == SET_RX)
        return sx1250_reg_w(opcode, frame, parameters, chain) ? SX126X_HAL_STATUS_ERROR : SX126X_HAL_STATUS_OK;
    unsigned mode = opcode == CLR_IRQ_STATUS ? 0 : 3;
    return write_cmd(opcode, frame, parameters + data_length, mode) ?
           SX126X_HAL_STATUS_ERROR : SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                   uint16_t command_length, uint8_t *data, uint16_t data_length) {
    if (context != &sx1250_usp_rf1 || !command || command_length < 2 ||
        command[command_length - 1] != SX126X_NOP || !data || !data_length)
        return SX126X_HAL_STATUS_ERROR;
    switch (command[0]) {
    case 0x11: /* GetPacketType */
        if (command_length != 2 || data_length != 1) return SX126X_HAL_STATUS_ERROR;
        break;
    case GET_IRQ_STATUS:
    case GET_RX_BUFFER_STATUS:
        if (command_length != 2 || data_length != 2) return SX126X_HAL_STATUS_ERROR;
        break;
    case READ_REGISTER:
        if (command_length != 4) return SX126X_HAL_STATUS_ERROR;
        break;
    case READ_BUFFER:
        if (command_length != 3) return SX126X_HAL_STATUS_ERROR;
        break;
    default:
        return SX126X_HAL_STATUS_ERROR;
    }
    /* USP appends its own NOP. bank_read inserts the same byte when it packs
     * the concentrator transaction, then checks its returned status. */
    return read_cmd((sx1250_op_code_t)command[0], command + 1, command_length - 2, data, data_length) ?
           SX126X_HAL_STATUS_ERROR : SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context) {
    (void)context;
    return SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context) {
    (void)context;
    return SX126X_HAL_STATUS_ERROR;
}
