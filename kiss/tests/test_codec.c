#include <assert.h>
#include <string.h>

#include "codec.h"

static void test_escape_round_trip(void) {
    const uint8_t payload[] = {0xdb, 0x53, 0xc0};
    const uint8_t expected[] = {0xc0, 0xdb, 0xdc, 0xdb, 0xdd, 0x53, 0xdb, 0xdc, 0xc0};
    uint8_t wire[KISS_ENCODED_MAX(sizeof payload)];
    memset(wire, 0xff, sizeof wire);
    assert(kiss_encode_frame(wire, sizeof wire - 1, 0xc0, payload, sizeof payload) == 0);
    for (size_t i = 0; i < sizeof wire; ++i) assert(wire[i] == 0xff);

    const size_t length = kiss_encode_frame(wire, sizeof wire, 0xc0, payload, sizeof payload);
    assert(length == sizeof expected);
    assert(memcmp(wire, expected, length) == 0);

    struct kiss_decoder decoder = {0};
    for (size_t i = 0; i < length - 1; ++i) assert(kiss_decode_byte(&decoder, wire[i]) == 0);
    assert(kiss_decode_byte(&decoder, wire[length - 1]) == sizeof payload + 1);
    assert(decoder.frame[0] == 0xc0);
    assert(memcmp(decoder.frame + 1, payload, sizeof payload) == 0);
}

static void test_frame_limit(void) {
    uint8_t payload[KISS_FRAME_CAPACITY];
    memset(payload, 0xdb, sizeof payload);
    uint8_t wire[KISS_ENCODED_MAX(sizeof payload - 1)];
    assert(kiss_encode_frame(wire, sizeof wire, 0xdb, payload, sizeof payload) == 0);
    assert(kiss_encode_frame(wire, sizeof wire, 0xdb, payload, sizeof payload - 1) == sizeof wire);

    struct kiss_decoder decoder = {0};
    for (size_t i = 0; i < sizeof wire - 1; ++i) assert(kiss_decode_byte(&decoder, wire[i]) == 0);
    assert(kiss_decode_byte(&decoder, wire[sizeof wire - 1]) == sizeof decoder.frame);
    assert(memcmp(decoder.frame, payload, sizeof payload) == 0);

    for (size_t i = 0; i <= sizeof decoder.frame; ++i) assert(kiss_decode_byte(&decoder, 0x31) == 0);
    assert(kiss_decode_byte(&decoder, 0xc0) == 0);
    assert(kiss_decode_byte(&decoder, 0x06) == 0);
    assert(kiss_decode_byte(&decoder, 0xc0) == 1);
    assert(decoder.frame[0] == 0x06);
}

static void test_discard_damaged_frames(void) {
    const uint8_t damaged[] = {
        0x91, 0xdb, 0xdc, 0xc0, 0xc0,
        0x06, 0xdb, 0x00, 0x17, 0xc0,
        0x06, 0xdb, 0xc0,
    };
    struct kiss_decoder decoder = {0};
    for (size_t i = 0; i < sizeof damaged; ++i) assert(kiss_decode_byte(&decoder, damaged[i]) == 0);

    assert(kiss_decode_byte(&decoder, 0x06) == 0);
    assert(kiss_decode_byte(&decoder, 0x17) == 0);
    assert(kiss_decode_byte(&decoder, 0xc0) == 2);
    assert(decoder.frame[0] == 0x06 && decoder.frame[1] == 0x17);
}

int main(void) {
    test_escape_round_trip();
    test_frame_limit();
    test_discard_damaged_frames();
}
