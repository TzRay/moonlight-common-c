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

static bool encryptedReplay, lateOriginal, continuousTraffic;
static unsigned latePackets;
static PPLT_CRYPTO_CONTEXT encryptCtx;
static void transmitEncrypted(const void* bytes, size_t size) {
    uint8_t wire[sizeof(ENC_VIDEO_HEADER) + WIRE] = {0};
    PENC_VIDEO_HEADER header = (PENC_VIDEO_HEADER)wire;
    static uint32_t iv;
    iv++;
    memcpy(header->iv, &iv, sizeof(iv));
    header->frameNumber = ((PNV_VIDEO_PACKET)((const uint8_t*)bytes + MAX_RTP_HEADER_SIZE))->frameIndex;
    int length;
    CHECK(PltEncryptMessage(encryptCtx, ALGORITHM_AES_GCM, 0,
                            (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                            header->iv, sizeof(header->iv), header->tag, sizeof(header->tag),
                            (unsigned char*)bytes, (int)size, wire + sizeof(*header), &length));
    transmit(wire, sizeof(*header) + length);
}

static unsigned probes, degraded, timedPolls, terminated, mediaPackets;
static unsigned middleOriginals, firstFramePackets;
static uint64_t initialObserved;
static uint32_t initialGaps;
static void checkEncryptedObservation(void);
static int terminationCode;
static void recordTermination(int code) { terminated++; terminationCode = code; }

static int receiveTimed(LI_ENDPOINT_REFRESH* state, SOCKET* s, char* packet, int size,
                        bool fallback, uint64_t deadline, uint64_t* timestamp) {
    CHECK(fallback == failTimeout);
    if (encryptedReplay && mediaPackets >= firstFramePackets) checkEncryptedObservation();
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
        if (lateOriginal && !latePackets++) {
            sendShard(NULL, GROUPS - 1, DATA - 1, true, 15);
        }
        if (continuousTraffic) {
            // Duplicate media from a finished group keeps the socket readable.
            sendShard(NULL, 0, 0, true, 0);
            nowUs += 1000;
        }
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
            CHECK(encryptedReplay || delivered == GROUPS * DATA);
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

static void checkEncryptedObservation(void) {
    uint64_t observed = LiGetObservedVideoDataPackets();
    uint32_t total = (uint32_t)observed - (uint32_t)initialObserved;
    uint32_t missing = (uint32_t)(observed >> 32) - (uint32_t)(initialObserved >> 32);
    // The abandoned last block lacks one original and cannot settle until frame two.
    bool nextFrame = mediaPackets > firstFramePackets;
    unsigned settled = middleOriginals ? 3 : 2;
    CHECK(rtpQueue.observation.frame == (nextFrame ? 2U : 1U));
    if (!nextFrame) CHECK(rtpQueue.observation.abandoned);
    CHECK(total == (settled + (unsigned)nextFrame) * DATA);
    CHECK(missing == (middleOriginals ? DATA - middleOriginals : 0) + (unsigned)nextFrame);
    CHECK(LiGetVideoPacketCoverageGaps() - initialGaps == (middleOriginals ? 0U : 1U));
}

static void runSocketReplay(bool refresh, bool fallback) {
    latePackets = 0;
    lost = 0;
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
    if (encryptedReplay) {
        initialObserved = LiGetObservedVideoDataPackets();
        initialGaps = LiGetVideoPacketCoverageGaps();
        firstFramePackets = 0;
        for (unsigned g = 0; g < GROUPS; g++) {
            unsigned count = g == 1 ? middleOriginals : DATA + 1;
            for (unsigned i = 0; i < count; i++) {
                if (g == GROUPS - 1 && i == DATA - 1) continue;
                sendShard(NULL, g, i, false, 0);
                firstFramePackets++;
            }
        }
        prepare(2, GROUPS * TOTAL);
        sendShard(NULL, 0, 0, false, 0);
        sendShard(NULL, 0, 0, false, 0); // Duplicate of the new frame's first packet
        prepare(1, 0);
        sendShard(NULL, GROUPS - 1, DATA - 1, false, 0); // Late original from the retired frame
    }
    else staticDesktop(&rtpQueue, 8);
    VideoReceiveThreadProc(NULL);
    CHECK(terminated == 1 && terminationCode == ML_ERROR_NO_VIDEO_TRAFFIC);
    if (encryptedReplay) {
        CHECK(!delivered && lost && !timedPolls && mediaPackets == firstFramePackets + 3);
        checkEncryptedObservation();
        uint64_t observed = LiGetObservedVideoDataPackets();
        printf("encrypted next frame, previous middle block %u/%u: total %u, missing %u, unknown blocks %u\n",
               middleOriginals, DATA, (uint32_t)observed - (uint32_t)initialObserved,
               (uint32_t)(observed >> 32) - (uint32_t)(initialObserved >> 32),
               LiGetVideoPacketCoverageGaps() - initialGaps);
    }
    else {
        CHECK(delivered == GROUPS * DATA && repaired == !lateOriginal && !lost);
        CHECK(mediaPackets >= 3 * (DATA + 1) + DATA - 1 + 8);
        CHECK(timedPolls == (!lateOriginal && !continuousTraffic));
    }
    CHECK(probes == (unsigned)refresh && degraded == (unsigned)refresh);
    CHECK(lastVideoDataTimeMs >= 1000 && firstDataTimeMs == 1000);

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
    lateOriginal = true;
    runSocketReplay(false, false);
    lateOriginal = false;
    continuousTraffic = true;
    runSocketReplay(false, false);
    continuousTraffic = false;
    encryptedReplay = true;
    EncryptionFeaturesEnabled |= SS_ENC_VIDEO;
    encryptCtx = PltCreateCryptoContext();
    decryptionCtx = PltCreateCryptoContext();
    CHECK(encryptCtx && decryptionCtx);
    sendOnSocket = transmitEncrypted;
    runSocketReplay(false, false);
    middleOriginals = 9;
    runSocketReplay(false, false);
    PltDestroyCryptoContext(decryptionCtx);
    PltDestroyCryptoContext(encryptCtx);
    closeSocket(sender);
    cleanupPlatform();
    puts("static frame over loopback: all refresh and SO_RCVTIMEO configurations passed");
    return 0;
}
