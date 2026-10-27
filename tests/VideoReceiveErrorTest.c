// Exercise the real receive loop while clock/logging hooks deliberately clobber
// the socket error. No socket or streaming peer is needed for this failure path.
#define recvUdpSocket testRecvUdpSocket
#define setNonFatalRecvTimeoutMs testSetRecvTimeout
#define PltGetMicroseconds testClock
#include "../src/VideoStream.c"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static int socketError, terminationError, receives;

int testRecvUdpSocket(SOCKET socket, char* buffer, int size, bool useSelect) {
    (void)socket; (void)buffer; (void)size; (void)useSelect;
    receives++;
    SetLastSocketError(socketError);
    return -1;
}

int testSetRecvTimeout(SOCKET socket, int timeoutMs) {
    (void)socket; (void)timeoutMs;
    return 0;
}

uint64_t testClock(void) {
    SetLastSocketError(999);
    return 100;
}

static void clobberingLog(const char* format, ...) {
    (void)format;
    SetLastSocketError(998);
}

static void connectionTerminated(int error) {
    terminationError = error;
}

int main(void) {
    StreamConfig.packetSize = 1392;
    ListenerCallbacks.logMessage = clobberingLog;
    ListenerCallbacks.connectionTerminated = connectionTerminated;
    for (int timing = 0; timing < 2; timing++) {
        StreamConfig.videoReceiveOptions = timing ? VIDEO_RECEIVE_TIMING_STATS : 0;
        for (int missingError = 0; missingError < 2; missingError++) {
            socketError = missingError ? 0 : 123;
            terminationError = receives = 0;
            initializeVideoDiagnostics();
            VideoReceiveThreadProc(NULL);
            assert(receives == 1);
            assert(terminationError == (missingError ? -1 : 123));
        }
    }
    puts("Video receive error propagation tests passed");
    return 0;
}
