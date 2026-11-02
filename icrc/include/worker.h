#ifndef NODE_B_WORKER_H
#define NODE_B_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "groups.h"
#include "icrc.h"
#include "icrc_delta.h"
#include "metrics.h"
#include "reliability.h"

#define NODE_B_WORKERS 8u
#define NODE_B_MAX_FRAME 2048u

/* Adapter supplies a validated RX length and copies across mbuf segments.
 * copy returns zero only after copying exactly length bytes. release is called
 * once by forward, even on drop; neither callback retains the RX packet. */
struct worker_rx {
    void *packet;
    size_t length;
    int (*copy)(void *packet, uint8_t *destination, size_t length);
    void (*release)(void *packet);
};

struct worker_slot {
    uint8_t *data;                 /* Adapter-owned, registered TX storage. */
    size_t capacity;
    size_t length;
    uint64_t generation;
    bool busy;
    size_t member_index; /* SIZE_MAX for unchanged baseline packets. */
};

struct worker_stats {
    uint64_t rx_packets, rx_bytes;
    uint64_t tx_submitted, tx_submitted_bytes;
    uint64_t tx_completed, tx_completed_bytes, tx_errors;
    uint64_t drop_full, drop_oversize, drop_copy, drop_submit, drop_stopping;
    uint64_t invalid_completions;
    uint64_t drop_malformed, drop_unsupported, drop_icrc, drop_unknown_group;
    uint64_t icrc_verify_fail;
    uint64_t ack_forwarded, ack_suppressed, ack_invalid;
    struct metric_summary icrc_compute;
};

enum worker_submit_result {
    WORKER_SUBMITTED = 0,
    WORKER_TX_FULL = 1,
    WORKER_TX_ERROR = -1
};

/* Successful submit transfers buffer ownership until worker_complete.
 * Failure transfers no ownership and must never produce a completion.
 * Completion is asynchronous: submit must not invoke worker_complete inline.
 * Progress and completions run on the same worker thread as forward. */
struct worker_tx {
    void *context;
    int (*submit)(void *context, size_t slot, uint64_t generation,
                  const uint8_t *data, size_t length);
    int (*progress)(void *context);
};

struct worker_member_stats {
    uint64_t submitted, completed, errors, drops;
};

struct worker_clock {
    uint64_t (*read_ticks)(void *context);
    void *context;
};

struct worker {
    struct worker_slot *slots;
    size_t slot_count, next_slot, pending;
    unsigned queue_id;
    bool stopping;
    struct worker_tx tx;
    struct worker_stats stats;
    const struct groups *groups; /* Immutable and alive until all workers drain. */
    struct reliability *reliability; /* Shared, internally synchronized. */
    struct worker_clock clock;
    const struct icrc_backend *icrc;
    const struct icrc_delta *icrc_delta;
    bool verify_icrc_delta;
    bool ipv4_udp_checksum_enabled;
    uint8_t scratch[NODE_B_MAX_FRAME];
    struct worker_member_stats members[MEMBERS_MAX];
};

/* Each worker exclusively owns its RX queue, TX context, slots and counters.
 * Initialization requires distinct non-overlapping slot storage and a fresh
 * context with no outstanding TX. No memory is allocated by the worker. */
int worker_init(struct worker *worker, unsigned queue_id,
                struct worker_slot *slots, size_t count, struct worker_tx tx);
/* Enable group rewrite before receiving packets; NULL keeps unchanged baseline. */
int worker_set_groups(struct worker *worker, const struct groups *groups);
int worker_set_reliability(struct worker *worker, struct reliability *reliability);
int worker_set_clock(struct worker *worker, struct worker_clock clock);
int worker_set_icrc_backend(struct worker *worker,
                            const struct icrc_backend *backend);
int worker_set_icrc_delta(struct worker *worker,
                          const struct icrc_delta *delta, bool verify);
/* IPv4 alone permits disabling the UDP checksum. IPv6 remains checksummed. */
int worker_set_ipv4_udp_checksum(struct worker *worker, bool enabled);
/* Returns 0 if any replica was submitted, 1 if all dropped, -1 for invalid arguments.
 * Fan-out is best effort: failed/full member submissions do not roll back others.
 * Valid RX descriptors are always released, including during shutdown. */
int worker_forward(struct worker *worker, const struct worker_rx *rx);
int worker_complete(struct worker *worker, size_t slot, uint64_t generation,
                    bool success);
int worker_progress(struct worker *worker);
void worker_stop(struct worker *worker);
bool worker_drained(const struct worker *worker);

#endif
