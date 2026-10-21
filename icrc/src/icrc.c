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
