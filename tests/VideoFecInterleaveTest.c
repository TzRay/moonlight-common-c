#include "VideoFecFixture.h"

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

static void compareWindows(void) {
    // Replay the same six final-original delays at each negotiated rate. The
    // final trace has permanent loss. The final profile varies inter-frame arrival
    // intervals under a 60 fps negotiation, as on a static or variable-rate desktop.
    const unsigned rates[] = {30, 60, 120, 240, 60};
    const unsigned delays[] = {2000, 6000, 10000, 15000, 25000, UINT32_MAX};
    for (unsigned r = 0; r < sizeof(rates)/sizeof(rates[0]); r++) {
        StreamConfig.fps = rates[r];
        unsigned period = 1000000 / rates[r];
        unsigned bounded = period / 2;
        if (bounded < 4000) bounded = 4000;
        if (bounded > 12000) bounded = 12000;
        const unsigned windows[] = {0, period, bounded};
        for (unsigned w = 0; w < 3; w++) {
            unsigned caught = 0, calls = 0, sumUs = 0;
            for (unsigned t = 0; t < sizeof(delays)/sizeof(delays[0]); t++) {
                RTP_VIDEO_QUEUE queue;
                RtpvInitializeQueue(&queue);
                prepare(1, 0);
                nowUs += r == 4 ? (t % 2 ? 250000 : 4167) : period;
                staticDesktop(&queue, 8);
                CHECK(!delivered && RtpvGetRecoveryDeadlineUs(&queue) == nowUs + bounded);
                // Override just the deadline for the two comparison arms.
                if (w != 2) queue.interleaved->blocks[GROUPS - 1].recoveryDeadlineUs = nowUs + windows[w];
                unsigned before = rsCalls;
                if (delays[t] < windows[w]) {
                    nowUs += delays[t];
                    RtpvExpireRecoveryDeadline(&queue, nowUs);
                    CHECK(!delivered);
                    sendShard(&queue, GROUPS - 1, DATA - 1, true, 15);
                    sumUs += delays[t];
                    caught++;
                } else {
                    if (windows[w]) {
                        RtpvExpireRecoveryDeadline(&queue, nowUs + windows[w] - 1);
                        CHECK(!delivered);
                    }
                    nowUs += windows[w];
                    RtpvExpireRecoveryDeadline(&queue, nowUs);
                    sumUs += windows[w];
                }
                CHECK(delivered == GROUPS * DATA && !RtpvGetRecoveryDeadlineUs(&queue));
                CHECK(repaired == (delays[t] < windows[w] ? 0 : 1));
                RtpvExpireRecoveryDeadline(&queue, nowUs + 100000);
                CHECK(delivered == GROUPS * DATA);
                calls += rsCalls - before;
                RtpvCleanupQueue(&queue);
            }
            printf("fps=%u%s window=%uus recovered_frames=6/6 mean_delivery=%uus late_originals=%u final_block_RS=%u\n",
                   rates[r], r == 4 ? " VFR" : "", windows[w], sumUs/6, caught, calls);
        }
    }
}

static void checkBoundary(void) {
    RTP_VIDEO_QUEUE queue;
    RtpvInitializeQueue(&queue);
    prepare(1, 0);
    staticDesktop(&queue, 0);
    CHECK(!RtpvGetRecoveryDeadlineUs(&queue));
    RtpvExpireRecoveryDeadline(&queue, nowUs + 100000);
    CHECK(!delivered);
    sendShard(&queue, 3, DATA, true, 7);
#ifdef FEC_VALIDATION_MODE
    CHECK(!RtpvGetRecoveryDeadlineUs(&queue));
    sendShard(&queue, 3, DATA + 1, true, 7);
#endif
    CHECK(RtpvGetRecoveryDeadlineUs(&queue));
    sendShardNumber(&queue, 0, 0, true, 0, 2);
    CHECK(delivered == GROUPS * DATA && repaired == 1);
    RtpvCleanupQueue(&queue);
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
    compareWindows();
    checkBoundary();
    cleanupPlatform();
    printf("ready packet: sequential %u, interleaved %u of %u; recovered 24 originals\n",
           sequential, interleaved, GROUPS * TOTAL);
    return 0;
}
