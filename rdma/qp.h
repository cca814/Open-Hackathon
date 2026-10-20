#ifndef _QP_H
#define _QP_H

#include "common.h"

// One RDMA port of one device: the context, protection domain, completion queue
// and memory region that the QPs on that port share.
typedef struct rdma_side {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_mr *mr;
  int ib_port;
  int gid_index;
} RdmaSide;

int side_open(RdmaSide *side, const char *device, int ib_port, int gid_index,
              int cq_depth);

// Registers `buf` for local writes; the send path reads through the same key.
int side_register(RdmaSide *side, void *buf, size_t len);

// Creates an RC QP on side->cq and moves it to INIT. Receive buffers can be
// posted from here on, before the peer is known.
struct ibv_qp *qp_create(RdmaSide *side, int max_send_wr, int max_recv_wr);

int qp_local_info(const RdmaSide *side, const struct ibv_qp *qp,
                  ConnectInfo *local);

// RTR then RTS, with PSN 1 on both directions like client.c and server.c.
int qp_connect(const RdmaSide *side, struct ibv_qp *qp, const ConnectInfo *remote,
               enum ibv_mtu mtu);

// Same wire format as client.c and server.c: 4-byte QPN in network order, then
// the 16-byte GID. Both ends send first, then receive.
int exchange_info(int fd, const ConnectInfo *local, ConnectInfo *remote);

int qp_post_recv(struct ibv_qp *qp, uint64_t wr_id, void *addr, uint32_t len,
                 uint32_t lkey);
int qp_post_send(struct ibv_qp *qp, uint64_t wr_id, void *addr, uint32_t len,
                 uint32_t lkey);

// Accepts 256, 512, 1024, 2048 or 4096.
int parse_mtu(const char *text, enum ibv_mtu *mtu);

// Parses a decimal number no larger than `max`.
int parse_uint(const char *text, unsigned long max, unsigned long *value);

#endif
