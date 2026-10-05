// Readiness of the emulator's typewriter input fd, for the tyi IOT plugin.
// 14-Jul-2026 wje cleanup, no warning now, still needs printf logging removed, real logging added.
// 05-Oct-2026 Claude no poll thread: the feeding thread sets ready, waitfd() re-arms it itself.
//
// An FD's ready flag says a read() will not block.
// The thread that writes into the fd's peer (typtelnet.c's relay) sets it after each write.
// The emulator thread, the only reader, clears it in waitfd() after each read and
// sets it again if data is left.
// Either order of the two leaves ready set while data waits.
// Ready is never set with nothing to read, the reader's read() blocks on the emulator thread.

#include <unistd.h>
#include <poll.h>
#include "common.h"

// Marks fdP readable. Called by the thread that feeds the fd, after its write.
void
markFdReady( FD *fdP )
{
    __atomic_store_n( &fdP->ready, 1, __ATOMIC_RELEASE );
}

// Re-arms fdP after a read: ready is cleared, then set again if a byte is still waiting.
void
waitfd( FD *fdP )
{
struct pollfd pfd;

    // Clear before looking, so a write landing after the poll below is caught by its writer's
    // markFdReady(), and one landing before it is seen by the poll.
    __atomic_store_n( &fdP->ready, 0, __ATOMIC_SEQ_CST );

    if( fdP->fd < 0 )
    {
        return;
    }

    pfd.fd = fdP->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if( (poll(&pfd, 1, 0) > 0) && (pfd.revents & POLLIN) )
    {
        __atomic_store_n( &fdP->ready, 1, __ATOMIC_RELEASE );
    }
}

// Closes fdP, after a failed read. The caller also sets fdP->fd to -1.
void
closefd( FD *fdP )
{
    __atomic_store_n( &fdP->ready, 0, __ATOMIC_RELEASE );

    if( fdP->fd >= 0 )
    {
        close( fdP->fd );
    }
}
