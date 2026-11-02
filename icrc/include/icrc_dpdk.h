#ifndef NODE_B_ICRC_DPDK_H
#define NODE_B_ICRC_DPDK_H

#include "icrc.h"

struct icrc_dpdk;

/* EAL must be initialized first. Creation includes a portable differential
 * self-test. The returned backend remains valid until icrc_dpdk_destroy(). */
int icrc_dpdk_create(struct icrc_dpdk **instance);
const struct icrc_backend *icrc_dpdk_backend(const struct icrc_dpdk *instance);
void icrc_dpdk_destroy(struct icrc_dpdk **instance);

#endif
