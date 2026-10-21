#include "worker.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct packet { uint8_t data[2048]; size_t length; unsigned released; };
static int copy(void *opaque, uint8_t *data, size_t length)
{
    struct packet *p = opaque;
    assert(p->length == length);
    size_t split = length / 2;
    memcpy(data, p->data, split);
    memcpy(data + split, p->data + split, length - split);
    return 0;
}
static void release(void *opaque) { ++((struct packet *)opaque)->released; }
struct backend { unsigned attempts; bool fail_first; };
static int submit(void *opaque, size_t slot, uint64_t generation, const uint8_t *data, size_t length)
{
    struct backend *b = opaque;
    (void)slot; (void)generation;
    assert(roce_check_icrc(data, length) == ROCE_OK);
    ++b->attempts;
    if (b->fail_first && b->attempts == 1) return WORKER_TX_FULL;
    return WORKER_SUBMITTED;
}
static int progress(void *opaque) { (void)opaque; return 0; }

int main(int argc, char **argv)
{
    assert(argc == 4);
    struct groups *groups = NULL;
    char error[256];
    assert(groups_load(argv[1], &groups, error, sizeof(error)) == 0);
    assert(groups->count == 1 && groups->member_count == 2);
    struct packet packet = {0};
    FILE *in = fopen(argv[2], "rb"); assert(in);
    packet.length = fread(packet.data, 1, sizeof(packet.data), in); fclose(in);
    assert(roce_check_icrc(packet.data, packet.length) == ROCE_OK);
    uint8_t original[2048]; memcpy(original, packet.data, packet.length);
    struct worker_rx rx = {&packet, packet.length, copy, release};
    uint8_t storage[2][2048];
    struct worker_slot slots[] = {{.data = storage[0], .capacity = 2048}, {.data = storage[1], .capacity = 2048}};
    struct backend backend = {0};
    struct worker_tx tx = {&backend, submit, progress};
    struct worker *w = calloc(1, sizeof(*w)); assert(w);
    assert(worker_init(w, 0, slots, 2, tx) == 0);
    assert(worker_set_groups(w, groups) == 0);
    assert(worker_forward(w, &rx) == 0 && packet.released == 1 && w->pending == 2);
    assert(worker_set_groups(w, NULL) == -1);
    assert(memcmp(packet.data, original, packet.length) == 0);
    FILE *out = fopen(argv[3], "wb"); assert(out);
    for (unsigned i = 0; i < 2; ++i) {
        struct roce_packet parsed;
        assert(roce_parse(storage[i], packet.length, &parsed) == ROCE_OK);
        assert(parsed.dst_qpn == groups->member[i].qpn);
        assert(w->members[i].submitted == 1);
        assert(fwrite(storage[i], 1, packet.length, out) == packet.length);
    }
    fclose(out);
    uint8_t saved[2][2048]; memcpy(saved, storage, sizeof(saved));
    assert(worker_forward(w, &rx) == 1 && w->stats.drop_full == 2);
    assert(memcmp(saved, storage, sizeof(saved)) == 0);
    assert(w->members[0].drops == 1 && w->members[1].drops == 1);
    assert(worker_complete(w, 1, slots[1].generation, false) == 0);
    assert(worker_complete(w, 0, slots[0].generation, true) == 0);
    assert(w->members[0].completed == 1 && w->members[1].errors == 1);
    worker_stop(w); assert(worker_drained(w));

    /* Submission failure of member 0 still allows member 1, with no aliasing. */
    backend = (struct backend){.fail_first = true};
    assert(worker_init(w, 0, slots, 2, tx) == 0);
    assert(worker_set_groups(w, groups) == 0);
    assert(worker_forward(w, &rx) == 0 && w->pending == 1);
    assert(w->members[0].drops == 1 && w->members[1].submitted == 1);
    assert(worker_complete(w, 0, slots[0].generation, true) == 0);
    assert(w->members[1].completed == 1);

    /* Reject unknown mappings and corrupted input rather than repair them. */
    struct roce_destination unknown = groups->member[0];
    assert(roce_rewrite(packet.data, packet.length, &unknown) == ROCE_OK);
    assert(worker_forward(w, &rx) == 1 && w->stats.drop_unknown_group == 1);
    memcpy(packet.data, original, packet.length);
    packet.data[packet.length - 1] ^= 1;
    assert(worker_forward(w, &rx) == 1 && w->stats.drop_icrc == 1);
    memcpy(packet.data, original, packet.length);
    packet.data[12] = 0x86;
    assert(worker_forward(w, &rx) == 1 && w->stats.drop_unsupported == 1);
    packet.length = rx.length = 1;
    assert(worker_forward(w, &rx) == 1 && w->stats.drop_malformed == 1);
    worker_stop(w); assert(worker_drained(w));
    assert(packet.released == 7);
    free(groups); free(w);
    return 0;
}
