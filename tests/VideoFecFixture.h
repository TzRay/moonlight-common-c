// Use the real queue and RS codec; only replace the consumer and network callbacks.
#define reed_solomon_decode countedDecode
#define PltGetMicroseconds replayClockUs
#define queueRtpPacket capturePacket
#define notifyFrameLost captureLoss
#define connectionSawFrame ignoreFrame
#define connectionRecordFecBlock captureStats
#define connectionSendFrameFecStatus ignoreStatus
#define isReferenceFrameInvalidationEnabled allowRfi
#include "../src/RtpVideoQueue.c"
#undef reed_solomon_decode
#include <stdio.h>
int reed_solomon_decode(reed_solomon*, uint8_t**, uint8_t*, int, int);
static unsigned rsCalls;
int countedDecode(reed_solomon* rs, uint8_t** shards, uint8_t* marks, int count, int size) {
    rsCalls++;
    return reed_solomon_decode(rs, shards, marks, count, size);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { GROUPS = 4, DATA = 32, PARITY = 16, TOTAL = DATA + PARITY, WIRE = 96 };
static uint8_t original[GROUPS][TOTAL][WIRE];
static unsigned delivered, lost, repaired, arrival, ready;
static uint32_t number, first;
static uint64_t nowUs = 1000000;
static unsigned recoveryReports;
uint64_t replayClockUs(void) { return nowUs; }

bool capturePacket(PRTPV_QUEUE_ENTRY entry) {
    unsigned g = delivered / DATA, i = delivered % DATA;
    CHECK(g < GROUPS && !entry->isParity && entry->receiveTimeUs);
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)((uint8_t*)entry->packet + MAX_RTP_HEADER_SIZE);
    PNV_VIDEO_PACKET expected = (PNV_VIDEO_PACKET)(original[g][i] + MAX_RTP_HEADER_SIZE);
    CHECK(entry->packet->sequenceNumber == (uint16_t)(first + g * TOTAL + i));
    CHECK(nv->frameIndex == number && nv->streamPacketIndex == (uint32_t)((first + g * TOTAL + i) << 8));
    CHECK(nv->flags == expected->flags && nv->multiFecBlocks == expected->multiFecBlocks);
    CHECK(memcmp(nv + 1, expected + 1, WIRE - MAX_RTP_HEADER_SIZE - sizeof(*nv)) == 0);
    delivered++;
    if (delivered == GROUPS * DATA) ready = arrival;
    free(entry->packet);
    return true;
}
void captureLoss(unsigned frame, bool speculative) { (void)frame; (void)speculative; lost++; }
void ignoreFrame(uint32_t frame) { (void)frame; }
void ignoreStatus(PSS_FRAME_FEC_STATUS status) { (void)status; recoveryReports++; }
bool allowRfi(void) { return true; }
#undef connectionRecordFecBlock
void connectionRecordFecBlock(uint16_t data, uint16_t received);
void captureStats(uint16_t data, uint16_t received) {
    connectionRecordFecBlock(data, received);
    repaired += data - received;
}

static void prepare(uint32_t frame, uint32_t base) {
    number = frame; first = base;
    delivered = repaired = arrival = ready = recoveryReports = 0;
    memset(original, 0, sizeof(original));
    reed_solomon* rs = reed_solomon_new(DATA, PARITY);
    CHECK(rs);
    for (unsigned g = 0; g < GROUPS; g++) {
        uint8_t* shards[TOTAL];
        for (unsigned i = 0; i < TOTAL; i++) {
            shards[i] = original[g][i];
            if (i >= DATA) continue;
            PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(shards[i] + MAX_RTP_HEADER_SIZE);
            nv->frameIndex = frame;
            nv->streamPacketIndex = (base + g * TOTAL + i) << 8;
            nv->multiFecFlags = 0x10;
            nv->multiFecBlocks = (GROUPS - 1) << 6 | g << 4;
            nv->flags = FLAG_CONTAINS_PIC_DATA | (!i ? FLAG_SOF : 0) | (i + 1 == DATA ? FLAG_EOF : 0);
            for (size_t j = MAX_RTP_HEADER_SIZE + sizeof(*nv); j < WIRE; j++)
                shards[i][j] = (uint8_t)(frame + g * 19 + i * 67 + j * (i + 3));
        }
        CHECK(reed_solomon_encode(rs, shards, TOTAL, WIRE) == 0);
    }
    reed_solomon_release(rs);
}

static void (*sendOnSocket)(const void* packet, size_t length);
static void sendShardNumber(PRTP_VIDEO_QUEUE queue, unsigned g, unsigned i, bool mixed, unsigned mask, uint32_t frame) {
    uint8_t* bytes = malloc(WIRE + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(bytes);
    memcpy(bytes, original[g][i], WIRE);
    PRTP_PACKET rtp = (PRTP_PACKET)bytes;
    rtp->header = 0x80 | FLAG_EXTENSION;
    rtp->sequenceNumber = (uint16_t)(first + g * TOTAL + i);
    rtp->timestamp = frame * 1500;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(bytes + MAX_RTP_HEADER_SIZE);
    nv->frameIndex = LE32(frame);
    nv->multiFecBlocks = (GROUPS - 1) << 6 | g << 4 | mask;
    nv->fecInfo = LE32(DATA << 22 | i << 12 | 50 << 4 | (mixed ? SS_FEC_INFO_INTERLEAVED : 0));
    if (sendOnSocket) {
        rtp->sequenceNumber = BE16(rtp->sequenceNumber);
        rtp->timestamp = BE32(rtp->timestamp);
        sendOnSocket(bytes, WIRE);
        free(bytes);
        return;
    }
    if (RtpvAddPacket(queue, rtp, WIRE, (PRTPV_QUEUE_ENTRY)(bytes + WIRE)) == RTPF_RET_REJECTED)
        free(bytes);
}

static void sendShard(PRTP_VIDEO_QUEUE queue, unsigned g, unsigned i, bool mixed, unsigned mask) {
    sendShardNumber(queue, g, i, mixed, mask, number);
}

// Earlier blocks are complete. The last block carries no completion marker;
// early parity precedes the second half of its originals as on the wire.
static void staticDesktop(PRTP_VIDEO_QUEUE queue, unsigned parity) {
    for (unsigned g = 0; g < GROUPS - 1; g++) {
        for (unsigned i = 0; i <= DATA; i++) sendShard(queue, g, i, true, (1U << g) - 1);
    }
    for (unsigned i = 0; i < DATA / 2; i++) sendShard(queue, GROUPS - 1, i, true, 7);
    for (unsigned i = 0; i < parity; i++) sendShard(queue, GROUPS - 1, DATA + i, true, 7);
    for (unsigned i = DATA / 2; i < DATA - 1; i++) sendShard(queue, GROUPS - 1, i, true, 7);
}

