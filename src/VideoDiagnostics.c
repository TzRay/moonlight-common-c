#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "Limelight-internal.h"
#include "VideoDiagnostics.h"
#include "VideoThreadAffinity.h"
#include "VideoTiming.h"

#include <inttypes.h>

#define VIDEO_TIMING_REPORT_US 10000000

typedef struct _VIDEO_TIMING_WINDOW {
    VIDEO_TIMING_HISTOGRAM histogram;
    uint64_t startUs;
    uint64_t endUs;
} VIDEO_TIMING_WINDOW;

// Each metric has exactly one writer during a session. FEC and receive metrics
// belong to VideoRecv; submit belongs to VideoRecv OR VideoDec, never both.
// Initialization/flush run before creation/after join, so no hot-path locks or
// per-packet atomic counters are needed.
static VIDEO_TIMING_WINDOW timingWindows[VIDEO_TIMING_METRIC_COUNT];
static const char* metricNames[VIDEO_TIMING_METRIC_COUNT] = {
    "loop_gap", "receive_wait", "packet_process", "fec_decode", "decode_submit"
};

void initializeVideoDiagnostics(void) {
    memset(timingWindows, 0, sizeof(timingWindows));
}

static int currentCpu(void) {
#ifdef __linux__
    return sched_getcpu();
#else
    return -1;
#endif
}

void configureVideoReceiveThread(void) {
    bool avoidCpu0 = (StreamConfig.videoReceiveOptions & VIDEO_RECEIVE_AVOID_CPU0) != 0;
    if (!avoidCpu0 && !(StreamConfig.videoReceiveOptions & VIDEO_RECEIVE_TIMING_STATS)) return;

    VIDEO_AFFINITY_RESULT affinity = applyVideoReceiveAffinity(avoidCpu0);
    Limelog("SD[affinity] thread=VideoRecv requested=%d status=%s errno=%d restore_errno=%d "
            "allowed_cpus=%d cpu0_allowed=%d running_cpu=%d\n",
            avoidCpu0, affinity.status, affinity.error, affinity.restoreError,
            affinity.allowedCpuCount, affinity.cpu0Allowed, currentCpu());
    // allowed_cpus/cpu0_allowed are only observations when a get succeeded.
    // In default mode we deliberately do not query or change the CPU mask.
    Limelog("SD[session] video_format=0x%x fps=%d timing=%d direct_submit_cap=%d fec_validation=%d\n",
            NegotiatedVideoFormat, StreamConfig.fps,
            !!(StreamConfig.videoReceiveOptions & VIDEO_RECEIVE_TIMING_STATS),
            !!(VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT),
#if defined(LC_DEBUG) && !defined(LC_FUZZING)
            1
#else
            0
#endif
    );
}

static void reportVideoTiming(VIDEO_TIMING_METRIC metric) {
    VIDEO_TIMING_WINDOW* window = &timingWindows[metric];
    VIDEO_TIMING_HISTOGRAM* histogram = &window->histogram;
    if (histogram->count == 0) return;

    Limelog("SD[native] metric=%s n=%" PRIu64 " span_ms=%" PRIu64
            " p95_us_le=%" PRIu64 " p99_us_le=%" PRIu64 " max_us=%" PRIu64 "\n",
            metricNames[metric], histogram->count, (window->endUs - window->startUs) / 1000,
            videoTimingPercentile(histogram, 95), videoTimingPercentile(histogram, 99),
            histogram->maxUs);
    videoTimingReset(histogram);
}

void recordVideoTiming(VIDEO_TIMING_METRIC metric, uint64_t startUs, uint64_t endUs) {
    if (endUs < startUs) return;
    VIDEO_TIMING_WINDOW* window = &timingWindows[metric];
    if (window->histogram.count == 0) window->startUs = endUs;
    window->endUs = endUs;
    videoTimingRecord(&window->histogram, endUs - startUs);
    if (endUs - window->startUs >= VIDEO_TIMING_REPORT_US) {
        reportVideoTiming(metric);
        if (metric == VIDEO_TIMING_LOOP_GAP) {
            int allowedCpuCount = -1, cpu0Allowed = -1;
#ifdef __linux__
            cpu_set_t effective;
            if (sched_getaffinity(0, sizeof(effective), &effective) == 0) {
                allowedCpuCount = CPU_COUNT(&effective);
                cpu0Allowed = CPU_ISSET(0, &effective);
            }
#endif
            Limelog("SD[cpu] thread=VideoRecv running_cpu=%d allowed_cpus=%d cpu0_allowed=%d\n",
                    currentCpu(), allowedCpuCount, cpu0Allowed);
        }
    }
}

void flushVideoDiagnostics(void) {
    for (int metric = 0; metric < VIDEO_TIMING_METRIC_COUNT; metric++) {
        reportVideoTiming((VIDEO_TIMING_METRIC)metric);
    }
}

int submitVideoDecodeUnit(PDECODE_UNIT decodeUnit) {
    if (!(StreamConfig.videoReceiveOptions & VIDEO_RECEIVE_TIMING_STATS)) {
        return VideoCallbacks.submitDecodeUnit(decodeUnit);
    }
    uint64_t startUs = PltGetMicroseconds();
    int result = VideoCallbacks.submitDecodeUnit(decodeUnit);
    recordVideoTiming(VIDEO_TIMING_DECODE_SUBMIT, startUs, PltGetMicroseconds());
    return result;
}
