#define _POSIX_C_SOURCE 200809L
#include "reliability.h"
#include "roce.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static void lock_state(struct reliability_group_state *state)
{
    while (atomic_flag_test_and_set_explicit(&state->lock, memory_order_acquire))
        ;
}

static void unlock_state(struct reliability_group_state *state)
{
    atomic_flag_clear_explicit(&state->lock, memory_order_release);
}

int roce_psn_after(uint32_t candidate, uint32_t reference)
{
    uint32_t distance = (candidate - reference) & ROCE_PSN_MASK;
    return distance != 0 && distance < ROCE_PSN_HALF;
}

int reliability_init(struct reliability *manager, const struct groups *groups)
{
    if (!manager || !groups)
        return -1;
    memset(manager, 0, sizeof(*manager));
    manager->groups = groups;
    atomic_init(&manager->test_delay_used, false);
    for (size_t g = 0; g < groups->count; ++g) {
        const struct multicast_group *group = &groups->group[g];
        struct reliability_group_state *state = &manager->state[g];
        atomic_flag_clear(&state->lock);
        if (!group->reliable)
            continue;
        state->send_stamp = calloc(RELIABILITY_RTT_SLOTS,
                                   sizeof(*state->send_stamp));
        if (!state->send_stamp) {
            reliability_destroy(manager);
            return -1;
        }
        uint32_t before_first = (group->sender_initial_psn - 1u) & ROCE_PSN_MASK;
        state->agg_ack_psn = before_first;
        state->tri_port = 0;
        for (size_t i = 0; i < group->member_count; ++i)
            state->ack_psn[i] = before_first;
    }
    return 0;
}

void reliability_destroy(struct reliability *manager)
{
    if (!manager)
        return;
    for (size_t g = 0; g < GROUPS_MAX; ++g) {
        free(manager->state[g].send_stamp);
        manager->state[g].send_stamp = NULL;
    }
    manager->groups = NULL;
}

int reliability_set_test_delay(struct reliability *manager, size_t member,
                               uint32_t microseconds)
{
    if (!manager || !manager->groups || microseconds == 0 ||
        member >= GROUP_MEMBERS_MAX)
        return -1;
    manager->test_delay_member = member;
    manager->test_delay_us = microseconds;
    atomic_store(&manager->test_delay_used, false);
    return 0;
}

static uint32_t sender_psn(const struct groups *groups,
                           const struct multicast_group *group,
                           size_t member, uint32_t receiver_psn)
{
    size_t absolute = group->first_member + member;
    uint32_t offset = (receiver_psn - groups->member_initial_psn[absolute]) &
                      ROCE_PSN_MASK;
    return (group->sender_initial_psn + offset) & ROCE_PSN_MASK;
}

static int identify_member(const struct reliability *manager,
                           const struct multicast_group *group,
                           const struct reliability_group_state *state,
                           uint16_t candidates, uint32_t receiver_psn,
                           size_t *member, uint32_t *translated)
{
    if (!candidates || !member || !translated)
        return -1;
    if ((candidates & (uint16_t)(candidates - 1u)) == 0) {
        for (size_t i = 0; i < group->member_count; ++i)
            if (candidates & (uint16_t)(1u << i)) {
                *member = i;
                *translated = sender_psn(manager->groups, group, i, receiver_psn);
                return 0;
            }
    }
    uint32_t best_distance = UINT32_MAX;
    size_t best_member = SIZE_MAX;
    uint32_t best_translated = 0;
    bool tied = false;
    for (size_t i = 0; i < group->member_count; ++i) {
        if (!(candidates & (uint16_t)(1u << i)))
            continue;
        uint32_t candidate = sender_psn(manager->groups, group, i, receiver_psn);
        uint32_t forward = (candidate - state->ack_psn[i]) & ROCE_PSN_MASK;
        uint32_t backward = (state->ack_psn[i] - candidate) & ROCE_PSN_MASK;
        uint32_t distance = forward < backward ? forward : backward;
        if (distance > RELIABLE_ACK_WINDOW)
            continue;
        if (distance < best_distance) {
            best_distance = distance;
            best_member = i;
            best_translated = candidate;
            tied = false;
        } else if (distance == best_distance) {
            tied = true;
        }
    }
    if (best_member == SIZE_MAX || tied)
        return -1;
    *member = best_member;
    *translated = best_translated;
    return 0;
}

int reliability_record_send(struct reliability *manager,
                            const struct multicast_group *group,
                            uint32_t sender_psn, uint64_t send_ticks)
{
    if (!manager || !manager->groups || !group || !group->reliable || !send_ticks)
        return -1;
    size_t group_index = (size_t)(group - manager->groups->group);
    if (group_index >= manager->groups->count)
        return -1;
    struct reliability_group_state *state = &manager->state[group_index];
    size_t index = sender_psn & (RELIABILITY_RTT_SLOTS - 1u);
    lock_state(state);
    struct reliability_send_stamp *stamp = &state->send_stamp[index];
    if (stamp->valid)
        ++state->counters.rtt_overwrites;
    *stamp = (struct reliability_send_stamp){
        .ticks = send_ticks,
        .psn = sender_psn & ROCE_PSN_MASK,
        .valid = 1,
    };
    unlock_state(state);
    return 0;
}

void reliability_cancel_send(struct reliability *manager,
                             const struct multicast_group *group,
                             uint32_t sender_psn)
{
    if (!manager || !manager->groups || !group || !group->reliable)
        return;
    size_t group_index = (size_t)(group - manager->groups->group);
    if (group_index >= manager->groups->count)
        return;
    struct reliability_group_state *state = &manager->state[group_index];
    size_t index = sender_psn & (RELIABILITY_RTT_SLOTS - 1u);
    lock_state(state);
    struct reliability_send_stamp *stamp = &state->send_stamp[index];
    if (stamp->valid && stamp->psn == (sender_psn & ROCE_PSN_MASK))
        stamp->valid = 0;
    unlock_state(state);
}

static void retire_cumulatively_acked_stamps(
    struct reliability_group_state *state, uint32_t previous, uint32_t aggregate)
{
    uint32_t advance = (aggregate - previous) & ROCE_PSN_MASK;
    if (advance >= RELIABILITY_RTT_SLOTS) {
        memset(state->send_stamp, 0,
               RELIABILITY_RTT_SLOTS * sizeof(*state->send_stamp));
        return;
    }
    for (uint32_t offset = 1; offset <= advance; ++offset) {
        uint32_t psn = (previous + offset) & ROCE_PSN_MASK;
        struct reliability_send_stamp *stamp =
            &state->send_stamp[psn & (RELIABILITY_RTT_SLOTS - 1u)];
        if (stamp->valid && stamp->psn == psn)
            stamp->valid = 0;
    }
}

enum reliability_result reliability_process_at(struct reliability *manager,
                                                uint8_t *frame, size_t length,
                                                uint64_t ack_ticks,
                                                struct roce_timing *timing)
{
    if (!manager || !manager->groups || !frame)
        return RELIABILITY_INVALID_ACK;
    struct roce_packet packet;
    if (roce_parse(frame, length, &packet) != ROCE_OK || packet.opcode != 0x11)
        return RELIABILITY_NOT_ACK;

    uint16_t member_mask = 0;
    const struct multicast_group *group = groups_find_feedback(
        manager->groups, &packet.source, &packet.destination, packet.dst_qpn,
        &member_mask);
    if (!group)
        return RELIABILITY_NOT_ACK;
    size_t group_index = (size_t)(group - manager->groups->group);
    struct reliability_group_state *state = &manager->state[group_index];
    if (packet.aeth_syndrome >= 0x20 || roce_check_icrc(frame, length) != ROCE_OK) {
        lock_state(state);
        ++state->counters.malformed;
        unlock_state(state);
        return RELIABILITY_INVALID_ACK;
    }

    size_t member = 0;
    uint32_t translated = 0;
    lock_state(state);
    int identified = identify_member(manager, group, state, member_mask, packet.psn,
                                     &member, &translated);
    if (identified != 0)
        ++state->counters.malformed;
    unlock_state(state);
    if (identified != 0)
        return RELIABILITY_INVALID_ACK;

    bool expected = false;
    if (manager->test_delay_us && member == manager->test_delay_member &&
        atomic_compare_exchange_strong(&manager->test_delay_used, &expected, true)) {
        struct timespec delay = {
            .tv_sec = manager->test_delay_us / 1000000u,
            .tv_nsec = (long)(manager->test_delay_us % 1000000u) * 1000L,
        };
        while (nanosleep(&delay, &delay) != 0)
            ;
        lock_state(state);
        ++state->counters.delayed;
        unlock_state(state);
    }

    uint32_t aggregate = 0;
    lock_state(state);
    if (identify_member(manager, group, state, member_mask, packet.psn,
                        &member, &translated) != 0) {
        ++state->counters.malformed;
        unlock_state(state);
        return RELIABILITY_INVALID_ACK;
    }
    ++state->counters.received;
    if (!roce_psn_after(translated, state->ack_psn[member])) {
        ++state->counters.stale;
        ++state->counters.suppressed;
        unlock_state(state);
        return RELIABILITY_SUPPRESS_ACK;
    }
    state->ack_psn[member] = translated;

    /* Cepheus triPort: a non-minimum receiver cannot advance the group-wide
     * frontier. Only rescan when the tracked minimum receiver advances. */
    if (member != state->tri_port) {
        ++state->counters.suppressed;
        unlock_state(state);
        return RELIABILITY_SUPPRESS_ACK;
    }
    size_t minimum_member = 0;
    uint32_t minimum_distance =
        (state->ack_psn[0] - state->agg_ack_psn) & ROCE_PSN_MASK;
    for (size_t i = 1; i < group->member_count; ++i) {
        uint32_t distance =
            (state->ack_psn[i] - state->agg_ack_psn) & ROCE_PSN_MASK;
        if (distance < minimum_distance) {
            minimum_distance = distance;
            minimum_member = i;
        }
    }
    state->tri_port = minimum_member;
    if (minimum_distance == 0) {
        ++state->counters.suppressed;
        unlock_state(state);
        return RELIABILITY_SUPPRESS_ACK;
    }
    uint32_t previous_aggregate = state->agg_ack_psn;
    aggregate = state->ack_psn[minimum_member];
    state->agg_ack_psn = aggregate;
    ++state->counters.forwarded;
    if (ack_ticks) {
        struct reliability_send_stamp *stamp =
            &state->send_stamp[aggregate & (RELIABILITY_RTT_SLOTS - 1u)];
        if (stamp->valid && stamp->psn == aggregate) {
            metric_observe(&state->rtt, ack_ticks - stamp->ticks);
            stamp->valid = 0;
        } else {
            ++state->counters.rtt_missed;
        }
    }
    /* A cumulative RC ACK retires every earlier PSN it covers, even when the
     * receiver coalesces ACKs and the aggregate frontier jumps. Leaving those
     * timestamps valid produces false ring-overwrite reports on long runs. */
    retire_cumulatively_acked_stamps(state, previous_aggregate, aggregate);
    unlock_state(state);

    if (roce_rewrite_ack_timed(frame, length, &group->sender, &group->ack_source,
                               aggregate, timing) != ROCE_OK) {
        lock_state(state);
        ++state->counters.malformed;
        unlock_state(state);
        return RELIABILITY_INVALID_ACK;
    }
    return RELIABILITY_FORWARD_ACK;
}

enum reliability_result reliability_process(struct reliability *manager,
                                              uint8_t *frame, size_t length)
{
    return reliability_process_at(manager, frame, length, 0, NULL);
}
