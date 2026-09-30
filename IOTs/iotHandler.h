// Primary include file for IOTs.
// It needs to also bring in the pdp1 struct and related.
#ifndef IOTHANDLER_H
#define IOTHANDLER_H

#define NOT_IN_PDP1
#include "pdp1.h"
#include "dynamicIots.h"

#define IONOWAIT(p) (p->ioh = 0)
#define IOCOMPLETE(p) (p->ios = 1)

#define IOINHOLD(p) (p->ioh)
#define IOWANTWAIT(p) ((p->mb & 0014000) == 0010000)
#define IOWANTCOMPLETE(p) ((p->mb & 0014000) == 0004000)
#define IOCOMPLETE_IFNEEDED(p, needed) ((needed)?IOCOMPLETE(p):0)

// Convenience macros, USTOCYCLES rounds DOWN to the nearest 5 usec cycle
#define MSTOCYCLES(ms) (((ms) * 1000) / 5)
#define USTOCYCLES(us) ((us) / 5)

// And for iotPollAt() deadlines, in ns
#define MSTONS(ms) ((uint64_t)(ms) * 1000000)
#define USTONS(us) ((uint64_t)(us) * 1000)

// Convenience macros, hide pdp1 struct details
#define IO(pdp1P) ((pdp1P)->io)
#define AC(pdp1P) ((pdp1P)->ac)
#define MB(pdp1P) ((pdp1P)->mb)
#define PC(pdp1P) ((pdp1P)->pc)
#define CKS(pdp1P) ((pdp1P)->cksflags)
#define PFLAGS(pdp1P) ((pdp1P)->pf)
#define SENSE(pdp1P) ((pdp1P)->ss)
#define SWITCHES(pdp1P) ((pdp1P)->tw)
// This refrences the entire memory space of the -1
#define CORE(pdp1P) (pdp1P)->core

// And same for memory sizes, addresses, etc.
#define MAXBANK 15
#define BANKSIZE 4096
#define MAXADDR (((MAXBANK + 1) * BANKSIZE) - 1)

#define CURBANK(pdp1P) (((pdp1P)->ema >> 12) & 0xF)
#define FULLADDRESS(pdp1P, addr) ((pdp1P)->ema | ((addr) & 0xFFF))

#define BANKOF(fulladdr) (((fulladdr) >> 12) & 0xF)
#define ADDRESSOF(fulladdr) ((fulladdr) & 0xFFF)

// Include to be used by IOT handler implementations
int iotHandler(PDP1 *, int device,  int pulse, int completion);
void iotStart(void);
void iotStop(void);
void iotPoll(PDP1 *);
// 19-Jun-2026 wje added for the rpa/rpb (reader) extraction. If implemented, called
// once per main-loop pass while the power is on -- no enablePolling() needed/used, and unlike
// iotPoll above this keeps running even while the CPU is halted. See dynamicIots.h/IotIOPollP.
void iotIOPoll(PDP1 *);
void initiateBreak(int chan);
// Call iotPoll() every cycles executed cycles, 0 to stop. Each call starts a fresh count.
void enablePolling(int cycles);
int iotIsAlias(void);

// Deadline polling, on simtime rather than executed cycles: it counts stolen cycles and the whole
// length of a mul or div. Base IOT_TIME_DEVICE also runs while the machine is halted, IOT_TIME_RUN
// does not (dynamicIots.h). iotDeadline() is called once base reaches deadline, at the end of that
// main-loop pass; the deadline is one-shot, so a periodic device re-arms from its last deadline,
// not from iotTime(), and never drifts. One deadline per plugin: arming replaces it.
// Emulator thread only.
void iotDeadline(PDP1 *);
uint64_t iotTime(int base);
void iotPollAt(int base, uint64_t deadline);
void iotPollCancel(void);

// Hidden method and vars used for control, implemented here to hide details from handlers
static IotEntryP _iotControlBlockP;
void dynamicIotProcessBreak(int);

// Called by pdp1.c during setup of this handler, not for direct use in a handler

void _setIotControlBlock(IotEntryP cbP)
{
    _iotControlBlockP = cbP;
}

void initiateBreak(int chan)
{
    dynamicIotProcessBreak(chan);
}

void enablePolling(int cycles)
{
    _iotControlBlockP->pollCount = 0;
    _iotControlBlockP->pollEnabled = cycles;
}

uint64_t iotTime(int base)
{
    return( dynamicIotTime(base) );
}

void iotPollAt(int base, uint64_t deadline)
{
    dynamicIotSetDeadline(_iotControlBlockP, base, deadline);
}

void iotPollCancel(void)
{
    dynamicIotCancelDeadline(_iotControlBlockP);
}

#endif
