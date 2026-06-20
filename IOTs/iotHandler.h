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

#define BANKOF(fulladdr) (((fulladddr) >> 12) & 0xF)
#define ADDRESSOF(fulladdr) ((fulladddr) & 0xFFF)

// Include to be used by IOT handler implementations
int iotHandler(PDP1 *, int device,  int pulse, int completion);
void iotStart(void);
void iotStop(void);
void iotPoll(PDP1 *);
// 19-Jun-2026 wje added for the rpa/rpb (reader) extraction. If implemented, called
// unconditionally once per main loop iteration -- no enablePolling() needed/used, and unlike
// iotPoll above this keeps running even while the CPU is halted. See dynamicIots.h/IotIOPollP.
void iotIOPoll(PDP1 *);
void initiateBreak(int chan);
void enablePolling(int cycles);
int iotIsAlias(void);

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

void enablePolling(int on)
{
    _iotControlBlockP->pollEnabled = on;
}

#endif
