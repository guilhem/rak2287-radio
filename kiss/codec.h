#ifndef KISS_CODEC_H
#define KISS_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { KISS_FRAME_CAPACITY = 512 };
#define KISS_ENCODED_MAX(payload_size) (4u + 2u * (payload_size))

struct kiss_decoder {
    uint8_t frame[KISS_FRAME_CAPACITY];
    size_t length;
    bool in_frame, escaped;
};

/* Zero-initialize the decoder. A nonzero return gives the frame length
 * including the type byte. Read the frame before feeding another byte. */
size_t kiss_decode_byte(struct kiss_decoder *decoder, uint8_t byte);

/* Return zero and leave out untouched if the frame or its worst-case
 * encoding exceeds the supplied limits. */
size_t kiss_encode_frame(uint8_t *out, size_t capacity, uint8_t type,
                         const uint8_t *payload, size_t length);

#endif
