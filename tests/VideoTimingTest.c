#include "VideoTiming.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    VIDEO_TIMING_HISTOGRAM histogram = {0};
    assert(videoTimingPercentile(&histogram, 99) == 0);
    for (int i = 0; i < 94; i++) videoTimingRecord(&histogram, 100);
    for (int i = 0; i < 5; i++) videoTimingRecord(&histogram, 40000);
    videoTimingRecord(&histogram, 2000000);
    assert(histogram.count == 100);
    assert(videoTimingPercentile(&histogram, 95) == 50000);
    assert(videoTimingPercentile(&histogram, 99) == 50000);
    assert(videoTimingPercentile(&histogram, 100) == 2000000);
    assert(histogram.maxUs == 2000000);
    videoTimingReset(&histogram);
    assert(histogram.count == 0 && histogram.maxUs == 0);
    videoTimingRecord(&histogram, 0);
    assert(videoTimingPercentile(&histogram, 99) == 0);
    videoTimingRecord(&histogram, UINT64_MAX);
    assert(videoTimingPercentile(&histogram, 99) == UINT64_MAX);
    assert(videoTimingPercentile(&histogram, 0) == 0);
    assert(videoTimingPercentile(&histogram, 101) == 0);
    puts("Video timing histogram tests passed");
    return 0;
}
