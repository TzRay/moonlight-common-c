#include "Limelight-internal.h"
#include "VideoDiagnostics.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
int NegotiatedVideoFormat;
static int clockCalls, submitCalls, logCount;
static char logs[16][512];

uint64_t PltGetMicroseconds(void) {
    return (uint64_t)++clockCalls * 100;
}

static int fakeSubmit(PDECODE_UNIT unit) {
    (void)unit;
    submitCalls++;
    return DR_NEED_IDR;
}

static void captureLog(const char* format, ...) {
    assert(logCount < 16);
    va_list args;
    va_start(args, format);
    vsnprintf(logs[logCount++], sizeof(logs[0]), format, args);
    va_end(args);
}

int main(void) {
    ListenerCallbacks.logMessage = captureLog;
    VideoCallbacks.submitDecodeUnit = fakeSubmit;
    initializeVideoDiagnostics();
    assert(submitVideoDecodeUnit(NULL) == DR_NEED_IDR);
    assert(submitCalls == 1 && clockCalls == 0);
    configureVideoReceiveThread();
    flushVideoDiagnostics();
    assert(logCount == 0);

    StreamConfig.videoReceiveOptions = VIDEO_RECEIVE_TIMING_STATS;
    assert(submitVideoDecodeUnit(NULL) == DR_NEED_IDR);
    assert(submitCalls == 2 && clockCalls == 2);
    recordVideoTiming(VIDEO_TIMING_PACKET_PROCESS, 1000, 1100);
    recordVideoTiming(VIDEO_TIMING_PACKET_PROCESS, 1200, 1199); // invalid clock ordering
    recordVideoTiming(VIDEO_TIMING_PACKET_PROCESS, 10001000, 10001100);
    assert(logCount == 1);
    assert(strstr(logs[0], "metric=packet_process n=2 "));
    assert(strstr(logs[0], "max_us=100"));
    flushVideoDiagnostics();
    assert(logCount == 2 && strstr(logs[1], "metric=decode_submit n=1 "));
    flushVideoDiagnostics();
    assert(logCount == 2);

    // A new session cannot inherit samples or produce empty summaries.
    recordVideoTiming(VIDEO_TIMING_FEC_DECODE, 100, 200);
    initializeVideoDiagnostics();
    flushVideoDiagnostics();
    assert(logCount == 2);
    puts("Video diagnostics lifecycle tests passed");
    return 0;
}
