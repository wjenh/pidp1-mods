// The localhost command-port client: one command line out, and the reply back.
// Port 1040 is the emulator's command port. It runs one command per read(), so a command goes
// out in a single write, and the reply (one line, or several for help) ends with a newline.
// Port 1050 is the running front end's (pdp1_periphES or pdpsrv), which never replies, so for
// it the caller passes no reply buffer and the line is only sent.

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "core.h"

#define PORT_TIMEOUT_MS 1000
#define REPLY_QUIET_MS 50       // after a newline, how long to wait for more of a many-line reply

// Milliseconds on the monotonic clock.
static long long
nowMs(void)
{
struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return( ((long long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000) );
}

// Connect to 127.0.0.1:port within the timeout.
// Returns the socket, -1 refused, -2 timed out, -3 any other error.
static int
connectLocal(int port)
{
struct sockaddr_in addr;
struct pollfd pfd;
socklen_t errLen;
int fd, err, n;

    if( (fd = socket(AF_INET, (SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC), 0)) < 0 )
    {
        return(-3);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if( !connect(fd, (struct sockaddr *)&addr, sizeof(addr)) )
    {
        return(fd);
    }

    err = errno;
    if( err == EINPROGRESS )
    {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if( (n = poll(&pfd, 1, PORT_TIMEOUT_MS)) == 0 )
        {
            close(fd);
            return(-2);
        }

        errLen = sizeof(err);
        if( (n < 0) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) )
        {
            close(fd);
            return(-3);
        }

        if( err == 0 )
        {
            return(fd);
        }
    }

    close(fd);
    return( (err == ECONNREFUSED) ? -1 : -3 );
}

// Send one command line to 127.0.0.1:port and, when replyP is given, read the reply into it
// without its final newline. With no reply buffer the line is only sent.
// Returns 0 done (replyP holds the reply, empty if the port closed without one), -1 refused,
// -2 timed out, -3 any other error.
int
portCommand(int port, const char *lineP, char *replyP, size_t replyLen)
{
char out[1024];
struct pollfd pfd;
long long deadline, waitMs;
size_t len, got;
ssize_t n;
int fd, result;

    len = (size_t)snprintf(out, sizeof(out), "%s\n", lineP);
    if( len >= sizeof(out) )
    {
        return(-3);
    }

    if( (fd = connectLocal(port)) < 0 )
    {
        return(fd);
    }

    // One write, so the emulator sees the whole command in one read().
    deadline = (nowMs() + PORT_TIMEOUT_MS);
    pfd.fd = fd;
    pfd.events = POLLOUT;
    // MSG_NOSIGNAL: a peer that has gone gives an error here, not a SIGPIPE that ends the app.
    if( (poll(&pfd, 1, PORT_TIMEOUT_MS) != 1) || (send(fd, out, len, MSG_NOSIGNAL) != (ssize_t)len) )
    {
        close(fd);
        return(-3);
    }

    if( !replyP || (replyLen == 0) )
    {
        close(fd);
        return(0);
    }

    // Read until the reply ends: a newline followed by a quiet spell, the port closing, or the
    // buffer filling.
    got = 0;
    result = -2;
    replyP[0] = '\0';
    pfd.events = POLLIN;
    while( (waitMs = (deadline - nowMs())) > 0 )
    {
        if( (got > 0) && (replyP[got - 1] == '\n') )
        {
            waitMs = REPLY_QUIET_MS;
        }

        if( (n = poll(&pfd, 1, (int)waitMs)) == 0 )
        {
            if( (got > 0) && (replyP[got - 1] == '\n') )
            {
                result = 0;
            }
            break;
        }

        if( (n < 0) && (errno != EINTR) )
        {
            result = -3;
            break;
        }

        if( n < 0 )
        {
            continue;
        }

        if( (n = read(fd, replyP + got, (replyLen - 1 - got))) < 0 )
        {
            if( (errno == EAGAIN) || (errno == EINTR) )
            {
                continue;
            }
            result = -3;
            break;
        }

        got += n;
        replyP[got] = '\0';
        if( (n == 0) || (got == (replyLen - 1)) )
        {
            result = 0;
            break;
        }
    }

    close(fd);
    if( (got > 0) && (replyP[got - 1] == '\n') )
    {
        replyP[got - 1] = '\0';
        if( result == -2 )
        {
            result = 0;         // the deadline came during the quiet spell after a whole reply
        }
    }

    return(result);
}

// Send a paper tape command (r or p, with or without a path) the way the tape scripts do: to
// the front end's port, which mounts the tape through its own reader or punch stream, or, when
// nothing listens there, straight to the emulator's command port.
// Returns the port that took it (replyP holds the emulator's reply, or is empty for the front
// end, which does not reply), or portCommand's negative code from the emulator's port.
int
tapeCommand(int frontPort, int emuPort, const char *lineP, char *replyP, size_t replyLen)
{
int result;

    replyP[0] = '\0';
    if( (result = portCommand(frontPort, lineP, NULL, 0)) == 0 )
    {
        return(frontPort);
    }

    if( result != -1 )
    {
        return(result);
    }

    if( (result = portCommand(emuPort, lineP, replyP, replyLen)) == 0 )
    {
        return(emuPort);
    }

    return(result);
}
