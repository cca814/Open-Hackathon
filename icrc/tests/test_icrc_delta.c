#include "icrc.h"
#include "icrc_delta.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void fill(uint8_t *data, size_t length, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < length; ++i) {
        state = state * UINT32_C(1103515245) + UINT32_C(12345);
        data[i] = (uint8_t)(state >> 16);
    }
}

static void check(int ipv6, const struct icrc_delta *delta)
{
    uint8_t before[1600], after[sizeof(before)];
    const size_t ipv4_offsets[] = {12, 13, 14, 15, 16, 17, 18, 19,
                                   33, 34, 35, 37, 38, 39};
    const size_t ipv6_offsets[] = {8, 15, 23, 24, 31, 39,
                                   53, 54, 55, 57, 58, 59};
    const size_t *offsets = ipv6 ? ipv6_offsets : ipv4_offsets;
    size_t count = ipv6 ? sizeof(ipv6_offsets) / sizeof(ipv6_offsets[0]) :
        sizeof(ipv4_offsets) / sizeof(ipv4_offsets[0]);

    for (size_t length = 64; length <= sizeof(before); length += 41) {
        fill(before, length, (uint32_t)(length + (ipv6 ? 6 : 4)));
        if (ipv6)
            before[0] = (uint8_t)((before[0] & 0x0f) | 0x60);
        memcpy(after, before, length);
        struct icrc_change changes[16];
        for (size_t i = 0; i < count; ++i) {
            size_t offset = offsets[i];
            changes[i] = (struct icrc_change){
                .offset = offset,
                .before = after[offset],
                .after = (uint8_t)(after[offset] ^ (uint8_t)(0x31 + i)),
            };
            after[offset] = changes[i].after;
        }
        uint32_t original = ipv6 ? roce_icrc_ipv6(before, length) :
            roce_icrc_ipv4(before, length);
        uint32_t expected = ipv6 ? roce_icrc_ipv6(after, length) :
            roce_icrc_ipv4(after, length);
        uint32_t corrected = 0;
        assert(icrc_delta_correct(delta, original, length, changes, count,
                                  &corrected) == 0);
        assert(corrected == expected);
    }
}

int main(void)
{
    struct icrc_delta *delta = NULL;
    assert(icrc_delta_create(1600, &delta) == 0);
    check(0, delta);
    check(1, delta);

    struct icrc_change invalid = {.offset = 64, .before = 0, .after = 1};
    uint32_t corrected;
    assert(icrc_delta_correct(delta, 0, 64, &invalid, 1, &corrected) != 0);
    icrc_delta_destroy(&delta);
    assert(delta == NULL);
    return 0;
}
