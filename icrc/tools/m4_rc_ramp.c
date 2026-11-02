/* SPDX-License-Identifier: BSD-2-Clause
 * Five-QP, single-receiver-interface IPv4 RC SEND gradual-rate experiment.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    MAX_MESSAGE_SIZE = 65536,
    MAX_RATES = 16,
    MAX_WINDOW = 4096,
    CQ_BATCH = 64,
    PATH_MTU_BYTES = 1024,
    RECEIVER_COUNT = 5
};
enum { SENDER_PSN = 0x123456 };

static uint32_t receiver_initial_psn(size_t index)
{
    return (uint32_t)((index + 1u) * 0x200000u) & 0x00ffffffu;
}

struct endpoint {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint8_t *buffer;
    size_t buffer_bytes;
    union ibv_gid gid;
    bool owns_device;
};

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static struct ibv_context *open_device(const char *name)
{
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices)
        return NULL;
    struct ibv_context *context = NULL;
    for (int i = 0; i < count; ++i)
        if (strcmp(ibv_get_device_name(devices[i]), name) == 0) {
            context = ibv_open_device(devices[i]);
            break;
        }
    ibv_free_device_list(devices);
    return context;
}

static int endpoint_allocate(struct endpoint *endpoint, const char *device,
                             size_t buffer_bytes, unsigned queue_depth)
{
    endpoint->cq = ibv_create_cq(endpoint->context, (int)queue_depth * 2,
                                 NULL, NULL, 0);
    int memory_status = posix_memalign((void **)&endpoint->buffer, 64,
                                       buffer_bytes);
    if (!endpoint->cq || memory_status != 0) {
        fprintf(stderr, "cannot allocate %s CQ/buffer: %s\n", device,
                strerror(memory_status ? memory_status : errno));
        return -1;
    }
    endpoint->buffer_bytes = buffer_bytes;
    memset(endpoint->buffer, 0, buffer_bytes);
    endpoint->mr = ibv_reg_mr(endpoint->pd, endpoint->buffer, buffer_bytes,
                              IBV_ACCESS_LOCAL_WRITE);
    if (!endpoint->mr) {
        fprintf(stderr, "cannot register %s memory: %s\n", device, strerror(errno));
        return -1;
    }
    struct ibv_qp_init_attr init = {
        .send_cq = endpoint->cq,
        .recv_cq = endpoint->cq,
        .cap = {
            .max_send_wr = queue_depth,
            .max_recv_wr = queue_depth,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    endpoint->qp = ibv_create_qp(endpoint->pd, &init);
    if (!endpoint->qp)
        fprintf(stderr, "cannot create %s QP: %s\n", device, strerror(errno));
    return endpoint->qp ? 0 : -1;
}

static int endpoint_open(struct endpoint *endpoint, const char *device, int gid_index,
                         size_t buffer_bytes, unsigned queue_depth)
{
    endpoint->context = open_device(device);
    if (!endpoint->context) {
        fprintf(stderr, "cannot open RDMA device %s\n", device);
        return -1;
    }
    endpoint->owns_device = true;
    if (ibv_query_gid(endpoint->context, 1, gid_index, &endpoint->gid) != 0) {
        fprintf(stderr, "cannot query %s GID index %d: %s\n",
                device, gid_index, strerror(errno));
        return -1;
    }
    endpoint->pd = ibv_alloc_pd(endpoint->context);
    if (!endpoint->pd) {
        fprintf(stderr, "cannot allocate %s PD: %s\n", device, strerror(errno));
        return -1;
    }
    return endpoint_allocate(endpoint, device, buffer_bytes, queue_depth);
}

static int endpoint_open_shared(struct endpoint *endpoint,
                                const struct endpoint *owner,
                                const char *device, size_t buffer_bytes,
                                unsigned queue_depth)
{
    endpoint->context = owner->context;
    endpoint->pd = owner->pd;
    endpoint->gid = owner->gid;
    return endpoint_allocate(endpoint, device, buffer_bytes, queue_depth);
}

static struct ibv_qp *create_dummy_qp(struct endpoint *endpoint)
{
    struct ibv_qp_init_attr init = {
        .send_cq = endpoint->cq,
        .recv_cq = endpoint->cq,
        .cap = {.max_send_wr = 1, .max_recv_wr = 1,
                .max_send_sge = 1, .max_recv_sge = 1},
        .qp_type = IBV_QPT_RC,
    };
    return ibv_create_qp(endpoint->pd, &init);
}

static int qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = 0,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                         IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint32_t remote_psn,
                     const union ibv_gid *remote_gid, int gid_index)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = remote_qpn,
        .rq_psn = remote_psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .port_num = 1,
            .grh = {.sgid_index = gid_index, .hop_limit = 255},
        },
    };
    attr.ah_attr.grh.dgid = *remote_gid;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static int qp_to_rts(struct ibv_qp *qp, uint32_t psn)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = psn,
        .max_rd_atomic = 1,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
        IBV_QP_MAX_QP_RD_ATOMIC);
}

static int post_receive(struct endpoint *endpoint, size_t offset,
                        size_t length, uint64_t id)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)(endpoint->buffer + offset),
        .length = (uint32_t)length,
        .lkey = endpoint->mr->lkey,
    };
    struct ibv_recv_wr wr = {.wr_id = id, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(endpoint->qp, &wr, &bad);
}

static int post_send(struct endpoint *endpoint, size_t offset,
                     size_t length, uint64_t id)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)(endpoint->buffer + offset),
        .length = (uint32_t)length,
        .lkey = endpoint->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = id,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(endpoint->qp, &wr, &bad);
}

static void fill_payload(uint8_t *buffer, size_t length, uint64_t sequence)
{
    for (size_t i = 0; i < length; ++i)
        buffer[i] = (uint8_t)(sequence + i * 17u);
    if (length >= sizeof(sequence))
        memcpy(buffer, &sequence, sizeof(sequence));
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int run_stage(struct endpoint *sender,
                     struct endpoint receivers[RECEIVER_COUNT],
                     unsigned rate_mbps, unsigned seconds, size_t message_size,
                     unsigned window, uint64_t wire_packet_target,
                     uint64_t *sequence)
{
    uint64_t segments_per_message =
        (message_size + PATH_MTU_BYTES - 1u) / PATH_MTU_BYTES;
    bool unpaced = wire_packet_target != 0;
    if (unpaced && wire_packet_target % segments_per_message) {
        fprintf(stderr,
                "wire packet target=%" PRIu64
                " is not divisible by segments_per_message=%" PRIu64 "\n",
                wire_packet_target, segments_per_message);
        return -1;
    }
    uint64_t count = unpaced ? wire_packet_target / segments_per_message :
        ((uint64_t)rate_mbps * UINT64_C(1000000) * seconds) /
        (message_size * 8u);
    if (count == 0)
        count = 1;
    if (count > UINT64_C(10000000)) {
        fprintf(stderr, "stage exceeds ten million messages\n");
        return -1;
    }
    uint64_t *latency = calloc((size_t)count, sizeof(*latency));
    uint64_t *posted_ns = calloc(window, sizeof(*posted_ns));
    uint8_t *slot_state = calloc(window, sizeof(*slot_state));
    if (!latency || !posted_ns || !slot_state)
        goto failed;
    const uint8_t all_completions = (uint8_t)((1u << (RECEIVER_COUNT + 1u)) - 1u);
    uint64_t first = *sequence + 1u;
    uint64_t posted = 0, completed = 0, inflight = 0;
    uint64_t sender_cqes = 0, receiver_cqes[RECEIVER_COUNT] = {0};
    uint64_t start = monotonic_ns();
    uint64_t last_progress = start;
    while (completed < count) {
        uint64_t now = monotonic_ns();
        uint64_t elapsed_for_pacing = now - start;
        if (elapsed_for_pacing > (uint64_t)seconds * UINT64_C(1000000000))
            elapsed_for_pacing = (uint64_t)seconds * UINT64_C(1000000000);
        uint64_t due = unpaced ? count :
            (uint64_t)(((long double)rate_mbps * 1.0e6L *
                        (long double)elapsed_for_pacing) /
                       ((long double)message_size * 8.0L * 1.0e9L));
        if (due > count)
            due = count;
        while (posted < due && inflight < window) {
            uint64_t id = first + posted;
            size_t slot = (size_t)((id - 1u) % window);
            size_t offset = slot * message_size;
            if (slot_state[slot]) {
                fprintf(stderr, "slot %zu reused before completion\n", slot);
                goto failed;
            }
            for (size_t receiver = 0; receiver < RECEIVER_COUNT; ++receiver)
                if (post_receive(&receivers[receiver], offset, message_size, id) != 0) {
                    fprintf(stderr, "receiver%zu post failed at sequence=%" PRIu64
                            ": %s\n", receiver, id, strerror(errno));
                    goto failed;
                }
            fill_payload(sender->buffer + offset, message_size, id);
            posted_ns[slot] = monotonic_ns();
            slot_state[slot] = all_completions;
            if (post_send(sender, offset, message_size, id) != 0) {
                fprintf(stderr, "sender post failed at sequence=%" PRIu64 ": %s\n",
                        id, strerror(errno));
                goto failed;
            }
            ++posted;
            ++inflight;
            *sequence = id;
        }

        struct ibv_wc wc[CQ_BATCH];
        for (size_t endpoint_index = 0;
             endpoint_index <= RECEIVER_COUNT; ++endpoint_index) {
            struct endpoint *endpoint = endpoint_index == RECEIVER_COUNT ?
                sender : &receivers[endpoint_index];
            for (;;) {
                int polled = ibv_poll_cq(endpoint->cq, CQ_BATCH, wc);
                if (polled < 0) {
                    fprintf(stderr, "CQ poll failed for endpoint %zu\n", endpoint_index);
                    goto failed;
                }
                for (int c = 0; c < polled; ++c) {
                    uint64_t id = wc[c].wr_id;
                    bool sender_cqe = endpoint_index == RECEIVER_COUNT;
                    enum ibv_wc_opcode expected_opcode =
                        sender_cqe ? IBV_WC_SEND : IBV_WC_RECV;
                    if (wc[c].status != IBV_WC_SUCCESS ||
                        wc[c].opcode != expected_opcode || id < first ||
                        id >= first + count ||
                        (!sender_cqe && wc[c].byte_len != message_size)) {
                        fprintf(stderr,
                                "CQE failed endpoint=%zu id=%" PRIu64
                                " status=%s opcode=%d len=%u\n",
                                endpoint_index, id,
                                ibv_wc_status_str(wc[c].status), wc[c].opcode,
                                wc[c].byte_len);
                        goto failed;
                    }
                    size_t slot = (size_t)((id - 1u) % window);
                    size_t offset = slot * message_size;
                    uint8_t bit = sender_cqe ? (uint8_t)(1u << RECEIVER_COUNT) :
                        (uint8_t)(1u << endpoint_index);
                    if (!(slot_state[slot] & bit)) {
                        fprintf(stderr, "duplicate CQE endpoint=%zu id=%" PRIu64 "\n",
                                endpoint_index, id);
                        goto failed;
                    }
                    if (sender_cqe) {
                        latency[id - first] = monotonic_ns() - posted_ns[slot];
                        ++sender_cqes;
                    } else {
                        if (memcmp(sender->buffer + offset,
                                   receivers[endpoint_index].buffer + offset,
                                   message_size) != 0) {
                            fprintf(stderr,
                                    "receiver%zu payload mismatch at sequence=%" PRIu64
                                    "\n", endpoint_index, id);
                            goto failed;
                        }
                        ++receiver_cqes[endpoint_index];
                    }
                    slot_state[slot] &= (uint8_t)~bit;
                    if (!slot_state[slot]) {
                        --inflight;
                        ++completed;
                        last_progress = monotonic_ns();
                    }
                }
                if (polled < CQ_BATCH)
                    break;
            }
        }
        if (monotonic_ns() - last_progress > UINT64_C(30000000000)) {
            fprintf(stderr,
                    "stage stalled mode=%s requested_mbps=%u posted=%" PRIu64
                    " completed=%" PRIu64 " inflight=%" PRIu64 "\n",
                    unpaced ? "unpaced" : "paced", rate_mbps,
                    posted, completed, inflight);
            goto failed;
        }
    }
    uint64_t elapsed = monotonic_ns() - start, total = 0;
    uint64_t total_receiver_cqes = 0;
    for (size_t receiver = 0; receiver < RECEIVER_COUNT; ++receiver) {
        total_receiver_cqes += receiver_cqes[receiver];
        if (receiver_cqes[receiver] != count) {
            fprintf(stderr,
                    "receiver%zu completion mismatch expected=%" PRIu64
                    " actual=%" PRIu64 "\n",
                    receiver, count, receiver_cqes[receiver]);
            goto failed;
        }
    }
    if (sender_cqes != count) {
        fprintf(stderr, "sender completion mismatch expected=%" PRIu64
                        " actual=%" PRIu64 "\n", count, sender_cqes);
        goto failed;
    }
    for (uint64_t i = 0; i < count; ++i)
        total += latency[i];
    qsort(latency, (size_t)count, sizeof(*latency), compare_u64);
    long double achieved = ((long double)count * message_size * 8.0L) /
                           ((long double)elapsed);
    printf("STAGE mode=%s requested_mbps=%u achieved_mbps=%.6Lf"
           " messages=%" PRIu64 " wire_packets=%" PRIu64
           " segments_per_message=%" PRIu64
           " bytes=%zu duration_ms=%.3Lf rtt_avg_us=%.3Lf rtt_p50_us=%.3Lf"
           " rtt_p99_us=%.3Lf rtt_max_us=%.3Lf receivers=%u"
           " window=%u"
           " per_receiver_cqes=%" PRIu64 " total_receiver_cqes=%" PRIu64
           " sender_cqes=%" PRIu64 "\n",
           unpaced ? "unpaced" : "paced", rate_mbps, achieved * 1000.0L,
           count, count * segments_per_message, segments_per_message, message_size,
           (long double)elapsed / 1.0e6L,
           (long double)total / count / 1000.0L,
           (long double)latency[(count - 1) * 50 / 100] / 1000.0L,
           (long double)latency[(count - 1) * 99 / 100] / 1000.0L,
           (long double)latency[count - 1] / 1000.0L, RECEIVER_COUNT, window,
           receiver_cqes[0], total_receiver_cqes, sender_cqes);
    fflush(stdout);
    free(latency);
    free(posted_ns);
    free(slot_state);
    return 0;

failed:
    free(latency);
    free(posted_ns);
    free(slot_state);
    return -1;
}

static int ipv4_gid(const union ibv_gid *gid, char output[INET_ADDRSTRLEN])
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(gid->raw, prefix, sizeof(prefix)) != 0)
        return -1;
    return inet_ntop(AF_INET, gid->raw + 12, output, INET_ADDRSTRLEN) ? 0 : -1;
}

static int write_config(const char *path, const struct endpoint *sender,
                        const struct endpoint receivers[RECEIVER_COUNT],
                        uint32_t virtual_qpn, const char *sender_mac,
                        const char *receiver_mac)
{
    char sender_ip[INET_ADDRSTRLEN], receiver_ip[INET_ADDRSTRLEN];
    if (ipv4_gid(&sender->gid, sender_ip) ||
        ipv4_gid(&receivers[0].gid, receiver_ip))
        return -1;
    for (size_t i = 1; i < RECEIVER_COUNT; ++i) {
        char candidate[INET_ADDRSTRLEN];
        if (ipv4_gid(&receivers[i].gid, candidate) || strcmp(candidate, receiver_ip))
            return -1;
    }
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    int status = fprintf(file,
        "{\n  \"groups\": [\n    {\n"
        "      \"id\": \"milestone-4-live-ipv4\",\n"
        "      \"match\": {\"dst_ipv4\": \"%s\", \"dst_qpn\": %u, "
        "\"initial_psn\": %u},\n"
        "      \"sender\": {\"dst_mac\": \"%s\", \"dst_ipv4\": \"%s\", "
        "\"dst_qpn\": %u},\n"
        "      \"ack_source\": {\"src_mac\": \"%s\", \"src_ipv4\": \"%s\"},\n"
        "      \"members\": [\n",
        receiver_ip, virtual_qpn, SENDER_PSN,
        sender_mac, sender_ip, sender->qp->qp_num,
        receiver_mac, receiver_ip);
    if (status < 0)
        goto failed;
    for (size_t i = 0; i < RECEIVER_COUNT; ++i)
        if (fprintf(file,
                    "        {\"dst_mac\": \"%s\", \"dst_ipv4\": \"%s\", "
                    "\"dst_qpn\": %u, \"initial_psn\": %u}%s\n",
                    receiver_mac, receiver_ip, receivers[i].qp->qp_num,
                    receiver_initial_psn(i),
                    i + 1 == RECEIVER_COUNT ? "" : ",") < 0)
            goto failed;
    int write_failed = fputs("      ]\n    }\n  ]\n}\n", file) == EOF;
    if (fclose(file) != 0)
        write_failed = 1;
    if (write_failed)
        return -1;
    return 0;

failed:
    fclose(file);
    return -1;
}

static int parse_rates(char *text, unsigned rates[MAX_RATES], size_t *count)
{
    char *save = NULL;
    for (char *token = strtok_r(text, ",", &save); token;
         token = strtok_r(NULL, ",", &save)) {
        char *end = NULL;
        unsigned long value = strtoul(token, &end, 10);
        if (!*token || *end || value == 0 || value > 100000 || *count == MAX_RATES)
            return -1;
        rates[(*count)++] = (unsigned)value;
    }
    return *count ? 0 : -1;
}

static void endpoint_close(struct endpoint *endpoint)
{
    if (endpoint->qp) ibv_destroy_qp(endpoint->qp);
    if (endpoint->mr) ibv_dereg_mr(endpoint->mr);
    free(endpoint->buffer);
    if (endpoint->cq) ibv_destroy_cq(endpoint->cq);
    if (endpoint->owns_device && endpoint->pd) ibv_dealloc_pd(endpoint->pd);
    if (endpoint->owns_device && endpoint->context) ibv_close_device(endpoint->context);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 7) {
        fprintf(stderr,
                "Usage: %s CONFIG [RATES_MBPS [SECONDS [MESSAGE_SIZE [WINDOW"
                " [WIRE_PACKET_COUNT]]]]]\n",
                argv[0]);
        return 2;
    }
    char *rates_text = strdup(argc > 2 ? argv[2] : "5,10,25,50,100");
    unsigned seconds = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 2;
    size_t message_size = argc > 4 ? (size_t)strtoul(argv[4], NULL, 10) : 4096;
    unsigned window = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 1;
    uint64_t wire_packet_target = argc > 6 ? strtoull(argv[6], NULL, 10) : 0;
    unsigned rates[MAX_RATES];
    size_t rate_count = 0;
    if (!rates_text || parse_rates(rates_text, rates, &rate_count) || seconds == 0 ||
        message_size < 8 || message_size > MAX_MESSAGE_SIZE || window == 0 ||
        window > MAX_WINDOW || message_size > SIZE_MAX / window ||
        (argc > 6 && wire_packet_target == 0)) {
        fprintf(stderr, "invalid rates, duration, message size, or window\n");
        free(rates_text);
        return 2;
    }
    free(rates_text);

    const int gid_index = 1;
    const char *sender_mac = "02:96:42:f8:5e:f2";
    const char *receiver_mac = "02:7e:6d:fd:79:7c";
    struct endpoint sender = {0}, receivers[RECEIVER_COUNT] = {{0}};
    struct ibv_qp *virtual_qp = NULL;
    int result = 1;
    size_t buffer_bytes = message_size * window;
    if (endpoint_open(&sender, "mlx5_2", gid_index, buffer_bytes, window))
        goto done;
    if (endpoint_open(&receivers[0], "mlx5_3", gid_index, buffer_bytes, window))
        goto done;
    for (size_t i = 1; i < RECEIVER_COUNT; ++i)
        if (endpoint_open_shared(&receivers[i], &receivers[0], "mlx5_3",
                                 buffer_bytes, window))
            goto done;
    virtual_qp = create_dummy_qp(&receivers[0]);
    if (!virtual_qp) {
        fprintf(stderr, "cannot create virtual QP: %s\n", strerror(errno));
        goto qp_failed;
    }
    if (qp_to_init(sender.qp)) {
        fprintf(stderr, "sender INIT failed: %s\n", strerror(errno));
        goto qp_failed;
    }
    for (size_t i = 0; i < RECEIVER_COUNT; ++i) {
        if (qp_to_init(receivers[i].qp)) {
            fprintf(stderr, "receiver%zu INIT failed: %s\n", i, strerror(errno));
            goto qp_failed;
        }
        if (qp_to_rtr(receivers[i].qp, sender.qp->qp_num,
                      receiver_initial_psn(i), &sender.gid, gid_index)) {
            fprintf(stderr, "receiver%zu RTR failed: %s\n", i, strerror(errno));
            goto qp_failed;
        }
        if (qp_to_rts(receivers[i].qp,
                      0xc00000u + (uint32_t)i * 0x10000u)) {
            fprintf(stderr, "receiver%zu RTS failed: %s\n", i, strerror(errno));
            goto qp_failed;
        }
    }
    if (qp_to_rtr(sender.qp, virtual_qp->qp_num, 0xc00000,
                  &receivers[0].gid, gid_index)) {
        fprintf(stderr, "sender RTR failed: %s\n", strerror(errno));
        goto qp_failed;
    }
    if (qp_to_rts(sender.qp, SENDER_PSN)) {
        fprintf(stderr, "sender RTS failed: %s\n", strerror(errno));
        goto qp_failed;
    }
    if (write_config(argv[1], &sender, receivers, virtual_qp->qp_num,
                     sender_mac, receiver_mac)) {
        fprintf(stderr, "runtime config write failed: %s\n", strerror(errno));
        goto qp_failed;
    }
    goto qp_ready;
qp_failed:
    goto done;
qp_ready:
    printf("READY sender_qpn=%u virtual_qpn=%u receiver_qpns=",
           sender.qp->qp_num, virtual_qp->qp_num);
    for (size_t i = 0; i < RECEIVER_COUNT; ++i)
        printf("%s%u", i ? "," : "", receivers[i].qp->qp_num);
    printf(" config=%s rates_mbps=", argv[1]);
    for (size_t i = 0; i < rate_count; ++i)
        printf("%s%u", i ? "," : "", rates[i]);
    printf(" seconds=%u message_size=%zu window=%u wire_packet_target=%" PRIu64 "\n"
           "Press Enter after Node B is running.\n",
           seconds, message_size, window, wire_packet_target);
    fflush(stdout);
    if (getchar() == EOF)
        goto done;
    uint64_t sequence = 0;
    for (size_t i = 0; i < rate_count; ++i)
        if (run_stage(&sender, receivers, rates[i], seconds, message_size,
                      window, wire_packet_target, &sequence)) {
            fprintf(stderr, "FAIL stage_mbps=%u sequence=%" PRIu64 "\n",
                    rates[i], sequence);
            goto done;
        }
    printf("PASS stages=%zu total_messages=%" PRIu64
           " receivers=%u receiver_parity=true sender_parity=true\n",
           rate_count, sequence, RECEIVER_COUNT);
    result = 0;

done:
    if (virtual_qp) ibv_destroy_qp(virtual_qp);
    for (size_t i = RECEIVER_COUNT; i > 0; --i)
        endpoint_close(&receivers[i - 1]);
    endpoint_close(&sender);
    return result;
}
