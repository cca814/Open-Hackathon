#ifndef NODE_B_METRICS_H
#define NODE_B_METRICS_H

#include <stdint.h>

#define METRIC_BUCKETS 64u

struct metric_summary {
    uint64_t count, total, minimum, maximum;
    uint64_t bucket[METRIC_BUCKETS];
};

void metric_observe(struct metric_summary *summary, uint64_t ticks);
void metric_merge(struct metric_summary *destination,
                  const struct metric_summary *source);
/* Returns the inclusive upper-bound tick value of the selected logarithmic
 * bucket. percentile is an integer in [1, 100]. Zero means no samples. */
uint64_t metric_percentile_upper(const struct metric_summary *summary,
                                 unsigned percentile);
uint64_t metric_ticks_to_ns(uint64_t ticks, uint64_t ticks_per_second);

#endif
