/*
 * This is a loose implementation of the Type 19 High Speed Channel Control.
 * It is for use in IOTs or other emulator code to package up direct memory access
 * and simulate the behavior of the PDP-1 dma, hiding details of memory back wraparound, etc.
 * It does one read/write operation every run cycle, 5us.
 * It can cycle-steal, when it does it takes over for as long as it takes to transfer all data at 5us per
 * word transfer, a simultaenous read/write counts as one word transfer, 5us.
 *
 * 23-Apr-2026 wje - rework to make it more realistic
 * 29-Apr-2026 wje - fix overrun of channel list
 * 30-Apr-2026 wje - add fake break cycles for THREADED so emulator will skip cycles semi-properly
 * 21-Jun-2026 wje/claude - fix minor issue with done synchronization
 * 28-Jun02026 wje - add HSC_MODE_UPDATEPANEL for use with HSC_MODE_IMMEDIATE
 * 02-Jul-2026 wje/claude - HSCwait() THREADED-mode delay now busy-spins (hscSpinWait())
 *    for delays at or below HSC_SPIN_LIMIT_US. Testing showed usleep() was wildly inconsistent.
 * 05-Jul-2026 wje - extended HSC_MODE_UPDATEPANEL to HSC_MODE_THREADED.
 *    Rework light control to be more robust, allow pulse stretching so the hsc state will show up better.
 * 06-Jul-2026 wje/claude - fix HSCwait() double-counting the THREADED waitDelay.
 *    HSCwait() didn't check for done and always slept the full waitDelay regardless, adding a redundant
 *    delay on top of whatever real time had already elapsed.
 *    Now skipped if the channel is already done.
 * 2-Sep-2026 wje - add HSC_MODE_TRUESTEAL and some safety data masking.
 *    THREADED front-loads all of a transfer's stealticks and only afterward lets the CPU
 *    run free for the remainder of the device's completion time, which starves the CPU
 *    for the whole steal block instead of interleaving it the way real HSC hardware did.
 *    TRUESTEAL spreads the steal ticks evenly across the transfer's actual real-time duration.
 *    The channel's busy/done timing is accurate on its own and a device no longer needs a
 *    separate real-clock completion check.
 *    Note this means a TRUESTEAL channel reports HSC_BUSY for its full durationa.
*/

#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

//#define DOLOGGING
#define LOG_HSC 0
#define LOG_EXEC 0
#define LOG_DATA 0

#include "common.h"
#include "pdp1.h"
#include "logger.h"
#include "highSpeedChannels.h"

// Stretch any high speed transfer request of < HSC_STRETCH words to this.
// Otherwise, we would rarely see the light wen using the type340 which
// does single-word transfers.
#define HSC_STRETCH 20

typedef struct {
    bool isInitialized;
    bool isAssigned;
    bool isWaiting;
    bool needLightoff;      // HSC_MODE_UPDATE_PANEL was requested
    bool trueSteal;             // true while a TRUESTEAL transfer is in progress on this channel
    int status;
    int waitDelay;          // if we are in THREADED mode, how long to sleep in HSCwait()
    int brkCount;           // if we are in THREADED mode, simulate break requests, more or less
    int onCount;            // HSC_MODE_UPDATEPANEL was usd, number of cycles to keep hsc cylcle on for
    int stealsLeft;    // steal-ticks (one per word) still owed
    int ticksLeft;     // total ticks (steal + free) still owed; the live DDA denominator
    int stealAcc;           // DDA accumulator
    sem_t accessSemaphore;  // how we synchrnonize modification of the control structure
    sem_t waitSemaphore;    // how we synchrnonize completion
    HSCRequest request;     // pending request if any, copied by execute from user space
    } HSCControl, *HSCControlP;

// Original had three channels, priority ordered 1-3, we do 5, same priority order
#define NUMCHANS 5

// THe threshold where we usleep() instead of spin-wait.
#define HSC_SPIN_LIMIT_US 20

static HSCControl chan1;
static HSCControl chan2;
static HSCControl chan3;
static HSCControl chan4;
static HSCControl chan5;

static HSCControlP chans[] = {&chan1, &chan2, &chan3, &chan4, &chan5};

static HSCControlP getControlP(HSCChannelP chanP);
static void lockControl(HSCControlP ctlP);
static void unlockControl(HSCControlP ctlP);
static void HSCdone(HSCControlP ctlP);
static void processImmediate(HSCRequestP requestP);
static bool processChannel(HSCControlP controlP);
static void hscSpinWait(int us);

extern PDP1P pdp1P;     // from main.c

// Service routine called from run loop.
// This happens every simulated 5 microsecond cycle.
// Returns 0 if it took no time, 1 if it did a 'memory cycle' and we are in steal mode.
bool
processHSCchannels()
{
int i;
bool done, steal;
HSCControlP ctlP;

    // We do in priority order, 0 being highest.
    done = steal = false;
    for( i = 0; i < NUMCHANS; ++i )
    {
        // The channels are scanned from low to high, first one that's busy wins.
        // If a channel still needs lightoff processing, let that happen,
        // Any THREADED or IMMEDATE mode that dis not specify HSC_MODE_UPDATEPANEL
        // has to do its own.
        ctlP = chans[i];

        // Check to see if the light needs turning off.
        if( ctlP->needLightoff )
        {
            if( ctlP->onCount-- <= 0 )
            {
                ctlP->onCount = 0;
                ctlP->needLightoff = false;
                pdp1P->hsc = 0;
                updatelights(pdp1P, pdp1P->panel);
                updatelights_pwm(pdp1P->panel, 1);
            }
            else if( pdp1P->hsc == 0 )
            {
                // It was turned off by a higher priority channel, turn it back on
                pdp1P->hsc = 1;
                updatelights(pdp1P, pdp1P->panel);
                updatelights_pwm(pdp1P->panel, 1);
            }
        }

        if( ctlP->status == HSC_BUSY )
        {
            if( ctlP->onCount > 0 )
            {
                pdp1P->hsc = 1;         // be sure the light is on
            }

            if( processChannel(ctlP) )
            {
                steal = true;        // we processed one, steal a cycle
            }
            
            done = true;                // finished processing
        }

        if( done )
        {
            break;
        }
    }

    return(steal);
}

// User side calls.

// Allocate a channel if available and return its channel pointer.
// The channel object is malloced.
// If the channel number is invalid or the channel is already allocated, return null,
// else the channel pointer.
HSCChannelP
HSCallocateChannel(int chanNo)
{
HSCChannelP chanP;
HSCControlP ctlP;

    if( (chanNo < 1) || (chanNo > NUMCHANS) )
    {
        return(NULL);
    }

    ctlP = chans[chanNo-1];
    if( ctlP->isAssigned )
    {
        return(NULL);
    }

    if( !ctlP->isInitialized )
    {
        sem_init(&(ctlP->accessSemaphore), 0, 1);
        sem_init(&(ctlP->waitSemaphore), 0, 0);
        ctlP->status = HSC_OK;
        ctlP->isInitialized = true;
    }

    ctlP->isAssigned = true;
    ctlP->isWaiting = false;
    ctlP->brkCount = 0;
    ctlP->trueSteal = false;

    chanP = (HSCChannelP)malloc(sizeof(HSCChannel));
    chanP->chanNo = chanNo - 1;         // we keep it as an offset in the channel table
    return(chanP);
}

// Free a channel.
// The channel object is also freed and is no longer valid.
// If the channel number is invalid or the channel is already freed, return false else true.
bool
HSCfreeChannel(HSCChannelP chanP)
{
HSCControlP ctlP;

    if( !(ctlP = getControlP(chanP)) || !ctlP->isAssigned )
    {
        return(false);      // someone is doing something stupid.
    }

    ctlP->isAssigned = false;
    free(chanP);
    return(true);
}

// Emulator says to stop everything in progress.
void
HSCreset()
{
int i;
HSCControlP ctlP;

    for( i = 0; i < NUMCHANS; ++i )
    {
        ctlP = chans[i];

        if( ctlP->isAssigned )
        {
            ctlP->status = HSC_ABORT;
            ctlP->waitDelay = ctlP->brkCount = 0;
            ctlP->isWaiting = false;
            ctlP->trueSteal = false;
            ctlP->stealsLeft = ctlP->ticksLeft = ctlP->stealAcc = 0;
        }
    }

    // Including the lights, will get updated in the halt loop.
    pdp1P->hsc = 0;
}

// Main interaction from user side.
// Returns HSC_ERR for invalid chan, mode, count > 4096, banks out of range of 0-15 dec.
// Returns HSC_BUSY if a request is still executing.
// Returns HSC_OK otherwise.
// Note that TRUESTEAL mode will keep the channel busy until the true transfer time has been reached.
// Code needs to actually check for request completion!
int
HSCexecute(HSCChannelP chanP, HSCRequestP rqstP)
{
HSCControlP ctlP;

    if( (rqstP->memBank > 15) || (rqstP->memBank < 0) || (rqstP->memAddr > 4095) || (rqstP->memAddr < 0) ||
        (rqstP->count > 4096) || (rqstP->count < 0) )
    {
        logger(LOG_HSC, "request_channel called bad addr or count bank %d addr %d count %d\n",
            rqstP->memBank, rqstP->memAddr, rqstP->count);
        return( HSC_ERR );
    }

    if( !(ctlP = getControlP(chanP)) || !ctlP->isAssigned )
    {
        logger(LOG_HSC, "execute called but channel is not assigned\n");
        return( HSC_ERR );
    }

    if( ctlP->status == HSC_BUSY )
    {
        logger(LOG_HSC, "execute called but channel is busy\n");
        return(HSC_BUSY);           // wait your turn
    }

    if( !(rqstP->mode & (HSC_MODE_FROMMEM | HSC_MODE_TOMEM)) )
    {
        logger(LOG_HSC, "request_channel called bad mode %x\n", rqstP->mode);
        return( HSC_ERR );      // no from or to, nothing to do
    }

    if( (rqstP->mode & HSC_MODE_TOMEM) && (rqstP->toBufferP == 0) )
    {
        logger(LOG_HSC, "request_channel called bad read address 0\n");
        return( HSC_ERR );      // bad address
    }

    if( (rqstP->mode & HSC_MODE_FROMMEM) && (rqstP->fromBufferP == 0) )
    {
        logger(LOG_HSC, "request_channel called bad write address 0\n");
        return( HSC_ERR );      // bad address
    }

    // IMMEDIATE, THREADED, and TRUESTEAL all handle the transfer in this call.
    // IMMEDIATE does not check for busy nor does it do any timing emulation.
    // THREADED operates as if in normal mode, including a 5usec delay per count, with busy and wait.
    // TRUESTEAL is like THREADED but for a device slower than memory speed (wordTime > 50),
    // it spreads its steal ticks evenly across the transfer's actual real-time duration.
    if( rqstP->mode & (HSC_MODE_IMMEDIATE | HSC_MODE_THREADED | HSC_MODE_TRUESTEAL) )
    {
        switch( rqstP->mode & (HSC_MODE_IMMEDIATE | HSC_MODE_THREADED | HSC_MODE_TRUESTEAL) )
        {
        case HSC_MODE_IMMEDIATE:
            logger(LOG_HSC, "request_channel immediate transfer\n");
            processImmediate(rqstP);
            ctlP->status = HSC_DONE;
            logger(LOG_HSC, "request_channel immediate transfer done\n");
            if( rqstP->mode & HSC_MODE_UPDATEPANEL )
            {
                // We turn the hsc cycle light on, is turned off in the process loop.
                pdp1P->hsc = 1;
                updatelights(pdp1P, pdp1P->panel);
                updatelights_pwm(pdp1P->panel, 1);
                ctlP->onCount = rqstP->count;          // keep it on for the number of transfers we do
                if( ctlP->onCount < HSC_STRETCH )
                {
                    ctlP->onCount = HSC_STRETCH;
                }
                ctlP->needLightoff = true;
            }
            return( HSC_OK );

        case HSC_MODE_THREADED:
            logger(LOG_HSC, "request_channel threaded transfer\n");
            if( ctlP->status == HSC_BUSY )
            {
                return( HSC_ERR );      // not now
            }

            // Mark the channel busy so processHSCchannels() will process it.
            ctlP->status = HSC_BUSY;

            ctlP->waitDelay = rqstP->count * 5;       // 5us per word
            ctlP->brkCount = rqstP->count;  // hack to vaguely simulate the break conditions

            if( rqstP->mode & HSC_MODE_UPDATEPANEL )
            {
                pdp1P->hsc = 1;
                updatelights(pdp1P, pdp1P->panel);
                updatelights_pwm(pdp1P->panel, 1);
                ctlP->onCount = rqstP->count;
                if( ctlP->onCount < HSC_STRETCH )
                {
                    ctlP->onCount = HSC_STRETCH;
                }
                ctlP->needLightoff = true;
            }

            processImmediate(rqstP);
            return(HSC_BUSY);

        case HSC_MODE_TRUESTEAL:
            logger(LOG_HSC, "request_channel TRUESTEAL transfer\n");
            if( ctlP->status == HSC_BUSY )
            {
                return( HSC_ERR );      // not now
            }

            if( rqstP->wordTime < 50 )
            {
                // This mode is for devices slower than one memory cycle, 5us, such as the drum.
                // A device at or faster than memory speed should use THREADED instead.
                logger(LOG_HSC, "request_channel TRUESTEAL wordTime %d too fast for TRUESTEAL\n",
                    rqstP->wordTime);
                return( HSC_ERR );
            }

            // Mark the channel busy so processHSCchannels() will process it.
            // Unlike THREADED, this channel stays busy for the transfer's total tick count,
            // as the original hardware would do.
            ctlP->status = HSC_BUSY;
            ctlP->waitDelay = 0;
            ctlP->brkCount = 0;
            ctlP->trueSteal = true;
            ctlP->stealsLeft = rqstP->count;
            // Round count*word-transfer-time (in us/10) to the nearest whole 5us tick.
            ctlP->ticksLeft = ((rqstP->count * rqstP->wordTime) + 25) / 50;

            if( ctlP->ticksLeft < ctlP->stealsLeft )
            {
                ctlP->ticksLeft = ctlP->stealsLeft;   // never owe fewer ticks than steals
            }
            ctlP->stealAcc = 0;

            logger(LOG_HSC, "TRUESTEAL ticks left %d, steals left %d\n", ctlP->ticksLeft, ctlP->stealsLeft);

            if( rqstP->mode & HSC_MODE_UPDATEPANEL )
            {
                pdp1P->hsc = 1;
                updatelights(pdp1P, pdp1P->panel);
                updatelights_pwm(pdp1P->panel, 1);
                ctlP->onCount = ctlP->ticksLeft;
                if( ctlP->onCount < HSC_STRETCH )
                {
                    ctlP->onCount = HSC_STRETCH;
                }
                ctlP->needLightoff = true;
            }

            processImmediate(rqstP);
            return(HSC_BUSY);

        default:
            logger(LOG_HSC, "request_channel illegal request\n");
            return( HSC_ERR );  // can't have both
        }
    }

    // Ok, chan is free, set it up and go.
    lockControl(ctlP);
    memcpy(&(ctlP->request), rqstP, sizeof(HSCRequest));
    ctlP->status = HSC_BUSY;
    logger(LOG_EXEC, "channel %d set to BUSY, addr %d:%o\n", chanP->chanNo+1, rqstP->memBank, rqstP->memAddr);
    pdp1P->hsc = 1;      // be sure our in-use light is on
    ctlP->onCount = rqstP->count;
    ctlP->needLightoff = true;
    unlockControl(ctlP);
    return( HSC_BUSY );
}

// Validate a channel and return its control ptr.
// If invalid, return null.
HSCControlP
getControlP(HSCChannelP chanP)
{
HSCControlP ctlP;

    if( !chanP || (chanP->chanNo < 0) || (chanP->chanNo >= NUMCHANS) )
    {
        return(NULL);            // someone is cheating
    }

    ctlP = chans[chanP->chanNo];
    return(ctlP);
}

// Called from user to wait for a response.
// Returns a status value or HSC_ERROR if the chanP is invalid.
int
HSCwait(HSCChannelP chanP)
{
int status;
HSCControlP ctlP;

    if( !(ctlP = getControlP(chanP)) )
    {
        return( HSC_ERR );
    }

    // Emulator said to stop any ongoing transfers
    if( ctlP->status == HSC_ABORT )
    {
        ctlP->status = HSC_DONE;
        return( HSC_ABORT );
    }

    // Special case for THREADED pseudo-delay
    if( ctlP->waitDelay > 0 )
    {
        // If the channel already reached HSC_DONE on its own, we're done.
        if( ctlP->status == HSC_DONE )
        {
            ctlP->waitDelay = 0;
            return(HSC_DONE);
        }

        // We aren't necessarily in the same thread as the main emulator,
        // just idle if it isn't in run state.
        while( !pdp1P->run )
        {
            usleep(100);
        }

        // Enforce the simulated transfer delay.
        // Short delays busy-spin, longer usleep().
        if( ctlP->waitDelay <= HSC_SPIN_LIMIT_US )
        {
            hscSpinWait(ctlP->waitDelay);
        }
        else
        {
            usleep(ctlP->waitDelay);
        }

        ctlP->waitDelay = 0;

        // The real data transfer already happened synchronously back in HSCexecute(),
        // all we were waiting for here is the simulated delay completion.
        lockControl(ctlP);
        ctlP->brkCount = 0;
        ctlP->status = HSC_DONE;
        HSCdone(ctlP);
        unlockControl(ctlP);

        return(HSC_DONE);
    }

    lockControl(ctlP);
    if( (ctlP->status == HSC_BUSY) && !(ctlP->isWaiting) )
    {
        ctlP->isWaiting = true;
        unlockControl(ctlP);
        sem_wait(&(ctlP->waitSemaphore));
        status = ctlP->status;
    }
    else
    {
        status = ctlP->status;
        unlockControl(ctlP);
    }

    return( status );
}

int
HSCgetStatus(HSCChannelP chanP)
{
HSCControlP ctlP;

    if( !(ctlP = getControlP(chanP)) )
    {
        return( HSC_ERR );
    }

    return( ctlP->status );
}

// And how we complete.
// ctlP should be locked before calling this.
static void
HSCdone(HSCControlP ctlP)
{
    if( ctlP->isWaiting )
    {
        ctlP->isWaiting = false;
        sem_post(&(ctlP->waitSemaphore));
    }
}

static void
lockControl(HSCControlP ctlP)
{
    sem_wait(&(ctlP->accessSemaphore));
}

static void
unlockControl(HSCControlP ctlP)
{
    sem_post(&(ctlP->accessSemaphore));
}

// Busy-wait for approximately the given number of microseconds.
static void
hscSpinWait(int us)
{
struct timespec tm;
uint64_t startNs;
uint64_t nowNs;
uint64_t targetNs;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    startNs = tm.tv_nsec;
    startNs += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    targetNs = (uint64_t)us * 1000;

    nowNs = startNs;
    while( (nowNs - startNs) < targetNs )
    {
        clock_gettime( CLOCK_MONOTONIC, &tm );
        nowNs = tm.tv_nsec;
        nowNs += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    }
}

// This is a special case.
// It does not block or wait, it immediately completes.
// It does not set or clear the hs light.
static void
processImmediate(HSCRequestP rqstP)
{
uint32_t *memBaseP;

    memBaseP = &pdp1P->core[rqstP->memBank * 4096];

#ifdef DOLOGGING
    if( rqstP->mode & HSC_MODE_FROMMEM )
    {
        logger(LOG_DATA,"Transfer %d words from core addr %06o\n",
            rqstP->count, (rqstP->memBank * 4096) + rqstP->memAddr);
    }

    if( rqstP->mode & HSC_MODE_TOMEM )
    {
        logger(LOG_DATA,"Transfer %d words to core addr %06o\n",
            rqstP->count, (rqstP->memBank * 4096) + rqstP->memAddr);
    }
#endif

    while( rqstP->count-- > 0 )
    {
        if( rqstP->memAddr > 4095 )
        {
            rqstP->memAddr = 0;
        }

        // Always get from mem first
        if( rqstP->mode & HSC_MODE_FROMMEM )
        {
            *(rqstP->fromBufferP++) = *(memBaseP + rqstP->memAddr) & 0777777;   // just for cleanliness
        }

        if( rqstP->mode & HSC_MODE_TOMEM )
        {
            *(memBaseP + rqstP->memAddr) = *(rqstP->toBufferP++) & 0777777;
        }

        ++(rqstP->memAddr);
    }
}

// Process one channel, one word.
// We do a read before a write if both are enabled and we are in normal mode.
// Returns true if a cycle steal is needed, else false.
static bool
processChannel(HSCControlP ctlP)
{
bool steal;
uint32_t fullAddr;
uint32_t data;
HSCRequestP rqstP;

    steal = false;
    if( !ctlP->isAssigned || (ctlP->status != HSC_BUSY) )
    {
        return(steal);
    }

    lockControl(ctlP);
    rqstP = &(ctlP->request);

    // If a TRUESTEAL transfer is in progress, see if we need to steal a cycle.
    // Spreads stealsLeft steal-ticks evenly across ticksLeft total total ticks.
    // As a side note, this is a version of the Bresenham algorithm used for drawing smooth lines.
    // That algorithm ended up being very useful for many things.
    if( ctlP->trueSteal )
    {
        steal = false;
        ctlP->stealAcc += ctlP->stealsLeft;

        if( ctlP->stealAcc >= ctlP->ticksLeft )
        {
            ctlP->stealAcc -= ctlP->ticksLeft;
            steal = true;
            ctlP->stealsLeft--;
        }

        ctlP->ticksLeft--;

        if( ctlP->ticksLeft <= 0 )
        {
            logger(LOG_HSC, "processChannel TRUESTEAL marking DONE\n");
            ctlP->status = HSC_DONE;
            ctlP->trueSteal = false;
            ctlP->needLightoff = true;
            HSCdone(ctlP);
        }

        unlockControl(ctlP);
        return(steal);
    }

    // If a THREADED operation was done, fake break cycles.
    if( ctlP->brkCount > 0 )
    {
        ctlP->brkCount--;
        steal = true;

        if( ctlP->brkCount <= 0 )
        {
            logger(LOG_HSC, "processChannel threaded marking DONE\n");
            ctlP->status = HSC_DONE;
            ctlP->needLightoff = true;
            HSCdone(ctlP);
        }

        unlockControl(ctlP);
        return(steal);
    }

    if( rqstP->count-- > 0 )
    {
        // We wrap
        if( rqstP->memAddr > 4095 )
        {
            rqstP->memAddr = 0;
        }

        fullAddr = (rqstP->memBank * 4096) + rqstP->memAddr;

        // We do a read from memory before a write to memory, same as the original hardware
        if( rqstP->mode & HSC_MODE_FROMMEM )
        {
            data = pdp1P->core[fullAddr];
            *(rqstP->fromBufferP++) = data & 0777777;
            logger(LOG_DATA,"%06o from core %o\n", data, fullAddr);
        }

        if( rqstP->mode & HSC_MODE_TOMEM )
        {
            data = *(rqstP->toBufferP++) & 0777777;   // just for cleanliness
            logger(LOG_DATA,"%06o to core %o\n", data, fullAddr);
            pdp1P->core[fullAddr] = data;
        }

        rqstP->memAddr++;
        steal = true;
    }

    // We might still need to steal a cycle if we completed a transfer, so don't change the steal state.
    if( rqstP->count <= 0 )
    {
        logger(LOG_HSC, "processChannel marking DONE\n");
        ctlP->status = HSC_DONE;
        ctlP->needLightoff = true;
        HSCdone(ctlP);
    }

    unlockControl(ctlP);
    return( steal );
}
