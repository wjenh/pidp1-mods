// Include for the dynamic IOT processor

#ifdef NOTIOTH
// What's called from pdp1.c
int dynamicIotProcessor(PDP1 *pdpP, int device, int pulse, int completion);
void dynamicIotProcessorStart(void);
void dynamicIotProcessorStop(void);
void dynamicIotProcessorSetPDP1(PDP1 *pdpP);
void dynamicIotProcessorDoPoll(PDP1 *pdpP);
#endif

// Called from an implemented handler
void dynamicIotSetPollingState(void *, int); // really gets called with a pointer to the control block for the IOT

// What a loadable IOT handler implements, PDP1 state, pulse hi/low, completion pulse wanted
// The IOT handler implements a function 'int iotHandler(PDP1 *pdp1P, int device, int pulse, int completion)'.
// The handler function will be called twice for each IOT executed, once for start pulse going high,
// again with the start pulse goes low.
typedef int (*IotHandlerP)(PDP1 *, int, int, int);

// If implemented, will be called when the emulator goes into run
// The IOT handler implements a function 'void iotStart()'.
typedef void (*IotStartP)(void);

// If implemented, will be called when the emulator goes into halt
// The IOT handler implements a function 'void iotStop()'.
typedef void (*IotStopP)(void);

// If implemented, will duplicate this IOT into the IOT number returned.
typedef int (*IotAliasP)();

// These two are implemented if the handler is to get poll calls from the emulator once per instruction cycle
typedef void (*IotPollEnableP)(int);
typedef void (*IotPollP)(PDP1 *);

// Additionally, a 'hidden' callback is set up to allow the handler to initiate a sequence break
// Within the handler, initiateBreak(chan) can be used to signal a break;
typedef void (*IotSeqBreakP)(int chan);     // same as in iotHandler.h
typedef void (*IotSeqBreakHandlerP)(IotSeqBreakP);

typedef struct _IotEntry
{
    int invalid;        // if 1, we tried to load already, nothing found
    int isAlias;        // if 1, we are a copy
    int pollEnabled;    // 0, no polling, the default
    void *dlHandleP;
    IotHandlerP handlerP;
    IotStartP startP;
    IotStopP stopP;
    IotPollP pollP;
    struct _IotEntry *actualEntryP;    // for aliases
} IotEntry, *IotEntryP;

#ifdef NOTIOTH
typedef struct pollEntry
{
    struct pollEntry *nextP;         // we link all polls in a chain
    IotEntryP iotEntryP;            // the definition for a given IOT
    int numCycles;                  // if not 0, how many cycles between calls
    int curCount;                   // cycles since last entry call
} PollEntry, *PollEntryP;

// Similarly, set a reference back to the IotEntry for an IOT
typedef void (*IotControlBlockSetterP)(IotEntryP);
#endif
