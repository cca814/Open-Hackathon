#include "icrc_delta.h"

#include <stdint.h>
#include <stdlib.h>

#define DELTA_VALUES 256u

struct icrc_delta {
    size_t max_ip_length;
    uint32_t *contribution;
};

static uint32_t update(uint32_t state, uint8_t byte)
{
    state ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        state = (state >> 1) ^ ((state & 1u) ? UINT32_C(0xedb88320) : 0);
    return state;
}

int icrc_delta_create(size_t max_ip_length, struct icrc_delta **instance)
{
    if (!instance || *instance || max_ip_length == 0 ||
        max_ip_length > SIZE_MAX / DELTA_VALUES / sizeof(uint32_t) - 1)
        return -1;
    struct icrc_delta *created = calloc(1, sizeof(*created));
    if (!created)
        return -1;
    size_t entries = (max_ip_length + 1) * DELTA_VALUES;
    created->contribution = malloc(entries * sizeof(*created->contribution));
    if (!created->contribution) {
        free(created);
        return -1;
    }
    created->max_ip_length = max_ip_length;
    for (unsigned delta = 0; delta < DELTA_VALUES; ++delta)
        created->contribution[delta] = update(0, (uint8_t)delta);
    for (size_t remaining = 1; remaining <= max_ip_length; ++remaining)
        for (unsigned delta = 0; delta < DELTA_VALUES; ++delta)
            created->contribution[remaining * DELTA_VALUES + delta] =
                update(created->contribution[(remaining - 1) * DELTA_VALUES + delta], 0);
    *instance = created;
    return 0;
}

void icrc_delta_destroy(struct icrc_delta **instance)
{
    if (!instance || !*instance)
        return;
    free((*instance)->contribution);
    free(*instance);
    *instance = NULL;
}

int icrc_delta_correct(const struct icrc_delta *instance, uint32_t original,
                       size_t ip_length, const struct icrc_change *changes,
                       size_t change_count, uint32_t *corrected)
{
    if (!instance || !corrected || (!changes && change_count))
        return -1;
    uint32_t result = original;
    for (size_t i = 0; i < change_count; ++i) {
        if (changes[i].offset >= ip_length)
            return -1;
        size_t remaining = ip_length - changes[i].offset - 1;
        if (remaining > instance->max_ip_length)
            return -1;
        uint8_t delta = changes[i].before ^ changes[i].after;
        result ^= instance->contribution[remaining * DELTA_VALUES + delta];
    }
    *corrected = result;
    return 0;
}
