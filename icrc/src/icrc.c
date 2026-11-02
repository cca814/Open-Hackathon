#include "icrc.h"

static uint32_t update(uint32_t state, uint8_t byte)
{
    state ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        state = (state >> 1) ^ ((state & 1u) ? UINT32_C(0xedb88320) : 0);
    return state;
}

static uint32_t portable_crc32(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    uint32_t state = UINT32_MAX;
    for (size_t i = 0; i < length; ++i)
        state = update(state, data[i]);
    return ~state;
}

const struct icrc_backend *icrc_backend_portable(void)
{
    static const struct icrc_backend backend = {
        .name = "portable-bitwise",
        .context = NULL,
        .crc32 = portable_crc32,
    };
    return &backend;
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

static uint32_t inplace(uint8_t *prefix, size_t length,
                        const struct icrc_backend *backend, int ipv6)
{
    uint8_t saved_prefix[8];
    uint8_t *ip = prefix + 8;
    uint8_t saved[8];
    static const uint8_t ipv4_offsets[] = {1, 8, 10, 11, 26, 27, 32};
    static const uint8_t ipv6_offsets[] = {0, 1, 2, 3, 7, 46, 47, 52};
    const uint8_t *offsets = ipv6 ? ipv6_offsets : ipv4_offsets;
    size_t count = ipv6 ? sizeof(ipv6_offsets) : sizeof(ipv4_offsets);

    if (!backend || !backend->crc32)
        backend = icrc_backend_portable();
    /* The parser validates a complete fixed IP/UDP/BTH header before this
     * path is reached. Keep a defensive fallback for direct API callers. */
    if ((!ipv6 && length <= ipv4_offsets[sizeof(ipv4_offsets) - 1]) ||
        (ipv6 && length <= ipv6_offsets[sizeof(ipv6_offsets) - 1]))
        return ipv6 ? roce_icrc_ipv6(ip, length) : roce_icrc_ipv4(ip, length);

    for (size_t i = 0; i < count; ++i)
        saved[i] = ip[offsets[i]];
    for (size_t i = 0; i < sizeof(saved_prefix); ++i) {
        saved_prefix[i] = prefix[i];
        prefix[i] = 0xff;
    }
    if (ipv6) {
        ip[0] = (uint8_t)((ip[0] & 0xf0u) | 0x0fu);
        for (size_t i = 1; i < count; ++i)
            ip[offsets[i]] = 0xff;
    } else {
        for (size_t i = 0; i < count; ++i)
            ip[offsets[i]] = 0xff;
    }

    uint32_t result = backend->crc32(backend->context, prefix, length + 8);

    for (size_t i = 0; i < count; ++i)
        ip[offsets[i]] = saved[i];
    for (size_t i = 0; i < sizeof(saved_prefix); ++i)
        prefix[i] = saved_prefix[i];
    return result;
}

uint32_t roce_icrc_ipv4_inplace(uint8_t *prefix, size_t length,
                                const struct icrc_backend *backend)
{
    return inplace(prefix, length, backend, 0);
}

uint32_t roce_icrc_ipv6_inplace(uint8_t *prefix, size_t length,
                                const struct icrc_backend *backend)
{
    return inplace(prefix, length, backend, 1);
}
