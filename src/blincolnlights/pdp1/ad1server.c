// Debugger server for ad1 and fastload: a network link to the running emulator.
//
// One I/O thread owns the listening sockets, the single client connection and all frame
// reading and writing; it never touches machine state. The emulator thread is the only thread
// that reads or writes the PDP1 struct on a client's behalf: the I/O thread hands each request
// over in a one-slot mailbox and the emulator thread serves it from ad1Service(), called between
// two machine cycles, so a memory or register access is always consistent. Replies and events
// come back through a small ring in production order. See ad1proto.h for the wire format.
//
// There are two listeners, a loopback one (config key ad1port, default 1044) and a remote one
// on all interfaces (ad1remoteport, default off). Only one client may be connected across both.
//
// Run control uses the same ad1flags the main loop already turns into front-panel switch
// actions; here they are set and waited on by the emulator thread instead of by another process.
//
// 20-Sep-2026 Claude: written to replace the shared-memory link.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// common.h and pdp1.h both typedef FD; common.h has to come first, as it does in main.c.
#include "common.h"
#define NOT_IN_PDP1
#include "pdp1.h"
#include "configuration.h"
#include "ad1proto.h"
#include "ad1server.h"

// Mailbox states. EMPTY -> REQUEST (I/O thread) -> TAKEN (emulator) -> REPLY (emulator) -> EMPTY (I/O thread).
#define MB_EMPTY 0
#define MB_REQUEST 1
#define MB_TAKEN 2
#define MB_REPLY 3

// Session states as the emulator thread must see them.
#define CS_NONE 0
#define CS_READY 1
#define CS_CLOSING 2       // the client is gone; the emulator thread cleans up, then sets CS_NONE

// What the I/O thread is doing with its one connection.
#define SS_NONE 0
#define SS_HELLO 1          // accepted, waiting for HELLO
#define SS_IDLE 2           // handshaken, waiting for a request
#define SS_WAIT 3           // a request is with the emulator thread

#define RING_SIZE 16
#define RING_EVENT 1
#define RING_REPLY 2
#define RING_DATA 48

#define HELLO_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 5000
#define REFUSE_DRAIN_MS 100
#define OP_TIMEOUT_NS 250000000ULL
#define MAX_HELLO_PAYLOAD 4096

#define OP_NONE 0
#define OP_START 1
#define OP_STOP 2
#define OP_CONTINUE 3
#define OP_STEP 4
#define OP_WRITE 5

#define REC_OFFSET 20       // STEP reply: five header words, then the records

typedef struct
{
    _Atomic int state;
    uint16_t type;
    uint32_t id;
    uint32_t reqLen;
    uint16_t repStatus;
    uint32_t repLen;
} Mailbox;

typedef struct
{
    int kind;
    uint32_t len;
    uint8_t data[RING_DATA];
} RingEntry;

_Atomic int ad1Work;

static bool serverActive;
static int wakeFd = -1;             // I/O thread to emulator: ends the throttle sleep
static int txFd = -1;               // emulator to I/O thread: a reply or event is ready
static int listenFds[2] = { -1, -1 };
static const char *listenNames[2] = { "loopback", "remote" };

static Mailbox mb;
static uint8_t *reqBufP;
static uint8_t *repBufP;
static RingEntry ringEntries[RING_SIZE];
static _Atomic unsigned ringHead;   // written by the emulator thread
static _Atomic unsigned ringTail;   // written by the I/O thread
static _Atomic int clientState;
static _Atomic int clientSubscribe;
static _Atomic unsigned droppedEvents;

// Emulator-thread state.
static bool sessionKnown;           // the emulator thread has seen the current client
static bool subscribedRun;
static bool lastRun;
static bool reportedBrk;
static bool reportedWatch;
static int policy;
static int opKind;
static u64 opDeadline;
static uint32_t opStepsLeft;
static uint32_t opStepsDone;
static bool opRecording;
static uint32_t opRecords;
static uint32_t opWasRunning;

static uint32_t rd32(const uint8_t *pP);
static uint16_t rd16(const uint8_t *pP);
static void wr32(uint8_t *pP, uint32_t v);
static void wr16(uint8_t *pP, uint16_t v);
static void wakeEmulator(void);
static void wakeIoThread(void);

// Little-endian field access. Frames are built and parsed byte by byte so nothing depends on
// the host's layout.
static uint32_t
rd32(const uint8_t *pP)
{
    return( (uint32_t)pP[0] | ((uint32_t)pP[1] << 8) | ((uint32_t)pP[2] << 16) | ((uint32_t)pP[3] << 24) );
}

static uint16_t
rd16(const uint8_t *pP)
{
    return( (uint16_t)((uint16_t)pP[0] | ((uint16_t)pP[1] << 8)) );
}

static void
wr32(uint8_t *pP, uint32_t v)
{
    pP[0] = (uint8_t)(v & 0xFF);
    pP[1] = (uint8_t)((v >> 8) & 0xFF);
    pP[2] = (uint8_t)((v >> 16) & 0xFF);
    pP[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void
wr16(uint8_t *pP, uint16_t v)
{
    pP[0] = (uint8_t)(v & 0xFF);
    pP[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Wall clock in milliseconds, for the I/O thread's timeouts.
static uint64_t
monoMs(void)
{
struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return( ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000) );
}

// Make an eventfd readable so a poller wakes. The counter saturating is harmless: it is
// already readable.
static void
kick(int fd)
{
uint64_t one;

    one = 1;
    if( write(fd, &one, sizeof(one)) < 0 )
    {
        // EAGAIN means the counter is full, so the waiter is awake or about to be
    }
}

static void
wakeEmulator(void)
{
    kick(wakeFd);
}

static void
wakeIoThread(void)
{
    kick(txFd);
}

// Clear an eventfd's counter so it stops reporting readable.
static void
drainEventFd(int fd)
{
uint64_t v;

    if( read(fd, &v, sizeof(v)) < 0 )
    {
        // nothing was pending
    }
}

// Build a frame in outP: the 12-byte header followed by nWords words. Returns its length.
static uint32_t
buildFrame(uint8_t *outP, uint16_t type, uint16_t status, uint32_t id, const uint32_t *wordsP, int nWords)
{
int i;

    wr32(outP, (uint32_t)(nWords * 4));
    wr16(outP + 4, type);
    wr16(outP + 6, status);
    wr32(outP + 8, id);
    for( i = 0; i < nWords; ++i )
    {
        wr32(outP + AD1P_HEADER_SIZE + (i * 4), wordsP[i]);
    }

    return( (uint32_t)(AD1P_HEADER_SIZE + (nWords * 4)) );
}

// Push an entry onto the emulator-to-I/O ring. Events leave the last slot free so a reply,
// which cannot be dropped, always fits. Returns false if the ring had no room.
static bool
ringPush(int kind, const uint8_t *dataP, uint32_t len)
{
unsigned head;
unsigned tail;
unsigned limit;
RingEntry *entryP;

    head = atomic_load_explicit(&ringHead, memory_order_relaxed);
    tail = atomic_load_explicit(&ringTail, memory_order_acquire);
    limit = (kind == RING_REPLY) ? RING_SIZE : (RING_SIZE - 1);
    if( (head - tail) >= limit )
    {
        return( false );
    }

    entryP = &ringEntries[head % RING_SIZE];
    entryP->kind = kind;
    entryP->len = len;
    if( len )
    {
        memcpy(entryP->data, dataP, len);
    }

    atomic_store_explicit(&ringHead, head + 1, memory_order_release);
    wakeIoThread();
    return( true );
}

// Queue an event for the client, counting it if the ring was full.
static void
pushEvent(uint16_t type, const uint32_t *wordsP, int nWords)
{
uint8_t frame[RING_DATA];
uint32_t len;

    len = buildFrame(frame, type, 0, 0, wordsP, nWords);
    if( !ringPush(RING_EVENT, frame, len) )
    {
        atomic_fetch_add(&droppedEvents, 1);
    }
}

// The I/O thread side.

// Write every byte of a header and payload to the client. Returns false on error or timeout.
static bool
sendFrame(int fd, uint16_t type, uint16_t status, uint32_t id, const uint8_t *payloadP, uint32_t len)
{
uint8_t hdr[AD1P_HEADER_SIZE];
struct iovec iov[2];
struct pollfd pfd;
int niov;
ssize_t n;
uint32_t sent;
uint32_t total;

    wr32(hdr, len);
    wr16(hdr + 4, type);
    wr16(hdr + 6, status);
    wr32(hdr + 8, id);

    total = AD1P_HEADER_SIZE + len;
    sent = 0;
    while( sent < total )
    {
        niov = 0;
        if( sent < AD1P_HEADER_SIZE )
        {
            iov[niov].iov_base = hdr + sent;
            iov[niov].iov_len = AD1P_HEADER_SIZE - sent;
            ++niov;
            if( len )
            {
                iov[niov].iov_base = (void *)payloadP;
                iov[niov].iov_len = len;
                ++niov;
            }
        }
        else
        {
            iov[niov].iov_base = (void *)(payloadP + (sent - AD1P_HEADER_SIZE));
            iov[niov].iov_len = total - sent;
            ++niov;
        }

        n = writev(fd, iov, niov);
        if( n > 0 )
        {
            sent += (uint32_t)n;
        }
        else if( (n < 0) && ((errno == EAGAIN) || (errno == EINTR)) )
        {
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if( poll(&pfd, 1, IO_TIMEOUT_MS) <= 0 )
            {
                return( false );
            }
        }
        else
        {
            return( false );
        }
    }

    return( true );
}

// Send a frame whose payload is a text message.
static bool
sendText(int fd, uint16_t type, uint16_t status, uint32_t id, const char *textP)
{
    return( sendFrame(fd, type, status, id, (const uint8_t *)textP, (uint32_t)strlen(textP)) );
}

// Read exactly n bytes, waiting up to timeoutMs for each chunk. Returns false on EOF, error
// or timeout.
static bool
readFull(int fd, uint8_t *bufP, uint32_t n, int timeoutMs)
{
struct pollfd pfd;
ssize_t got;
uint32_t have;

    have = 0;
    while( have < n )
    {
        pfd.fd = fd;
        pfd.events = POLLIN;
        if( poll(&pfd, 1, timeoutMs) <= 0 )
        {
            return( false );
        }

        got = recv(fd, bufP + have, n - have, 0);
        if( got <= 0 )
        {
            if( (got < 0) && ((errno == EINTR) || (errno == EAGAIN)) )
            {
                continue;
            }

            return( false );
        }

        have += (uint32_t)got;
    }

    return( true );
}

// Options for an accepted connection: no Nagle delay, and keepalive so a vanished remote peer
// gives up the one client slot in about 45 seconds.
static void
tuneSocket(int fd)
{
struct timeval sendLimit;
int on;
int idle;
int intvl;
int cnt;

    nodelay(fd);
    on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

    // A client that stops reading must not stall the I/O thread's writes for ever.
    sendLimit.tv_sec = IO_TIMEOUT_MS / 1000;
    sendLimit.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendLimit, sizeof(sendLimit));
    idle = 30;
    intvl = 5;
    cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

// Close a connection after a last message. Bytes the peer has already sent may still be unread,
// and closing with unread data resets the connection, which can destroy the message before the
// peer reads it, so the send side is shut down first and the peer's bytes are drained.
static void
gracefulClose(int fd)
{
uint8_t junk[256];
struct pollfd pfd;
uint64_t end;
int left;

    shutdown(fd, SHUT_WR);

    end = monoMs() + REFUSE_DRAIN_MS;
    for( ;; )
    {
        left = (int)(end - monoMs());
        if( left <= 0 )
        {
            break;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        if( (poll(&pfd, 1, left) <= 0) || (recv(fd, junk, sizeof(junk), 0) <= 0) )
        {
            break;
        }
    }

    close(fd);
}

// Tell a second client why it was refused and close it.
static void
refuseClient(int fd, const char *whoP)
{
char text[160];

    snprintf(text, sizeof(text), "another client is already connected (%s)", whoP);
    sendText(fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BUSY, 0, text);
    gracefulClose(fd);
}

// Everything the I/O thread knows about its one connection.
typedef struct
{
    int fd;
    int state;
    bool ready;                 // the emulator thread has been told about this client
    uint64_t helloDeadline;
    char desc[96];
} Session;

// Finish the connection. If the emulator thread knew about the client, hand it the cleanup.
static void
sessionEnd(Session *sP)
{
int fd;

    fd = sP->fd;
    sP->fd = -1;
    sP->state = SS_NONE;
    if( sP->ready )
    {
        sP->ready = false;
        atomic_store(&clientState, CS_CLOSING);
        atomic_fetch_or(&ad1Work, AD1_WORK_CONN);
        wakeEmulator();
    }

    gracefulClose(fd);
}

// Answer HELLO: check the protocol version and, if it matches, tell the emulator thread a
// client is ready. Returns false if the session is over.
static bool
handleHello(Session *sP, uint32_t id, const uint8_t *pP, uint32_t len)
{
char build[64];
uint32_t words[7];
uint32_t nameLen;
uint32_t buildLen;
uint32_t proto;
char text[96];
uint8_t payload[64];
int i;

    if( len < 12 )
    {
        sendText(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BAD_REQUEST, id, "HELLO payload too short");
        return( false );
    }

    proto = rd32(pP);
    nameLen = rd32(pP + 8);
    if( nameLen > (len - 12) )
    {
        sendText(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BAD_REQUEST, id, "HELLO name length is wrong");
        return( false );
    }

    if( proto != AD1P_VERSION )
    {
        snprintf(text, sizeof(text), "the emulator speaks protocol %d, the client speaks %u",
            AD1P_VERSION, proto);
        sendText(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_VERSION, id, text);
        return( false );
    }

    snprintf(build, sizeof(build), "pidp1 built %s", __DATE__);
    buildLen = (uint32_t)strlen(build);

    words[0] = AD1P_VERSION;
    words[1] = AD1P_MEM_WORDS;
    words[2] = AD1P_NUM_BREAKPOINTS;
    words[3] = AD1P_NUM_WATCHES;
    words[4] = AD1P_MAX_REQUEST;
    words[5] = AD1P_MAX_REPLY;
    words[6] = buildLen;
    for( i = 0; i < 7; ++i )
    {
        wr32(payload + (i * 4), words[i]);
    }

    memcpy(payload + 28, build, buildLen);

    if( !sendFrame(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_OK, id, payload, 28 + buildLen) )
    {
        return( false );
    }

    // The emulator thread is idle with respect to the mailbox and ring until CS_READY, so they
    // can be reset here without a race.
    atomic_store(&ringTail, atomic_load(&ringHead));
    atomic_store(&mb.state, MB_EMPTY);
    atomic_store(&clientSubscribe, (int)rd32(pP + 4));
    sP->ready = true;
    sP->state = SS_IDLE;
    atomic_store(&clientState, CS_READY);
    atomic_fetch_or(&ad1Work, AD1_WORK_CONN);
    wakeEmulator();
    return( true );
}

// Read one frame from the client. In SS_HELLO it must be HELLO; in SS_IDLE a request is read
// into the mailbox and handed to the emulator thread. Returns false if the session is over.
static bool
handleFrame(Session *sP)
{
uint8_t hdr[AD1P_HEADER_SIZE];
uint8_t small[MAX_HELLO_PAYLOAD];
uint32_t len;
uint32_t id;
uint16_t type;

    if( !readFull(sP->fd, hdr, AD1P_HEADER_SIZE, IO_TIMEOUT_MS) )
    {
        return( false );
    }

    len = rd32(hdr);
    type = rd16(hdr + 4);
    id = rd32(hdr + 8);

    if( sP->state == SS_HELLO )
    {
        if( (type != AD1P_HELLO) || (len > MAX_HELLO_PAYLOAD) )
        {
            sendText(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BAD_REQUEST, id,
                "this is the pidp1 debugger port; the first frame must be HELLO");
            return( false );
        }

        if( !readFull(sP->fd, small, len, IO_TIMEOUT_MS) )
        {
            return( false );
        }

        return( handleHello(sP, id, small, len) );
    }

    if( len > AD1P_MAX_REQUEST )
    {
        sendText(sP->fd, type | AD1P_REPLY, AD1P_ST_TOOBIG, id, "frame too large");
        return( false );
    }

    if( type == AD1P_HELLO )
    {
        // Already handshaken; read and discard its payload to stay in step.
        while( len > 0 )
        {
            uint32_t chunk;

            chunk = (len > sizeof(small)) ? (uint32_t)sizeof(small) : len;
            if( !readFull(sP->fd, small, chunk, IO_TIMEOUT_MS) )
            {
                return( false );
            }

            len -= chunk;
        }

        return( sendText(sP->fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BAD_REQUEST, id, "already connected") );
    }

    if( (len > 0) && !readFull(sP->fd, reqBufP, len, IO_TIMEOUT_MS) )
    {
        return( false );
    }

    mb.type = type;
    mb.id = id;
    mb.reqLen = len;
    atomic_store(&mb.state, MB_REQUEST);
    sP->state = SS_WAIT;
    atomic_fetch_or(&ad1Work, AD1_WORK_REQUEST);
    wakeEmulator();
    return( true );
}

// Send whatever the emulator thread has queued, in order. Entries queued for a client that is
// gone, or not yet handshaken, are discarded so a new client never sees them. Returns false if
// a write failed.
static bool
drainRing(Session *sP)
{
unsigned head;
unsigned tail;
RingEntry *entryP;
bool ok;
bool live;

    ok = true;
    live = ((sP->fd >= 0) && ((sP->state == SS_IDLE) || (sP->state == SS_WAIT)));
    for( ;; )
    {
        head = atomic_load_explicit(&ringHead, memory_order_acquire);
        tail = atomic_load_explicit(&ringTail, memory_order_relaxed);
        if( head == tail )
        {
            break;
        }

        entryP = &ringEntries[tail % RING_SIZE];
        if( live )
        {
            if( entryP->kind == RING_EVENT )
            {
                ok = ok && (write(sP->fd, entryP->data, entryP->len) == (ssize_t)entryP->len);
            }
            else
            {
                ok = ok && sendFrame(sP->fd, mb.type | AD1P_REPLY, mb.repStatus, mb.id, repBufP, mb.repLen);
            }
        }

        if( entryP->kind == RING_REPLY )
        {
            atomic_store(&mb.state, MB_EMPTY);
            if( sP->state == SS_WAIT )
            {
                sP->state = SS_IDLE;
            }
        }

        atomic_store_explicit(&ringTail, tail + 1, memory_order_release);
    }

    return( ok );
}

// Accept a connection on a listener. It becomes the session if there is none, otherwise it is
// told why it was refused.
static void
acceptClient(Session *sP, int which)
{
struct sockaddr_in peer;
socklen_t plen;
char ip[INET_ADDRSTRLEN];
char who[96];
int fd;

    plen = sizeof(peer);
    fd = accept(listenFds[which], (struct sockaddr *)&peer, &plen);
    if( fd < 0 )
    {
        return;
    }

    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    snprintf(who, sizeof(who), "%s:%u on the %s port", ip, (unsigned)ntohs(peer.sin_port), listenNames[which]);
    tuneSocket(fd);

    if( (sP->fd >= 0) || (atomic_load(&clientState) != CS_NONE) )
    {
        refuseClient(fd, (sP->fd >= 0) ? sP->desc : "the previous session is still closing");
        return;
    }

    sP->fd = fd;
    sP->state = SS_HELLO;
    sP->ready = false;
    sP->helloDeadline = monoMs() + HELLO_TIMEOUT_MS;
    snprintf(sP->desc, sizeof(sP->desc), "%s", who);
}

// The I/O thread: polls the listeners, the client and the emulator's wake, forever.
static void *
ioThread(void *argP)
{
Session s;
struct pollfd pfds[4];
int which[4];
int n;
int i;
int timeout;
int clientIdx;
int txIdx;
int64_t left;

    (void)argP;
    s.fd = -1;
    s.state = SS_NONE;
    s.ready = false;
    s.helloDeadline = 0;
    s.desc[0] = '\0';

    for( ;; )
    {
        n = 0;
        pfds[n].fd = txFd;
        pfds[n].events = POLLIN;
        txIdx = n;
        which[n++] = -1;

        // While a finished session is being cleaned up by the emulator thread, a new
        // connection waits in the listen backlog instead of being refused.
        for( i = 0; i < 2; ++i )
        {
            if( (listenFds[i] >= 0) && (atomic_load(&clientState) != CS_CLOSING) )
            {
                pfds[n].fd = listenFds[i];
                pfds[n].events = POLLIN;
                which[n++] = i;
            }
        }

        clientIdx = -1;
        if( s.fd >= 0 )
        {
            clientIdx = n;
            pfds[n].fd = s.fd;
            pfds[n].events = POLLRDHUP;
            if( (s.state == SS_HELLO) || (s.state == SS_IDLE) )
            {
                pfds[n].events |= POLLIN;
            }

            which[n++] = -1;
        }

        timeout = -1;
        if( s.state == SS_HELLO )
        {
            left = (int64_t)s.helloDeadline - (int64_t)monoMs();
            timeout = (left > 0) ? (int)left : 0;
        }

        if( poll(pfds, n, timeout) < 0 )
        {
            if( errno == EINTR )
            {
                continue;
            }

            break;
        }

        if( (s.state == SS_HELLO) && (monoMs() >= s.helloDeadline) )
        {
            sendText(s.fd, AD1P_HELLO | AD1P_REPLY, AD1P_ST_BAD_REQUEST, 0, "no HELLO received");
            sessionEnd(&s);
            continue;
        }

        // The order matters: the poll results describe the connection as it was when polled, so
        // the client's events are handled before anything can end that connection and a new
        // one be accepted in its place.
        if( clientIdx >= 0 )
        {
            if( pfds[clientIdx].revents & (POLLHUP | POLLERR | POLLRDHUP | POLLNVAL) )
            {
                sessionEnd(&s);
            }
            else if( pfds[clientIdx].revents & POLLIN )
            {
                if( !handleFrame(&s) )
                {
                    sessionEnd(&s);
                }
            }
        }

        if( pfds[txIdx].revents & POLLIN )
        {
            drainEventFd(txFd);
        }

        if( !drainRing(&s) && (s.fd >= 0) )
        {
            sessionEnd(&s);
        }

        for( i = 0; i < n; ++i )
        {
            if( (which[i] >= 0) && (pfds[i].revents & POLLIN) )
            {
                acceptClient(&s, which[i]);
            }
        }
    }

    return( NULL );
}

// Bind a listening socket on addr:port. Returns the fd, or -1 (and says why on stderr).
static int
openListener(uint32_t addr, int port, const char *nameP)
{
struct sockaddr_in sin;
int fd;
int on;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if( fd < 0 )
    {
        fprintf(stderr, "pidp1: debugger %s port %d: socket failed: %s\n", nameP, port, strerror(errno));
        return( -1 );
    }

    on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(addr);
    sin.sin_port = htons((uint16_t)port);

    if( (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) || (listen(fd, 4) < 0) )
    {
        fprintf(stderr, "pidp1: debugger %s port %d unavailable: %s\n", nameP, port, strerror(errno));
        close(fd);
        return( -1 );
    }

    return( fd );
}

// Read a port from the config extras. A missing key gives defaultPort; off, 0 or no gives 0
// (the listener is not opened); on gives defaultPort. Anything not a valid port is reported and
// gives defaultPort.
static int
configPort(ConfigurationP configP, char *nameP, int defaultPort)
{
ConfigurationSettingP settingP;

    settingP = findConfigurationSetting(configP, nameP);
    if( !settingP )
    {
        return( defaultPort );
    }

    if( settingP->strvalueP || (settingP->ivalue < 0) || (settingP->ivalue > 65535) )
    {
        fprintf(stderr, "pidp1: %s must be a port number, off or on; using %d\n", nameP, defaultPort);
        return( defaultPort );
    }

    if( settingP->onOff && (settingP->ivalue == 0) )
    {
        return( defaultPort );
    }

    return( settingP->ivalue );
}

// Undo a start that did not complete, so the main loop sees no server at all.
static void
serverRelease(void)
{
int i;

    for( i = 0; i < 2; ++i )
    {
        if( listenFds[i] >= 0 )
        {
            close(listenFds[i]);
            listenFds[i] = -1;
        }
    }

    if( wakeFd >= 0 )
    {
        close(wakeFd);
        wakeFd = -1;
    }

    if( txFd >= 0 )
    {
        close(txFd);
        txFd = -1;
    }

    free(reqBufP);
    free(repBufP);
    reqBufP = NULL;
    repBufP = NULL;
}

// Start the server if either port is configured and can be bound. The emulator runs the same
// with or without it; if neither listener opens it says so and the main loop never sees the
// server.
void
ad1ServerStart(PDP1 *pdp, ConfigurationP configP)
{
pthread_t th;
int localPort;
int remotePort;

    (void)pdp;

    if( configP && configP->useShm )
    {
        fprintf(stderr, "pidp1: the 'shared' setting is obsolete and ignored; ad1 and fastload use the "
            "network ports ad1port and ad1remoteport\n");
    }

    localPort = configPort(configP, "ad1port", AD1P_DEFAULT_LOCAL_PORT);
    remotePort = configPort(configP, "ad1remoteport", 0);
    if( !localPort && !remotePort )
    {
        return;
    }

    wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    txFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    reqBufP = malloc(AD1P_MAX_REQUEST);
    repBufP = malloc(AD1P_MAX_REPLY);
    if( (wakeFd < 0) || (txFd < 0) || !reqBufP || !repBufP )
    {
        fprintf(stderr, "pidp1: debugger server could not allocate its resources\n");
        serverRelease();
        return;
    }

    if( localPort )
    {
        listenFds[0] = openListener(INADDR_LOOPBACK, localPort, listenNames[0]);
    }

    if( remotePort )
    {
        listenFds[1] = openListener(INADDR_ANY, remotePort, listenNames[1]);
    }

    if( (listenFds[0] < 0) && (listenFds[1] < 0) )
    {
        serverRelease();
        return;
    }

    if( pthread_create(&th, NULL, ioThread, NULL) != 0 )
    {
        fprintf(stderr, "pidp1: debugger server thread could not start\n");
        serverRelease();
        return;
    }

    pthread_detach(th);
    serverActive = true;

    if( listenFds[0] >= 0 )
    {
        fprintf(stderr, "pidp1: debugger listening on 127.0.0.1:%d\n", localPort);
    }

    if( listenFds[1] >= 0 )
    {
        fprintf(stderr, "pidp1: debugger listening on all interfaces, port %d\n", remotePort);
    }
}

// The emulator thread side.

// The full 16-bit address of the PC, extension bits included.
static uint32_t
fullPc(PDP1 *pdp)
{
    return( (pdp->epc & EXTMASK) | (pdp->pc & ADDRMASK) );
}

static uint32_t
wordAt(PDP1 *pdp, uint32_t addr)
{
    return( pdp->core[addr & 0177777] & WORDMASK );
}

// Reply construction: the payload is built at repBufP and mb.repLen counts it.
static void
repPut(uint32_t v)
{
    wr32(repBufP + mb.repLen, v);
    mb.repLen += 4;
}

// Publish the reply. The ring entry is what tells the I/O thread to send it.
static void
postReply(uint16_t status)
{
    mb.repStatus = status;
    atomic_store(&mb.state, MB_REPLY);
    ringPush(RING_REPLY, NULL, 0);
}

static void
replyStatus(uint16_t status)
{
    mb.repLen = 0;
    postReply(status);
}

static void
replyText(uint16_t status, const char *textP)
{
    mb.repLen = (uint32_t)strlen(textP);
    memcpy(repBufP, textP, mb.repLen);
    postReply(status);
}

// Keep the derived enable flags of the tables in step with their contents: on while any entry
// is set.
static void
syncTableFlags(PDP1 *pdp)
{
int i;
bool anyBrk;
bool anyWatch;

    anyBrk = false;
    anyWatch = false;
    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        anyBrk = anyBrk || pdp->ad1Breakpoints[i].isSet;
    }

    for( i = 0; i < AD1_NUM_WATCHES; ++i )
    {
        anyWatch = anyWatch || pdp->ad1Watches[i].isSet;
    }

    if( anyBrk )
    {
        AD1_ENABLE_BREAKPOINTS(pdp);
    }
    else
    {
        AD1_DISABLE_BREAKPOINTS(pdp);
    }

    if( anyWatch )
    {
        AD1_ENABLE_WATCHES(pdp);
    }
    else
    {
        AD1_DISABLE_WATCHES(pdp);
    }
}

static void
beginBusy(int kind)
{
    opKind = kind;
    opDeadline = gettime() + OP_TIMEOUT_NS;
    atomic_fetch_or(&ad1Work, AD1_WORK_BUSY);
}

static void
endBusy(void)
{
    opKind = OP_NONE;
    atomic_fetch_and(&ad1Work, ~AD1_WORK_BUSY);
}

// Check every block of a WRITE_MEM payload before anything is written. Returns true if the
// payload is exactly a block count followed by that many well-formed blocks inside memory;
// *wordsP is the total word count.
static bool
blocksValid(const uint8_t *pP, uint32_t len, uint32_t *wordsP)
{
uint32_t nBlocks;
uint32_t off;
uint32_t i;
uint32_t addr;
uint32_t count;
uint32_t total;

    if( len < 8 )
    {
        return( false );
    }

    nBlocks = rd32(pP + 4);
    if( nBlocks == 0 )
    {
        return( false );
    }

    off = 8;
    total = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        if( (len - off) < 8 )
        {
            return( false );
        }

        addr = rd32(pP + off);
        count = rd32(pP + off + 4);
        off += 8;
        if( (count == 0) || (addr >= AD1P_MEM_WORDS) || (count > (AD1P_MEM_WORDS - addr)) )
        {
            return( false );
        }

        if( count > ((len - off) / 4) )
        {
            return( false );
        }

        off += (count * 4);
        total += count;
    }

    *wordsP = total;
    return( off == len );
}

// Store the words of an already-validated WRITE_MEM payload.
static void
blocksApply(PDP1 *pdp, const uint8_t *pP)
{
uint32_t nBlocks;
uint32_t off;
uint32_t i;
uint32_t j;
uint32_t addr;
uint32_t count;

    nBlocks = rd32(pP + 4);
    off = 8;
    for( i = 0; i < nBlocks; ++i )
    {
        addr = rd32(pP + off);
        count = rd32(pP + off + 4);
        off += 8;
        for( j = 0; j < count; ++j )
        {
            pdp->core[addr + j] = (rd32(pP + off + (j * 4)) & WORDMASK);
        }

        off += (count * 4);
    }
}

static void
replyRunPc(PDP1 *pdp, uint16_t status)
{
    mb.repLen = 0;
    repPut((uint32_t)pdp->run);
    repPut(fullPc(pdp));
    postReply(status);
}

// GET_STATE: a snapshot of the registers, run state and hit latches.
static void
opGetState(PDP1 *pdp)
{
uint32_t pc;

    pc = fullPc(pdp);
    mb.repLen = 0;
    repPut(pdp->ac & WORDMASK);
    repPut(pdp->io & WORDMASK);
    repPut(pc);
    repPut(pdp->tw & WORDMASK);
    repPut((uint32_t)pdp->pf);
    repPut((uint32_t)pdp->ss);
    repPut(pdp->ta & WORDMASK);
    repPut(pdp->ma & WORDMASK);
    repPut(pdp->mb & WORDMASK);
    repPut((uint32_t)pdp->exd);
    repPut((uint32_t)pdp->run);
    repPut((uint32_t)pdp->run_enable);
    repPut((uint32_t)pdp->power_sw);
    repPut(AD1_SINGLE(pdp) ? 1 : 0);
    repPut(AD1_BREAKPOINT_HIT(pdp) ? 1 : 0);
    repPut((uint32_t)pdp->ad1brkNo);
    repPut(AD1_WATCH_HIT(pdp) ? 1 : 0);
    repPut((uint32_t)pdp->ad1watchNo);
    repPut(AD1_BREAKPOINTS_ENABLED(pdp) ? 1 : 0);
    repPut(AD1_WATCHES_ENABLED(pdp) ? 1 : 0);
    repPut(atomic_load(&droppedEvents));
    repPut(wordAt(pdp, pc));
    postReply(AD1P_ST_OK);
}

// SET_REG: assign AC, IO, PC or PF, or set or clear bits of PF.
static void
opSetReg(PDP1 *pdp, const uint8_t *pP)
{
uint32_t reg;
uint32_t op;
uint32_t value;

    reg = rd32(pP);
    op = rd32(pP + 4);
    value = rd32(pP + 8);

    if( (op != AD1P_OP_ASSIGN) && (reg != AD1P_REG_PF) )
    {
        replyText(AD1P_ST_BAD_ARG, "only the program flags take OR and clear");
        return;
    }

    switch( reg )
    {
    case AD1P_REG_AC:
        pdp->ac = (value & WORDMASK);
        break;

    case AD1P_REG_IO:
        pdp->io = (value & WORDMASK);
        break;

    case AD1P_REG_PC:
        if( value >= AD1P_MEM_WORDS )
        {
            replyText(AD1P_ST_BAD_ARG, "the address is outside memory");
            return;
        }

        pdp->pc = (value & ADDRMASK);
        pdp->epc = (value & EXTMASK);
        pdp->exd = (pdp->epc != 0);
        break;

    case AD1P_REG_PF:
        if( value > 077 )
        {
            replyText(AD1P_ST_BAD_ARG, "the program flags are 6 bits");
            return;
        }

        if( op == AD1P_OP_ASSIGN )
        {
            pdp->pf = (int)value;
        }
        else if( op == AD1P_OP_OR )
        {
            pdp->pf |= (int)value;
        }
        else if( op == AD1P_OP_CLEAR )
        {
            pdp->pf &= (int)(~value & 077);
        }
        else
        {
            replyText(AD1P_ST_BAD_ARG, "unknown register operation");
            return;
        }

        break;

    default:
        replyText(AD1P_ST_BAD_ARG, "that register cannot be set");
        return;
    }

    replyStatus(AD1P_ST_OK);
}

// READ_MEM: one consistent snapshot of a range.
static void
opReadMem(PDP1 *pdp, const uint8_t *pP)
{
uint32_t addr;
uint32_t count;
uint32_t i;

    addr = rd32(pP);
    count = rd32(pP + 4);
    if( (count == 0) || (addr >= AD1P_MEM_WORDS) || (count > (AD1P_MEM_WORDS - addr)) )
    {
        replyText(AD1P_ST_BAD_ARG, "the range is outside memory");
        return;
    }

    mb.repLen = 0;
    repPut(count);
    for( i = 0; i < count; ++i )
    {
        repPut(pdp->core[addr + i] & WORDMASK);
    }

    postReply(AD1P_ST_OK);
}

// WRITE_MEM: all blocks are checked, then applied together between two cycles. With
// STOP_FIRST a running machine is stopped first, which takes more than one pass.
static void
opWriteMem(PDP1 *pdp, const uint8_t *pP, uint32_t len)
{
uint32_t flags;
uint32_t words;

    if( !blocksValid(pP, len, &words) )
    {
        replyText(AD1P_ST_BAD_ARG, "a block is outside memory or the request is malformed");
        return;
    }

    flags = rd32(pP);
    opWasRunning = (uint32_t)(pdp->run != 0);
    if( (flags & AD1P_WRITE_STOP_FIRST) && opWasRunning )
    {
        AD1_CLEAR_SINGLE(pdp);
        AD1_SET_STOP(pdp);
        beginBusy(OP_WRITE);
        return;
    }

    blocksApply(pdp, pP);
    mb.repLen = 0;
    repPut(opWasRunning);
    repPut(words);
    postReply(AD1P_ST_OK);
}

// The pre-step record and the flag that makes the main loop take one step. The pair is
// taken before the step, which is what a trace of the instruction stream needs.
static void
stepIssue(PDP1 *pdp)
{
uint32_t pc;

    if( opRecording )
    {
        pc = fullPc(pdp);
        wr32(repBufP + REC_OFFSET + (opRecords * 8), pc);
        wr32(repBufP + REC_OFFSET + (opRecords * 8) + 4, wordAt(pdp, pc));
        ++opRecords;
    }

    AD1_SET_CONTINUE(pdp);
    --opStepsLeft;
    opDeadline = gettime() + OP_TIMEOUT_NS;
}

// Send the STEP reply: how many steps ran, why it ended, where the PC is, and the records.
static void
stepFinish(PDP1 *pdp, uint32_t reason, uint16_t status)
{
uint32_t pc;

    pc = fullPc(pdp);
    wr32(repBufP, opStepsDone);
    wr32(repBufP + 4, reason);
    wr32(repBufP + 8, pc);
    wr32(repBufP + 12, wordAt(pdp, pc));
    wr32(repBufP + 16, opRecords);
    mb.repLen = REC_OFFSET + (opRecords * 8);
    endBusy();
    postReply(status);
}

// STEP: the machine must be stopped. The single-step state is left set afterwards, as a
// stepping debugger expects.
static void
opStep(PDP1 *pdp, const uint8_t *pP)
{
uint32_t count;
uint32_t flags;

    count = rd32(pP);
    flags = rd32(pP + 4);
    if( (count < 1) || (count > 0x7FFFFFFF) || ((flags & AD1P_STEP_RECORD) && (count > AD1P_MAX_RECORDS)) )
    {
        replyText(AD1P_ST_BAD_ARG, "bad step count");
        return;
    }

    if( pdp->run || !pdp->power_sw )
    {
        replyText(AD1P_ST_BAD_STATE, pdp->run ? "the machine must be stopped to step" : "the power is off");
        return;
    }

    AD1_SET_SINGLE(pdp);
    opStepsLeft = count;
    opStepsDone = 0;
    opRecording = ((flags & AD1P_STEP_RECORD) != 0);
    opRecords = 0;
    beginBusy(OP_STEP);
    stepIssue(pdp);
}

// Check on the operation in progress; if it has finished or timed out, send its reply.
static void
opPoll(PDP1 *pdp)
{
bool expired;

    expired = (gettime() > opDeadline);

    switch( opKind )
    {
    case OP_START:
        if( !AD1_START(pdp) )
        {
            endBusy();
            replyRunPc(pdp, AD1P_ST_OK);
        }
        else if( expired )
        {
            AD1_CLEAR_START(pdp);
            endBusy();
            replyRunPc(pdp, AD1P_ST_TIMEOUT);
        }

        break;

    case OP_STOP:
        if( !AD1_STOP(pdp) && !pdp->run )
        {
            endBusy();
            replyRunPc(pdp, AD1P_ST_OK);
        }
        else if( expired )
        {
            AD1_CLEAR_STOP(pdp);
            endBusy();
            replyRunPc(pdp, AD1P_ST_TIMEOUT);
        }

        break;

    case OP_CONTINUE:
        if( !AD1_CONTINUE(pdp) )
        {
            endBusy();
            mb.repLen = 0;
            repPut((uint32_t)pdp->run);
            postReply(AD1P_ST_OK);
        }
        else if( expired )
        {
            AD1_CLEAR_CONTINUE(pdp);
            endBusy();
            mb.repLen = 0;
            repPut((uint32_t)pdp->run);
            postReply(AD1P_ST_TIMEOUT);
        }

        break;

    case OP_STEP:
        // The main loop clears the flag after the first machine cycle, not after the
        // instruction. With single-step set the machine drops run when the instruction retires,
        // so both must be seen before the next step is issued or the PC would be re-entered
        // mid-instruction.
        if( !AD1_CONTINUE(pdp) && !pdp->run )
        {
            ++opStepsDone;
            if( AD1_BREAKPOINT_HIT(pdp) || AD1_WATCH_HIT(pdp) )
            {
                stepFinish(pdp, AD1P_END_HIT, AD1P_ST_OK);
            }
            else if( opStepsLeft > 0 )
            {
                stepIssue(pdp);
            }
            else
            {
                stepFinish(pdp, AD1P_END_DONE, AD1P_ST_OK);
            }
        }
        else if( expired )
        {
            AD1_CLEAR_CONTINUE(pdp);
            stepFinish(pdp, AD1P_END_TIMEOUT, AD1P_ST_TIMEOUT);
        }

        break;

    case OP_WRITE:
        if( !AD1_STOP(pdp) && !pdp->run )
        {
            uint32_t words;

            endBusy();
            blocksValid(reqBufP, mb.reqLen, &words);
            blocksApply(pdp, reqBufP);
            mb.repLen = 0;
            repPut(opWasRunning);
            repPut(words);
            postReply(AD1P_ST_OK);
        }
        else if( expired )
        {
            AD1_CLEAR_STOP(pdp);
            endBusy();
            mb.repLen = 0;
            repPut(opWasRunning);
            repPut(0);
            postReply(AD1P_ST_TIMEOUT);
        }

        break;

    default:
        endBusy();
        break;
    }
}

// Set up one of the breakpoint or watch tables' first free entry. Returns the 0-based index,
// or -1 if the table is full.
static int
freeBreakpoint(PDP1 *pdp)
{
int i;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
    {
        if( !pdp->ad1Breakpoints[i].isSet )
        {
            return( i );
        }
    }

    return( -1 );
}

static int
freeWatch(PDP1 *pdp)
{
int i;

    for( i = 0; i < AD1_NUM_WATCHES; ++i )
    {
        if( !pdp->ad1Watches[i].isSet )
        {
            return( i );
        }
    }

    return( -1 );
}

static void
clearBreakpointEntry(BreakpointP brkP)
{
    brkP->isSet = false;
    brkP->isEnabled = false;
    brkP->address = 0;
}

static void
clearWatchEntry(WatchP watchP)
{
    watchP->isSet = false;
    watchP->isEnabled = false;
    watchP->address = 0;
    watchP->value = 0;
}

// The six breakpoint requests.
static void
opBreakpoint(PDP1 *pdp, uint16_t type, const uint8_t *pP, uint32_t len)
{
BreakpointP brkP;
uint32_t number;
int slot;
int i;

    switch( type )
    {
    case AD1P_BP_SET:
        if( len != 8 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            return;
        }

        if( (slot = freeBreakpoint(pdp)) < 0 )
        {
            replyStatus(AD1P_ST_NO_SLOT);
            return;
        }

        brkP = &pdp->ad1Breakpoints[slot];
        brkP->isSet = true;
        brkP->isEnabled = true;
        brkP->number = slot + 1;
        brkP->address = (rd32(pP) & 0177777);
        brkP->count = (int)rd32(pP + 4);
        brkP->curCount = 0;
        syncTableFlags(pdp);
        mb.repLen = 0;
        repPut((uint32_t)(slot + 1));
        postReply(AD1P_ST_OK);
        return;

    case AD1P_BP_LIST:
        mb.repLen = 0;
        repPut(AD1_NUM_BREAKPOINTS);
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            brkP = &pdp->ad1Breakpoints[i];
            repPut(brkP->isSet ? 1 : 0);
            repPut(brkP->isEnabled ? 1 : 0);
            repPut((uint32_t)brkP->number);
            repPut(brkP->address);
            repPut((uint32_t)brkP->count);
            repPut((uint32_t)brkP->curCount);
        }

        postReply(AD1P_ST_OK);
        return;

    default:
        break;
    }

    if( len != 4 )
    {
        replyStatus(AD1P_ST_BAD_REQUEST);
        return;
    }

    number = rd32(pP);
    if( number > AD1_NUM_BREAKPOINTS )
    {
        replyStatus(AD1P_ST_BAD_ARG);
        return;
    }

    if( (type == AD1P_BP_DELETE) && (number == 0) )
    {
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            clearBreakpointEntry(&pdp->ad1Breakpoints[i]);
        }

        syncTableFlags(pdp);
        replyStatus(AD1P_ST_OK);
        return;
    }

    if( number == 0 )
    {
        replyStatus(AD1P_ST_BAD_ARG);
        return;
    }

    brkP = &pdp->ad1Breakpoints[number - 1];
    if( !brkP->isSet )
    {
        replyStatus(AD1P_ST_NOT_SET);
        return;
    }

    if( type == AD1P_BP_DELETE )
    {
        clearBreakpointEntry(brkP);
        syncTableFlags(pdp);
    }
    else if( type == AD1P_BP_ENABLE )
    {
        if( brkP->isEnabled )
        {
            replyStatus(AD1P_ST_ALREADY);
            return;
        }

        brkP->isEnabled = true;
    }
    else
    {
        if( !brkP->isEnabled )
        {
            replyStatus(AD1P_ST_ALREADY);
            return;
        }

        brkP->isEnabled = false;
    }

    replyStatus(AD1P_ST_OK);
}

// The six watch requests.
static void
opWatch(PDP1 *pdp, uint16_t type, const uint8_t *pP, uint32_t len)
{
WatchP watchP;
uint32_t number;
int slot;
int i;

    switch( type )
    {
    case AD1P_WATCH_SET:
        if( len != 12 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            return;
        }

        if( (slot = freeWatch(pdp)) < 0 )
        {
            replyStatus(AD1P_ST_NO_SLOT);
            return;
        }

        watchP = &pdp->ad1Watches[slot];
        watchP->isSet = true;
        watchP->number = slot + 1;
        watchP->address = (rd32(pP) & 0177777);
        watchP->lastVal = (int)wordAt(pdp, watchP->address);
        watchP->onAny = (rd32(pP + 4) != 0);
        watchP->value = watchP->onAny ? 0 : (int)rd32(pP + 8);
        watchP->isEnabled = true;
        syncTableFlags(pdp);
        mb.repLen = 0;
        repPut((uint32_t)(slot + 1));
        postReply(AD1P_ST_OK);
        return;

    case AD1P_WATCH_LIST:
        mb.repLen = 0;
        repPut(AD1_NUM_WATCHES);
        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            watchP = &pdp->ad1Watches[i];
            repPut(watchP->isSet ? 1 : 0);
            repPut(watchP->isEnabled ? 1 : 0);
            repPut(watchP->onAny ? 1 : 0);
            repPut((uint32_t)watchP->number);
            repPut(watchP->address);
            repPut((uint32_t)watchP->value);
            repPut((uint32_t)watchP->lastVal);
        }

        postReply(AD1P_ST_OK);
        return;

    default:
        break;
    }

    if( len != 4 )
    {
        replyStatus(AD1P_ST_BAD_REQUEST);
        return;
    }

    number = rd32(pP);
    if( number > AD1_NUM_WATCHES )
    {
        replyStatus(AD1P_ST_BAD_ARG);
        return;
    }

    if( (type == AD1P_WATCH_DELETE) && (number == 0) )
    {
        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            clearWatchEntry(&pdp->ad1Watches[i]);
        }

        syncTableFlags(pdp);
        replyStatus(AD1P_ST_OK);
        return;
    }

    if( number == 0 )
    {
        replyStatus(AD1P_ST_BAD_ARG);
        return;
    }

    watchP = &pdp->ad1Watches[number - 1];
    if( !watchP->isSet )
    {
        replyStatus(AD1P_ST_NOT_SET);
        return;
    }

    if( type == AD1P_WATCH_DELETE )
    {
        clearWatchEntry(watchP);
        syncTableFlags(pdp);
    }
    else if( type == AD1P_WATCH_ENABLE )
    {
        if( watchP->isEnabled )
        {
            replyStatus(AD1P_ST_ALREADY);
            return;
        }

        // Refresh the remembered value so the watch does not fire for a change made while
        // it was disabled.
        watchP->isEnabled = true;
        watchP->lastVal = (int)wordAt(pdp, watchP->address);
    }
    else
    {
        if( !watchP->isEnabled )
        {
            replyStatus(AD1P_ST_ALREADY);
            return;
        }

        watchP->isEnabled = false;
    }

    replyStatus(AD1P_ST_OK);
}

// Apply the disconnect policy the client chose.
static void
applyPolicy(PDP1 *pdp)
{
int i;

    if( policy == AD1P_POLICY_DELETE_ALL )
    {
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            clearBreakpointEntry(&pdp->ad1Breakpoints[i]);
        }

        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            clearWatchEntry(&pdp->ad1Watches[i]);
        }

        syncTableFlags(pdp);
    }
    else if( policy == AD1P_POLICY_DISABLE_ALL )
    {
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            pdp->ad1Breakpoints[i].isEnabled = false;
        }

        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            pdp->ad1Watches[i].isEnabled = false;
        }
    }
}

// Serve the request in the mailbox.
static void
execRequest(PDP1 *pdp)
{
uint16_t type;
uint32_t len;
const uint8_t *pP;
uint32_t v;
int i;

    atomic_store(&mb.state, MB_TAKEN);
    type = mb.type;
    len = mb.reqLen;
    pP = reqBufP;

    switch( type )
    {
    case AD1P_PING:
        if( len > AD1P_MAX_PING )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        memcpy(repBufP, pP, len);
        mb.repLen = len;
        postReply(AD1P_ST_OK);
        break;

    case AD1P_GET_STATE:
        opGetState(pdp);
        break;

    case AD1P_SET_REG:
        if( len != 12 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        opSetReg(pdp, pP);
        break;

    case AD1P_READ_MEM:
        if( len != 8 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        opReadMem(pdp, pP);
        break;

    case AD1P_WRITE_MEM:
        opWriteMem(pdp, pP, len);
        break;

    case AD1P_START:
        if( len != 4 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        v = rd32(pP);
        if( v >= AD1P_MEM_WORDS )
        {
            replyText(AD1P_ST_BAD_ARG, "the address is outside memory");
            break;
        }

        if( !pdp->power_sw )
        {
            replyText(AD1P_ST_BAD_STATE, "the power is off");
            break;
        }

        pdp->run_enable = 0;
        pdp->ad1StartAddr = (int)(v & ADDRMASK);
        pdp->ad1ExtendedAddr = (int)(v & EXTMASK);
        AD1_CLEAR_SINGLE(pdp);
        AD1_SET_START(pdp);
        beginBusy(OP_START);
        break;

    case AD1P_STOP:
        AD1_CLEAR_SINGLE(pdp);
        AD1_SET_STOP(pdp);
        beginBusy(OP_STOP);
        break;

    case AD1P_CONTINUE:
        if( !pdp->power_sw )
        {
            replyText(AD1P_ST_BAD_STATE, "the power is off");
            break;
        }

        AD1_CLEAR_SINGLE(pdp);
        AD1_SET_CONTINUE(pdp);
        beginBusy(OP_CONTINUE);
        break;

    case AD1P_STEP:
        if( len != 8 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        opStep(pdp, pP);
        break;

    case AD1P_CLEAR_SINGLE:
        AD1_CLEAR_SINGLE(pdp);
        replyStatus(AD1P_ST_OK);
        break;

    case AD1P_BP_SET:
    case AD1P_BP_DELETE:
    case AD1P_BP_ENABLE:
    case AD1P_BP_DISABLE:
    case AD1P_BP_LIST:
        opBreakpoint(pdp, type, pP, len);
        break;

    case AD1P_WATCH_SET:
    case AD1P_WATCH_DELETE:
    case AD1P_WATCH_ENABLE:
    case AD1P_WATCH_DISABLE:
    case AD1P_WATCH_LIST:
        opWatch(pdp, type, pP, len);
        break;

    case AD1P_DISABLE_ALL:
        for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i )
        {
            pdp->ad1Breakpoints[i].isEnabled = false;
        }

        for( i = 0; i < AD1_NUM_WATCHES; ++i )
        {
            pdp->ad1Watches[i].isEnabled = false;
        }

        replyStatus(AD1P_ST_OK);
        break;

    case AD1P_ACK_HIT:
        if( len != 4 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        v = rd32(pP);
        if( v & AD1P_HIT_BREAK )
        {
            AD1_CLEAR_BREAKPOINT_HIT(pdp);
            reportedBrk = false;
        }

        if( v & AD1P_HIT_WATCH )
        {
            AD1_CLEAR_WATCH_HIT(pdp);
            reportedWatch = false;
        }

        replyStatus(AD1P_ST_OK);
        break;

    case AD1P_SET_POLICY:
        if( len != 4 )
        {
            replyStatus(AD1P_ST_BAD_REQUEST);
            break;
        }

        v = rd32(pP);
        if( v > AD1P_POLICY_DISABLE_ALL )
        {
            replyStatus(AD1P_ST_BAD_ARG);
            break;
        }

        policy = (int)v;
        replyStatus(AD1P_ST_OK);
        break;

    default:
        replyStatus(AD1P_ST_BAD_REQUEST);
        break;
    }
}

// Send a hit event for each latch that is set and has not been reported to this client yet.
// The latch itself stays set until the client acknowledges it, so a hit that happens with no
// client connected is delivered when one arrives.
static void
reportHits(PDP1 *pdp)
{
uint32_t words[4];
int slot;

    if( AD1_BREAKPOINT_HIT(pdp) && !reportedBrk )
    {
        slot = pdp->ad1brkNo;
        words[0] = (uint32_t)(slot + 1);
        words[1] = pdp->ad1Breakpoints[slot].address;
        words[2] = wordAt(pdp, words[1]);
        words[3] = (uint32_t)pdp->ad1Breakpoints[slot].count;
        pushEvent(AD1P_EVT_HIT_BREAK, words, 4);
        reportedBrk = true;
    }

    if( AD1_WATCH_HIT(pdp) && !reportedWatch )
    {
        slot = pdp->ad1watchNo;
        words[0] = (uint32_t)(slot + 1);
        words[1] = pdp->ad1Watches[slot].address;
        words[2] = wordAt(pdp, words[1]);
        pushEvent(AD1P_EVT_HIT_WATCH, words, 3);
        reportedWatch = true;
    }
}

// Called by the main loop when a breakpoint or watch has just stopped the machine.
void
ad1NoteHit(PDP1 *pdp)
{
    if( sessionKnown )
    {
        reportHits(pdp);
    }
}

// The client is gone: abandon any operation in progress, drop the sticky single-step state
// (a stepping debugger must not leave it behind), apply the client's disconnect policy, and let
// the I/O thread accept the next client.
static void
sessionClose(PDP1 *pdp)
{
    if( opKind != OP_NONE )
    {
        endBusy();
    }

    atomic_store(&mb.state, MB_EMPTY);
    AD1_CLEAR_SINGLE(pdp);
    applyPolicy(pdp);
    policy = AD1P_POLICY_KEEP;
    sessionKnown = false;
    subscribedRun = false;
    reportedBrk = false;
    reportedWatch = false;
    atomic_fetch_and(&ad1Work, ~AD1_WORK_RUNSTATE);
    atomic_store(&clientState, CS_NONE);
    wakeIoThread();
}

// A new client is ready: forget what was reported to the last one and send any hit that is
// still latched.
static void
sessionOpen(PDP1 *pdp)
{
    sessionKnown = true;
    reportedBrk = false;
    reportedWatch = false;
    subscribedRun = ((atomic_load(&clientSubscribe) & AD1P_SUB_RUN_STATE) != 0);
    lastRun = (pdp->run != 0);
    if( subscribedRun )
    {
        atomic_fetch_or(&ad1Work, AD1_WORK_RUNSTATE);
    }

    reportHits(pdp);
}

// Send a run-state event when the machine starts or stops.
static void
runStatePoll(PDP1 *pdp)
{
uint32_t words[2];

    if( (pdp->run != 0) != lastRun )
    {
        lastRun = (pdp->run != 0);
        words[0] = (uint32_t)lastRun;
        words[1] = fullPc(pdp);
        pushEvent(AD1P_EVT_RUN_STATE, words, 2);
    }
}

// The one place the emulator thread serves the network client. Called from the main loop when
// ad1Work is nonzero, and after a throttle sleep is cut short.
void
ad1Service(PDP1 *pdp)
{
int state;

    // Clear the hints before looking at what they point to, so work posted after this point
    // sets them again.
    atomic_fetch_and(&ad1Work, ~(AD1_WORK_REQUEST | AD1_WORK_CONN));

    state = atomic_load(&clientState);
    if( state == CS_CLOSING )
    {
        sessionClose(pdp);
        return;
    }

    if( (state == CS_READY) && !sessionKnown )
    {
        sessionOpen(pdp);
    }

    if( opKind != OP_NONE )
    {
        opPoll(pdp);
    }
    else if( (state == CS_READY) && (atomic_load(&mb.state) == MB_REQUEST) )
    {
        execRequest(pdp);
    }

    if( subscribedRun )
    {
        runStatePoll(pdp);
    }
}

// The throttle wait, as a drop-in for the plain sleep loop: sleep in one-millisecond steps until
// real time catches up with simulated time, but let a request end a sleep so it is served at
// once instead of after up to a millisecond. Simulated time is not advanced here.
void
ad1Throttle(PDP1 *pdp)
{
struct pollfd pfd;
struct timespec oneMs;

    while( pdp->realtime < pdp->simtime )
    {
        if( serverActive )
        {
            pfd.fd = wakeFd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            oneMs.tv_sec = 0;
            oneMs.tv_nsec = 1000000;
            if( ppoll(&pfd, 1, &oneMs, NULL) > 0 )
            {
                drainEventFd(wakeFd);
                ad1Service(pdp);
            }
        }
        else
        {
            usleep(1000);
        }

        pdp->realtime = gettime();
    }
}
