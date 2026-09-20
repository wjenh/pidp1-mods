// What ad1 talks to, a running emulator over the debugger link (ad1link), or, in test mode, a
// local image of memory and registers with no processor behind it.
// The rest of ad1 calls only/ these functions, so it does not know which one it has.
//
// Every function returns the server's status, the localimage imitates the ones that apply.
// A lost connection ends the program here with a message, callers never see one.
#ifndef TARGET_H
#define TARGET_H

#include <stdbool.h>
#include <stdint.h>

#include "ad1link.h"

// Connect to the emulator at host[:port] (NULL is localhost). Returns 0, or -1 with the reason
// in ad1LinkError().
int tgtOpen(const char *hostSpec);

// Use a local image instead. The memory is empty until the caller fills tgtLocalCore().
void tgtOpenLocal(void);
uint32_t *tgtLocalCore(void);

bool tgtIsLocal(void);

// Leave the emulator: apply the policy for the breakpoints and watches, and close the link. Safe
// to call more than once or after the link is gone.
void tgtClose(int policy);

// The socket to select() on, or -1 in test mode.
int tgtFd(void);

// Read and queue whatever the emulator has sent. Returns 1 if a hit event is waiting.
int tgtPump(void);

// The next queued event, or 0 if none.
int tgtNextEvent(Ad1Event *evP);

uint32_t tgtRead(uint32_t address);
int tgtWrite(uint32_t address, uint32_t value);
int tgtWriteBlocks(const Ad1Block *blocksP, uint32_t nBlocks, const uint32_t *wordsP, bool stopFirst,
    bool *wasRunningP);

// state[] is indexed by AD1P_STATE_ words, AD1P_STATE_WORDS of them.
int tgtGetState(uint32_t *state);
int tgtSetReg(uint32_t reg, uint32_t op, uint32_t value);

int tgtStart(uint32_t address);
int tgtStop(uint32_t *pcP);
int tgtContinue(void);
int tgtStep(uint32_t count, bool record, Ad1StepResult *resP);
int tgtClearSingle(void);

int tgtBpSet(uint32_t address, uint32_t count, uint32_t *numberP);
int tgtBpDelete(uint32_t number);
int tgtBpEnable(uint32_t number);
int tgtBpDisable(uint32_t number);
int tgtBpList(Ad1BpEntry *entriesP);            // AD1P_NUM_BREAKPOINTS entries

int tgtWatchSet(uint32_t address, bool onAnyChange, uint32_t value, uint32_t *numberP);
int tgtWatchDelete(uint32_t number);
int tgtWatchEnable(uint32_t number);
int tgtWatchDisable(uint32_t number);
int tgtWatchList(Ad1WatchEntry *entriesP);      // AD1P_NUM_WATCHES entries

int tgtAckHit(uint32_t mask);
int tgtSetPolicy(uint32_t policy);
int tgtDisableAll(void);

#endif
