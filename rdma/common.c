#include "common.h"
#include <stdlib.h>

struct ibv_context *get_dedicate_device_ctx(char *device_name) {
  int number_of_device;
  struct ibv_device *device = NULL;
  struct ibv_device **devices = ibv_get_device_list(&number_of_device);

  for (int i = 0; i < number_of_device; i++) {
    if (strcmp(devices[i]->name, device_name) == 0) {
      device = devices[i];
      break;
    }
  }

  if (device == NULL) {
    fprintf(stderr, "RDMA device not found: %s\n", device_name);
    return NULL;
  }

  // 2. Create context.
  struct ibv_context *ctx = ibv_open_device(device);
  if (ctx == NULL) {
    perror("ibv_open_device");
    return NULL;
  }

  return ctx;
}

void poll_complete(struct ibv_cq *cq) {
  struct ibv_wc wc;

  while (1) {
    int n = ibv_poll_cq(cq, 1, &wc);

    if (n < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      exit(1);
    }

    if (n == 0) {
      continue;
    }

    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "RDMA operation failed: %s\n",
              ibv_wc_status_str(wc.status));
      exit(1);
    }

    printf("WR %lu completed, opcode=%d\n", (unsigned long)wc.wr_id, wc.opcode);

    return;
  }
}