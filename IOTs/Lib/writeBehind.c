// writeBehind.c -- a write-behind queue for device plugins; see writeBehind.h.
//
// The queued operations are a linked list, each write carrying its own copy of the data. The
// writer thread takes one at a time off the head and does it with the lock released, so posting
// never waits on the disk, only on room. At exit a destructor lets every queue's writer finish
// what is queued and end, so nothing posted is lost to a normal exit; a write posted after that
// is done by the caller.
//
// 27-Sep-2026 Claude initial version.

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "writeBehind.h"

#define WB_WRITE 0
#define WB_TRUNCATE 1
#define WB_CLOSE 2

#define WB_MAXQUEUES 8          // queues one plugin may make

typedef struct WbOp
{
    struct WbOp *nextP;
    int kind;                   // WB_WRITE, WB_TRUNCATE or WB_CLOSE
    int fd;
    off_t offset;               // WB_WRITE: where the data goes; WB_TRUNCATE: the new length
    size_t len;                 // WB_WRITE: bytes in data, else 0
    WbDoneFn doneP;
    void *argP;
    char data[];                // WB_WRITE only
} WbOp, *WbOpP;

struct WbQueue
{
    pthread_mutex_t lock;
    pthread_cond_t workCond;    // an op was posted, or the queue is stopping
    pthread_cond_t roomCond;    // an op was done, so there is room or the queue may be empty
    pthread_t thread;
    WbOpP headP;
    WbOpP tailP;
    size_t queuedBytes;         // data in queued ops, and in the one being done
    size_t maxBytes;
    bool busy;                  // the writer is doing an op it took off the queue
    bool stopping;              // the process is exiting: finish the queue and end
    bool stopped;               // the writer has ended; the caller does its ops itself
};

static WbQueueP queues[WB_MAXQUEUES];
static int nQueues;
static pthread_mutex_t queuesLock = PTHREAD_MUTEX_INITIALIZER;

static WbOpP newOp(int kind, int fd, size_t len);
static void post(WbQueueP qP, WbOpP opP);
static void doOp(WbOpP opP);
static void *writer(void *argP);
static void stopAll(void) __attribute__((destructor));

WbQueueP
wbCreate(size_t maxBytes)
{
WbQueueP qP;

    if( !(qP = (WbQueueP)calloc(1, sizeof(WbQueue))) )
    {
        return(NULL);
    }

    pthread_mutex_init(&qP->lock, NULL);
    pthread_cond_init(&qP->workCond, NULL);
    pthread_cond_init(&qP->roomCond, NULL);
    qP->maxBytes = maxBytes;

    pthread_mutex_lock(&queuesLock);
    if( (nQueues >= WB_MAXQUEUES) || (pthread_create(&qP->thread, NULL, writer, qP) != 0) )
    {
        pthread_mutex_unlock(&queuesLock);
        free(qP);
        return(NULL);
    }

    queues[nQueues++] = qP;
    pthread_mutex_unlock(&queuesLock);
    return(qP);
}

void
wbWrite(WbQueueP qP, int fd, const void *dataP, size_t len, off_t offset, WbDoneFn doneP, void *argP)
{
WbOpP opP;

    if( !(opP = newOp(WB_WRITE, fd, len)) )
    {
        if( doneP )
        {
            doneP(argP, fd, false);
        }
        return;
    }

    memcpy(opP->data, dataP, len);
    opP->offset = offset;
    opP->doneP = doneP;
    opP->argP = argP;
    post(qP, opP);
}

void
wbTruncate(WbQueueP qP, int fd, off_t len, WbDoneFn doneP, void *argP)
{
WbOpP opP;

    if( !(opP = newOp(WB_TRUNCATE, fd, 0)) )
    {
        if( doneP )
        {
            doneP(argP, fd, false);
        }
        return;
    }

    opP->offset = len;
    opP->doneP = doneP;
    opP->argP = argP;
    post(qP, opP);
}

void
wbClose(WbQueueP qP, int fd)
{
WbOpP opP;

    if( !(opP = newOp(WB_CLOSE, fd, 0)) )
    {
        wbDrain(qP);            // no memory for the op: close it here, after what is queued
        close(fd);
        return;
    }

    post(qP, opP);
}

void
wbDrain(WbQueueP qP)
{
    pthread_mutex_lock(&qP->lock);
    while( (qP->headP || qP->busy) && !qP->stopped )
    {
        pthread_cond_wait(&qP->roomCond, &qP->lock);
    }
    pthread_mutex_unlock(&qP->lock);
}

// An op of the given kind with room for len bytes of data, or NULL if memory ran out.
static WbOpP
newOp(int kind, int fd, size_t len)
{
WbOpP opP;

    if( !(opP = (WbOpP)calloc(1, sizeof(WbOp) + len)) )
    {
        return(NULL);
    }

    opP->kind = kind;
    opP->fd = fd;
    opP->len = len;
    return(opP);
}

// Puts opP on the queue, waiting for room if it is full. An op larger than the whole queue goes
// on once the queue is empty. Once the process is exiting the queue takes nothing more, so the
// writer can finish: the op waits for it to end, keeping the order, and is done here. No return
// value.
static void
post(WbQueueP qP, WbOpP opP)
{
    pthread_mutex_lock(&qP->lock);
    while( !qP->stopped
        && (qP->stopping || ((qP->headP || qP->busy) && ((qP->queuedBytes + opP->len) > qP->maxBytes))) )
    {
        pthread_cond_wait(&qP->roomCond, &qP->lock);
    }

    if( qP->stopped )
    {
        pthread_mutex_unlock(&qP->lock);
        doOp(opP);
        free(opP);
        return;
    }

    if( qP->tailP )
    {
        qP->tailP->nextP = opP;
    }
    else
    {
        qP->headP = opP;
    }

    qP->tailP = opP;
    qP->queuedBytes += opP->len;
    pthread_cond_signal(&qP->workCond);
    pthread_mutex_unlock(&qP->lock);
}

// Does one op, then tells its doneP. No return value.
static void
doOp(WbOpP opP)
{
bool ok;
size_t done;
ssize_t n;

    ok = true;
    switch( opP->kind )
    {
    case WB_WRITE:
        // pwrite() may write short; carry on from where it stopped.
        for( done = 0; done < opP->len; done += (size_t)n )
        {
            n = pwrite(opP->fd, (opP->data + done), (opP->len - done), (opP->offset + (off_t)done));
            if( (n < 0) && (errno == EINTR) )
            {
                n = 0;
                continue;
            }

            if( n <= 0 )
            {
                ok = false;
                break;
            }
        }
        break;

    case WB_TRUNCATE:
        ok = (ftruncate(opP->fd, opP->offset) == 0);
        break;

    default:
        close(opP->fd);
        break;
    }

    if( opP->doneP )
    {
        opP->doneP(opP->argP, opP->fd, ok);
    }
}

// The writer thread: does the queue's ops in order until the process exits.
static void *
writer(void *argP)
{
WbQueueP qP;
WbOpP opP;

    qP = (WbQueueP)argP;
    pthread_mutex_lock(&qP->lock);
    for( ;; )
    {
        while( !qP->headP && !qP->stopping )
        {
            pthread_cond_wait(&qP->workCond, &qP->lock);
        }

        if( !qP->headP )
        {
            break;                  // stopping, and nothing is left
        }

        opP = qP->headP;
        if( !(qP->headP = opP->nextP) )
        {
            qP->tailP = NULL;
        }

        qP->busy = true;
        pthread_mutex_unlock(&qP->lock);
        doOp(opP);
        pthread_mutex_lock(&qP->lock);

        qP->queuedBytes -= opP->len;
        qP->busy = false;
        free(opP);
        pthread_cond_broadcast(&qP->roomCond);
    }

    qP->stopped = true;
    pthread_cond_broadcast(&qP->roomCond);
    pthread_mutex_unlock(&qP->lock);
    return(NULL);
}

// At exit, or when the plugin is unloaded: every writer finishes its queue and ends. No
// return value.
static void
stopAll(void)
{
int i;

    pthread_mutex_lock(&queuesLock);
    for( i = 0; i < nQueues; ++i )
    {
        pthread_mutex_lock(&queues[i]->lock);
        queues[i]->stopping = true;
        pthread_cond_signal(&queues[i]->workCond);
        pthread_mutex_unlock(&queues[i]->lock);
        pthread_join(queues[i]->thread, NULL);
    }
    pthread_mutex_unlock(&queuesLock);
}
