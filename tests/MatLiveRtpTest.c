/* Real RTP/FEC queue and AudioStream path. Synthetic carrier, no Dolby decode claim. */
#include "../src/AudioStream.c"
#include "MatLiveCrypto.h"
#include <rs.h>
#include <stdio.h>

static int failures, cycles, ends, terminated, concealed, opusPackets, workerStarts;
#ifndef LC_AUDIO_FEC_VALIDATION
static uint64_t queueClockUs = 1000000;
static unsigned rendererDelayUs;
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static MAT_LIVE_CIPHER cipher;
static reed_solomon* senderRs;
static uint8_t key[16];
static LI_MAT_LIVE_PROFILE profile = {123, 7};
typedef struct {
    uint64_t base;
    uint8_t shards[RTPA_TOTAL_SHARDS][MAT_LIVE_PACKET];
} TEST_BLOCK;

static void makeCarrier(uint8_t* bytes, unsigned index) {
    memcpy(bytes, "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
    for (unsigned i = 8; i < MAT_WIRE_CYCLE; ++i) bytes[i] = (uint8_t)(i + index);
}
static int cycle(void* context, const uint8_t* bytes, int n,
                 uint64_t epoch, uint64_t domain, uint64_t index, uint64_t pts) {
    uint8_t expected[MAT_WIRE_CYCLE];
    (void)context;
#ifndef LC_AUDIO_FEC_VALIDATION
    queueClockUs += rendererDelayUs;
#endif
    makeCarrier(expected, (unsigned)cycles);
    CHECK(n == MAT_WIRE_CYCLE && epoch == 123 && domain == 7);
    CHECK(index == (unsigned)cycles && pts == index * 960);
    CHECK(n == MAT_WIRE_CYCLE && memcmp(bytes, expected, sizeof(expected)) == 0);
    ++cycles;
    return 1;
}
static int end(void* context, uint64_t n) { (void)context; CHECK(n == (unsigned)cycles); ++ends; return 1; }
static void terminate(int error) { (void)error; ++terminated; }
static void opus(char* data, int size) { if (!data || !size) ++concealed; else ++opusPackets; }
static void workerStart(void* context, int role) {
    CHECK(context == &workerStarts);
    CHECK(role == LI_MAT_THREAD_RECEIVE || role == LI_MAT_THREAD_DECODE);
    ++workerStarts;
}

static void deliver(PQUEUED_AUDIO_PACKET packet) {
    int status = RtpaAddPacket(&rtpAudioQueue, (PRTP_PACKET)packet->data, (uint16_t)packet->header.size);
    if (RTPQ_HANDLE_NOW(status)) decodeInputData(packet);
    else {
        uint16_t length;
        PQUEUED_AUDIO_PACKET queued;
        while ((queued = (PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(
                    &rtpAudioQueue, sizeof(QUEUED_AUDIO_PACKET_HEADER), &length)) != NULL) {
            queued->header.size = length;
            decodeInputData(queued);
            free(queued);
        }
    }
    free(packet);
}
static void packet(uint64_t sequence, const uint8_t* payload, size_t size) {
    PQUEUED_AUDIO_PACKET p = calloc(1, sizeof(*p));
    CHECK(p != NULL); if (!p) return;
    p->header.size = sizeof(RTP_PACKET) + (int)size;
    PRTP_PACKET r = (PRTP_PACKET)p->data;
    r->header = 0x80; r->packetType = 97;
    r->sequenceNumber = (uint16_t)sequence; r->timestamp = (uint32_t)sequence;
    memcpy(r + 1, payload, size);
    deliver(p);
}
static void parity(const TEST_BLOCK* block, unsigned index) {
    PQUEUED_AUDIO_PACKET p = calloc(1, sizeof(*p));
    CHECK(p != NULL); if (!p) return;
    p->header.size = sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER) + MAT_LIVE_PACKET;
    PRTP_PACKET r = (PRTP_PACKET)p->data;
    r->header = 0x80; r->packetType = 127;
    PAUDIO_FEC_HEADER fec = (PAUDIO_FEC_HEADER)(r + 1);
    fec->fecShardIndex = (uint8_t)(index - RTPA_DATA_SHARDS); fec->payloadType = 97;
    fec->baseSequenceNumber = BE16((uint16_t)block->base);
    fec->baseTimestamp = BE32((uint32_t)block->base); fec->ssrc = 0;
    memcpy(fec + 1, block->shards[index], MAT_LIVE_PACKET);
    deliver(p);
}
static void encodeParity(TEST_BLOCK* block) {
    uint8_t* shards[RTPA_TOTAL_SHARDS];
    for (unsigned i = 0; i < RTPA_TOTAL_SHARDS; ++i) shards[i] = block->shards[i];
    CHECK(reed_solomon_encode(senderRs, shards, RTPA_TOTAL_SHARDS, MAT_LIVE_PACKET) == 0);
}
static void emitBlock(const TEST_BLOCK* block, unsigned omitted, bool reordered) {
    const unsigned order[RTPA_TOTAL_SHARDS] = {5, 3, 1, 4, 0, 2};
    for (unsigned n = 0; n < RTPA_TOTAL_SHARDS; ++n) {
        unsigned i = reordered ? order[n] : n;
        if (omitted & (1u << i)) continue;
        if (i < RTPA_DATA_SHARDS) packet(block->base + i, block->shards[i], MAT_LIVE_PACKET);
        else parity(block, i);
    }
}
static void dataBlock(TEST_BLOCK* block, const uint8_t* carrier, unsigned cycleIndex, unsigned fragment) {
    uint8_t plain[MAT_WIRE_PACKET];
    block->base = 4 + (uint64_t)cycleIndex * MAT_WIRE_FRAGMENTS + fragment;
    for (unsigned i = 0; i < RTPA_DATA_SHARDS; ++i) {
        mat_wire_make(plain, MAT_WIRE_DATA, 123, 7, cycleIndex, fragment + i,
                      mat_hash(carrier, MAT_WIRE_CYCLE), carrier);
        CHECK(MatLiveSeal(&cipher, block->base + i, plain, block->shards[i]));
    }
    encodeParity(block);
}
static void endBlock(TEST_BLOCK* block, unsigned count, uint32_t hash) {
    uint8_t plain[MAT_WIRE_PACKET];
    block->base = 4 + (uint64_t)count * MAT_WIRE_FRAGMENTS;
    mat_wire_make(plain, MAT_WIRE_END, 123, 7, count, 0, hash, NULL);
    for (unsigned i = 0; i < RTPA_DATA_SHARDS; ++i)
        CHECK(MatLiveSeal(&cipher, block->base + i, plain, block->shards[i]));
    encodeParity(block);
}
static void beginSession(void) {
    uint8_t plain[MAT_WIRE_PACKET], wire[MAT_LIVE_PACKET];
    LI_MAT_PLAYBACK_CALLBACKS cb = {0}; cb.cycle = cycle; cb.end = end;
    LiSetMatPlaybackCallbacksEx(&cb, sizeof(cb)); snapshotMatCallbacks();
    cb.cycle = timedMatCycle;
    cycles = ends = terminated = concealed = opusPackets = 0;
    atomic_store(&matFailureReported, false); atomic_store(&matSuppressedFailures, 0);
    atomic_store(&matCompletedCycles, 0); atomic_store(&matPendingPackets, 0);
    matPlaybackCompleteReported = false;
    memset(&matReceiveDiagnostics, 0, sizeof(matReceiveDiagnostics));
    memset(&matDecodeDiagnostics, 0, sizeof(matDecodeDiagnostics));
    matReceiveDiagnostics.nextLogUs = matDecodeDiagnostics.nextLogUs = PltGetMicroseconds() + 5000000;
    NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE; AudioEncryptionEnabled = true;
    MatLiveProfile = profile;
    AudioPacketDuration = 1; lastSeq = 3;
    RtpaInitializeQueue(&rtpAudioQueue);
    matLive = MatLivePlaybackOpen(&profile, key, &cb); CHECK(matLive != NULL);
    mat_wire_make(plain, MAT_WIRE_SYNC, 123, 7, 0, 0, 0, NULL);
    for (unsigned i = 0; i < 4; ++i) { CHECK(MatLiveSeal(&cipher, i, plain, wire)); packet(i, wire, sizeof(wire)); }
}
static void closeSession(void) {
    MatLivePlaybackClose(matLive); matLive = NULL; RtpaCleanupQueue(&rtpAudioQueue);
}
static void expireHead(void) {
    CHECK(rtpAudioQueue.blockHead != NULL);
    if (rtpAudioQueue.blockHead) rtpAudioQueue.blockHead->queueTimeUs = PltGetMicroseconds() - 20000;
}

static void fecRoundTrip(unsigned omitted, bool reordered, bool lateFinalBlock) {
    uint8_t carrier[MAT_WIRE_CYCLE]; TEST_BLOCK block, last, eos;
    beginSession(); makeCarrier(carrier, 0);
    for (unsigned fragment = 0; fragment < MAT_WIRE_FRAGMENTS; fragment += 4) {
        dataBlock(&block, carrier, 0, fragment);
        if (lateFinalBlock && fragment == 56) last = block;
        else emitBlock(&block, omitted, reordered);
    }
    endBlock(&eos, 1, mat_hash_update(2166136261u, carrier, sizeof(carrier)));
    if (lateFinalBlock) {
        emitBlock(&eos, 0, false); CHECK(!ends && !terminated);
        emitBlock(&last, omitted, reordered);
    } else emitBlock(&eos, 0, false);
    CHECK(cycles == 1 && ends == 1 && terminated == 0);
    closeSession();
}
#ifndef LC_AUDIO_FEC_VALIDATION
uint64_t MatRtpTestMicroseconds(void) {
    return queueClockUs;
}

static void delayedMatBlock(bool wholeBlock, unsigned delayUs, bool expectRecovery) {
    uint8_t carrier[MAT_WIRE_CYCLE];
    TEST_BLOCK last, eos;
    beginSession();
    makeCarrier(carrier, 0);
    for (unsigned fragment = 0; fragment < MAT_WIRE_FRAGMENTS; fragment += RTPA_DATA_SHARDS) {
        dataBlock(&last, carrier, 0, fragment);
        if (fragment + RTPA_DATA_SHARDS < MAT_WIRE_FRAGMENTS) {
            emitBlock(&last, 0, false);
        }
    }
    if (!wholeBlock) {
        for (unsigned i = 0; i < RTPA_DATA_SHARDS - 1; ++i) {
            packet(last.base + i, last.shards[i], MAT_LIVE_PACKET);
        }
    }
    endBlock(&eos, 1, mat_hash_update(2166136261u, carrier, sizeof(carrier)));
    packet(eos.base, eos.shards[0], MAT_LIVE_PACKET);

    // Trigger the queue's expiry check before delivering the missing block or parity.
    queueClockUs += delayUs;
    packet(eos.base + 1, eos.shards[1], MAT_LIVE_PACKET);
    if (wholeBlock) {
        emitBlock(&last, 0, false);
    }
    else {
        parity(&last, RTPA_DATA_SHARDS);
    }
    for (unsigned i = 2; i < RTPA_DATA_SHARDS; ++i) {
        packet(eos.base + i, eos.shards[i], MAT_LIVE_PACKET);
    }
    if (expectRecovery) {
        CHECK(cycles == 1 && ends == 1 && terminated == 0);
    }
    else {
        CHECK(cycles == 0 && ends == 0 && terminated == 1);
    }
    closeSession();
}

static void arrivalWindows(void) {
    MAT_AUDIO_DIAGNOSTICS stats = {0};
    RTP_PACKET packet = {0};
    uint64_t base = queueClockUs;
    stats.nextLogUs = base + 5000000;
    recordMatArrival(&stats, &packet, base, 10000);
    recordMatArrival(&stats, &packet, base + 4999999, 0);
    CHECK(stats.gapSamples == 1 && stats.missingKernelIntervals == 1);
    queueClockUs = stats.nextLogUs;
    logMatAudioDiagnostics(&stats, "receive", false, queueClockUs);
    recordMatArrival(&stats, &packet, queueClockUs, 20000);
    CHECK(stats.gapSamples == 2 && stats.missingKernelIntervals == 1 && stats.maxReceiveGapUs == 1);
    recordMatArrival(&stats, &packet, queueClockUs - 3, 20100);  // app clock rolls back independently
    CHECK(stats.gapSamples == 2 && stats.kernelIntervals == 1 && stats.maxKernelGapUs == 100);
    recordMatArrival(&stats, &packet, queueClockUs + 1, 14000);  // kernel realtime clock rolls back
    CHECK(stats.gapSamples == 3 && stats.kernelIntervals == 1 && stats.missingKernelIntervals == 2);
}

static void olderBlockBeforeExpiry(void) {
    uint8_t carrier[MAT_WIRE_CYCLE];
    TEST_BLOCK first, middle, later;
    beginSession();
    makeCarrier(carrier, 0);
    dataBlock(&first, carrier, 0, 0);
    dataBlock(&middle, carrier, 0, 4);
    dataBlock(&later, carrier, 0, 8);
    packet(later.base, later.shards[0], MAT_LIVE_PACKET);
    queueClockUs += 13999;
    packet(middle.base, middle.shards[0], MAT_LIVE_PACKET);
    CHECK(!terminated && rtpAudioQueue.stats.packetCountFecFailed == 0);
    queueClockUs += 2;
    packet(middle.base + 1, middle.shards[1], MAT_LIVE_PACKET);
    CHECK(terminated == 1 && rtpAudioQueue.stats.packetCountFecFailed == 1);
    packet(first.base, first.shards[0], MAT_LIVE_PACKET);
    CHECK(rtpAudioQueue.stats.packetCountFecFailed == 1);
    closeSession();
}
#endif

static void permanentLoss(bool wholeBlock) {
    uint8_t carrier[MAT_WIRE_CYCLE]; TEST_BLOCK first, next;
    beginSession(); makeCarrier(carrier, 0);
    dataBlock(&first, carrier, 0, 0); dataBlock(&next, carrier, 0, 4);
    if (!wholeBlock) emitBlock(&first, 7, false);
    packet(next.base, next.shards[0], MAT_LIVE_PACKET); CHECK(!terminated);
    expireHead(); packet(next.base + 1, next.shards[1], MAT_LIVE_PACKET);
    CHECK(terminated == 1 && !cycles && !ends);
    emitBlock(&first, 0, false); emitBlock(&next, 0, false);
    CHECK(terminated == 1 && !cycles && !ends);
    CHECK(atomic_load(&matSuppressedFailures) != 0);
    closeSession();
}
static void opusRecovery(void) {
    uint8_t carrier[MAT_WIRE_CYCLE]; TEST_BLOCK first, next;
    beginSession(); NegotiatedAudioCodec = AUDIO_CODEC_OPUS; AudioEncryptionEnabled = false;
    AudioCallbacks.decodeAndPlaySample = opus;
#ifdef LC_DEBUG
    opusHeaderByte = INVALID_OPUS_HEADER;
#endif
    makeCarrier(carrier, 0); dataBlock(&first, carrier, 0, 0); dataBlock(&next, carrier, 0, 4);
    emitBlock(&first, 3, true); CHECK(opusPackets == 4 && !concealed && !terminated);
    emitBlock(&next, 7, false);
    expireHead();
    dataBlock(&first, carrier, 0, 8); emitBlock(&first, 0, false);
    CHECK(concealed == 3 && opusPackets >= 5 && !terminated);
    closeSession();
}
static void workerCallback(void) {
    LI_MAT_PLAYBACK_CALLBACKS cb = {0}; cb.threadStart = workerStart; cb.context = &workerStarts;
    LiSetMatPlaybackCallbacksEx(&cb, sizeof(cb)); snapshotMatCallbacks(); NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE;
    startMatAudioWorker(LI_MAT_THREAD_RECEIVE); startMatAudioWorker(LI_MAT_THREAD_DECODE);
    CHECK(workerStarts == 2); NegotiatedAudioCodec = AUDIO_CODEC_OPUS;
    startMatAudioWorker(LI_MAT_THREAD_RECEIVE); CHECK(workerStarts == 2);
    LiSetMatPlaybackCallbacksEx(NULL, 0); snapshotMatCallbacks(); NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE;
    startMatAudioWorker(LI_MAT_THREAD_DECODE); CHECK(workerStarts == 2);
}
static void queueOverflow(void) {
    beginSession();
    CHECK(LbqInitializeLinkedBlockingQueue(&packetQueue, MAT_PACKET_QUEUE_BOUND) == 0);
    for (int i = 0; i < MAT_PACKET_QUEUE_BOUND; ++i) {
        PQUEUED_AUDIO_PACKET packet = calloc(1, sizeof(*packet));
        CHECK(packet && queuePacketToLbq(&packet) && packet == NULL);
        free(packet);
    }
    PQUEUED_AUDIO_PACKET rejected = calloc(1, sizeof(*rejected));
    CHECK(rejected);
    if (rejected) {
        CHECK(atomic_load(&matPendingPackets) == MAT_PACKET_QUEUE_BOUND && matReceiveDiagnostics.peakQueue == MAT_PACKET_QUEUE_BOUND);
        CHECK(!queuePacketToLbq(&rejected) && rejected != NULL);
        CHECK(terminated == 1 && atomic_load(&matPendingPackets) == MAT_PACKET_QUEUE_BOUND);
        CHECK(!queuePacketToLbq(&rejected) && terminated == 1);
    }
    free(rejected);
    LbqSignalQueueShutdown(&packetQueue);
    freePacketList(LbqDestroyLinkedBlockingQueue(&packetQueue));
    closeSession();
}
static void longReorderedStream(void) {
    uint8_t plain[MAT_WIRE_PACKET], wire[MAT_LIVE_PACKET], carrier[MAT_WIRE_CYCLE];
    uint8_t delayed[MAT_LIVE_PACKET], delayedBlocks[8][MAT_LIVE_PACKET];
    uint32_t hash = 2166136261u; uint64_t next = 4;
    beginSession();
    for (unsigned i = 0; i < 1300; ++i) {
        makeCarrier(carrier, i); uint32_t h = mat_hash(carrier, sizeof(carrier));
        for (unsigned j = 0; j < 60; ++j) {
            mat_wire_make(plain, MAT_WIRE_DATA, 123, 7, i, j, h, carrier);
            uint64_t sequence = next++; CHECK(MatLiveSeal(&cipher, sequence, plain, wire));
            if (i == 0 && j == 3) { memcpy(delayed, wire, sizeof(delayed)); continue; }
            if ((i == 1 || i == 1092) && j >= 8 && j < 16) {
                memcpy(delayedBlocks[j - 8], wire, sizeof(wire)); continue;
            }
            packet(sequence, wire, sizeof(wire));
            if (i == 0 && j == 4) packet(sequence - 1, delayed, sizeof(delayed));
            if ((i == 1 || i == 1092) && j == 16) {
                for (unsigned k = 0; k < 8; ++k) packet(sequence - 8 + k, delayedBlocks[k], sizeof(delayedBlocks[k]));
                CHECK(!terminated); if (terminated) goto cleanup;
            }
        }
        hash = mat_hash_update(hash, carrier, sizeof(carrier));
    }
    mat_wire_make(plain, MAT_WIRE_END, 123, 7, 1300, 0, hash, NULL);
    for (unsigned i = 0; i < 4; ++i) { CHECK(MatLiveSeal(&cipher, next, plain, wire)); packet(next++, wire, sizeof(wire)); }
    CHECK(cycles == 1300 && ends == 1 && !terminated && lastSeq == (uint16_t)(next - 1));
cleanup:
    closeSession();
}
int main(void) {
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431; AppVersionQuad[3] = -1;
    ListenerCallbacks.connectionTerminated = terminate;
    CHECK(MatLiveCipherInit(&cipher, key, sizeof(key), 123, 7));
    reed_solomon_init(); senderRs = reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS); CHECK(senderRs != NULL);
    if (!senderRs) return 1;
    const uint8_t matrix[] = {0x77,0x40,0x38,0x0e,0xc7,0xa7,0x0d,0x6c};
    memcpy(senderRs->p, matrix, sizeof(matrix));
    workerCallback(); fecRoundTrip(0, false, false); fecRoundTrip(1, true, false);
#ifndef LC_AUDIO_FEC_VALIDATION
    arrivalWindows();
    olderBlockBeforeExpiry();
    fecRoundTrip(3, false, false); fecRoundTrip(3, true, false);
    fecRoundTrip(1u | (1u << 4), true, false); fecRoundTrip(3, true, true);
    for (int whole = 0; whole <= 1; ++whole) {
        delayedMatBlock(whole, 12000, true);
        delayedMatBlock(whole, 14000, true);
        delayedMatBlock(whole, 14001, false);
    }
    permanentLoss(false); permanentLoss(true); queueOverflow(); opusRecovery(); longReorderedStream();
    // Reproduce the duration seen in the SHIELD trace at a known boundary.
    rendererDelayUs = 278848;
    fecRoundTrip(0, false, false);
    CHECK(matDecodeDiagnostics.slowSubmitUs == rendererDelayUs && matDecodeDiagnostics.slowProcessUs == 0);
    printf("Renderer stall control: submitUs=%llu processUs=%llu queueCapacity=%u\n",
           (unsigned long long)matDecodeDiagnostics.slowSubmitUs, (unsigned long long)matDecodeDiagnostics.slowProcessUs,
           MAT_PACKET_QUEUE_BOUND);
    rendererDelayUs = 0;
#endif
    reed_solomon_release(senderRs); cleanupPlatform();
    if (failures) return 1;
#ifdef LC_AUDIO_FEC_VALIDATION
    puts("PASS explicit offline FEC self-test: synthetic shard validation and exact carrier/EOS");
#else
    puts("PASS actual RTP/AudioStream: production 4+2 matrix, real loss/reordering, bounded failure, exact carrier/EOS, Opus concealment, worker callback");
#endif
    return 0;
}
