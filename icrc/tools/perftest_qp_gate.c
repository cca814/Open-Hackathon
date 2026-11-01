#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * LD_PRELOAD gate for a single-QP perftest run.  It records the QPN/PSNs at
 * RTR/RTS and pauses immediately before the RTS transition so Node B can be
 * configured with perftest's runtime connection metadata.
 */

struct qp_record {
    struct ibv_qp *qp;
    uint32_t dest_qpn;
    uint32_t rq_psn;
};

static struct qp_record records[16];
static size_t record_count;

static struct qp_record *record_for(struct ibv_qp *qp)
{
    for (size_t i = 0; i < record_count; ++i)
        if (records[i].qp == qp)
            return &records[i];
    if (record_count == sizeof(records) / sizeof(records[0]))
        return NULL;
    records[record_count] = (struct qp_record){.qp = qp};
    return &records[record_count++];
}

static int wait_for_release(const char *directory)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/release", directory) >= (int)sizeof(path))
        return -1;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
    for (unsigned attempt = 0; attempt < 6000; ++attempt) {
        if (access(path, F_OK) == 0)
            return 0;
        if (errno != ENOENT || nanosleep(&pause, NULL) != 0)
            return -1;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int publish_and_wait(struct ibv_qp *qp, const struct ibv_qp_attr *attr,
                            const struct qp_record *record, const char *directory)
{
    char final_path[PATH_MAX], temporary_path[PATH_MAX];
    if (snprintf(final_path, sizeof(final_path), "%s/%ld.meta", directory,
                 (long)getpid()) >= (int)sizeof(final_path) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s/%ld.tmp", directory,
                 (long)getpid()) >= (int)sizeof(temporary_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(temporary_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    const char *device = ibv_get_device_name(qp->context->device);
    char line[256];
    int length = snprintf(line, sizeof(line), "%s,%u,%u,%u,%u\n", device,
                          qp->qp_num, record->dest_qpn, record->rq_psn,
                          attr->sq_psn);
    int failed = length < 0 || length >= (int)sizeof(line) ||
                 write(fd, line, (size_t)length) != length || fsync(fd) != 0;
    if (close(fd) != 0)
        failed = 1;
    if (failed || rename(temporary_path, final_path) != 0)
        return -1;
    return wait_for_release(directory);
}

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
    static int (*real_modify_qp)(struct ibv_qp *, struct ibv_qp_attr *, int);
    if (real_modify_qp == NULL) {
        void *symbol = dlsym(RTLD_NEXT, "ibv_modify_qp");
        memcpy(&real_modify_qp, &symbol, sizeof(real_modify_qp));
        if (real_modify_qp == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }
    if (getenv("PERFTEST_QP_TRACE") != NULL &&
        (attr_mask & IBV_QP_STATE) != 0 && attr->qp_state == IBV_QPS_RTR) {
        fprintf(stderr,
                "QP_TRACE device=%s qp=%u mask=0x%x mtu=%u dest_qpn=%u rq_psn=%u "
                "max_dest_rd_atomic=%u min_rnr_timer=%u is_global=%u port=%u "
                "sgid_index=%u hop_limit=%u dgid=",
                ibv_get_device_name(qp->context->device), qp->qp_num, attr_mask,
                attr->path_mtu, attr->dest_qp_num, attr->rq_psn,
                attr->max_dest_rd_atomic, attr->min_rnr_timer,
                attr->ah_attr.is_global, attr->ah_attr.port_num,
                attr->ah_attr.grh.sgid_index, attr->ah_attr.grh.hop_limit);
        for (size_t i = 0; i < sizeof(attr->ah_attr.grh.dgid.raw); ++i)
            fprintf(stderr, "%02x", attr->ah_attr.grh.dgid.raw[i]);
        fputc('\n', stderr);
        fflush(stderr);
    }
    const char *directory = getenv("PERFTEST_QP_GATE_DIR");
    if (directory != NULL && (attr_mask & IBV_QP_STATE) != 0) {
        struct qp_record *record = record_for(qp);
        if (record == NULL) {
            errno = ENOSPC;
            return -1;
        }
        if (attr->qp_state == IBV_QPS_RTR) {
            if ((attr_mask & (IBV_QP_DEST_QPN | IBV_QP_RQ_PSN)) !=
                (IBV_QP_DEST_QPN | IBV_QP_RQ_PSN)) {
                errno = EINVAL;
                return -1;
            }
            record->dest_qpn = attr->dest_qp_num;
            record->rq_psn = attr->rq_psn;
        } else if (attr->qp_state == IBV_QPS_RTS) {
            if ((attr_mask & IBV_QP_SQ_PSN) == 0 || record->dest_qpn == 0) {
                errno = EINVAL;
                return -1;
            }
            if (publish_and_wait(qp, attr, record, directory) != 0)
                return -1;
        }
    }
    return real_modify_qp(qp, attr, attr_mask);
}
