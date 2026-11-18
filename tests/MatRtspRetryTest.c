/* Exercise the production handshake against a local RTSP peer, with no media. */
#include "Limelight-internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    SOCKET listener;
    atomic_bool stop;
    int busyResponses, finalStatus, describes, previousSequence;
    bool cancel;
} PEER;

static void serve(void* context) {
    PEER* peer = context;
    while (!peer->stop) {
        struct pollfd pollFd = {peer->listener, POLLIN, 0};
        if (pollSockets(&pollFd, 1, 100) <= 0) continue;
        SOCKET client = accept(peer->listener, NULL, NULL);
        CHECK(client != INVALID_SOCKET);
        char request[4096] = {0};
        int used = 0;
        while (!strstr(request, "\r\n\r\n")) {
            int n = (int)recv(client, request + used, (int)sizeof(request) - 1 - used, 0);
            CHECK(n > 0);
            used += n;
            CHECK(used < (int)sizeof(request) - 1);
        }
        const char* sequence = strstr(request, "CSeq: ");
        CHECK(sequence != NULL);
        int current = atoi(sequence + 6);
        CHECK(current > peer->previousSequence);
        peer->previousSequence = current;
        int status = 200;
        bool describe = !strncmp(request, "DESCRIBE ", 9);
        if (describe) {
            ++peer->describes;
            status = peer->describes <= peer->busyResponses ? 503 : peer->finalStatus;
            CHECK((strstr(request, "X-ML-Mat-Live: 1") != NULL) ==
                  (NegotiatedAudioCodec == AUDIO_CODEC_MAT_LIVE));
        }
        else {
            CHECK(!strncmp(request, "OPTIONS ", 8));
        }
        if (describe && status == 0) {
            // A partial reply following a 503 must share the original deadline.
            CHECK(send(client, "RTSP/", 5, 0) == 5);
            if (peer->cancel) {
                PltSleepMs(50);
                ConnectionInterrupted = true;
            }
            while (!peer->stop) PltSleepMs(20);
            closeSocket(client);
            return;
        }
        // A successful response with invalid SDP must still fail profile validation.
        const char* body = describe && status == 200 ? "v=0\r\n" : "";
        char response[512];
        int length = snprintf(response, sizeof(response),
            "RTSP/1.0 %d Test\r\nCSeq: %d\r\nContent-Length: %u\r\n\r\n%s",
            status, current, (unsigned)strlen(body), body);
        CHECK(send(client, response, length, 0) == length);
        closeSocket(client);
        if (describe && peer->cancel) {
            PltSleepMs(50);
            ConnectionInterrupted = true;
            return;
        }
    }
}

static void run(const char* name, int codec, int busyResponses, int finalStatus,
                bool cancel, int expectedResult, int minRequests, int maxRequests) {
    PEER peer = {0};
    peer.busyResponses = busyResponses;
    peer.finalStatus = finalStatus;
    peer.cancel = cancel;
    peer.listener = createSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, false);
    CHECK(peer.listener != INVALID_SOCKET);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(peer.listener, (struct sockaddr*)&address, sizeof(address)) == 0);
    CHECK(listen(peer.listener, 4) == 0);
    SOCKADDR_LEN size = sizeof(address);
    CHECK(getsockname(peer.listener, (struct sockaddr*)&address, &size) == 0);
    RtspPortNumber = ntohs(address.sin_port);
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    memcpy(&RemoteAddr, &address, sizeof(address));
    AddrLen = sizeof(address);
    NegotiatedAudioCodec = codec;
    ConnectionInterrupted = false;
    PLT_THREAD thread;
    CHECK(PltCreateThread("RTSP retry test", serve, &peer, &thread) == 0);
    SERVER_INFORMATION server = {0};
    uint64_t started = PltGetMillis();
    int result = performRtspHandshake(&server);
    uint64_t elapsed = PltGetMillis() - started;
    peer.stop = true;
    PltJoinThread(&thread);
    closeSocket(peer.listener);
    CHECK(result == expectedResult);
    CHECK(peer.describes >= minRequests && peer.describes <= maxRequests);
    if (busyResponses == 100 || (finalStatus == 0 && !cancel)) CHECK(elapsed >= 14950 && elapsed < 15400);
    else CHECK(elapsed < 2000);
    printf("PASS %s: result=%d describes=%d elapsed=%llu ms\n", name, result,
           peer.describes, (unsigned long long)elapsed);
}

int main(void) {
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.audioCodec = AUDIO_CODEC_MAT_LIVE;
    run("busy then different error", AUDIO_CODEC_MAT_LIVE, 2, 409, false, 409, 3, 3);
    run("busy then malformed SDP", AUDIO_CODEC_MAT_LIVE, 1, 200, false, -1, 2, 2);
    run("permanent busy is bounded", AUDIO_CODEC_MAT_LIVE, 100, 503, false, 503, 2, 31);
    run("late partial response shares deadline", AUDIO_CODEC_MAT_LIVE, 4, 0, false, ETIMEDOUT, 5, 5);
    run("cancel during partial response", AUDIO_CODEC_MAT_LIVE, 0, 0, true, -1, 1, 1);
    run("Opus does not retry", AUDIO_CODEC_OPUS, 1, 409, false, 503, 1, 1);
    run("cancel while waiting", AUDIO_CODEC_MAT_LIVE, 1, 409, true, -1, 1, 1);
    cleanupPlatform();
    return 0;
}
