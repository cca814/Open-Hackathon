// Multicast sender: connects to the relay (or to a single sink) and sends
// --count messages, keeping up to --window of them in flight.

#include "payload.h"
#include "qp.h"
#include "tcp.h"
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define POLL_BATCH 32
#define DEFAULT_PORT "10001"

typedef struct source_options {
  const char *device;
  const char *server_ip;
  int ib_port;
  int gid_index;
  char tcp_port[8];
  unsigned long count, size, window;
  enum ibv_mtu mtu;
} SourceOptions;

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s --device <dev> --gid-index <index> --server-ip <relay> [options]\n"
          "  --ib-port    RDMA port number (default 1)\n"
          "  --port       the relay's TCP port (default %s)\n"
          "  --count      messages to send (default 1)\n"
          "  --size       bytes per message, at least %d (default 64)\n"
          "  --window     messages in flight at once (default 16)\n"
          "  --mtu        path MTU: 256..4096 (default 1024)\n"
          "The size must not exceed the relay's and the sinks' --msg-size.\n",
          program, DEFAULT_PORT, PAYLOAD_MIN_LEN);
}

static int parse_options(int argc, char **argv, SourceOptions *options) {
  static const struct option flags[] = {
      {"device", required_argument, NULL, 'd'},
      {"ib-port", required_argument, NULL, 'p'},
      {"gid-index", required_argument, NULL, 'g'},
      {"server-ip", required_argument, NULL, 'i'},
      {"port", required_argument, NULL, 'l'},
      {"count", required_argument, NULL, 'c'},
      {"size", required_argument, NULL, 's'},
      {"window", required_argument, NULL, 'w'},
      {"mtu", required_argument, NULL, 'm'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  *options = (SourceOptions){.ib_port = 1,
                             .gid_index = -1,
                             .count = 1,
                             .size = 64,
                             .window = 16,
                             .mtu = IBV_MTU_1024};
  snprintf(options->tcp_port, sizeof(options->tcp_port), "%s", DEFAULT_PORT);

  int flag;
  while ((flag = getopt_long(argc, argv, "h", flags, NULL)) != -1) {
    unsigned long value = 0;
    int ok = 1;
    switch (flag) {
    case 'd': options->device = optarg; break;
    case 'i': options->server_ip = optarg; break;
    case 'p':
      ok = parse_uint(optarg, 255, &value) == 0 && value >= 1;
      options->ib_port = (int)value;
      break;
    case 'g':
      ok = parse_uint(optarg, 255, &value) == 0;
      options->gid_index = (int)value;
      break;
    case 'l':
      ok = parse_uint(optarg, 65535, &value) == 0 && value >= 1;
      snprintf(options->tcp_port, sizeof(options->tcp_port), "%lu", value);
      break;
    case 'c':
      ok = parse_uint(optarg, ULONG_MAX, &options->count) == 0 && options->count >= 1;
      break;
    case 's':
      ok = parse_uint(optarg, 1UL << 30, &options->size) == 0 &&
           options->size >= PAYLOAD_MIN_LEN;
      break;
    case 'w':
      ok = parse_uint(optarg, 65535, &options->window) == 0 && options->window >= 1;
      break;
    case 'm': ok = parse_mtu(optarg, &options->mtu) == 0; break;
    case 'h': print_usage(stdout, argv[0]); return 1;
    default: print_usage(stderr, argv[0]); return -1;
    }
    if (!ok) {
      fprintf(stderr, "Invalid value for option '%c': %s\n", flag,
              optarg != NULL ? optarg : "");
      return -1;
    }
  }
  if (optind != argc || options->device == NULL || options->gid_index < 0 ||
      options->server_ip == NULL) {
    print_usage(stderr, argv[0]);
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  SourceOptions options;
  int parsed = parse_options(argc, argv, &options);
  if (parsed != 0) {
    return parsed < 0 ? 1 : 0;
  }
  const int window = (int)options.window;
  const uint32_t size = (uint32_t)options.size;

  size_t buffer_bytes = (size_t)window * size;
  uint8_t *buffers;
  if (posix_memalign((void **)&buffers, 4096, buffer_bytes) != 0) {
    fprintf(stderr, "Cannot allocate %zu bytes for the send buffers\n", buffer_bytes);
    return 1;
  }

  RdmaSide side;
  if (side_open(&side, options.device, options.ib_port, options.gid_index,
                window + 1) != 0 ||
      side_register(&side, buffers, buffer_bytes) != 0) {
    return 1;
  }
  struct ibv_qp *qp = qp_create(&side, window, 1);
  ConnectInfo local, remote;
  if (qp == NULL || qp_local_info(&side, qp, &local) != 0) {
    return 1;
  }

  int fd = connect_server(options.server_ip, options.tcp_port);
  if (fd < 0) {
    return 1;
  }
  int exchanged = exchange_info(fd, &local, &remote);
  close(fd);
  if (exchanged != 0 || qp_connect(&side, qp, &remote, options.mtu) != 0) {
    return 1;
  }
  printf("QP ready: local QPN %u, remote QPN %u\n", local.qpn, remote.qpn);
  fflush(stdout);

  // The completion of a slot frees it for the next message.
  uint64_t posted = 0, completed = 0, bytes = 0;
  double start = now();
  for (int slot = 0; slot < window && posted < options.count; slot++, posted++) {
    uint8_t *buf = buffers + (size_t)slot * size;
    payload_fill(buf, size, posted);
    if (qp_post_send(qp, (uint64_t)slot, buf, size, side.mr->lkey) != 0) {
      return 1;
    }
  }

  struct ibv_wc wc[POLL_BATCH];
  while (completed < options.count) {
    int n = ibv_poll_cq(side.cq, POLL_BATCH, wc);
    if (n < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Send failed: %s\n", ibv_wc_status_str(wc[i].status));
        return 1;
      }
      completed++;
      bytes += size;
      if (posted < options.count) {
        uint32_t slot = (uint32_t)wc[i].wr_id;
        uint8_t *buf = buffers + (size_t)slot * size;
        payload_fill(buf, size, posted);
        if (qp_post_send(qp, slot, buf, size, side.mr->lkey) != 0) {
          return 1;
        }
        posted++;
      }
    }
  }

  double elapsed = now() - start;
  printf("SEND completed: %llu messages, %llu bytes in %.3f s: %.2f Gbps\n",
         (unsigned long long)completed, (unsigned long long)bytes, elapsed,
         elapsed > 0 ? (double)bytes * 8 / elapsed / 1e9 : 0.0);
  return 0;
}
