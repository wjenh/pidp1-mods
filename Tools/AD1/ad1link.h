// Client side of the emulator's debugger, shared by ad1 and fastload.
// One connection, one request in flight, a typed wrapper for each request.
//
// Every wrapper returns AD1L_COMM if there is no connection or no reply, with the reason in
// ad1LinkError().
// Otherwise it returns the server's status, AD1P_ST_OK or one of the other AD1P_ST_ codes.
#ifndef AD1LINK_H
#define AD1LINK_H

#include <stdint.h>
#include <stddef.h>

#include "ad1proto.h"

#define AD1L_COMM (-1)

#define AD1L_MAX_EVENTS 16
#define AD1L_MAX_HOST 128

// A pushed event. words[] holds the payload words: HIT_BREAK number, address, word, count;
// HIT_WATCH number, address, value; RUN_STATE run, pc.
typedef struct
{
    uint16_t type;
    uint32_t nWords;
    uint32_t words[4];
} Ad1Event;

typedef struct
{
    int fd;
    uint32_t nextId;
    char host[AD1L_MAX_HOST];
    int port;
    int replyTimeoutMs;

    // What the server said in its HELLO reply.
    uint32_t memWords;
    uint32_t nBreakpoints;
    uint32_t nWatches;
    uint32_t maxRequest;
    uint32_t maxReply;
    char build[64];

    Ad1Event events[AD1L_MAX_EVENTS];
    int evHead;
    int evCount;
    uint32_t evDropped;             // events that arrived with the queue full

    uint8_t *replyP;                // the last reply's payload, valid until the next request
    uint32_t replyLen;
    uint32_t *recordsP;             // (pc, word) pairs of the last STEP
} Ad1Link;

// One block of a memory write: the words are in a single array, block after block.
typedef struct
{
    uint32_t address;
    uint32_t count;
} Ad1Block;

typedef struct
{
    uint32_t done;                  // steps that ran
    uint32_t reason;                // AD1P_END_DONE, _HIT or _TIMEOUT
    uint32_t pc;
    uint32_t word;                  // the word at pc
    uint32_t nRecords;
    const uint32_t *recordsP;       // nRecords pairs, (pc, word), taken before each step
} Ad1StepResult;

typedef struct
{
    int isSet;
    int isEnabled;
    uint32_t number;
    uint32_t address;
    uint32_t count;
    uint32_t curCount;
} Ad1BpEntry;

typedef struct
{
    int isSet;
    int isEnabled;
    int onAnyChange;
    uint32_t number;
    uint32_t address;
    uint32_t value;
    uint32_t lastValue;
} Ad1WatchEntry;

// Split "host", "host:port" or ":port" into a host and a port. A missing host is localhost. A
// host with no port gets the loopback port if it is a loopback name or address, else the
// remote one. Returns 0, or AD1L_COMM if the text is not one of those.
int ad1LinkParseHost(const char *specP, char *hostP, size_t hostSize, int *portP);

// The text of the last failure. It names the host and port that were tried.
const char *ad1LinkError(void);

// Connect to a host given as for ad1LinkParseHost(), do the handshake, and fill in *lp.
// subscribe is a mask of AD1P_SUB_ bits. Returns 0 or AD1L_COMM.
int ad1LinkOpen(Ad1Link *lp, const char *specP, uint32_t subscribe, const char *clientNameP);

void ad1LinkClose(Ad1Link *lp);

// The socket, for a select() that also watches stdin.
int ad1LinkFd(const Ad1Link *lp);

// Read whatever the server has sent, without blocking for more than a partial frame. Events
// are queued. Returns 0, or AD1L_COMM if the connection is gone.
int ad1LinkPump(Ad1Link *lp);

// The next queued event; returns 1 and fills *evP, or 0 if there is none.
int ad1LinkNextEvent(Ad1Link *lp, Ad1Event *evP);

int ad1Ping(Ad1Link *lp);
int ad1GetState(Ad1Link *lp, uint32_t *stateP);
int ad1SetReg(Ad1Link *lp, uint32_t reg, uint32_t op, uint32_t value);
int ad1ReadMem(Ad1Link *lp, uint32_t address, uint32_t count, uint32_t *wordsP);
int ad1WriteMem(Ad1Link *lp, uint32_t address, uint32_t count, const uint32_t *wordsP);

// One write of several blocks, all or nothing, optionally stopping a running machine first.
// *wasRunningP and *writtenP may be NULL.
int ad1WriteBlocks(Ad1Link *lp, const Ad1Block *blocksP, uint32_t nBlocks, const uint32_t *wordsP,
    int stopFirst, uint32_t *wasRunningP, uint32_t *writtenP);

// Run control. *runP and *pcP may be NULL.
int ad1Start(Ad1Link *lp, uint32_t address, uint32_t *runP, uint32_t *pcP);
int ad1Stop(Ad1Link *lp, uint32_t *runP, uint32_t *pcP);
int ad1Continue(Ad1Link *lp, uint32_t *runP);
int ad1Step(Ad1Link *lp, uint32_t count, int record, Ad1StepResult *resP);
int ad1ClearSingle(Ad1Link *lp);

int ad1BpSet(Ad1Link *lp, uint32_t address, uint32_t count, uint32_t *numberP);
int ad1BpDelete(Ad1Link *lp, uint32_t number);        // 0 deletes them all
int ad1BpEnable(Ad1Link *lp, uint32_t number);
int ad1BpDisable(Ad1Link *lp, uint32_t number);
int ad1BpList(Ad1Link *lp, Ad1BpEntry *entriesP, uint32_t max, uint32_t *nP);

int ad1WatchSet(Ad1Link *lp, uint32_t address, int onAnyChange, uint32_t value, uint32_t *numberP);
int ad1WatchDelete(Ad1Link *lp, uint32_t number);
int ad1WatchEnable(Ad1Link *lp, uint32_t number);
int ad1WatchDisable(Ad1Link *lp, uint32_t number);
int ad1WatchList(Ad1Link *lp, Ad1WatchEntry *entriesP, uint32_t max, uint32_t *nP);

int ad1DisableAll(Ad1Link *lp);
int ad1AckHit(Ad1Link *lp, uint32_t mask);
int ad1SetPolicy(Ad1Link *lp, uint32_t policy);

// The text for a status code, for the ones a caller does not word itself.
const char *ad1StatusText(int status);

#endif
