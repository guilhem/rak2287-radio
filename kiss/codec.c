#include "codec.h"

enum { FEND = 0xc0, FESC = 0xdb, TFEND = 0xdc, TFESC = 0xdd };

size_t kiss_decode_byte(struct kiss_decoder *decoder, uint8_t byte) {
    if (byte == FEND) {
        const size_t length = decoder->in_frame && !decoder->escaped ? decoder->length : 0;
        decoder->in_frame = true;
        decoder->escaped = false;
        decoder->length = 0;
        return length;
    }
    if (!decoder->in_frame) return 0;
    if (decoder->escaped) {
        decoder->escaped = false;
        if (byte == TFEND) byte = FEND;
        else if (byte == TFESC) byte = FESC;
        else {
            decoder->in_frame = false;
            return 0;
        }
    } else if (byte == FESC) {
        decoder->escaped = true;
        return 0;
    }

    if (decoder->length == sizeof decoder->frame) decoder->in_frame = false;
    else decoder->frame[decoder->length++] = byte;
    return 0;
}

static uint8_t *write_escaped(uint8_t *out, uint8_t byte) {
    if (byte == FEND || byte == FESC) {
        *out++ = FESC;
        byte = byte == FEND ? TFEND : TFESC;
    }
    *out++ = byte;
    return out;
}

size_t kiss_encode_frame(uint8_t *out, size_t capacity, uint8_t type,
                         const uint8_t *payload, size_t length) {
    if (length >= KISS_FRAME_CAPACITY || capacity < KISS_ENCODED_MAX(length)) return 0;

    uint8_t *next = out;
    *next++ = FEND;
    next = write_escaped(next, type);
    for (size_t i = 0; i < length; ++i) next = write_escaped(next, payload[i]);
    *next++ = FEND;
    return (size_t)(next - out);
}
