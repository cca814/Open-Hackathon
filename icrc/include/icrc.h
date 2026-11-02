#ifndef NODE_B_ICRC_H
#define NODE_B_ICRC_H
#include <stddef.h>
#include <stdint.h>

/* Backend input is already the canonical contiguous ICRC byte string:
 * eight all-ones virtual LRH bytes followed by normalized IP/UDP/RDMA bytes.
 * Implementations must return the numeric reflected Ethernet CRC-32. */
struct icrc_backend {
    const char *name;
    void *context;
    uint32_t (*crc32)(void *context, const uint8_t *data, size_t length);
};

/* Always available and independent of DPDK/EAL. */
const struct icrc_backend *icrc_backend_portable(void);

/* Validated IP + UDP + BTH bytes, excluding the ICRC trailer.
 * Returns numeric reflected CRC-32; encode trailer least-significant byte first.
 * Includes the virtual 8-byte all-ones LRH and RoCE invariant normalization. */
uint32_t roce_icrc_ipv4(const uint8_t *ip, size_t length);
uint32_t roce_icrc_ipv6(const uint8_t *ip, size_t length);

/* Fast mutable path. prefix points to eight writable bytes immediately before
 * ip. The function restores the prefix and every invariant field before
 * returning, so callers observe no packet mutation. */
uint32_t roce_icrc_ipv4_inplace(uint8_t *prefix, size_t length,
                                const struct icrc_backend *backend);
uint32_t roce_icrc_ipv6_inplace(uint8_t *prefix, size_t length,
                                const struct icrc_backend *backend);
#endif
