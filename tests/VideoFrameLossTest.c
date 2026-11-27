// Drive the real connection-status sampler without waiting for a 32-bit wrap.
#include "../src/Limelight-internal.h"
#include <stdio.h>

static uint64_t clockMs;
static uint64_t testMillis(void) { return clockMs; }
#define PltGetMillis testMillis
#include "../src/ControlStream.c"

static unsigned poorUpdates;
static void recordStatus(int status) {
    if (status == CONN_STATUS_POOR) poorUpdates++;
}

int main(void) {
    if (initializePlatform() || initializeControlStream()) return 1;
    ListenerCallbacks.connectionStatusUpdate = recordStatus;
    // Approach the wrap during startup, always advancing less than 2^31 frames.
    connectionSawFrame(1);
    connectionSawFrame(INT32_MAX);
    connectionSawFrame(UINT32_MAX - 2);
    clockMs = 3000;
    connectionSawFrame(UINT32_MAX - 1);
    connectionReceivedCompleteFrame(UINT32_MAX - 1, false);
    connectionSawFrame(UINT32_MAX);
    clockMs = 4500;
    connectionSawFrame(0);
    connectionReceivedCompleteFrame(0, false);
    connectionSawFrame(1);
    clockMs = 6000;
    connectionSawFrame(2);
    double loss = LiGetEstimatedVideoFrameLossPercentage();
    printf("four frames across UINT32_MAX, two complete: loss %.2f%%, poor updates %u\n", loss, poorUpdates);
    destroyControlStream();
    cleanupPlatform();
    return loss == 50.0 && poorUpdates == 1 ? 0 : 1;
}
