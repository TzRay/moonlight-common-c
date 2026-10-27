#pragma once

#include "Limelight.h"

typedef enum _VIDEO_TIMING_METRIC {
    VIDEO_TIMING_LOOP_GAP,
    VIDEO_TIMING_RECEIVE_WAIT,
    VIDEO_TIMING_PACKET_PROCESS,
    VIDEO_TIMING_FEC_DECODE,
    VIDEO_TIMING_DECODE_SUBMIT,
    VIDEO_TIMING_METRIC_COUNT
} VIDEO_TIMING_METRIC;

void initializeVideoDiagnostics(void);
void configureVideoReceiveThread(void);
void recordVideoTiming(VIDEO_TIMING_METRIC metric, uint64_t startUs, uint64_t endUs);
// All producers must have stopped before flushing (destroyVideoStream).
void flushVideoDiagnostics(void);
int submitVideoDecodeUnit(PDECODE_UNIT decodeUnit);
