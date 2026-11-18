// Exercise the outer RTP counters before video recovery, without a network peer.
#include "../src/RtpVideoQueue.c"

static void shard(PRTP_VIDEO_QUEUE queue, uint32_t frame, uint8_t block,
                  uint16_t data, uint16_t index) {
    NV_VIDEO_PACKET packet = { 0 };
    packet.frameIndex = frame;
    packet.multiFecBlocks = block << 4;
    packet.fecInfo = ((uint32_t)data << 22) | ((uint32_t)index << 12);
    observePyrowaveRtpPacket(queue, &packet);
    queue->currentFrameNumber = frame;
}

int main(void) {
    RTP_VIDEO_QUEUE queue = { 0 };
    connectionRecordFecBlock(1, 0, 100, 98);
    connectionRecordFecBlock(1, 0, 100, 98);  // Repeated final report.
    shard(&queue, 2, 0, 70, 69);
    shard(&queue, 2, 0, 70, 69);
    shard(&queue, 2, 0, 70, 1);
    shard(&queue, 2, 1, 2, 1);   // Two unique shards of seventy in the old block.
    shard(&queue, 2, 0, 70, 4);  // Old block, already reported.
    shard(&queue, 2, 1, 2, 0);   // Clean block received out of order.
    shard(&queue, 3, 0, 7, 2);
    uint64_t stats = LiGetObservedVideoDataPackets();
    return (uint32_t)(stats >> 32) == 70 && (uint32_t)stats == 172 ? 0 : 1;
}
