// writeBehind.h -- a write-behind queue for device plugins whose file writes must not hold the
// emulator thread.
//
// A queue has its own writer thread, which does the queued operations on their files in the
// order they were posted. The emulator thread posts and goes on; only a full queue makes it
// wait, for room. A write under a heavy load on the card or disk can take seconds, and a
// plugin's handler and polls run on the emulator thread, so a write done there freezes the CPU,
// the panel, the audio and ad1 together for as long.
//
// 27-Sep-2026 Claude initial version.

#ifndef WRITEBEHIND_H
#define WRITEBEHIND_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct WbQueue WbQueue, *WbQueueP;

// Called on the writer thread when a queued write or truncate has been done: argP as posted,
// the fd it was done on, and whether it succeeded. It must not post to its own queue.
typedef void (*WbDoneFn)(void *argP, int fd, bool ok);

// Makes a queue holding at most maxBytes of queued data, and starts its writer thread.
// Returns the queue, or NULL if it could not be made; a plugin then does its writes itself.
WbQueueP wbCreate(size_t maxBytes);

// Queues a pwrite() of len bytes from dataP at offset on fd; the bytes are copied, so dataP may
// change as soon as this returns. doneP, if not NULL, is called with argP once it is done.
// Waits for room if the queue is full. No return value: a failure reaches doneP.
void wbWrite(WbQueueP qP, int fd, const void *dataP, size_t len, off_t offset, WbDoneFn doneP, void *argP);

// Queues an ftruncate() of fd to len bytes, after the writes queued before it. doneP as for
// wbWrite(). No return value.
void wbTruncate(WbQueueP qP, int fd, off_t len, WbDoneFn doneP, void *argP);

// Queues a close() of fd, after everything queued before it on the queue. No return value.
void wbClose(WbQueueP qP, int fd);

// Waits until everything queued so far has been done. No return value.
void wbDrain(WbQueueP qP);

#endif
