// Run the actual receive loop, endpoint maintenance, UDP waits and RS queue.
// Advance the media clock at a poll timeout so scheduler jitter cannot reorder
// a queued ACK and the deadline. Only socket-option failure is injected.
#include "VideoFecFixture.h"
#undef PltGetMicroseconds
uint64_t PltGetMicroseconds(void);

static uint64_t mediaMillis(void) { return nowUs / 1000; }
static uint64_t endpointOffsetMs;
static uint64_t endpointMillis(void) { return mediaMillis() + endpointOffsetMs; }
#define PltGetMillis endpointMillis
#include "../src/EndpointRefresh.c"
#undef PltGetMillis

static bool failTimeout;
static int setTimeout(SOCKET socket, int ms) {
    CHECK(ms == UDP_RECV_POLL_TIMEOUT_MS);
    return failTimeout ? -1 : setNonFatalRecvTimeoutMs(socket, ms);
}

static SOCKET sender;
static struct sockaddr_storage receiverAddr;

static void transmit(const void* bytes, size_t size) {
    CHECK(sendto(sender, (const char*)bytes, (int)size, 0,
                 (struct sockaddr*)&receiverAddr, AddrLen) == (int)size);
}

static unsigned probes, degraded, timedPolls, terminated, mediaPackets;
static int terminationCode;
static void recordTermination(int code) { terminated++; terminationCode = code; }

static int receiveTimed(LI_ENDPOINT_REFRESH* state, SOCKET* s, char* packet, int size,
                        bool fallback, uint64_t deadline, uint64_t* timestamp) {
    CHECK(fallback == failTimeout);
    uint64_t realDeadline = 0;
    bool enabled = LiIsEndpointAutoRefreshEnabled();
    if (deadline) {
        CHECK(deadline > nowUs && deadline - nowUs <= 12000);
        // Match the production socket clock while preserving the replay interval.
        realDeadline = PltGetMicroseconds() + (deadline - nowUs);
        if (enabled && !probes && !state->probeSent) {
            unsigned char ack[LI_ER_PACKET_SIZE];
            CHECK(LiErClientPing(state));
            CHECK(LiErEncode(ack, LI_ER_ACK, LI_ER_VIDEO, state->session, state->key, state->sequence, NULL));
            transmit(ack, sizeof(ack));
        }
        if (enabled && probes) endpointOffsetMs = 8100;
    }
    int result = LiErClientRecvTimed(state, s, packet, size, fallback, realDeadline, timestamp);
    if (result == LI_ER_RECV_HANDLED) {
        CHECK(state->ackSequence == state->sequence);
        probes++;
    } else if (result <= 0) {
        if (result == LI_ER_RECV_DEGRADED) degraded++;
        else CHECK(result == 0);
        if (deadline) {
            nowUs = deadline;
            timedPolls++;
        } else {
            // The full default wait after the static frame is actual silence.
            CHECK(delivered == GROUPS * DATA);
            nowUs += 10000000;
        }
    } else {
        mediaPackets++;
    }
    return result;
}

#define PltGetMillis mediaMillis
#define PltGetMicroseconds replayClockUs
#define LiErClientRecvTimed receiveTimed
#define setNonFatalRecvTimeoutMs setTimeout
#include "../src/VideoStream.c"

static void runSocketReplay(bool refresh, bool fallback) {
    nowUs = 1000000;
    endpointOffsetMs = 0;
    failTimeout = fallback;
    probes = degraded = timedPolls = terminated = mediaPackets = 0;
    receivedDataFromPeer = receivedFullFrame = false;
    lastVideoDataTimeMs = firstDataTimeMs = firstVideoWaitStartMs = 0;

    rtpSocket = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 1 << 20, 0);
    CHECK(rtpSocket != INVALID_SOCKET);
    SOCKADDR_LEN addrLen = sizeof(receiverAddr);
    CHECK(getsockname(rtpSocket, (struct sockaddr*)&receiverAddr, &addrLen) == 0);
    EndpointRefreshRequested = refresh;
    LiSetEndpointAutoRefreshEnabled(refresh);
    CHECK(LiErClientInitialize(&endpointRefresh, rtpSocket, LI_ER_VIDEO,
                               "0123456789abcdef", ntohs(((struct sockaddr_in*)&RemoteAddr)->sin_port)));

    RtpvInitializeQueue(&rtpQueue);
    prepare(1, 0);
    staticDesktop(&rtpQueue, 8);
    VideoReceiveThreadProc(NULL);
    CHECK(delivered == GROUPS * DATA && repaired == 1 && !lost);
    CHECK(mediaPackets == 3 * (DATA + 1) + DATA - 1 + 8);
    CHECK(timedPolls == 1 && terminated == 1 && terminationCode == ML_ERROR_NO_VIDEO_TRAFFIC);
    CHECK(probes == (unsigned)refresh && degraded == (unsigned)refresh);
    CHECK(lastVideoDataTimeMs == 1000 && firstDataTimeMs == 1000);

    RtpvCleanupQueue(&rtpQueue);
    LiErClientDestroy(&endpointRefresh);
    closeSocket(rtpSocket);
}

int main(void) {
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431;
    StreamConfig.packetSize = WIRE - MAX_RTP_HEADER_SIZE;
    StreamConfig.fps = 60;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    VideoFecInterleavingEnabled = true;
    ListenerCallbacks.connectionTerminated = recordTermination;

    AddrLen = sizeof(struct sockaddr_in);
    ((struct sockaddr_in*)&LocalAddr)->sin_family = AF_INET;
    ((struct sockaddr_in*)&LocalAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sender = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(sender != INVALID_SOCKET);
    SOCKADDR_LEN addrLen = sizeof(RemoteAddr);
    CHECK(getsockname(sender, (struct sockaddr*)&RemoteAddr, &addrLen) == 0);
    sendOnSocket = transmit;
    for (int refresh = 0; refresh <= 1; refresh++) {
        for (int fallback = 0; fallback <= 1; fallback++) runSocketReplay(refresh, fallback);
    }
    closeSocket(sender);
    cleanupPlatform();
    puts("static frame over loopback: all refresh and SO_RCVTIMEO configurations passed");
    return 0;
}
