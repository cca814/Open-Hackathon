#include "metrics.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    struct metric_summary first = {0};
    const uint64_t values[] = {0, 1, 2, 3, 4, 8};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        metric_observe(&first, values[i]);
    assert(first.count == 6 && first.total == 18);
    assert(first.minimum == 0 && first.maximum == 8);
    assert(metric_percentile_upper(&first, 50) == 3);
    assert(metric_percentile_upper(&first, 99) == 15);
    assert(metric_ticks_to_ns(5, UINT64_C(1000000000)) == 5);

    struct metric_summary second = {0};
    metric_observe(&second, 16);
    metric_merge(&first, &second);
    assert(first.count == 7 && first.total == 34 && first.maximum == 16);
    assert(metric_percentile_upper(&first, 100) == 31);
    assert(metric_percentile_upper(&(struct metric_summary){0}, 50) == 0);
    puts("metric summary, percentile and tick conversion tests passed");
    return 0;
}
