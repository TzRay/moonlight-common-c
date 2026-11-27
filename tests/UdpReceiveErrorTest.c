// Exercise the production deadline and SO_RCVTIMEO paths at the OS boundary.
#include "../src/Limelight-internal.h"
#include <stdio.h>

static int pollError, readError, reads, readFlags;
enum { ERROR_ONLY, RESET_THEN_DATA, RESET_UNTIL_DEADLINE };
static int scenario, waits[2], polls;
static uint64_t clockUs = 100000;
static uint64_t testMicros(void) { return clockUs; }
static int testPoll(int timeoutMs) {
    if (scenario != ERROR_ONLY) {
        if (polls >= 2) abort();
        waits[polls++] = timeoutMs;
    }
    if (!pollError) return 1;
    SetLastSocketError(pollError);
    return -1;
}
static int testRead(char* buffer, int flags) {
    reads++;
    readFlags = flags;
    if (scenario != ERROR_ONLY) {
        if (reads == 2 && scenario == RESET_THEN_DATA) {
            *buffer = 'v';
            return 1;
        }
        clockUs += reads == 1 ? 5000 : 7000;
#if defined(LC_WINDOWS) && !defined(NXDK)
        readError = WSAECONNRESET;
#else
        readError = ECONNREFUSED;
#endif
    }
    SetLastSocketError(readError);
    return -1;
}
#define recvfrom(s, buf, len, flags, addr, addrlen) testRead(buf, flags)
#define recvmsg(s, msg, flags) testRead((char*)(msg)->msg_iov[0].iov_base, flags)
#if defined(LC_WINDOWS) && !defined(NXDK)
static int testSelect(const struct timeval* timeout) {
    return testPoll(timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1);
}
#define select(n, rd, wr, ex, timeout) testSelect(timeout)
#else
#define poll(fds, count, timeout) testPoll(timeout)
#endif
#define PltGetMicroseconds testMicros
#include "../src/PlatformSockets.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    char byte;
    int errors[] = { EINTR, EWOULDBLOCK, EAGAIN, ETIMEDOUT,
#if defined(LC_WINDOWS) && !defined(NXDK)
                     WSA_IO_PENDING,
#endif
    };
    CHECK(initializePlatform() == 0);
    uint64_t deadline = PltGetMicroseconds() + 1000000;
    pollError = EINTR;
    CHECK(recvUdpSocketTimed(1, &byte, 1, false, deadline, NULL) == 0 && !reads);
    pollError = 0;
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        readError = errors[i];
        CHECK(recvUdpSocketTimed(1, &byte, 1, false, deadline, NULL) == 0);
#ifdef MSG_DONTWAIT
        CHECK((readFlags & MSG_DONTWAIT) != 0);
#endif
        CHECK(recvUdpSocketTimed(1, &byte, 1, false, 0, NULL) == 0 && !readFlags);
    }
    readError = EMSGSIZE;
    CHECK(recvUdpSocketTimed(1, &byte, 1, false, deadline, NULL) < 0 && LastSocketError() == EMSGSIZE);
    for (scenario = RESET_THEN_DATA; scenario <= RESET_UNTIL_DEADLINE; scenario++) {
        uint64_t start = clockUs;
        reads = polls = 0;
        byte = 0;
        int result = recvUdpSocketTimed(1, &byte, 1, false, start + 12000, NULL);
        CHECK(reads == 2 && polls == 2 && waits[0] == 12 && waits[1] == 7);
        if (scenario == RESET_THEN_DATA) {
            CHECK(result == 1 && byte == 'v' && clockUs == start + 5000);
        }
        else {
            CHECK(result == 0 && byte == 0 && clockUs == start + 12000);
        }
        printf("UDP reset: poll intervals %d and %d ms, recv result %d\n", waits[0], waits[1], result);
    }
    cleanupPlatform();
    return 0;
}
