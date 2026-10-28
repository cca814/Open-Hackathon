#include "common.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdlib.h>

struct ibv_context *get_dedicate_device_ctx(const char *device_name) {
  int number_of_device;
  struct ibv_device *device = NULL;
  struct ibv_device **devices = ibv_get_device_list(&number_of_device);
  if (devices == NULL) {
    return NULL;
  }

  for (int i = 0; i < number_of_device; i++) {
    if (strcmp(devices[i]->name, device_name) == 0) {
      device = devices[i];
      break;
    }
  }

  if (device == NULL) {
    fprintf(stderr, "RDMA device not found: %s\n", device_name);
    ibv_free_device_list(devices);
    return NULL;
  }

  // 2. Create context.
  struct ibv_context *ctx = ibv_open_device(device);
  ibv_free_device_list(devices);
  if (ctx == NULL) {
    perror("ibv_open_device");
    return NULL;
  }

  return ctx;
}

static int parse_number(const char *text, uint32_t max, uint32_t *value) {
  char *end;
  errno = 0;
  unsigned long number = strtoul(text, &end, 0);
  if (errno || *text == '-' || end == text || *end != '\0' || number > max) {
    return -1;
  }
  *value = (uint32_t)number;
  return 0;
}

static void print_usage(FILE *out, const char *program, int is_server) {
  fprintf(out, "Usage: %s --device <device> --gid-index <index> ", program);
  if (is_server) {
    fprintf(out, "[--config-file <file> [--info-file <json>]]\n"
                 "Without --config-file, listen for TCP exchange on port 10001.\n");
  } else {
    fprintf(out, "(--server-ip <host> | --config-file <file>) [--rid <rid>] [--info-file <json>]\n"
                 "--server-ip: exchange QPN/GID over TCP, using PSN 1.\n"
                 "--config-file: use the preconfigured P4 virtual endpoint.\n"
                 "--rid: multicast receiver ID; the info file defaults to receiver-<rid>.json.\n");
  }
  fprintf(out, "--info-file applies only to --config-file (default: %s-connection.json).\n",
          is_server ? "server" : "client");
}

int parse_rdma_options(int argc, char **argv, int is_server, RdmaOptions *options) {
  static const struct option flags[] = {
      {"device", required_argument, NULL, 'd'},
      {"gid-index", required_argument, NULL, 'g'},
      {"server-ip", required_argument, NULL, 's'},
      {"config-file", required_argument, NULL, 'c'},
      {"info-file", required_argument, NULL, 'o'},
      {"rid", required_argument, NULL, 'r'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  *options = (RdmaOptions){.gid_index = -1};
  int flag, rid = -1;
  while ((flag = getopt_long(argc, argv, "d:g:s:c:o:r:h", flags, NULL)) != -1) {
    uint32_t index;
    if (optarg != NULL && *optarg == '\0') {
      fprintf(stderr, "Option values must not be empty\n");
      return -1;
    }
    switch (flag) {
    case 'd': options->device_name = optarg; break;
    case 'g':
      if (parse_number(optarg, UINT8_MAX, &index) != 0) {
        fprintf(stderr, "--gid-index must be between 0 and 255\n");
        return -1;
      }
      options->gid_index = (int)index;
      break;
    case 's': options->server_ip = optarg; break;
    case 'c': options->config_file = optarg; break;
    case 'o': options->info_file = optarg; break;
    case 'r':
      // standard_metadata.egress_rid is bit<16>.
      if (parse_number(optarg, UINT16_MAX, &index) != 0) {
        fprintf(stderr, "--rid must be between 0 and 65535\n");
        return -1;
      }
      rid = (int)index;
      break;
    case 'h': print_usage(stdout, argv[0], is_server); return 1;
    default: print_usage(stderr, argv[0], is_server); return -1;
    }
  }
  if (optind != argc || options->device_name == NULL || options->gid_index < 0) {
    print_usage(stderr, argv[0], is_server);
    return -1;
  }
  if (is_server && options->server_ip != NULL) {
    fprintf(stderr, "--server-ip is only valid for the client\n");
    return -1;
  }
  if (is_server && rid >= 0) {
    fprintf(stderr, "--rid is only valid for the client\n");
    return -1;
  }
  if (!is_server && ((options->server_ip == NULL) == (options->config_file == NULL))) {
    fprintf(stderr, "Choose exactly one of --server-ip and --config-file\n");
    return -1;
  }
  if (options->config_file == NULL && options->info_file != NULL) {
    fprintf(stderr, "--info-file requires --config-file\n");
    return -1;
  }
  if (options->config_file == NULL && rid >= 0) {
    fprintf(stderr, "--rid requires --config-file\n");
    return -1;
  }
  if (options->config_file != NULL && options->info_file == NULL) {
    if (rid >= 0) {
      // The name generate-multicast-group-config.py expects in group<gid>/.
      static char rid_info_file[sizeof("receiver-65535.json")];
      snprintf(rid_info_file, sizeof(rid_info_file), "receiver-%d.json", rid);
      options->info_file = rid_info_file;
    } else {
      options->info_file = is_server ? "server-connection.json" : "client-connection.json";
    }
  }
  return 0;
}

static int parse_mac(const char *text, char output[18]) {
  if (strlen(text) != 17) {
    return -1;
  }
  for (int i = 0; i < 17; i++) {
    if ((i % 3 == 2 && text[i] != ':') ||
        (i % 3 != 2 && !isxdigit((unsigned char)text[i]))) {
      return -1;
    }
  }
  unsigned int bytes[6];
  int used = 0;
  if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%n", &bytes[0], &bytes[1],
             &bytes[2], &bytes[3], &bytes[4], &bytes[5], &used) != 6 || used != 17 ||
      (bytes[0] & 1) != 0 ||
      (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) == 0) {
    return -1;
  }
  snprintf(output, 18, "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1],
           bytes[2], bytes[3], bytes[4], bytes[5]);
  return 0;
}

int load_peer_config(const char *path, PeerConfig *config) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    perror(path);
    return -1;
  }
  memset(config, 0, sizeof(*config));
  char line[256], key[32], value[64], extra;
  unsigned int seen = 0, number = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    number++;
    char *comment = strchr(line, '#');
    if (comment != NULL) {
      *comment = '\0';
    }
    int count = sscanf(line, "%31s %63s %c", key, value, &extra);
    if (count == EOF) {
      continue;
    }
    unsigned int field = 0;
    int ret = -1;
    if (count == 2) {
      if (strcmp(key, "remote_gid") == 0) {
        field = 1;
        ret = inet_pton(AF_INET6, value, config->remote.gid.raw) == 1 ? 0 : -1;
        // The current P4 MITM only translates IPv4 RoCEv2.
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
        if (memcmp(config->remote.gid.raw, mapped, sizeof(mapped)) != 0) {
          ret = -1;
        }
      } else if (strcmp(key, "remote_qpn") == 0) {
        field = 2;
        ret = parse_number(value, 0xffffff, &config->remote.qpn);
        if (config->remote.qpn == 0) ret = -1;
      } else if (strcmp(key, "sq_psn") == 0) {
        field = 4;
        ret = parse_number(value, 0xffffff, &config->sq_psn);
      } else if (strcmp(key, "rq_psn") == 0) {
        field = 8;
        ret = parse_number(value, 0xffffff, &config->rq_psn);
      } else if (strcmp(key, "switch_port") == 0) {
        field = 16;
        ret = parse_number(value, 509, &config->switch_port);
        if (config->switch_port == 0) ret = -1;
      } else if (strcmp(key, "local_mac") == 0) {
        field = 32;
        ret = parse_mac(value, config->local_mac);
      } else if (strcmp(key, "remote_mac") == 0) {
        field = 64;
        ret = parse_mac(value, config->remote_mac);
      }
    }
    if (ret != 0 || (seen & field) != 0) {
      fprintf(stderr, "%s:%u: invalid, unknown or duplicate configuration field\n",
              path, number);
      fclose(file);
      return -1;
    }
    seen |= field;
  }
  int failed = ferror(file);
  fclose(file);
  if (failed || seen != 127) {
    fprintf(stderr, "%s: cannot read all seven required configuration fields\n", path);
    return -1;
  }
  return 0;
}

int write_connection_info(const char *path, const ConnectInfo *local,
                          const PeerConfig *config) {
  char gid[INET6_ADDRSTRLEN], remote_gid[INET6_ADDRSTRLEN];
  if (inet_ntop(AF_INET6, local->gid.raw, gid, sizeof(gid)) == NULL ||
      inet_ntop(AF_INET6, config->remote.gid.raw, remote_gid, sizeof(remote_gid)) == NULL) {
    return -1;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    perror(path);
    return -1;
  }
  int ret = fprintf(file,
      "{\n  \"qpn\": %u, \"gid\": \"%s\",\n"
      "  \"remote_qpn\": %u, \"remote_gid\": \"%s\",\n"
      "  \"sq_psn\": %u, \"rq_psn\": %u, \"switch_port\": %u,\n"
      "  \"local_mac\": \"%s\", \"remote_mac\": \"%s\"\n}\n",
      local->qpn, gid, config->remote.qpn, remote_gid, config->sq_psn,
      config->rq_psn, config->switch_port, config->local_mac, config->remote_mac);
  int closed = fclose(file);
  if (ret < 0 || closed != 0) {
    fprintf(stderr, "Failed to write %s\n", path);
    return -1;
  }
  printf("Connection information written to %s\n", path);
  return 0;
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
