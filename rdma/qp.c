#include "qp.h"
#include "tcp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>

int side_open(RdmaSide *side, const char *device, int ib_port, int gid_index,
              int cq_depth) {
  *side = (RdmaSide){.ib_port = ib_port, .gid_index = gid_index};

  side->ctx = get_dedicate_device_ctx(device);
  if (side->ctx == NULL) {
    return -1;
  }
  side->pd = ibv_alloc_pd(side->ctx);
  if (side->pd == NULL) {
    perror("ibv_alloc_pd");
    return -1;
  }
  side->cq = ibv_create_cq(side->ctx, cq_depth, NULL, NULL, 0);
  if (side->cq == NULL) {
    perror("ibv_create_cq");
    return -1;
  }
  return 0;
}

int side_register(RdmaSide *side, void *buf, size_t len) {
  side->mr = ibv_reg_mr(side->pd, buf, len, IBV_ACCESS_LOCAL_WRITE);
  if (side->mr == NULL) {
    perror("ibv_reg_mr");
    return -1;
  }
  return 0;
}

struct ibv_qp *qp_create(RdmaSide *side, int max_send_wr, int max_recv_wr) {
  struct ibv_qp_init_attr attr = {.cap = {.max_recv_sge = 1,
                                          .max_send_sge = 1,
                                          .max_recv_wr = max_recv_wr,
                                          .max_send_wr = max_send_wr},
                                  .qp_type = IBV_QPT_RC,
                                  .send_cq = side->cq,
                                  .recv_cq = side->cq};
  struct ibv_qp *qp = ibv_create_qp(side->pd, &attr);
  if (qp == NULL) {
    perror("ibv_create_qp");
    return NULL;
  }

  struct ibv_qp_attr init_attr = {.qp_state = IBV_QPS_INIT,
                                  .pkey_index = 0,
                                  .port_num = side->ib_port,
                                  .qp_access_flags = 0};
  int ret = ibv_modify_qp(qp, &init_attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_ACCESS_FLAGS);
  if (ret != 0) {
    fprintf(stderr, "QP INIT failed: %s\n", strerror(ret));
    ibv_destroy_qp(qp);
    return NULL;
  }
  return qp;
}

int qp_local_info(const RdmaSide *side, const struct ibv_qp *qp,
                  ConnectInfo *local) {
  *local = (ConnectInfo){.qpn = qp->qp_num};
  if (ibv_query_gid(side->ctx, side->ib_port, side->gid_index, &local->gid) != 0) {
    fprintf(stderr, "Failed to query GID at index %d on port %d\n",
            side->gid_index, side->ib_port);
    return -1;
  }
  return 0;
}

int qp_connect(const RdmaSide *side, struct ibv_qp *qp, const ConnectInfo *remote,
               enum ibv_mtu mtu) {
  struct ibv_qp_attr rtr_attr = {.qp_state = IBV_QPS_RTR,
                                 .path_mtu = mtu,
                                 .dest_qp_num = remote->qpn,
                                 .rq_psn = 1,
                                 .max_dest_rd_atomic = 1,
                                 .min_rnr_timer = 12,
                                 .ah_attr = {.is_global = 1,
                                             .port_num = side->ib_port,
                                             .grh = {.dgid = remote->gid,
                                                     .sgid_index = side->gid_index,
                                                     .hop_limit = 64}}};
  int ret = ibv_modify_qp(
      qp, &rtr_attr,
      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (ret != 0) {
    fprintf(stderr, "QP RTR failed: %s\n", strerror(ret));
    return -1;
  }

  // rnr_retry 7 retries forever, so a peer that is slow to post a receive
  // slows the sender down instead of failing the connection.
  struct ibv_qp_attr rts_attr = {.qp_state = IBV_QPS_RTS,
                                 .timeout = 14,
                                 .retry_cnt = 7,
                                 .rnr_retry = 7,
                                 .sq_psn = 1,
                                 .max_rd_atomic = 1};
  ret = ibv_modify_qp(qp, &rts_attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC);
  if (ret != 0) {
    fprintf(stderr, "QP RTS failed: %s\n", strerror(ret));
    return -1;
  }
  return 0;
}

int exchange_info(int fd, const ConnectInfo *local, ConnectInfo *remote) {
  unsigned char data[sizeof(uint32_t) + sizeof(local->gid.raw)];
  uint32_t qpn = htonl(local->qpn);
  memcpy(data, &qpn, sizeof(qpn));
  memcpy(data + sizeof(qpn), local->gid.raw, sizeof(local->gid.raw));
  if (send_connection_data(fd, data, sizeof(data)) != 0 ||
      receive_connection_data(fd, data, sizeof(data)) != 0) {
    fprintf(stderr, "Failed to exchange QPN and GID\n");
    return -1;
  }
  memcpy(&qpn, data, sizeof(qpn));
  remote->qpn = ntohl(qpn);
  memcpy(remote->gid.raw, data + sizeof(qpn), sizeof(remote->gid.raw));
  return 0;
}

int qp_post_recv(struct ibv_qp *qp, uint64_t wr_id, void *addr, uint32_t len,
                 uint32_t lkey) {
  struct ibv_sge sge = {.addr = (uintptr_t)addr, .length = len, .lkey = lkey};
  struct ibv_recv_wr wr = {.wr_id = wr_id, .sg_list = &sge, .num_sge = 1};
  struct ibv_recv_wr *bad_wr = NULL;
  int ret = ibv_post_recv(qp, &wr, &bad_wr);
  if (ret != 0) {
    fprintf(stderr, "ibv_post_recv failed: %s\n", strerror(ret));
    return -1;
  }
  return 0;
}

int qp_post_send(struct ibv_qp *qp, uint64_t wr_id, void *addr, uint32_t len,
                 uint32_t lkey) {
  struct ibv_sge sge = {.addr = (uintptr_t)addr, .length = len, .lkey = lkey};
  struct ibv_send_wr wr = {.wr_id = wr_id,
                           .sg_list = &sge,
                           .num_sge = 1,
                           .opcode = IBV_WR_SEND,
                           .send_flags = IBV_SEND_SIGNALED};
  struct ibv_send_wr *bad_wr = NULL;
  int ret = ibv_post_send(qp, &wr, &bad_wr);
  if (ret != 0) {
    fprintf(stderr, "ibv_post_send failed: %s\n", strerror(ret));
    return -1;
  }
  return 0;
}

int parse_mtu(const char *text, enum ibv_mtu *mtu) {
  unsigned long value;
  if (parse_uint(text, 4096, &value) != 0) {
    return -1;
  }
  switch (value) {
  case 256: *mtu = IBV_MTU_256; return 0;
  case 512: *mtu = IBV_MTU_512; return 0;
  case 1024: *mtu = IBV_MTU_1024; return 0;
  case 2048: *mtu = IBV_MTU_2048; return 0;
  case 4096: *mtu = IBV_MTU_4096; return 0;
  default: return -1;
  }
}

int parse_uint(const char *text, unsigned long max, unsigned long *value) {
  char *end;
  errno = 0;
  unsigned long number = strtoul(text, &end, 10);
  if (errno || *text == '-' || end == text || *end != '\0' || number > max) {
    return -1;
  }
  *value = number;
  return 0;
}
