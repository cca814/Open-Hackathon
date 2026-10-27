#ifndef NODE_B_ROCE_H
#define NODE_B_ROCE_H
#include <stddef.h>
#include <stdint.h>

enum roce_result { ROCE_OK, ROCE_MALFORMED, ROCE_UNSUPPORTED, ROCE_BAD_ICRC };
enum roce_ip_family { ROCE_IPV4 = 4, ROCE_IPV6 = 6 };
struct roce_address {
    uint8_t family;
    uint8_t bytes[16]; /* Network byte order; IPv4 uses bytes 0..3. */
};
struct roce_packet {
    size_t ip, udp, bth, icrc, end;
    struct roce_address destination;
    uint32_t dst_qpn;
};
struct roce_destination {
    uint8_t mac[6];
    struct roce_address address;
    uint32_t qpn;
};
/* Ethernet II, IPv4 without options/fragments or IPv6 without extension
 * headers/jumbograms, UDP/4791, RC SEND 0..5.
 * Length excludes Ethernet FCS; trailing Ethernet pad is not part of ICRC.
 * Output stays unchanged on failure. Structural validation, not RC state. */
int roce_parse(const uint8_t *frame, size_t length, struct roce_packet *packet);
int roce_check_icrc(const uint8_t *frame, size_t length);
/* Revalidates before mutation. The destination family must match the packet.
 * Updates destination MAC/IP/QPN, IPv4 checksum when present, full ICRC and
 * UDP checksum. An absent IPv4 checksum stays zero; IPv6 UDP is mandatory. */
int roce_rewrite(uint8_t *frame, size_t length, const struct roce_destination *dst);
#endif
