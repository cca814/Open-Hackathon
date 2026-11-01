#ifndef NODE_B_GROUPS_H
#define NODE_B_GROUPS_H
#include <stdbool.h>
#include "roce.h"
#define GROUPS_MAX 64u
#define GROUP_MEMBERS_MAX 16u
#define MEMBERS_MAX (GROUPS_MAX * GROUP_MEMBERS_MAX)
#define RELIABLE_ACK_WINDOW 16384u
struct multicast_group {
    char id[64];
    struct roce_address destination;
    uint32_t dst_qpn;
    size_t first_member, member_count;
    bool reliable;
    uint32_t sender_initial_psn;
    struct roce_destination sender;
    struct roce_destination ack_source;
};
struct groups {
    size_t count, member_count;
    struct multicast_group group[GROUPS_MAX];
    struct roce_destination member[MEMBERS_MAX];
    uint32_t member_initial_psn[MEMBERS_MAX];
    bool member_initial_set[MEMBERS_MAX];
};
/* Strict schema, ASCII strings without escapes, decimal integer QPNs.
 * Rejects duplicate/unknown fields, duplicate keys/IDs/members and empty groups.
 * Returns a malloc-owned immutable table; output unchanged on failure. */
int groups_load(const char *path, struct groups **out, char *error, size_t error_size);
const struct multicast_group *groups_find(const struct groups *groups,
                                          const struct roce_address *address,
                                          uint32_t qpn);
/* Finds a reliable group/member for a receiver-generated ACK. */
const struct multicast_group *groups_find_feedback(const struct groups *groups,
                                                   const struct roce_address *source,
                                                   const struct roce_address *destination,
                                                   uint32_t sender_qpn,
                                                   uint16_t *member_mask);
#endif
