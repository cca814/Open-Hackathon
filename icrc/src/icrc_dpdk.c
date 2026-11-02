#include "icrc_dpdk.h"

#include <rte_net_crc.h>
#include <stdlib.h>

struct icrc_dpdk {
    struct rte_net_crc *context;
    struct icrc_backend backend;
};

static uint32_t calculate(void *context, const uint8_t *data, size_t length)
{
    return rte_net_crc_calc(context, data, (uint32_t)length);
}

static int self_test(const struct icrc_backend *candidate)
{
    uint8_t data[1536];
    uint32_t state = UINT32_C(0x12345678);
    for (size_t i = 0; i < sizeof(data); ++i) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        data[i] = (uint8_t)(state >> 24);
    }
    static const size_t lengths[] = {
        1, 7, 8, 15, 16, 31, 32, 39, 40, 63, 64, 65,
        127, 128, 255, 256, 1024, 1536,
    };
    const struct icrc_backend *reference = icrc_backend_portable();
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        uint32_t expected = reference->crc32(reference->context, data, lengths[i]);
        uint32_t actual = candidate->crc32(candidate->context, data, lengths[i]);
        if (actual != expected)
            return -1;
    }
    return 0;
}

int icrc_dpdk_create(struct icrc_dpdk **instance)
{
    if (!instance || *instance)
        return -1;
    struct icrc_dpdk *created = calloc(1, sizeof(*created));
    if (!created)
        return -1;
    created->context = rte_net_crc_set_alg(RTE_NET_CRC_NEON,
                                            RTE_NET_CRC32_ETH);
    if (!created->context) {
        free(created);
        return -1;
    }
    created->backend = (struct icrc_backend){
        .name = "dpdk-neon-full",
        .context = created->context,
        .crc32 = calculate,
    };
    if (self_test(&created->backend) != 0) {
        rte_net_crc_free(created->context);
        free(created);
        return -1;
    }
    *instance = created;
    return 0;
}

const struct icrc_backend *icrc_dpdk_backend(const struct icrc_dpdk *instance)
{
    return instance ? &instance->backend : NULL;
}

void icrc_dpdk_destroy(struct icrc_dpdk **instance)
{
    if (!instance || !*instance)
        return;
    rte_net_crc_free((*instance)->context);
    free(*instance);
    *instance = NULL;
}
