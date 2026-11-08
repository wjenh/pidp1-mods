// The emulator's network ports: the command port (1040), the paper tape reader and punch (1042,
// 1043) and the four display screens (3400-3403).
// The typewriter's port, 1041, is typtelnet.c's.
// netthread() listens on them for the life of the process and runs the fd hand-overs on its own
// thread; serveN() (pollfd.c) gives the command port's handler a thread per connection.

#include <fcntl.h>
#include <unistd.h>

#include "common.h"
#include "pdp1.h"
#include "display.h"
#include "papertape.h"
#include "netPorts.h"

// Network command-port connection handler.
// Reads lines from fd, a connected socket, passes each to handlecmd() for processing,
// and writes the response back followed by a newline.
// Loops until the peer closes the connection then closes fd.
void
handlenetcmd(int fd, void *arg)
{
PDP1 *pdp = (PDP1*)arg;
char *r;
char line[1024];
int n;

    while( (n = read(fd, line, (sizeof(line) - 1))), n > 0 )
    {
        line[n] = 0;
        r = handlecmd(pdp, line);
        n = strlen(r);

        // handlecmd() returns a pointer to this thread's 1024-byte response buffer.
        // Only append our newline terminator when doing so cannot walk past the end
        // of that buffer, otherwise send the response as-is.
        if( (n + 2) <= 1024 )
        {
            r[n] = '\n';
            r[n + 1] = '\0';
            if( write(fd, r, (n + 1)) < 0 )
            {
                break;      // peer gone or socket error; stop feeding it more commands
            }
        }
        else
        {
            if( write(fd, r, n) < 0 )
            {
                break;
            }
        }
    }

    close(fd);
}

// Attaches a newly-connected display client's socket fd to display screen screenNo (0-3) and
// puts it in nonblocking mode.
void
connectdpy(int screenNo, int fd)
{
    nodelay(fd);
    setDisplayFD(screenNo, fd);
}

// Called when a connection request comes in on the screen-0 display port
// Hooks fd up as screen 0's display client via connectdpy().
void
handledpy(int fd, void *arg)
{
    (void)arg;      // signature matched to the pollfd accept-callback type; not needed here
    connectdpy(0, fd);
}

// Same as handledpy() above, but for display screen 1.
void
handledpy2(int fd, void *arg)
{
    (void)arg;
    connectdpy(1, fd);
}

// Same as handledpy() above, but for display screen 2.
void
handledpy3(int fd, void *arg)
{
    (void)arg;
    connectdpy(2, fd);
}

// Same as handledpy() above, but for display screen 3.
void
handledpy4(int fd, void *arg)
{
    (void)arg;
    connectdpy(3, fd);
}

// Called when a connection request comes in on the paper-tape-reader network port.
// Connects the newly-connected fd in the reader in place of whatever it had.
// The fd is made non-blocking so a client that pauses or stops reading the echoes
// cannot hold the emulator thread, and it is handed over with mountReader() (papertape.c),
// which the emulator thread adopts at its next pass.
void
handleptr(int fd, void *arg)
{
    (void)arg;      // signature matched to the pollfd accept-callback type; not needed here
    nodelay(fd);
    fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
    mountReader(fd);
}

// Same as handleptr() above, but for the paper-tape punch (p_fd) instead of the reader.
void
handleptp(int fd, void *arg)
{
    (void)arg;
    nodelay(fd);
    fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
    mountPunch(fd);
}

// Thread entry point that listens on all the network command/display/reader/punch ports
//  and dispatches accepted connections to the matching handler.
// Runs for the life of the process, serveN() only returns if its poll() fails.
// Returns nil, the thread's exit value is unused.
void*
netthread(void *arg)
{
    // The command port's handler keeps its connection so it runs threaded.
    // The others just hand the fd over.
    struct PortHandler ports[] =
    {
        { 1040, handlenetcmd, 1 },
        // 1041 is typewriter
        { 1042, handleptr, 0 },
        { 1043, handleptp, 0 },
        { 3400, handledpy, 0 },
        { 3401, handledpy2, 0 },
        { 3402, handledpy3, 0 },
        { 3403, handledpy4, 0 },
    };
    serveN(ports, nelem(ports), arg);
    return( nil );
}
