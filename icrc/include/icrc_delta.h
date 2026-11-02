#ifndef NODE_B_ICRC_DELTA_H
#define NODE_B_ICRC_DELTA_H

#include <stddef.h>
#include <stdint.h>

struct icrc_delta;

struct icrc_change {
    size_t offset; /* IP-relative byte offset in the canonical ICRC body. */
    uint8_t before;
    uint8_t after;
};

/* Precomputes reflected CRC-32 byte contributions for every distance from the
 * end of an IP-through-payload body up to max_ip_length. */
int icrc_delta_create(size_t max_ip_length, struct icrc_delta **instance);
void icrc_delta_destroy(struct icrc_delta **instance);

/* Corrects a validated packet's numeric ICRC for equal-length byte changes.
 * The virtual LRH prefix need not be listed because it remains unchanged. */
int icrc_delta_correct(const struct icrc_delta *instance, uint32_t original,
                       size_t ip_length, const struct icrc_change *changes,
                       size_t change_count, uint32_t *corrected);

#endif
