#ifndef NODE_B_ETH_TX_H
#define NODE_B_ETH_TX_H
#include "worker.h"
#include <doca_dev.h>
#include <doca_error.h>

struct eth_tx;
/* Borrows dev; one queue and PE per worker. Output may remain non-NULL on
 * partial initialization failure so the caller can retry eth_tx_close. */
doca_error_t eth_tx_open(struct doca_dev *dev, unsigned queue_id,
                         unsigned slots, size_t frame_capacity,
                         struct worker *worker, struct eth_tx **out);
/* Stop worker first and keep progressing until drained. On cleanup failure,
 * retains live resources and *tx for retry; never frees DMA-active memory. */
doca_error_t eth_tx_close(struct eth_tx **tx);
#endif
