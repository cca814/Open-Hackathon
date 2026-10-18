#ifndef _COMMON_H
#define _COMMON_H

#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct connect_info {
  uint32_t qpn;
  union ibv_gid gid;
} ConnectInfo;

typedef struct peer_config {
  ConnectInfo remote;
  uint32_t sq_psn;
  uint32_t rq_psn;
  uint32_t switch_port;
  char local_mac[18];
  char remote_mac[18];
} PeerConfig;

typedef struct rdma_options {
  const char *device_name;
  int gid_index;
  const char *server_ip;
  const char *config_file;
  const char *info_file;
} RdmaOptions;

typedef struct rdma_node {
  char *device_name;
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_mr *mr;
  struct ibv_qp *qp;
} RdmaNode;

struct ibv_context *get_dedicate_device_ctx(const char *device_name);

// Returns 0 on success, 1 for --help, and -1 for invalid arguments.
int parse_rdma_options(int argc, char **argv, int is_server, RdmaOptions *options);

void poll_complete(struct ibv_cq *cq);

int load_peer_config(const char *path, PeerConfig *config);
int write_connection_info(const char *path, const ConnectInfo *local,
                          const PeerConfig *config);

#endif
