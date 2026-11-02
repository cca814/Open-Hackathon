// Multicast target: receives messages on one RC QP, or on several with
// --receivers. Start one per target (or per group of targets), each on its own
// --port, before the relay connects to it.

#include "payload.h"
#include "qp.h"
#include "tcp.h"
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define POLL_BATCH 32
#define DEFAULT_PORT 10001
#define MAX_RECEIVERS 64

typedef struct sink_options {
  const char *device;
  int ib_port;
  int gid_index;
  unsigned long tcp_port, receivers;
  unsigned long pool, msg_size, count;
  enum ibv_mtu mtu;
  int verify;
  int latency;
  unsigned long latency_skip;
} SinkOptions;

// What one QP has received so far.
typedef struct receiver {
  struct ibv_qp *qp;
  uint64_t messages, bytes, corrupted, out_of_order, expected;
  double first_message, last_message;
  uint32_t *latency;
  size_t latency_count, latency_capacity;
  uint64_t no_send_time, clock_behind, not_recorded;
} Receiver;

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
          "Usage: %s --device <dev> --gid-index <index> [options]\n"
          "  --ib-port    RDMA port number (default 1)\n"
          "  --port       TCP port to listen on for the QPN/GID exchange (default %d);\n"
          "               with --receivers N, the N ports from here on\n"
          "  --receivers  QPs to serve in this process, one per TCP port (default 1, max %d)\n"
          "  --count      messages to receive on each QP before exiting, 0 = until Ctrl+C (default 1)\n"
          "  --pool       receive buffers per QP (default 16)\n"
          "  --msg-size   bytes per buffer, the largest message (default 4096)\n"
          "  --mtu        path MTU: 256..4096 (default 1024)\n"
          "  --verify     check messages made by source: intact and in sequence\n"
          "  --latency    time each message from the source's post to this receive;\n"
          "               source and sink must run on one machine (one clock)\n"
          "  --latency-skip  leave the first N messages out of the latency figures (default 0)\n",
          program, DEFAULT_PORT, MAX_RECEIVERS);
}

static int parse_options(int argc, char **argv, SinkOptions *options) {
  static const struct option flags[] = {
      {"device", required_argument, NULL, 'd'},
      {"ib-port", required_argument, NULL, 'p'},
      {"gid-index", required_argument, NULL, 'g'},
      {"port", required_argument, NULL, 'l'},
      {"receivers", required_argument, NULL, 'r'},
      {"count", required_argument, NULL, 'c'},
      {"pool", required_argument, NULL, 'n'},
      {"msg-size", required_argument, NULL, 's'},
      {"mtu", required_argument, NULL, 'm'},
      {"verify", no_argument, NULL, 'v'},
      {"latency", no_argument, NULL, 'L'},
      {"latency-skip", required_argument, NULL, 'k'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  *options = (SinkOptions){.ib_port = 1,
                           .gid_index = -1,
                           .tcp_port = DEFAULT_PORT,
                           .receivers = 1,
                           .count = 1,
                           .pool = 16,
                           .msg_size = 4096,
                           .mtu = IBV_MTU_1024};

  int flag;
  while ((flag = getopt_long(argc, argv, "h", flags, NULL)) != -1) {
    unsigned long value = 0;
    int ok = 1;
    switch (flag) {
    case 'd': options->device = optarg; break;
    case 'p':
      ok = parse_uint(optarg, 255, &value) == 0 && value >= 1;
      options->ib_port = (int)value;
      break;
    case 'g':
      ok = parse_uint(optarg, 255, &value) == 0;
      options->gid_index = (int)value;
      break;
    case 'l':
      ok = parse_uint(optarg, 65535, &options->tcp_port) == 0 && options->tcp_port >= 1;
      break;
    case 'r':
      ok = parse_uint(optarg, MAX_RECEIVERS, &options->receivers) == 0 &&
           options->receivers >= 1;
      break;
    case 'c': ok = parse_uint(optarg, ULONG_MAX, &options->count) == 0; break;
    case 'n':
      ok = parse_uint(optarg, 65535, &options->pool) == 0 && options->pool >= 1;
      break;
    case 's':
      ok = parse_uint(optarg, 1UL << 30, &options->msg_size) == 0 &&
           options->msg_size >= 1;
      break;
    case 'm': ok = parse_mtu(optarg, &options->mtu) == 0; break;
    case 'v': options->verify = 1; break;
    case 'L': options->latency = 1; break;
    case 'k': ok = parse_uint(optarg, ULONG_MAX, &options->latency_skip) == 0; break;
    case 'h': print_usage(stdout, argv[0]); return 1;
    default: print_usage(stderr, argv[0]); return -1;
    }
    if (!ok) {
      fprintf(stderr, "Invalid value for option '%c': %s\n", flag,
              optarg != NULL ? optarg : "");
      return -1;
    }
  }
  if (optind != argc || options->device == NULL || options->gid_index < 0) {
    print_usage(stderr, argv[0]);
    return -1;
  }
  if (options->tcp_port + options->receivers - 1 > 65535) {
    fprintf(stderr, "--port %lu and --receivers %lu need TCP ports above 65535\n",
            options->tcp_port, options->receivers);
    return -1;
  }
  return 0;
}

// Text such as client.c's "Hello RDMA!" is shown as is; anything else as hex.
static void print_message(const uint8_t *buf, uint32_t len) {
  uint32_t shown = len < 64 ? len : 64;
  while (shown > 0 && buf[shown - 1] == '\0') {
    shown--;
  }
  int text = 1;
  for (uint32_t i = 0; i < shown; i++) {
    text = text && isprint(buf[i]);
  }
  if (text) {
    printf("Received data: %.*s\n", (int)shown, (const char *)buf);
    return;
  }
  printf("Received %u bytes, first %u:", len, len < 16 ? len : 16);
  for (uint32_t i = 0; i < len && i < 16; i++) {
    printf(" %02x", buf[i]);
  }
  printf("\n");
}

// Delays are kept as 32-bit nanoseconds (about 4 s at most) up to this many for
// each receiver this caps the memory at 64 MB.
#define LATENCY_MAX_SAMPLES (1UL << 24)

static int compare_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

// Nearest-rank percentile of sorted samples, in microseconds. permille is 500 for
// the median, 999 for the 99.9th percentile.
static double percentile_us(const uint32_t *sorted, size_t n, unsigned permille) {
  size_t rank = (n * permille + 999) / 1000;
  return (double)sorted[(rank < 1 ? 1 : rank) - 1] / 1e3;
}

static void print_latency(const char *prefix, uint32_t *samples, size_t n,
                          uint64_t no_send_time, uint64_t clock_behind,
                          uint64_t not_recorded) {
  if (n == 0) {
    printf("%sLatency: n=0 (no message carried a usable send time)\n", prefix);
  } else {
    qsort(samples, n, sizeof(*samples), compare_u32);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
      sum += samples[i];
    }
    printf("%sLatency: n=%zu min=%.1f p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f "
           "mean=%.1f us\n",
           prefix, n, samples[0] / 1e3, percentile_us(samples, n, 500),
           percentile_us(samples, n, 900), percentile_us(samples, n, 990),
           percentile_us(samples, n, 999), samples[n - 1] / 1e3,
           (double)sum / (double)n / 1e3);
  }
  if (no_send_time != 0 || clock_behind != 0 || not_recorded != 0) {
    printf("%sLatency left out: %llu without a send time, %llu received before their send "
           "time (the clocks differ), %llu over the sample limit\n",
           prefix, (unsigned long long)no_send_time, (unsigned long long)clock_behind,
           (unsigned long long)not_recorded);
  }
}

// The summary of one receiver. With several, each line starts with "[r<i>] ".
// Returns 0 when --verify found nothing wrong.
static int print_summary(const SinkOptions *options, Receiver *rc, int index) {
  char prefix[16] = "";
  if (options->receivers > 1) {
    snprintf(prefix, sizeof(prefix), "[r%d] ", index);
  }
  double elapsed = rc->last_message - rc->first_message;
  printf("%sReceived %llu messages, %llu bytes in %.3f s: %.2f Gbps\n", prefix,
         (unsigned long long)rc->messages, (unsigned long long)rc->bytes, elapsed,
         elapsed > 0 ? (double)rc->bytes * 8 / elapsed / 1e9 : 0.0);
  if (options->latency) {
    print_latency(prefix, rc->latency, rc->latency_count, rc->no_send_time,
                  rc->clock_behind, rc->not_recorded);
  }
  if (options->verify) {
    printf("%sVerify: %llu corrupted, %llu out of order\n", prefix,
           (unsigned long long)rc->corrupted, (unsigned long long)rc->out_of_order);
    return rc->corrupted == 0 && rc->out_of_order == 0 ? 0 : 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  SinkOptions options;
  int parsed = parse_options(argc, argv, &options);
  if (parsed != 0) {
    return parsed < 0 ? 1 : 0;
  }
  const int pool_size = (int)options.pool;
  const int nrecv = (int)options.receivers;
  const uint32_t msg_size = (uint32_t)options.msg_size;

  // One pool, pool_size slots for each receiver. A work request id is the slot's
  // number in the whole pool, which also says which receiver it belongs to.
  size_t pool_bytes = (size_t)nrecv * pool_size * msg_size;
  uint8_t *pool;
  if (posix_memalign((void **)&pool, 4096, pool_bytes) != 0) {
    fprintf(stderr, "Cannot allocate %zu bytes for the buffer pool\n", pool_bytes);
    return 1;
  }
  memset(pool, 0, pool_bytes);

  RdmaSide side;
  if (side_open(&side, options.device, options.ib_port, options.gid_index,
                nrecv * pool_size + 1) != 0 ||
      side_register(&side, pool, pool_bytes) != 0) {
    return 1;
  }

  uint8_t *ramp = payload_ramp_new(msg_size);
  if (ramp == NULL) {
    perror("malloc");
    return 1;
  }

  Receiver receivers[MAX_RECEIVERS] = {0};
  int listen_fds[MAX_RECEIVERS];

  // Post every receive buffer while the QPs are still in INIT, ahead of the TCP
  // exchange, and listen on every port before accepting on any: the relay
  // connects to its targets one after the other and must not find a port closed.
  for (int r = 0; r < nrecv; r++) {
    receivers[r].qp = qp_create(&side, 1, pool_size);
    if (receivers[r].qp == NULL) {
      return 1;
    }
    for (int i = 0; i < pool_size; i++) {
      size_t slot = (size_t)r * pool_size + i;
      if (qp_post_recv(receivers[r].qp, (uint64_t)slot, pool + slot * msg_size,
                       msg_size, side.mr->lkey) != 0) {
        return 1;
      }
    }
    char port[8];
    snprintf(port, sizeof(port), "%lu", options.tcp_port + (unsigned long)r);
    listen_fds[r] = listen_server(port);
    if (listen_fds[r] < 0) {
      return 1;
    }
  }

  for (int r = 0; r < nrecv; r++) {
    printf("[Server] Waiting for IPv4 client on port %lu ...\n",
           options.tcp_port + (unsigned long)r);
    fflush(stdout);
    int fd = accept(listen_fds[r], NULL, NULL);
    close(listen_fds[r]);
    ConnectInfo local, remote;
    if (fd < 0 || qp_local_info(&side, receivers[r].qp, &local) != 0) {
      return 1;
    }
    int exchanged = exchange_info(fd, &local, &remote);
    close(fd);
    if (exchanged != 0 ||
        qp_connect(&side, receivers[r].qp, &remote, options.mtu) != 0) {
      return 1;
    }
    printf("QP ready: local QPN %u, remote QPN %u\n", local.qpn, remote.qpn);
  }
  printf("Receive buffers ready; waiting for messages.\n");
  fflush(stdout);

  struct sigaction action = {.sa_handler = handle_signal};
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);

  if (options.latency) {
    for (int r = 0; r < nrecv; r++) {
      receivers[r].latency_capacity =
          options.count != 0 && options.count < LATENCY_MAX_SAMPLES
              ? options.count
              : LATENCY_MAX_SAMPLES;
      receivers[r].latency =
          malloc(receivers[r].latency_capacity * sizeof(*receivers[r].latency));
      if (receivers[r].latency == NULL) {
        perror("malloc");
        return 1;
      }
    }
  }

  struct ibv_wc wc[POLL_BATCH];
  int finished = 0;
  while (!stop_requested && (options.count == 0 || finished < nrecv)) {
    int n = ibv_poll_cq(side.cq, POLL_BATCH, wc);
    if (n < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      return 1;
    }
    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Receive failed: %s\n", ibv_wc_status_str(wc[i].status));
        return 1;
      }
      uint32_t slot = (uint32_t)wc[i].wr_id;
      Receiver *rc = &receivers[slot / (uint32_t)pool_size];
      uint32_t len = wc[i].byte_len;
      uint8_t *buf = pool + (size_t)slot * msg_size;
      rc->last_message = now();
      uint64_t received_ns = options.latency ? mono_ns() : 0;
      if (rc->messages == 0) {
        rc->first_message = rc->last_message;
        if (nrecv == 1) {
          print_message(buf, len);
        }
      }
      if (options.verify) {
        uint64_t seq;
        if (payload_check(buf, len, &seq, ramp) != 0) {
          rc->corrupted++;
        } else {
          if (rc->messages > 0 && seq != rc->expected) {
            rc->out_of_order++;
          }
          rc->expected = seq + 1;
        }
      }
      if (options.latency && rc->messages >= options.latency_skip) {
        uint64_t sent_ns;
        if (payload_sent_ns(buf, len, &sent_ns) != 0) {
          rc->no_send_time++;
        } else if (received_ns < sent_ns) {
          rc->clock_behind++;
        } else if (rc->latency_count < rc->latency_capacity) {
          uint64_t delay = received_ns - sent_ns;
          rc->latency[rc->latency_count++] =
              delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
        } else {
          rc->not_recorded++;
        }
      }
      rc->messages++;
      rc->bytes += len;
      if (options.count != 0 && rc->messages == options.count) {
        finished++;
      }
      if (qp_post_recv(rc->qp, slot, buf, msg_size, side.mr->lkey) != 0) {
        return 1;
      }
    }
  }

  int failed = 0;
  for (int r = 0; r < nrecv; r++) {
    failed |= print_summary(&options, &receivers[r], r);
  }
  return failed;
}
