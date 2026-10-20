// Baseline: the DPU is an RDMA endpoint. It terminates the sender's RC
// connection, then forwards every message it receives to each target over a
// connection of its own. The NIC handles ICRC, ACKs and retransmission on both
// sides; this program only moves completions between the two.
//
//   source ──QP_in──▶ relay ──QP_out[0]──▶ sink 0
//                           ├─QP_out[1]──▶ sink 1
//                           └─QP_out[2]──▶ sink 2

#include "qp.h"
#include "tcp.h"
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MAX_TARGETS 16
#define POLL_BATCH 64
#define DEFAULT_PORT "10001"

typedef struct target {
  char host[64];
  char port[8];
  struct ibv_qp *qp;
} Target;

typedef struct relay_options {
  const char *in_device;
  const char *out_device;
  int in_port, out_port;
  int in_gid, out_gid;
  char listen_port[8];
  unsigned long pool, msg_size, count;
  enum ibv_mtu mtu;
  int stats;
  Target targets[MAX_TARGETS];
  int ntargets;
} RelayOptions;

static volatile sig_atomic_t stop_requested;

static void handle_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s --in-device <dev> --in-gid <index> --target <host[:port]> ...\n"
          "  --in-device    RDMA device facing the sender\n"
          "  --in-port      its port number (default 1)\n"
          "  --in-gid       its GID index\n"
          "  --out-device   RDMA device facing the targets (default: --in-device)\n"
          "  --out-port     its port number (default: --in-port)\n"
          "  --out-gid      its GID index (default: --in-gid)\n"
          "  --target       a sink's TCP address, repeat once per target (max %d)\n"
          "  --listen-port  TCP port the sender connects to (default %s)\n"
          "  --pool         receive buffers (default 16)\n"
          "  --msg-size     bytes per buffer, the largest message (default 4096)\n"
          "  --mtu          path MTU: 256..4096 (default 1024)\n"
          "  --count        stop after forwarding this many messages (default: run until Ctrl+C)\n"
          "  --stats        print throughput once a second\n",
          program, MAX_TARGETS, DEFAULT_PORT);
}

static int parse_target(const char *text, Target *target) {
  const char *colon = strrchr(text, ':');
  size_t host_len = colon != NULL ? (size_t)(colon - text) : strlen(text);
  unsigned long port = 0;
  if (host_len == 0 || host_len >= sizeof(target->host) ||
      (colon != NULL && parse_uint(colon + 1, 65535, &port) != 0)) {
    return -1;
  }
  memcpy(target->host, text, host_len);
  target->host[host_len] = '\0';
  if (colon != NULL) {
    snprintf(target->port, sizeof(target->port), "%lu", port);
  } else {
    snprintf(target->port, sizeof(target->port), "%s", DEFAULT_PORT);
  }
  return 0;
}

static int parse_options(int argc, char **argv, RelayOptions *options) {
  static const struct option flags[] = {
      {"in-device", required_argument, NULL, 'd'},
      {"in-port", required_argument, NULL, 'p'},
      {"in-gid", required_argument, NULL, 'g'},
      {"out-device", required_argument, NULL, 'D'},
      {"out-port", required_argument, NULL, 'P'},
      {"out-gid", required_argument, NULL, 'G'},
      {"target", required_argument, NULL, 't'},
      {"listen-port", required_argument, NULL, 'l'},
      {"pool", required_argument, NULL, 'n'},
      {"msg-size", required_argument, NULL, 's'},
      {"mtu", required_argument, NULL, 'm'},
      {"count", required_argument, NULL, 'c'},
      {"stats", no_argument, NULL, 'S'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  *options = (RelayOptions){.in_port = 1,
                            .out_port = -1,
                            .in_gid = -1,
                            .out_gid = -1,
                            .pool = 16,
                            .msg_size = 4096,
                            .mtu = IBV_MTU_1024};
  snprintf(options->listen_port, sizeof(options->listen_port), "%s", DEFAULT_PORT);

  int flag;
  while ((flag = getopt_long(argc, argv, "h", flags, NULL)) != -1) {
    unsigned long value = 0;
    int ok = 1;
    switch (flag) {
    case 'd': options->in_device = optarg; break;
    case 'D': options->out_device = optarg; break;
    case 'p':
      ok = parse_uint(optarg, 255, &value) == 0 && value >= 1;
      options->in_port = (int)value;
      break;
    case 'P':
      ok = parse_uint(optarg, 255, &value) == 0 && value >= 1;
      options->out_port = (int)value;
      break;
    case 'g':
      ok = parse_uint(optarg, 255, &value) == 0;
      options->in_gid = (int)value;
      break;
    case 'G':
      ok = parse_uint(optarg, 255, &value) == 0;
      options->out_gid = (int)value;
      break;
    case 't':
      ok = options->ntargets < MAX_TARGETS &&
           parse_target(optarg, &options->targets[options->ntargets]) == 0;
      options->ntargets += ok;
      break;
    case 'l':
      ok = parse_uint(optarg, 65535, &value) == 0 && value >= 1;
      snprintf(options->listen_port, sizeof(options->listen_port), "%lu", value);
      break;
    case 'n':
      ok = parse_uint(optarg, 65535, &options->pool) == 0 && options->pool >= 1;
      break;
    case 's':
      ok = parse_uint(optarg, 1UL << 30, &options->msg_size) == 0 &&
           options->msg_size >= 1;
      break;
    case 'm': ok = parse_mtu(optarg, &options->mtu) == 0; break;
    case 'c': ok = parse_uint(optarg, ULONG_MAX, &options->count) == 0; break;
    case 'S': options->stats = 1; break;
    case 'h': print_usage(stdout, argv[0]); return 1;
    default: print_usage(stderr, argv[0]); return -1;
    }
    if (!ok) {
      fprintf(stderr, "Invalid value for option '%c': %s\n", flag,
              optarg != NULL ? optarg : "");
      return -1;
    }
  }
  if (optind != argc || options->in_device == NULL || options->in_gid < 0 ||
      options->ntargets == 0) {
    print_usage(stderr, argv[0]);
    return -1;
  }
  if (options->out_device == NULL) options->out_device = options->in_device;
  if (options->out_port < 0) options->out_port = options->in_port;
  if (options->out_gid < 0) options->out_gid = options->in_gid;
  return 0;
}

static double gbps(uint64_t bytes, double seconds) {
  return seconds > 0 ? (double)bytes * 8 / seconds / 1e9 : 0;
}

int main(int argc, char **argv) {
  RelayOptions options;
  int parsed = parse_options(argc, argv, &options);
  if (parsed != 0) {
    return parsed < 0 ? 1 : 0;
  }
  const int pool_size = (int)options.pool;
  const uint32_t msg_size = (uint32_t)options.msg_size;
  const int ntargets = options.ntargets;

  // One buffer pool, registered on both sides. A message lands in a slot
  // through the in-side key and leaves through the out-side key, never copied.
  size_t pool_bytes = (size_t)pool_size * msg_size;
  uint8_t *pool;
  if (posix_memalign((void **)&pool, 4096, pool_bytes) != 0) {
    fprintf(stderr, "Cannot allocate %zu bytes for the buffer pool\n", pool_bytes);
    return 1;
  }
  memset(pool, 0, pool_bytes);

  // Every slot can be on the wire to every target at the same time.
  RdmaSide in, out;
  if (side_open(&in, options.in_device, options.in_port, options.in_gid,
                pool_size + 1) != 0 ||
      side_open(&out, options.out_device, options.out_port, options.out_gid,
                pool_size * ntargets + 1) != 0 ||
      side_register(&in, pool, pool_bytes) != 0 ||
      side_register(&out, pool, pool_bytes) != 0) {
    return 1;
  }

  // Connect to the targets first: each one already has its receive buffers
  // posted by the time it accepts our TCP connection.
  for (int t = 0; t < ntargets; t++) {
    Target *target = &options.targets[t];
    target->qp = qp_create(&out, pool_size, 1);
    ConnectInfo local, remote;
    if (target->qp == NULL || qp_local_info(&out, target->qp, &local) != 0) {
      return 1;
    }
    int fd = connect_server(target->host, target->port);
    if (fd < 0) {
      return 1;
    }
    int exchanged = exchange_info(fd, &local, &remote);
    close(fd);
    if (exchanged != 0 || qp_connect(&out, target->qp, &remote, options.mtu) != 0) {
      return 1;
    }
    printf("Target %d (%s:%s): local QPN %u, remote QPN %u\n", t, target->host,
           target->port, local.qpn, remote.qpn);
  }

  // Post the receive buffers while the QP is still in INIT, so the sender's
  // first message finds one waiting.
  struct ibv_qp *qp_in = qp_create(&in, 1, pool_size);
  ConnectInfo local, remote;
  if (qp_in == NULL || qp_local_info(&in, qp_in, &local) != 0) {
    return 1;
  }
  for (int i = 0; i < pool_size; i++) {
    if (qp_post_recv(qp_in, (uint64_t)i, pool + (size_t)i * msg_size, msg_size,
                     in.mr->lkey) != 0) {
      return 1;
    }
  }
  int fd = bind_server(options.listen_port);
  if (fd < 0) {
    return 1;
  }
  int exchanged = exchange_info(fd, &local, &remote);
  close(fd);
  if (exchanged != 0 || qp_connect(&in, qp_in, &remote, options.mtu) != 0) {
    return 1;
  }
  printf("Sender: local QPN %u, remote QPN %u\n", local.qpn, remote.qpn);
  printf("Relay ready: %d buffers of %u bytes, %d target(s).\n", pool_size, msg_size,
         ntargets);
  fflush(stdout);

  struct sigaction action = {.sa_handler = handle_signal};
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);

  // pending[slot] counts the sends of that slot still in flight. The slot goes
  // back to the receive queue when the last one completes.
  uint32_t *pending = calloc((size_t)pool_size, sizeof(*pending));
  if (pending == NULL) {
    perror("calloc");
    return 1;
  }
  struct ibv_wc wc[POLL_BATCH];
  uint64_t messages = 0, bytes = 0, inflight = 0;
  uint64_t stat_messages = 0, stat_bytes = 0;
  double first_message = 0, last_message = 0, next_stat = now() + 1;
  unsigned spin = 0;

  while (!stop_requested &&
         !(options.count != 0 && messages >= options.count && inflight == 0)) {
    int n = ibv_poll_cq(in.cq, POLL_BATCH, wc);
    if (n < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Receive from the sender failed: %s\n",
                ibv_wc_status_str(wc[i].status));
        return 1;
      }
      uint32_t slot = (uint32_t)wc[i].wr_id;
      uint32_t len = wc[i].byte_len;
      last_message = now();
      if (messages == 0) {
        first_message = last_message;
      }
      pending[slot] = (uint32_t)ntargets;
      inflight++;
      for (int t = 0; t < ntargets; t++) {
        if (qp_post_send(options.targets[t].qp, slot, pool + (size_t)slot * msg_size,
                         len, out.mr->lkey) != 0) {
          return 1;
        }
      }
      messages++;
      bytes += len;
    }

    n = ibv_poll_cq(out.cq, POLL_BATCH, wc);
    if (n < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        int t = 0;
        while (t < ntargets && options.targets[t].qp->qp_num != wc[i].qp_num) {
          t++;
        }
        fprintf(stderr, "Send to target %d (%s:%s) failed: %s\n", t,
                t < ntargets ? options.targets[t].host : "?",
                t < ntargets ? options.targets[t].port : "?",
                ibv_wc_status_str(wc[i].status));
        return 1;
      }
      uint32_t slot = (uint32_t)wc[i].wr_id;
      if (--pending[slot] == 0) {
        inflight--;
        if (qp_post_recv(qp_in, slot, pool + (size_t)slot * msg_size, msg_size,
                         in.mr->lkey) != 0) {
          return 1;
        }
      }
    }

    if (options.stats && (++spin & 0x3ff) == 0 && now() >= next_stat) {
      printf("[relay] %llu msg/s, %.2f Gbps in, %.2f Gbps out (total %llu messages)\n",
             (unsigned long long)(messages - stat_messages),
             gbps(bytes - stat_bytes, 1.0),
             gbps((bytes - stat_bytes) * (uint64_t)ntargets, 1.0),
             (unsigned long long)messages);
      fflush(stdout);
      stat_messages = messages;
      stat_bytes = bytes;
      next_stat += 1;
    }
  }

  double elapsed = last_message - first_message;
  printf("Forwarded %llu messages, %llu bytes in, to %d target(s) in %.3f s: "
         "%.2f Gbps in, %.2f Gbps out\n",
         (unsigned long long)messages, (unsigned long long)bytes, ntargets, elapsed,
         gbps(bytes, elapsed), gbps(bytes * (uint64_t)ntargets, elapsed));
  return 0;
}
