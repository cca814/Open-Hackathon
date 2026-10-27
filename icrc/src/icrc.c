#include "icrc.h"

static uint32_t update(uint32_t state, uint8_t byte)
{
    state ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        state = (state >> 1) ^ ((state & 1u) ? UINT32_C(0xedb88320) : 0);
    return state;
}

uint32_t roce_icrc_ipv4(const uint8_t *ip, size_t length)
{
    uint32_t state = UINT32_MAX;
    for (unsigned i = 0; i < 8; ++i)
        state = update(state, 0xff);
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = ip[i];
        /* IPv4 TOS, TTL, checksum; UDP checksum; BTH resv8a. */
        if (i == 1 || i == 8 || i == 10 || i == 11 ||
            i == 26 || i == 27 || i == 32)
            byte = 0xff;
        state = update(state, byte);
    }
    return ~state;
}

uint32_t roce_icrc_ipv6(const uint8_t *ip, size_t length)
{
    uint32_t state = UINT32_MAX;
    for (unsigned i = 0; i < 8; ++i)
        state = update(state, 0xff);
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = ip[i];
        /* IPv6 traffic class/flow label and hop limit; UDP checksum;
         * BTH resv8a. Preserve the version nibble while masking byte 0. */
        if (i == 0)
            byte = (uint8_t)((byte & 0xf0u) | 0x0fu);
        else if ((i >= 1 && i <= 3) || i == 7 || i == 46 || i == 47 || i == 52)
            byte = 0xff;
        state = update(state, byte);
    }
    return ~state;
}
