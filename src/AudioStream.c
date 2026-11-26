#include "Limelight-internal.h"
#include "EndpointRefresh.h"
#include "MatLivePlayback.h"
#include "MatLiveCrypto.h"
#include <stdatomic.h>
static atomic_bool matInputEnded;
static atomic_ullong matLastAcceptedMs;
static MAT_LIVE_PLAYBACK* matLive;
static LI_MAT_PLAYBACK_CALLBACKS matCallbacks;
static bool matPlaybackCompleteReported;
static atomic_bool matFailureReported;
static atomic_uint matPendingPackets;
static atomic_ullong matCompletedCycles, matSuppressedFailures;
static atomic_bool matGenerationStopping;
static atomic_bool matRecoveryAccepting, matRecoveryStopping;
static atomic_bool initialAudioStartComplete;
static PLT_MUTEX matRecoveryMutex;
static PLT_EVENT matRecoveryEvent;
static PLT_THREAD matRecoveryThread;
static bool matRecoveryThreadStarted, matRecoveryAttempted, matRecoveryBusy;
static bool matRecoveryHavePrepare, matRecoveryHaveStart, matRecoveryFailed, matRecoveryResendReady, matRecoveryReady;
static uint64_t matRecoveryEpoch;
static MAT_AUDIO_RECOVERY matRecoveryProfile;
static void* matAudioContext;
static int matAudioFlags;
static bool audioPrepared, receiveStarted, decoderStarted;
static int initializeAudioGeneration(void);
static void destroyAudioGeneration(void);
static void stopAudioGeneration(void);
static int prepareAudioGeneration(void* audioContext, int arFlags);
static int startAudioGeneration(void);
static void MatRecoveryThreadProc(void* context);
/* v2 fields are protected by matRecoveryMutex. Only MatRecover owns media
 * teardown/setup; the control receiver never creates or destroys resources. */
typedef enum { MAT_V2_RUNNING, MAT_V2_REQUESTED, MAT_V2_PENDING, MAT_V2_READY,
               MAT_V2_CONFIRMING, MAT_V2_PLAYING, MAT_V2_FAILED } MAT_V2_PHASE;
static MAT_V2_PHASE matV2Phase;
static MAT_AUDIO_RECOVERY matV2Profile;
static uint32_t matV2InstalledGeneration;
static uint64_t matV2Deadline;
static bool matV2Report, matV2ResendReady, matV2Start, matV2PlayingSent, matV2StableSent;
static atomic_bool matRecoveryBlocked;
static atomic_ullong matRejectedRtp;
static void MatRecoveryV2ThreadProc(void);
static bool requestMatRecoveryV2(uint32_t reason);
static void queueMatRecoveryV2(const MAT_AUDIO_RECOVERY* m);

void LiBlockMatAudioRecovery(void) {
    // Cleared only by initializeAudioStream(), never by a generation rebuild.
    atomic_store(&matRecoveryBlocked, true);
}

static bool matV2AcceptRtp(const uint8_t* bytes, size_t length) {
    if (length < sizeof(RTP_PACKET)) return false;
    const RTP_PACKET* packet = (const RTP_PACKET*)bytes;
    if (packet->header != 0x80 || BE32(packet->ssrc) != NegotiatedMatAudioSsrc) return false;
    if (packet->packetType == 97) {
        return length == sizeof(RTP_PACKET) + MAT_LIVE_PACKET &&
            mat_get64(bytes + sizeof(RTP_PACKET) + 8) == MatLiveProfile.epoch;
    }
    if (packet->packetType == 127 && length == sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + MAT_LIVE_PACKET) {
        const AUDIO_FEC_HEADER* fec = (const AUDIO_FEC_HEADER*)(packet + 1);
        return fec->payloadType == 97 && BE32(fec->ssrc) == NegotiatedMatAudioSsrc;
    }
    return false;
}

// Each worker owns its counters. Only the small cross-worker gauges are atomic.
typedef struct {
    uint64_t nextLogUs, packets, lastReceiveUs, maxReceiveGapUs;
    uint64_t maxQueueAgeUs, maxConsumeUs, consumeBuckets[4];
    uint64_t currentSubmitUs, slowStartUs, slowQueueUs, slowProcessUs, slowSubmitUs;
    unsigned peakQueue;
    uint64_t lastKernelUs, kernelIntervals, missingKernelIntervals, maxKernelGapUs, lastCompleteUs;
    uint16_t lastSequence;
    uint8_t lastPayloadType;
    // Session totals follow the per-generation fields. 25 us buckets saturate at 64 ms.
    uint64_t gapHistogram[2561];
    uint64_t gapSamples;
} MAT_AUDIO_DIAGNOSTICS;
static MAT_AUDIO_DIAGNOSTICS matReceiveDiagnostics, matDecodeDiagnostics;
#define MAT_PACKET_QUEUE_BOUND 512
static void recordMatGap(MAT_AUDIO_DIAGNOSTICS* d, uint64_t gap) {
    unsigned bucket = gap >= 64000 ? 2560 : (unsigned)(gap / 25);
    ++d->gapHistogram[bucket];
    ++d->gapSamples;
}

static void recordMatArrival(MAT_AUDIO_DIAGNOSTICS* d, const RTP_PACKET* rtp, uint64_t now, uint64_t kernelUs) {
    bool haveAppGap = d->packets && now >= d->lastReceiveUs;
    uint64_t gap = haveAppGap ? now - d->lastReceiveUs : 0;
    bool haveKernelGap = kernelUs && d->lastKernelUs && kernelUs >= d->lastKernelUs;
    uint64_t kernelGap = haveKernelGap ? kernelUs - d->lastKernelUs : 0;
    if (d->packets) {
        if (haveAppGap) recordMatGap(d, gap);
        if (haveKernelGap) ++d->kernelIntervals;
        else ++d->missingKernelIntervals;
        if (kernelGap > d->maxKernelGapUs) d->maxKernelGapUs = kernelGap;
        if (gap > 50000 || kernelGap > 50000)
            Limelog("MAT receive gap epoch=%llu prevRtp=%u/%u rtp=%u/%u appUs=%llu kernelUs=%lld pending=%u peakPending=%u\n",
                (unsigned long long)MatLiveProfile.epoch, d->lastPayloadType, d->lastSequence,
                rtp->packetType, BE16(rtp->sequenceNumber), (unsigned long long)gap,
                haveKernelGap ? (long long)kernelGap : -1LL, atomic_load(&matPendingPackets), d->peakQueue);
    }
    d->lastSequence = BE16(rtp->sequenceNumber);
    d->lastPayloadType = rtp->packetType;
    d->lastKernelUs = kernelUs;
    if (gap > d->maxReceiveGapUs) d->maxReceiveGapUs = gap;
    d->lastReceiveUs = now;
    ++d->packets;
}

static unsigned matGapP99(const MAT_AUDIO_DIAGNOSTICS* d) {
    uint64_t wanted = d->gapSamples - d->gapSamples / 100;
    if (!wanted) return 0;
    for (unsigned i = 0; i < 2561; ++i) {
        if (wanted <= d->gapHistogram[i]) return i * 25;
        wanted -= d->gapHistogram[i];
    }
    return 64000;
}

static void logMatAudioDiagnostics(MAT_AUDIO_DIAGNOSTICS* d, const char* worker, bool force, uint64_t now) {
    if (!force && now < d->nextLogUs) return;
    Limelog("MAT audio %s: epoch=%llu packets=%llu cycles=%llu pending=%u peakPending=%u maxReceiveGapUs=%llu maxQueueAgeUs=%llu maxAuthenticateReassembleSubmitUs=%llu consumeBuckets=%llu/%llu/%llu/%llu suppressedFailures=%llu\n",
        worker, (unsigned long long)MatLiveProfile.epoch, (unsigned long long)d->packets,
        (unsigned long long)atomic_load_explicit(&matCompletedCycles, memory_order_relaxed),
        atomic_load_explicit(&matPendingPackets, memory_order_relaxed), d->peakQueue,
        (unsigned long long)d->maxReceiveGapUs, (unsigned long long)d->maxQueueAgeUs,
        (unsigned long long)d->maxConsumeUs, (unsigned long long)d->consumeBuckets[0],
        (unsigned long long)d->consumeBuckets[1], (unsigned long long)d->consumeBuckets[2],
        (unsigned long long)d->consumeBuckets[3],
        (unsigned long long)atomic_load_explicit(&matSuppressedFailures, memory_order_relaxed));
    Limelog("MAT gaps worker=%s samples=%llu p99FloorUs=%u kernelIntervals=%llu missingKernelIntervals=%llu maxKernelGapUs=%llu (histogram=session kernel=window)\n",
        worker, (unsigned long long)d->gapSamples, matGapP99(d), (unsigned long long)d->kernelIntervals,
        (unsigned long long)d->missingKernelIntervals, (unsigned long long)d->maxKernelGapUs);
    if (d->slowStartUs) {
        Limelog("MAT slowest decode: epoch=%llu startUs=%llu queueUs=%llu authenticateReassembleUs=%llu rendererSubmitUs=%llu\n",
            (unsigned long long)MatLiveProfile.epoch, (unsigned long long)d->slowStartUs,
            (unsigned long long)d->slowQueueUs, (unsigned long long)d->slowProcessUs, (unsigned long long)d->slowSubmitUs);
        d->slowStartUs = 0;
    }
    d->nextLogUs = now + 5000000;
    d->maxReceiveGapUs = d->maxQueueAgeUs = d->maxConsumeUs = 0;
    d->kernelIntervals = d->missingKernelIntervals = d->maxKernelGapUs = 0;
    d->peakQueue = 0;
    memset(d->consumeBuckets, 0, sizeof(d->consumeBuckets));
}
static void reportMatFailure(const char* stage, MAT_AUDIO_DIAGNOSTICS* d,
                             unsigned expectedRtp, unsigned actualRtp, int error) {
    if (atomic_load(&matGenerationStopping)) return;
    if (atomic_exchange_explicit(&matFailureReported, true, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&matSuppressedFailures, 1, memory_order_relaxed);
        return;
    }
    Limelog("MAT first failure: stage=%s epoch=%llu domain=%llu completedCycles=%llu expectedRtp=%u actualRtp=%u error=%d\n",
        stage, (unsigned long long)MatLiveProfile.epoch, (unsigned long long)MatLiveProfile.domain,
        (unsigned long long)atomic_load_explicit(&matCompletedCycles, memory_order_relaxed),
        expectedRtp, actualRtp, error);
    logMatAudioDiagnostics(d, stage, true, PltGetMicroseconds());
    if (!requestMatAudioRecovery(MAT_RECOVERY_RETRYABLE)) ListenerCallbacks.connectionTerminated(error);
}
static void startMatAudioWorker(int role) {
    if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) && matCallbacks.threadStart) {
        matCallbacks.threadStart(matCallbacks.context, role);
    }
}
void LiSetMatPlaybackCallbacks(const LI_MAT_PLAYBACK_CALLBACKS* callbacks) {
    if(callbacks) matCallbacks=*callbacks; else memset(&matCallbacks,0,sizeof(matCallbacks));
}
static int matEnd(void* context,uint64_t cycles) {
    // A verified END may take time to drain the hardware. Stop the network
    // watchdog before that bounded drain, not before authentication.
    atomic_store_explicit(&matInputEnded,true,memory_order_relaxed);
    return matCallbacks.end(context,cycles);
}
static int timedMatCycle(void* context, const unsigned char* data, int size, uint64_t epoch,
                         uint64_t domain, uint64_t index, uint64_t pts) {
    uint64_t start = PltGetMicroseconds();
    int result = matCallbacks.cycle(context, data, size, epoch, domain, index, pts);
    matDecodeDiagnostics.currentSubmitUs = PltGetMicroseconds() - start;
    return result;
}
static bool matInputTimedOut(void) {
    if(!AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)||atomic_load_explicit(&matInputEnded,memory_order_relaxed))return false;
    // The decoder can publish a newer timestamp while the receive thread runs.
    // Load it before reading the clock so unsigned subtraction cannot underflow.
    uint64_t last=atomic_load_explicit(&matLastAcceptedMs,memory_order_relaxed);
    uint64_t now=PltGetMillis();
    return now>=last && now-last>=2000;
}
static void failMatInput(void) { MatLivePlaybackFail(matLive); }
static void consumeAudio(char* data,int size, uint64_t readyUs) {
    if(NegotiatedAudioCodec==AUDIO_CODEC_MAT_LIVE) {
        uint64_t started = PltGetMicroseconds();
        matDecodeDiagnostics.currentSubmitUs = 0;
        int result=MatLivePlaybackPush(matLive,(unsigned char*)data,(size_t)(size>0?size:0));
        uint64_t elapsed = PltGetMicroseconds() - started;
        ++matDecodeDiagnostics.packets;
        if (elapsed > matDecodeDiagnostics.maxConsumeUs) {
            matDecodeDiagnostics.maxConsumeUs = elapsed;
            // An ordinary generation submits a carrier every 20 ms. Keep the
            // components of the same outlier so unrelated maxima aren't added.
            if (elapsed >= 20000 && result != 2) {
                matDecodeDiagnostics.slowStartUs = started;
                matDecodeDiagnostics.slowQueueUs = readyUs && started >= readyUs ? started - readyUs : 0;
                matDecodeDiagnostics.slowSubmitUs = matDecodeDiagnostics.currentSubmitUs;
                matDecodeDiagnostics.slowProcessUs = elapsed - matDecodeDiagnostics.currentSubmitUs;
            }
        }
        ++matDecodeDiagnostics.consumeBuckets[elapsed < 100 ? 0 : elapsed < 1000 ? 1 : elapsed < 10000 ? 2 : 3];
        if (result == 1) {
            atomic_fetch_add_explicit(&matCompletedCycles, 1, memory_order_relaxed);
            if (readyUs && matDecodeDiagnostics.lastCompleteUs && readyUs >= matDecodeDiagnostics.lastCompleteUs)
                recordMatGap(&matDecodeDiagnostics, readyUs - matDecodeDiagnostics.lastCompleteUs);
            matDecodeDiagnostics.lastCompleteUs = readyUs;
        }
        if(result>=0)atomic_store_explicit(&matLastAcceptedMs,PltGetMillis(),memory_order_relaxed);
        if(result<0) {
            if(NegotiatedMatAudioRecovery==2 && MatLivePlaybackRecoveryBlocked(matLive)) LiBlockMatAudioRecovery();
            reportMatFailure("authentication_sequence_or_renderer",&matDecodeDiagnostics,0,0,-1);
        }
        else if(result==2&&!matPlaybackCompleteReported){matPlaybackCompleteReported=true;Limelog("Live MAT authenticated EOS and renderer drain completed\n");}
        logMatAudioDiagnostics(&matDecodeDiagnostics, "decode", false, PltGetMicroseconds());
        return;
    }
    AudioCallbacks.decodeAndPlaySample(data,size);
}

static SOCKET rtpSocket = INVALID_SOCKET;
static LI_ENDPOINT_REFRESH endpointRefresh;

static LINKED_BLOCKING_QUEUE packetQueue;
// Only full receive buffers can go back to recv(). Reconstructed packets and
// loss placeholders have variable-sized allocations owned by the FEC queue.
static LINKED_BLOCKING_QUEUE matFreePackets;
static bool matPoolReady;
static RTP_AUDIO_QUEUE rtpAudioQueue;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static PPLT_CRYPTO_CONTEXT audioDecryptionCtx;
static uint32_t avRiKeyId;

static unsigned short lastSeq;

static bool pingThreadStarted;
static bool receivedDataFromPeer;
static uint64_t firstReceiveTime;

#ifdef LC_DEBUG
#define INVALID_OPUS_HEADER 0x00
static uint8_t opusHeaderByte;
#endif

// Sized to fit:
//   - Opus packets (typically <= 1400 B)
//   - AC3/E-AC3 frames at up to 640 kbps / 32 ms (~2.5 KB)
//   - Raw PCM_S16 5.1 @ 5 ms (= 240 * 6 * 2 = 2880 B)
//   - Raw PCM_S16 7.1.4 @ 5 ms (= 240 * 12 * 2 = 5760 B)
// Receivers always allocate this size; senders still respect the network
// MTU when fragmenting upstream.
#define MAX_PACKET_SIZE 8192

typedef struct _QUEUE_AUDIO_PACKET_HEADER {
    LINKED_BLOCKING_QUEUE_ENTRY lentry;
    int size;
    bool reusable;
    uint64_t queuedAtUs;
} QUEUED_AUDIO_PACKET_HEADER, *PQUEUED_AUDIO_PACKET_HEADER;

typedef struct _QUEUED_AUDIO_PACKET {
    QUEUED_AUDIO_PACKET_HEADER header;
    char data[MAX_PACKET_SIZE];
} QUEUED_AUDIO_PACKET, *PQUEUED_AUDIO_PACKET;

static PQUEUED_AUDIO_PACKET acquireReceivePacket(void) {
    PQUEUED_AUDIO_PACKET packet;
    if (matPoolReady && LbqPollQueueElement(&matFreePackets, (void**)&packet) == LBQ_SUCCESS) {
        return packet;
    }
    packet = malloc(sizeof(*packet));
    if (packet != NULL) {
        packet->header.reusable = matPoolReady;
    }
    return packet;
}

static void releaseDecodedPacket(PQUEUED_AUDIO_PACKET_HEADER packet) {
    if (packet->reusable &&
        LbqOfferQueueItem(&matFreePackets, packet, &packet->lentry) == LBQ_SUCCESS) {
        return;
    }
    free(packet);
}

static void AudioPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(AudioPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, AudioPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (LiErClientPing(&endpointRefresh)) {
            // Negotiated keepalive; audio may still be waiting for ANNOUNCE.
        }
        else if (NegotiatedMatAudioRecovery || AudioPingPayload.payload[0] != 0) {
            pingCount++;
            AudioPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&AudioPingPayload, sizeof(AudioPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Initialize the audio stream and start
int initializeAudioStream(void) {
    int err = PltCreateMutex(&matRecoveryMutex);
    if (err != 0) return err;
    err = PltCreateEvent(&matRecoveryEvent);
    if (err != 0) { PltDeleteMutex(&matRecoveryMutex); return err; }
    matRecoveryThreadStarted = matRecoveryAttempted = matRecoveryBusy = false;
    matRecoveryHavePrepare = matRecoveryHaveStart = matRecoveryFailed = matRecoveryResendReady = matRecoveryReady = false;
    memset(&matRecoveryProfile, 0, sizeof(matRecoveryProfile));
    atomic_store(&matRecoveryAccepting, false);
    atomic_store(&matRecoveryStopping, false);
    atomic_store(&initialAudioStartComplete, false);
    audioPrepared = receiveStarted = decoderStarted = false;
    matV2Phase = MAT_V2_RUNNING;
    memset(&matV2Profile, 0, sizeof(matV2Profile));
    matV2InstalledGeneration = 0;
    matV2Report = matV2ResendReady = matV2Start = matV2PlayingSent = matV2StableSent = false;
    atomic_store(&matRecoveryBlocked, false);
    memset(&matReceiveDiagnostics, 0, sizeof(matReceiveDiagnostics));
    memset(&matDecodeDiagnostics, 0, sizeof(matDecodeDiagnostics));
    return initializeAudioGeneration();
}

static int initializeAudioGeneration(void) {
    // RTSP can still select Opus for a MAT preference. Final receive parameters
    // are set in startAudioStream(), before either queue worker starts.
    LbqInitializeLinkedBlockingQueue(&packetQueue, 30);
    RtpaInitializeQueue(&rtpAudioQueue);
    lastSeq = 0;
    matPlaybackCompleteReported=false;
    atomic_store_explicit(&matFailureReported,false,memory_order_relaxed);
    atomic_store_explicit(&matPendingPackets,0,memory_order_relaxed);
    atomic_store_explicit(&matCompletedCycles,0,memory_order_relaxed);
    atomic_store_explicit(&matSuppressedFailures,0,memory_order_relaxed);
    atomic_store(&matGenerationStopping, false);
    atomic_store(&matRejectedRtp, 0);
    // A new socket has a new RTP sequence and kernel clock baseline. Keep the session histograms.
    memset(&matReceiveDiagnostics, 0, offsetof(MAT_AUDIO_DIAGNOSTICS, gapHistogram));
    memset(&matDecodeDiagnostics, 0, offsetof(MAT_AUDIO_DIAGNOSTICS, gapHistogram));
    matReceiveDiagnostics.nextLogUs = matDecodeDiagnostics.nextLogUs = PltGetMicroseconds() + 5000000;
    receivedDataFromPeer = false;
    pingThreadStarted = false;
    firstReceiveTime = 0;
    audioDecryptionCtx = PltCreateCryptoContext();
#ifdef LC_DEBUG
    opusHeaderByte = INVALID_OPUS_HEADER;
#endif

    // Copy and byte-swap the AV RI key ID used for the audio encryption IV
    memcpy(&avRiKeyId, StreamConfig.remoteInputAesIv, sizeof(avRiKeyId));
    avRiKeyId = BE32(avRiKeyId);

    return 0;
}

// Each recovered MAT generation has its own socket and authentication token.
static bool initializeAudioEndpoint(void) {
    return LiErClientInitialize(&endpointRefresh, rtpSocket, LI_ER_AUDIO,
                                AudioPingPayload.payload, AudioPortNumber);
}

// This is called when the RTSP SETUP message is parsed and the audio port
// number is parsed out of it. Alternatively, it's also called if parsing fails
// and will use the well known audio port instead.
int notifyAudioPortNegotiationComplete(void) {
    LC_ASSERT(!pingThreadStarted);
    LC_ASSERT(AudioPortNumber != 0);

    // For GFE 3.22 compatibility, we must start the audio ping thread before the RTSP handshake.
    // It will not reply to our RTSP PLAY request until the audio ping has been received.
    // MAT carries about 41.5 Mbps including FEC. Preserve the initial carrier
    // while the renderer starts instead of relying on an Opus-sized OS default.
    int receiveBufferSize = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) ? 1024 * 1024 : 0;
    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, receiveBufferSize, SOCK_QOS_TYPE_AUDIO);
    if (rtpSocket == INVALID_SOCKET) {
        return LastSocketFail();
    }
    if (!initializeAudioEndpoint()) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
        return -1;
    }
    LiRecordStreamSocket(rtpSocket, STREAM_SOCKET_SLOT_AUDIO);

    // We may receive audio before our threads are started, but that's okay. We'll
    // drop the first 1 second of audio packets to catch up with the backlog.
    // MAT starts the authentic ping only AFTER its receiver/consumer are ready.
    // The host waits for it, so no startup data is buffered or discarded.
    if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec))return 0;
    int err = PltCreateThread("AudioPing", AudioPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        return err;
    }

    pingThreadStarted = true;
    return 0;
}

static void freePacketList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored within the data allocation
        free(entry->data);

        entry = nextEntry;
    }
}

// Tear down the audio stream once we're done with it
void destroyAudioStream(void) {
    atomic_store(&matRecoveryAccepting, false);
    destroyAudioGeneration();
    PltCloseEvent(&matRecoveryEvent);
    PltDeleteMutex(&matRecoveryMutex);
}

static void destroyAudioGeneration(void) {
    MatLivePlaybackClose(matLive);matLive=NULL;
    if (rtpSocket != INVALID_SOCKET) {
        if (pingThreadStarted) {
            PltInterruptThread(&udpPingThread);
            PltJoinThread(&udpPingThread);
        }

        LiRecordStreamSocket(INVALID_SOCKET, STREAM_SOCKET_SLOT_AUDIO);
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    LiErClientDestroy(&endpointRefresh);
    PltDestroyCryptoContext(audioDecryptionCtx);
    audioDecryptionCtx = NULL;
    freePacketList(LbqDestroyLinkedBlockingQueue(&packetQueue));
    if (matPoolReady) {
        LbqSignalQueueShutdown(&matFreePackets);
        freePacketList(LbqDestroyLinkedBlockingQueue(&matFreePackets));
        matPoolReady = false;
    }
    RtpaCleanupQueue(&rtpAudioQueue);
}

static bool queuePacketToLbq(PQUEUED_AUDIO_PACKET* packet) {
    int err;

    do {
        unsigned pending = 0;
        if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
            // queuedAtUs is the time this packet became deliverable.
            // Publish before offering: the decoder may take ownership immediately.
            pending = atomic_fetch_add_explicit(&matPendingPackets, 1, memory_order_relaxed) + 1;
        }
        err = LbqOfferQueueItem(&packetQueue, *packet, &(*packet)->header.lentry);
        if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
            if (err != LBQ_SUCCESS) atomic_fetch_sub_explicit(&matPendingPackets, 1, memory_order_relaxed);
            else if (pending > matReceiveDiagnostics.peakQueue) matReceiveDiagnostics.peakQueue = pending;
        }
        if (err == LBQ_SUCCESS) {
            // The LBQ owns the buffer now
            *packet = NULL;
        }
        else if (err == LBQ_BOUND_EXCEEDED) {
            if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)){
                reportMatFailure("decode_queue_overflow", &matReceiveDiagnostics, 0, 0, -1);
                return false; // Don't flush a loss-intolerant stream or silently resume.
            }
            Limelog("Audio packet queue overflow\n");

            // The audio queue is full, so free all existing items and try again
            freePacketList(LbqFlushQueueItems(&packetQueue));
        }
    } while (err == LBQ_BOUND_EXCEEDED);

    return err == LBQ_SUCCESS;
}

static void decodeInputData(PQUEUED_AUDIO_PACKET packet) {
    if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) && atomic_load_explicit(&matFailureReported, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&matSuppressedFailures, 1, memory_order_relaxed);
        return;
    }
    // If the packet size is zero, this is a placeholder for a missing
    // packet. Trigger packet loss concealment logic in libopus by
    // invoking the decoder with a NULL buffer.
    if (packet->header.size == 0) {
        if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
            failMatInput();
            reportMatFailure("rtp_missing", &matDecodeDiagnostics, (unsigned short)(lastSeq + 1), 0, -1);
            return;
        }
        consumeAudio(NULL, 0, 0);
        return;
    }
    PRTP_PACKET rtp = (PRTP_PACKET)&packet->data[0];
    if (lastSeq != 0 && (unsigned short)(lastSeq + 1) != rtp->sequenceNumber) {
        if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)){
            failMatInput();
            reportMatFailure("rtp_sequence", &matDecodeDiagnostics, (unsigned short)(lastSeq + 1), rtp->sequenceNumber, -1);
            return;
        }
        Limelog("Network dropped audio data (expected %d, but received %d)\n", lastSeq + 1, rtp->sequenceNumber);
    }

    lastSeq = rtp->sequenceNumber;

    if (AudioEncryptionEnabled && NegotiatedAudioCodec!=AUDIO_CODEC_MAT_LIVE) {
        // We must have room for the AES padding which may be written to the buffer
        unsigned char decryptedOpusData[ROUND_TO_PKCS7_PADDED_LEN(MAX_PACKET_SIZE)];
        unsigned char iv[16] = { 0 };
        int dataLength = packet->header.size - sizeof(*rtp);

        LC_ASSERT(dataLength <= MAX_PACKET_SIZE);

        // The IV is the avkeyid (equivalent to the rikeyid) +
        // the RTP sequence number, in big endian.
        uint32_t ivSeq = BE32(avRiKeyId + rtp->sequenceNumber);

        memcpy(iv, &ivSeq, sizeof(ivSeq));

        if (!PltDecryptMessage(audioDecryptionCtx, ALGORITHM_AES_CBC, CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH,
                               (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                               iv, sizeof(iv),
                               NULL, 0,
                               (unsigned char*)(rtp + 1), dataLength,
                               decryptedOpusData, &dataLength)) {
            Limelog("Failed to decrypt audio packet (sequence number: %u)\n", rtp->sequenceNumber);
            if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)){failMatInput();consumeAudio(NULL,0,0);}
            LC_ASSERT_VT(false);
            return;
        }

#ifdef LC_DEBUG
        if (opusHeaderByte == INVALID_OPUS_HEADER) {
            opusHeaderByte = decryptedOpusData[0];
            LC_ASSERT_VT(opusHeaderByte != INVALID_OPUS_HEADER);
        }
        else {
            // Opus header should stay constant for the entire stream.
            // If it doesn't, it may indicate that the RtpAudioQueue
            // incorrectly recovered a data shard or the decryption
            // of the audio packet failed. Sunshine violates this for
            // surround sound in some cases, so just ignore it.
            LC_ASSERT_VT(decryptedOpusData[0] == opusHeaderByte || IS_SUNSHINE());
        }
#endif

        consumeAudio((char*)decryptedOpusData, dataLength, packet->header.queuedAtUs);
    }
    else {
#ifdef LC_DEBUG
        if (opusHeaderByte == INVALID_OPUS_HEADER) {
            opusHeaderByte = ((uint8_t*)(rtp + 1))[0];
            LC_ASSERT_VT(opusHeaderByte != INVALID_OPUS_HEADER);
        }
        else {
            // Opus header should stay constant for the entire stream.
            // If it doesn't, it may indicate that the RtpAudioQueue
            // incorrectly recovered a data shard. Sunshine violates
            // this for surround sound in some cases, so just ignore it.
            LC_ASSERT_VT(((uint8_t*)(rtp + 1))[0] == opusHeaderByte || IS_SUNSHINE());
        }
#endif

        consumeAudio((char*)(rtp + 1), packet->header.size - sizeof(*rtp), packet->header.queuedAtUs);
    }

    // All media comes from the original UDP socket in the conservative
    // recovery mode. Accepted audio refreshes endpoint health.
    LiErClientMedia(&endpointRefresh);
}

static void AudioReceiveThreadProc(void* context) {
    PRTP_PACKET rtp;
    PQUEUED_AUDIO_PACKET packet;
    int queueStatus;
    bool useSelect;
    uint32_t packetsToDrop;
    int waitingForAudioMs;


    startMatAudioWorker(LI_MAT_THREAD_RECEIVE);
    bool mat = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec);
    bool kernelTiming = mat && enableUdpReceiveTimestamps(rtpSocket);
    packet = NULL;
    packetsToDrop = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)?0:500 / AudioPacketDuration;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    waitingForAudioMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        if(matInputTimedOut()) {
            reportMatFailure("validated_input_timeout", &matReceiveDiagnostics, 0, 0, -1);break;
        }
        if (packet == NULL) {
            packet = acquireReceivePacket();
            if (packet == NULL) {
                if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) reportMatFailure("receive_allocate", &matReceiveDiagnostics, 0, 0, -1);
                else { Limelog("Audio Receive: malloc() failed\n"); ListenerCallbacks.connectionTerminated(-1); }
                break;
            }
        }

        uint64_t kernelUs = 0;
        packet->header.size = LiErClientRecvTimed(&endpointRefresh, &rtpSocket, &packet->data[0], MAX_PACKET_SIZE, useSelect, 0,
                                                  kernelTiming ? &kernelUs : NULL);
        if (packet->header.size == LI_ER_RECV_HANDLED) {
            continue;
        }
        else if (packet->header.size == LI_ER_RECV_DEGRADED) {
            Limelog("Audio endpoint is degraded; keeping the session alive\n");
            continue;
        }
        else if (packet->header.size < 0) {
            if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) reportMatFailure("receive_socket", &matReceiveDiagnostics, 0, 0, LastSocketFail());
            else { Limelog("Audio Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError()); ListenerCallbacks.connectionTerminated(LastSocketFail()); }
            break;
        }
        else if (packet->header.size == 0) {
            // Receive timed out; try again
            if (!receivedDataFromPeer) {
                waitingForAudioMs += UDP_RECV_POLL_TIMEOUT_MS;
            }
            else {
                // If we hit this path, there are no queued audio packets on the host PC,
                // so we don't need to drop anything.
                packetsToDrop = 0;
            }
            continue;
        }

        if (packet->header.size < (int)sizeof(RTP_PACKET)) {
            // Runt packet
            continue;
        }

        rtp = (PRTP_PACKET)&packet->data[0];
        if (NegotiatedMatAudioRecovery == 2 &&
            !matV2AcceptRtp((const uint8_t*)packet->data, (size_t)packet->header.size)) {
            atomic_fetch_add_explicit(&matRejectedRtp, 1, memory_order_relaxed);
            continue;
        }
        uint64_t receivedUs = mat ? PltGetMicroseconds() : 0;
        if (mat) {
            logMatAudioDiagnostics(&matReceiveDiagnostics, "receive", false, receivedUs);
            // An interval belongs to the window in which its ending packet arrives.
            recordMatArrival(&matReceiveDiagnostics, rtp, receivedUs, kernelUs);
        }

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first audio packet after %d ms\n", waitingForAudioMs);

            if (firstReceiveTime != 0 && !AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
                // XXX firstReceiveTime is never set here...
                // We're already dropping 500ms of audio so this probably doesn't matter
                packetsToDrop += (uint32_t)(PltGetMillis() - firstReceiveTime) / AudioPacketDuration;
            }

            Limelog("Initial audio resync period: %d milliseconds\n", packetsToDrop * AudioPacketDuration);
        }

        // GFE accumulates audio samples before we are ready to receive them, so
        // we will drop the ones that arrived before the receive thread was ready.
        if (packetsToDrop > 0) {
            // Only count actual audio data (not FEC) in the packets to drop calculation
            if (rtp->packetType == 97) {
                packetsToDrop--;
            }
            continue;
        }

        // Convert fields to host byte-order
        rtp->sequenceNumber = BE16(rtp->sequenceNumber);
        rtp->timestamp = BE32(rtp->timestamp);
        rtp->ssrc = BE32(rtp->ssrc);

        packet->header.queuedAtUs = receivedUs;
        queueStatus = RtpaAddPacket(&rtpAudioQueue, (PRTP_PACKET)&packet->data[0], (uint16_t)packet->header.size);
        if (RTPQ_HANDLE_NOW(queueStatus)) {
            if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                if (!queuePacketToLbq(&packet)) {
                    // An exit signal was received
                    break;
                }
                else {
                    // Ownership should have been taken by the LBQ
                    LC_ASSERT(packet == NULL);
                }
            }
            else {
                decodeInputData(packet);
            }
        }
        else {
            if (RTPQ_PACKET_CONSUMED(queueStatus)) {
                // The queue consumed our packet, so we must allocate a new one
                packet = NULL;
            }

            if (RTPQ_PACKET_READY(queueStatus)) {
                // If packets are ready, pull them and send them to the decoder
                uint16_t length;
                PQUEUED_AUDIO_PACKET queuedPacket;
                while ((queuedPacket = (PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(&rtpAudioQueue, sizeof(QUEUED_AUDIO_PACKET_HEADER), &length)) != NULL) {
                    // Populate header data (not preserved in queued packets)
                    queuedPacket->header.size = length;
                    queuedPacket->header.reusable = false;
                    queuedPacket->header.queuedAtUs = receivedUs;

                    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                        if (!queuePacketToLbq(&queuedPacket)) {
                            // An exit signal was received
                            free(queuedPacket);
                            break;
                        }
                        else {
                            // Ownership should have been taken by the LBQ
                            LC_ASSERT(queuedPacket == NULL);
                        }
                    }
                    else {
                        decodeInputData(queuedPacket);
                        free(queuedPacket);
                    }
                }

                // Break on exit
                if (queuedPacket != NULL) {
                    break;
                }
            }
        }
    }

    if (packet != NULL) {
        free(packet);
    }
}

static void AudioDecoderThreadProc(void* context) {
    int err;
    PQUEUED_AUDIO_PACKET packet;

    startMatAudioWorker(LI_MAT_THREAD_DECODE);
    while (!PltIsThreadInterrupted(&decoderThread)) {
        err = LbqWaitForQueueElement(&packetQueue, (void**)&packet);
        if (err != LBQ_SUCCESS) {
            // An exit signal was received
            return;
        }

        if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
            atomic_fetch_sub_explicit(&matPendingPackets, 1, memory_order_relaxed);
            uint64_t age = PltGetMicroseconds() - packet->header.queuedAtUs;
            if (age > matDecodeDiagnostics.maxQueueAgeUs) matDecodeDiagnostics.maxQueueAgeUs = age;
        }
        decodeInputData(packet);

        releaseDecodedPacket(&packet->header);
    }
}

void stopAudioStream(void) {
    atomic_store(&matRecoveryAccepting, false);
    atomic_store(&matRecoveryStopping, true);
    if (matRecoveryThreadStarted) {
        PltInterruptThread(&matRecoveryThread);
        PltSetEvent(&matRecoveryEvent);
        PltJoinThread(&matRecoveryThread);
        matRecoveryThreadStarted = false;
    }
    stopAudioGeneration();
}

static void stopAudioGeneration(void) {
    atomic_store(&matGenerationStopping, true);
    if (!receivedDataFromPeer) {
        Limelog("No audio traffic was ever received from the host!\n");
    }

    if (audioPrepared) AudioCallbacks.stop();

    if (receiveStarted) PltInterruptThread(&receiveThread);
    if (decoderStarted) {
        // Signal threads waiting on the LBQ
        LbqSignalQueueShutdown(&packetQueue);
        PltInterruptThread(&decoderThread);
    }

    if (receiveStarted) PltJoinThread(&receiveThread);
    if (decoderStarted) {
        PltJoinThread(&decoderThread);
    }
    receiveStarted = decoderStarted = false;
    if (pingThreadStarted) {
        PltInterruptThread(&udpPingThread);
        PltJoinThread(&udpPingThread);
        pingThreadStarted = false;
    }
    // A partially prepared generation can have queued packets but no decoder.
    LbqSignalQueueShutdown(&packetQueue);

    if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
        uint64_t now = PltGetMicroseconds();
        logMatAudioDiagnostics(&matReceiveDiagnostics, "receive_stopped", true, now);
        logMatAudioDiagnostics(&matDecodeDiagnostics, "decode_stopped", true, now);
    }

    MatLivePlaybackClose(matLive);matLive=NULL;
    if (audioPrepared) { AudioCallbacks.cleanup(); audioPrepared = false; }
}

int startAudioStream(void* audioContext, int arFlags) {
    int err;
    matAudioContext = audioContext;
    matAudioFlags = arFlags;
    matRecoveryEpoch = MatLiveProfile.epoch;
    err = prepareAudioGeneration(audioContext, arFlags);
    if (err != 0) return err;
    if (NegotiatedMatAudioRecovery) {
        atomic_store(&matRecoveryAccepting, true);
        err = PltCreateThread("MatRecover", MatRecoveryThreadProc, NULL, &matRecoveryThread);
        if (err != 0) {
            atomic_store(&matRecoveryAccepting, false);
            stopAudioGeneration();
            return err;
        }
        matRecoveryThreadStarted = true;
    }
    err = startAudioGeneration();
    if (err != 0) atomic_store(&matRecoveryStopping, true);
    atomic_store(&initialAudioStartComplete, true);
    if (err != 0) stopAudioStream();
    return err;
}

static int prepareAudioGeneration(void* audioContext, int arFlags) {
    int err;
    OPUS_MULTISTREAM_CONFIGURATION chosenConfig;

    LC_ASSERT(packetQueue.currentSize == 0);
    packetQueue.sizeBound = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) ? MAT_PACKET_QUEUE_BOUND : 30;
    if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) && !matPoolReady) {
        err = LbqInitializeLinkedBlockingQueue(&matFreePackets, packetQueue.sizeBound);
        if (err != 0) {
            return err;
        }
        matPoolReady = true;
    }
    lastSeq = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) ? 3 : 0;

    if (HighQualitySurroundEnabled) {
        LC_ASSERT(HighQualitySurroundSupported);
        LC_ASSERT(HighQualityOpusConfig.channelCount != 0);
        LC_ASSERT(HighQualityOpusConfig.streams != 0);
        chosenConfig = HighQualityOpusConfig;
    }
    else {
        LC_ASSERT(NormalQualityOpusConfig.channelCount != 0);
        LC_ASSERT(NormalQualityOpusConfig.streams != 0);
        chosenConfig = NormalQualityOpusConfig;
    }

    chosenConfig.samplesPerFrame = AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)?960:48 * AudioPacketDuration;

    err = AudioCallbacks.init(StreamConfig.audioConfiguration, &chosenConfig, audioContext, arFlags);
    if (err != 0) {
        return err;
    }
    audioPrepared = true;

    LI_MAT_PLAYBACK_CALLBACKS callbacks=matCallbacks;callbacks.end=matCallbacks.end?matEnd:NULL;
    if (callbacks.cycle) callbacks.cycle = timedMatCycle;
    if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
        atomic_store_explicit(&matInputEnded,false,memory_order_relaxed);
        atomic_store_explicit(&matLastAcceptedMs,PltGetMillis(),memory_order_relaxed);
    }
    if(NegotiatedAudioCodec==AUDIO_CODEC_MAT_LIVE) {
        matLive=MatLivePlaybackOpen(&MatLiveProfile,(const uint8_t*)StreamConfig.remoteInputAesKey,&callbacks);
        if(!matLive){AudioCallbacks.cleanup();audioPrepared=false;return -1;}
    }
    return 0;
}

static int startAudioGeneration(void) {
    int err;
    atomic_store_explicit(&matLastAcceptedMs,PltGetMillis(),memory_order_relaxed);
    AudioCallbacks.start();

    err = PltCreateThread("AudioRecv", AudioReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        return err;
    }
    receiveStarted = true;

    if ((AudioCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        err = PltCreateThread("AudioDec", AudioDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            return err;
        }
        decoderStarted = true;
    }

    if(AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec) && !pingThreadStarted){
        err=PltCreateThread("AudioPing",AudioPingThreadProc,NULL,&udpPingThread);
        if(err!=0)return err;
        pingThreadStarted=true;
    }
    return 0;
}

bool requestMatAudioRecovery(uint32_t reason) {
    if (!NegotiatedMatAudioRecovery || !atomic_load(&matRecoveryAccepting)) return false;
    if (NegotiatedMatAudioRecovery == 2) return requestMatRecoveryV2(reason);
    PltLockMutex(&matRecoveryMutex);
    bool accepted = !matRecoveryFailed && (!matRecoveryAttempted || matRecoveryBusy);
    if (accepted && !matRecoveryAttempted) {
        matRecoveryAttempted = matRecoveryBusy = true;
        memset(&matRecoveryProfile, 0, sizeof(matRecoveryProfile));
        matRecoveryProfile.verb = MAT_RECOVERY_REQUEST;
        matRecoveryProfile.generation = 1;
        matRecoveryProfile.previousEpoch = matRecoveryEpoch;
        matRecoveryProfile.reason = reason;
    }
    PltUnlockMutex(&matRecoveryMutex);
    if (accepted) PltSetEvent(&matRecoveryEvent);
    return accepted;
}

void queueMatAudioRecoveryControl(const MAT_AUDIO_RECOVERY* m) {
    if (!m || !NegotiatedMatAudioRecovery || !atomic_load(&matRecoveryAccepting)) return;
    if (MatRecoveryVersion(m) != NegotiatedMatAudioRecovery) return;
    if (NegotiatedMatAudioRecovery == 2) { queueMatRecoveryV2(m); return; }
    PltLockMutex(&matRecoveryMutex);
    if (matRecoveryFailed) { PltUnlockMutex(&matRecoveryMutex); return; }
    if (m->verb == MAT_RECOVERY_PREPARE) {
        if (matRecoveryHavePrepare) {
            if (!MatRecoveryMatchesProfile(&matRecoveryProfile, m)) matRecoveryFailed = true;
            else if (matRecoveryReady) matRecoveryResendReady = true;
        }
        else if (m->previousEpoch == matRecoveryEpoch && m->generation == 1) {
            matRecoveryAttempted = matRecoveryBusy = matRecoveryHavePrepare = true;
            matRecoveryProfile = *m;
        }
        else matRecoveryFailed = true;
    }
    else if (m->verb == MAT_RECOVERY_START) {
        if (!matRecoveryReady || !MatRecoveryMatchesProfile(&matRecoveryProfile, m) ||
            m->clientPort != matRecoveryProfile.clientPort) matRecoveryFailed = true;
        else matRecoveryHaveStart = true;
    }
    else if (m->verb == MAT_RECOVERY_FAIL &&
             (m->previousEpoch == matRecoveryEpoch || m->previousEpoch == matRecoveryProfile.previousEpoch))
        matRecoveryFailed = true;
    PltUnlockMutex(&matRecoveryMutex);
    PltSetEvent(&matRecoveryEvent);
}

static bool recoveryCancelled(void) {
    return atomic_load(&matRecoveryStopping) || ConnectionInterrupted;
}

static bool waitRecoveryMessage(bool start, uint64_t deadline) {
    while (!recoveryCancelled() && PltGetMillis() < deadline) {
        PltLockMutex(&matRecoveryMutex);
        bool failed = matRecoveryFailed;
        bool ready = start ? matRecoveryHaveStart : matRecoveryHavePrepare;
        bool resend = start && matRecoveryResendReady;
        MAT_AUDIO_RECOVERY message = matRecoveryProfile;
        if (resend) matRecoveryResendReady = false;
        PltUnlockMutex(&matRecoveryMutex);
        if (failed) return false;
        if (resend) {
            message.verb = MAT_RECOVERY_READY;
            if (sendMatAudioRecoveryControl(&message) != 0) return false;
        }
        if (ready) return true;
        PltSleepMsInterruptible(&matRecoveryThread, 10);
    }
    return false;
}

static uint16_t audioSocketPort(SOCKET socket) {
    struct sockaddr_storage address;
    SOCKADDR_LEN length = sizeof(address);
    if (getsockname(socket, (struct sockaddr*)&address, &length) < 0) return 0;
    return ntohs(address.ss_family == AF_INET6 ? ((struct sockaddr_in6*)&address)->sin6_port :
                 ((struct sockaddr_in*)&address)->sin_port);
}

static bool requestMatRecoveryV2(uint32_t reason) {
    PltLockMutex(&matRecoveryMutex);
    if (reason == MAT_RECOVERY_BLOCKED) atomic_store(&matRecoveryBlocked, true);
    if (matV2Phase == MAT_V2_RUNNING || matV2Phase == MAT_V2_CONFIRMING || matV2Phase == MAT_V2_PLAYING) {
        matV2Phase = matV2Profile.generation ? MAT_V2_FAILED : MAT_V2_REQUESTED;
        matV2Report = true;
    }
    PltUnlockMutex(&matRecoveryMutex);
    PltSetEvent(&matRecoveryEvent);
    return true; // Audio-only failures must not terminate a v2 video session.
}

static void queueMatRecoveryV2(const MAT_AUDIO_RECOVERY* m) {
    if (MatRecoveryVersion(m) != 2) return;
    PltLockMutex(&matRecoveryMutex);
    if (m->verb == MAT_RECOVERY_PREPARE && m->generation > matV2Profile.generation) {
        matV2Profile = *m;
        matV2Phase = atomic_load(&matRecoveryBlocked) ? MAT_V2_FAILED : MAT_V2_PENDING;
        matV2Report = atomic_load(&matRecoveryBlocked);
        matV2Start = matV2ResendReady = matV2PlayingSent = matV2StableSent = false;
        matV2Deadline = PltGetMillis() + MAT_RECOVERY_TIMEOUT_MS;
    }
    else if (MatRecoveryMatchesProfile(&matV2Profile, m)) {
        if (m->verb == MAT_RECOVERY_FAIL &&
            (!m->clientPort || !matV2Profile.clientPort || m->clientPort == matV2Profile.clientPort)) {
            if (m->reason == MAT_RECOVERY_BLOCKED) atomic_store(&matRecoveryBlocked, true);
            matV2Phase = MAT_V2_FAILED;
            // Do not echo FAIL indefinitely. A cleanup failure can still
            // upgrade this result to BLOCKED in the worker.
        }
        else if (matV2Phase != MAT_V2_FAILED && !atomic_load(&matRecoveryBlocked)) {
            if (m->verb == MAT_RECOVERY_PREPARE && matV2Profile.clientPort &&
                (matV2Phase == MAT_V2_READY || matV2Phase == MAT_V2_CONFIRMING || matV2Phase == MAT_V2_PLAYING))
                matV2ResendReady = true;
            else if (m->verb == MAT_RECOVERY_START && m->clientPort == matV2Profile.clientPort &&
                     matV2Phase == MAT_V2_READY && PltGetMillis() < matV2Deadline)
                matV2Start = true;
        }
    }
    // Old, conflicting and unsolicited messages cannot mutate the attempt.
    PltUnlockMutex(&matRecoveryMutex);
    PltSetEvent(&matRecoveryEvent);
}

static bool matV2Current(uint32_t generation) {
    PltLockMutex(&matRecoveryMutex);
    bool current = matV2Profile.generation == generation && matV2Phase != MAT_V2_FAILED;
    PltUnlockMutex(&matRecoveryMutex);
    return current && !atomic_load(&matRecoveryBlocked) && !recoveryCancelled();
}

static void matV2Fail(uint32_t generation) {
    PltLockMutex(&matRecoveryMutex);
    if (generation == matV2Profile.generation) {
        matV2Phase = MAT_V2_FAILED;
        matV2Report = true;
    }
    PltUnlockMutex(&matRecoveryMutex);
}

static bool matV2Send(MAT_AUDIO_RECOVERY* m, unsigned verb) {
    if (recoveryCancelled()) return false;
    m->verb = (uint8_t)verb;
    if (sendMatAudioRecoveryControl(m) == 0) return true;
    // This is loss of the control transport, not an audio recovery failure.
    if (!recoveryCancelled()) ListenerCallbacks.connectionTerminated(-1);
    return false;
}

static void MatRecoveryV2ThreadProc(void) {
    bool stopped = false;
    while (!recoveryCancelled()) {
        PltLockMutex(&matRecoveryMutex);
        MAT_V2_PHASE phase = matV2Phase;
        MAT_AUDIO_RECOVERY message = matV2Profile;
        uint64_t deadline = matV2Deadline;
        bool report = matV2Report, resend = matV2ResendReady;
        matV2Report = matV2ResendReady = false;
        PltUnlockMutex(&matRecoveryMutex);

        if (phase == MAT_V2_FAILED || phase == MAT_V2_REQUESTED) {
            if (!stopped) {
                bool wasBlocked = atomic_load(&matRecoveryBlocked);
                stopAudioGeneration();
                stopped = true;
                if (!wasBlocked && atomic_load(&matRecoveryBlocked)) report = true;
            }
            if (report) {
                message.reason = atomic_load(&matRecoveryBlocked) ? MAT_RECOVERY_BLOCKED : MAT_RECOVERY_RETRYABLE;
                if (!message.generation) {
                    memset(&message, 0, sizeof(message));
                    message.version = 2;
                    message.previousEpoch = matRecoveryEpoch;
                    message.generation = matV2InstalledGeneration;
                    message.reason = atomic_load(&matRecoveryBlocked) ? MAT_RECOVERY_BLOCKED : MAT_RECOVERY_RETRYABLE;
                    if (!matV2Send(&message, MAT_RECOVERY_REQUEST)) return;
                }
                else if (!matV2Send(&message, MAT_RECOVERY_FAIL)) return;
                Limelog("MAT audio attempt %u failed: %s; video/control retained\n", message.generation,
                    message.reason == MAT_RECOVERY_BLOCKED ? "automatic recovery blocked, reconnect required" : "waiting for host retry");
            }
        }
        else if (phase == MAT_V2_PENDING) {
            SOCKET nextSocket = INVALID_SOCKET;
            if (!stopped) { stopAudioGeneration(); stopped = true; }
            if (!matV2Current(message.generation)) { matV2Fail(message.generation); continue; }
            // Keep the immediately previous port bound until the new bind.
            // SSRC/epoch filtering also isolates any older port reuse.
            nextSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 1024 * 1024, SOCK_QOS_TYPE_AUDIO);
            if (nextSocket == INVALID_SOCKET) { matV2Fail(message.generation); continue; }
            message.clientPort = audioSocketPort(nextSocket);
            if (!message.clientPort || message.clientPort == audioSocketPort(rtpSocket)) {
                closeSocket(nextSocket); matV2Fail(message.generation); continue;
            }
            destroyAudioGeneration();
            initializeAudioGeneration();
            stopped = false;
            rtpSocket = nextSocket;
            LiRecordStreamSocket(rtpSocket, STREAM_SOCKET_SLOT_AUDIO);
            MatLiveProfile.epoch = message.newEpoch;
            MatLiveProfile.domain = message.domain;
            NegotiatedMatAudioSsrc = message.ssrc;
            AudioPortNumber = message.serverPort;
            memcpy(AudioPingPayload.payload, message.token, sizeof(message.token));
            AudioPingPayload.sequenceNumber = 0;
            matRecoveryEpoch = message.newEpoch;
            matV2InstalledGeneration = message.generation;
            if (!initializeAudioEndpoint() || !matV2Current(message.generation) ||
                prepareAudioGeneration(matAudioContext, matAudioFlags) != 0 ||
                atomic_load(&matRecoveryBlocked) || PltGetMillis() >= deadline) {
                matV2Fail(message.generation); continue;
            }
            PltLockMutex(&matRecoveryMutex);
            bool current = matV2Profile.generation == message.generation && matV2Phase == MAT_V2_PENDING;
            if (current) {
                matV2Profile.clientPort = message.clientPort;
                matV2Phase = MAT_V2_READY;
            }
            PltUnlockMutex(&matRecoveryMutex);
            if (!current || recoveryCancelled()) continue;
            if (!matV2Send(&message, MAT_RECOVERY_READY)) return;
        }
        else if (phase == MAT_V2_READY) {
            if (PltGetMillis() >= deadline) { matV2Fail(message.generation); continue; }
            if (resend && !matV2Send(&message, MAT_RECOVERY_READY)) return;
            PltLockMutex(&matRecoveryMutex);
            bool start = matV2Profile.generation == message.generation && matV2Phase == MAT_V2_READY && matV2Start;
            if (start) matV2Phase = MAT_V2_CONFIRMING;
            PltUnlockMutex(&matRecoveryMutex);
            if (start) {
                if (!matV2Current(message.generation) || startAudioGeneration() != 0 || atomic_load(&matRecoveryBlocked))
                    matV2Fail(message.generation);
                else Limelog("MAT audio started: generation=%u epoch=%llu ssrc=%u; awaiting playback progress\n",
                    message.generation, (unsigned long long)message.newEpoch, message.ssrc);
            }
            else if (PltGetMillis() >= deadline) matV2Fail(message.generation);
        }
        else if (phase == MAT_V2_CONFIRMING || phase == MAT_V2_PLAYING) {
            if (resend && !matV2Send(&message, MAT_RECOVERY_READY)) return;
            int progress = matCallbacks.progress ? matCallbacks.progress(matCallbacks.context, message.newEpoch, message.domain) : -1;
            if (atomic_load(&matRecoveryBlocked) || progress < 0) { matV2Fail(message.generation); continue; }
            PltLockMutex(&matRecoveryMutex);
            bool current = matV2Profile.generation == message.generation &&
                (matV2Phase == MAT_V2_CONFIRMING || matV2Phase == MAT_V2_PLAYING);
            bool playing = current && (progress & 1) && !matV2PlayingSent;
            bool stable = current && (progress & 3) == 3 && !matV2StableSent;
            if (playing) { matV2PlayingSent = true; matV2Phase = MAT_V2_PLAYING; }
            if (stable) matV2StableSent = true;
            bool timedOut = current && matV2Phase == MAT_V2_CONFIRMING && PltGetMillis() >= deadline;
            PltUnlockMutex(&matRecoveryMutex);
            if (playing && !matV2Send(&message, MAT_RECOVERY_PLAYING)) return;
            if (stable && !matV2Send(&message, MAT_RECOVERY_STABLE)) return;
            if (timedOut) matV2Fail(message.generation);
        }
        PltSleepMsInterruptible(&matRecoveryThread, phase == MAT_V2_PENDING || phase == MAT_V2_READY ? 10 : 50);
    }
}

static void MatRecoveryThreadProc(void* context) {
    (void)context;
    // The initial start owns the same generation globals. Its workers may
    // report a fault immediately, before all creation flags are published.
    while (!atomic_load(&initialAudioStartComplete) && !recoveryCancelled())
        PltSleepMsInterruptible(&matRecoveryThread, 1);
    if (recoveryCancelled()) return;
    if (NegotiatedMatAudioRecovery == 2) { MatRecoveryV2ThreadProc(); return; }
    for (;;) {
        PltWaitForEvent(&matRecoveryEvent);
        PltClearEvent(&matRecoveryEvent);
        if (recoveryCancelled()) return;
        PltLockMutex(&matRecoveryMutex);
        bool busy = matRecoveryBusy, failed = matRecoveryFailed;
        bool sendRequest = busy && !matRecoveryHavePrepare;
        bool resendReady = matRecoveryResendReady;
        matRecoveryResendReady = false;
        MAT_AUDIO_RECOVERY message = matRecoveryProfile;
        PltUnlockMutex(&matRecoveryMutex);
        if (resendReady && !failed) {
            message.verb = MAT_RECOVERY_READY;
            if (sendMatAudioRecoveryControl(&message) != 0) failed = true;
        }
        if (!busy && !failed) continue;
        const uint64_t deadline = PltGetMillis() + 10000;
        SOCKET nextSocket = INVALID_SOCKET;
        if (failed || (sendRequest && sendMatAudioRecoveryControl(&message) != 0) ||
            !waitRecoveryMessage(false, deadline)) goto failure;
        PltLockMutex(&matRecoveryMutex);
        message = matRecoveryProfile;
        PltUnlockMutex(&matRecoveryMutex);

        // Bind while the old socket still owns its port. Neither old RTP nor
        // old parity can be consumed by the new generation's queue.
        nextSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 1024 * 1024, SOCK_QOS_TYPE_AUDIO);
        if (nextSocket == INVALID_SOCKET) goto failure;
        message.clientPort = audioSocketPort(nextSocket);
        if (!message.clientPort || message.clientPort == audioSocketPort(rtpSocket)) goto failure;
        stopAudioGeneration();
        destroyAudioGeneration();
        initializeAudioGeneration();
        rtpSocket = nextSocket;
        nextSocket = INVALID_SOCKET;
        if (recoveryCancelled()) goto failure;
        LiRecordStreamSocket(rtpSocket, STREAM_SOCKET_SLOT_AUDIO);
        MatLiveProfile.epoch = message.newEpoch;
        MatLiveProfile.domain = message.domain;
        AudioPortNumber = message.serverPort;
        memcpy(AudioPingPayload.payload, message.token, sizeof(message.token));
        AudioPingPayload.sequenceNumber = 0;
        if (!initializeAudioEndpoint() || prepareAudioGeneration(matAudioContext, matAudioFlags) != 0 || recoveryCancelled()) goto failure;
        PltLockMutex(&matRecoveryMutex);
        matRecoveryProfile.clientPort = message.clientPort;
        matRecoveryReady = true;
        PltUnlockMutex(&matRecoveryMutex);
        message.verb = MAT_RECOVERY_READY;
        if (sendMatAudioRecoveryControl(&message) != 0 || !waitRecoveryMessage(true, deadline)) goto failure;
        PltLockMutex(&matRecoveryMutex);
        failed = matRecoveryFailed;
        if (!failed) {
            matRecoveryEpoch = message.newEpoch;
            matRecoveryBusy = false;
        }
        PltUnlockMutex(&matRecoveryMutex);
        if (failed || recoveryCancelled() || startAudioGeneration() != 0) goto failure;
        Limelog("MAT audio recovery resumed: generation=1 epoch=%llu localPort=%u\n",
                (unsigned long long)message.newEpoch, message.clientPort);
        continue;

failure:
        if (nextSocket != INVALID_SOCKET) closeSocket(nextSocket);
        PltLockMutex(&matRecoveryMutex);
        matRecoveryFailed = true;
        matRecoveryBusy = false;
        message = matRecoveryProfile;
        PltUnlockMutex(&matRecoveryMutex);
        atomic_store(&matRecoveryAccepting, false);
        if (!recoveryCancelled()) {
            message.verb = MAT_RECOVERY_FAIL;
            message.generation = 1;
            message.reason = 1;
            sendMatAudioRecoveryControl(&message);
        }
        stopAudioGeneration();
        if (!recoveryCancelled()) {
            Limelog("MAT audio recovery failed; reconnect required\n");
            ListenerCallbacks.connectionTerminated(-1);
        }
        return;
    }
}

int LiGetPendingAudioFrames(void) {
    return LbqGetItemCount(&packetQueue);
}

int LiGetPendingAudioDuration(void) {
    return LiGetPendingAudioFrames() * AudioPacketDuration;
}

const RTP_AUDIO_STATS* LiGetRTPAudioStats(void) {
    return &rtpAudioQueue.stats;
}
