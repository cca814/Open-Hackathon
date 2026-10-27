#include "roce.h"
#include "icrc.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value; }

int roce_parse(const uint8_t *frame, size_t length, struct roce_packet *out)
{
    if (!frame || !out || length < 14)
        return ROCE_MALFORMED;
    size_t ip = 14;
    size_t udp, ip_length, address_offset, address_length;
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
        address_offset = ip + 16;
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
        address_offset = ip + 24;
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
    if (opcode > 5 || (frame[bth + 1] & 15u) != 0)
        return ROCE_UNSUPPORTED;
    size_t headers = 12 + ((opcode == 3 || opcode == 5) ? 4 : 0);
    if (trailer - bth < headers || padding > trailer - bth - headers ||
        ((opcode == 0 || opcode == 1) && padding != 0))
        return ROCE_MALFORMED;
    *out = (struct roce_packet){.ip = ip, .udp = udp, .bth = bth,
                                .icrc = trailer, .end = end,
                                .destination.family = family,
                                .dst_qpn = be32(frame + bth + 4) & 0xffffff};
    memcpy(out->destination.bytes, frame + address_offset, address_length);
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

int roce_rewrite(uint8_t *frame, size_t length, const struct roce_destination *dst)
{
    if (!dst || dst->qpn > 0xffffff ||
        (dst->address.family != ROCE_IPV4 && dst->address.family != ROCE_IPV6))
        return ROCE_MALFORMED;
    struct roce_packet p;
    int status = roce_parse(frame, length, &p);
    if (status != ROCE_OK)
        return status;
    if (dst->address.family != p.destination.family)
        return ROCE_UNSUPPORTED;
    int udp_checksum_present = be16(frame + p.udp + 6) != 0;
    memcpy(frame, dst->mac, 6);
    if (p.destination.family == ROCE_IPV4)
        memcpy(frame + p.ip + 16, dst->address.bytes, 4);
    else
        memcpy(frame + p.ip + 24, dst->address.bytes, 16);
    frame[p.bth + 5] = dst->qpn >> 16;
    frame[p.bth + 6] = dst->qpn >> 8;
    frame[p.bth + 7] = dst->qpn;
    if (p.destination.family == ROCE_IPV4) {
        put16(frame + p.ip + 10, 0);
        put16(frame + p.ip + 10, checksum(sum_bytes(frame + p.ip, 20, 0)));
    }
    uint32_t crc = p.destination.family == ROCE_IPV4 ?
        roce_icrc_ipv4(frame + p.ip, p.icrc - p.ip) :
        roce_icrc_ipv6(frame + p.ip, p.icrc - p.ip);
    for (unsigned i = 0; i < 4; ++i)
        frame[p.icrc + i] = (uint8_t)(crc >> (8 * i));
    if (udp_checksum_present || p.destination.family == ROCE_IPV6) {
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
