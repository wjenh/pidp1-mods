// Client side of the emulator's debugger link: connection, handshake, framing, one request in
// flight, and a typed wrapper per request.
// The protocol is in src/blincolnlights/pdp1/ad1proto.h and the server is ad1server.c.
// Every frame is built and read field by field, so nothing here depends on the emulator's PDP1 struct.
//
// 20-Sep-2026 Claude: written to replace the shared-memory link.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ad1link.h"

#define CONNECT_TIMEOUT_MS 5000
#define DEFAULT_REPLY_TIMEOUT_MS 10000
#define FRAME_TIMEOUT_MS 10000          // once a frame has started, the rest must follow
#define STEP_MS_EACH 2                  // extra reply time allowed per step

static char linkError[256];

static void
setError(const char *fmtP, ...) __attribute__((format(printf, 1, 2)));

static void
setError(const char *fmtP, ...)
{
va_list ap;

    va_start(ap, fmtP);
    vsnprintf(linkError, sizeof(linkError), fmtP, ap);
    va_end(ap);
}

const char *
ad1LinkError(void)
{
    return( linkError );
}

static uint32_t
rd32(const uint8_t *pP)
{
    return( (uint32_t)pP[0] | ((uint32_t)pP[1] << 8) | ((uint32_t)pP[2] << 16) | ((uint32_t)pP[3] << 24) );
}

static uint16_t
rd16(const uint8_t *pP)
{
    return( (uint16_t)(pP[0] | (pP[1] << 8)) );
}

static void
wr32(uint8_t *pP, uint32_t v)
{
    pP[0] = (uint8_t)v;
    pP[1] = (uint8_t)(v >> 8);
    pP[2] = (uint8_t)(v >> 16);
    pP[3] = (uint8_t)(v >> 24);
}

static void
wr16(uint8_t *pP, uint16_t v)
{
    pP[0] = (uint8_t)v;
    pP[1] = (uint8_t)(v >> 8);
}

static int
isLoopbackHost(const char *hostP)
{
    return( (strcmp(hostP, "localhost") == 0) || (strncmp(hostP, "127.", 4) == 0) || (strcmp(hostP, "::1") == 0) );
}

int
ad1LinkParseHost(const char *specP, char *hostP, size_t hostSize, int *portP)
{
const char *colonP;
size_t hostLen;
char *endP;
long port;

    if( (specP == NULL) || (*specP == '\0') )
    {
        specP = "localhost";
    }

    colonP = strrchr(specP, ':');
    hostLen = (colonP != NULL) ? (size_t)(colonP - specP) : strlen(specP);
    if( hostLen == 0 )
    {
        snprintf(hostP, hostSize, "localhost");
    }
    else if( hostLen >= hostSize )
    {
        return( AD1L_COMM );
    }
    else
    {
        memcpy(hostP, specP, hostLen);
        hostP[hostLen] = '\0';
    }

    if( (colonP == NULL) || (colonP[1] == '\0') )
    {
        *portP = isLoopbackHost(hostP) ? AD1P_DEFAULT_LOCAL_PORT : AD1P_DEFAULT_REMOTE_PORT;
        return( (colonP != NULL) ? AD1L_COMM : 0 );
    }

    port = strtol(colonP + 1, &endP, 10);
    if( (*endP != '\0') || (port < 1) || (port > 65535) )
    {
        return( AD1L_COMM );
    }

    *portP = (int)port;
    return( 0 );
}

// Milliseconds left until a monotonic deadline given in ms.
static int64_t
nowMs(void)
{
struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return( ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000) );
}

// Read exactly n bytes, waiting up to timeoutMs in all. Returns 0, 1 for a timeout, or
// AD1L_COMM for EOF or an error.
static int
readFull(int fd, uint8_t *bufP, uint32_t n, int timeoutMs)
{
struct pollfd pfd;
uint32_t have;
int64_t end;
int64_t left;
ssize_t got;

    end = nowMs() + timeoutMs;
    have = 0;
    while( have < n )
    {
        left = end - nowMs();
        if( left < 0 )
        {
            left = 0;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        got = poll(&pfd, 1, (int)left);
        if( got < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }

            return( AD1L_COMM );
        }

        if( got == 0 )
        {
            return( 1 );
        }

        got = recv(fd, bufP + have, n - have, 0);
        if( got == 0 )
        {
            return( AD1L_COMM );
        }

        if( got < 0 )
        {
            if( (errno == EINTR) || (errno == EAGAIN) )
            {
                continue;
            }

            return( AD1L_COMM );
        }

        have += (uint32_t)got;
    }

    return( 0 );
}

static int
writeFull(int fd, const uint8_t *bufP, uint32_t n)
{
ssize_t sent;
uint32_t have;

    have = 0;
    while( have < n )
    {
        sent = send(fd, bufP + have, n - have, MSG_NOSIGNAL);
        if( sent < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }

            return( AD1L_COMM );
        }

        have += (uint32_t)sent;
    }

    return( 0 );
}

static void
lost(Ad1Link *lp, const char *whyP)
{
    setError("the connection to the pdp-1 at %s:%d was lost (%s)", lp->host, lp->port, whyP);
    if( lp->fd >= 0 )
    {
        close(lp->fd);
        lp->fd = -1;
    }
}

// Queue an event, or count it if the queue is full.
static void
queueEvent(Ad1Link *lp, uint16_t type, const uint8_t *pP, uint32_t len)
{
Ad1Event *evP;
uint32_t i;

    if( lp->evCount >= AD1L_MAX_EVENTS )
    {
        ++lp->evDropped;
        return;
    }

    evP = &lp->events[(lp->evHead + lp->evCount) % AD1L_MAX_EVENTS];
    evP->type = type;
    evP->nWords = (len / 4 > 4) ? 4 : (len / 4);
    memset(evP->words, 0, sizeof(evP->words));
    for( i = 0; i < evP->nWords; ++i )
    {
        evP->words[i] = rd32(pP + (i * 4));
    }

    ++lp->evCount;
}

// Read one frame. Its payload is left in lp->replyP. Returns 0, 1 if nothing arrived within
// firstTimeoutMs, or AD1L_COMM.
static int
readFrame(Ad1Link *lp, int firstTimeoutMs, uint16_t *typeP, uint16_t *statusP, uint32_t *idP)
{
uint8_t hdr[AD1P_HEADER_SIZE];
uint32_t len;
int rc;

    rc = readFull(lp->fd, hdr, 1, firstTimeoutMs);
    if( rc != 0 )
    {
        if( rc == AD1L_COMM )
        {
            lost(lp, "the emulator closed it");
        }

        return( rc );
    }

    if( readFull(lp->fd, hdr + 1, AD1P_HEADER_SIZE - 1, FRAME_TIMEOUT_MS) != 0 )
    {
        lost(lp, "a reply was cut off");
        return( AD1L_COMM );
    }

    len = rd32(hdr);
    *typeP = rd16(hdr + 4);
    *statusP = rd16(hdr + 6);
    *idP = rd32(hdr + 8);
    if( len > AD1P_MAX_REPLY )
    {
        lost(lp, "the reply was larger than the protocol allows");
        return( AD1L_COMM );
    }

    if( (len > 0) && (readFull(lp->fd, lp->replyP, len, FRAME_TIMEOUT_MS) != 0) )
    {
        lost(lp, "a reply was cut off");
        return( AD1L_COMM );
    }

    lp->replyLen = len;
    return( 0 );
}

// Send a request and wait for its reply, queueing any events that arrive first. On success
// the server's status is returned and the payload is in lp->replyP and lp->replyLen.
static int
request(Ad1Link *lp, uint16_t type, const uint8_t *payloadP, uint32_t len, int timeoutMs)
{
uint8_t hdr[AD1P_HEADER_SIZE];
uint16_t gotType;
uint16_t gotStatus;
uint32_t gotId;
uint32_t id;
int64_t end;
int64_t left;
int rc;

    if( lp->fd < 0 )
    {
        setError("not connected to the pdp-1");
        return( AD1L_COMM );
    }

    if( (lp->maxRequest != 0) && (len > lp->maxRequest) )
    {
        setError("the request is larger than the emulator accepts (%u bytes)", lp->maxRequest);
        return( AD1L_COMM );
    }

    if( ++lp->nextId == 0 )
    {
        lp->nextId = 1;
    }

    id = lp->nextId;
    wr32(hdr, len);
    wr16(hdr + 4, type);
    wr16(hdr + 6, 0);
    wr32(hdr + 8, id);

    // One send for header and payload keeps a small request in one packet.
    if( len <= 256 )
    {
        uint8_t small[AD1P_HEADER_SIZE + 256];

        memcpy(small, hdr, AD1P_HEADER_SIZE);
        if( len > 0 )
        {
            memcpy(small + AD1P_HEADER_SIZE, payloadP, len);
        }

        rc = writeFull(lp->fd, small, AD1P_HEADER_SIZE + len);
    }
    else
    {
        rc = writeFull(lp->fd, hdr, AD1P_HEADER_SIZE);
        if( rc == 0 )
        {
            rc = writeFull(lp->fd, payloadP, len);
        }
    }

    if( rc != 0 )
    {
        lost(lp, "a request could not be sent");
        return( AD1L_COMM );
    }

    end = nowMs() + timeoutMs;
    for( ;; )
    {
        left = end - nowMs();
        if( left < 0 )
        {
            left = 0;
        }

        rc = readFrame(lp, (int)left, &gotType, &gotStatus, &gotId);
        if( rc == 1 )
        {
            lost(lp, "no reply came in time");
            return( AD1L_COMM );
        }

        if( rc != 0 )
        {
            return( AD1L_COMM );
        }

        if( gotType >= AD1P_EVENT_BASE )
        {
            queueEvent(lp, gotType, lp->replyP, lp->replyLen);
            continue;
        }

        // A refusal sent when the connection was accepted, before the HELLO was read, has no id.
        if( (gotType != (type | AD1P_REPLY)) || ((gotId != id) && (type != AD1P_HELLO)) )
        {
            lost(lp, "an unexpected reply arrived");
            return( AD1L_COMM );
        }

        return( gotStatus );
    }
}

void
ad1LinkClose(Ad1Link *lp)
{
    if( lp->fd >= 0 )
    {
        close(lp->fd);
    }

    free(lp->replyP);
    free(lp->recordsP);
    memset(lp, 0, sizeof(*lp));
    lp->fd = -1;
}

int
ad1LinkFd(const Ad1Link *lp)
{
    return( lp->fd );
}

// Connect with a limit on how long it may take. Returns a connected socket or -1 with errno set,
// or -2 with the name-lookup error text already in linkError.
static int
connectTo(const char *hostP, int port)
{
struct addrinfo hints;
struct addrinfo *resP;
struct addrinfo *aP;
char portText[16];
struct pollfd pfd;
int fd;
int rc;
int err;
socklen_t errLen;
int lastErrno;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portText, sizeof(portText), "%d", port);
    if( (rc = getaddrinfo(hostP, portText, &hints, &resP)) != 0 )
    {
        setError("cannot find the host '%s': %s", hostP, gai_strerror(rc));
        return( -2 );
    }

    lastErrno = ECONNREFUSED;
    for( aP = resP; aP != NULL; aP = aP->ai_next )
    {
        if( (fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol)) < 0 )
        {
            lastErrno = errno;
            continue;
        }

        rc = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, rc | O_NONBLOCK);
        rc = connect(fd, aP->ai_addr, aP->ai_addrlen);
        if( (rc < 0) && (errno == EINPROGRESS) )
        {
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if( poll(&pfd, 1, CONNECT_TIMEOUT_MS) <= 0 )
            {
                lastErrno = ETIMEDOUT;
                close(fd);
                continue;
            }

            err = 0;
            errLen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
            if( err != 0 )
            {
                lastErrno = err;
                close(fd);
                continue;
            }
        }
        else if( rc < 0 )
        {
            lastErrno = errno;
            close(fd);
            continue;
        }

        rc = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, rc & ~O_NONBLOCK);
        freeaddrinfo(resP);
        return( fd );
    }

    freeaddrinfo(resP);
    errno = lastErrno;
    return( -1 );
}

int
ad1LinkOpen(Ad1Link *lp, const char *specP, uint32_t subscribe, const char *clientNameP)
{
uint8_t payload[12 + 64];
uint32_t nameLen;
uint32_t buildLen;
int status;
int on;
int fd;

    memset(lp, 0, sizeof(*lp));
    lp->fd = -1;
    lp->replyTimeoutMs = DEFAULT_REPLY_TIMEOUT_MS;

    if( ad1LinkParseHost(specP, lp->host, sizeof(lp->host), &lp->port) != 0 )
    {
        setError("'%s' is not a host or host:port", (specP != NULL) ? specP : "");
        return( AD1L_COMM );
    }

    if( (lp->replyP = malloc(AD1P_MAX_REPLY)) == NULL )
    {
        setError("out of memory");
        return( AD1L_COMM );
    }

    if( (fd = connectTo(lp->host, lp->port)) < 0 )
    {
        if( fd == -1 )
        {
            setError("cannot connect to the pdp-1 at %s:%d: %s. Is it running, and is ad1port%s set in pidp1.config?",
                lp->host, lp->port, strerror(errno), isLoopbackHost(lp->host) ? "" : "/ad1remoteport");
        }

        free(lp->replyP);
        lp->replyP = NULL;
        return( AD1L_COMM );
    }

    on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    lp->fd = fd;

    nameLen = (clientNameP != NULL) ? (uint32_t)strlen(clientNameP) : 0;
    if( nameLen > 64 )
    {
        nameLen = 64;
    }

    wr32(payload, AD1P_VERSION);
    wr32(payload + 4, subscribe);
    wr32(payload + 8, nameLen);
    if( nameLen > 0 )
    {
        memcpy(payload + 12, clientNameP, nameLen);
    }

    status = request(lp, AD1P_HELLO, payload, 12 + nameLen, lp->replyTimeoutMs);
    if( status == AD1L_COMM )
    {
        ad1LinkClose(lp);
        return( AD1L_COMM );
    }

    if( status != AD1P_ST_OK )
    {
        setError("the pdp-1 at %s:%d refused the connection: %.*s", lp->host, lp->port,
            (int)lp->replyLen, (const char *)lp->replyP);
        ad1LinkClose(lp);
        return( AD1L_COMM );
    }

    if( lp->replyLen < 28 )
    {
        setError("the pdp-1 at %s:%d sent a HELLO reply that is too short", lp->host, lp->port);
        ad1LinkClose(lp);
        return( AD1L_COMM );
    }

    lp->memWords = rd32(lp->replyP + 4);
    lp->nBreakpoints = rd32(lp->replyP + 8);
    lp->nWatches = rd32(lp->replyP + 12);
    lp->maxRequest = rd32(lp->replyP + 16);
    buildLen = rd32(lp->replyP + 24);
    if( buildLen > (lp->replyLen - 28) )
    {
        buildLen = lp->replyLen - 28;
    }

    if( buildLen >= sizeof(lp->build) )
    {
        buildLen = sizeof(lp->build) - 1;
    }

    memcpy(lp->build, lp->replyP + 28, buildLen);
    lp->build[buildLen] = '\0';
    return( 0 );
}

int
ad1LinkPump(Ad1Link *lp)
{
uint16_t type;
uint16_t status;
uint32_t id;
int rc;

    if( lp->fd < 0 )
    {
        return( AD1L_COMM );
    }

    // Take frames while one is already waiting; a zero wait returns at once when there is none.
    for( ;; )
    {
        rc = readFrame(lp, 0, &type, &status, &id);
        if( rc == 1 )
        {
            return( 0 );
        }

        if( rc != 0 )
        {
            return( AD1L_COMM );
        }

        if( type >= AD1P_EVENT_BASE )
        {
            queueEvent(lp, type, lp->replyP, lp->replyLen);
        }
        else
        {
            lost(lp, "an unexpected reply arrived");
            return( AD1L_COMM );
        }
    }
}

int
ad1LinkNextEvent(Ad1Link *lp, Ad1Event *evP)
{
    if( lp->evCount == 0 )
    {
        return( 0 );
    }

    *evP = lp->events[lp->evHead];
    lp->evHead = (lp->evHead + 1) % AD1L_MAX_EVENTS;
    --lp->evCount;
    return( 1 );
}

// Build a request from a list of words and send it.
static int
requestWords(Ad1Link *lp, uint16_t type, const uint32_t *wordsP, int nWords)
{
uint8_t payload[64];
int i;

    for( i = 0; i < nWords; ++i )
    {
        wr32(payload + (i * 4), wordsP[i]);
    }

    return( request(lp, type, payload, (uint32_t)(nWords * 4), lp->replyTimeoutMs) );
}

// The reply must be at least n words long for the caller to read n words from it. A short one
// from a good status is a protocol fault.
static int
needWords(Ad1Link *lp, uint32_t n)
{
    if( lp->replyLen < (n * 4) )
    {
        lost(lp, "a reply was shorter than expected");
        return( AD1L_COMM );
    }

    return( 0 );
}

int
ad1Ping(Ad1Link *lp)
{
    return( request(lp, AD1P_PING, (const uint8_t *)"p", 1, lp->replyTimeoutMs) );
}

int
ad1GetState(Ad1Link *lp, uint32_t *stateP)
{
uint32_t i;
int status;

    if( (status = request(lp, AD1P_GET_STATE, NULL, 0, lp->replyTimeoutMs)) != AD1P_ST_OK )
    {
        return( status );
    }

    if( needWords(lp, AD1P_STATE_WORDS) != 0 )
    {
        return( AD1L_COMM );
    }

    for( i = 0; i < AD1P_STATE_WORDS; ++i )
    {
        stateP[i] = rd32(lp->replyP + (i * 4));
    }

    return( AD1P_ST_OK );
}

int
ad1SetReg(Ad1Link *lp, uint32_t reg, uint32_t op, uint32_t value)
{
uint32_t w[3];

    w[0] = reg;
    w[1] = op;
    w[2] = value;
    return( requestWords(lp, AD1P_SET_REG, w, 3) );
}

int
ad1ReadMem(Ad1Link *lp, uint32_t address, uint32_t count, uint32_t *wordsP)
{
uint32_t w[2];
uint32_t i;
int status;

    w[0] = address;
    w[1] = count;
    if( (status = requestWords(lp, AD1P_READ_MEM, w, 2)) != AD1P_ST_OK )
    {
        return( status );
    }

    if( (needWords(lp, 1 + count) != 0) || (rd32(lp->replyP) != count) )
    {
        if( lp->fd >= 0 )
        {
            lost(lp, "a memory read returned the wrong count");
        }

        return( AD1L_COMM );
    }

    for( i = 0; i < count; ++i )
    {
        wordsP[i] = rd32(lp->replyP + 4 + (i * 4));
    }

    return( AD1P_ST_OK );
}

int
ad1WriteMem(Ad1Link *lp, uint32_t address, uint32_t count, const uint32_t *wordsP)
{
Ad1Block block;

    block.address = address;
    block.count = count;
    return( ad1WriteBlocks(lp, &block, 1, wordsP, 0, NULL, NULL) );
}

int
ad1WriteBlocks(Ad1Link *lp, const Ad1Block *blocksP, uint32_t nBlocks, const uint32_t *wordsP,
    int stopFirst, uint32_t *wasRunningP, uint32_t *writtenP)
{
uint8_t *reqP;
uint32_t total;
uint32_t off;
uint32_t i;
uint32_t j;
uint32_t used;
int status;

    total = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        total += blocksP[i].count;
    }

    // flags, block count, then per block an address, a count and the words
    if( (8 + (nBlocks * 8) + (total * 4)) > AD1P_MAX_REQUEST )
    {
        setError("the load is larger than one request can carry (%u words in %u blocks)", total, nBlocks);
        return( AD1L_COMM );
    }

    if( (reqP = malloc(8 + (nBlocks * 8) + (total * 4))) == NULL )
    {
        setError("out of memory");
        return( AD1L_COMM );
    }

    wr32(reqP, stopFirst ? AD1P_WRITE_STOP_FIRST : 0);
    wr32(reqP + 4, nBlocks);
    off = 8;
    used = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        wr32(reqP + off, blocksP[i].address);
        wr32(reqP + off + 4, blocksP[i].count);
        off += 8;
        for( j = 0; j < blocksP[i].count; ++j )
        {
            wr32(reqP + off, wordsP[used++]);
            off += 4;
        }
    }

    // A stop can take a while to complete on the server's side, which has its own limit.
    status = request(lp, AD1P_WRITE_MEM, reqP, off, lp->replyTimeoutMs);
    free(reqP);
    if( (status == AD1P_ST_OK) || (status == AD1P_ST_TIMEOUT) )
    {
        if( needWords(lp, 2) != 0 )
        {
            return( AD1L_COMM );
        }

        if( wasRunningP != NULL )
        {
            *wasRunningP = rd32(lp->replyP);
        }

        if( writtenP != NULL )
        {
            *writtenP = rd32(lp->replyP + 4);
        }
    }

    return( status );
}

static int
runReply(Ad1Link *lp, int status, uint32_t *runP, uint32_t *pcP)
{
    if( (status == AD1P_ST_OK) || (status == AD1P_ST_TIMEOUT) )
    {
        if( needWords(lp, 2) != 0 )
        {
            return( AD1L_COMM );
        }

        if( runP != NULL )
        {
            *runP = rd32(lp->replyP);
        }

        if( pcP != NULL )
        {
            *pcP = rd32(lp->replyP + 4);
        }
    }

    return( status );
}

int
ad1Start(Ad1Link *lp, uint32_t address, uint32_t *runP, uint32_t *pcP)
{
    return( runReply(lp, requestWords(lp, AD1P_START, &address, 1), runP, pcP) );
}

int
ad1Stop(Ad1Link *lp, uint32_t *runP, uint32_t *pcP)
{
    return( runReply(lp, request(lp, AD1P_STOP, NULL, 0, lp->replyTimeoutMs), runP, pcP) );
}

int
ad1Continue(Ad1Link *lp, uint32_t *runP)
{
int status;

    status = request(lp, AD1P_CONTINUE, NULL, 0, lp->replyTimeoutMs);
    if( ((status == AD1P_ST_OK) || (status == AD1P_ST_TIMEOUT)) && (runP != NULL) )
    {
        if( needWords(lp, 1) != 0 )
        {
            return( AD1L_COMM );
        }

        *runP = rd32(lp->replyP);
    }

    return( status );
}

int
ad1Step(Ad1Link *lp, uint32_t count, int record, Ad1StepResult *resP)
{
uint8_t payload[8];
uint32_t i;
int timeoutMs;
int status;

    wr32(payload, count);
    wr32(payload + 4, record ? AD1P_STEP_RECORD : 0);

    // Every step is a round of the emulator's loop; give a long run of them proportionally
    // longer than an ordinary reply.
    timeoutMs = lp->replyTimeoutMs + (int)((count > 100000) ? 200000 : count * STEP_MS_EACH);
    status = request(lp, AD1P_STEP, payload, 8, timeoutMs);

    if( (status != AD1P_ST_OK) && (status != AD1P_ST_TIMEOUT) )
    {
        return( status );
    }

    if( needWords(lp, 5) != 0 )
    {
        return( AD1L_COMM );
    }

    resP->done = rd32(lp->replyP);
    resP->reason = rd32(lp->replyP + 4);
    resP->pc = rd32(lp->replyP + 8);
    resP->word = rd32(lp->replyP + 12);
    resP->nRecords = rd32(lp->replyP + 16);
    resP->recordsP = NULL;
    if( (resP->nRecords > AD1P_MAX_RECORDS) || (needWords(lp, 5 + (resP->nRecords * 2)) != 0) )
    {
        if( lp->fd >= 0 )
        {
            lost(lp, "a step reply had a bad record count");
        }

        return( AD1L_COMM );
    }

    if( resP->nRecords > 0 )
    {
        if( lp->recordsP == NULL )
        {
            lp->recordsP = malloc(AD1P_MAX_RECORDS * 2 * sizeof(uint32_t));
        }

        if( lp->recordsP == NULL )
        {
            setError("out of memory");
            return( AD1L_COMM );
        }

        for( i = 0; i < (resP->nRecords * 2); ++i )
        {
            lp->recordsP[i] = rd32(lp->replyP + 20 + (i * 4));
        }

        resP->recordsP = lp->recordsP;
    }

    return( status );
}

int
ad1ClearSingle(Ad1Link *lp)
{
    return( request(lp, AD1P_CLEAR_SINGLE, NULL, 0, lp->replyTimeoutMs) );
}

// Set a breakpoint or watch and pick up the number the server chose.
static int
setReply(Ad1Link *lp, int status, uint32_t *numberP)
{
    if( status == AD1P_ST_OK )
    {
        if( needWords(lp, 1) != 0 )
        {
            return( AD1L_COMM );
        }

        if( numberP != NULL )
        {
            *numberP = rd32(lp->replyP);
        }
    }

    return( status );
}

int
ad1BpSet(Ad1Link *lp, uint32_t address, uint32_t count, uint32_t *numberP)
{
uint32_t w[2];

    w[0] = address;
    w[1] = count;
    return( setReply(lp, requestWords(lp, AD1P_BP_SET, w, 2), numberP) );
}

int
ad1BpDelete(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_BP_DELETE, &number, 1) );
}

int
ad1BpEnable(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_BP_ENABLE, &number, 1) );
}

int
ad1BpDisable(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_BP_DISABLE, &number, 1) );
}

int
ad1BpList(Ad1Link *lp, Ad1BpEntry *entriesP, uint32_t max, uint32_t *nP)
{
uint32_t n;
uint32_t i;
const uint8_t *pP;
int status;

    if( (status = request(lp, AD1P_BP_LIST, NULL, 0, lp->replyTimeoutMs)) != AD1P_ST_OK )
    {
        return( status );
    }

    if( (needWords(lp, 1) != 0) || ((n = rd32(lp->replyP)) > 1024) || (needWords(lp, 1 + (n * 6)) != 0) )
    {
        if( lp->fd >= 0 )
        {
            lost(lp, "a breakpoint list was malformed");
        }

        return( AD1L_COMM );
    }

    if( n > max )
    {
        n = max;
    }

    for( i = 0; i < n; ++i )
    {
        pP = lp->replyP + 4 + (i * 24);
        entriesP[i].isSet = (int)rd32(pP);
        entriesP[i].isEnabled = (int)rd32(pP + 4);
        entriesP[i].number = rd32(pP + 8);
        entriesP[i].address = rd32(pP + 12);
        entriesP[i].count = rd32(pP + 16);
        entriesP[i].curCount = rd32(pP + 20);
    }

    *nP = n;
    return( AD1P_ST_OK );
}

int
ad1WatchSet(Ad1Link *lp, uint32_t address, int onAnyChange, uint32_t value, uint32_t *numberP)
{
uint32_t w[3];

    w[0] = address;
    w[1] = onAnyChange ? 1 : 0;
    w[2] = value;
    return( setReply(lp, requestWords(lp, AD1P_WATCH_SET, w, 3), numberP) );
}

int
ad1WatchDelete(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_WATCH_DELETE, &number, 1) );
}

int
ad1WatchEnable(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_WATCH_ENABLE, &number, 1) );
}

int
ad1WatchDisable(Ad1Link *lp, uint32_t number)
{
    return( requestWords(lp, AD1P_WATCH_DISABLE, &number, 1) );
}

int
ad1WatchList(Ad1Link *lp, Ad1WatchEntry *entriesP, uint32_t max, uint32_t *nP)
{
uint32_t n;
uint32_t i;
const uint8_t *pP;
int status;

    if( (status = request(lp, AD1P_WATCH_LIST, NULL, 0, lp->replyTimeoutMs)) != AD1P_ST_OK )
    {
        return( status );
    }

    if( (needWords(lp, 1) != 0) || ((n = rd32(lp->replyP)) > 1024) || (needWords(lp, 1 + (n * 7)) != 0) )
    {
        if( lp->fd >= 0 )
        {
            lost(lp, "a watch list was malformed");
        }

        return( AD1L_COMM );
    }

    if( n > max )
    {
        n = max;
    }

    for( i = 0; i < n; ++i )
    {
        pP = lp->replyP + 4 + (i * 28);
        entriesP[i].isSet = (int)rd32(pP);
        entriesP[i].isEnabled = (int)rd32(pP + 4);
        entriesP[i].onAnyChange = (int)rd32(pP + 8);
        entriesP[i].number = rd32(pP + 12);
        entriesP[i].address = rd32(pP + 16);
        entriesP[i].value = rd32(pP + 20);
        entriesP[i].lastValue = rd32(pP + 24);
    }

    *nP = n;
    return( AD1P_ST_OK );
}

int
ad1DisableAll(Ad1Link *lp)
{
    return( request(lp, AD1P_DISABLE_ALL, NULL, 0, lp->replyTimeoutMs) );
}

int
ad1AckHit(Ad1Link *lp, uint32_t mask)
{
    return( requestWords(lp, AD1P_ACK_HIT, &mask, 1) );
}

int
ad1SetPolicy(Ad1Link *lp, uint32_t policy)
{
    return( requestWords(lp, AD1P_SET_POLICY, &policy, 1) );
}

const char *
ad1StatusText(int status)
{
    switch( status )
    {
    case AD1P_ST_OK:
        return( "ok" );

    case AD1P_ST_BAD_REQUEST:
        return( "the emulator did not understand the request" );

    case AD1P_ST_BAD_ARG:
        return( "a value was out of range" );

    case AD1P_ST_BAD_STATE:
        return( "the machine is not in a state where that can be done" );

    case AD1P_ST_VERSION:
        return( "the emulator speaks a different protocol version" );

    case AD1P_ST_BUSY:
        return( "another client is connected" );

    case AD1P_ST_TIMEOUT:
        return( "the machine did not respond in time" );

    case AD1P_ST_TOOBIG:
        return( "the request was too large" );

    case AD1P_ST_NOT_SET:
        return( "that breakpoint or watch is not set" );

    case AD1P_ST_ALREADY:
        return( "it is already in that state" );

    case AD1P_ST_NO_SLOT:
        return( "no free breakpoint or watch entry" );

    case AD1L_COMM:
        return( ad1LinkError() );

    default:
        return( "unknown status" );
    }
}
