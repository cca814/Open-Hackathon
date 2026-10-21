#ifndef NODE_B_ROCE_H
#define NODE_B_ROCE_H
#include <stddef.h>
#include <stdint.h>

enum roce_result { ROCE_OK, ROCE_MALFORMED, ROCE_UNSUPPORTED, ROCE_BAD_ICRC };
struct roce_packet {
    size_t ip, udp, bth, icrc, end;
    uint32_t dst_ipv4, dst_qpn;
};
struct roce_destination {
    uint8_t mac[6];
    uint32_t ipv4; /* Numeric host value: 192.0.2.1 = 0xc0000201. */
    uint32_t qpn;
};
/* Ethernet II, IPv4 without options/fragments, UDP/4791, RC SEND 0..5.
 * Length excludes Ethernet FCS; trailing Ethernet pad is not part of ICRC.
 * Output stays unchanged on failure. Structural validation, not RC state. */
int roce_parse(const uint8_t *frame, size_t length, struct roce_packet *packet);
int roce_check_icrc(const uint8_t *frame, size_t length);
/* Revalidates before mutation. Updates destination MAC/IP/QPN, IPv4 checksum,
 * full ICRC and nonzero UDP checksum. An absent IPv4 UDP checksum stays zero. */
int roce_rewrite(uint8_t *frame, size_t length, const struct roce_destination *dst);
#endif
