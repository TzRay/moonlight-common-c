#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static cpu_set_t originalMask, currentMask, requestedMask;
static int getCalls, setCalls;
static bool failGet, failSet, failVerify, invalidVerify, failRestore, clampMask;

static int fakeGetAffinity(pid_t pid, size_t size, cpu_set_t* mask) {
    assert(pid == 0 && size == sizeof(*mask));
    getCalls++;
    if ((getCalls == 1 && failGet) || (getCalls == 2 && failVerify)) {
        errno = EACCES;
        return -1;
    }
    *mask = currentMask;
    if (getCalls == 2 && invalidVerify) CPU_SET(0, mask);
    return 0;
}

static int fakeSetAffinity(pid_t pid, size_t size, const cpu_set_t* mask) {
    assert(pid == 0 && size == sizeof(*mask));
    setCalls++;
    if ((setCalls == 1 && failSet) || (setCalls == 2 && failRestore)) {
        errno = EPERM;
        return -1;
    }
    if (setCalls == 1) requestedMask = *mask;
    currentMask = *mask;
    if (clampMask) CPU_CLR(5, &currentMask);
    return 0;
}

#define sched_getaffinity fakeGetAffinity
#define sched_setaffinity fakeSetAffinity
#include "VideoThreadAffinity.h"

static void reset(void) {
    CPU_ZERO(&originalMask);
    CPU_SET(0, &originalMask);
    CPU_SET(5, &originalMask);
    CPU_SET(12, &originalMask);
    currentMask = originalMask;
    CPU_ZERO(&requestedMask);
    getCalls = setCalls = 0;
    failGet = failSet = failVerify = invalidVerify = failRestore = clampMask = false;
}

int main(void) {
    reset();
    assert(strcmp(applyVideoReceiveAffinity(false).status, "default") == 0);
    assert(getCalls == 0 && setCalls == 0);

    reset();
    VIDEO_AFFINITY_RESULT result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "applied") == 0 && result.allowedCpuCount == 2);
    assert(!CPU_ISSET(0, &requestedMask) && CPU_ISSET(5, &requestedMask) && CPU_ISSET(12, &requestedMask));
    assert(CPU_COUNT(&requestedMask) == 2 && !result.cpu0Allowed);

    reset();
    CPU_CLR(0, &currentMask);
    assert(strcmp(applyVideoReceiveAffinity(true).status, "already_excluded") == 0);
    assert(setCalls == 0);

    reset();
    CPU_ZERO(&currentMask);
    CPU_SET(0, &currentMask);
    assert(strcmp(applyVideoReceiveAffinity(true).status, "no_alternative_cpu") == 0);
    assert(setCalls == 0 && CPU_ISSET(0, &currentMask));

    reset();
    clampMask = true;
    result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "applied") == 0 && result.allowedCpuCount == 1);
    assert(CPU_ISSET(12, &currentMask));

    reset();
    failGet = true;
    result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "get_failed") == 0 && result.error == EACCES);
    assert(setCalls == 0);

    reset();
    failSet = true;
    result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "set_failed") == 0 && result.error == EPERM);
    assert(CPU_EQUAL(&originalMask, &currentMask));

    reset();
    failVerify = true;
    result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "verify_failed") == 0 && result.error == EACCES);
    assert(result.restoreError == 0 && setCalls == 2 && CPU_EQUAL(&originalMask, &currentMask));

    reset();
    invalidVerify = failRestore = true;
    result = applyVideoReceiveAffinity(true);
    assert(strcmp(result.status, "verify_failed") == 0 && result.error == EINVAL);
    assert(result.restoreError == EPERM && result.allowedCpuCount == -1);
    puts("Video thread affinity tests passed (9 scenarios)");
    return 0;
}
