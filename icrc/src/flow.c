/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "flow.h"
#include "worker.h"
#include <doca_bitfield.h>
#include <stdio.h>

static void entry_processed(struct doca_flow_pipe_entry *entry, uint16_t queue,
                            enum doca_flow_entry_status status,
                            enum doca_flow_entry_op operation, void *user)
{
    (void)entry;
    (void)queue;
    (void)operation;
    struct flow *flow = user;
    if (flow) {
        ++flow->processed;
        if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
            flow->failed = true;
    }
}

#define CHECK(call, label) do { \
    status = (call); \
    if (status != DOCA_SUCCESS) { \
        fprintf(stderr, "%s: %s\n", #call, doca_error_get_descr(status)); \
        goto label; \
    } \
} while (0)

doca_error_t flow_init(void)
{
    struct doca_flow_cfg *cfg;
    doca_error_t status = doca_flow_cfg_create(&cfg);
    if (status != DOCA_SUCCESS)
        return status;
    CHECK(doca_flow_cfg_set_pipe_queues(cfg, 1), done);
    CHECK(doca_flow_cfg_set_mode_args(cfg, "vnf,hws"), done);
    CHECK(doca_flow_cfg_set_resource_mode(cfg, DOCA_FLOW_RESOURCE_MODE_PORT), done);
    CHECK(doca_flow_cfg_set_cb_entry_process(cfg, entry_processed), done);
    status = doca_flow_init(cfg);
done:
    doca_flow_cfg_destroy(cfg);
    return status;
}

doca_error_t flow_start_port(struct flow *flow, struct doca_dev *dev, uint16_t port_id)
{
    if (!flow || !dev || flow->port)
        return DOCA_ERROR_INVALID_VALUE;
    struct doca_flow_port_cfg *cfg;
    doca_error_t status = doca_flow_port_cfg_create(&cfg);
    if (status != DOCA_SUCCESS)
        return status;
    CHECK(doca_flow_port_cfg_set_port_id(cfg, port_id), done);
    CHECK(doca_flow_port_cfg_set_dev(cfg, dev), done);
    /* Four entries need 512 bytes plus the sample's 1024-byte baseline,
     * rounded to the next power of two. RSS is a port-level resource in
     * DOCA 3.5 and must be reserved before starting the port. */
    CHECK(doca_flow_port_cfg_set_actions_mem_size(cfg, 2048), done);
    CHECK(doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_COUNTER, 2), done);
    CHECK(doca_flow_port_cfg_set_nr_resources(cfg, DOCA_FLOW_RESOURCE_RSS, 2), done);
    status = doca_flow_port_start(cfg, &flow->port);
done:
    doca_flow_port_cfg_destroy(cfg);
    return status;
}

doca_error_t flow_steer_roce(struct flow *flow)
{
    if (!flow || !flow->port || flow->control_pipe || flow->rss_pipe[0] || flow->rss_pipe[1])
        return DOCA_ERROR_INVALID_VALUE;
    static const enum doca_flow_l3_meta l3_type[2] = {
        DOCA_FLOW_L3_META_IPV4,
        DOCA_FLOW_L3_META_IPV6,
    };
    static const uint32_t rss_flags[2] = {
        DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP,
        DOCA_FLOW_RSS_IPV6 | DOCA_FLOW_RSS_UDP,
    };
    uint16_t queues[NODE_B_WORKERS];
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i)
        queues[i] = i;
    flow->processed = 0;
    flow->failed = false;
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_fwd miss = {.type = DOCA_FLOW_FWD_DROP};
    struct doca_flow_monitor monitor = {
        .counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
    };
    doca_error_t status = DOCA_SUCCESS;

    for (unsigned family = 0; family < 2; ++family) {
        struct doca_flow_match match = {0};
        match.parser_meta.outer_l3_type = l3_type[family];
        match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
        /* All ones declares the destination port as entry supplied. */
        match.outer.udp.l4_port.dst_port = UINT16_MAX;
        struct doca_flow_fwd rss = {
            .type = DOCA_FLOW_FWD_RSS,
            .rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
            .rss = {
                .queues_array = queues,
                .nr_queues = NODE_B_WORKERS,
                .outer_flags = rss_flags[family],
            },
        };
        CHECK(doca_flow_pipe_cfg_create(&cfg, flow->port), done);
        CHECK(doca_flow_pipe_cfg_set_name(cfg,
              family == 0 ? "NODE_B_ROCE_IPV4_RSS" : "NODE_B_ROCE_IPV6_RSS"), done);
        CHECK(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), done);
        CHECK(doca_flow_pipe_cfg_set_is_root(cfg, false), done);
        CHECK(doca_flow_pipe_cfg_set_match(cfg, &match, NULL), done);
        CHECK(doca_flow_pipe_cfg_set_monitor(cfg, &monitor), done);
        CHECK(doca_flow_pipe_create(cfg, &rss, &miss, &flow->rss_pipe[family]), done);
        doca_flow_pipe_cfg_destroy(cfg);
        cfg = NULL;

        struct doca_flow_match entry_match = {0};
        entry_match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(4791);
        CHECK(doca_flow_pipe_basic_add_entry(0, flow->rss_pipe[family], &entry_match,
              0, NULL, NULL, NULL, 0, flow, &flow->rss_entry[family]), done);
    }

    CHECK(doca_flow_pipe_cfg_create(&cfg, flow->port), done);
    CHECK(doca_flow_pipe_cfg_set_name(cfg, "NODE_B_ROCE_IP_CLASSIFIER"), done);
    CHECK(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_CONTROL), done);
    CHECK(doca_flow_pipe_cfg_set_is_root(cfg, true), done);
    CHECK(doca_flow_pipe_create(cfg, NULL, &miss, &flow->control_pipe), done);
    doca_flow_pipe_cfg_destroy(cfg);
    cfg = NULL;

    for (unsigned family = 0; family < 2; ++family) {
        struct doca_flow_match match = {0};
        struct doca_flow_match mask = {0};
        match.parser_meta.outer_l3_type = l3_type[family];
        match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
        match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(4791);
        mask.parser_meta.outer_l3_type = UINT32_MAX;
        mask.parser_meta.outer_l4_type = UINT32_MAX;
        mask.outer.udp.l4_port.dst_port = UINT16_MAX;
        struct doca_flow_fwd next = {
            .type = DOCA_FLOW_FWD_PIPE,
            .next_pipe = flow->rss_pipe[family],
        };
        CHECK(doca_flow_pipe_control_add_entry(0, flow->control_pipe,
              &match, &mask, NULL, NULL, NULL, NULL, NULL, family, &next, flow,
              &flow->control_entry[family]), done);
    }
    CHECK(doca_flow_entries_process(flow->port, 0, 10000, 4), done);
    if (flow->failed || flow->processed != 4)
        status = DOCA_ERROR_BAD_STATE;
done:
    if (cfg)
        doca_flow_pipe_cfg_destroy(cfg);
    /* On failure retain handles until flow_stop_port; callback user must live. */
    return status;
}

doca_error_t flow_read_counter(struct flow *flow, uint64_t *packets, uint64_t *bytes)
{
    if (!flow || !flow->rss_entry[0] || !flow->rss_entry[1] || !packets || !bytes)
        return DOCA_ERROR_INVALID_VALUE;
    *packets = 0;
    *bytes = 0;
    for (unsigned family = 0; family < 2; ++family) {
        struct doca_flow_resource_query query = {0};
        doca_error_t status = doca_flow_resource_query_entry(flow->rss_entry[family], &query);
        if (status != DOCA_SUCCESS)
            return status;
        *packets += query.counter.total_pkts;
        *bytes += query.counter.total_bytes;
    }
    return DOCA_SUCCESS;
}

doca_error_t flow_stop_port(struct flow *flow)
{
    if (!flow)
        return DOCA_ERROR_INVALID_VALUE;
    if (!flow->port)
        return DOCA_SUCCESS;
    doca_error_t status = doca_flow_port_stop(flow->port);
    if (status == DOCA_SUCCESS)
        *flow = (struct flow){0};
    return status;
}

void flow_destroy(void)
{
    doca_flow_destroy();
}
