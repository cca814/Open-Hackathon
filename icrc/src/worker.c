#include "worker.h"

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
    }
    return 0;
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
        goto release;
    }
    if (w->pending == w->slot_count) {
        ++w->stats.drop_full;
        goto release;
    }
    struct worker_slot *slot = NULL;
    size_t index = w->next_slot;
    for (size_t i = 0; i < w->slot_count; ++i) {
        if (!w->slots[index].busy && w->slots[index].capacity >= rx->length) {
            slot = &w->slots[index];
            break;
        }
        if (++index == w->slot_count)
            index = 0;
    }
    if (!slot || rx->length == 0) {
        ++w->stats.drop_oversize;
        goto release;
    }
    if (rx->copy(rx->packet, slot->data, rx->length) != 0) {
        ++w->stats.drop_copy;
        goto release;
    }
    slot->length = rx->length;
    ++slot->generation;
    int submitted = w->tx.submit(w->tx.context, index, slot->generation,
                                  slot->data, slot->length);
    if (submitted != WORKER_SUBMITTED) {
        if (submitted == WORKER_TX_FULL)
            ++w->stats.drop_full;
        else
            ++w->stats.drop_submit;
        slot->length = 0;
        goto release;
    }
    slot->busy = true;
    ++w->pending;
    ++w->stats.tx_submitted;
    w->stats.tx_submitted_bytes += rx->length;
    w->next_slot = index + 1 == w->slot_count ? 0 : index + 1;
    result = 0;
release:
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
