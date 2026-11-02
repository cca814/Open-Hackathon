#include "icrc.h"
#include "reliability.h"
#include "roce.h"
#include "worker.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t sum_bytes(const uint8_t *bytes, size_t length, uint32_t sum)
{
    while (length >= 2) {
        sum += (uint32_t)bytes[0] << 8 | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)bytes[0] << 8;
    return sum;
}

static uint16_t finish_checksum(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put24(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 16);
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)value;
}

static size_t make_ack(uint8_t *frame, const struct roce_destination *receiver,
                       const struct roce_destination *sender, uint32_t psn,
                       uint8_t syndrome)
{
    const size_t ip = 14, udp = 54, bth = 62, icrc = 78, length = 82;
    memset(frame, 0, length);
    memcpy(frame, sender->mac, 6);
    memcpy(frame + 6, receiver->mac, 6);
    frame[12] = 0x86;
    frame[13] = 0xdd;
    frame[ip] = 0x60;
    put16(frame + ip + 4, 28);
    frame[ip + 6] = 17;
    frame[ip + 7] = 64;
    memcpy(frame + ip + 8, receiver->address.bytes, 16);
    memcpy(frame + ip + 24, sender->address.bytes, 16);
    put16(frame + udp, 4791);
    put16(frame + udp + 2, 4791);
    put16(frame + udp + 4, 28);
    frame[bth] = 0x11;
    put16(frame + bth + 2, 0xffff);
    put24(frame + bth + 5, sender->qpn);
    put24(frame + bth + 9, psn);
    frame[bth + 12] = syndrome;
    uint32_t crc = roce_icrc_ipv6(frame + ip, icrc - ip);
    for (unsigned i = 0; i < 4; ++i)
        frame[icrc + i] = (uint8_t)(crc >> (8 * i));
    uint32_t sum = 17 + 28;
    sum = sum_bytes(frame + ip + 8, 32, sum);
    uint16_t checksum = finish_checksum(sum_bytes(frame + udp, 28, sum));
    put16(frame + udp + 6, checksum ? checksum : 0xffff);
    return length;
}

static size_t make_ack4(uint8_t *frame, const struct roce_destination *receiver,
                        const struct roce_destination *sender, uint32_t psn,
                        uint8_t syndrome)
{
    const size_t ip = 14, udp = 34, bth = 42, icrc = 58, length = 62;
    memset(frame, 0, length);
    memcpy(frame, sender->mac, 6);
    memcpy(frame + 6, receiver->mac, 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    frame[ip] = 0x45;
    put16(frame + ip + 2, 48);
    put16(frame + ip + 4, 0x1234);
    put16(frame + ip + 6, 0x4000);
    frame[ip + 8] = 64;
    frame[ip + 9] = 17;
    memcpy(frame + ip + 12, receiver->address.bytes, 4);
    memcpy(frame + ip + 16, sender->address.bytes, 4);
    put16(frame + ip + 10, finish_checksum(sum_bytes(frame + ip, 20, 0)));
    put16(frame + udp, 4791);
    put16(frame + udp + 2, 4791);
    put16(frame + udp + 4, 28);
    frame[bth] = 0x11;
    put16(frame + bth + 2, 0xffff);
    put24(frame + bth + 5, sender->qpn);
    put24(frame + bth + 9, psn);
    frame[bth + 12] = syndrome;
    uint32_t crc = roce_icrc_ipv4(frame + ip, icrc - ip);
    for (unsigned i = 0; i < 4; ++i)
        frame[icrc + i] = (uint8_t)(crc >> (8 * i));
    uint32_t sum = 17 + 28;
    sum = sum_bytes(frame + ip + 12, 8, sum);
    uint16_t checksum = finish_checksum(sum_bytes(frame + udp, 28, sum));
    put16(frame + udp + 6, checksum ? checksum : 0xffff);
    return length;
}

static struct roce_destination endpoint(const char *mac, const char *ip, uint32_t qpn)
{
    struct roce_destination result = {.address.family = ROCE_IPV6, .qpn = qpn};
    unsigned values[6];
    assert(sscanf(mac, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
                  &values[3], &values[4], &values[5]) == 6);
    for (unsigned i = 0; i < 6; ++i)
        result.mac[i] = (uint8_t)values[i];
    assert(inet_pton(AF_INET6, ip, result.address.bytes) == 1);
    return result;
}

static struct roce_destination endpoint4(const char *mac, const char *ip, uint32_t qpn)
{
    struct roce_destination result = {.address.family = ROCE_IPV4, .qpn = qpn};
    unsigned values[6];
    assert(sscanf(mac, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
                  &values[3], &values[4], &values[5]) == 6);
    for (unsigned i = 0; i < 6; ++i)
        result.mac[i] = (uint8_t)values[i];
    assert(inet_pton(AF_INET, ip, result.address.bytes) == 1);
    return result;
}

static struct groups fixture(uint32_t sender_initial)
{
    struct groups groups = {.count = 1, .member_count = 2};
    struct multicast_group *group = &groups.group[0];
    strcpy(group->id, "reliable-demo");
    group->reliable = true;
    group->member_count = 2;
    group->sender_initial_psn = sender_initial;
    group->sender = endpoint("02:00:00:00:00:10", "2001:db8::10", 500);
    group->ack_source = endpoint("02:00:00:00:01:00", "2001:db8::100", 0);
    groups.member[0] = endpoint("02:00:00:00:00:01", "2001:db8::1", 101);
    groups.member[1] = endpoint("02:00:00:00:00:02", "2001:db8::2", 102);
    groups.member_initial_psn[0] = 1000;
    groups.member_initial_psn[1] = 2000;
    groups.member_initial_set[0] = groups.member_initial_set[1] = true;
    return groups;
}

static struct groups fixture4(uint32_t sender_initial)
{
    struct groups groups = {.count = 1, .member_count = 2};
    struct multicast_group *group = &groups.group[0];
    strcpy(group->id, "reliable-ipv4-demo");
    group->reliable = true;
    group->member_count = 2;
    group->sender_initial_psn = sender_initial;
    group->sender = endpoint4("02:00:00:00:00:10", "192.0.2.10", 500);
    group->ack_source = endpoint4("02:00:00:00:01:00", "192.0.2.100", 0);
    groups.member[0] = endpoint4("02:00:00:00:00:01", "192.0.2.1", 101);
    groups.member[1] = endpoint4("02:00:00:00:00:02", "192.0.2.2", 102);
    groups.member_initial_psn[0] = 1000;
    groups.member_initial_psn[1] = 2000;
    groups.member_initial_set[0] = groups.member_initial_set[1] = true;
    return groups;
}

static enum reliability_result ack(struct reliability *manager,
                                   const struct groups *groups, size_t member,
                                   uint32_t receiver_psn, uint8_t syndrome,
                                   uint8_t *frame, size_t *length)
{
    *length = make_ack(frame, &groups->member[member], &groups->group[0].sender,
                       receiver_psn, syndrome);
    return reliability_process(manager, frame, *length);
}

struct packet_buffer {
    uint8_t bytes[128];
    size_t length;
    unsigned releases;
};

static int copy_packet(void *opaque, uint8_t *destination, size_t length)
{
    struct packet_buffer *packet = opaque;
    if (length != packet->length)
        return -1;
    memcpy(destination, packet->bytes, length);
    return 0;
}

static void release_packet(void *opaque)
{
    ++((struct packet_buffer *)opaque)->releases;
}

struct submit_backend { unsigned submissions; };
static int submit_packet(void *opaque, size_t slot, uint64_t generation,
                         const uint8_t *data, size_t length)
{
    struct submit_backend *backend = opaque;
    (void)slot;
    assert(generation != 0 && roce_check_icrc(data, length) == ROCE_OK);
    ++backend->submissions;
    return WORKER_SUBMITTED;
}
static int progress_packets(void *opaque) { (void)opaque; return 0; }

struct fake_clock { uint64_t ticks, step; };
static uint64_t read_fake_clock(void *opaque)
{
    struct fake_clock *clock = opaque;
    clock->ticks += clock->step;
    return clock->ticks;
}

int main(void)
{
    assert(roce_psn_after(0, 0xffffff));
    assert(roce_psn_after(0x7fffff, 0));
    assert(!roce_psn_after(0x800000, 0));
    assert(!roce_psn_after(7, 7));

    struct groups groups = fixture(100);
    struct reliability manager;
    assert(reliability_init(&manager, &groups) == 0);
    uint8_t frame[128];
    size_t length;
    assert(ack(&manager, &groups, 0, 1000, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK);
    assert(ack(&manager, &groups, 1, 2000, 0, frame, &length) ==
           RELIABILITY_FORWARD_ACK);
    struct roce_packet parsed;
    assert(roce_parse(frame, length, &parsed) == ROCE_OK);
    assert(parsed.psn == 100 && parsed.dst_qpn == groups.group[0].sender.qpn);
    assert(!memcmp(frame, groups.group[0].sender.mac, 6));
    assert(!memcmp(frame + 6, groups.group[0].ack_source.mac, 6));
    assert(!memcmp(parsed.source.bytes, groups.group[0].ack_source.address.bytes, 16));
    assert(roce_check_icrc(frame, length) == ROCE_OK);

    assert(ack(&manager, &groups, 1, 2000, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK); /* duplicate */
    assert(ack(&manager, &groups, 0, 1002, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK); /* delayed receiver still at 100 */
    assert(ack(&manager, &groups, 0, 1001, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK); /* reordered/stale */
    assert(ack(&manager, &groups, 1, 2001, 0, frame, &length) ==
           RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 101);
    assert(ack(&manager, &groups, 1, 2002, 0, frame, &length) ==
           RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 102);
    assert(ack(&manager, &groups, 0, 1003, 0x60, frame, &length) ==
           RELIABILITY_INVALID_ACK); /* NACK deferred to Milestone 5 */
    assert(manager.state[0].counters.forwarded == 3);
    assert(manager.state[0].counters.stale == 2);
    assert(manager.state[0].counters.malformed == 1);

    reliability_destroy(&manager);
    groups = fixture(0xffffff);
    assert(reliability_init(&manager, &groups) == 0);
    assert(ack(&manager, &groups, 0, 1000, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK);
    assert(ack(&manager, &groups, 1, 2000, 0, frame, &length) ==
           RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 0xffffff);
    assert(ack(&manager, &groups, 0, 1001, 0, frame, &length) ==
           RELIABILITY_SUPPRESS_ACK);
    assert(ack(&manager, &groups, 1, 2001, 0, frame, &length) ==
           RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 0);

    /* Exercise the actual reverse worker path: first receiver is suppressed;
     * second receiver advances the frontier and owns one TX slot. */
    reliability_destroy(&manager);
    groups = fixture(100);
    assert(reliability_init(&manager, &groups) == 0);
    assert(reliability_set_test_delay(&manager, 0, 1000) == 0);
    uint8_t storage[128];
    struct worker_slot slot = {.data = storage, .capacity = sizeof(storage)};
    struct submit_backend backend = {0};
    struct worker worker;
    struct worker_tx tx = {&backend, submit_packet, progress_packets};
    struct icrc_delta *delta = NULL;
    assert(icrc_delta_create(NODE_B_MAX_FRAME, &delta) == 0);
    assert(worker_init(&worker, 0, &slot, 1, tx) == 0);
    assert(worker_set_reliability(&worker, &manager) == 0);
    assert(worker_set_icrc_delta(&worker, delta, true) == 0);
    struct fake_clock clock = {.ticks = 100, .step = 10};
    assert(worker_set_clock(&worker,
                            (struct worker_clock){read_fake_clock, &clock}) == 0);
    assert(reliability_record_send(&manager, &groups.group[0], 100, 100) == 0);
    struct packet_buffer packet = {0};
    packet.length = make_ack(packet.bytes, &groups.member[0], &groups.group[0].sender,
                             1000, 0);
    struct worker_rx rx = {&packet, packet.length, copy_packet, release_packet};
    assert(worker_forward(&worker, &rx) == 1 && worker.pending == 0);
    assert(manager.state[0].counters.delayed == 1);
    packet.length = make_ack(packet.bytes, &groups.member[1], &groups.group[0].sender,
                             2000, 0);
    rx.length = packet.length;
    assert(worker_forward(&worker, &rx) == 0 && worker.pending == 1);
    assert(backend.submissions == 1 && worker.stats.ack_suppressed == 1 &&
           worker.stats.ack_forwarded == 1 && packet.releases == 2);
    assert(manager.state[0].rtt.count == 1 && manager.state[0].rtt.total == 20);
    assert(worker.stats.icrc_compute.count == 1 &&
           worker.stats.icrc_compute.total == 10);
    assert(roce_parse(storage, packet.length, &parsed) == ROCE_OK && parsed.psn == 100);
    assert(!memcmp(parsed.source.bytes, groups.group[0].ack_source.address.bytes, 16));
    assert(worker_complete(&worker, 0, slot.generation, true) == 0);
    worker_stop(&worker);
    assert(worker_drained(&worker));
    icrc_delta_destroy(&delta);
    reliability_destroy(&manager);

    /* IPv4 uses the same PSN/ACK state while additionally requiring IPv4,
     * UDP, and ICRC updates after aggregate feedback address rewriting. */
    groups = fixture4(100);
    assert(reliability_init(&manager, &groups) == 0);
    assert(reliability_record_send(&manager, &groups.group[0], 100, 100) == 0);
    length = make_ack4(frame, &groups.member[0], &groups.group[0].sender, 1000, 0);
    assert(reliability_process_at(&manager, frame, length, 150, NULL) ==
           RELIABILITY_SUPPRESS_ACK);
    length = make_ack4(frame, &groups.member[1], &groups.group[0].sender, 2000, 0);
    clock = (struct fake_clock){.ticks = 200, .step = 10};
    struct roce_timing timing = {
        .read_ticks = read_fake_clock,
        .context = &clock,
    };
    assert(reliability_process_at(&manager, frame, length, 175, &timing) ==
           RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK);
    assert(parsed.destination.family == ROCE_IPV4 && parsed.psn == 100);
    assert(!memcmp(parsed.source.bytes, groups.group[0].ack_source.address.bytes, 4));
    assert(!memcmp(parsed.destination.bytes, groups.group[0].sender.address.bytes, 4));
    assert(roce_check_icrc(frame, length) == ROCE_OK && timing.icrc_ticks == 10);
    assert(finish_checksum(sum_bytes(frame + 14, 20, 0)) == 0);
    uint32_t udp_sum = 17 + 28;
    udp_sum = sum_bytes(frame + 26, 8, udp_sum);
    assert(finish_checksum(sum_bytes(frame + 34, 28, udp_sum)) == 0);
    assert(manager.state[0].rtt.count == 1 && manager.state[0].rtt.total == 75);

    /* A coalesced cumulative ACK must retire timestamps for every skipped PSN,
     * otherwise later ring reuse is falsely counted as an overwrite. */
    assert(reliability_record_send(&manager, &groups.group[0], 101, 110) == 0);
    assert(reliability_record_send(&manager, &groups.group[0], 102, 120) == 0);
    assert(reliability_record_send(&manager, &groups.group[0], 103, 130) == 0);
    length = make_ack4(frame, &groups.member[0], &groups.group[0].sender, 1003, 0);
    assert(reliability_process_at(&manager, frame, length, 200, NULL) ==
           RELIABILITY_SUPPRESS_ACK);
    length = make_ack4(frame, &groups.member[1], &groups.group[0].sender, 2003, 0);
    assert(reliability_process_at(&manager, frame, length, 250, NULL) ==
           RELIABILITY_FORWARD_ACK);
    for (uint32_t psn = 101; psn <= 103; ++psn)
        assert(!manager.state[0].send_stamp[
            psn & (RELIABILITY_RTT_SLOTS - 1u)].valid);
    assert(reliability_record_send(&manager, &groups.group[0],
                                   101 + RELIABILITY_RTT_SLOTS, 300) == 0);
    assert(manager.state[0].counters.rtt_overwrites == 0);
    reliability_destroy(&manager);

    /* Two QPs on one receiver interface share source IP/MAC. Their translated
     * PSN domains, not a second interface, identify the ACK member. */
    groups = fixture4(100);
    groups.member[1].address = groups.member[0].address;
    memcpy(groups.member[1].mac, groups.member[0].mac, sizeof(groups.member[1].mac));
    groups.member_initial_psn[1] = 0x100000;
    assert(reliability_init(&manager, &groups) == 0);
    length = make_ack4(frame, &groups.member[0], &groups.group[0].sender, 1000, 0);
    assert(reliability_process(&manager, frame, length) == RELIABILITY_SUPPRESS_ACK);
    length = make_ack4(frame, &groups.member[1], &groups.group[0].sender,
                       0x100000, 0);
    assert(reliability_process(&manager, frame, length) == RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 100);
    length = make_ack4(frame, &groups.member[1], &groups.group[0].sender,
                       0x100000, 0);
    assert(reliability_process(&manager, frame, length) == RELIABILITY_SUPPRESS_ACK);
    length = make_ack4(frame, &groups.member[0], &groups.group[0].sender, 1001, 0);
    assert(reliability_process(&manager, frame, length) == RELIABILITY_SUPPRESS_ACK);
    length = make_ack4(frame, &groups.member[1], &groups.group[0].sender,
                       0x100001, 0);
    assert(reliability_process(&manager, frame, length) == RELIABILITY_FORWARD_ACK);
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 101);
    assert(manager.state[0].counters.received == 5);
    assert(manager.state[0].counters.forwarded == 2);
    assert(manager.state[0].counters.stale == 1);
    reliability_destroy(&manager);

    /* Five QPs on one interface/IP advance only after all five ACK. */
    groups = fixture4(100);
    groups.member_count = 5;
    groups.group[0].member_count = 5;
    for (size_t i = 0; i < 5; ++i) {
        groups.member[i] = groups.member[0];
        groups.member[i].qpn = 101u + (uint32_t)i;
        groups.member_initial_psn[i] = (uint32_t)(i + 1u) * 0x200000u;
        groups.member_initial_set[i] = true;
    }
    assert(reliability_init(&manager, &groups) == 0);
    for (size_t i = 0; i < 5; ++i) {
        length = make_ack4(frame, &groups.member[i], &groups.group[0].sender,
                           groups.member_initial_psn[i], 0);
        enum reliability_result expected = i == 4 ? RELIABILITY_FORWARD_ACK :
                                                    RELIABILITY_SUPPRESS_ACK;
        assert(reliability_process(&manager, frame, length) == expected);
    }
    assert(roce_parse(frame, length, &parsed) == ROCE_OK && parsed.psn == 100);
    assert(manager.state[0].counters.received == 5);
    assert(manager.state[0].counters.forwarded == 1);
    assert(manager.state[0].counters.suppressed == 4);
    reliability_destroy(&manager);

    puts("IPv4/IPv6 ACK aggregation, metrics, stale ACK and PSN wrap tests passed");
    return 0;
}
