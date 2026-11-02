#include "roce.h"
#include "icrc.h"
#include "icrc_delta.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value; }

int roce_parse(const uint8_t *frame, size_t length, struct roce_packet *out)
{
    if (!frame || !out || length < 14)
        return ROCE_MALFORMED;
    size_t ip = 14;
    size_t udp, ip_length, source_offset, destination_offset, address_length;
    uint8_t family;
    if (be16(frame + 12) == 0x0800) {
        if (length - ip < 20)
            return ROCE_MALFORMED;
        if (frame[ip] != 0x45 || frame[ip + 9] != 17 ||
            (be16(frame + ip + 6) & 0xbfff) != 0)
            return ROCE_UNSUPPORTED;
        ip_length = be16(frame + ip + 2);
        if (ip_length < 44 || ip_length > length - ip)
            return ROCE_MALFORMED;
        udp = ip + 20;
        source_offset = ip + 12;
        destination_offset = ip + 16;
        address_length = 4;
        family = ROCE_IPV4;
    } else if (be16(frame + 12) == 0x86dd) {
        if (length - ip < 40)
            return ROCE_MALFORMED;
        if ((frame[ip] >> 4) != 6 || frame[ip + 6] != 17)
            return ROCE_UNSUPPORTED;
        size_t payload_length = be16(frame + ip + 4);
        if (payload_length < 24 || payload_length > length - ip - 40)
            return ROCE_MALFORMED;
        ip_length = 40 + payload_length;
        udp = ip + 40;
        source_offset = ip + 8;
        destination_offset = ip + 24;
        address_length = 16;
        family = ROCE_IPV6;
    } else {
        return ROCE_UNSUPPORTED;
    }
    size_t udp_length = be16(frame + udp + 4);
    if (udp_length != ip_length - (udp - ip) || udp_length % 4 != 0)
        return ROCE_MALFORMED;
    if (be16(frame + udp + 2) != 4791)
        return ROCE_UNSUPPORTED;
    size_t bth = udp + 8, end = ip + ip_length, trailer = end - 4;
    uint8_t opcode = frame[bth];
    unsigned padding = (frame[bth + 1] >> 4) & 3u;
    if ((opcode > 5 && opcode != 0x11) || (frame[bth + 1] & 15u) != 0)
        return ROCE_UNSUPPORTED;
    size_t headers = 12 + ((opcode == 3 || opcode == 5 || opcode == 0x11) ? 4 : 0);
    if (trailer - bth < headers || padding > trailer - bth - headers ||
        ((opcode == 0 || opcode == 1 || opcode == 0x11) && padding != 0) ||
        (opcode == 0x11 && trailer - bth != 16))
        return ROCE_MALFORMED;
    *out = (struct roce_packet){.ip = ip, .udp = udp, .bth = bth,
                                .icrc = trailer, .end = end,
                                .source.family = family,
                                .destination.family = family,
                                .dst_qpn = be32(frame + bth + 4) & 0xffffff,
                                .psn = be32(frame + bth + 8) & 0xffffff,
                                .opcode = opcode,
                                .aeth_syndrome = opcode == 0x11 ? frame[bth + 12] : 0};
    memcpy(out->source.bytes, frame + source_offset, address_length);
    memcpy(out->destination.bytes, frame + destination_offset, address_length);
    return ROCE_OK;
}

int roce_check_icrc(const uint8_t *frame, size_t length)
{
    struct roce_packet p;
    int status = roce_parse(frame, length, &p);
    if (status != ROCE_OK)
        return status;
    uint32_t expected = p.destination.family == ROCE_IPV4 ?
        roce_icrc_ipv4(frame + p.ip, p.icrc - p.ip) :
        roce_icrc_ipv6(frame + p.ip, p.icrc - p.ip);
    for (unsigned i = 0; i < 4; ++i)
        if (frame[p.icrc + i] != (uint8_t)(expected >> (8 * i)))
            return ROCE_BAD_ICRC;
    return ROCE_OK;
}

static int parsed_offsets_valid(const struct roce_packet *p, size_t length)
{
    return p && p->ip >= 8 && p->udp >= p->ip && p->bth >= p->udp &&
        p->icrc >= p->bth && p->end >= p->icrc + 4 && p->end <= length &&
        (p->destination.family == ROCE_IPV4 ||
         p->destination.family == ROCE_IPV6);
}

static uint32_t calculate_icrc(uint8_t *frame, const struct roce_packet *p,
                               const struct icrc_backend *backend)
{
    size_t ip_length = p->icrc - p->ip;
    if (!backend)
        return p->destination.family == ROCE_IPV4 ?
            roce_icrc_ipv4(frame + p->ip, ip_length) :
            roce_icrc_ipv6(frame + p->ip, ip_length);
    return p->destination.family == ROCE_IPV4 ?
        roce_icrc_ipv4_inplace(frame + p->ip - 8, ip_length, backend) :
        roce_icrc_ipv6_inplace(frame + p->ip - 8, ip_length, backend);
}

int roce_check_icrc_parsed(uint8_t *frame, size_t length,
                           const struct roce_packet *p,
                           const struct icrc_backend *backend)
{
    if (!frame || !parsed_offsets_valid(p, length))
        return ROCE_MALFORMED;
    uint32_t expected = calculate_icrc(frame, p, backend);
    for (unsigned i = 0; i < 4; ++i)
        if (frame[p->icrc + i] != (uint8_t)(expected >> (8 * i)))
            return ROCE_BAD_ICRC;
    return ROCE_OK;
}

static uint32_t sum_bytes(const uint8_t *bytes, size_t length, uint32_t sum)
{
    while (length >= 2) {
        sum += be16(bytes); bytes += 2; length -= 2;
    }
    if (length)
        sum += (uint32_t)bytes[0] << 8;
    return sum;
}
static uint16_t checksum(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int rewrite_bytes(uint8_t *frame, size_t absolute_offset,
                         const uint8_t *replacement, size_t count,
                         size_t ip_offset, const struct icrc_delta *delta,
                         struct icrc_change *changes, size_t *change_count)
{
    if (delta) {
        if (absolute_offset < ip_offset || *change_count + count > 40)
            return -1;
        for (size_t i = 0; i < count; ++i)
            changes[(*change_count)++] = (struct icrc_change){
                .offset = absolute_offset + i - ip_offset,
                .before = frame[absolute_offset + i],
                .after = replacement[i],
            };
    }
    memcpy(frame + absolute_offset, replacement, count);
    return 0;
}

static int rewrite(uint8_t *frame, size_t length,
                   const struct roce_packet *parsed,
                   const struct roce_destination *dst,
                   const struct roce_destination *source, const uint32_t *psn,
                   const struct icrc_backend *backend,
                   const struct icrc_delta *delta, int verify_delta,
                   struct roce_timing *timing)
{
    if (timing) {
        timing->icrc_ticks = 0;
        timing->measured = 0;
    }
    if (!dst || dst->qpn > 0xffffff ||
        (dst->address.family != ROCE_IPV4 && dst->address.family != ROCE_IPV6))
        return ROCE_MALFORMED;
    struct roce_packet p;
    if (parsed) {
        if (!parsed_offsets_valid(parsed, length))
            return ROCE_MALFORMED;
        p = *parsed;
    } else {
        int status = roce_parse(frame, length, &p);
        if (status != ROCE_OK)
            return status;
    }
    if (dst->address.family != p.destination.family)
        return ROCE_UNSUPPORTED;
    if (source && source->address.family != p.destination.family)
        return ROCE_UNSUPPORTED;
    if (verify_delta && !delta)
        return ROCE_MALFORMED;
    struct icrc_change changes[40];
    size_t change_count = 0;
    uint32_t original_icrc = le32(frame + p.icrc);
    int udp_checksum_present = be16(frame + p.udp + 6) != 0;
    int disable_ipv4_udp_checksum = timing &&
        timing->disable_ipv4_udp_checksum &&
        p.destination.family == ROCE_IPV4;
    memcpy(frame, dst->mac, 6);
    if (source)
        memcpy(frame + 6, source->mac, 6);
    if (p.destination.family == ROCE_IPV4) {
        if (rewrite_bytes(frame, p.ip + 16, dst->address.bytes, 4, p.ip,
                          delta, changes, &change_count) != 0 ||
            (source && rewrite_bytes(frame, p.ip + 12, source->address.bytes,
                                     4, p.ip, delta, changes,
                                     &change_count) != 0))
            return ROCE_MALFORMED;
    } else {
        if (rewrite_bytes(frame, p.ip + 24, dst->address.bytes, 16, p.ip,
                          delta, changes, &change_count) != 0 ||
            (source && rewrite_bytes(frame, p.ip + 8, source->address.bytes,
                                     16, p.ip, delta, changes,
                                     &change_count) != 0))
            return ROCE_MALFORMED;
    }
    uint8_t qpn_bytes[3] = {dst->qpn >> 16, dst->qpn >> 8, dst->qpn};
    if (rewrite_bytes(frame, p.bth + 5, qpn_bytes, sizeof(qpn_bytes), p.ip,
                      delta, changes, &change_count) != 0)
        return ROCE_MALFORMED;
    if (psn) {
        if (*psn > 0xffffff)
            return ROCE_MALFORMED;
        uint8_t psn_bytes[3] = {*psn >> 16, *psn >> 8, *psn};
        if (rewrite_bytes(frame, p.bth + 9, psn_bytes, sizeof(psn_bytes), p.ip,
                          delta, changes, &change_count) != 0)
            return ROCE_MALFORMED;
    }
    if (p.destination.family == ROCE_IPV4) {
        put16(frame + p.ip + 10, 0);
        put16(frame + p.ip + 10, checksum(sum_bytes(frame + p.ip, 20, 0)));
    }
    uint64_t icrc_start = 0;
    if (timing && timing->read_ticks)
        icrc_start = timing->read_ticks(timing->context);
    uint32_t crc;
    if (delta) {
        if (icrc_delta_correct(delta, original_icrc, p.icrc - p.ip, changes,
                               change_count, &crc) != 0)
            return ROCE_MALFORMED;
        if (verify_delta && crc != calculate_icrc(frame, &p, backend))
            return ROCE_BAD_ICRC;
    } else {
        crc = calculate_icrc(frame, &p, backend);
    }
    if (timing && timing->read_ticks) {
        uint64_t icrc_end = timing->read_ticks(timing->context);
        timing->icrc_ticks = icrc_end - icrc_start;
        timing->measured = 1;
    }
    for (unsigned i = 0; i < 4; ++i)
        frame[p.icrc + i] = (uint8_t)(crc >> (8 * i));
    if (disable_ipv4_udp_checksum) {
        /* A zero checksum is the explicit IPv4 UDP no-checksum encoding. */
        put16(frame + p.udp + 6, 0);
    } else if (udp_checksum_present || p.destination.family == ROCE_IPV6) {
        /* UDP checksum covers the final ICRC. ICRC masks the UDP checksum. */
        put16(frame + p.udp + 6, 0);
        size_t udp_length = p.end - p.udp;
        uint32_t sum = 17 + (uint32_t)udp_length;
        if (p.destination.family == ROCE_IPV4)
            sum = sum_bytes(frame + p.ip + 12, 8, sum);
        else
            sum = sum_bytes(frame + p.ip + 8, 32, sum);
        uint16_t value = checksum(sum_bytes(frame + p.udp, udp_length, sum));
        put16(frame + p.udp + 6, value ? value : 0xffff);
    }
    return ROCE_OK;
}

int roce_rewrite(uint8_t *frame, size_t length, const struct roce_destination *dst)
{
    return rewrite(frame, length, NULL, dst, NULL, NULL, NULL, NULL, 0, NULL);
}

int roce_rewrite_timed(uint8_t *frame, size_t length,
                       const struct roce_destination *dst,
                       struct roce_timing *timing)
{
    return rewrite(frame, length, NULL, dst, NULL, NULL, NULL, NULL, 0, timing);
}

int roce_rewrite_psn(uint8_t *frame, size_t length,
                     const struct roce_destination *dst, uint32_t psn)
{
    if (psn > 0xffffff)
        return ROCE_MALFORMED;
    return rewrite(frame, length, NULL, dst, NULL, &psn, NULL, NULL, 0, NULL);
}

int roce_rewrite_psn_timed(uint8_t *frame, size_t length,
                           const struct roce_destination *dst, uint32_t psn,
                           struct roce_timing *timing)
{
    if (psn > 0xffffff)
        return ROCE_MALFORMED;
    return rewrite(frame, length, NULL, dst, NULL, &psn, NULL, NULL, 0, timing);
}

int roce_rewrite_ack(uint8_t *frame, size_t length,
                     const struct roce_destination *dst,
                     const struct roce_destination *source, uint32_t psn)
{
    if (!source || psn > 0xffffff)
        return ROCE_MALFORMED;
    return rewrite(frame, length, NULL, dst, source, &psn, NULL, NULL, 0, NULL);
}

int roce_rewrite_ack_timed(uint8_t *frame, size_t length,
                           const struct roce_destination *dst,
                           const struct roce_destination *source, uint32_t psn,
                           struct roce_timing *timing)
{
    if (!source || psn > 0xffffff)
        return ROCE_MALFORMED;
    return rewrite(frame, length, NULL, dst, source, &psn, NULL, NULL, 0, timing);
}

int roce_rewrite_parsed_timed(uint8_t *frame, size_t length,
                              const struct roce_packet *packet,
                              const struct roce_destination *dst,
                              const struct roce_destination *source,
                              const uint32_t *psn,
                              const struct icrc_backend *backend,
                              const struct icrc_delta *delta,
                              int verify_delta,
                              struct roce_timing *timing)
{
    if (psn && *psn > 0xffffff)
        return ROCE_MALFORMED;
    return rewrite(frame, length, packet, dst, source, psn, backend, delta,
                   verify_delta, timing);
}
