#ifndef NODE_B_FLOW_H
#define NODE_B_FLOW_H
#include <stdbool.h>
#include <stdint.h>
#include <doca_dev.h>
#include <doca_flow.h>

struct flow {
    struct doca_flow_port *port;
    struct doca_flow_pipe *rss_pipe[2];
    struct doca_flow_pipe_entry *rss_entry[2];
    struct doca_flow_pipe *control_pipe;
    struct doca_flow_pipe_entry *control_entry[2];
    unsigned processed;
    bool failed;
};
/* One global Flow engine for both ingress DPDK and egress ETH TX.
 * Caller must first establish the SDK-supported DPDK/device mapping. */
doca_error_t flow_init(void);
doca_error_t flow_start_port(struct flow *flow, struct doca_dev *dev, uint16_t port_id);
/* Installs IPv4/IPv6 UDP/4791 -> queues 0..7, with unmatched traffic dropped.
 * A single root control pipe selects one of two non-root RSS pipes by L3 family.
 * Called on each ingress after all eight RX queues on that port are ready. */
doca_error_t flow_steer_roce(struct flow *flow);
doca_error_t flow_read_counter(struct flow *flow, uint64_t *packets, uint64_t *bytes);
doca_error_t flow_stop_port(struct flow *flow);
/* After both ports have stopped. */
void flow_destroy(void);
#endif
