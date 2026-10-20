#include "worker.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct packet {
    const char *first, *second;
    size_t first_len, second_len;
    unsigned releases;
    bool truncated;
};

static int copy(void *opaque, uint8_t *data, size_t length)
{
    struct packet *p = opaque;
    if (p->truncated || length != p->first_len + p->second_len)
        return -1;
    memcpy(data, p->first, p->first_len);
    memcpy(data + p->first_len, p->second, p->second_len);
    return 0;
}

static void release(void *opaque)
{
    ++((struct packet *)opaque)->releases;
}

struct backend {
    int result;
    unsigned submissions, progresses;
};

static int submit(void *opaque, size_t index, uint64_t generation,
                  const uint8_t *data, size_t length)
{
    struct backend *b = opaque;
    (void)index;
    assert(generation > 0);
    assert(length == 6);
    assert(memcmp(data, "abcdef", length) == 0);
    ++b->submissions;
    return b->result;
}

static int progress(void *opaque)
{
    ++((struct backend *)opaque)->progresses;
    return 0;
}

static void exercise(unsigned queue)
{
    uint8_t storage[2][64];
    struct worker_slot slots[] = {{.data = storage[0], .capacity = 64},
                                  {.data = storage[1], .capacity = 64}};
    struct backend backend = {0};
    struct worker w;
    struct worker_tx tx = {&backend, submit, progress};
    assert(worker_init(&w, queue, slots, 2, tx) == 0);
    struct packet packet = {"abc", "def", 3, 3, 0, false};
    struct worker_rx rx = {&packet, 6, copy, release};
    assert(worker_forward(&w, &rx) == 0);
    assert(worker_forward(&w, &rx) == 0);
    assert(w.pending == 2 && packet.releases == 2);
    uint64_t old_generation = slots[0].generation;
    assert(worker_forward(&w, &rx) == 1);
    assert(w.stats.drop_full == 1 && packet.releases == 3);
    assert(worker_complete(&w, 1, slots[1].generation, false) == 0);
    assert(worker_complete(&w, 0, old_generation, true) == 0);
    assert(w.stats.tx_errors == 1 && w.stats.tx_completed_bytes == 6);
    assert(worker_complete(&w, 0, old_generation, true) == -1);
    assert(worker_forward(&w, &rx) == 0);
    assert(worker_complete(&w, 0, old_generation, true) == -1);
    assert(slots[0].busy && w.pending == 1);
    worker_stop(&w);
    assert(!worker_drained(&w));
    assert(worker_forward(&w, &rx) == 1);
    assert(w.stats.drop_stopping == 1);
    assert(worker_progress(&w) == 0 && backend.progresses == 1);
    assert(worker_complete(&w, 0, slots[0].generation, true) == 0);
    assert(worker_drained(&w));
    assert(packet.releases == 5);

    assert(worker_init(&w, queue, slots, 2, tx) == 0);
    backend.result = WORKER_TX_FULL;
    assert(worker_forward(&w, &rx) == 1 && w.pending == 0);
    backend.result = WORKER_TX_ERROR;
    assert(worker_forward(&w, &rx) == 1 && w.pending == 0);
    assert(w.stats.drop_full == 1 && w.stats.drop_submit == 1);
    backend.result = WORKER_SUBMITTED;
    packet.truncated = true;
    assert(worker_forward(&w, &rx) == 1 && w.stats.drop_copy == 1);
    rx.length = 65;
    assert(worker_forward(&w, &rx) == 1 && w.stats.drop_oversize == 1);
    rx.length = 0;
    assert(worker_forward(&w, &rx) == 1 && w.stats.drop_oversize == 2);
    assert(packet.releases == 10);
    worker_stop(&w);
    assert(worker_drained(&w));

    assert(worker_init(&w, queue, slots, 2, tx) == 0);
    packet.truncated = false;
    rx.length = 6;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        assert(worker_forward(&w, &rx) == 0);
        assert(worker_forward(&w, &rx) == 0);
        /* Reverse completion order exercises independently owned buffers. */
        assert(worker_complete(&w, 1, slots[1].generation, true) == 0);
        assert(worker_complete(&w, 0, slots[0].generation, false) == 0);
        assert(w.pending == 0);
    }
    assert(w.stats.tx_submitted == 20000);
    assert(w.stats.tx_completed == 10000 && w.stats.tx_errors == 10000);
    assert(packet.releases == 20010);
    worker_stop(&w);
    assert(worker_drained(&w));
}

int main(void)
{
    for (unsigned queue = 0; queue < NODE_B_WORKERS; ++queue)
        exercise(queue);
    assert(worker_init(NULL, 0, NULL, 0, (struct worker_tx){0}) == -1);
    puts("worker ownership/backpressure checks passed for eight queue IDs");
    return 0;
}
