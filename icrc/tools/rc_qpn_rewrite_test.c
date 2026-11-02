/* SPDX-License-Identifier: BSD-2-Clause
 * Minimal single-process RC SEND acceptance test for the Node B QPN rewrite.
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

enum { BUFFER_SIZE = 256, SENDER_PSN = 0x123456, RECEIVER_PSN = 0x654321 };
static const char payload[] = "milestone-2-rc-send-through-qpn-rewrite";

struct endpoint {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    void *buffer;
    union ibv_gid gid;
};

static struct ibv_context *open_device(const char *name)
{
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices)
        return NULL;
    struct ibv_context *context = NULL;
    for (int i = 0; i < count; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), name) == 0) {
            context = ibv_open_device(devices[i]);
            break;
        }
    }
    ibv_free_device_list(devices);
    return context;
}

static int endpoint_open(struct endpoint *endpoint, const char *device, int gid_index)
{
    endpoint->context = open_device(device);
    if (!endpoint->context) {
        fprintf(stderr, "cannot open RDMA device %s\n", device);
        return -1;
    }
    if (ibv_query_gid(endpoint->context, 1, gid_index, &endpoint->gid)) {
        fprintf(stderr, "cannot query %s GID index %d: %s\n",
                device, gid_index, strerror(errno));
        return -1;
    }
    endpoint->pd = ibv_alloc_pd(endpoint->context);
    endpoint->cq = ibv_create_cq(endpoint->context, 8, NULL, NULL, 0);
    if (!endpoint->pd || !endpoint->cq ||
        posix_memalign(&endpoint->buffer, 64, BUFFER_SIZE) != 0)
        return -1;
    memset(endpoint->buffer, 0, BUFFER_SIZE);
    endpoint->mr = ibv_reg_mr(endpoint->pd, endpoint->buffer, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    if (!endpoint->mr)
        return -1;
    struct ibv_qp_init_attr init = {
        .send_cq = endpoint->cq,
        .recv_cq = endpoint->cq,
        .cap = {
            .max_send_wr = 4,
            .max_recv_wr = 4,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    endpoint->qp = ibv_create_qp(endpoint->pd, &init);
    if (!endpoint->qp)
        return -1;
    return 0;
}

static struct ibv_qp *create_dummy_qp(struct endpoint *endpoint)
{
    struct ibv_qp_init_attr init = {
        .send_cq = endpoint->cq,
        .recv_cq = endpoint->cq,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
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
                     const union ibv_gid *remote_gid, int local_gid_index)
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
            .dlid = 0,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = 1,
            .grh = {
                .flow_label = 0,
                .sgid_index = local_gid_index,
                .hop_limit = 64,
                .traffic_class = 0,
            },
        },
    };
    attr.ah_attr.grh.dgid = *remote_gid;
    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
               IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    return ibv_modify_qp(qp, &attr, mask);
}

static int qp_to_rts(struct ibv_qp *qp, uint32_t local_psn)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = local_psn,
        .max_rd_atomic = 1,
    };
    int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
               IBV_QP_MAX_QP_RD_ATOMIC;
    return ibv_modify_qp(qp, &attr, mask);
}

static int post_receive(struct endpoint *receiver)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)receiver->buffer,
        .length = BUFFER_SIZE,
        .lkey = receiver->mr->lkey,
    };
    struct ibv_recv_wr wr = {.wr_id = 2, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(receiver->qp, &wr, &bad);
}

static int post_send(struct endpoint *sender)
{
    memcpy(sender->buffer, payload, sizeof(payload));
    struct ibv_sge sge = {
        .addr = (uintptr_t)sender->buffer,
        .length = sizeof(payload),
        .lkey = sender->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 1,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(sender->qp, &wr, &bad);
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int wait_for_completion(struct endpoint *endpoint, enum ibv_wc_opcode opcode,
                               const char *label)
{
    uint64_t deadline = monotonic_milliseconds() + 15000;
    while (monotonic_milliseconds() < deadline) {
        struct ibv_wc wc;
        int count = ibv_poll_cq(endpoint->cq, 1, &wc);
        if (count < 0) {
            fprintf(stderr, "%s CQ poll failed\n", label);
            return -1;
        }
        if (count == 0)
            continue;
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != opcode) {
            fprintf(stderr, "%s completion failed: status=%s opcode=%d\n",
                    label, ibv_wc_status_str(wc.status), wc.opcode);
            return -1;
        }
        printf("%s_cqe status=success byte_len=%u\n", label, wc.byte_len);
        return 0;
    }
    fprintf(stderr, "%s completion timed out\n", label);
    return -1;
}

static int write_config(const char *path, const char *receiver_gid,
                        const char *receiver_mac, uint32_t virtual_qpn,
                        uint32_t receiver_qpn)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    int result = fprintf(file,
        "{\n"
        "  \"groups\": [\n"
        "    {\n"
        "      \"id\": \"milestone-2-live-rc\",\n"
        "      \"match\": {\"dst_ipv6\": \"%s\", \"dst_qpn\": %u},\n"
        "      \"members\": [\n"
        "        {\"dst_mac\": \"%s\", \"dst_ipv6\": \"%s\", \"dst_qpn\": %u}\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n",
        receiver_gid, virtual_qpn, receiver_mac, receiver_gid, receiver_qpn);
    if (result < 0 || fclose(file) != 0)
        return -1;
    return 0;
}

static void endpoint_close(struct endpoint *endpoint)
{
    if (endpoint->qp)
        ibv_destroy_qp(endpoint->qp);
    if (endpoint->mr)
        ibv_dereg_mr(endpoint->mr);
    free(endpoint->buffer);
    if (endpoint->cq)
        ibv_destroy_cq(endpoint->cq);
    if (endpoint->pd)
        ibv_dealloc_pd(endpoint->pd);
    if (endpoint->context)
        ibv_close_device(endpoint->context);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 6) {
        fprintf(stderr, "Usage: %s CONFIG [SENDER_DEV [RECEIVER_DEV [GID_INDEX [RECEIVER_MAC]]]]\n",
                argv[0]);
        return 2;
    }
    const char *sender_device = argc > 2 ? argv[2] : "mlx5_2";
    const char *receiver_device = argc > 3 ? argv[3] : "mlx5_3";
    int gid_index = argc > 4 ? atoi(argv[4]) : 0;
    const char *receiver_mac = argc > 5 ? argv[5] : "02:7e:6d:fd:79:7c";
    struct endpoint sender = {0}, receiver = {0};
    struct ibv_qp *virtual_qp = NULL;
    int result = 1;

    if (endpoint_open(&sender, sender_device, gid_index) ||
        endpoint_open(&receiver, receiver_device, gid_index))
        goto done;
    virtual_qp = create_dummy_qp(&receiver);
    if (!virtual_qp || virtual_qp->qp_num == receiver.qp->qp_num) {
        fprintf(stderr, "cannot allocate distinct virtual receiver QP\n");
        goto done;
    }
    if (qp_to_init(sender.qp) || qp_to_init(receiver.qp) ||
        qp_to_rtr(receiver.qp, sender.qp->qp_num, SENDER_PSN,
                  &sender.gid, gid_index) ||
        qp_to_rts(receiver.qp, RECEIVER_PSN) ||
        qp_to_rtr(sender.qp, virtual_qp->qp_num, RECEIVER_PSN,
                  &receiver.gid, gid_index) ||
        qp_to_rts(sender.qp, SENDER_PSN) || post_receive(&receiver)) {
        fprintf(stderr, "QP setup failed: %s\n", strerror(errno));
        goto done;
    }

    char sender_gid[INET6_ADDRSTRLEN], receiver_gid[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, sender.gid.raw, sender_gid, sizeof(sender_gid)) ||
        !inet_ntop(AF_INET6, receiver.gid.raw, receiver_gid, sizeof(receiver_gid)) ||
        write_config(argv[1], receiver_gid, receiver_mac,
                     virtual_qp->qp_num, receiver.qp->qp_num))
        goto done;

    printf("READY sender_qpn=%u virtual_qpn=%u receiver_qpn=%u sender_gid=%s receiver_gid=%s config=%s\n",
           sender.qp->qp_num, virtual_qp->qp_num, receiver.qp->qp_num,
           sender_gid, receiver_gid, argv[1]);
    printf("Press Enter after Node B is running with this config.\n");
    fflush(stdout);
    if (getchar() == EOF) {
        fputs("control input closed before send\n", stderr);
        goto done;
    }
    if (post_send(&sender)) {
        fprintf(stderr, "send submission failed: %s\n", strerror(errno));
        goto done;
    }
    if (wait_for_completion(&receiver, IBV_WC_RECV, "receiver") ||
        memcmp(receiver.buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, "receiver payload verification failed\n");
        goto done;
    }
    if (wait_for_completion(&sender, IBV_WC_SEND, "sender"))
        goto done;
    puts("PASS rc_send_completed=true receiver_accepted=true qpn_rewritten=true ack_returned=true");
    result = 0;

done:
    if (virtual_qp)
        ibv_destroy_qp(virtual_qp);
    endpoint_close(&receiver);
    endpoint_close(&sender);
    return result;
}
