/*
 * This is a loose implementation of the Type 19 High Speed Channel Control.
 * It is for use in IOTs or other emulator code to package up direct memory access
 * and simulate the behavior of the PDP-1 dma, hiding details of memory back wraparound, etc.
 * It does one read/write operation every run cycle, 5us.
 * It can cycle-steal, when it does it takes over for as long as it takes to transfer all data at 5us per
 * word transfer, a simultaenous read/write counts as one word transfer, 5us.
 *
 * 23-Apr-2026 wje - rework to make it more realistic
*/

#include <unistd.h>
#include <pthread.h>

//#define DOLOGGING
#define LOG_HSC 0
#define LOG_EXEC 0
#define LOG_DATA 0

#include "common.h"
#include "pdp1.h"
#include "logger.h"
#include "highSpeedChannels.h"

typedef struct {
    bool isInitialized;
    bool isAssigned;
    bool isWaiting;
    int status;
    int waitDelay;          // if we are in THREADED mode, how long to sleep in HSCwait()
    sem_t accessSemaphore;   // how we synchrnonize modification of the control structure
    sem_t waitSemaphore;    // how we synchrnonize completion
    HSCRequest request;     // pending request if any, copied by execute from user space
    } HSCControl, *HSCControlP;

// Original had three channels, priority ordered 1-3, we do 5, same priority order
#define NUMCHANS 5

static HSCControl chan1;
static HSCControl chan2;
static HSCControl chan3;
static HSCControl chan4;
static HSCControl chan5;

static HSCControlP chans[] = {&chan1, &chan2, &chan3, &chan4, &chan5};

static HSCControlP getControlP(HSCChannelP chanP);
static void lockControl(HSCControlP ctlP);
static void unlockControl(HSCControlP ctlP);
static void hscDone(HSCControlP controlP);
static void processImmediate(HSCRequestP requestP);
static bool processChannel(HSCControlP controlP);

extern PDP1P pdp1P;     // from main.c

// Service routine called from run loop. Question - did the hardware pause on a halt, or complete?
// Returns 0 if it took no time, 1 if it did a 'memory cycle' and we are in steal mode.
bool
processHSCchannels()
{
int i;
HSCControlP ctlP;

    // we do in priority order, 0 being highest
    for( i = 0; ctlP = chans[i++]; i < NUMCHANS )
    {
        // The channels are scanned from low to high, first one that needs a cycle steal wins.
        if( ctlP->status == HSC_BUSY )
        {
            if( processChannel(ctlP) )
            {
                return( true );        // we processed one, steal a cycle
            }
        }
    }

    return(false);
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

// Main interaction from user side.
// Returns HSC_ERR for invalid chan, mode, count > 4096, banks out of range of 0-15 dec.
// Returns HSC_BUSY if a request is still executing.
// Returns HSC_OK otherwise.
int
HSCexecute(HSCChannelP chanP, HSCRequestP rqstP)
{
HSCControlP ctlP;

    if( (rqstP->memBank > 15) || (rqstP->memBank < 0) || (rqstP->memAddr > 4095) || (rqstP->memAddr < 0) ||
        (rqstP->count > 4096) )
    {
        logger(LOG_HSC, "request_channel called bad addr or countm bank %d addr %d count %d\n",
            rqstP->memBank, rqstP->memAddr, rqstP->count);
        return( HSC_ERR );
    }

    if( !(ctlP = getControlP(chanP)) || !ctlP->isAssigned )
    {
        return( HSC_ERR );
    }

    if( ctlP->status == HSC_BUSY )
    {
        return(HSC_BUSY);           // wait your turn
    }

    if( !(rqstP->mode & (HSC_MODE_FROMMEM | HSC_MODE_TOMEM)) )
    {
        logger(LOG_HSC, "request_channel called bad mode %x\n", rqstP->mode);
        return( HSC_ERR );      // no from or to, nothing to do
    } 

    if( (rqstP->mode & HSC_MODE_TOMEM) && (rqstP->toBufferP == 0) )
    {
        return( HSC_ERR );      // bad address
    }

    if( (rqstP->mode & HSC_MODE_FROMMEM) && (rqstP->fromBufferP == 0) )
    {
        return( HSC_ERR );      // bad address
    }

    // Both IMMEDIATE and THREADED handle the tranfer in this call.
    // The difference is that IMMEDIATE does not check for busy nor does it do any timing emulation.
    // THREADED operates as if in normal mode, including a 5usc delay per count, with busy and wait.
    if( rqstP->mode & (HSC_MODE_IMMEDIATE | HSC_MODE_THREADED) )
    {
        switch( rqstP->mode & (HSC_MODE_IMMEDIATE | HSC_MODE_THREADED) )
        {
        case HSC_MODE_IMMEDIATE:
            logger(LOG_HSC, "request_channel immediate transfer\n");
            processImmediate(rqstP);
            ctlP->status = HSC_DONE;
            logger(LOG_HSC, "request_channel immediate transfer done\n");
            return( HSC_OK );

        case HSC_MODE_THREADED:
            logger(LOG_HSC, "request_channel threaded transfer\n");
            if( ctlP->status == HSC_BUSY )
            {
                return( HSC_ERR );      // not now
            }

            ctlP->waitDelay = rqstP->count * 5;       // 5us per word
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
    unlockControl(ctlP);
    return( HSC_BUSY );
}

// Validate a channel and return its control ptr.
// If invalid, return null.
HSCControlP
getControlP(HSCChannelP chanP)
{
HSCControlP ctlP;

    if( !chanP || (chanP->chanNo < 0) || (chanP->chanNo > NUMCHANS) )
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

    // Special case for THREADED pseudo-delay
    if( ctlP->waitDelay > 0 )
    {
        pdp1P->hsc = 1;
        updatelights(pdp1P, pdp1P->panel);
        usleep(ctlP->waitDelay);
        ctlP->waitDelay = 0;
        pdp1P->hsc = 0;
        updatelights(pdp1P, pdp1P->panel);
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

// This is a special case.
// It does not block or wait, it immediately completes.
// It does not set or clear the hs light.
static void
processImmediate(HSCRequestP rqstP)
{
Word *memBaseP;

    memBaseP = &pdp1P->core[rqstP->memBank * 4096];

    while( rqstP->count-- > 0 )
    {
        if( rqstP->memAddr > 4095 )
        {
            rqstP->memAddr = 0;
        }

        // Always get from mem first
        if( rqstP->mode & HSC_MODE_FROMMEM )
        {
            *(rqstP->fromBufferP++) = *(memBaseP + rqstP->memAddr);
        }

        if( rqstP->mode & HSC_MODE_TOMEM )
        {
            *(memBaseP + rqstP->memAddr) = *(rqstP->toBufferP++);
        }

        ++(rqstP->memAddr);
    }
}

// Process one channel, one word.
// We do a read before a write if both are enabled.
// Returns true if a cycle steal is needed, else false.
static bool
processChannel(HSCControlP ctlP)
{
bool steal;
Word fullAddr;
Word data;
HSCRequestP rqstP;

    steal = false;
    if( !ctlP->isAssigned || (ctlP->status != HSC_BUSY) )
    {
        return(steal);
    }

    lockControl(ctlP);
    rqstP = &(ctlP->request);

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
            *(rqstP->fromBufferP++) = data;
            logger(LOG_DATA,"%06o from core %o\n", data, fullAddr);
        }

        if( rqstP->mode & HSC_MODE_TOMEM )
        {
            data = *(rqstP->toBufferP++) & 0777777;
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
        HSCdone(ctlP);
        pdp1P->hsc = 0;
    }

    unlockControl(ctlP);
    return( steal );
}
