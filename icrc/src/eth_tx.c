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

#include "eth_tx.h"

#include <stdlib.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_mmap.h>
#include <doca_pe.h>

struct eth_tx {
    struct doca_eth_txq *queue;
    struct doca_ctx *ctx;
    struct doca_pe *pe;
    struct doca_mmap *map;
    struct doca_buf_inventory *inventory;
    struct worker *worker;
    struct worker_slot *slots;
    uint8_t *memory;
    bool worker_initialized;
    doca_error_t callback_error;
};

static void completed(struct doca_eth_txq_task_send *send,
                      union doca_data task_data, union doca_data context_data)
{
    struct eth_tx *tx = context_data.ptr;
    size_t index = task_data.u64;
    struct doca_task *task = doca_eth_txq_task_send_as_doca_task(send);
    bool success = doca_task_get_status(task) == DOCA_SUCCESS;
    struct doca_buf *buf = NULL;
    doca_error_t status = doca_eth_txq_task_send_get_pkt(send, &buf);
    if (status == DOCA_SUCCESS)
        status = doca_buf_dec_refcount(buf, NULL);
    doca_task_free(task);
    if (status != DOCA_SUCCESS) {
        tx->callback_error = status;
        /* Keep slot pinned: teardown must not guess whether references remain. */
        return;
    }
    if (index >= tx->worker->slot_count ||
        worker_complete(tx->worker, index, tx->slots[index].generation, success) != 0)
        tx->callback_error = DOCA_ERROR_BAD_STATE;
}

static int progress(void *context)
{
    struct eth_tx *tx = context;
    (void)doca_pe_progress(tx->pe);
    return tx->callback_error == DOCA_SUCCESS ? 0 : -1;
}

static int submit(void *context, size_t index, uint64_t generation,
                  const uint8_t *data, size_t length)
{
    struct eth_tx *tx = context;
    struct doca_buf *buf = NULL;
    struct doca_eth_txq_task_send *send = NULL;
    union doca_data user_data = {.u64 = index};
    (void)generation; /* One task per busy slot; callback occurs before reuse. */
    doca_error_t status = doca_buf_inventory_buf_get_by_data(
        tx->inventory, tx->map, (void *)data, length, &buf);
    if (status != DOCA_SUCCESS)
        return WORKER_TX_ERROR;
    status = doca_eth_txq_task_send_allocate_init(tx->queue, buf, user_data, &send);
    if (status == DOCA_SUCCESS) {
        status = doca_task_submit(doca_eth_txq_task_send_as_doca_task(send));
        if (status == DOCA_SUCCESS)
            return WORKER_SUBMITTED;
        doca_task_free(doca_eth_txq_task_send_as_doca_task(send));
    }
    doca_error_t released = doca_buf_dec_refcount(buf, NULL);
    if (released != DOCA_SUCCESS)
        tx->callback_error = released;
    return status == DOCA_ERROR_AGAIN ? WORKER_TX_FULL : WORKER_TX_ERROR;
}

#define TRY(call) do { status = (call); if (status != DOCA_SUCCESS) return status; } while (0)

doca_error_t eth_tx_open(struct doca_dev *dev, unsigned queue_id,
                         unsigned count, size_t capacity,
                         struct worker *worker, struct eth_tx **out)
{
    if (!dev || !worker || !out || !count || !capacity ||
        queue_id >= NODE_B_WORKERS || capacity > SIZE_MAX / count)
        return DOCA_ERROR_INVALID_VALUE;
    *out = NULL;
    uint32_t max_burst;
    doca_error_t status;
    TRY(doca_eth_txq_cap_get_max_burst_size(doca_dev_as_devinfo(dev), 1, 0, &max_burst));
    if (count > max_burst)
        return DOCA_ERROR_NOT_SUPPORTED;
    TRY(doca_eth_txq_cap_is_type_supported(doca_dev_as_devinfo(dev),
          DOCA_ETH_TXQ_TYPE_REGULAR, DOCA_ETH_TXQ_DATA_PATH_TYPE_CPU));
    struct eth_tx *tx = calloc(1, sizeof(*tx));
    if (!tx)
        return DOCA_ERROR_NO_MEMORY;
    *out = tx;
    tx->worker = worker;
    tx->memory = malloc(count * capacity);
    tx->slots = calloc(count, sizeof(*tx->slots));
    if (!tx->memory || !tx->slots)
        return DOCA_ERROR_NO_MEMORY;
    TRY(doca_mmap_create(&tx->map));
    TRY(doca_mmap_add_dev(tx->map, dev));
    TRY(doca_mmap_set_memrange(tx->map, tx->memory, count * capacity));
    TRY(doca_mmap_start(tx->map));
    TRY(doca_buf_inventory_create(count, &tx->inventory));
    TRY(doca_buf_inventory_start(tx->inventory));
    TRY(doca_pe_create(&tx->pe));
    TRY(doca_eth_txq_create(dev, count, &tx->queue));
    TRY(doca_eth_txq_set_type(tx->queue, DOCA_ETH_TXQ_TYPE_REGULAR));
    TRY(doca_eth_txq_task_send_set_conf(tx->queue, completed, completed, count));
    tx->ctx = doca_eth_txq_as_doca_ctx(tx->queue);
    if (!tx->ctx)
        return DOCA_ERROR_BAD_STATE;
    TRY(doca_pe_connect_ctx(tx->pe, tx->ctx));
    union doca_data user_data = {.ptr = tx};
    TRY(doca_ctx_set_user_data(tx->ctx, user_data));
    TRY(doca_ctx_start(tx->ctx));
    TRY(doca_eth_txq_apply_queue_id(tx->queue, queue_id));
    for (unsigned i = 0; i < count; ++i)
        tx->slots[i] = (struct worker_slot){.data = tx->memory + i * capacity,
                                          .capacity = capacity};
    struct worker_tx backend = {tx, submit, progress};
    if (worker_init(worker, queue_id, tx->slots, count, backend) != 0)
        return DOCA_ERROR_INVALID_VALUE;
    tx->worker_initialized = true;
    return DOCA_SUCCESS;
}

doca_error_t eth_tx_close(struct eth_tx **handle)
{
    if (!handle || !*handle)
        return DOCA_SUCCESS;
    struct eth_tx *tx = *handle;
    doca_error_t status;
    if (tx->worker_initialized && !worker_drained(tx->worker))
        return DOCA_ERROR_IN_PROGRESS;
    if (tx->callback_error != DOCA_SUCCESS)
        return tx->callback_error;
    if (tx->ctx) {
        enum doca_ctx_states state;
        TRY(doca_ctx_get_state(tx->ctx, &state));
        if (state != DOCA_CTX_STATE_IDLE) {
            status = doca_ctx_stop(tx->ctx);
            if (status != DOCA_SUCCESS && status != DOCA_ERROR_IN_PROGRESS)
                return status;
            (void)doca_pe_progress(tx->pe);
            TRY(doca_ctx_get_state(tx->ctx, &state));
            if (state != DOCA_CTX_STATE_IDLE)
                return DOCA_ERROR_IN_PROGRESS;
        }
    }
    if (tx->queue) {
        TRY(doca_eth_txq_destroy(tx->queue));
        tx->queue = NULL;
        tx->ctx = NULL;
    }
    if (tx->pe) { TRY(doca_pe_destroy(tx->pe)); tx->pe = NULL; }
    if (tx->inventory) {
        TRY(doca_buf_inventory_destroy(tx->inventory));
        tx->inventory = NULL;
    }
    if (tx->map) { TRY(doca_mmap_destroy(tx->map)); tx->map = NULL; }
    if (tx->worker_initialized)
        *tx->worker = (struct worker){0};
    free(tx->slots);
    free(tx->memory);
    free(tx);
    *handle = NULL;
    return DOCA_SUCCESS;
}
