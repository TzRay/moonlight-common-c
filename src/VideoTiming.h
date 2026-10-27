#pragma once

#include <stdint.h>
#include <string.h>

// Fixed buckets, no allocations or locks. Values above one second are retained.
// Keep the Android StreamTimingHistogram boundaries in sync.
static const uint64_t videoTimingUpperBoundsUs[] = {
    0, 50, 100, 250, 500, 1000, 2000, 4000, 8000, 12000,
    16000, 20000, 25000, 33000, 50000, 75000, 100000,
    200000, 500000, 1000000, UINT64_MAX
};
#define VIDEO_TIMING_BUCKETS (sizeof(videoTimingUpperBoundsUs) / sizeof(videoTimingUpperBoundsUs[0]))

typedef struct _VIDEO_TIMING_HISTOGRAM {
    uint64_t buckets[VIDEO_TIMING_BUCKETS];
    uint64_t count;
    uint64_t maxUs;
} VIDEO_TIMING_HISTOGRAM;

static inline void videoTimingRecord(VIDEO_TIMING_HISTOGRAM* histogram, uint64_t durationUs) {
    unsigned int low = 0, high = VIDEO_TIMING_BUCKETS - 1;
    while (low < high) {
        unsigned int mid = (low + high) / 2;
        if (durationUs <= videoTimingUpperBoundsUs[mid]) high = mid;
        else low = mid + 1;
    }
    histogram->buckets[low]++;
    histogram->count++;
    if (durationUs > histogram->maxUs) histogram->maxUs = durationUs;
}

static inline uint64_t videoTimingPercentile(const VIDEO_TIMING_HISTOGRAM* histogram,
                                            unsigned int percent) {
    if (histogram->count == 0 || percent == 0 || percent > 100) return 0;
    uint64_t rank = (histogram->count * percent + 99) / 100;
    uint64_t cumulative = 0;
    for (unsigned int i = 0; i < VIDEO_TIMING_BUCKETS; i++) {
        cumulative += histogram->buckets[i];
        if (cumulative >= rank) {
            return videoTimingUpperBoundsUs[i] < histogram->maxUs ?
                videoTimingUpperBoundsUs[i] : histogram->maxUs;
        }
    }
    return histogram->maxUs;
}

static inline void videoTimingReset(VIDEO_TIMING_HISTOGRAM* histogram) {
    memset(histogram, 0, sizeof(*histogram));
}
