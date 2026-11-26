/* Exercise actual DESCRIBE -> SDP ANNOUNCE -> confirmation using localhost only. */
#include "Limelight-internal.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    SOCKET listener;
    atomic_bool stop;
    const char *offer, *confirmation;
    bool encryptedControl, pingIdentity, mat;
    unsigned expectedEcho;
    unsigned announces;
} PEER;

static void serve(void* context) {
    PEER* peer = context;
    while (!peer->stop) {
        struct pollfd pollFd = {peer->listener, POLLIN, 0};
        if (pollSockets(&pollFd, 1, 50) <= 0) continue;
        SOCKET client = accept(peer->listener, NULL, NULL);
        CHECK(client != INVALID_SOCKET);
        char request[16384] = {0};
        int used = 0, needed = 0;
        do {
            int n = (int)recv(client, request + used, (int)sizeof(request) - 1 - used, 0);
            CHECK(n > 0);
            used += n;
            CHECK(used < (int)sizeof(request) - 1);
            char* body = strstr(request, "\r\n\r\n");
            if (body) {
                const char* length = strstr(request, "Content-length: ");
                needed = (int)(body + 4 - request) + (length ? atoi(length + 16) : 0);
                CHECK(needed < (int)sizeof(request));
            }
        } while (!needed || used < needed);

        const char* sequence = strstr(request, "CSeq: ");
        CHECK(sequence != NULL);
        int current = atoi(sequence + 6);
        char body[2048] = "", headers[2048] = "";
        if (!strncmp(request, "DESCRIBE ", 9)) {
            if (strstr(peer->offer, "a=x-ss-audio.mat-unavailable:")) {
                snprintf(body, sizeof(body), "v=0\r\na=x-ss-general.encryptionSupported:%u\r\n%s",
                         peer->encryptedControl ? SS_ENC_CONTROL_V2 : 0, peer->offer);
            } else {
                snprintf(body, sizeof(body),
                    "v=0\r\na=x-ss-audio.mat-live:1\r\n"
                    "a=x-ss-audio.mat-live-carrier:" MAT_CARRIER "\r\n"
                    "a=x-ss-audio.mat-live-clock:" MAT_CLOCK "\r\n"
                    "a=x-ss-audio.mat-live-crypto:" MAT_LIVE_CRYPTO "\r\n"
                    "a=x-ss-audio.mat-live-epoch:7\r\na=x-ss-audio.mat-live-domain:9\r\n"
                    "a=x-ss-general.encryptionSupported:%u\r\n%s",
                    peer->encryptedControl ? SS_ENC_CONTROL_V2 : 0, peer->offer);
            }
        }
        else if (!strncmp(request, "SETUP ", 6)) {
            unsigned port = strstr(request, "streamid=audio") ? 53000 :
                            strstr(request, "streamid=video") ? 52998 : 52999;
            char transport[128];
            if (port == 53000) snprintf(transport, sizeof(transport), "unicast;server_port=%u-%u;source=127.0.0.1", port, port + 1);
            else if (port == 52998) snprintf(transport, sizeof(transport), "x-server_port=2;server_port=%u", port);
            else snprintf(transport, sizeof(transport), "server_port=%u-invalid", port);
            snprintf(headers, sizeof(headers),
                "Session: recovery-test\r\nTransport: %s\r\n"
                "X-SS-Connect-Data: 123\r\n%s", transport,
                peer->pingIdentity ? "X-SS-Ping-Payload: 0123456789abcdef\r\n" : "");
        }
        else if (!strncmp(request, "ANNOUNCE ", 9)) {
            ++peer->announces;
            bool interleave = (StreamConfig.videoReceiveOptions & VIDEO_RECEIVE_INTERLEAVED_FEC) &&
                strstr(peer->offer, "a=x-ss-video[0].fecInterleaveVersion:1\r\n") != NULL;
            CHECK((strstr(request, "a=x-ml-video.fecInterleaveVersion:1") != NULL) == interleave);
            CHECK(!NegotiatedMatAudioRecovery);
            char selection[64];snprintf(selection,sizeof(selection),"a=x-ml-audio.mat-live-recovery:%u",peer->expectedEcho);
            CHECK((strstr(request, "a=x-ml-audio.mat-live-recovery:") != NULL) == (peer->expectedEcho != 0));
            if(peer->expectedEcho)CHECK(strstr(request,selection)!=NULL);
            const char* feature = strstr(request, "a=x-ml-general.featureFlags:");
            CHECK(feature != NULL);
            CHECK(strtoul(feature + strlen("a=x-ml-general.featureFlags:"), NULL, 10) & ML_FF_SESSION_ID_V1);
            snprintf(headers, sizeof(headers), "%s%s", peer->mat ?
                "X-SS-Audio-Codec: " MAT_LIVE_NAME "\r\n"
                "X-SS-Mat-Carrier: " MAT_CARRIER "\r\nX-SS-Mat-Clock: " MAT_CLOCK "\r\n"
                "X-SS-Mat-Live-Crypto: " MAT_LIVE_CRYPTO "\r\n"
                "X-SS-Mat-Live-Epoch: 7\r\nX-SS-Mat-Live-Domain: 9\r\n" :
                "X-SS-Audio-Codec: opus\r\n", peer->confirmation);
        }
        else CHECK(!strncmp(request, "OPTIONS ", 8) || !strncmp(request, "PLAY ", 5));

        char response[4096];
        int length = snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%sContent-Length: %u\r\n\r\n%s",
            current, headers, (unsigned)strlen(body), body);
        CHECK(length > 0 && length < (int)sizeof(response));
        CHECK(send(client, response, length, 0) == length);
        closeSocket(client);
    }
}

static void run(const char* name, const char* offer, const char* confirmation,
                bool encryptedControl, bool pingIdentity, bool mat,
                unsigned expectedEcho, int expectedResult, unsigned expectedRecovery) {
    PEER peer = {0};
    peer.offer = offer;
    peer.confirmation = confirmation;
    peer.encryptedControl = encryptedControl;
    peer.pingIdentity = pingIdentity;
    bool unavailable = strstr(offer, "a=x-ss-audio.mat-unavailable:") != NULL;
    peer.mat = mat && !unavailable;
    peer.expectedEcho = expectedEcho;
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
    AudioPortNumber = VideoPortNumber = ControlPortNumber = 0;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    memcpy(&RemoteAddr, &address, sizeof(address));
    memset(&LocalAddr, 0, sizeof(LocalAddr));
    ((struct sockaddr_in*)&LocalAddr)->sin_family = AF_INET;
    AddrLen = sizeof(address);
    NegotiatedAudioCodec = mat || unavailable ? AUDIO_CODEC_MAT_LIVE : AUDIO_CODEC_OPUS;
    StreamConfig.audioCodec = !mat && unavailable ? AUDIO_CODEC_MAT_AUTO : NegotiatedAudioCodec;
    ConnectionInterrupted = false;
    CHECK(initializeAudioStream() == 0);
    PLT_THREAD thread;
    CHECK(PltCreateThread("RTSP recovery test", serve, &peer, &thread) == 0);
    SERVER_INFORMATION server = {0};
    int result = performRtspHandshake(&server);
    peer.stop = true;
    PltJoinThread(&thread);
    closeSocket(peer.listener);
    destroyAudioStream();
    CHECK(result == expectedResult);
    if (result == 0) CHECK(AudioPortNumber == 53000 && VideoPortNumber == 52998 && ControlPortNumber == 47999);
    CHECK(NegotiatedMatAudioRecovery == expectedRecovery);
    CHECK(peer.announces == (unavailable && mat ? 0 : 1));
    if (unavailable && !mat) CHECK(NegotiatedAudioCodec == AUDIO_CODEC_OPUS);
    if(expectedRecovery==2)CHECK(NegotiatedMatAudioSsrc==123456);
    printf("PASS %s: result=%d recovery=%d\n", name, result, NegotiatedMatAudioRecovery);
}

int main(void) {
    const char* offer = "a=x-ss-audio.mat-live-recovery:1\r\n";
    const char* confirmation = "X-SS-Mat-Live-Recovery: 1\r\n";
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431; AppVersionQuad[3] = -1;
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.width = 1280; StreamConfig.height = 720; StreamConfig.fps = 30;
    StreamConfig.bitrate = 10000; StreamConfig.packetSize = 1392;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    run("three-phase agreement", offer, confirmation, true, true, true, true, 0, true);
    run("missing encoder, strict", "a=x-ss-audio.mat-unavailable:1\r\n", "", true, true, true, 0, ML_ERROR_MAT_ENCODER_MISSING, 0);
    run("invalid decimal suffix", "a=x-ss-audio.mat-unavailable:1x\r\n", "", true, true, true, 0, -1, 0);
    run("overflowing reason", "a=x-ss-audio.mat-unavailable:4294967297\r\n", "", true, true, true, 0, -1, 0);
    run("conflicting reasons", "a=x-ss-audio.mat-unavailable:1\r\na=x-ss-audio.mat-unavailable:2\r\n",
        "", true, true, true, 0, -1, 0);
    run("incompatible encoder, strict", "a=x-ss-audio.mat-unavailable:2\r\n", "", true, true, true, 0, ML_ERROR_MAT_ENCODER_INCOMPATIBLE, 0);
    run("missing encoder, auto", "a=x-ss-audio.mat-unavailable:1\r\n", "", true, true, false, 0, 0, 0);
    run("incompatible encoder, auto", "a=x-ss-audio.mat-unavailable:2\r\n", "", true, true, false, 0, 0, 0);
    run("old host resets capability", "", "", true, true, true, false, 0, false);
    run("offer without confirmation", offer, "", true, true, true, true, 0, false);
    run("unsolicited confirmation", "", confirmation, true, true, true, false, -1, false);
    run("invalid confirmation version", offer, "X-SS-Mat-Live-Recovery: 2\r\n", true, true, true, true, -1, false);
    run("duplicate confirmation", offer, "X-SS-Mat-Live-Recovery: 1\r\nx-ss-mat-live-recovery: 1\r\n", true, true, true, true, -1, false);
    run("unsupported offer", "a=x-ss-audio.mat-live-recovery:2\r\n", "", true, true, true, false, 0, false);
    run("duplicate offer", "a=x-ss-audio.mat-live-recovery:1\r\na=x-ss-audio.mat-live-recovery:1\r\n", "", true, true, true, false, 0, false);
    run("no encrypted control v2", offer, "", false, true, true, false, 0, false);
    run("no session ping identity", offer, "", true, false, true, false, 0, false);
    run("Opus stays on legacy path", offer, "", true, true, false, false, 0, false);
    const char* videoOffer = "a=x-ss-video[0].fecInterleaveVersion:1\r\n";
    run("interleave off by default", videoOffer, "", true, true, false, 0, 0, 0);
    StreamConfig.videoReceiveOptions |= VIDEO_RECEIVE_INTERLEAVED_FEC;
    run("interleave negotiated", videoOffer, "", true, true, false, 0, 0, 0);
    run("old host with interleave enabled", "", "", true, true, false, 0, 0, 0);
    StreamConfig.videoReceiveOptions &= ~VIDEO_RECEIVE_INTERLEAVED_FEC;
    const char* both="a=x-ss-audio.mat-live-recovery:1\r\na=x-ss-audio.mat-live-recovery-v2:1\r\n";
#ifdef MAT_RECOVERY_LEGACY_CLIENT
    run("legacy client with new host",both,confirmation,true,true,true,1,0,1);
#else
    run("v2 agreement",both,"X-SS-Mat-Live-Recovery: 2\r\nX-SS-Mat-Live-Ssrc: 123456\r\n",true,true,true,2,0,2);
    run("v2 missing confirmation",both,"",true,true,true,2,-1,0);
    run("v2 missing identity",both,"X-SS-Mat-Live-Recovery: 2\r\n",true,true,true,2,-1,0);
    run("v2 mismatched confirmation",both,confirmation,true,true,true,2,-1,0);
    run("v2 duplicate identity",both,"X-SS-Mat-Live-Recovery: 2\r\nX-SS-Mat-Live-Ssrc: 123456\r\nX-SS-Mat-Live-Ssrc: 999\r\n",true,true,true,2,-1,0);
    run("v2 invalid identity",both,"X-SS-Mat-Live-Recovery: 2\r\nX-SS-Mat-Live-Ssrc: 4294967296\r\n",true,true,true,2,-1,0);
#endif
    cleanupPlatform();
    return 0;
}
