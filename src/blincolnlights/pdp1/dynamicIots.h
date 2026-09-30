// Include for the dynamic IOT processor

#ifdef NOTIOTH
// What's called from pdp1.c
int dynamicIotProcessor(PDP1 *pdpP, int device, int pulse, int completion);
void dynamicIotProcessorStart(void);
void dynamicIotProcessorStop(void);
void dynamicIotProcessorUpdate(void);
void dynamicIotProcessorSetPDP1(PDP1 *pdpP);
void dynamicIotProcessorDoPoll(PDP1 *pdpP);

// Returns 1 if a handler is loaded for dev, else 0.
int dynamicIotOwnsDevice(int dev);

// Called once per main-loop pass while the power is on, whether the CPU is running or halted.
void dynamicIotProcessorDoIOPoll(PDP1 *pdpP);

// Called at the end of every main-loop pass while the power is on: advances the deadline time
// bases by passNs (the pass's simtime, less any throttle lag-cap span) and calls iotDeadline()
// for any plugin whose deadline has come. ran is true if the machine ran the pass, a stolen
// cycle included.
void dynamicIotProcessorAdvance(PDP1 *pdpP, uint64_t passNs, bool ran);

// 11-Sep-2026 wje/claude: per-device handler and poll timing. main.c no longer calls these (its
// calls were removed on purpose), so the timing stays off. Enable (non-zero) or disable timing;
// the report writes one line per device used, then resets (a NULL stream resets without writing).
void dynamicIotTimingEnable(int on);
void dynamicIotTimingReport(FILE *fP);
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

// These two are implemented if the handler is to get poll calls from the emulator every n executed
// cycles, n set by enablePolling(). Executed cycles only: stolen cycles and halted passes are not
// counted, and a mul or div counts as two.
typedef void (*IotPollEnableP)(int);
typedef void (*IotPollP)(PDP1 *);

// If implemented, called once per main-loop pass (5us of simtime) while the power is on, running
// or halted.
typedef void (*IotIOPollP)(PDP1 *);

// If implemented, called once the plugin's deadline, set by iotPollAt(), has come on its time
// base. The deadline is one-shot: it is disarmed before the call.
typedef void (*IotDeadlineP)(PDP1 *);

// The deadline time bases, in ns of simtime. Neither counts a throttle lag-cap span or time with
// the power off. IOT_TIME_DEVICE also runs while the machine is halted; IOT_TIME_RUN does not.
#define IOT_TIME_DEVICE 0
#define IOT_TIME_RUN 1


// If implemented, will be called after the pdp1 emulator gets a SIGHUP, to reload configuration.
// It runs on the emulator thread, from the main loop between cycles, not in the signal handler.
typedef void (*IotUpdateP)();

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
    IotUpdateP updateP;
    IotPollP pollP;
    IotIOPollP ioPollP;     // 19-Jun-2026 wje, see IotIOPollP above
    struct _IotEntry *actualEntryP;    // for aliases
    // Added at the end, so a plugin built before them still loads.
    int pollCount;          // executed cycles since the last iotPoll(); enablePolling() zeroes it
    IotDeadlineP deadlineP;
    int deadlineBase;       // IOT_TIME_DEVICE or IOT_TIME_RUN
    bool deadlineArmed;
    uint64_t deadline;      // ns on deadlineBase
} IotEntry, *IotEntryP;

// The deadline calls iotHandler.h makes for a plugin. Emulator thread only.
uint64_t dynamicIotTime(int base);
void dynamicIotSetDeadline(IotEntryP entryP, int base, uint64_t deadline);
void dynamicIotCancelDeadline(IotEntryP entryP);

#ifdef NOTIOTH
typedef struct pollEntry
{
    struct pollEntry *nextP;         // we link all polls in a chain
    IotEntryP iotEntryP;            // the definition for a given IOT
    int numCycles;                  // if not 0, how many cycles between calls
    int iotNum;                     // just for convenience
} PollEntry, *PollEntryP;

// Used only for the special IO poll.
typedef struct ioPollEntry
{
    struct ioPollEntry *nextP;
    IotEntryP iotEntryP;
} IoPollEntry, *IoPollEntryP;

// Set a reference back to the IotEntry for an IOT
typedef void (*IotControlBlockSetterP)(IotEntryP);
#endif
