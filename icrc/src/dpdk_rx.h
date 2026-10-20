#ifndef NODE_B_DPDK_RX_H
#define NODE_B_DPDK_RX_H
#include "worker.h"
/* Called only by the owner of worker->queue_id after port/queue startup.
 * Progresses TX even when RX is idle; returns received count or -1 on error. */
int dpdk_rx_poll(struct worker *worker, uint16_t port_id);
#endif
