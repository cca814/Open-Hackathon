#ifndef NODE_B_ICRC_H
#define NODE_B_ICRC_H
#include <stddef.h>
#include <stdint.h>
/* Validated IPv4/IHL=5 + UDP + BTH bytes, excluding the ICRC trailer.
 * Returns numeric reflected CRC-32; encode trailer least-significant byte first.
 * Includes the virtual 8-byte all-ones LRH and RoCE invariant normalization. */
uint32_t roce_icrc_ipv4(const uint8_t *ip, size_t length);
#endif
