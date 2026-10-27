#pragma once

#include <stdbool.h>
#include <errno.h>

typedef struct _VIDEO_AFFINITY_RESULT {
    const char* status;
    int error;
    int restoreError;
    int allowedCpuCount;
    int cpu0Allowed; // -1 when not observed
} VIDEO_AFFINITY_RESULT;

#ifdef __linux__
#include <sched.h>

// Call only from VideoRecv itself. Never widen its inherited/cpuset CPU mask.
// The caller must define _GNU_SOURCE before including any libc headers.
static inline VIDEO_AFFINITY_RESULT applyVideoReceiveAffinity(bool avoidCpu0) {
    VIDEO_AFFINITY_RESULT result = { .status = "default", .allowedCpuCount = -1, .cpu0Allowed = -1 };
    cpu_set_t original, requested, effective;
    if (!avoidCpu0) return result;

    if (sched_getaffinity(0, sizeof(original), &original) != 0) {
        result.status = "get_failed";
        result.error = errno;
        return result;
    }
    result.allowedCpuCount = CPU_COUNT(&original);
    result.cpu0Allowed = CPU_ISSET(0, &original);
    if (!result.cpu0Allowed) {
        result.status = "already_excluded";
        return result;
    }

    requested = original;
    CPU_CLR(0, &requested);
    if (CPU_COUNT(&requested) == 0) {
        result.status = "no_alternative_cpu";
        return result;
    }
    if (sched_setaffinity(0, sizeof(requested), &requested) != 0) {
        result.status = "set_failed";
        result.error = errno;
        return result;
    }

    // Success can still be silently intersected with the current cpuset.
    int verifyResult = sched_getaffinity(0, sizeof(effective), &effective);
    if (verifyResult != 0 || CPU_COUNT(&effective) == 0 || CPU_ISSET(0, &effective)) {
        result.status = "verify_failed";
        result.error = verifyResult != 0 ? errno : EINVAL;
        result.allowedCpuCount = -1;
        result.cpu0Allowed = -1;
        if (sched_setaffinity(0, sizeof(original), &original) != 0) {
            result.restoreError = errno;
        }
        return result;
    }

    result.status = "applied";
    result.allowedCpuCount = CPU_COUNT(&effective);
    result.cpu0Allowed = false;
    return result;
}
#else
static inline VIDEO_AFFINITY_RESULT applyVideoReceiveAffinity(bool avoidCpu0) {
    VIDEO_AFFINITY_RESULT result = {
        .status = avoidCpu0 ? "unsupported" : "default", .allowedCpuCount = -1, .cpu0Allowed = -1
    };
    return result;
}
#endif
