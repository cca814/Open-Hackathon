#include "metrics.h"

#include <limits.h>

static unsigned bucket_for(uint64_t value)
{
    if (value == 0)
        return 0;
    unsigned bucket = 0;
    while (value >>= 1)
        ++bucket;
    return bucket;
}

void metric_observe(struct metric_summary *summary, uint64_t ticks)
{
    if (!summary)
        return;
    if (summary->count == 0 || ticks < summary->minimum)
        summary->minimum = ticks;
    if (ticks > summary->maximum)
        summary->maximum = ticks;
    ++summary->count;
    summary->total += ticks;
    ++summary->bucket[bucket_for(ticks)];
}

void metric_merge(struct metric_summary *destination,
                  const struct metric_summary *source)
{
    if (!destination || !source || source->count == 0)
        return;
    if (destination->count == 0 || source->minimum < destination->minimum)
        destination->minimum = source->minimum;
    if (source->maximum > destination->maximum)
        destination->maximum = source->maximum;
    destination->count += source->count;
    destination->total += source->total;
    for (unsigned i = 0; i < METRIC_BUCKETS; ++i)
        destination->bucket[i] += source->bucket[i];
}

uint64_t metric_percentile_upper(const struct metric_summary *summary,
                                 unsigned percentile)
{
    if (!summary || summary->count == 0 || percentile == 0 || percentile > 100)
        return 0;
    uint64_t target = summary->count / 100u * percentile +
                      (summary->count % 100u * percentile + 99u) / 100u;
    uint64_t seen = 0;
    for (unsigned i = 0; i < METRIC_BUCKETS; ++i) {
        seen += summary->bucket[i];
        if (seen >= target)
            return i == 63 ? UINT64_MAX : ((UINT64_C(1) << (i + 1)) - 1);
    }
    return summary->maximum;
}

uint64_t metric_ticks_to_ns(uint64_t ticks, uint64_t ticks_per_second)
{
    if (!ticks_per_second)
        return 0;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remainder = ticks % ticks_per_second;
    if (seconds > UINT64_MAX / UINT64_C(1000000000))
        return UINT64_MAX;
    return seconds * UINT64_C(1000000000) +
           remainder * UINT64_C(1000000000) / ticks_per_second;
}
