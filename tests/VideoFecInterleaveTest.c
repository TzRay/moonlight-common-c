// Use the real queue and RS codec; only replace the consumer and network callbacks.
#define queueRtpPacket capturePacket
#define notifyFrameLost captureLoss
#define connectionSawFrame ignoreFrame
#define connectionRecordFecBlock captureStats
#define connectionSendFrameFecStatus ignoreStatus
#define isReferenceFrameInvalidationEnabled allowRfi
#include "../src/RtpVideoQueue.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { GROUPS = 4, DATA = 32, PARITY = 16, TOTAL = DATA + PARITY, WIRE = 96 };
static uint8_t original[GROUPS][TOTAL][WIRE];
static unsigned delivered, lost, repaired, arrival, ready;
static uint32_t number, first;

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
void ignoreStatus(PSS_FRAME_FEC_STATUS status) { (void)status; }
bool allowRfi(void) { return true; }
void captureStats(uint32_t frame, uint8_t group, uint16_t data, uint16_t received) {
    (void)frame; (void)group;
    repaired += data - received;
}

static void prepare(uint32_t frame, uint32_t base) {
    number = frame; first = base;
    delivered = repaired = arrival = ready = 0;
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

static void sendShard(PRTP_VIDEO_QUEUE queue, unsigned g, unsigned i, bool mixed, unsigned mask) {
    uint8_t* bytes = malloc(WIRE + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(bytes);
    memcpy(bytes, original[g][i], WIRE);
    PRTP_PACKET rtp = (PRTP_PACKET)bytes;
    rtp->header = 0x80 | FLAG_EXTENSION;
    rtp->sequenceNumber = (uint16_t)(first + g * TOTAL + i);
    rtp->timestamp = number * 1500;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(bytes + MAX_RTP_HEADER_SIZE);
    nv->frameIndex = LE32(number);
    nv->multiFecBlocks = (GROUPS - 1) << 6 | g << 4 | mask;
    nv->fecInfo = LE32(DATA << 22 | i << 12 | 50 << 4 | (mixed ? SS_FEC_INFO_INTERLEAVED : 0));
    if (RtpvAddPacket(queue, rtp, WIRE, (PRTPV_QUEUE_ENTRY)(bytes + WIRE)) == RTPF_RET_REJECTED)
        free(bytes);
}

static unsigned runFrame(PRTP_VIDEO_QUEUE queue, bool mixed, unsigned dropFirst, bool reorder) {
    unsigned mask = 0;
    // Four eight-packet stripes through each group's originals, with one early parity stripe.
    const unsigned stripes[] = {0, 8, DATA, 16, 24, DATA + 8};
    for (unsigned slot = 0; slot < GROUPS * TOTAL; slot++) {
        unsigned g = mixed ? (slot / 8) % GROUPS : slot / TOTAL;
        unsigned i = mixed ? stripes[slot / (GROUPS * 8)] + slot % 8 : slot % TOTAL;
        if (i == DATA - 1 && mixed) mask |= 1U << g;
        arrival = slot + 1;
        if (slot < dropFirst) continue;
        // Delay one original inside the first stripe, and duplicate a shard from another group.
        if (reorder && g == 0 && i == 2) continue;
        sendShard(queue, g, i, mixed, mask);
        if (reorder && g == 0 && i == 6) sendShard(queue, g, 2, mixed, mask);
        if (reorder && g == 1 && i == 4) sendShard(queue, g, i, mixed, mask);
    }
    return ready;
}

int main(void) {
    RTP_VIDEO_QUEUE queue;
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431;
    StreamConfig.packetSize = WIRE - MAX_RTP_HEADER_SIZE;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    RtpvInitializeQueue(&queue);
    VideoFecInterleavingEnabled = false;
    prepare(1, 0);
    unsigned sequential = runFrame(&queue, false, 0, false);
    CHECK(sequential && repaired == 0);

    VideoFecInterleavingEnabled = true;
    prepare(2, GROUPS * TOTAL);
    CHECK(runFrame(&queue, false, 0, false) == sequential); // Unmarked fallback.
    prepare(3, 65510); // RTP wraps within the first group; SPI continues across it.
    unsigned interleaved = runFrame(&queue, true, 0, true);
    CHECK(interleaved && interleaved < sequential && repaired == 0 && !lost);
    prepare(4, first + GROUPS * TOTAL);
    CHECK(runFrame(&queue, true, 24, false) && repaired == 24 && !lost);

    prepare(5, first + GROUPS * TOTAL);
    CHECK(!runFrame(&queue, true, 96, false));
    prepare(6, first + GROUPS * TOTAL);
    CHECK(runFrame(&queue, false, 0, false) == sequential && lost == 1);
    prepare(7, first + GROUPS * TOTAL);
    sendShard(&queue, 3, 5, true, 0); // Stop with a later group outstanding.
    RtpvCleanupQueue(&queue);
    cleanupPlatform();
    printf("ready packet: sequential %u, interleaved %u of %u; recovered 24 originals\n",
           sequential, interleaved, GROUPS * TOTAL);
    return 0;
}
