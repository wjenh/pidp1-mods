#include "dynamicIots.h"

#define IONOWAIT(p) (p->ioh = 0)
#define IOCOMPLETE(p) (p->ios = 1)

#define IOINHOLD(p) (p->ioh)
#define IOWANTWAIT(p) ((p->mb & 0014000) == 0010000)
#define IOWANTCOMPLETE(p) ((p->mb & 0014000) == 0004000)
#define IOCOMPLETE_IFNEEDED(p, com) ((com)?IOCOMPLETE(p):0)

// Include to be used by IOT handler implementations
int iotHandler(PDP1 *, int device,  int pulse, int completion);
void iotStart(void);
void iotStop(void);
void iotPoll(PDP1 *);
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
