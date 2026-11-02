#include "icrc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct spy_context {
    unsigned calls;
};

static uint32_t spy_crc32(void *opaque, const uint8_t *data, size_t length)
{
    struct spy_context *spy = opaque;
    const struct icrc_backend *portable = icrc_backend_portable();
    ++spy->calls;
    assert(length >= 8);
    for (unsigned i = 0; i < 8; ++i)
        assert(data[i] == 0xff);
    return portable->crc32(portable->context, data, length);
}

static void fill(uint8_t *data, size_t length, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < length; ++i) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        data[i] = (uint8_t)(state >> 24);
    }
}

static void check_family(int ipv6)
{
    uint8_t bytes[8 + 1600], original[sizeof(bytes)];
    struct spy_context spy = {0};
    const struct icrc_backend backend = {
        .name = "test-spy",
        .context = &spy,
        .crc32 = spy_crc32,
    };
    const size_t minimum = ipv6 ? 53 : 33;
    for (size_t length = minimum; length <= 1500; length += 37) {
        fill(bytes, 8 + length, (uint32_t)(length + (ipv6 ? 6000 : 4000)));
        if (ipv6)
            bytes[8] = (uint8_t)((bytes[8] & 0x0f) | 0x60);
        memcpy(original, bytes, 8 + length);
        uint32_t expected = ipv6 ?
            roce_icrc_ipv6(bytes + 8, length) :
            roce_icrc_ipv4(bytes + 8, length);
        uint32_t actual = ipv6 ?
            roce_icrc_ipv6_inplace(bytes, length, &backend) :
            roce_icrc_ipv4_inplace(bytes, length, &backend);
        assert(actual == expected);
        assert(memcmp(bytes, original, 8 + length) == 0);
    }
    assert(spy.calls != 0);
}

int main(void)
{
    check_family(0);
    check_family(1);

    uint8_t short_input[24], original[sizeof(short_input)];
    fill(short_input, sizeof(short_input), 7);
    memcpy(original, short_input, sizeof(short_input));
    assert(roce_icrc_ipv4_inplace(short_input, 10, NULL) ==
           roce_icrc_ipv4(short_input + 8, 10));
    assert(memcmp(short_input, original, sizeof(short_input)) == 0);
    return 0;
}
