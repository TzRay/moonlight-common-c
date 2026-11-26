// Exercise the pre-recovery counters through the real RTP ingress, including
// data arriving after a sequential frame was abandoned by the decoder.
#define queueRtpPacket discardPacket
#define notifyFrameLost noteLoss
#define connectionSawFrame ignoreFrame
#define connectionSendFrameFecStatus noteStatus
#define isReferenceFrameInvalidationEnabled noSpeculation
#include "../src/RtpVideoQueue.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned losses, reports;
bool discardPacket(PRTPV_QUEUE_ENTRY entry) { free(entry->packet); return true; }
void noteLoss(unsigned frame, bool speculative) { (void)frame; CHECK(!speculative); losses++; }
void ignoreFrame(uint32_t frame) { (void)frame; }
void noteStatus(PSS_FRAME_FEC_STATUS status) { (void)status; reports++; }
bool noSpeculation(void) { return false; }

static unsigned fecPercent;
static void shard(PRTP_VIDEO_QUEUE queue, uint32_t frame, unsigned count, unsigned block,
                  unsigned data, unsigned index, uint16_t base) {
    size_t length = MAX_RTP_HEADER_SIZE + sizeof(NV_VIDEO_PACKET);
    char* bytes = calloc(1, length + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(bytes);
    PRTP_PACKET packet = (PRTP_PACKET)bytes;
    packet->header = FLAG_EXTENSION;
    packet->sequenceNumber = base + index;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(bytes + MAX_RTP_HEADER_SIZE);
    nv->frameIndex = LE32(frame);
    nv->multiFecBlocks = ((count - 1) << 6) | (block << 4);
    nv->flags = FLAG_CONTAINS_PIC_DATA | (!index ? FLAG_SOF : 0) | (index + 1 == data ? FLAG_EOF : 0);
    nv->fecInfo = LE32((data << 22) | (index << 12) | (fecPercent << 4));
    if (RtpvAddPacket(queue, packet, length, (PRTPV_QUEUE_ENTRY)(bytes + length)) == RTPF_RET_REJECTED) free(bytes);
}

int main(void) {
    RTP_VIDEO_QUEUE queue;
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431;
    StreamConfig.packetSize = sizeof(NV_VIDEO_PACKET);
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    RtpvInitializeQueue(&queue);

    for (unsigned i = 0; i < 32; i++) shard(&queue, 1, 3, 0, 32, i, 0);
    CHECK(LiGetObservedVideoDataPackets() == 32);
    // Block 1 has no header on the wire. Block 2 has a different size.
    shard(&queue, 1, 3, 2, 31, 0, 64);
    CHECK(losses == 1 && reports == 0 && LiGetVideoPacketCoverageGaps() == 1);
    shard(&queue, 1, 3, 0, 32, 31, 0);
    shard(&queue, 1, 3, 2, 31, 0, 64);
    shard(&queue, 4, 1, 0, 1, 0, 96);
    CHECK(LiGetObservedVideoDataPackets() == ((UINT64_C(30) << 32) | 64));
    CHECK(LiGetVideoPacketCoverageGaps() == 3 && reports == 0);
    RtpvCleanupQueue(&queue);

    // Rejected sequence numbers and an isolated discontinuity cannot replace
    // the observation watermark, even on a plaintext stream.
    RtpvInitializeQueue(&queue);
    shard(&queue, 1, 1, 0, 3, 0, 0);
    shard(&queue, 905000, 1, 0, 1, 0, 200);
    shard(&queue, 8, 1, 0, 1, 0, 65530);
    shard(&queue, 1, 1, 0, 3, 9, 0);
    CHECK(queue.currentFrameNumber == 1 && queue.observation.frame == 1);
    CHECK(LiGetVideoPacketCoverageGaps() == 3);
    RtpvCleanupQueue(&queue);

    // PyroWave feeds each original straight to the depacketizer.
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    RtpvInitializeQueue(&queue);
    shard(&queue, 1, 2, 0, 70, 69, 0);
    shard(&queue, 1, 2, 0, 70, 69, 0);
    shard(&queue, 1, 2, 0, 70, 1, 0);
    shard(&queue, 1, 2, 1, 2, 1, 70);
    shard(&queue, 1, 2, 0, 70, 4, 0);
    shard(&queue, 1, 2, 1, 2, 0, 70);
    CHECK(LiGetObservedVideoDataPackets() == ((UINT64_C(98) << 32) | 136));
    CHECK(LiGetVideoPacketCoverageGaps() == 3);
    RtpvCleanupQueue(&queue);
    // A parity shard describes the number of originals even when all are lost.
    CHECK(initializeControlStream() == 0);
    VideoFecControlSupported = true;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    RtpvInitializeQueue(&queue);
    fecPercent = 25;
    shard(&queue, 1, 1, 0, 4, 4, 0);
    fecPercent = 0;
    shard(&queue, 2, 1, 0, 1, 0, 5);
    CHECK(LiGetObservedVideoDataPackets() == ((UINT64_C(4) << 32) | 5));
    CHECK(!LiGetVideoPacketCoverageGaps());
    SS_FEC_SUMMARY summary;
    connectionTakeFecSummary(&summary);
    CHECK(BE32(summary.dataPackets) == 5 && BE32(summary.missingDataPackets) == 4);
    connectionTakeFecSummary(&summary);
    CHECK(!summary.dataPackets && !summary.missingDataPackets && BE32(summary.sequence) == 2);
    RtpvCleanupQueue(&queue);
    // Older single-block hosts do not define the multi-block fields. A header
    // with no usable FEC geometry supplies neither a numerator nor a denominator.
    AppVersionQuad[0] = 4;
    RtpvInitializeQueue(&queue);
    shard(&queue, 1, 3, 2, 0, 0, 0);
    shard(&queue, 1, 3, 2, 1, 0, 0);
    CHECK(LiGetObservedVideoDataPackets() == ((UINT64_C(4) << 32) | 6));
    CHECK(!LiGetVideoPacketCoverageGaps());
    RtpvCleanupQueue(&queue);
    destroyControlStream();
    cleanupPlatform();
    return 0;
}
