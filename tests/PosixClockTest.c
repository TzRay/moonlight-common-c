#include "Limelight-internal.h"

#include <stdio.h>

// GNU ld wraps the two system clock reads made by the linked production library.
static bool monotonicAvailable;
static unsigned wallReads, monotonicReads;
static struct timeval wall = {1000, 900000};
static struct timespec monotonic = {700, 950000000};

int __wrap_clock_gettime(clockid_t clock, struct timespec* value) {
    (void)clock;
    monotonicReads++;
    if (!monotonicAvailable) {
        errno = EINVAL;
        return -1;
    }
    *value = monotonic;
    return 0;
}

int __wrap_gettimeofday(struct timeval* value, void* zone) {
    (void)zone;
    wallReads++;
    *value = wall;
    return 0;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int sendPacket(PRTP_AUDIO_QUEUE queue, uint16_t seq) {
    struct {
        RTP_PACKET header;
        uint8_t payload[4];
    } packet = {0};
    packet.header.header = 0x80;
    packet.header.packetType = 97;
    packet.header.sequenceNumber = seq;
    packet.header.timestamp = seq;
    return RtpaAddPacket(queue, &packet.header, sizeof(packet));
}

static int checkReorderWindow(bool latePacketArrives) {
    RTP_AUDIO_QUEUE queue;
    AppVersionQuad[0] = 8;
    AudioPacketDuration = 1;
    NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE;
    CHECK(RtpaInitializeQueue(&queue));
    sendPacket(&queue, 0); // The queue discards the first block to synchronize.
    for (uint16_t seq = 4; seq < 8; seq++) {
        CHECK(RTPQ_HANDLE_NOW(sendPacket(&queue, seq)));
    }
    CHECK(sendPacket(&queue, 12) == 0); // 8..11 have not arrived.
    CHECK(queue.nextRtpSequenceNumber == 8);

    wall = (struct timeval){998, 900000}; // Across the process start.
    CHECK(sendPacket(&queue, 13) == 0);
    CHECK(queue.nextRtpSequenceNumber == 8 && queue.stats.packetCountFecFailed == 0);
    wall.tv_usec += (RTPA_DATA_SHARDS + RTPQ_OOS_WAIT_TIME_MS) * 1000;
    CHECK(sendPacket(&queue, 14) == 0); // Still inside the inclusive MAT deadline.
    if (latePacketArrives) {
        CHECK(RTPQ_HANDLE_NOW(sendPacket(&queue, 8)));
        CHECK(queue.nextRtpSequenceNumber == 9 && queue.stats.packetCountFecFailed == 0);
    }
    else {
        wall.tv_usec++;
        CHECK(sendPacket(&queue, 15) == RTPQ_RET_PACKET_READY);
        CHECK(queue.nextRtpSequenceNumber == 12 && queue.stats.packetCountFecFailed == 1);
    }
    RtpaCleanupQueue(&queue);
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    monotonicAvailable = strcmp(argv[1], "monotonic") == 0;
    PltTicksInit();
    CHECK(PltGetMicroseconds() == 0);
    if (monotonicAvailable) {
        monotonic = (struct timespec){701, 5000000};
        wall.tv_sec -= 20;
        CHECK(PltGetMicroseconds() == 55000);
        CHECK(wallReads == 0 && monotonicReads >= 2);
    }
    else if (strcmp(argv[1], "queue") == 0) {
        wall.tv_usec += 5000;
        CHECK(checkReorderWindow(true) == 0);
        wall = (struct timeval){1001, 5000};
        CHECK(checkReorderWindow(false) == 0);
    }
    else {
        CHECK(strcmp(argv[1], "fallback") == 0);
        wall.tv_usec += 5000;
        CHECK(PltGetMicroseconds() == 5000);
        wall.tv_usec -= 5001; // Earlier than startup, in the same second.
        CHECK(PltGetMicroseconds() == 5000);
        wall = (struct timeval){1001, 5000}; // Borrow one second for the microseconds.
        CHECK(PltGetMicroseconds() == 110001);
        CHECK(wallReads > 1 && monotonicReads == 1);
    }
    return 0;
}
