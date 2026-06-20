/*
 * dcsecho.c -- Trivial TCP echo server for DCS2 client-mode testing.
 *
 * Purpose:
 *   Companion to dcstest.am1.  Listens on TCP port 2022 (or an optional
 *   command-line override), accepts one incoming connection, and echoes
 *   every received byte back to the sender.  When the remote side closes
 *   the connection the server exits cleanly.
 *
 *   This exercises the DCS2 client path (IOT 22, channel 0) in the
 *   PDP-1D emulator without requiring a second PDP-1 process or a real
 *   network peer.
 *
 * Build:
 *   gcc -Wall -Wextra -o dcsecho dcsecho.c
 *
 * Run:
 *   ./dcsecho            -- listens on default port 2022
 *   ./dcsecho 9000       -- listens on port 9000
 *
 * Architecture / execution model:
 *   Single-threaded, blocking I/O.  No concurrency is needed because only
 *   one connection is served.  The program loops recv() / send() until
 *   recv() returns 0 (clean close from PDP-1 side) or -1 (socket error),
 *   then exits.
 *
 * Dependencies:
 *   POSIX sockets (Linux / any POSIX system).
 *   No external libraries beyond the C standard library.
 *
 * Coding conventions follow the project's CLAUDE.md standards:
 *   - 4-space indent, no tabs
 *   - mandatory parentheses for arithmetic, logical, bitwise operations
 *   - explicit braces on all control constructs, bodies on separate lines
 *   - camelCase naming, trailing P/ptr for pointer variables
 *   - global variables at top of file, locals at top of function
 *   - declarations separated from initializations in most cases
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/* Default listening port -- must match the port in dcstest.am1's ctl block. */
#define DEFAULT_PORT    2022

/* Size of the echo I/O buffer.  One byte would work; 256 reduces syscall
 * overhead if the OS coalesces multiple bytes into one recv. */
#define BUF_SIZE        256

/* -------------------------------------------------------------------------
 * Global variables
 * ------------------------------------------------------------------------- */

/* File descriptors for the listening socket and the accepted connection.
 * Kept global so they are accessible from a SIGINT handler if one is added. */
static int listenFd;
static int clientFd;

/* =========================================================================
 * main()
 *
 * Preconditions : argc is 1 or 2; if 2, argv[1] is a decimal port number.
 * Postconditions: returns 0 on clean close from client, 1 on any error.
 * Arguments     : argc, argv  -- standard main arguments.
 * Returns       : 0 on success, 1 on error.
 * Edge cases    : port out of range, bind failure (port in use), accept
 *                 failure, and recv/send errors are all reported and cause
 *                 immediate exit with status 1.
 * ========================================================================= */
int main(int argc, char *argv[])
{
    /* -- local variable declarations -- */
    int                 port;           /* TCP port to listen on */
    int                 opt;            /* setsockopt value */
    int                 nRecv;          /* bytes returned by recv() */
    int                 nSent;          /* bytes returned by send() */
    int                 nToSend;        /* bytes remaining to send in current buf */
    int                 offset;         /* offset into buf for partial sends */
    char                buf[BUF_SIZE];  /* shared recv/send buffer */
    struct sockaddr_in  serverAddr;     /* our listening address */
    struct sockaddr_in  clientAddr;     /* remote peer address (from accept) */
    socklen_t           clientAddrLen;  /* length passed to accept() */
    char                addrStr[INET_ADDRSTRLEN]; /* printable peer IP */

    /* -- parse optional port argument -- */
    port = DEFAULT_PORT;

    if(argc == 2)
    {
        port = atoi(argv[1]);

        if( (port <= 0) || (port > 65535) )
        {
            fprintf(stderr, "dcsecho: invalid port '%s' (must be 1-65535)\n",
                    argv[1]);
            return(1);
        }
    }
    else if(argc != 1)
    {
        fprintf(stderr, "Usage: dcsecho [port]\n");
        return(1);
    }

    /* -- create the listening socket -- */
    listenFd = -1;
    clientFd = -1;

    if( (listenFd = socket(AF_INET, SOCK_STREAM, 0)) < 0 )
    {
        fprintf(stderr, "dcsecho: socket() failed: %s\n", strerror(errno));
        return(1);
    }

    /* SO_REUSEADDR lets us restart the server immediately after a previous
     * instance exits, without waiting for the TIME_WAIT state to clear. */
    opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* -- bind to the chosen port on all interfaces -- */
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port        = htons((uint16_t)port);

    if( bind(listenFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0 )
    {
        fprintf(stderr, "dcsecho: bind() on port %d failed: %s\n",
                port, strerror(errno));
        close(listenFd);
        return(1);
    }

    /* -- start listening -- */
    if( listen(listenFd, 1) < 0 )
    {
        fprintf(stderr, "dcsecho: listen() failed: %s\n", strerror(errno));
        close(listenFd);
        return(1);
    }

    fprintf(stdout, "dcsecho: listening on port %d ...\n", port);
    fflush(stdout);

    /* -- accept one connection -- */
    clientAddrLen = sizeof(clientAddr);

    if( (clientFd = accept(listenFd,
                           (struct sockaddr *)&clientAddr,
                           &clientAddrLen)) < 0 )
    {
        fprintf(stderr, "dcsecho: accept() failed: %s\n", strerror(errno));
        close(listenFd);
        return(1);
    }

    /* The listening socket is no longer needed once the connection is accepted.
     * Closing it now prevents a second connection attempt from blocking in
     * accept() and keeps the server strictly single-connection. */
    close(listenFd);
    listenFd = -1;

    /* Print the peer's IP address for confirmation. */
    inet_ntop(AF_INET, &(clientAddr.sin_addr), addrStr, sizeof(addrStr));
    fprintf(stdout, "dcsecho: connection from %s:%d\n",
            addrStr, ntohs(clientAddr.sin_port));
    fflush(stdout);

    /* -- echo loop: recv -> send until remote close or error -- */
    for(;;)
    {
        /* recv() returns:
         *   > 0  : number of bytes received, stored in buf
         *   = 0  : remote side performed a clean shutdown (FIN received)
         *   < 0  : socket error; errno is set                             */
        nRecv = (int)recv(clientFd, buf, sizeof(buf), 0);

        if(nRecv == 0)
        {
            /* Clean close from the PDP-1 side.  This is the normal exit path
             * after the DCS2 program calls sbcrst or scb-close. */
            fprintf(stdout, "dcsecho: connection closed by remote end\n");
            break;
        }

        if(nRecv < 0)
        {
            fprintf(stderr, "dcsecho: recv() failed: %s\n", strerror(errno));
            close(clientFd);
            return(1);
        }

        /* send() may write fewer bytes than requested on a non-blocking socket
         * or under memory pressure.  Although this socket is blocking, we use
         * a send loop for correctness: partial sends are not an error, they
         * just mean we must retry with the remaining bytes. */
        nToSend = nRecv;
        offset  = 0;

        while(nToSend > 0)
        {
            nSent = (int)send(clientFd,
                              (buf + offset),
                              (size_t)nToSend,
                              MSG_NOSIGNAL);  /* suppress SIGPIPE on broken conn */

            if(nSent < 0)
            {
                fprintf(stderr, "dcsecho: send() failed: %s\n",
                        strerror(errno));
                close(clientFd);
                return(1);
            }

            /* Advance past the bytes already sent. */
            offset  += nSent;
            nToSend -= nSent;
        }

        /* Optional: log each echoed byte for debugging.  At high data rates
         * this printf will slow things down; comment it out if needed. */
        fprintf(stdout, "dcsecho: echoed %d byte(s)\n", nRecv);
        fflush(stdout);
    }

    /* -- clean shutdown -- */
    close(clientFd);
    fprintf(stdout, "dcsecho: done\n");
    return(0);
}
