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
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <string.h>

static int copy_packet(void *packet, uint8_t *destination, size_t length)
{
    const struct rte_mbuf *head = packet;
    const struct rte_mbuf *segment = head;
    size_t copied = 0;
    /* Bound traversal as well as bytes, including malformed zero-length chains. */
    unsigned segments = 0;
    for (; segments < head->nb_segs && segment; ++segments) {
        size_t n = rte_pktmbuf_data_len(segment);
        if (segment->data_off > segment->buf_len ||
            n > (size_t)(segment->buf_len - segment->data_off) || n > length - copied)
            return -1;
        memcpy(destination + copied, rte_pktmbuf_mtod(segment, const void *), n);
        copied += n;
        segment = segment->next;
    }
    return segment == NULL && segments == head->nb_segs && copied == length ? 0 : -1;
}

static void release_packet(void *packet)
{
    rte_pktmbuf_free(packet);
}

int dpdk_rx_poll(struct worker *worker, uint16_t port_id)
{
    if (!worker || worker_progress(worker) != 0)
        return -1;
    if (worker->stopping)
        return 0;
    struct rte_mbuf *packets[32];
    uint16_t count = rte_eth_rx_burst(port_id, worker->queue_id, packets, 32);
    for (uint16_t i = 0; i < count; ++i) {
        struct worker_rx rx = {packets[i], rte_pktmbuf_pkt_len(packets[i]),
                               copy_packet, release_packet};
        (void)worker_forward(worker, &rx);
    }
    return count;
}
