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

#include "dpdk_rx.h"
#include "eth_tx.h"
#include "flow.h"
#include "icrc_delta.h"
#include "icrc_dpdk.h"

#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <doca_dpdk.h>

static volatile sig_atomic_t interrupted;
static atomic_bool stop_requested;

struct runner {
    struct worker forward;
    struct worker reverse;
    struct eth_tx *forward_tx;
    struct eth_tx *reverse_tx;
    uint16_t forward_port;
    uint16_t reverse_port;
};

static void on_signal(int number)
{
    (void)number;
    interrupted = 1;
}

static uint64_t milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t read_tsc(void *context)
{
    (void)context;
    return rte_rdtsc();
}

static void print_metric(const char *name, const struct metric_summary *metric,
                         uint64_t ticks_per_second)
{
    if (!metric->count)
        return;
    printf("metric=%s samples=%" PRIu64 " avg_ns=%" PRIu64
           " min_ns=%" PRIu64 " p50_upper_ns=%" PRIu64
           " p99_upper_ns=%" PRIu64 " max_ns=%" PRIu64 "\n",
           name, metric->count,
           metric_ticks_to_ns(metric->total / metric->count, ticks_per_second),
           metric_ticks_to_ns(metric->minimum, ticks_per_second),
           metric_ticks_to_ns(metric_percentile_upper(metric, 50), ticks_per_second),
           metric_ticks_to_ns(metric_percentile_upper(metric, 99), ticks_per_second),
           metric_ticks_to_ns(metric->maximum, ticks_per_second));
}

static int run_worker(void *argument)
{
    struct runner *runner = argument;
    int failed = 0;
    while (!atomic_load(&stop_requested)) {
        if (dpdk_rx_poll(&runner->forward, runner->forward_port) < 0 ||
            dpdk_rx_poll(&runner->reverse, runner->reverse_port) < 0) {
            failed = 1;
            atomic_store(&stop_requested, true);
            break;
        }
    }
    worker_stop(&runner->forward);
    worker_stop(&runner->reverse);
    uint64_t deadline = milliseconds() + 5000;
    while (!worker_drained(&runner->forward) || !worker_drained(&runner->reverse)) {
        if ((!worker_drained(&runner->forward) && worker_progress(&runner->forward) != 0) ||
            (!worker_drained(&runner->reverse) && worker_progress(&runner->reverse) != 0) ||
            milliseconds() >= deadline)
            return -1;
    }
    return failed ? -1 : 0;
}

static void print_worker_stats(const char *direction, unsigned queue,
                               const struct worker *worker, const struct groups *groups,
                               uint64_t ticks_per_second)
{
    const struct worker_stats *s = &worker->stats;
    printf("direction=%s queue=%u rx=%" PRIu64 " submitted=%" PRIu64
           " completed=%" PRIu64 " tx_errors=%" PRIu64 " drops=%" PRIu64
           " pending=%zu\n", direction, queue, s->rx_packets, s->tx_submitted,
           s->tx_completed, s->tx_errors,
           s->drop_full + s->drop_copy + s->drop_oversize + s->drop_submit + s->drop_stopping,
           worker->pending);
    printf("direction=%s queue=%u malformed=%" PRIu64 " unsupported=%" PRIu64
           " bad_icrc=%" PRIu64 " unknown_group=%" PRIu64
           " icrc_verify_fail=%" PRIu64
           " ack_forwarded=%" PRIu64 " ack_suppressed=%" PRIu64
           " ack_invalid=%" PRIu64 "\n", direction, queue,
           s->drop_malformed, s->drop_unsupported, s->drop_icrc,
           s->drop_unknown_group, s->icrc_verify_fail,
           s->ack_forwarded, s->ack_suppressed, s->ack_invalid);
    if (s->icrc_compute.count) {
        char metric_name[64];
        snprintf(metric_name, sizeof(metric_name), "icrc_compute_%s_q%u",
                 direction, queue);
        print_metric(metric_name, &s->icrc_compute, ticks_per_second);
    }
    if (!groups)
        return;
    for (size_t g = 0; g < groups->count; ++g) {
        const struct multicast_group *group = &groups->group[g];
        for (size_t m = 0; m < group->member_count; ++m) {
            const struct worker_member_stats *ms = &worker->members[group->first_member + m];
            printf("direction=%s queue=%u group=%s member=%zu submitted=%" PRIu64
                   " completed=%" PRIu64 " errors=%" PRIu64 " drops=%" PRIu64 "\n",
                   direction, queue, group->id, m, ms->submitted, ms->completed,
                   ms->errors, ms->drops);
        }
    }
}

static doca_error_t open_device(const char *pci, struct doca_dev **dev)
{
    struct doca_devinfo **list;
    uint32_t count;
    doca_error_t status = doca_devinfo_create_list(&list, &count);
    if (status != DOCA_SUCCESS)
        return status;
    status = DOCA_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t equal = 0;
        if (doca_devinfo_is_equal_pci_addr(list[i], pci, &equal) == DOCA_SUCCESS && equal) {
            status = doca_dev_open(list[i], dev);
            break;
        }
    }
    doca_devinfo_destroy_list(list);
    return status;
}

static int map_port(struct doca_dev *dev, uint16_t *port)
{
    uint16_t candidate;
    unsigned matches = 0;
    RTE_ETH_FOREACH_DEV(candidate) {
        struct doca_dev *mapped = NULL;
        if (doca_dpdk_port_as_dev(candidate, &mapped) == DOCA_SUCCESS && mapped == dev) {
            *port = candidate;
            ++matches;
        }
    }
    return matches == 1 ? 0 : -1;
}

static int start_rx(uint16_t port, struct rte_mempool *pool)
{
    struct rte_eth_dev_info info;
    if (rte_eth_dev_info_get(port, &info) < 0 || info.max_rx_queues < NODE_B_WORKERS)
        return -1;
    struct rte_eth_conf config = {0};
    config.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    config.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP |
                                           RTE_ETH_RSS_NONFRAG_IPV6_UDP;
    if ((info.flow_type_rss_offloads & config.rx_adv_conf.rss_conf.rss_hf) !=
        config.rx_adv_conf.rss_conf.rss_hf)
        return -1;
    if (rte_eth_dev_configure(port, NODE_B_WORKERS, 0, &config) < 0)
        return -1;
    uint16_t rx_descriptors = 1024, tx_descriptors = 0;
    if (rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_descriptors, &tx_descriptors) < 0)
        return -1;
    for (unsigned q = 0; q < NODE_B_WORKERS; ++q)
        if (rte_eth_rx_queue_setup(port, q, rx_descriptors,
                                  rte_eth_dev_socket_id(port), NULL, pool) < 0)
            return -1;
    return rte_eth_dev_start(port);
}

#define DOCA_CHECK(call) do { doca_error_t error = (call); \
    if (error != DOCA_SUCCESS) { \
        fprintf(stderr, "%s: %s\n", #call, doca_error_get_descr(error)); \
        goto cleanup; } } while (0)

int main(int argc, char **argv)
{
    if ((argc != 4 && argc != 6) || (argc == 6 && strcmp(argv[4], "--groups")) || strcmp(argv[1], argv[2]) == 0) {
        fprintf(stderr, "Usage: %s PORT0-PCI PORT1-PCI EAL-LCORE-LIST [--groups FILE]\n"
                "Requires 9 available lcores (main + 8 workers), distinct physical devices.\n", argv[0]);
        return 2;
    }
    struct groups *groups = NULL;
    struct reliability reliability;
    bool reliability_enabled = false;
    if (argc == 6) {
        char error[256];
        if (groups_load(argv[5], &groups, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s\n", error);
            return 2;
        }
        printf("Loaded %zu groups / %zu members\n", groups->count, groups->member_count);
        if (reliability_init(&reliability, groups) != 0) {
            free(groups);
            return 2;
        }
        reliability_enabled = true;
        const char *delay = getenv("M4_ACK_DELAY");
        if (delay) {
            size_t member;
            unsigned microseconds;
            char trailing;
            if (sscanf(delay, "%zu:%u%c", &member, &microseconds, &trailing) != 2 ||
                reliability_set_test_delay(&reliability, member, microseconds) != 0) {
                fprintf(stderr, "M4_ACK_DELAY must be MEMBER:MICROSECONDS\n");
                reliability_destroy(&reliability);
                free(groups);
                return 2;
            }
            printf("Milestone 4 test delay: member=%zu microseconds=%u (first ACK only)\n",
                   member, microseconds);
        }
    }
    char *eal[] = {argv[0], "-l", argv[3], "-a", "00:0.0", NULL};
    if (rte_eal_init(5, eal) < 0) {
        if (reliability_enabled) reliability_destroy(&reliability);
        free(groups);
        return 1;
    }
    struct icrc_dpdk *dpdk_icrc = NULL;
    struct icrc_delta *delta_icrc = NULL;
    const struct icrc_backend *icrc = icrc_backend_portable();
    const char *icrc_setting = getenv("NODE_B_ICRC_BACKEND");
    if (!icrc_setting)
        icrc_setting = "dpdk-neon";
    bool incremental = strcmp(icrc_setting, "incremental") == 0 ||
        strcmp(icrc_setting, "incremental-verify") == 0;
    bool verify_delta = strcmp(icrc_setting, "incremental-verify") == 0;
    if (strcmp(icrc_setting, "dpdk-neon") == 0 || incremental) {
        if (icrc_dpdk_create(&dpdk_icrc) == 0)
            icrc = icrc_dpdk_backend(dpdk_icrc);
        else
            fprintf(stderr, "DPDK NEON ICRC unavailable or failed self-test; "
                    "using portable-bitwise\n");
    } else if (strcmp(icrc_setting, "portable") != 0) {
        fprintf(stderr, "NODE_B_ICRC_BACKEND must be dpdk-neon, portable, "
                "incremental-verify, or incremental\n");
        rte_eal_cleanup();
        if (reliability_enabled) reliability_destroy(&reliability);
        free(groups);
        return 2;
    }
    if (incremental && icrc_delta_create(NODE_B_MAX_FRAME, &delta_icrc) != 0) {
        fprintf(stderr, "Cannot initialize incremental ICRC correction table\n");
        icrc_dpdk_destroy(&dpdk_icrc);
        rte_eal_cleanup();
        if (reliability_enabled) reliability_destroy(&reliability);
        free(groups);
        return 1;
    }
    printf("ICRC backend=%s strategy=%s\n", icrc->name,
           incremental ? (verify_delta ? "incremental-verify" : "incremental") :
                         "full-recompute");
    const char *ipv4_udp_setting = getenv("NODE_B_IPV4_UDP_CHECKSUM");
    bool ipv4_udp_checksum_enabled = !ipv4_udp_setting ||
        strcmp(ipv4_udp_setting, "0") != 0;
    printf("IPv4 UDP checksum rewrite=%s\n",
           ipv4_udp_checksum_enabled ? "enabled" : "disabled");
    uint64_t ticks_per_second = rte_get_tsc_hz();
    const char *metrics_setting = getenv("NODE_B_METRICS");
    bool metrics_enabled = !metrics_setting || strcmp(metrics_setting, "0") != 0;
    printf("Metrics %s; tsc_hz=%" PRIu64 "\n",
           metrics_enabled ? "enabled" : "disabled", ticks_per_second);
    struct doca_dev *devices[2] = {0};
    struct flow flows[2] = {0};
    uint16_t ports[2] = {UINT16_MAX, UINT16_MAX};
    struct runner runners[NODE_B_WORKERS] = {0};
    unsigned cores[NODE_B_WORKERS], core, core_count = 0, launched = 0;
    struct rte_mempool *pools[2] = {0};
    bool flow_initialized = false, rx_started[2] = {false, false};
    int result = 1;
    RTE_LCORE_FOREACH_WORKER(core) {
        if (core_count < NODE_B_WORKERS)
            cores[core_count++] = core;
    }
    if (core_count != NODE_B_WORKERS) {
        fprintf(stderr, "Eight worker lcores plus the main lcore are required\n");
        goto cleanup;
    }
    for (unsigned i = 0; i < 2; ++i) {
        DOCA_CHECK(open_device(argv[i + 1], &devices[i]));
        /* NVIDIA DPDK 26.03 defaults to one consecutive allocation for all
         * mlx5 Tx queue memory. The BF3 Arm runtime cannot satisfy that
         * allocation even with a populated hugepage pool; zero selects the
         * driver's supported legacy per-queue allocation path. */
        DOCA_CHECK(doca_dpdk_port_probe(devices[i],
                                       "dv_flow_en=2,txq_mem_algn=0"));
        if (map_port(devices[i], &ports[i]) != 0) {
            fprintf(stderr, "Cannot uniquely map PCI %s to a DPDK port\n", argv[i + 1]);
            goto cleanup;
        }
        printf("port%u PCI=%s DPDK-port=%u\n", i, argv[i + 1], ports[i]);
    }
    if (ports[0] == ports[1])
        goto cleanup;
    const char *pool_names[2] = {"node_b_port0_rx", "node_b_port1_rx"};
    for (unsigned i = 0; i < 2; ++i) {
        pools[i] = rte_pktmbuf_pool_create(pool_names[i], 16383, 256, 0,
                                           RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (!pools[i] || start_rx(ports[i], pools[i]) < 0) {
            fprintf(stderr, "port%u RX pool/queue startup failed\n", i);
            goto cleanup;
        }
        rx_started[i] = true;
    }
    /* Each lcore owns one RX/TX pair in each direction. Group rewriting is
     * limited to port0 -> port1; matching reliable ACKs are aggregated in the
     * reverse direction. TSC reads add no per-packet logging or allocation. */
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        runners[i].forward_port = ports[0];
        runners[i].reverse_port = ports[1];
        DOCA_CHECK(eth_tx_open(devices[1], i, 256, 2048,
                              &runners[i].forward, &runners[i].forward_tx));
        if (worker_set_groups(&runners[i].forward, groups) != 0 ||
            worker_set_icrc_backend(&runners[i].forward, icrc) != 0 ||
            worker_set_ipv4_udp_checksum(&runners[i].forward,
                                         ipv4_udp_checksum_enabled) != 0)
            goto cleanup;
        DOCA_CHECK(eth_tx_open(devices[0], i, 256, 2048,
                              &runners[i].reverse, &runners[i].reverse_tx));
        if (worker_set_icrc_backend(&runners[i].reverse, icrc) != 0 ||
            worker_set_ipv4_udp_checksum(&runners[i].reverse,
                                         ipv4_udp_checksum_enabled) != 0)
            goto cleanup;
        if (delta_icrc &&
            (worker_set_icrc_delta(&runners[i].forward, delta_icrc,
                                   verify_delta) != 0 ||
             worker_set_icrc_delta(&runners[i].reverse, delta_icrc,
                                   verify_delta) != 0))
            goto cleanup;
        if (metrics_enabled) {
            struct worker_clock clock = {read_tsc, NULL};
            if (worker_set_clock(&runners[i].forward, clock) != 0 ||
                worker_set_clock(&runners[i].reverse, clock) != 0)
                goto cleanup;
        }
        if (reliability_enabled) {
            if (worker_set_reliability(&runners[i].forward, &reliability) != 0 ||
                worker_set_reliability(&runners[i].reverse, &reliability) != 0)
                goto cleanup;
        }
    }
    DOCA_CHECK(flow_init());
    flow_initialized = true;
    for (unsigned i = 0; i < 2; ++i)
        DOCA_CHECK(flow_start_port(&flows[i], devices[i], ports[i]));
    /* Workers are initialized before exposing either ingress to its queues. */
    for (unsigned i = 0; i < 2; ++i)
        DOCA_CHECK(flow_steer_roce(&flows[i]));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (; launched < NODE_B_WORKERS; ++launched) {
        if (rte_eal_remote_launch(run_worker, &runners[launched], cores[launched]) != 0)
            goto cleanup;
    }
    puts("Eight bidirectional workers running; forward traffic may rewrite/fan out; matching reverse ACKs aggregate.");
    while (!interrupted && !atomic_load(&stop_requested)) {
        struct timespec interval = {.tv_sec = 1};
        nanosleep(&interval, NULL);
    }
    result = 0;
cleanup:
    atomic_store(&stop_requested, true);
    for (unsigned i = 0; i < launched; ++i)
        if (rte_eal_wait_lcore(cores[i]) != 0)
            result = 1;
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        worker_stop(&runners[i].forward);
        worker_stop(&runners[i].reverse);
        print_worker_stats("forward", i, &runners[i].forward, groups,
                           ticks_per_second);
        print_worker_stats("reverse", i, &runners[i].reverse, NULL,
                           ticks_per_second);
    }
    struct metric_summary icrc_total = {0};
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        metric_merge(&icrc_total, &runners[i].forward.stats.icrc_compute);
        metric_merge(&icrc_total, &runners[i].reverse.stats.icrc_compute);
    }
    print_metric("icrc_compute_total", &icrc_total, ticks_per_second);
    if (reliability_enabled)
        for (size_t g = 0; g < groups->count; ++g) {
            const struct multicast_group *group = &groups->group[g];
            if (!group->reliable)
                continue;
            const struct reliability_group_state *state = &reliability.state[g];
            printf("group=%s ack_received=%" PRIu64 " ack_forwarded=%" PRIu64
                   " ack_suppressed=%" PRIu64 " ack_stale=%" PRIu64
                   " ack_malformed=%" PRIu64 " ack_delayed=%" PRIu64
                   " rtt_overwrites=%" PRIu64 " rtt_missed=%" PRIu64
                   " agg_ack_psn=%u tri_port=%zu\n",
                   group->id, state->counters.received, state->counters.forwarded,
                   state->counters.suppressed, state->counters.stale,
                   state->counters.malformed, state->counters.delayed,
                   state->counters.rtt_overwrites, state->counters.rtt_missed,
                   state->agg_ack_psn, state->tri_port);
            char metric_name[96];
            snprintf(metric_name, sizeof(metric_name), "traffic_rtt_group_%s", group->id);
            print_metric(metric_name, &state->rtt, ticks_per_second);
        }
    for (unsigned i = 0; i < 2; ++i) {
        if (flows[i].rss_entry[0] && flows[i].rss_entry[1]) {
            uint64_t packets, bytes;
            if (flow_read_counter(&flows[i], &packets, &bytes) == DOCA_SUCCESS)
                printf("direction=%s flow_matched_packets=%" PRIu64 " bytes=%" PRIu64 "\n",
                       i ? "reverse" : "forward", packets, bytes);
            else
                result = 1;
        }
    }
    /* On failed drain, keep registered memory and device handles alive until
     * process exit. Do not explicitly tear down resources still used by DMA. */
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        struct eth_tx **contexts[2] = {&runners[i].forward_tx, &runners[i].reverse_tx};
        const char *directions[2] = {"forward", "reverse"};
        for (unsigned d = 0; d < 2; ++d) {
            uint64_t deadline = milliseconds() + 5000;
            doca_error_t error;
            do {
                error = eth_tx_close(contexts[d]);
            } while (error == DOCA_ERROR_IN_PROGRESS && milliseconds() < deadline);
            if (error != DOCA_SUCCESS) {
                fprintf(stderr, "%s TX cleanup failed for queue %u: %s\n",
                        directions[d], i, doca_error_get_descr(error));
                return 1;
            }
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        if (flow_stop_port(&flows[i]) != DOCA_SUCCESS)
            return 1;
    if (flow_initialized)
        flow_destroy();
    for (unsigned i = 0; i < 2; ++i)
        if (rx_started[i] && rte_eth_dev_stop(ports[i]) < 0)
            return 1;
    for (unsigned i = 0; i < 2; ++i)
        if (ports[i] != UINT16_MAX && rte_eth_dev_close(ports[i]) < 0)
            return 1;
    for (unsigned i = 0; i < 2; ++i)
        if (pools[i])
            rte_mempool_free(pools[i]);
    for (unsigned i = 0; i < 2; ++i)
        if (devices[i] && doca_dev_close(devices[i]) != DOCA_SUCCESS)
            return 1;
    icrc_delta_destroy(&delta_icrc);
    icrc_dpdk_destroy(&dpdk_icrc);
    if (rte_eal_cleanup() < 0)
        return 1;
    if (reliability_enabled)
        reliability_destroy(&reliability);
    free(groups);
    return result;
}
