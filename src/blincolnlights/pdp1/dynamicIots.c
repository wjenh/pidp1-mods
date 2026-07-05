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
 * void iotPoll(PDP1P pdp1P); -called every n instruction cycles if enabled/
 * and then calling:
 * void enablePolling(int n) - 1..n to enable polling, 0 to disable, but only if an isPoll() is implemented
 *
 * 11-Apr-2026 wje fix filename formatting for single-digit IOTs
 * 19-Jun-2026 wje added a second, independent poll mechanism for the rpa/rpb (reader)
 * extraction and anything similar (punch, typewriter), iotIOPoll().
 * If implemented it is called unconditionally on everydynamicIotProcessorDoIOPoll() call.
 * This is called from the same main-loop call site ashandleio(), regardless of run state,
 * because real tape/typewriter timing doesn't stop just because the CPU is halted.
 * Also added dynamicIotOwnsDevice(int dev), so core code can tell whether a given device number is now
 * owned by a loaded dynamic IOT and skip its own builtin servicing of that device accordingly.
 * 
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

extern PDP1 *pdp1P;              // from main.c
extern void dynamicReq(PDP1 *pdp, int chan);

void dynamicIotProcessBreak(int chan);
static IotEntryP initializeEntry(int dev);

// Common lookup/lazy-load logic shared by dynamicIotProcessor() and dynamicIotOwnsDevice().
// Resolves dev to its real IotEntry (following aliases), lazily dlopen()ing IOT_<dev>.so via
// initializeEntry() if this is the first time dev has been touched.
// Returns a pointer to the resolved IotEntry if dev has a loaded or
// loadable handler; returns 0 if dev is out of range or no handler could be loaded for it.
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

    if( !(entryP = resolveEntry(dev)) )
    {
        return(0);
    }

    stopped = 0;
    status = entryP->handlerP(pdpP, dev, pulse, completion);
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

// Called every instruction cycle to hande any IOTs with polling.
void
dynamicIotProcessorDoPoll(PDP1 *pdp1P)
{
IotEntryP entryP;
PollEntryP pollItemP;

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
            if( ++(pollItemP->curCount) >= entryP->pollEnabled )
            {
                pollItemP->curCount = 0;
                entryP->pollP(pdp1P);
            }
        }
    }
}

// Called unconditionally once per main-loop iteration regardless of run/stop state.
void
dynamicIotProcessorDoIOPoll(PDP1 *pdp1P)
{
IoPollEntryP itemP;

    for( itemP = ioPollList; itemP; itemP = itemP->nextP )
    {
        itemP->iotEntryP->ioPollP(pdp1P);
    }
}

// Try to resolve a dynamic IOT's .so, if successful, initialize it.
// Returns a pointer to the rIotEntry/ on success; returns 0 if the .so can't be opened, or if it
// exports neither iotHandler nor a usable iotAlias, or if an iotAlias target is out of range or
// itself fails to resolve.
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
            if( (i < 1) || (i > 63) )          // audit M9: was "&&", could never be true -- no range check at all
            {
                return(0);      // out of range
            }

            tmpEntryP = &handles[i];    // our real entry
            if( !tmpEntryP->handlerP )
            {
                tmpEntryP = initializeEntry(i);
            }

            if( !tmpEntryP )
            {
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

    // Be sure start gets called, we're already running so it won't have been yet.
    if( entryP->startP )
    {
        stopped = 0;
        entryP->startP();
    }

    return( entryP );
}
