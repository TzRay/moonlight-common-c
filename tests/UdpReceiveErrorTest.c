// Exercise the production deadline and SO_RCVTIMEO paths at the OS boundary.
#include "../src/Limelight-internal.h"
#include <stdio.h>

static int pollError, readError, reads, readFlags;
static int testPoll(void) {
    if (!pollError) return 1;
    SetLastSocketError(pollError);
    return -1;
}
static int testRead(int flags) {
    reads++;
    readFlags = flags;
    SetLastSocketError(readError);
    return -1;
}
#define recvfrom(s, buf, len, flags, addr, addrlen) testRead(flags)
#define recvmsg(s, msg, flags) testRead(flags)
#if defined(LC_WINDOWS) && !defined(NXDK)
#define select(n, rd, wr, ex, timeout) testPoll()
#else
#define poll(fds, count, timeout) testPoll()
#endif
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
    cleanupPlatform();
    return 0;
}
