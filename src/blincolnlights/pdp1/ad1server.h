// Interface between the emulator's main loop and the debugger server (ad1server.c).
// The server owns a network thread; everything that reads or writes machine state runs on the
// emulator thread, through ad1Service(), so a request is always applied between two cycles.
#ifndef AD1SERVER_H
#define AD1SERVER_H

#include <stdatomic.h>

#include "pdp1.h"
#include "configuration.h"

// Bits of ad1Work: hints that ad1Service() has something to do. The state they point at is
// authoritative, so a stale bit costs one empty call and never loses work.
#define AD1_WORK_REQUEST 0x1        // a request is waiting in the mailbox
#define AD1_WORK_CONN 0x2           // a client connected or went away
#define AD1_WORK_BUSY 0x4           // a multi-pass operation is in progress
#define AD1_WORK_RUNSTATE 0x8       // the client wants run-state events

extern _Atomic int ad1Work;

// One relaxed load; this is the only cost of the server on the emulator thread when idle.
#define ad1WorkPending() (atomic_load_explicit(&ad1Work, memory_order_relaxed) != 0)

void ad1ServerStart(PDP1 *pdp, ConfigurationP configP);
void ad1Service(PDP1 *pdp);
void ad1NoteHit(PDP1 *pdp);
void ad1Throttle(PDP1 *pdp);

#endif
