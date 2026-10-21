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

#define CHECK(call, label) do { status = (call); if (status != DOCA_SUCCESS) goto label; } while (0)

doca_error_t flow_init(void)
{
    struct doca_flow_cfg *cfg;
    doca_error_t status = doca_flow_cfg_create(&cfg);
    if (status != DOCA_SUCCESS)
        return status;
    CHECK(doca_flow_cfg_set_pipe_queues(cfg, 1), done);
    CHECK(doca_flow_cfg_set_mode_args(cfg, "vnf,hws,isolated"), done);
    CHECK(doca_flow_cfg_set_nr_counters(cfg, 1), done);
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
    status = doca_flow_port_start(cfg, &flow->port);
done:
    doca_flow_port_cfg_destroy(cfg);
    return status;
}

doca_error_t flow_steer_roce(struct flow *flow)
{
    if (!flow || !flow->port || flow->pipe)
        return DOCA_ERROR_INVALID_VALUE;
    struct doca_flow_match match = {0};
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
    match.outer.udp.l4_port.dst_port = DOCA_HTOBE16(4791);
    struct doca_flow_actions actions = {0};
    struct doca_flow_actions *actions_array[] = {&actions};
    struct doca_flow_monitor monitor = {0};
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    uint16_t queues[NODE_B_WORKERS];
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i)
        queues[i] = i;
    struct doca_flow_fwd rss = {0};
    rss.type = DOCA_FLOW_FWD_RSS;
    rss.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    rss.rss.queues_array = queues;
    rss.rss.nr_queues = NODE_B_WORKERS;
    rss.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
    struct doca_flow_fwd miss = {.type = DOCA_FLOW_FWD_DROP};
    struct doca_flow_pipe_cfg *cfg;
    doca_error_t status = doca_flow_pipe_cfg_create(&cfg, flow->port);
    if (status != DOCA_SUCCESS)
        return status;
    CHECK(doca_flow_pipe_cfg_set_name(cfg, "NODE_B_ROCE_RSS"), done);
    CHECK(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC), done);
    CHECK(doca_flow_pipe_cfg_set_is_root(cfg, true), done);
    CHECK(doca_flow_pipe_cfg_set_match(cfg, &match, NULL), done);
    CHECK(doca_flow_pipe_cfg_set_actions(cfg, actions_array, NULL, NULL, 1), done);
    CHECK(doca_flow_pipe_cfg_set_monitor(cfg, &monitor), done);
    CHECK(doca_flow_pipe_create(cfg, &rss, &miss, &flow->pipe), done);
    flow->processed = 0;
    flow->failed = false;
    /* Match is fixed by the pipe; entry has no variable fields. */
    struct doca_flow_match entry_match = {0};
    CHECK(doca_flow_pipe_basic_add_entry(0, flow->pipe, &entry_match, 0, &actions,
          NULL, NULL, 0, flow, &flow->entry), done);
    CHECK(doca_flow_entries_process(flow->port, 0, 10000, 1), done);
    if (flow->failed || flow->processed != 1)
        status = DOCA_ERROR_BAD_STATE;
done:
    doca_flow_pipe_cfg_destroy(cfg);
    /* On failure retain handles until flow_stop_port; callback user must live. */
    return status;
}

doca_error_t flow_read_counter(struct flow *flow, uint64_t *packets, uint64_t *bytes)
{
    if (!flow || !flow->entry || !packets || !bytes)
        return DOCA_ERROR_INVALID_VALUE;
    struct doca_flow_resource_query query = {0};
    doca_error_t status = doca_flow_resource_query_entry(flow->entry, &query);
    if (status == DOCA_SUCCESS) {
        *packets = query.counter.total_pkts;
        *bytes = query.counter.total_bytes;
    }
    return status;
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
