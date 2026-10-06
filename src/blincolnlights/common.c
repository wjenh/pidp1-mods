/* Support routines used by many programs. */
// 8-Apr-2026 wje initial cleanup */
// 14-Jul-2026 wje more cleaning, no warning now
// 4-Oct-2026 Claude split() no longer reads past the end of its string
// 05-Oct-2026 Claude serveN() retries ports that cannot listen, gives a threaded port's
//    connections their own threads, dial() times out, the segment fds are closed after mmap()
// 06-Oct-2026 Claude dialQuietly(), dial() without its report, for a caller that retries
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include <poll.h>

#include "common.h"

// Room for a few connections arriving together on one port.
#define LISTENBACKLOG 8
// How long dial() waits for each address to answer.
#define DIALTIMEOUTMS 5000
// How often serveN() tries a port that could not listen again.
#define LISTENRETRYMS 1000

struct HandlerCall
{
    void (*handle)(int fd, void *arg);
    int fd;
    void *arg;
};

void
panic( const char *fmt, ... )
{
va_list ap;

    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    fprintf( stderr, "\n" );
    va_end( ap );
    exit( 1 );
}

// Returns 1 if fd has input waiting, else 0 (also for a negative fd).
int
hasinput( int fd )
{
struct pollfd pfd;

    if( fd < 0 )
    {
        return( 0 );
    }

    // poll(), not select(): select() is undefined for an fd at or above FD_SETSIZE.
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return( (poll(&pfd, 1, 0) > 0) && ((pfd.revents & POLLIN) != 0) );
}

// Opens a TCP socket listening on port on every interface, printing nothing, so a caller that
// retries can report once. Returns the socket, or -1 with errno set.
static int
listenQuietly( int port )
{
int x;
int fd;
int err;
struct sockaddr_in server;

    fd = socket( AF_INET, SOCK_STREAM, 0 );

    if( fd < 0 )
    {
        return( -1 );
    }

    x = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (void *) &x, sizeof x );

    memset( &server, 0, sizeof(server) );
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons( port );

    if( (bind(fd, (struct sockaddr *) &server, sizeof(server)) < 0) || (listen(fd, LISTENBACKLOG) < 0) )
    {
        err = errno;
        close( fd );
        errno = err;
        return( -1 );
    }

    return( fd );
}

// Opens a TCP socket listening on port. Returns the socket, or -1 after printing why.
int
socketlisten( int port )
{
int fd;

    fd = listenQuietly( port );

    if( fd < 0 )
    {
        fprintf( stderr, "error: can't listen on port %d: %s\n", port, strerror(errno) );
    }

    return( fd );
}

// Connects fd to addrP, waiting at most timeoutMs. Returns 0 with fd connected and blocking
// again, or -1 with errno set (ETIMEDOUT for no answer in time).
static int
connectWithin( int fd, const struct sockaddr *addrP, socklen_t len, int timeoutMs )
{
int flags;
int ret;
int err;
socklen_t errLen;
struct pollfd pfd;

    flags = fcntl( fd, F_GETFL, 0 );

    if( (flags < 0) || (fcntl(fd, F_SETFL, (flags | O_NONBLOCK)) < 0) )
    {
        return( -1 );
    }

    ret = connect( fd, addrP, len );

    if( (ret < 0) && (errno == EINPROGRESS) )
    {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        do
        {
            ret = poll( &pfd, 1, timeoutMs );
        }
        while( (ret < 0) && (errno == EINTR) );

        if( ret == 0 )
        {
            errno = ETIMEDOUT;
            return( -1 );
        }

        if( ret < 0 )
        {
            return( -1 );
        }

        err = 0;
        errLen = sizeof( err );

        if( getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0 )
        {
            return( -1 );
        }

        if( err != 0 )
        {
            errno = err;
            return( -1 );
        }

        ret = 0;
    }

    if( ret < 0 )
    {
        return( -1 );
    }

    // Callers read and write the socket as a blocking one.
    if( fcntl(fd, F_SETFL, flags) < 0 )
    {
        return( -1 );
    }

    return( 0 );
}

// Connects to host:port, trying each address the name gives for at most DIALTIMEOUTMS,
// printing nothing, so a caller that retries can report once. The name lookup itself has no
// timeout of its own.
// Returns the connected socket, or -1 with errno set and why it failed in whyP (whyLen bytes).
int
dialQuietly( const char *host, int port, char *whyP, size_t whyLen )
{
int sockfd;
int ret;
int err;
struct addrinfo *result, *rp, hints;
char portstr[32];

    memset( &hints, 0, sizeof(hints) );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf( portstr, 32, "%d", port );

    ret = getaddrinfo( host, portstr, &hints, &result );

    if( ret != 0 )
    {
        snprintf( whyP, whyLen, "can't find %s: %s", host, gai_strerror(ret) );
        errno = EHOSTUNREACH;
        return( -1 );
    }

    err = ECONNREFUSED;

    for( rp = result; rp; rp = rp->ai_next )
    {
        sockfd = socket( rp->ai_family, rp->ai_socktype, rp->ai_protocol );

        if( sockfd < 0 )
        {
            err = errno;
            continue;
        }

        if( connectWithin(sockfd, rp->ai_addr, rp->ai_addrlen, DIALTIMEOUTMS) == 0 )
        {
            goto win;
        }

        err = errno;
        close( sockfd );
    }

    freeaddrinfo( result );
    snprintf( whyP, whyLen, "can't connect to %s port %d: %s", host, port, strerror(err) );
    errno = err;
    return( -1 );

win:
    freeaddrinfo( result );
    return( sockfd );
}

// dialQuietly(), printing why it failed.
// Returns the connected socket, or -1 with errno set.
int
dial( const char *host, int port )
{
int fd;
int err;
char why[256];

    if( (fd = dialQuietly(host, port, why, sizeof(why))) < 0 )
    {
        err = errno;
        fprintf( stderr, "error: %s\n", why );
        errno = err;
    }

    return( fd );
}

// Listens on port and accepts one connection, then stops listening. Returns the connection,
// or -1 with errno set; prints nothing, so a caller that retries can report once.
int
serve1( int port )
{
int sockfd, confd;
int err;
socklen_t len;
struct sockaddr_in client;

    sockfd = listenQuietly( port );

    if( sockfd < 0 )
    {
        return( -1 );
    }

    len = sizeof( client );
    confd = accept( sockfd, (struct sockaddr *)&client, &len );
    err = errno;
    close( sockfd );

    if( confd < 0 )
    {
        errno = err;
    }

    return( confd );
}

// Runs one threaded port's handler for its connection, then frees the call.
static void *
handlerThread( void *argP )
{
struct HandlerCall *callP;

    callP = (struct HandlerCall *)argP;
    callP->handle( callP->fd, callP->arg );
    free( callP );
    return( nil );
}

// Hands confd to portP's handler: inline, or on a detached thread of its own when the port is
// threaded, so a handler that keeps its connection cannot hold the other ports.
static void
dispatch( struct PortHandler *portP, int confd, void *arg )
{
pthread_t th;
pthread_attr_t attr;
struct HandlerCall *callP;

    if( !portP->threaded )
    {
        portP->handle( confd, arg );
        return;
    }

    callP = (struct HandlerCall *)malloc( sizeof(*callP) );

    if( callP == nil )
    {
        close( confd );
        return;
    }

    callP->handle = portP->handle;
    callP->fd = confd;
    callP->arg = arg;

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

    // With no thread for it, this client is refused rather than served inline.
    if( pthread_create(&th, &attr, handlerThread, callP) != 0 )
    {
        fprintf( stderr, "port %d: no thread for a connection, closed\n", portP->port );
        close( confd );
        free( callP );
    }

    pthread_attr_destroy( &attr );
}

// Listens on every port in ports and hands each connection to its port's handler; returns only
// if poll() fails. A port that cannot listen, at the start or later, is tried again every
// LISTENRETRYMS (another process may hold it for a while, a pdp1 still exiting, say); each
// port reports once when it fails and once when it recovers.
void
serveN( struct PortHandler *ports, int nports, void *arg )
{
int i;
int ret;
int missing;
int confd;
socklen_t len;
struct sockaddr_in client;
struct pollfd pfds[100];
char reported[100];

    if( nports > (int)nelem(pfds) )
    {
        nports = nelem( pfds );
    }

    // poll() skips a negative fd, so a port not listening stays in the table as -1.
    for( i = 0; i < nports; i++ )
    {
        pfds[i].fd = -1;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
        reported[i] = 0;
    }

    for( ;; )
    {
        missing = 0;

        for( i = 0; i < nports; i++ )
        {
            if( pfds[i].fd >= 0 )
            {
                continue;
            }

            pfds[i].fd = listenQuietly( ports[i].port );

            if( pfds[i].fd >= 0 )
            {
                if( reported[i] )
                {
                    fprintf( stderr, "port %d: listening\n", ports[i].port );
                    reported[i] = 0;
                }
            }
            else
            {
                if( !reported[i] )
                {
                    fprintf( stderr, "port %d: can't listen (%s), retrying\n", ports[i].port, strerror(errno) );
                    reported[i] = 1;
                }

                missing++;
            }
        }

        ret = poll( pfds, nports, ((missing > 0) ? LISTENRETRYMS : -1) );

        if( ret < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }

            break;
        }

        for( i = 0; i < nports; i++ )
        {
            if( pfds[i].revents & POLLIN )
            {
                len = sizeof( client );
                confd = accept( pfds[i].fd, (struct sockaddr *) &client, &len );

                if( confd >= 0 )
                {
                    dispatch( &ports[i], confd, arg );
                }
            }
            else if( pfds[i].revents )
            {
                // An error on a listener would wake every poll() from now on; reopen it instead.
                fprintf( stderr, "port %d: listener failed, reopening\n", ports[i].port );
                close( pfds[i].fd );
                pfds[i].fd = -1;
                reported[i] = 1;
            }

            pfds[i].revents = 0;
        }
    }

    for( i = 0; i < nports; i++ )
    {
        if( pfds[i].fd >= 0 )
        {
            close( pfds[i].fd );
        }
    }
}

void
nodelay( int fd )
{
int flag;

    flag = 1;
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag) );
}

void *
createseg( const char *name, size_t sz )
{
int fd;
void *p;
mode_t mask;

    mask = umask( 0 );
    fd = open( name, O_RDWR | O_CREAT, 0666 );
    umask( mask );

    /* if we try to open a /tmp file owned by another user */
    /* with O_CREAT, the above will fail (even for root). */
    /* so try again without O_CREAT */
    if( fd == -1 )
    {
        fd = open( name, O_RDWR );
    }

    if( fd == -1 )
    {
        fprintf( stderr, "couldn't open file %s\n", name );
        return( nil );
    }

    if( ftruncate( fd, sz ) < 0 )
    {
        // Segment couldn't be sized; mapping it anyway risks a short mapping
        // (SIGBUS on access past the actual file size).
        fprintf( stderr, "couldn't size file %s to %zu bytes\n", name, sz );
        close( fd );
        return( nil );
    }

    // The mapping keeps the file; the fd is not needed past mmap().
    p = mmap( nil, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    close( fd );

    if( p == MAP_FAILED )
    {
        fprintf( stderr, "couldn't mmap file\n" );
        return( nil );
    }

    return( p );
}

void *
attachseg( const char *name, size_t sz )
{
int fd;
void *p;

    fd = open( name, O_RDWR );

    if( fd == -1 )
    {
        fprintf( stderr, "couldn't open file %s\n", name );
        return( nil );
    }

    p = mmap( NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    close( fd );

    if( p == MAP_FAILED )
    {
        fprintf( stderr, "couldn't mmap file\n" );
        return( nil );
    }

    return( p );
}

static struct timespec starttime;
void
inittime( void )
{
    clock_gettime( CLOCK_MONOTONIC, &starttime );
}

u64
gettime( void )
{
struct timespec tm;
u64 t;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    tm.tv_sec -= starttime.tv_sec;
    t = tm.tv_nsec;
    t += ( u64 ) tm.tv_sec * 1000 * 1000 * 1000;
    return( t );
}

void
nsleep( u64 ns )
{
struct timespec tm;

    tm.tv_sec = ns / ( 1000 * 1000 * 1000 );
    tm.tv_nsec = ns % ( 1000 * 1000 * 1000 );
    nanosleep( &tm, nil );
}

static int
isdelim( char c )
{
    return( (c == '\0') || (strchr(" \t\n;'\"", c) != nil) );
}

// Breaks line into words at blanks and ';', honoring quotes and backslash escapes. Returns a
// malloc'd argv ending in nil; *pargc, if given, gets the word count.
char **
split( char *line, int *pargc )
{
int argc, n;
char **argv, *lp, delim;

    n = strlen( line ) + 1;

    /* just allocate enough */
    lp = (char *)malloc( 2 * n );
    argv = (char **)malloc( sizeof(char *) * n );
    argc = 0;

    for( ; *line; line++ )
    {
        while( isspace(*line) )
        {
            line++;
        }

        if( *line == '\0' )
        {
            break;
        }

        argv[argc++] = lp;

        if( *line == '"' || *line == '\'' )
        {
            delim = *line++;

            while( *line && (*line != delim) )
            {
                // A trailing backslash has nothing to escape and is dropped.
                if( *line == '\\' )
                {
                    line++;

                    if( *line == '\0' )
                    {
                        break;
                    }
                }

                *lp++ = *line++;
            }
        }
        else
        {
            while( !isdelim(*line) )
            {
                if( *line == '\\' )
                {
                    line++;

                    if( *line == '\0' )
                    {
                        break;
                    }
                }

                *lp++ = *line++;
            }
        }

        *lp++ = '\0';

        // A word that ran to the end of the string ends the scan; the loop's line++ would step
        // over the NUL into whatever follows it.
        if( *line == '\0' )
        {
            break;
        }
    }

    if( pargc )
    {
        *pargc = argc;
    }

    argv[argc++] = nil;

    return( argv );
}
