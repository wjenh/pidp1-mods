/*
 * dcstestharness.c -- Multi-mode TCP test peer for DCS2 IOT plugin tests.
 *
 * Purpose:
 *   Provides the external (non-PDP-1) side for each DCS2 automated test.
 *   The PDP-1 side runs one of the T0N.am1 programs under the emulator;
 *   this program provides the TCP counterpart so the PDP-1 test can make
 *   real socket connections without requiring a second machine.
 *
 * Usage:
 *   dcstestharness <mode> [args...]
 *
 * Modes:
 *   echo-server <port>
 *       Listen on <port>, accept one connection, echo every received byte
 *       back to the sender.  Exit when the connection closes (recv == 0).
 *       Exit code: 0 on clean close, 1 on any socket error.
 *       Used by: T01 (PDP-1 as client), T09 (PDP-1 as client, flexo mode).
 *
 *   send-verify-client <host> <port> <byte_hex>
 *       Connect to <host>:<port>.  Send one byte whose value is <byte_hex>
 *       (two hex digits without 0x prefix, e.g. "41" for 0x41 = 'A').
 *       Receive one byte back.  Print received byte as "RX: 0xNN".
 *       Exit 0 if received == sent, exit 1 otherwise.
 *       Used by: T02 (PDP-1 as server).
 *
 *   two-echo-servers <port1> <port2>
 *       Listen on both ports simultaneously (using select(2)).  Accept one
 *       connection on each port.  Echo all received bytes on both connections
 *       until both have closed.
 *       Exit code: 0 on clean close of both, 1 on any error.
 *       Used by: T04 (RRC/ROC test), T05 (scanner round-robin).
 *
 *   delay-connect <host> <port> <delay_ms>
 *       Sleep <delay_ms> milliseconds, then connect to <host>:<port>.
 *       Stay connected for 500 ms (so the PDP-1 can observe the connected
 *       state via rcs), then close gracefully.
 *       Exit code: 0 on success, 1 on connect/sleep error.
 *       Used by: T06 (RCS status, delay=100), T08 (RWE, delay=300).
 *
 *   reconnect-client <host> <port>
 *       Round-trip 1: connect, send 0x41, receive one byte, verify == 0x41,
 *                     close.
 *       Sleep 500 ms (gives PDP-1 time to issue SCBREBIND).
 *       Round-trip 2: connect again, send 0x42, receive one byte, verify
 *                     == 0x42, close.
 *       Exit 0 if both round-trips verified, 1 otherwise.
 *       Used by: T07 (SCBREBIND).
 *
 *   interrupt-client <host> <port>
 *       Connect to <host>:<port>.  Send byte 0x41.  Then wait up to 2 s
 *       for the server to close the connection (recv returns 0).  Exit 0
 *       on clean server-side close, 1 on timeout or error.
 *       Used by: T11 (SBS interrupt on receive).
 *
 * Architecture:
 *   Single-file, blocking I/O throughout except where noted.  select(2) is
 *   used in two-echo-servers to multiplex two listeners without threads.
 *   Each mode is a separate function; main() dispatches based on argv[1].
 *
 * Dependencies:
 *   POSIX sockets (Linux).  No external libraries beyond the C standard library.
 *
 * Build:
 *   gcc -Wall -Wextra -o dcstestharness dcstestharness.c
 *
 * Coding conventions follow the project CLAUDE.md standard:
 *   - 4-space indent, no tabs
 *   - mandatory parentheses for arithmetic, logical, bitwise operations
 *   - explicit braces on all control constructs, bodies on separate lines
 *   - camelCase naming, trailing P/ptr suffix on pointer variables
 *   - globals at top of file, locals at top of function body
 *   - declarations separated from initializations in most cases
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/* Buffer size for echo I/O.  Large enough to drain a full TCP segment
 * without requiring multiple recv/send cycles per iteration. */
#define BUF_SIZE        4096

/* Timeout (seconds) used by interrupt-client mode while waiting for the
 * server to close after sending the trigger byte. */
#define INTERRUPT_TIMEOUT_S     2

/* How long (ms) reconnect-client and delay-connect modes keep the
 * connection alive before closing, allowing the PDP-1 to observe it. */
#define HOLD_MS         500

/* -------------------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------------------- */

/* Program name from argv[0], used in error messages. */
static const char *progName;

/* =========================================================================
 * Utility helpers
 * ========================================================================= */

/*
 * sleepMs -- sleep for the given number of milliseconds.
 * Uses nanosleep() for sub-second precision.
 * Returns 0 on success, -1 if nanosleep was interrupted (EINTR).
 */
static int
sleepMs(int ms)
{
    struct timespec ts;

    ts.tv_sec  = (ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    return(nanosleep(&ts, NULL));
}

/*
 * makeTcpListener -- create a listening TCP socket on the given port.
 * SO_REUSEADDR is set so the server can restart immediately after a prior
 * instance exits without waiting for TIME_WAIT to clear.
 * Returns the listening fd on success, -1 on any socket/bind/listen error
 * (error already printed to stderr).
 */
static int
makeTcpListener(int port)
{
    int                 listenFd;
    int                 opt;
    struct sockaddr_in  addr;

    listenFd = -1;
    opt = 1;

    if( (listenFd = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        fprintf(stderr, "%s: socket() failed: %s\n", progName, strerror(errno));
        return(-1);
    }

    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if( bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0 )
    {
        fprintf(stderr, "%s: bind() on port %d failed: %s\n",
                progName, port, strerror(errno));
        close(listenFd);
        return(-1);
    }

    if( listen(listenFd, 4) < 0 )
    {
        fprintf(stderr, "%s: listen() on port %d failed: %s\n",
                progName, port, strerror(errno));
        close(listenFd);
        return(-1);
    }

    return(listenFd);
}

/*
 * acceptOne -- block until one connection arrives on listenFd, then return
 * the accepted connection fd.  Prints the remote address for diagnostics.
 * Returns the accepted fd on success, -1 on accept() failure.
 */
static int
acceptOne(int listenFd, int port)
{
    int                 connFd;
    struct sockaddr_in  peerAddr;
    socklen_t           peerLen;
    char                peerStr[INET_ADDRSTRLEN];

    peerLen = sizeof(peerAddr);
    connFd = -1;

    if( (connFd = accept(listenFd,
                         (struct sockaddr *)&peerAddr,
                         &peerLen)) < 0 )
    {
        fprintf(stderr, "%s: accept() on port %d failed: %s\n",
                progName, port, strerror(errno));
        return(-1);
    }

    inet_ntop(AF_INET, &(peerAddr.sin_addr), peerStr, sizeof(peerStr));
    fprintf(stdout, "%s: connection on port %d from %s:%d\n",
            progName, port, peerStr, ntohs(peerAddr.sin_port));
    fflush(stdout);

    return(connFd);
}

/*
 * connectTo -- create a TCP socket and connect to host:port.
 * host must be a dotted-decimal IPv4 string.
 * Returns the connected fd on success, -1 on any failure (error printed).
 */
static int
connectTo(const char *hostP, int port)
{
    int                 fd;
    struct sockaddr_in  addr;

    fd = -1;

    if( (fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        fprintf(stderr, "%s: socket() failed: %s\n", progName, strerror(errno));
        return(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if( inet_pton(AF_INET, hostP, &(addr.sin_addr)) <= 0 )
    {
        fprintf(stderr, "%s: inet_pton('%s') failed\n", progName, hostP);
        close(fd);
        return(-1);
    }

    if( connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 )
    {
        fprintf(stderr, "%s: connect() to %s:%d failed: %s\n",
                progName, hostP, port, strerror(errno));
        close(fd);
        return(-1);
    }

    fprintf(stdout, "%s: connected to %s:%d\n", progName, hostP, port);
    fflush(stdout);

    return(fd);
}

/*
 * echoLoop -- echo bytes from connFd back to connFd until the remote side
 * closes (recv returns 0) or a socket error occurs.
 * Returns 0 on clean close, 1 on socket error.
 */
static int
echoLoop(int connFd, int port)
{
    char    buf[BUF_SIZE];
    int     nRecv;
    int     nSent;
    int     nToSend;
    int     offset;

    for(;;)
    {
        nRecv = (int)recv(connFd, buf, sizeof(buf), 0);

        if( nRecv == 0 )
        {
            fprintf(stdout, "%s: port %d connection closed by remote\n",
                    progName, port);
            fflush(stdout);
            return(0);
        }

        if( nRecv < 0 )
        {
            fprintf(stderr, "%s: recv() on port %d failed: %s\n",
                    progName, port, strerror(errno));
            return(1);
        }

        fprintf(stdout, "%s: port %d echoing %d byte(s)\n",
                progName, port, nRecv);
        fflush(stdout);

        nToSend = nRecv;
        offset  = 0;

        while( nToSend > 0 )
        {
            nSent = (int)send(connFd,
                              (buf + offset),
                              (size_t)nToSend,
                              MSG_NOSIGNAL);

            if( nSent < 0 )
            {
                fprintf(stderr, "%s: send() on port %d failed: %s\n",
                        progName, port, strerror(errno));
                return(1);
            }

            offset  += nSent;
            nToSend -= nSent;
        }
    }
}

/* =========================================================================
 * Mode: echo-server <port>
 * ========================================================================= */

/*
 * modeEchoServer -- listen on port, accept one connection, echo all bytes
 * until the remote side closes.
 * Returns 0 on clean exit, 1 on error.
 */
static int
modeEchoServer(int port)
{
    int listenFd;
    int connFd;
    int result;

    listenFd = -1;
    connFd   = -1;

    fprintf(stdout, "%s: echo-server listening on port %d\n", progName, port);
    fflush(stdout);

    if( (listenFd = makeTcpListener(port)) < 0 )
    {
        return(1);
    }

    if( (connFd = acceptOne(listenFd, port)) < 0 )
    {
        close(listenFd);
        return(1);
    }

    /* Listening socket no longer needed once connection is accepted. */
    close(listenFd);
    listenFd = -1;

    result = echoLoop(connFd, port);

    close(connFd);
    fprintf(stdout, "%s: echo-server done\n", progName);
    return(result);
}

/* =========================================================================
 * Mode: send-verify-client <host> <port> <byte_hex>
 * ========================================================================= */

/*
 * modeSendVerifyClient -- connect to host:port, send one byte, receive one
 * byte, verify received == sent.
 * byteVal: the byte to send (0-255).
 * Returns 0 if received byte matches sent byte, 1 otherwise.
 */
static int
modeSendVerifyClient(const char *hostP, int port, int byteVal)
{
    int             fd;
    unsigned char   sendBuf[1];
    unsigned char   recvBuf[1];
    int             n;

    fd = -1;

    if( (fd = connectTo(hostP, port)) < 0 )
    {
        return(1);
    }

    sendBuf[0] = (unsigned char)(byteVal & 0xFF);
    n = (int)send(fd, sendBuf, 1, MSG_NOSIGNAL);

    if( n != 1 )
    {
        fprintf(stderr, "%s: send() returned %d: %s\n",
                progName, n, strerror(errno));
        close(fd);
        return(1);
    }

    fprintf(stdout, "%s: sent 0x%02X\n", progName, (unsigned int)sendBuf[0]);
    fflush(stdout);

    n = (int)recv(fd, recvBuf, 1, 0);

    if( n != 1 )
    {
        fprintf(stderr, "%s: recv() returned %d: %s\n",
                progName, n, (n == 0) ? "connection closed" : strerror(errno));
        close(fd);
        return(1);
    }

    fprintf(stdout, "%s: received 0x%02X\n", progName, (unsigned int)recvBuf[0]);
    fflush(stdout);

    close(fd);

    if( recvBuf[0] != sendBuf[0] )
    {
        fprintf(stderr, "%s: MISMATCH -- sent 0x%02X but received 0x%02X\n",
                progName,
                (unsigned int)sendBuf[0],
                (unsigned int)recvBuf[0]);
        return(1);
    }

    fprintf(stdout, "%s: send-verify-client OK\n", progName);
    return(0);
}

/* =========================================================================
 * Mode: two-echo-servers <port1> <port2>
 * ========================================================================= */

/*
 * modeTwoEchoServers -- listen on both ports, accept one connection on each,
 * then echo bytes on both until both connections have closed.
 *
 * select(2) is used to multiplex the two listening sockets without threads.
 * Once both connections are accepted, the two echo loops also run via select
 * so neither blocks the other.
 *
 * Returns 0 if both connections closed cleanly, 1 on any error.
 */
static int
modeTwoEchoServers(int port1, int port2)
{
    int         listenFd1;
    int         listenFd2;
    int         connFd1;
    int         connFd2;
    int         nfds;
    int         n;
    int         i;
    fd_set      readSet;
    char        buf[BUF_SIZE];
    int         nSent;
    int         nToSend;
    int         offset;
    /* Track which connections are still alive. */
    int         alive1;
    int         alive2;

    listenFd1 = -1;
    listenFd2 = -1;
    connFd1   = -1;
    connFd2   = -1;
    alive1    = 0;
    alive2    = 0;

    fprintf(stdout, "%s: two-echo-servers on ports %d and %d\n",
            progName, port1, port2);
    fflush(stdout);

    if( (listenFd1 = makeTcpListener(port1)) < 0 )
    {
        return(1);
    }

    if( (listenFd2 = makeTcpListener(port2)) < 0 )
    {
        close(listenFd1);
        return(1);
    }

    /* Wait for a connection on each listener.  Use select so we don't
     * block forever on one port if the PDP-1 opens them in a fixed order. */
    while( (connFd1 < 0) || (connFd2 < 0) )
    {
        FD_ZERO(&readSet);
        nfds = 0;

        if( connFd1 < 0 )
        {
            FD_SET(listenFd1, &readSet);
            if( listenFd1 > nfds ) { nfds = listenFd1; }
        }

        if( connFd2 < 0 )
        {
            FD_SET(listenFd2, &readSet);
            if( listenFd2 > nfds ) { nfds = listenFd2; }
        }

        if( select((nfds + 1), &readSet, NULL, NULL, NULL) < 0 )
        {
            fprintf(stderr, "%s: select() failed: %s\n",
                    progName, strerror(errno));
            close(listenFd1);
            close(listenFd2);
            return(1);
        }

        if( (connFd1 < 0) && FD_ISSET(listenFd1, &readSet) )
        {
            connFd1 = acceptOne(listenFd1, port1);
            if( connFd1 < 0 )
            {
                close(listenFd1);
                close(listenFd2);
                return(1);
            }
            alive1 = 1;
        }

        if( (connFd2 < 0) && FD_ISSET(listenFd2, &readSet) )
        {
            connFd2 = acceptOne(listenFd2, port2);
            if( connFd2 < 0 )
            {
                close(listenFd1);
                close(listenFd2);
                if( connFd1 >= 0 ) { close(connFd1); }
                return(1);
            }
            alive2 = 1;
        }
    }

    close(listenFd1);
    listenFd1 = -1;
    close(listenFd2);
    listenFd2 = -1;

    /* Echo loop: run until both connections have closed. */
    while( alive1 || alive2 )
    {
        FD_ZERO(&readSet);
        nfds = 0;

        if( alive1 )
        {
            FD_SET(connFd1, &readSet);
            if( connFd1 > nfds ) { nfds = connFd1; }
        }

        if( alive2 )
        {
            FD_SET(connFd2, &readSet);
            if( connFd2 > nfds ) { nfds = connFd2; }
        }

        if( select((nfds + 1), &readSet, NULL, NULL, NULL) < 0 )
        {
            fprintf(stderr, "%s: select() in echo loop failed: %s\n",
                    progName, strerror(errno));
            break;
        }

        /* Process each ready fd. */
        for( i = 0; i < 2; i++ )
        {
            int fd   = (i == 0) ? connFd1 : connFd2;
            int port = (i == 0) ? port1   : port2;
            int *aliveP = (i == 0) ? &alive1 : &alive2;

            if( !(*aliveP) || !FD_ISSET(fd, &readSet) )
            {
                continue;
            }

            n = (int)recv(fd, buf, sizeof(buf), 0);

            if( n == 0 )
            {
                fprintf(stdout, "%s: port %d closed by remote\n",
                        progName, port);
                fflush(stdout);
                close(fd);
                *aliveP = 0;
                continue;
            }

            if( n < 0 )
            {
                fprintf(stderr, "%s: recv() on port %d: %s\n",
                        progName, port, strerror(errno));
                close(fd);
                *aliveP = 0;
                continue;
            }

            fprintf(stdout, "%s: port %d echoing %d byte(s)\n",
                    progName, port, n);
            fflush(stdout);

            nToSend = n;
            offset  = 0;

            while( nToSend > 0 )
            {
                nSent = (int)send(fd,
                                  (buf + offset),
                                  (size_t)nToSend,
                                  MSG_NOSIGNAL);

                if( nSent < 0 )
                {
                    fprintf(stderr, "%s: send() on port %d: %s\n",
                            progName, port, strerror(errno));
                    close(fd);
                    *aliveP = 0;
                    nToSend = 0;
                }
                else
                {
                    offset  += nSent;
                    nToSend -= nSent;
                }
            }
        }
    }

    fprintf(stdout, "%s: two-echo-servers done\n", progName);
    return(0);
}

/* =========================================================================
 * Mode: delay-connect <host> <port> <delay_ms>
 * ========================================================================= */

/*
 * modeDelayConnect -- sleep delay_ms ms then connect to host:port, hold
 * the connection for HOLD_MS ms (giving the PDP-1 time to observe it),
 * then close.
 * Returns 0 on success, 1 on error.
 */
static int
modeDelayConnect(const char *hostP, int port, int delayMs)
{
    int fd;

    fd = -1;

    fprintf(stdout, "%s: delay-connect: sleeping %d ms before connecting to %s:%d\n",
            progName, delayMs, hostP, port);
    fflush(stdout);

    sleepMs(delayMs);

    if( (fd = connectTo(hostP, port)) < 0 )
    {
        return(1);
    }

    fprintf(stdout, "%s: delay-connect: holding for %d ms\n",
            progName, HOLD_MS);
    fflush(stdout);

    sleepMs(HOLD_MS);

    close(fd);
    fprintf(stdout, "%s: delay-connect: closed\n", progName);
    return(0);
}

/* =========================================================================
 * Mode: reconnect-client <host> <port>
 * ========================================================================= */

/*
 * doOneRoundTrip -- connect to host:port, send sendByte, receive one byte,
 * verify received == sendByte, close.
 * Returns 0 on success, 1 on any mismatch or socket error.
 */
static int
doOneRoundTrip(const char *hostP, int port, unsigned char sendByte, int tripNum)
{
    int             fd;
    unsigned char   recvBuf[1];
    unsigned char   sendBuf[1];
    int             n;

    fd = -1;
    sendBuf[0] = sendByte;

    fprintf(stdout, "%s: reconnect-client: round trip %d, send 0x%02X\n",
            progName, tripNum, (unsigned int)sendByte);
    fflush(stdout);

    if( (fd = connectTo(hostP, port)) < 0 )
    {
        return(1);
    }

    n = (int)send(fd, sendBuf, 1, MSG_NOSIGNAL);

    if( n != 1 )
    {
        fprintf(stderr, "%s: round trip %d: send() returned %d\n",
                progName, tripNum, n);
        close(fd);
        return(1);
    }

    n = (int)recv(fd, recvBuf, 1, 0);

    if( n != 1 )
    {
        fprintf(stderr, "%s: round trip %d: recv() returned %d: %s\n",
                progName, tripNum, n,
                (n == 0) ? "connection closed" : strerror(errno));
        close(fd);
        return(1);
    }

    fprintf(stdout, "%s: round trip %d: received 0x%02X\n",
            progName, tripNum, (unsigned int)recvBuf[0]);
    fflush(stdout);

    close(fd);

    if( recvBuf[0] != sendBuf[0] )
    {
        fprintf(stderr, "%s: round trip %d: MISMATCH -- sent 0x%02X got 0x%02X\n",
                progName, tripNum,
                (unsigned int)sendBuf[0],
                (unsigned int)recvBuf[0]);
        return(1);
    }

    fprintf(stdout, "%s: round trip %d OK\n", progName, tripNum);
    return(0);
}

/*
 * modeReconnectClient -- two sequential round trips separated by a 500 ms
 * pause to allow the PDP-1 to issue SCBREBIND between them.
 * Returns 0 if both round trips verified, 1 otherwise.
 */
static int
modeReconnectClient(const char *hostP, int port)
{
    int result;

    result = 0;

    if( doOneRoundTrip(hostP, port, 0x41, 1) != 0 )
    {
        result = 1;
    }

    /* Pause to give the PDP-1 time to detect the close and issue SCBREBIND. */
    fprintf(stdout, "%s: reconnect-client: pausing 500 ms before reconnect\n",
            progName);
    fflush(stdout);
    sleepMs(500);

    if( doOneRoundTrip(hostP, port, 0x42, 2) != 0 )
    {
        result = 1;
    }

    if( result == 0 )
    {
        fprintf(stdout, "%s: reconnect-client: both round trips OK\n", progName);
    }
    else
    {
        fprintf(stderr, "%s: reconnect-client: one or more round trips FAILED\n",
                progName);
    }

    return(result);
}

/* =========================================================================
 * Mode: interrupt-client <host> <port>
 * ========================================================================= */

/*
 * modeInterruptClient -- connect, send 0x41 to trigger IOR interrupt on
 * the PDP-1 server, then wait up to INTERRUPT_TIMEOUT_S seconds for the
 * server to close the connection.
 * Returns 0 on clean server-side close, 1 on timeout or error.
 */
static int
modeInterruptClient(const char *hostP, int port)
{
    int             fd;
    unsigned char   sendBuf[1];
    unsigned char   recvBuf[1];
    int             n;
    fd_set          readSet;
    struct timeval  tv;

    fd = -1;
    sendBuf[0] = 0x41;

    fprintf(stdout, "%s: interrupt-client: connecting to %s:%d\n",
            progName, hostP, port);
    fflush(stdout);

    if( (fd = connectTo(hostP, port)) < 0 )
    {
        return(1);
    }

    n = (int)send(fd, sendBuf, 1, MSG_NOSIGNAL);

    if( n != 1 )
    {
        fprintf(stderr, "%s: interrupt-client: send() returned %d\n",
                progName, n);
        close(fd);
        return(1);
    }

    fprintf(stdout, "%s: interrupt-client: sent 0x41, waiting for server close\n",
            progName);
    fflush(stdout);

    /* Wait up to INTERRUPT_TIMEOUT_S for the server to close. */
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    tv.tv_sec  = INTERRUPT_TIMEOUT_S;
    tv.tv_usec = 0;

    n = select((fd + 1), &readSet, NULL, NULL, &tv);

    if( n == 0 )
    {
        fprintf(stderr, "%s: interrupt-client: timed out waiting for server close\n",
                progName);
        close(fd);
        return(1);
    }

    if( n < 0 )
    {
        fprintf(stderr, "%s: interrupt-client: select() failed: %s\n",
                progName, strerror(errno));
        close(fd);
        return(1);
    }

    /* Data or close available -- drain to find the close. */
    n = (int)recv(fd, recvBuf, sizeof(recvBuf), 0);

    if( n == 0 )
    {
        fprintf(stdout, "%s: interrupt-client: server closed connection cleanly\n",
                progName);
        close(fd);
        return(0);
    }

    fprintf(stderr, "%s: interrupt-client: expected server close but recv returned %d\n",
            progName, n);
    close(fd);
    return(1);
}

/* =========================================================================
 * Usage
 * ========================================================================= */

/*
 * printUsage -- print mode summary to stderr.
 */
static void
printUsage(void)
{
    fprintf(stderr,
        "Usage: %s <mode> [args...]\n"
        "\n"
        "Modes:\n"
        "  echo-server <port>\n"
        "  send-verify-client <host> <port> <byte_hex>\n"
        "  two-echo-servers <port1> <port2>\n"
        "  delay-connect <host> <port> <delay_ms>\n"
        "  reconnect-client <host> <port>\n"
        "  interrupt-client <host> <port>\n",
        progName);
}

/* =========================================================================
 * main()
 *
 * Preconditions : argc >= 2.
 * Returns       : 0 on success, 1 on error.
 * ========================================================================= */
int
main(int argc, char *argv[])
{
    /* -- local variable declarations -- */
    const char  *modeP;    /* argv[1], the mode string */
    int          port;     /* parsed port number(s) */
    int          port2;    /* second port for two-echo-servers */
    int          delayMs;  /* parsed delay_ms */
    int          byteVal;  /* parsed byte value (hex) */

    progName = argv[0];

    if( argc < 2 )
    {
        printUsage();
        return(1);
    }

    modeP = argv[1];

    /* ---- echo-server <port> ---- */
    if( strcmp(modeP, "echo-server") == 0 )
    {
        if( argc != 3 )
        {
            fprintf(stderr, "%s: echo-server requires <port>\n", progName);
            return(1);
        }

        port = atoi(argv[2]);
        return(modeEchoServer(port));
    }

    /* ---- send-verify-client <host> <port> <byte_hex> ---- */
    if( strcmp(modeP, "send-verify-client") == 0 )
    {
        if( argc != 5 )
        {
            fprintf(stderr, "%s: send-verify-client requires <host> <port> <byte_hex>\n",
                    progName);
            return(1);
        }

        port    = atoi(argv[3]);
        byteVal = (int)strtol(argv[4], NULL, 16);
        return(modeSendVerifyClient(argv[2], port, byteVal));
    }

    /* ---- two-echo-servers <port1> <port2> ---- */
    if( strcmp(modeP, "two-echo-servers") == 0 )
    {
        if( argc != 4 )
        {
            fprintf(stderr, "%s: two-echo-servers requires <port1> <port2>\n",
                    progName);
            return(1);
        }

        port  = atoi(argv[2]);
        port2 = atoi(argv[3]);
        return(modeTwoEchoServers(port, port2));
    }

    /* ---- delay-connect <host> <port> <delay_ms> ---- */
    if( strcmp(modeP, "delay-connect") == 0 )
    {
        if( argc != 5 )
        {
            fprintf(stderr, "%s: delay-connect requires <host> <port> <delay_ms>\n",
                    progName);
            return(1);
        }

        port    = atoi(argv[3]);
        delayMs = atoi(argv[4]);
        return(modeDelayConnect(argv[2], port, delayMs));
    }

    /* ---- reconnect-client <host> <port> ---- */
    if( strcmp(modeP, "reconnect-client") == 0 )
    {
        if( argc != 4 )
        {
            fprintf(stderr, "%s: reconnect-client requires <host> <port>\n",
                    progName);
            return(1);
        }

        port = atoi(argv[3]);
        return(modeReconnectClient(argv[2], port));
    }

    /* ---- interrupt-client <host> <port> ---- */
    if( strcmp(modeP, "interrupt-client") == 0 )
    {
        if( argc != 4 )
        {
            fprintf(stderr, "%s: interrupt-client requires <host> <port>\n",
                    progName);
            return(1);
        }

        port = atoi(argv[3]);
        return(modeInterruptClient(argv[2], port));
    }

    fprintf(stderr, "%s: unknown mode '%s'\n", progName, modeP);
    printUsage();
    return(1);
}
