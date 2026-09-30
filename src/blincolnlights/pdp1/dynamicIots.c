/**
 * This implements dynamic loading and execution of custom IOT commands for the pidp-1.
 * When an unknown IOT is seen by the emulator, this will search for a handler compilied as a shared object.
 * The file must be named 'IOT_nn.so', where nn is the OCTAL IOT device number.
 * If found, it is registered and then used to handle the IOT.
 *
 * The handler will be called twice for every IOT instruction for it, once at the start of IOT 'hardware' pulse,
 * again at the end of the pulse.
 *
 * The handler is passed a pointer to the PDP structure which contains the entire state information of the emulator.
 * If the handler returns 0, then the IOT will be treated as an undefined IOT as if there was no handler.
 * Any other return value means the IOT was processed.
 *
 * An implemented IOT handler must implement an iotHandler() methood.
 * It can optionally implement:
 * void iotStart(void); - called when the emulator transitions to run state
 * void iotStop(void); - called when the emulator transitions to halt state
 * void iotUpdate(void); - called when the emulator gets SIGHUP to reload its configuration
 *
 * Pseudo-asynchronous behavior can be done by implementing:
 * void iotPoll(PDP1P pdp1P); - called every n executed cycles while enabled
 * and then calling:
 * void enablePolling(int n) - n > 0 to poll every n cycles, 0 to stop; useful only if iotPoll() is implemented
 *
 * Or, timed on simtime rather than executed cycles, by implementing:
 * void iotDeadline(PDP1P pdp1P); - called once the deadline set by iotPollAt() has come
 * and then calling iotPollAt(base, deadline) and iotPollCancel(), with iotTime(base) for now.
 * The two time bases are kept here, advanced by main.c once per powered pass (see
 * dynamicIotProcessorAdvance()), and the deadline check runs on every such pass, halted and
 * stolen ones included, since a device keeps running while the machine is halted.
 *
 * 11-Apr-2026 wje fix filename formatting for single-digit IOTs
 * 19-Jun-2026 wje added a second, independent poll mechanism for the rpa/rpb (reader)
 * extraction and anything similar, iotIOPoll(); the reader, tyi and the drum use it.
 * If implemented it is called on every dynamicIotProcessorDoIOPoll() call, which main.c makes
 * once per main-loop pass beside handleio(), while the power is on, running or halted,
 * because real tape/typewriter timing doesn't stop just because the CPU is halted.
 * Also added dynamicIotOwnsDevice(int dev), so core code can tell whether a given device number is now
 * owned by a loaded dynamic IOT and skip its own builtin servicing of that device accordingly.
 * 11-Sep-2026 wje a failed alias now closes its .so and marks its entry invalid.
 * 11-Sep-2026 wje/claude per-device handler and poll timing. Off unless dynamicIotTimingEnable()
 * turns it on, which main.c no longer does (removed on purpose); when on, every handler and
 * poll call is bracketed by two gettime() calls and its duration accumulated by device number,
 * so the report can say which device's code the emulator thread's cycle time is going to.
 */

#include <unistd.h>
#include <dlfcn.h>

#include "common.h"
#include "pdp1.h"
#define NOTIOTH
#include "dynamicIots.h"

static int stopped = 1;         // assume we are halted initially
static IotEntry handles[64];
static PollEntryP pollList;
static IoPollEntryP ioPollList;   // 19-Jun-2026 wje, see iotIOPoll above

// The deadline time bases, indexed by IOT_TIME_DEVICE and IOT_TIME_RUN, and the earliest armed
// deadline on each (UINT64_MAX for none). An earliest value may be early, never late: arming
// only lowers it, and a cancel leaves it, costing one walk that finds nothing due.
// Only the emulator thread touches these, so no locking is needed.
static uint64_t timeNs[2];
static uint64_t earliestNs[2] = { UINT64_MAX, UINT64_MAX };

// The plugins that implement iotDeadline(), at most one per device number.
typedef struct
{
    IotEntryP entryP;
    int iotNum;
} DeadlineEntry;
static DeadlineEntry deadlineList[64];
static int deadlineCount;

// Per-device timing, indexed by IOT device number (0-077), see the file header.
// Only the emulator thread touches these, so no locking is needed.
static int devTiming;                   // non-zero while per-device timing is on
static long handlerCalls[64];           // iotHandler() calls, both pulses counted
static u64 handlerNs[64];               // total ns spent in iotHandler()
static u64 handlerMaxNs[64];            // longest single iotHandler() call
static long pollCalls[64];              // iotPoll() calls
static u64 pollNs[64];                  // total ns spent in iotPoll()
static u64 pollMaxNs[64];               // longest single iotPoll() call

extern PDP1 *pdp1P;              // from main.c
extern void dynamicReq(PDP1 *pdp, int chan);

void dynamicIotProcessBreak(int chan);
static IotEntryP initializeEntry(int dev);
static void noteDevTime(long *callsP, u64 *totalP, u64 *maxP, int dev, u64 ns);
static void runDeadlines(PDP1 *pdp1P);

// Common lookup/lazy-load logic shared by dynamicIotProcessor() and dynamicIotOwnsDevice().
// Resolves dev to its real IotEntry following aliases.
// Returns a pointer to the resolved IotEntry if dev has a handler,
// returns 0 if dev is out of range or no handler could be loaded for it.
static IotEntryP
resolveEntry(int dev)
{
IotEntryP entryP;

    if( dev > 077 )
    {
        return(0);              // bad dev value, treat as unknown
    }

    entryP = &handles[dev];
    if( entryP->isAlias )
    {
        entryP = entryP->actualEntryP;
    }

    if( entryP->invalid )       // been here before, nothing
    {
        return(0);
    }
    else if( !entryP->dlHandleP )    // it hasn't been resolved yet
    {
        if( !(entryP = initializeEntry(dev)) )
        {
            return(0);
        }
    }

    return( entryP );
}

// Called from the emulator to try to invoke a dynamic IOT.
// It is called twice for each IOT, once on the IOT start pulse rising edge, once on the falling edge.
// The completion parameter is derived from bits 5 and 6 of the IOT instruction and can have the values:
// 0 - no completion pulse expected
// 1 - completion pulse enabled
// Returns 1 for success, 0 if none found.
int
dynamicIotProcessor(PDP1 *pdpP, int dev, int pulse, int completion)
{
IotEntryP entryP;
int status;
u64 startNs;

    if( !(entryP = resolveEntry(dev)) )
    {
        return(0);
    }

    stopped = 0;
    if( devTiming )
    {
        // Charged to the device number the program used; resolveEntry() guarantees dev <= 077.
        startNs = gettime();
        status = entryP->handlerP(pdpP, dev, pulse, completion);
        noteDevTime(handlerCalls, handlerNs, handlerMaxNs, dev, (gettime() - startNs));
    }
    else
    {
        status = entryP->handlerP(pdpP, dev, pulse, completion);
    }

    return( status );
}

// Returns 1 if a dynamic IOT handler is loaded or successfully loads for dev, else 0.
int
dynamicIotOwnsDevice(int dev)
{
    return( resolveEntry(dev) != 0 );
}

// Lets a dynamic IOT plugin's handler request a sequence break on channel chan, via the
// initiateBreak() callback it's given.
void
dynamicIotProcessBreak(int chan)
{
    if( (chan >= 0) && (chan < 16) )
    {
        dynamicReq(pdp1P, chan);               // signal a break, convoluted because of various unshared bits
    }
}

// Called when the emulator is started so IOTs that need to can clean up.
void
dynamicIotProcessorStart(void)
{
int i;
IotStartP startP;

    if( !stopped )
    {
        return;             // already done
    }

    for( i = 0; i < 64; ++i )                  // audit M9: handles[] has 64 entries (device numbers 0-63)
    {
        if( (startP = handles[i].startP) && !handles[i].isAlias )
        {
            startP();
        }
    }

    stopped = 0;
}

// Called when the emulator is halted so IOTs that need to can clean up.
void
dynamicIotProcessorStop(void)
{
int i;
IotStopP stopP;

    if( stopped )
    {
        return;             // already done
    }

    for( i = 0; i < 64; ++i )                  // audit M9: handles[] has 64 entries (device numbers 0-63)
    {
        if( (stopP = handles[i].stopP) && !handles[i].isAlias )
        {
            stopP();
        }
    }

    stopped = 1;
}

// Called when the emulator gets SIGHUP so an IOT can reload its configuration.
void
dynamicIotProcessorUpdate(void)
{
int i;
IotUpdateP updateP;

    for( i = 0; i < 64; ++i )                  // audit M9: handles[] has 64 entries (device numbers 0-63)
    {
        if( (updateP = handles[i].updateP) && !handles[i].isAlias )
        {
            updateP();
        }
    }
}

// Called at the end of every executed cycle (pdp1.c's cycle()) to handle any IOTs with polling;
// not on a stolen cycle or while halted, and a mul or div is two calls.
void
dynamicIotProcessorDoPoll(PDP1 *pdp1P)
{
IotEntryP entryP;
PollEntryP pollItemP;
u64 startNs;

    if( stopped )
    {
        return;             // nothing to do
    }

    // go thru the chain calling any that is enabled and has reached its cycle count
    for( pollItemP = pollList; pollItemP; pollItemP = pollItemP->nextP )
    {
        entryP = pollItemP-> iotEntryP;
        if( entryP->pollEnabled )
        {
            if( ++(entryP->pollCount) >= entryP->pollEnabled )
            {
                entryP->pollCount = 0;
                if( devTiming )
                {
                    startNs = gettime();
                    entryP->pollP(pdp1P);
                    noteDevTime(pollCalls, pollNs, pollMaxNs, pollItemP->iotNum, (gettime() - startNs));
                }
                else
                {
                    entryP->pollP(pdp1P);
                }
            }
        }
    }
}

// Called once per main-loop pass while the power is on, whether the CPU is running or halted.
void
dynamicIotProcessorDoIOPoll(PDP1 *pdp1P)
{
IoPollEntryP itemP;

    for( itemP = ioPollList; itemP; itemP = itemP->nextP )
    {
        itemP->iotEntryP->ioPollP(pdp1P);
    }
}

// Called by main.c at the end of every main-loop pass while the power is on, running, stolen or
// halted. passNs is the pass's simtime less any throttle lag-cap span, so neither time base
// counts time in which no instruction ran; the run base also skips halted passes. Two compares
// a pass: the deadline list is walked only once one of them says something may be due.
void
dynamicIotProcessorAdvance(PDP1 *pdp1P, uint64_t passNs, bool ran)
{
    timeNs[IOT_TIME_DEVICE] += passNs;
    if( ran )
    {
        timeNs[IOT_TIME_RUN] += passNs;
    }

    if( (timeNs[IOT_TIME_DEVICE] >= earliestNs[IOT_TIME_DEVICE]) ||
        (timeNs[IOT_TIME_RUN] >= earliestNs[IOT_TIME_RUN]) )
    {
        runDeadlines(pdp1P);
    }
}

// Returns the time on base, in ns. In a handler it is the time at the start of the pass; in an
// iotDeadline() call, the end of the pass that made the call.
uint64_t
dynamicIotTime(int base)
{
    return( timeNs[(base == IOT_TIME_RUN) ? IOT_TIME_RUN : IOT_TIME_DEVICE] );
}

// Arms entryP's one deadline, replacing any it had, for iotPollAt(). A deadline already passed
// is called at the end of this pass. Ignored for a plugin with no iotDeadline(), which would
// otherwise hold an earliest value no walk could ever clear.
void
dynamicIotSetDeadline(IotEntryP entryP, int base, uint64_t deadline)
{
    if( !entryP || !entryP->deadlineP )
    {
        return;
    }

    base = (base == IOT_TIME_RUN) ? IOT_TIME_RUN : IOT_TIME_DEVICE;
    entryP->deadlineBase = base;
    entryP->deadline = deadline;
    entryP->deadlineArmed = true;
    if( deadline < earliestNs[base] )
    {
        earliestNs[base] = deadline;
    }
}

// Drops entryP's deadline, for iotPollCancel(). The earliest value is left as it is.
void
dynamicIotCancelDeadline(IotEntryP entryP)
{
    if( entryP )
    {
        entryP->deadlineArmed = false;
    }
}

// Calls iotDeadline() for every plugin whose deadline has come, then recomputes the earliest
// values, since any call may arm or cancel. Each deadline is disarmed before its call, so a
// plugin that re-arms at or before now is called on the next pass, not again in this walk.
static void
runDeadlines(PDP1 *pdp1P)
{
int i;
IotEntryP entryP;
uint64_t lowest[2] = { UINT64_MAX, UINT64_MAX };
u64 startNs;

    for( i = 0; i < deadlineCount; ++i )
    {
        entryP = deadlineList[i].entryP;
        if( entryP->deadlineArmed && (timeNs[entryP->deadlineBase] >= entryP->deadline) )
        {
            entryP->deadlineArmed = false;
            if( devTiming )
            {
                startNs = gettime();
                entryP->deadlineP(pdp1P);
                noteDevTime(pollCalls, pollNs, pollMaxNs, deadlineList[i].iotNum, (gettime() - startNs));
            }
            else
            {
                entryP->deadlineP(pdp1P);
            }
        }
    }

    for( i = 0; i < deadlineCount; ++i )
    {
        entryP = deadlineList[i].entryP;
        if( entryP->deadlineArmed && (entryP->deadline < lowest[entryP->deadlineBase]) )
        {
            lowest[entryP->deadlineBase] = entryP->deadline;
        }
    }

    earliestNs[IOT_TIME_DEVICE] = lowest[IOT_TIME_DEVICE];
    earliestNs[IOT_TIME_RUN] = lowest[IOT_TIME_RUN];
}

// Accumulate one timed call of device dev into the given call-count, total and maximum arrays.
// dev outside 0-077 is ignored rather than trusted as an index.
static void
noteDevTime(long *callsP, u64 *totalP, u64 *maxP, int dev, u64 ns)
{
    if( (dev < 0) || (dev > 077) )
    {
        return;
    }

    ++(callsP[dev]);
    totalP[dev] += ns;
    if( ns > maxP[dev] )
    {
        maxP[dev] = ns;
    }
}

// Turn per-device timing on (non-zero) or off. It only sets the flag, so accumulated data is kept
// until the next report. Nothing calls it at present (see the file header).
void
dynamicIotTimingEnable(int on)
{
    devTiming = on;
}

// Write one line per device that had any timed handler or poll calls since the last report,
// giving call count, average and maximum ns for each, then clear everything for the next run.
// fP is an open, writable stream, or NULL to clear without writing; nothing is written for
// devices with no calls.
void
dynamicIotTimingReport(FILE *fP)
{
int dev;

    for( dev = 0; (fP && (dev < 64)); ++dev )
    {
        if( (handlerCalls[dev] == 0) && (pollCalls[dev] == 0) )
        {
            continue;       // device not used this run
        }

        fprintf(fP, "  IOT %02o: handler %ld calls avg %lluns max %lluns; poll %ld calls avg %lluns max %lluns\n",
            dev,
            handlerCalls[dev],
            (unsigned long long)((handlerCalls[dev]) ? (handlerNs[dev] / (u64)handlerCalls[dev]) : 0),
            (unsigned long long)handlerMaxNs[dev],
            pollCalls[dev],
            (unsigned long long)((pollCalls[dev]) ? (pollNs[dev] / (u64)pollCalls[dev]) : 0),
            (unsigned long long)pollMaxNs[dev]);
    }

    memset(handlerCalls, 0, sizeof(handlerCalls));
    memset(handlerNs, 0, sizeof(handlerNs));
    memset(handlerMaxNs, 0, sizeof(handlerMaxNs));
    memset(pollCalls, 0, sizeof(pollCalls));
    memset(pollNs, 0, sizeof(pollNs));
    memset(pollMaxNs, 0, sizeof(pollMaxNs));
}

// Try to resolve a dynamic IOT's .so, if successful, initialize it.
// Returns a pointer to the rIotEntry/ on success; returns 0 if the .so can't be opened, or if it
// exports neither iotHandler nor a usable iotAlias, or if an iotAlias target is out of range or
// itself fails to resolve. Every failure marks dev's entry invalid, so it is not tried again.
static IotEntryP
initializeEntry(int dev)
{
int i;
IotEntryP entryP, tmpEntryP;
PollEntryP pollEntryP;
IoPollEntryP ioPollEntryP;
IotAliasP aliasP;
char fname[256];

    entryP = &handles[dev];

    sprintf(fname,"/opt/pidp1-mods/IOTs/IOT_%o.so", dev);

    if( !(entryP->dlHandleP = dlopen(fname, RTLD_LAZY)) )
    {
        // Not found, record that and fail
        entryP->invalid = 1;
        return(0);
    }

    entryP->handlerP = (IotHandlerP)dlsym(entryP->dlHandleP, "iotHandler");
    if( !entryP->handlerP )
    {
        // It could be an alias
        if( (aliasP = (IotAliasP)dlsym(entryP->dlHandleP, "iotAlias")) )
        {
            // we need to have this alias point to the real one
            i = aliasP();
            if( (i < 1) || (i > 63) || (i == dev) )    // audit M9: was "&&", could never be true -- no range check at all
            {
                dlclose(entryP->dlHandleP);
                entryP->dlHandleP = 0;
                entryP->invalid = 1;
                return(0);      // out of range
            }

            // Our real entry, loaded if need be. It must be a handler: a target that is missing,
            // or is itself still being set up (an alias loop), fails the alias.
            tmpEntryP = resolveEntry(i);
            if( !tmpEntryP || !tmpEntryP->handlerP )
            {
                dlclose(entryP->dlHandleP);
                entryP->dlHandleP = 0;
                entryP->invalid = 1;
                return(0);              // target doesn't exist
            }

            entryP->isAlias = 1;
            entryP->actualEntryP = tmpEntryP;
            return( tmpEntryP );
        }
        else
        {
            dlclose(entryP->dlHandleP);
            entryP->invalid = 1;
            return(0);
        }
    }

    // Should be implemented, but if not, ignore
    IotControlBlockSetterP setterP = (IotControlBlockSetterP)dlsym(entryP->dlHandleP, "_setIotControlBlock");
    if( setterP )
    {
        setterP(entryP);                       // this is us
    }

    // not required to be implemented
    entryP->startP = (IotStartP)dlsym(entryP->dlHandleP, "iotStart");
    entryP->stopP = (IotStopP)dlsym(entryP->dlHandleP, "iotStop");
    entryP->updateP = (IotStopP)dlsym(entryP->dlHandleP, "iotUpdate");

    entryP->pollP = (IotPollP)dlsym(entryP->dlHandleP, "iotPoll");
    if( entryP->pollP )
    {
        pollEntryP = (PollEntryP)calloc(1, sizeof(PollEntry));
        pollEntryP->iotEntryP = entryP;
        pollEntryP->nextP = pollList;
        pollEntryP->iotNum = dev;
        pollList = pollEntryP;
    }

    entryP->ioPollP = (IotIOPollP)dlsym(entryP->dlHandleP, "iotIOPoll");
    if( entryP->ioPollP )
    {
        ioPollEntryP = (IoPollEntryP)calloc(1, sizeof(IoPollEntry));
        ioPollEntryP->iotEntryP = entryP;
        ioPollEntryP->nextP = ioPollList;
        ioPollList = ioPollEntryP;
    }

    // An entry is initialized once per device number, so the list cannot overflow.
    entryP->deadlineP = (IotDeadlineP)dlsym(entryP->dlHandleP, "iotDeadline");
    if( entryP->deadlineP )
    {
        deadlineList[deadlineCount].entryP = entryP;
        deadlineList[deadlineCount].iotNum = dev;
        ++deadlineCount;
    }

    // Be sure start gets called, we're already running so it won't have been yet.
    if( entryP->startP )
    {
        stopped = 0;
        entryP->startP();
    }

    return( entryP );
}
