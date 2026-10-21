#ifndef NODE_B_GROUPS_H
#define NODE_B_GROUPS_H
#include "roce.h"
#define GROUPS_MAX 64u
#define GROUP_MEMBERS_MAX 16u
#define MEMBERS_MAX (GROUPS_MAX * GROUP_MEMBERS_MAX)
struct multicast_group {
    char id[64];
    uint32_t dst_ipv4, dst_qpn;
    size_t first_member, member_count;
};
struct groups {
    size_t count, member_count;
    struct multicast_group group[GROUPS_MAX];
    struct roce_destination member[MEMBERS_MAX];
};
/* Strict schema, ASCII strings without escapes, decimal integer QPNs.
 * Rejects duplicate/unknown fields, duplicate keys/IDs/members and empty groups.
 * Returns a malloc-owned immutable table; output unchanged on failure. */
int groups_load(const char *path, struct groups **out, char *error, size_t error_size);
const struct multicast_group *groups_find(const struct groups *groups,
                                          uint32_t ipv4, uint32_t qpn);
#endif
