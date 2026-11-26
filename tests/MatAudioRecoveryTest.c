/* Real AudioStream workers, sockets, RTP, authentication and generation cleanup.
 * The host control transport and Android renderer are deterministic local fakes. */
#include "Limelight-internal.h"
#include "PlatformCrypto.h"
static atomic_bool failNextContext;
static PPLT_CRYPTO_CONTEXT testCreateCryptoContext(void) {
    return atomic_exchange(&failNextContext, false) ? NULL : PltCreateCryptoContext();
}
#define PltCreateCryptoContext testCreateCryptoContext
int testSendRecoveryControl(const MAT_AUDIO_RECOVERY*);
static SOCKET testBindUdpSocket(int, struct sockaddr_storage*, SOCKADDR_LEN, int, int);
int testSendBackgroundControl(const MAT_BACKGROUND*);
#define sendMatBackgroundControl testSendBackgroundControl
#define bindUdpSocket testBindUdpSocket
#define sendMatAudioRecoveryControl testSendRecoveryControl
#include "../src/AudioStream.c"
#undef bindUdpSocket
#include "MatLiveCrypto.h"
#include <stdio.h>
#include <stdarg.h>

static atomic_int failures, initialized, cleaned, terminated, delivered, readyCount, requestCount;
static atomic_bool rejectNextWrite;
static atomic_uint lastRequestReason;
static SOCKET serverSocket;
static uint16_t oldPort;
static MAT_AUDIO_RECOVERY prepared;
static PLT_MUTEX testMutex;
static bool failNewRenderer, blockDuringCleanup;
static atomic_int bgSentCount;
static MAT_BACKGROUND bgSent;
int testSendBackgroundControl(const MAT_BACKGROUND* message) {
    PltLockMutex(&testMutex);
    bgSent = *message;
    PltUnlockMutex(&testMutex);
    atomic_fetch_add(&bgSentCount, 1);
    return 0;
}
static MAT_BACKGROUND latestBg(void) {
    PltLockMutex(&testMutex);
    MAT_BACKGROUND m = bgSent;
    PltUnlockMutex(&testMutex);
    return m;
}
static unsigned testVersion = 1;
static atomic_int playingCount, stableCount, failCount, progressValue;
static atomic_uint forcedPort, progressCalls;
static atomic_ullong failureReportedAt, rendererStoppedAt;
static bool eventRenderer;
static atomic_ullong progressEpoch;
static MAT_AUDIO_RECOVERY lastFailure;
static SOCKET testBindUdpSocket(int family, struct sockaddr_storage* addr, SOCKADDR_LEN length, int size, int qos) {
    unsigned port = atomic_exchange(&forcedPort, 0);
    if (!port) return bindUdpSocket(family, addr, length, size, qos);
    SOCKET fd = createSocket(family, SOCK_DGRAM, IPPROTO_UDP, false);
    if (fd == INVALID_SOCKET) return fd;
    struct sockaddr_storage target = *addr;
    ((struct sockaddr_in*)&target)->sin_port = htons((uint16_t)port);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(size));
    if (bind(fd, (struct sockaddr*)&target, length) != 0) { closeSocket(fd); return INVALID_SOCKET; }
    return fd;
}
static int rendererProgress(void* context, uint64_t epoch, uint64_t domain) {
    (void)context;
    atomic_fetch_add(&progressCalls, 1);
    return epoch == atomic_load(&progressEpoch) && domain == 7 ? atomic_load(&progressValue) : 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); atomic_fetch_add(&failures,1); } } while(0)

static void carrier(uint8_t* bytes) {
    memcpy(bytes,"\x72\xf8\x1f\x4e\x16\0\x02\0",8);
    for(unsigned i=8;i<MAT_WIRE_CYCLE;i++)bytes[i]=(uint8_t)i;
}
static int cycle(void* context,const uint8_t* bytes,int length,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts) {
    uint8_t expected[MAT_WIRE_CYCLE];carrier(expected);
    CHECK(context == (void*)(uintptr_t)atomic_load(&initialized));
    CHECK(epoch == MatLiveProfile.epoch && domain == 7 && index == 0 && pts == 0);
    CHECK(length == MAT_WIRE_CYCLE && !memcmp(bytes,expected,sizeof(expected)));
    if (atomic_exchange(&rejectNextWrite, false)) return 0;
    atomic_fetch_add(&delivered,1);return 1;
}
static int end(void* context,uint64_t count) {(void)context;(void)count;return 1;}
static int rendererInit(int configuration,POPUS_MULTISTREAM_CONFIGURATION opus,void* context,int flags) {
    (void)configuration;(void)opus;CHECK(context==(void*)0x1234&&flags==17);
    int count=atomic_fetch_add(&initialized,1)+1;
    if(failNewRenderer&&count==2)return -1;
    LI_MAT_PLAYBACK_CALLBACKS cb={cycle,end,(void*)(uintptr_t)count,NULL,rendererProgress,eventRenderer};
    LiSetMatPlaybackCallbacksEx(&cb, eventRenderer ? sizeof(cb) : offsetof(LI_MAT_PLAYBACK_CALLBACKS, reportsFailures));return 0;
}
static void rendererCleanup(void){atomic_fetch_add(&cleaned,1);LiSetMatPlaybackCallbacksEx(NULL, 0);if(blockDuringCleanup)LiBlockMatAudioRecovery();}
static void noOp(void){}
static void rendererStop(void){atomic_store(&rendererStoppedAt, PltGetMillis());}
static void terminate(int error){(void)error;atomic_fetch_add(&terminated,1);}
static void logger(const char* format,...) {va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);}

int testSendRecoveryControl(const MAT_AUDIO_RECOVERY* message) {
    if(message->verb==MAT_RECOVERY_REQUEST){
        atomic_store(&lastRequestReason,message->reason);
        atomic_fetch_add(&requestCount,1);
        if (testVersion == 2) return 0;
        MAT_AUDIO_RECOVERY next={0};next.verb=MAT_RECOVERY_PREPARE;next.generation=1;
        next.previousEpoch=123;next.newEpoch=456;next.domain=7;
        next.serverPort=audioSocketPort(serverSocket);next.token[15]=9;
        queueMatAudioRecoveryControl(&next);
    } else if(message->verb==MAT_RECOVERY_READY){
        CHECK(message->clientPort && (testVersion == 2 || message->clientPort!=oldPort));
        PltLockMutex(&testMutex);prepared=*message;PltUnlockMutex(&testMutex);
        atomic_fetch_add(&readyCount,1);
    } else if(message->verb==MAT_RECOVERY_PLAYING) atomic_fetch_add(&playingCount,1);
    else if(message->verb==MAT_RECOVERY_STABLE) atomic_fetch_add(&stableCount,1);
    else if(message->verb==MAT_RECOVERY_FAIL) {
        PltLockMutex(&testMutex);lastFailure=*message;PltUnlockMutex(&testMutex);
        atomic_store(&failureReportedAt, PltGetMillis());
        atomic_fetch_add(&failCount,1);
    }
    return 0;
}
static bool awaitCount(atomic_int* value,int count) {
    uint64_t deadline=PltGetMillis()+3000;
    while(atomic_load(value)<count&&PltGetMillis()<deadline)PltSleepMs(2);
    return atomic_load(value)>=count;
}

// Sunshine learns the media destination from a ping for this generation.
static bool awaitAudioPing(uint16_t port, const void* token) {
    uint64_t deadline = PltGetMillis() + 2000;
    struct pollfd socket = {0};
    socket.fd = serverSocket;
    socket.events = POLLIN;
    for (;;) {
        uint64_t now = PltGetMillis();
        if (now >= deadline || pollSockets(&socket, 1, (int)(deadline - now)) <= 0) return false;
        SS_PING ping;
        struct sockaddr_in from;
        SOCKADDR_LEN length = sizeof(from);
        int received = recvfrom(serverSocket, (char*)&ping, sizeof(ping), 0, (struct sockaddr*)&from, &length);
        if (received < 0) return false;
        if (received == (int)sizeof(ping) && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            ntohs(from.sin_port) == port && !memcmp(ping.payload, token, sizeof(ping.payload))) return true;
    }
}

static void sendPacketWithSsrc(uint16_t port,uint16_t rtpSequence,const uint8_t* bytes,uint32_t ssrc) {
    uint8_t packet[sizeof(RTP_PACKET)+MAT_LIVE_PACKET];
    PRTP_PACKET rtp=(PRTP_PACKET)packet;memset(rtp,0,sizeof(*rtp));
    rtp->header=0x80;rtp->packetType=97;rtp->sequenceNumber=BE16(rtpSequence);rtp->timestamp=BE32(rtpSequence);rtp->ssrc=BE32(ssrc);
    memcpy(rtp+1,bytes,MAT_LIVE_PACKET);
    struct sockaddr_in peer={0};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);peer.sin_port=htons(port);
    int sent=sendto(serverSocket,(char*)packet,sizeof(packet),0,(struct sockaddr*)&peer,sizeof(peer));
    if (sent != (int)sizeof(packet))
        fprintf(stderr,"Test UDP write: port=%u sequence=%u ssrc=%u sent=%d error=%d\n",
                port,rtpSequence,ssrc,sent,LastSocketError());
    CHECK(sent==(int)sizeof(packet));
}
static void sendPacket(uint16_t port,uint16_t sequence,const uint8_t* bytes) {
    sendPacketWithSsrc(port,sequence,bytes,NegotiatedMatAudioSsrc);
}
static void sendCycle(uint16_t port,uint64_t epoch) {
    uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET],bytes[MAT_WIRE_CYCLE];
    MAT_LIVE_CIPHER cipher;carrier(bytes);
    CHECK(MatLiveCipherInit(&cipher,(uint8_t*)StreamConfig.remoteInputAesKey,16,epoch,7));
    mat_wire_make(plain,MAT_WIRE_SYNC,epoch,7,0,0,0,NULL);
    for(unsigned i=0;i<4;i++){CHECK(MatLiveSeal(&cipher,i,plain,wire));sendPacket(port,i,wire);}
    for(unsigned i=0;i<MAT_WIRE_FRAGMENTS;i++){
        mat_wire_make(plain,MAT_WIRE_DATA,epoch,7,0,i,mat_hash(bytes,sizeof(bytes)),bytes);
        CHECK(MatLiveSeal(&cipher,4+i,plain,wire));sendPacket(port,4+i,wire);
    }
}
static void setup(bool failRenderer) {
    AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=431;AppVersionQuad[3]=-1;
    failNewRenderer=failRenderer;blockDuringCleanup=false;
    atomic_store(&playingCount,0);atomic_store(&stableCount,0);atomic_store(&failCount,0);
    atomic_store(&progressValue,0);atomic_store(&progressEpoch,0);atomic_store(&forcedPort,0);
    atomic_store(&initialized,0);atomic_store(&cleaned,0);atomic_store(&terminated,0);
    atomic_store(&delivered,0);atomic_store(&readyCount,0);atomic_store(&requestCount,0);
    atomic_store(&rejectNextWrite,false);atomic_store(&lastRequestReason,0);
    memset(&prepared,0,sizeof(prepared));
    ConnectionInterrupted=false;LiClearStreamSockets();
    LiInitializeStreamConfiguration(&StreamConfig);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);ListenerCallbacks.connectionTerminated=terminate;
    ListenerCallbacks.logMessage=logger;
    LiInitializeAudioCallbacks(&AudioCallbacks);
    AudioCallbacks.init=rendererInit;AudioCallbacks.start=noOp;AudioCallbacks.stop=rendererStop;AudioCallbacks.cleanup=rendererCleanup;
    memset(&RemoteAddr,0,sizeof(RemoteAddr));memset(&LocalAddr,0,sizeof(LocalAddr));
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    memcpy(&RemoteAddr,&address,sizeof(address));memcpy(&LocalAddr,&address,sizeof(address));AddrLen=sizeof(address);
    serverSocket=bindUdpSocket(AF_INET,&LocalAddr,AddrLen,1024*1024,SOCK_QOS_TYPE_AUDIO);
    CHECK(serverSocket!=INVALID_SOCKET);AudioPortNumber=audioSocketPort(serverSocket);
    NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;NegotiatedMatAudioRecovery=testVersion;NegotiatedMatAudioSsrc=testVersion==2?99:0;
    MatLiveProfile.epoch=123;MatLiveProfile.domain=7;AudioPacketDuration=1;
    memset(&NormalQualityOpusConfig,0,sizeof(NormalQualityOpusConfig));
    NormalQualityOpusConfig.sampleRate=48000;NormalQualityOpusConfig.channelCount=2;NormalQualityOpusConfig.streams=1;
    HighQualitySurroundEnabled=false;memset(&AudioPingPayload,0,sizeof(AudioPingPayload));AudioPingPayload.payload[15]=1;
    CHECK(initializeAudioStream()==0);
    CHECK(!matReceiveDiagnostics.gapSamples && !matDecodeDiagnostics.gapSamples);
    CHECK(notifyAudioPortNegotiationComplete()==0);
    oldPort=audioSocketPort(rtpSocket);CHECK(startAudioStream((void*)0x1234,17)==0);
    CHECK(awaitAudioPing(oldPort, AudioPingPayload.payload));
}
static void teardown(void) {
    stopAudioStream();destroyAudioStream();closeSocket(serverSocket);
    LI_STREAM_SOCKETS sockets;LiGetStreamSockets(&sockets);CHECK(sockets.audioRtp.fd==-1);
}

static void generationAllocationFailure(void) {
    atomic_store(&failNextContext, true);
    CHECK(initializeAudioStream() != 0);
    CHECK(audioDecryptionCtx == NULL && rtpAudioQueue.rs == NULL);
    setup(false);
    atomic_store(&failNextContext, true);
    CHECK(requestMatAudioRecovery(1));
    CHECK(awaitCount(&terminated, 1));
    teardown();
    CHECK(audioDecryptionCtx == NULL && rtpAudioQueue.rs == NULL);
}
static void restartAndRejectOldEpoch(void) {
    setup(false);sendCycle(oldPort,123);CHECK(awaitCount(&delivered,1));
    CHECK(requestMatAudioRecovery(1));CHECK(awaitCount(&readyCount,1));
    CHECK(atomic_load(&initialized)==2&&atomic_load(&cleaned)==1&&!atomic_load(&terminated));
    PltLockMutex(&testMutex);MAT_AUDIO_RECOVERY start=prepared;PltUnlockMutex(&testMutex);
    // Duplicate PREPARE is idempotent while waiting for START.
    MAT_AUDIO_RECOVERY duplicate=start;duplicate.verb=MAT_RECOVERY_PREPARE;duplicate.clientPort=0;
    queueMatAudioRecoveryControl(&duplicate);CHECK(awaitCount(&readyCount,2));
    start.verb=MAT_RECOVERY_START;queueMatAudioRecoveryControl(&start);queueMatAudioRecoveryControl(&start);
    uint64_t deadline=PltGetMillis()+2000;
    while(PltGetMillis()<deadline){PltLockMutex(&matRecoveryMutex);bool busy=matRecoveryBusy;PltUnlockMutex(&matRecoveryMutex);if(!busy)break;PltSleepMs(1);}
    CHECK(awaitAudioPing(start.clientPort, start.token));
    sendCycle(start.clientPort,456);CHECK(awaitCount(&delivered,2));
    CHECK(atomic_load(&initialized)==2&&atomic_load(&requestCount)==1);
    // A packet authenticated for the old epoch cannot enter new playback.
    MAT_LIVE_CIPHER old;uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET];
    CHECK(MatLiveCipherInit(&old,(uint8_t*)StreamConfig.remoteInputAesKey,16,123,7));
    mat_wire_make(plain,MAT_WIRE_SYNC,123,7,0,0,0,NULL);
    CHECK(MatLiveSeal(&old,64,plain,wire));sendPacket(start.clientPort,64,wire);
    CHECK(awaitCount(&terminated,1));CHECK(atomic_load(&requestCount)==1&&atomic_load(&delivered)==2);
    teardown();CHECK(atomic_load(&cleaned)==2);
}
static MAT_AUDIO_RECOVERY v2Prepare(unsigned generation, uint64_t epoch) {
    MAT_AUDIO_RECOVERY m={0};m.version=2;m.verb=MAT_RECOVERY_PREPARE;m.generation=generation;
    m.previousEpoch=generation==1?123:epoch-100;m.newEpoch=epoch;m.domain=7;
    m.serverPort=AudioPortNumber;m.ssrc=99+generation;m.token[15]=(uint8_t)generation;
    m.reason=MAT_RECOVERY_RETRYABLE;
    uint8_t wire[MAT_RECOVERY_V2_BYTES];MAT_AUDIO_RECOVERY decoded;
    MatRecoveryEncode(wire,&m);CHECK(MatRecoveryDecode(wire,sizeof(wire),&decoded));
    queueMatAudioRecoveryControl(&decoded);return m;
}
static MAT_AUDIO_RECOVERY v2Start(int ready) {
    CHECK(awaitCount(&readyCount,ready));
    PltLockMutex(&testMutex);MAT_AUDIO_RECOVERY start=prepared;PltUnlockMutex(&testMutex);
    start.verb=MAT_RECOVERY_START;queueMatAudioRecoveryControl(&start);
    uint64_t deadline=PltGetMillis()+2000;
    for(;;) {
        PltLockMutex(&matRecoveryMutex);bool running=matV2Phase==MAT_V2_CONFIRMING;PltUnlockMutex(&matRecoveryMutex);
        if(running||PltGetMillis()>=deadline)break;
        PltSleepMs(1);
    }
    CHECK(awaitAudioPing(start.clientPort, start.token));
    return start;
}
static void oldPackets(uint16_t port,uint64_t epoch,uint32_t ssrc,uint32_t outer) {
    MAT_LIVE_CIPHER cipher;uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET];
    CHECK(MatLiveCipherInit(&cipher,(uint8_t*)StreamConfig.remoteInputAesKey,16,epoch,7));
    mat_wire_make(plain,MAT_WIRE_SYNC,epoch,7,0,0,0,NULL);
    CHECK(MatLiveSeal(&cipher,64,plain,wire));
    sendPacketWithSsrc(port,64,wire,ssrc);
    uint8_t fecBytes[sizeof(RTP_PACKET)+sizeof(AUDIO_FEC_HEADER)+MAT_LIVE_PACKET]={0};
    RTP_PACKET* rtp=(RTP_PACKET*)fecBytes;rtp->header=0x80;rtp->packetType=127;rtp->ssrc=BE32(outer);
    AUDIO_FEC_HEADER* fec=(AUDIO_FEC_HEADER*)(rtp+1);fec->payloadType=97;fec->ssrc=BE32(ssrc);
    struct sockaddr_in peer={0};peer.sin_family=AF_INET;peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);peer.sin_port=htons(port);
    CHECK(sendto(serverSocket,(char*)fecBytes,sizeof(fecBytes),0,(struct sockaddr*)&peer,sizeof(peer))==sizeof(fecBytes));
}
static void repeatedV2WithOldPort(void) {
    testVersion=2;setup(false);sendCycle(oldPort,123);CHECK(awaitCount(&delivered,1));
    uint16_t initialPort=oldPort;
    v2Prepare(1,456);MAT_AUDIO_RECOVERY first=v2Start(1);
    // Old playback progress cannot acknowledge the new attempt.
    atomic_store(&progressEpoch,123);atomic_store(&progressValue,3);
    oldPackets(first.clientPort,123,99,99);
    oldPackets(first.clientPort,123,99,first.ssrc); // Correct outer, stale inner FEC identity.
    PltSleepMs(60);CHECK(!atomic_load(&playingCount)&&!receivedDataFromPeer);
    CHECK(atomic_load(&matRejectedRtp)==4);
    sendCycle(first.clientPort,456);CHECK(awaitCount(&delivered,2));
    atomic_store(&progressEpoch,456);atomic_store(&progressValue,1);CHECK(awaitCount(&playingCount,1));
    CHECK(!atomic_load(&stableCount));atomic_store(&progressValue,3);CHECK(awaitCount(&stableCount,1));
    // The initial port is now free. Force its reuse by the actual audio worker.
    atomic_store(&forcedPort,initialPort);atomic_store(&progressValue,0);
    v2Prepare(2,556);MAT_AUDIO_RECOVERY second=v2Start(2);CHECK(second.clientPort==initialPort);
    oldPackets(second.clientPort,123,99,99);oldPackets(second.clientPort,456,first.ssrc,first.ssrc);
    PltSleepMs(60);CHECK(!receivedDataFromPeer&&atomic_load(&matRejectedRtp)==4);
    sendCycle(second.clientPort,556);CHECK(awaitCount(&delivered,3));
    atomic_store(&progressEpoch,556);atomic_store(&progressValue,1);CHECK(awaitCount(&playingCount,2));
    // At least three recoveries in the same connection, with old control traffic.
    atomic_store(&progressValue,0);v2Prepare(3,656);MAT_AUDIO_RECOVERY third=v2Start(3);
    first.verb=MAT_RECOVERY_FAIL;queueMatAudioRecoveryControl(&first);
    second.verb=MAT_RECOVERY_START;queueMatAudioRecoveryControl(&second);
    sendCycle(third.clientPort,656);CHECK(awaitCount(&delivered,4));
    atomic_store(&progressEpoch,656);atomic_store(&progressValue,1);CHECK(awaitCount(&playingCount,3));
    CHECK(atomic_load(&initialized)==4&&!atomic_load(&terminated)&&!atomic_load(&failCount));
    teardown();
    // Each of the four sockets received four sync packets and one full cycle.
    CHECK(matReceiveDiagnostics.gapSamples == 4 * (4 + MAT_WIRE_FRAGMENTS - 1));
    CHECK(matDecodeDiagnostics.gapSamples == 0);CHECK(atomic_load(&cleaned)==4);
}
static void failedV2CannotRevive(void) {
    setup(true);MAT_AUDIO_RECOVERY failed=v2Prepare(1,456);CHECK(awaitCount(&failCount,1));
    CHECK(atomic_load(&initialized)==2&&!atomic_load(&terminated));failNewRenderer=false;
    failed.verb=MAT_RECOVERY_START;failed.clientPort=1;queueMatAudioRecoveryControl(&failed);
    PltSleepMs(35);CHECK(atomic_load(&initialized)==2);
    v2Prepare(2,556);CHECK(awaitCount(&readyCount,1));
    PltLockMutex(&testMutex);failed=prepared;PltUnlockMutex(&testMutex);
    // Expire the real worker deadline; a late START must not create workers.
    PltLockMutex(&matRecoveryMutex);matV2Deadline=PltGetMillis();PltUnlockMutex(&matRecoveryMutex);
    CHECK(awaitCount(&failCount,2));failed.verb=MAT_RECOVERY_START;queueMatAudioRecoveryControl(&failed);
    PltSleepMs(35);CHECK(!receiveStarted&&!decoderStarted);
    v2Prepare(3,656);MAT_AUDIO_RECOVERY noProgress=v2Start(2);
    sendCycle(noProgress.clientPort,656);CHECK(awaitCount(&delivered,1));
    // Media received/authenticated is not proof of playback progress.
    CHECK(!atomic_load(&playingCount));
    PltLockMutex(&matRecoveryMutex);matV2Deadline=PltGetMillis();PltUnlockMutex(&matRecoveryMutex);
    atomic_store(&progressEpoch,656);atomic_store(&progressValue,3);
    CHECK(awaitCount(&failCount,3));queueMatAudioRecoveryControl(&noProgress);
    atomic_store(&progressEpoch,656);atomic_store(&progressValue,3);PltSleepMs(35);
    CHECK(!atomic_load(&playingCount)&&!atomic_load(&stableCount));
    v2Prepare(4,756);MAT_AUDIO_RECOVERY good=v2Start(3);
    // A same-generation conflict and an unsolicited larger START cannot mutate it.
    MAT_AUDIO_RECOVERY conflict=good;conflict.token[0]^=1;queueMatAudioRecoveryControl(&conflict);
    conflict=good;conflict.generation++;queueMatAudioRecoveryControl(&conflict);
    sendCycle(good.clientPort,756);CHECK(awaitCount(&delivered,2));
    atomic_store(&progressEpoch,756);atomic_store(&progressValue,1);CHECK(awaitCount(&playingCount,1));
    CHECK(!atomic_load(&terminated));
    uint64_t began=PltGetMillis();teardown();CHECK(PltGetMillis()-began<2000);
    int inits=atomic_load(&initialized);queueMatAudioRecoveryControl(&conflict);PltSleepMs(20);CHECK(atomic_load(&initialized)==inits);
}
static void stableNotificationCost(bool enabled) {
    eventRenderer = enabled;
    setup(false);
    v2Prepare(1,456);
    MAT_AUDIO_RECOVERY current = v2Start(1);
    sendCycle(current.clientPort,456);
    CHECK(awaitCount(&delivered,1));
    atomic_store(&progressEpoch,456);
    atomic_store(&progressValue,3);
    CHECK(awaitCount(&stableCount,1));
    unsigned before = atomic_load(&progressCalls);
    LiNotifyMatPlaybackFailure(123,7); // A delayed callback from the previous track.
    PltSleepMs(1000);
    unsigned calls = atomic_load(&progressCalls) - before;
    CHECK(!atomic_load(&failCount));
    CHECK(enabled ? calls == 0 : calls >= 5);
    uint64_t failedAt = PltGetMillis();
    if (enabled) LiNotifyMatPlaybackFailure(456,7);
    else atomic_store(&progressValue,-1);
    CHECK(awaitCount(&failCount,1));
    uint64_t latency = atomic_load(&failureReportedAt) - failedAt;
    CHECK(latency < 1000); // Includes the real RTP receive-thread join.
    printf("stable renderer events=%d progress calls/second=%u failure-to-stop=%llu ms failure-to-control=%llu ms\n",
           enabled, calls, (unsigned long long)(atomic_load(&rendererStoppedAt) - failedAt),
           (unsigned long long)latency);
    teardown();
    LiNotifyMatPlaybackFailure(456,7); // A posted focus callback after shutdown.
    eventRenderer = false;
}
static void peerFailureHasNoEcho(bool cleanupBlocked) {
    setup(false);
    MAT_AUDIO_RECOVERY peer = v2Prepare(1,456);
    CHECK(awaitCount(&readyCount,1));
    blockDuringCleanup = cleanupBlocked;
    peer.verb = MAT_RECOVERY_FAIL;
    peer.reason = MAT_RECOVERY_RETRYABLE;
    queueMatAudioRecoveryControl(&peer);
    CHECK(awaitCount(&cleaned,2));
    if (cleanupBlocked) CHECK(awaitCount(&failCount,1));
    queueMatAudioRecoveryControl(&peer);
    PltSleepMs(120);
    CHECK(atomic_load(&failCount) == (int)cleanupBlocked);
    CHECK(atomic_load(&cleaned) == 2 && !atomic_load(&terminated));
    teardown();
}
static void blockedV2PreservesConnection(void) {
    setup(false);blockDuringCleanup=true;v2Prepare(1,456);CHECK(awaitCount(&failCount,1));
    PltLockMutex(&testMutex);CHECK(lastFailure.reason==MAT_RECOVERY_BLOCKED);PltUnlockMutex(&testMutex);
    CHECK(atomic_load(&initialized)==1&&!atomic_load(&terminated));
    // Even a host attempt already in flight cannot clear the sticky barrier.
    v2Prepare(2,556);CHECK(awaitCount(&failCount,2));
    CHECK(atomic_load(&initialized)==1&&!atomic_load(&terminated));
    teardown();blockDuringCleanup=false;
}

// A SHIELD AudioTrack invalidated by HDMI removal rejects the next authenticated
// cycle. Only the existing recovery worker may replace that audio generation.
static void lostRendererRecoversOnReplacement(void) {
    testVersion=2;
    setup(false);
    atomic_store(&rejectNextWrite,true);
    sendCycle(oldPort,123);
    CHECK(awaitCount(&requestCount,1));
    CHECK(atomic_load(&lastRequestReason)==MAT_RECOVERY_RETRYABLE);
    CHECK(!atomic_load(&delivered) && !atomic_load(&terminated));
    v2Prepare(1,456);
    MAT_AUDIO_RECOVERY replacement=v2Start(1);
    sendCycle(replacement.clientPort,456);
    CHECK(awaitCount(&delivered,1));
    CHECK(!atomic_load(&playingCount));
    atomic_store(&progressEpoch,456);
    atomic_store(&progressValue,1);
    CHECK(awaitCount(&playingCount,1));
    atomic_store(&progressValue,3);
    CHECK(awaitCount(&stableCount,1));
    CHECK(atomic_load(&initialized)==2 && atomic_load(&cleaned)==1);
    CHECK(atomic_load(&requestCount)==1 && !atomic_load(&terminated));
    printf("Rejected renderer write: requests=%d initializations=%d delivered=%d playing=%d stable=%d terminations=%d\n",
           atomic_load(&requestCount),atomic_load(&initialized),atomic_load(&delivered),
           atomic_load(&playingCount),atomic_load(&stableCount),atomic_load(&terminated));
    teardown();
}

static void negotiatedBackground(void) {
    testVersion = 2;
    NegotiatedMatBackground = true;
    eventRenderer = true;
    atomic_store(&bgSentCount, 0);
    setup(false);
    CHECK(LiGetMatBackgroundSession() == 123);
    LiSetMatBackgroundPaused(122, true);
    LiSetMatBackgroundPaused(123, true);
    CHECK(awaitCount(&bgSentCount, 2)); // Drop first SUSPEND.
    MAT_BACKGROUND pause = latestBg();
    CHECK(pause.verb == MAT_BG_SUSPEND && pause.serial == 1 && pause.sessionEpoch == 123);
    CHECK(!atomic_load(&cleaned) && !atomic_load(&playingCount));
    pause.verb = MAT_BG_SUSPENDED;
    queueMatBackgroundControl(&pause);
    queueMatBackgroundControl(&pause);
    CHECK(awaitCount(&cleaned, 1));
    LiNotifyMatPlaybackFailure(123, 7);
    CHECK(!atomic_load(&failCount) && !atomic_load(&requestCount));
    sendCycle(oldPort, 123);
    LiSetMatBackgroundPaused(123, false);
    CHECK(awaitCount(&bgSentCount, 3));
    MAT_BACKGROUND resume = latestBg();
    CHECK(resume.verb == MAT_BG_RESUME && resume.serial == 2);
    queueMatBackgroundControl(&pause); // Late serial 1 ACK.
    v2Prepare(1, 456); // Obsolete in-flight attempt.
    v2Prepare(2, 556); // New PREPARE overtakes RESUMING.
    CHECK(!atomic_load(&readyCount));
    resume.verb = MAT_BG_RESUMING;
    resume.generation = 1;
    queueMatBackgroundControl(&resume);
    queueMatBackgroundControl(&resume);
    MAT_AUDIO_RECOVERY next = v2Start(1);
    CHECK(next.generation == 2 && next.newEpoch == 556);
    sendCycle(next.clientPort, 556);
    CHECK(awaitCount(&delivered, 1) && !atomic_load(&playingCount));
    atomic_store(&progressEpoch, 556);
    atomic_store(&progressValue, 1);
    CHECK(awaitCount(&playingCount, 1));
    CHECK(atomic_load(&initialized) == 2 && atomic_load(&cleaned) == 1 && !atomic_load(&terminated));
    atomic_store(&progressValue, 3);
    CHECK(awaitCount(&stableCount, 1)); // Wake the event-only worker on the next visibility change.
    // Return to foreground before the peer finished a second suspension.
    LiSetMatBackgroundPaused(123, true);
    CHECK(awaitCount(&bgSentCount, 4));
    MAT_BACKGROUND oldPause = latestBg(); oldPause.verb = MAT_BG_SUSPENDED;
    LiSetMatBackgroundPaused(123, false);
    queueMatBackgroundControl(&oldPause);
    CHECK(awaitCount(&bgSentCount, 5) && atomic_load(&cleaned) == 1);
    teardown();
    CHECK(!LiGetMatBackgroundSession());
    LiSetMatBackgroundPaused(123, true); // Late UI event during session teardown.
    NegotiatedMatBackground = false;
    eventRenderer = false;
    setup(false);
    CHECK(!LiGetMatBackgroundSession());
    LiSetMatBackgroundPaused(123, true);
    sendCycle(oldPort, 123);
    CHECK(awaitCount(&delivered, 1) && !atomic_load(&cleaned));
    teardown();
}

int main(void) {
    CHECK(initializePlatform()==0);CHECK(PltCreateMutex(&testMutex)==0);
    generationAllocationFailure();
    restartAndRejectOldEpoch();
    setup(false);
    MAT_AUDIO_RECOVERY host={0};host.verb=MAT_RECOVERY_PREPARE;host.generation=1;
    host.previousEpoch=123;host.newEpoch=456;host.domain=7;host.serverPort=AudioPortNumber;host.token[15]=9;
    queueMatAudioRecoveryControl(&host);CHECK(awaitCount(&readyCount,1));CHECK(!atomic_load(&requestCount));
    uint64_t began=PltGetMillis();teardown();CHECK(PltGetMillis()-began<2000);CHECK(!atomic_load(&terminated));
    setup(true);CHECK(requestMatAudioRecovery(1));CHECK(awaitCount(&terminated,1));teardown();
    repeatedV2WithOldPort();
    failedV2CannotRevive();
    stableNotificationCost(false);
    stableNotificationCost(true);
    peerFailureHasNoEcho(false);
    peerFailureHasNoEcho(true);
    blockedV2PreservesConnection();
    lostRendererRecoversOnReplacement();
    negotiatedBackground();
    PltDeleteMutex(&testMutex);cleanupPlatform();
    printf("MAT audio recovery: failures=%d\n",atomic_load(&failures));
    return atomic_load(&failures)?1:0;
}
