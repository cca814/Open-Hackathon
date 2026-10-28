#include "common.h"
#include "tcp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  RdmaOptions options;
  int parsed = parse_rdma_options(argc, argv, 0, &options);
  if (parsed != 0) {
    return parsed < 0 ? 1 : 0;
  }
  const char *device_name = options.device_name;
  int gid_index = options.gid_index;
  PeerConfig config = {.sq_psn = 1, .rq_psn = 1};
  if (options.config_file != NULL && load_peer_config(options.config_file, &config) != 0) {
    return 1;
  }

  RdmaNode node;

  node.ctx = get_dedicate_device_ctx(device_name);

  if (node.ctx == NULL) {
    perror("get_dedicate_device_ctx");
    return 1;
  }

  // 3. create protected domain.
  node.pd = ibv_alloc_pd(node.ctx);
  if (node.pd == NULL) {
    perror("ibv_alloc_pd");
    return 1;
  }

  // 4. create completion queue.
  node.cq = ibv_create_cq(node.ctx, 16, NULL, NULL, 0);
  if (node.cq == NULL) {
    perror("ibv_create_cq");
    return 1;
  }

  // 5. register memory region.
  char *buffer = malloc(4096);
  if (buffer == NULL) {
    perror("malloc");
    return 1;
  }
  // Enable local NIC to write into this mr.
  node.mr = ibv_reg_mr(node.pd, buffer, 4096, IBV_ACCESS_LOCAL_WRITE);
  if (node.mr == NULL) {
    perror("ibv_reg_mr");
    return 1;
  }

  // 6. Create queue pair.
  struct ibv_qp_init_attr attr = {.qp_context = node.ctx,
                                  .cap = {.max_recv_sge = 1,
                                          .max_send_sge = 1,
                                          .max_recv_wr = 16,
                                          .max_send_wr = 16},
                                  .qp_type = IBV_QPT_RC,
                                  .send_cq = node.cq,
                                  .recv_cq = node.cq};

  node.qp = ibv_create_qp(node.pd, &attr);
  if (node.qp == NULL) {
    perror("ibv_create_qp");
    return 1;
  }

  // QP state transition: RESET -> INIT -> RTR -> RTS.

  struct ibv_qp_attr init_attr = {.qp_state = IBV_QPS_INIT,
                                  .pkey_index = 0,
                                  .port_num = 1,
                                  .qp_access_flags = 0};

  int init_ret = ibv_modify_qp(node.qp, &init_attr,
                               IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                                   IBV_QP_ACCESS_FLAGS);
  if (init_ret != 0) {
    fprintf(stderr, "QP INIT failed: %s\n", strerror(init_ret));
    return 1;
  }

  // Preconfiguration supplies P4's virtual peer; TCP supplies the real server.
  ConnectInfo local = {.qpn = node.qp->qp_num}, remote = config.remote;
  if (ibv_query_gid(node.ctx, 1, gid_index, &local.gid) != 0) {
    fprintf(stderr, "Failed to query GID at index %d\n", gid_index);
    return 1;
  }

  if (options.server_ip != NULL) {
    int fd = connect_server(options.server_ip, "10001");
    if (fd < 0) {
      return 1;
    }
    // Original wire format: 4-byte QPN in network order + 16-byte GID.
    uint32_t qpn = htonl(local.qpn);
    unsigned char data[sizeof(qpn) + sizeof(local.gid.raw)];
    memcpy(data, &qpn, sizeof(qpn));
    memcpy(data + sizeof(qpn), local.gid.raw, sizeof(local.gid.raw));
    if (send_connection_data(fd, data, sizeof(data)) != 0 ||
        receive_connection_data(fd, data, sizeof(data)) != 0) {
      close(fd);
      fprintf(stderr, "Failed to exchange QPN and GID\n");
      return 1;
    }
    close(fd);
    memcpy(&qpn, data, sizeof(qpn));
    remote.qpn = ntohl(qpn);
    memcpy(remote.gid.raw, data + sizeof(qpn), sizeof(remote.gid.raw));
  }

  struct ibv_qp_attr rtr_attr = {.qp_state = IBV_QPS_RTR,
                                 .path_mtu = IBV_MTU_1024,
                                 .dest_qp_num = remote.qpn,
                                 .rq_psn = config.rq_psn,

                                 .max_dest_rd_atomic = 1,
                                 .min_rnr_timer = 12,

                                 .ah_attr = {.is_global = 1,
                                             .port_num = 1,
                                             .grh = {.dgid = remote.gid,
                                                     .sgid_index = gid_index,
                                                     .hop_limit = 64}}};

  int rtr_ret = ibv_modify_qp(
      node.qp, &rtr_attr,
      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
  if (rtr_ret != 0) {
    fprintf(stderr, "QP RTR failed: %s\n", strerror(rtr_ret));
    return 1;
  }

  struct ibv_qp_attr rts_attr = {.qp_state = IBV_QPS_RTS,

                                 .timeout = 14,
                                 .retry_cnt = 7,
                                 .rnr_retry = 7,

                                 .sq_psn = config.sq_psn,
                                 .max_rd_atomic = 1};

  int rts_ret = ibv_modify_qp(node.qp, &rts_attr,
                              IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                                  IBV_QP_MAX_QP_RD_ATOMIC);
  if (rts_ret != 0) {
    fprintf(stderr, "QP RTS failed: %s\n", strerror(rts_ret));
    return 1;
  }

  printf("QP ready: local QPN %u, remote QPN %u\n", local.qpn, remote.qpn);

  // Prepare receive buffer.
  struct ibv_sge sge = {
      .addr = (uintptr_t)buffer, .length = 4096, .lkey = node.mr->lkey};

  struct ibv_recv_wr recv_wr = {.wr_id = 1, .sg_list = &sge, .num_sge = 1};

  struct ibv_recv_wr *bad_wr = NULL;

  // Post RECV wr
  int ret = ibv_post_recv(node.qp, &recv_wr, &bad_wr);
  if (ret != 0) {
    fprintf(stderr, "ibv_post_recv failed: %s\n", strerror(ret));
    return 1;
  }
  // Publish only after the receive buffer is posted, so the server can send.
  if (options.config_file != NULL) {
    if (write_connection_info(options.info_file, &local, &config) != 0) {
      return 1;
    }
  }
  printf("Receive buffer ready; waiting for the server.\n");
  fflush(stdout);

  poll_complete(node.cq);

  printf("Received data: %.4096s\n", buffer);

  return 0;
}
