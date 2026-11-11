// The emulator's network ports: the command port (1040), the paper tape reader and punch (1042,
// 1043) and the four display screens (3400-3403).
// The typewriter's port, 1041, is typtelnet.c's.
// netthread() listens on them for the life of the process and runs the fd hand-overs on its own
// thread; serveN() (pollfd.c) gives the command port's handler a thread per connection.
//
// 8-Nov-2026 wje - start history, original code never had it, fix an issue with nonterminated lines.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "pdp1.h"
#include "display.h"
#include "papertape.h"
#include "netPorts.h"

// How long a command with no newline waits for more bytes before it runs as it stands.
// pdp1audio sends its command with no newline and waits for the reply on the open connection.
#define CMDQUIETMS 50

// Runs one command line and writes the response back followed by a newline.
// Returns 0, or -1 when the write fails.
static int
runnetcmd(int fd, PDP1 *pdp, char *lineP)
{
char *r;
int n;

    r = handlecmd(pdp, lineP);
    n = strlen(r);

    // handlecmd() returns a pointer to this thread's 1024-byte response buffer.
    // Only append our newline terminator when doing so cannot walk past the end
    // of that buffer, otherwise send the response as-is.
    if( (n + 2) <= 1024 )
    {
        r[n] = '\n';
        r[n + 1] = '\0';
        n++;
    }

    return( (write(fd, r, n) < 0) ? -1 : 0 );
}

// Network command-port connection handler.
// Reads from fd, a connected socket, and runs each newline-ended line as one command, however
// TCP splits or joins them: two commands in one read() both run, and a command split across
// reads runs once, whole. A tail with no newline runs when the client closes, or once no more
// bytes come for CMDQUIETMS. A line that fills the buffer with no newline runs as it stands.
// Loops until the peer closes the connection or a write fails, then closes fd.
void
handlenetcmd(int fd, void *arg)
{
PDP1 *pdp = (PDP1*)arg;
char buf[1024];
char *lineP;
char *endP;
char *nlP;
struct pollfd pfd;
size_t have;
ssize_t n;
int failed;

    pfd.fd = fd;
    pfd.events = POLLIN;
    have = 0;
    failed = 0;

    while( !failed )
    {
        // Nothing pending: wait as long as it takes. A partial command: only the quiet time.
        n = poll(&pfd, 1, ((have > 0) ? CMDQUIETMS : -1));
        if( n < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }
            break;
        }

        if( n == 0 )
        {
            buf[have] = '\0';
            have = 0;
            failed = (runnetcmd(fd, pdp, buf) < 0);
            continue;
        }

        n = read(fd, (buf + have), (sizeof(buf) - 1 - have));
        if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }

        if( n <= 0 )
        {
            // The client closed its side: a pending command still runs, and a client that only
            // shut down its writing still gets the reply.
            if( (n == 0) && (have > 0) )
            {
                buf[have] = '\0';
                runnetcmd(fd, pdp, buf);
            }
            break;
        }

        have += n;
        lineP = buf;
        endP = (buf + have);
        while( !failed && ((nlP = memchr(lineP, '\n', (endP - lineP))) != nil) )
        {
            *nlP = '\0';
            failed = (runnetcmd(fd, pdp, lineP) < 0);
            lineP = (nlP + 1);
        }

        have = (endP - lineP);
        memmove(buf, lineP, have);

        if( !failed && (have == (sizeof(buf) - 1)) )
        {
            buf[have] = '\0';
            have = 0;
            failed = (runnetcmd(fd, pdp, buf) < 0);
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
