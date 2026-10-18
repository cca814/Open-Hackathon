#ifndef _COMMON_H
#define _COMMON_H

#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>

typedef struct connect_info {
  uint32_t qpn;
  union ibv_gid gid;
} ConnectInfo;

typedef struct rdma_node {
  char *device_name;
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_mr *mr;
  struct ibv_qp *qp;
} RdmaNode;

struct ibv_context *get_dedicate_device_ctx(char *device_name);

void poll_complete(struct ibv_cq *cq);

#endif