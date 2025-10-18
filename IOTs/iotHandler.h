#include "dynamicIots.h"

#define DEVICE(p) (p->mb & 077)
#define IONOWAIT(p) (p->ioh = 0)
#define IOCOMPLETE(p) (p->ios = 1)

// Include to be used by IOT handler implementations
int iotHandler(PDP1 *, int device,  int pulse, int completion);
void iotStart(void);
void iotStop(void);
void iotPoll(PDP1 *);
void initiateBreak(int chan);
void enablePolling(int cycles);
int iotIsAlias(void);

// Hidden method and vars used for control, implemented here to hide details from handlers
typedef void (*IotSeqBreakP)(int);

static IotSeqBreakP _breakCallback;
static IotEntryP _iotControlBlockP;

// Called by pdp1.c during setup of this handler, not for direct use in a handler
void _setBreakCallback( IotSeqBreakP callback)
{
    _breakCallback = callback;
}

void _setIotControlBlock(IotEntryP cbP)
{
    _iotControlBlockP = cbP;
}

void initiateBreak(int chan)
{
    if( _breakCallback )
    {
        _breakCallback(chan);
    }
}

void enablePolling(int on)
{
    _iotControlBlockP->pollEnabled = on;
}
