/*
 * Paper tape reader and punch files: which fd the reader (r_fd) and punch (p_fd) use, and the
 * front panel's FEED key. The reader and punch IOTs (IOT_2, IOT_6) read and write those fds on
 * the emulator thread; everything here that can block runs on another thread.
 *
 * A new fd is posted by any thread (mountReader(), mountPunch(), the console's r and p commands,
 * the reader and punch network ports) and adopted by handleio() on the emulator thread, so r_fd
 * and p_fd are only replaced or closed by the thread that uses them.
 *
 * Every tape file is opened non-blocking, so a FIFO cannot hold the emulator thread. A FIFO with
 * no peer yet is a device that is not ready:
 * - Reader: the FIFO is mounted once a writer has opened it; until then the reader has no tape.
 *   A read with the writer idle returns EAGAIN, which the reader treats as not ready.
 * - Punch: the punch holds its own read end of the FIFO, so the open never fails and output
 *   waits in the pipe until a reader drains it. With the pipe full a write returns EAGAIN, and
 *   the punch holds the character and retries, so it is not ready.
 *
 * 27-Sep-2026 Claude moved out of pdp1.c, and tape files are opened non-blocking with FIFOs
 *    treated as not ready until their peer arrives.
*/

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "pdp1.h"
#include "papertape.h"

#define US(us) ((us)*1000 - 1)
#define PDLY US(15873)          // tape punch character delay: 63 chars/sec

#define TAPEFD_NONE (-2)        // nothing posted
#define FIFOWAITMS 200          // how often a FIFO waiter checks whether it was superseded

typedef struct
{
    int fd;                     // the FIFO, open O_RDONLY | O_NONBLOCK
    unsigned int gen;           // readerGen when the wait began
} FifoWait;

// A reader or punch fd waiting for the emulator thread to adopt it, or TAPEFD_NONE.
static int pendingReaderFd = TAPEFD_NONE;
static int pendingPunchFd = TAPEFD_NONE;

// Counts every reader mount, so a FIFO waiter mounts its tape only if no later mount came first.
// Changed and compared under tapeLock.
static unsigned int readerGen;
static pthread_mutex_t tapeLock = PTHREAD_MUTEX_INITIALIZER;

static void postTapeFd(int *pendingP, int fd);
static void adoptTapeFd(int *pendingP, int *fdP);
static bool isFifo(int fd);
static void *fifoReaderWait(void *argP);

// Post fd, or -1 to unmount, for handleio() to adopt. An fd posted earlier and not yet
// adopted is closed, since nothing will use it. Any thread.
static void
postTapeFd(int *pendingP, int fd)
{
int oldFd;

    oldFd = __atomic_exchange_n(pendingP, fd, __ATOMIC_ACQ_REL);
    if( oldFd >= 0 )
    {
        close(oldFd);
    }
}

// Adopt a posted fd into *fdP, closing the one it replaces. Emulator thread only.
// A pass with nothing posted costs one load.
static void
adoptTapeFd(int *pendingP, int *fdP)
{
int fd;

    if( __atomic_load_n(pendingP, __ATOMIC_RELAXED) == TAPEFD_NONE )
    {
        return;
    }

    fd = __atomic_exchange_n(pendingP, TAPEFD_NONE, __ATOMIC_ACQ_REL);
    if( fd == TAPEFD_NONE )
    {
        return;
    }

    if( *fdP >= 0 )
    {
        close(*fdP);
    }

    *fdP = fd;
}

// Returns true if fd is a FIFO.
static bool
isFifo(int fd)
{
struct stat st;

    return( (fstat(fd, &st) == 0) && S_ISFIFO(st.st_mode) );
}

// Mount fd in the reader, or unmount it with -1, at the emulator thread's next pass. Any wait
// for a FIFO's writer that is still pending is abandoned. Any thread.
void
mountReader(int fd)
{
    pthread_mutex_lock(&tapeLock);
    __atomic_add_fetch(&readerGen, 1, __ATOMIC_RELAXED);
    postTapeFd(&pendingReaderFd, fd);
    pthread_mutex_unlock(&tapeLock);
}

// Mount fd in the punch, or unmount it with -1, at the emulator thread's next pass. Any thread.
void
mountPunch(int fd)
{
    postTapeFd(&pendingPunchFd, fd);
}

// Waits on its own thread for a writer to open a reader FIFO, then mounts it, unless another
// mount came first. poll() reports nothing on a FIFO no writer has opened yet, so the first
// event is data or a writer that came and went.
static void *
fifoReaderWait(void *argP)
{
FifoWait *waitP;
struct pollfd pfd;
int n;
bool mounted;

    waitP = (FifoWait *)argP;
    pfd.fd = waitP->fd;
    pfd.events = POLLIN;
    mounted = false;

    for( ;; )
    {
        if( __atomic_load_n(&readerGen, __ATOMIC_RELAXED) != waitP->gen )
        {
            break;
        }

        pfd.revents = 0;
        n = poll(&pfd, 1, FIFOWAITMS);
        if( (n < 0) && (errno != EINTR) )
        {
            break;
        }

        if( n > 0 )
        {
            pthread_mutex_lock(&tapeLock);
            if( readerGen == waitP->gen )
            {
                postTapeFd(&pendingReaderFd, waitP->fd);
                mounted = true;
            }

            pthread_mutex_unlock(&tapeLock);
            break;
        }
    }

    if( !mounted )
    {
        close(waitP->fd);
    }

    free(waitP);
    return( nil );
}

// Mount the file at pathP in the reader, or unmount the reader if pathP is nil. Any thread
// but the emulator's. Returns TAPE_MOUNTED, TAPE_WAITING if pathP is a FIFO that no writer has
// opened yet (it is mounted when one does), or TAPE_FAILED if it couldn't be opened, in which
// case the reader is left with no tape.
int
mountReaderPath(const char *pathP)
{
int fd;
FifoWait *waitP;
pthread_t th;
pthread_attr_t attr;

    if( !pathP )
    {
        mountReader(-1);
        return( TAPE_MOUNTED );
    }

    if( (fd = open(pathP, (O_RDONLY | O_NONBLOCK))) < 0 )
    {
        mountReader(-1);
        return( TAPE_FAILED );
    }

    if( !isFifo(fd) )
    {
        mountReader(fd);
        return( TAPE_MOUNTED );
    }

    // A FIFO: the old tape comes out now, and the waiter mounts this one when a writer opens it.
    if( !(waitP = (FifoWait *)malloc(sizeof(FifoWait))) )
    {
        close(fd);
        mountReader(-1);
        return( TAPE_FAILED );
    }

    pthread_mutex_lock(&tapeLock);
    __atomic_add_fetch(&readerGen, 1, __ATOMIC_RELAXED);
    postTapeFd(&pendingReaderFd, -1);
    waitP->fd = fd;
    waitP->gen = __atomic_load_n(&readerGen, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&tapeLock);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if( pthread_create(&th, &attr, fifoReaderWait, waitP) != 0 )
    {
        pthread_attr_destroy(&attr);
        close(fd);
        free(waitP);
        return( TAPE_FAILED );
    }

    pthread_attr_destroy(&attr);
    return( TAPE_WAITING );
}

// Mount the file at pathP in the punch, created or truncated, or unmount the punch if pathP is
// nil. Any thread but the emulator's. Returns TAPE_MOUNTED, or TAPE_FAILED if it couldn't be
// opened, in which case the punch is left with no tape.
int
mountPunchPath(const char *pathP)
{
int fd;
int rwFd;

    if( !pathP )
    {
        mountPunch(-1);
        return( TAPE_MOUNTED );
    }

    // A FIFO with no reader refuses a non-blocking write-only open with ENXIO. Opened read-write,
    // the punch is its own reader, so the open succeeds, output waits in the pipe, and a reader
    // that leaves makes the punch not ready rather than losing characters to EPIPE.
    fd = open(pathP, (O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK), 0644);
    if( (fd < 0) && (errno == ENXIO) )
    {
        fd = open(pathP, (O_RDWR | O_NONBLOCK));
    }
    else if( (fd >= 0) && isFifo(fd) )
    {
        if( (rwFd = open(pathP, (O_RDWR | O_NONBLOCK))) >= 0 )
        {
            close(fd);
            fd = rwFd;
        }
    }

    mountPunch(fd);
    return( (fd >= 0) ? TAPE_MOUNTED : TAPE_FAILED );
}

// Adopts any reader or punch fd posted by mountReader()/mountPunch(), then drives the
// FEED key's blank tape to the punch. Called once a pass while the power is on.
void
handleio(PDP1 *pdp)
{
char c;
ssize_t wr;

    adoptTapeFd(&pendingReaderFd, &pdp->r_fd);
    adoptTapeFd(&pendingPunchFd, &pdp->p_fd);

    if( pdp->tape_feed && (pdp->feed_time < pdp->simtime) )
    {
        pdp->feed_time = (pdp->simtime + PDLY);

        if( pdp->p_fd >= 0 )
        {
            c = 0;
            wr = write(pdp->p_fd, &c, 1);       // best-effort blank-tape feed byte
            (void)wr;
        }
    }
}
