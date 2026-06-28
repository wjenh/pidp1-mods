// This include defines data structures for the emulation of the Type 19 High Speed Channel Control.

#ifndef HSC_H
#define HSC_H
#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>

// Statuses that can be returned
#define HSC_OK      0
#define HSC_BUSY    1
#define HSC_DONE    2
#define HSC_ABORT   3       // an HSCreset() was done, comes from a stop, start, examine, continue
#define HSC_ERR     -1

// These are bitflags in the mode field
#define HSC_MODE_FROMMEM       001  // from core memory to user space
#define HSC_MODE_TOMEM         002  // from user space to core memory
#define HSC_MODE_IMMEDIATE     004  // bypass all the wait states, immediate execution, no reschedule
#define HSC_MODE_THREADED      010  // immediate execution but check busy and imitate timing
#define HSC_MODE_UPDATEPANEL   020  // only for immediate, hsc controls the hsc cycle light

typedef struct {
    int mode;           // current operation mode, from, to, or both
    int count;          // number of words to transfer
    int memBank;        // memory bank to transfer to/from, 0-15 dec
    int memAddr;        // address offset in memory, offset in bank, 0-4095
    uint32_t *fromBufferP;  // should be 4k unless you're sure your count won't exceed the size
    uint32_t *toBufferP;    // ditto
    } HSCRequest, *HSCRequestP;

// This is just to control access, no execution requests by direct channel number.
typedef struct {
    int chanNo;
    } HSCChannel, *HSCChannelP;

// User methods.
// Get or free a channel, it is held until freed.
// The actual hardware dedicated channels to particular devices, but we don't.
// The device IOTs take control of a channel for as long as they need it.
// A null channel pointer is returned if a channel can't be allocated.
HSCChannelP HSCallocateChannel(int chanNo);
bool HSCfreeChannel(HSCChannelP channelP);

// Do a channel operation, a read, write, or read/write.
int HSCexecute(HSCChannelP channelP, HSCRequestP requestP);

int HSCwait(HSCChannelP channelP);       // wait for completion of a request
int HSCgetStatus(HSCChannelP channelP);   // returns one of the HSC statuses
#endif
