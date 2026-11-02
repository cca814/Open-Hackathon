#ifndef NODE_B_RELIABILITY_H
#define NODE_B_RELIABILITY_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "groups.h"
#include "metrics.h"

struct icrc_backend;
struct icrc_delta;

#define ROCE_PSN_MASK 0x00ffffffu
#define ROCE_PSN_HALF 0x00800000u
#define RELIABILITY_RTT_SLOTS RELIABLE_ACK_WINDOW

enum reliability_result {
    RELIABILITY_NOT_ACK,
    RELIABILITY_FORWARD_ACK,
    RELIABILITY_SUPPRESS_ACK,
    RELIABILITY_INVALID_ACK
};

struct reliability_counters {
    uint64_t received, forwarded, suppressed, stale, malformed, delayed;
    uint64_t rtt_overwrites, rtt_missed;
};

struct reliability_send_stamp {
    uint64_t ticks;
    uint32_t psn;
    uint8_t valid;
};

struct reliability_group_state {
    atomic_flag lock;
    uint32_t ack_psn[GROUP_MEMBERS_MAX];
    uint32_t agg_ack_psn;
    size_t tri_port;
    struct reliability_counters counters;
    struct metric_summary rtt;
    struct reliability_send_stamp *send_stamp;
};

struct reliability {
    const struct groups *groups;
    struct reliability_group_state state[GROUPS_MAX];
    atomic_bool test_delay_used;
    size_t test_delay_member;
    uint32_t test_delay_us;
};

/* RC PSNs are a 24-bit serial number space. Comparisons are defined only
 * within half the space, which is the normal RC send-window invariant. */
int roce_psn_after(uint32_t candidate, uint32_t reference);

/* The groups table must outlive the manager and remain immutable. */
int reliability_init(struct reliability *manager, const struct groups *groups);
void reliability_destroy(struct reliability *manager);
/* Controlled Milestone 4 validation hook. Delays the first ACK from one
 * member; disabled by default and never used for normal operation. */
int reliability_set_test_delay(struct reliability *manager, size_t member,
                               uint32_t microseconds);

/* Parses and validates a receiver ACK in frame. Frontier-advancing ACKs are
 * rewritten in place for the sender. Suppressed ACKs must not be transmitted. */
enum reliability_result reliability_process(struct reliability *manager,
                                               uint8_t *frame, size_t length);
enum reliability_result reliability_process_at(struct reliability *manager,
                                                uint8_t *frame, size_t length,
                                                uint64_t ack_ticks,
                                                struct roce_timing *timing);
enum reliability_result reliability_process_at_backend(
    struct reliability *manager, uint8_t *frame, size_t length,
    uint64_t ack_ticks, const struct icrc_backend *backend,
    const struct icrc_delta *delta, int verify_delta,
    struct roce_timing *timing);
int reliability_record_send(struct reliability *manager,
                            const struct multicast_group *group,
                            uint32_t sender_psn, uint64_t send_ticks);
void reliability_cancel_send(struct reliability *manager,
                             const struct multicast_group *group,
                             uint32_t sender_psn);

#endif
