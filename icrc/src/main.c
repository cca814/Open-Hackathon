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

#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <doca_dpdk.h>

static volatile sig_atomic_t interrupted;
static atomic_bool stop_requested;

struct runner {
    struct worker worker;
    struct eth_tx *tx;
    uint16_t port;
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

static int run_worker(void *argument)
{
    struct runner *runner = argument;
    int failed = 0;
    while (!atomic_load(&stop_requested)) {
        if (dpdk_rx_poll(&runner->worker, runner->port) < 0) {
            failed = 1;
            atomic_store(&stop_requested, true);
            break;
        }
    }
    worker_stop(&runner->worker);
    uint64_t deadline = milliseconds() + 5000;
    while (!worker_drained(&runner->worker)) {
        if (worker_progress(&runner->worker) != 0 || milliseconds() >= deadline)
            return -1;
    }
    return failed ? -1 : 0;
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
    config.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP;
    if ((info.flow_type_rss_offloads & config.rx_adv_conf.rss_conf.rss_hf) == 0)
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
    if (argc != 4 || strcmp(argv[1], argv[2]) == 0) {
        fprintf(stderr, "Usage: %s RX-PCI TX-PCI EAL-LCORE-LIST\n"
                "Requires 9 available lcores (main + 8 workers), distinct physical devices.\n", argv[0]);
        return 2;
    }
    char *eal[] = {argv[0], "-l", argv[3], "-a", "pci:00:00.0", NULL};
    if (rte_eal_init(5, eal) < 0)
        return 1;
    struct doca_dev *devices[2] = {0};
    struct flow flows[2] = {0};
    uint16_t ports[2] = {UINT16_MAX, UINT16_MAX};
    struct runner runners[NODE_B_WORKERS] = {0};
    unsigned cores[NODE_B_WORKERS], core, core_count = 0, launched = 0;
    struct rte_mempool *pool = NULL;
    bool flow_initialized = false, rx_started = false;
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
        DOCA_CHECK(doca_dpdk_port_probe(devices[i], "dv_flow_en=2"));
        if (map_port(devices[i], &ports[i]) != 0) {
            fprintf(stderr, "Cannot uniquely map PCI %s to a DPDK port\n", argv[i + 1]);
            goto cleanup;
        }
        printf("%s PCI=%s DPDK-port=%u\n", i ? "TX" : "RX", argv[i + 1], ports[i]);
    }
    if (ports[0] == ports[1])
        goto cleanup;
    pool = rte_pktmbuf_pool_create("node_b_rx", 16383, 256, 0,
                                  RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!pool || start_rx(ports[0], pool) < 0) {
        fprintf(stderr, "RX pool/queue startup failed\n");
        goto cleanup;
    }
    rx_started = true;
    /* Egress uses ETH TX queues only; never create DPDK TX queues there. */
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        runners[i].port = ports[0];
        DOCA_CHECK(eth_tx_open(devices[1], i, 256, 2048,
                              &runners[i].worker, &runners[i].tx));
    }
    DOCA_CHECK(flow_init());
    flow_initialized = true;
    for (unsigned i = 0; i < 2; ++i)
        DOCA_CHECK(flow_start_port(&flows[i], devices[i], ports[i]));
    /* Workers are initialized before exposing ingress to their queues. */
    DOCA_CHECK(flow_steer_roce(&flows[0]));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (; launched < NODE_B_WORKERS; ++launched) {
        if (rte_eal_remote_launch(run_worker, &runners[launched], cores[launched]) != 0)
            goto cleanup;
    }
    puts("Eight workers running; Ctrl-C stops RX and drains TX.");
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
        worker_stop(&runners[i].worker);
        const struct worker_stats *s = &runners[i].worker.stats;
        printf("queue=%u rx=%" PRIu64 " submitted=%" PRIu64 " completed=%" PRIu64
               " tx_errors=%" PRIu64 " drops=%" PRIu64 " pending=%zu\n", i,
               s->rx_packets, s->tx_submitted, s->tx_completed, s->tx_errors,
               s->drop_full + s->drop_copy + s->drop_oversize + s->drop_submit + s->drop_stopping,
               runners[i].worker.pending);
    }
    if (flows[0].entry) {
        uint64_t packets, bytes;
        if (flow_read_counter(&flows[0], &packets, &bytes) == DOCA_SUCCESS)
            printf("Flow matched packets=%" PRIu64 " bytes=%" PRIu64 "\n", packets, bytes);
        else
            result = 1;
    }
    /* On failed drain, keep registered memory and device handles alive until
     * process exit. Do not explicitly tear down resources still used by DMA. */
    for (unsigned i = 0; i < NODE_B_WORKERS; ++i) {
        uint64_t deadline = milliseconds() + 5000;
        doca_error_t error;
        do {
            error = eth_tx_close(&runners[i].tx);
        } while (error == DOCA_ERROR_IN_PROGRESS && milliseconds() < deadline);
        if (error != DOCA_SUCCESS) {
            fprintf(stderr, "TX cleanup failed for queue %u: %s\n", i, doca_error_get_descr(error));
            return 1;
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        if (flow_stop_port(&flows[i]) != DOCA_SUCCESS)
            return 1;
    if (flow_initialized)
        flow_destroy();
    if (rx_started && rte_eth_dev_stop(ports[0]) < 0)
        return 1;
    for (unsigned i = 0; i < 2; ++i)
        if (ports[i] != UINT16_MAX && rte_eth_dev_close(ports[i]) < 0)
            return 1;
    if (pool)
        rte_mempool_free(pool);
    for (unsigned i = 0; i < 2; ++i)
        if (devices[i] && doca_dev_close(devices[i]) != DOCA_SUCCESS)
            return 1;
    if (rte_eal_cleanup() < 0)
        return 1;
    return result;
}
