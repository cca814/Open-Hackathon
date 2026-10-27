#include "worker.h"
#include <string.h>

int worker_init(struct worker *w, unsigned queue_id,
                struct worker_slot *slots, size_t count, struct worker_tx tx)
{
    if (!w || !slots || count == 0 || queue_id >= NODE_B_WORKERS ||
        !tx.submit || !tx.progress)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].data || !slots[i].capacity)
            return -1;
    }
    *w = (struct worker){.slots = slots, .slot_count = count,
                         .queue_id = queue_id, .tx = tx};
    for (size_t i = 0; i < count; ++i) {
        slots[i].length = 0;
        slots[i].generation = 0;
        slots[i].busy = false;
        slots[i].member_index = SIZE_MAX;
    }
    return 0;
}

int worker_set_groups(struct worker *w, const struct groups *groups)
{
    if (!w || !w->slots || w->stats.rx_packets || w->pending || w->stopping)
        return -1;
    w->groups = groups;
    return 0;
}

static struct worker_slot *acquire(struct worker *w, size_t length, size_t *index)
{
    if (w->pending == w->slot_count) {
        ++w->stats.drop_full;
        return NULL;
    }
    *index = w->next_slot;
    for (size_t i = 0; i < w->slot_count; ++i) {
        struct worker_slot *slot = &w->slots[*index];
        if (!slot->busy && length && slot->capacity >= length)
            return slot;
        if (++*index == w->slot_count)
            *index = 0;
    }
    ++w->stats.drop_oversize;
    return NULL;
}

static int transmit(struct worker *w, size_t index, size_t length, size_t member)
{
    struct worker_slot *slot = &w->slots[index];
    slot->length = length;
    slot->member_index = member;
    ++slot->generation;
    int submitted = w->tx.submit(w->tx.context, index, slot->generation,
                                  slot->data, slot->length);
    if (submitted != WORKER_SUBMITTED) {
        if (submitted == WORKER_TX_FULL)
            ++w->stats.drop_full;
        else
            ++w->stats.drop_submit;
        slot->length = 0;
        slot->member_index = SIZE_MAX;
        return 1;
    }
    slot->busy = true;
    ++w->pending;
    ++w->stats.tx_submitted;
    w->stats.tx_submitted_bytes += length;
    if (member != SIZE_MAX)
        ++w->members[member].submitted;
    w->next_slot = index + 1 == w->slot_count ? 0 : index + 1;
    return 0;
}

static int fanout(struct worker *w, const struct worker_rx *rx)
{
    if (rx->length > sizeof(w->scratch)) {
        ++w->stats.drop_oversize;
        return 1;
    }
    if (rx->copy(rx->packet, w->scratch, rx->length) != 0) {
        ++w->stats.drop_copy;
        return 1;
    }
    struct roce_packet packet;
    int status = roce_parse(w->scratch, rx->length, &packet);
    if (status != ROCE_OK) {
        if (status == ROCE_UNSUPPORTED) ++w->stats.drop_unsupported;
        else ++w->stats.drop_malformed;
        return 1;
    }
    if (roce_check_icrc(w->scratch, rx->length) != ROCE_OK) {
        ++w->stats.drop_icrc;
        return 1;
    }
    const struct multicast_group *group = groups_find(w->groups, &packet.destination,
                                                       packet.dst_qpn);
    if (!group) {
        ++w->stats.drop_unknown_group;
        return 1;
    }
    int result = 1;
    for (size_t i = 0; i < group->member_count; ++i) {
        size_t member = group->first_member + i, index;
        struct worker_slot *slot = acquire(w, rx->length, &index);
        if (!slot) {
            ++w->members[member].drops;
            continue;
        }
        memcpy(slot->data, w->scratch, rx->length);
        if (roce_rewrite(slot->data, rx->length, &w->groups->member[member]) != ROCE_OK) {
            ++w->stats.drop_malformed;
            ++w->members[member].drops;
            continue;
        }
        if (transmit(w, index, rx->length, member) == 0)
            result = 0;
        else
            ++w->members[member].drops;
    }
    return result;
}

int worker_forward(struct worker *w, const struct worker_rx *rx)
{
    if (!w || !w->slots || !rx || !rx->copy || !rx->release)
        return -1;
    ++w->stats.rx_packets;
    w->stats.rx_bytes += rx->length;
    int result = 1;
    if (w->stopping) {
        ++w->stats.drop_stopping;
    } else if (w->groups) {
        result = fanout(w, rx);
    } else {
        size_t index;
        struct worker_slot *slot = acquire(w, rx->length, &index);
        if (slot) {
            if (rx->copy(rx->packet, slot->data, rx->length) != 0)
                ++w->stats.drop_copy;
            else
                result = transmit(w, index, rx->length, SIZE_MAX);
        }
    }
    rx->release(rx->packet);
    return result;
}

int worker_complete(struct worker *w, size_t index, uint64_t generation,
                    bool success)
{
    if (!w || !w->slots)
        return -1;
    if (index >= w->slot_count || !w->slots[index].busy ||
        w->slots[index].generation != generation) {
        ++w->stats.invalid_completions;
        return -1;
    }
    struct worker_slot *slot = &w->slots[index];
    if (success) {
        ++w->stats.tx_completed;
        w->stats.tx_completed_bytes += slot->length;
    } else {
        ++w->stats.tx_errors;
    }
    if (slot->member_index != SIZE_MAX) {
        struct worker_member_stats *member = &w->members[slot->member_index];
        if (success) ++member->completed;
        else ++member->errors;
    }
    slot->member_index = SIZE_MAX;
    slot->busy = false;
    slot->length = 0;
    --w->pending;
    return 0;
}

int worker_progress(struct worker *w)
{
    if (!w || !w->tx.progress)
        return -1;
    return w->tx.progress(w->tx.context);
}

void worker_stop(struct worker *w)
{
    if (w)
        w->stopping = true;
}

bool worker_drained(const struct worker *w)
{
    return w && w->stopping && w->pending == 0;
}
