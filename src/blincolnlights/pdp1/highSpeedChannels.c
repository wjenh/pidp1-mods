/*
 * This is a loose implementation of the Type 19 High Speed Channel Control.
 * It is for use in IOTs or other emulator code to package up direct memory access
 * and simulate the behavior of the PDP-1 dma, hiding details of memory back wraparound, etc.
 * It does one read/write operation every run cycle, 5us.
 * It can cycle-steal, when it does it takes over for as long as it takes to transfer all data at 5us per
 * word transfer, a simultaenous read/write counts as one word transfer, 5us.
 *
*/

#include <unistd.h>
#include <pthread.h>

//#define DOLOGGING
#define LOG_HSC 0   // logger enable for this

#include "common.h"
#include "pdp1.h"
#include "logger.h"
#include "highSpeedChannels.h"

// Original had three channels, priority ordered 1-3
static HSC_Control chan1;
static HSC_Control chan2;
static HSC_Control chan3;

static HSC_ControlP HSC_chans[] = {&chan1, &chan2, &chan3};

static void processChannel(PDP1 *pdp1P, HSC_ControlP controlP);
static void processImmediate(PDP1 *pdp1P, int mode, int count, int memBank, int memAddr,
    Word *toBufferP, Word *fromBufferP);

// Service routine called from run loop. Question - did the hardware pause on a halt, or complete?
// Returns 0 if it took no time, 1 if it did a 'memory cycle' and we are in steal mode.
bool
processHSChannels(PDP1 *pdp1P)
{
int i;
HSC_ControlP controlP;

    // we do in priority order, 0 being highest
    for( i = 0; controlP = HSC_chans[i++]; i < 3 )
    {
        if( controlP->status == HSC_BUSY )
        {
            processChannel(pdp1P, controlP);
            return( controlP->mode & HSC_MODE_STEAL );        // we processed one, maybe steal, maybe not
        }
    }

    return(false);
}

// main interaction from user side.
// Returns HSC_ERR for invalid chan, mode, count > 4096, banks out of range of 0-15 dec.
int
HSC_request_channel(
    PDP1 *pdp1P,        // emulator context
    int chan,           // channel,1-3, channel 1 being highest priority
    int mode,           // HSC_MODE_FROMMEM, _TOMEM, _IMMEDIATE (or'd together)
    int count,          // number of words to transfer, 0-4096
    int memBank,         // memory bank to copy to, 0-15
    int memAddr,         // address in bank, 0-4095
    Word *toBufferP,     // the user buffer to copy to memory, should be 4096 words
    Word *fromBufferP)   // the user buffer to copy memory into, should be 4096 words
{
HSC_ControlP controlP;

    if( (chan < 1) || (chan > 3) )
    {
        logger(LOG_HSC, "request_channel called bad channel %d\n", chan);
        return( HSC_ERR );
    }
    
    if( (memBank > 15) || (memBank < 0) || (memAddr > 4095) || (memAddr < 0) || (count > 4096))
    {
        logger(LOG_HSC, "request_channel called bad addr or countm bank %d addr %d count %d\n",
            memBank, memAddr, count);
        return( HSC_ERR );
    }

    if( !(mode & (HSC_MODE_FROMMEM | HSC_MODE_TOMEM)) )
    {
        logger(LOG_HSC, "request_channel called bad mode %x\n", mode);
        return( HSC_ERR );      // no from or to, nothing to do
    } 

    if( (mode & HSC_MODE_TOMEM) && (toBufferP == 0) )
    {
        return( HSC_ERR );      // bad address
    }

    if( (mode & HSC_MODE_FROMMEM) && (fromBufferP == 0) )
    {
        return( HSC_ERR );      // bad address
    }

    if( mode & HSC_MODE_IMMEDIATE )     // do it now, no -1 timing emulation, don't care if busy
    {
        logger(LOG_HSC, "request_channel immediate transfer\n");

        processImmediate(pdp1P, mode, count, memBank, memAddr, toBufferP, fromBufferP);
        controlP->status = HSC_DONE;

        logger(LOG_HSC, "request_channel immediate transfer done\n");
        return( HSC_OK );
    }

    controlP = HSC_chans[chan - 1];

    if( controlP->status == HSC_BUSY )
    {
        logger(LOG_HSC, "request_channel called but still busy\n");
        return( HSC_BUSY );
    }

    // Ok, chan is free, set it up and go.
    controlP->mode = mode;
    controlP->count = count;
    controlP->memBank = memBank;
    controlP->memAddr = memAddr;
    controlP->toBufP = toBufferP;
    controlP->fromBufP = fromBufferP;

    controlP->status = HSC_BUSY;

    logger(LOG_HSC, "channel %d set to BUSY\n", chan);
    return( HSC_OK );
}

static void
processImmediate(
    PDP1 *pdp1P,        // emulator context
    int mode,           // HSC_MODE_FROMMEM, _TOMEM, _IMMEDIATE (or'd together)
    int count,          // number of words to transfer, 0-4096, 0 means 4096
    int memBank,         // memory bank to copy to, 0-15
    int memAddr,         // address in bank, 0-4095
    Word *toBufferP,     // the user buffer to copy to memory, should be 4096 words
    Word *fromBufferP)   // the user buffer to copy memory into, should be 4096 words
{
Word *memBaseP;

    memBaseP = &pdp1P->core[memBank * 4096];

    while( count-- > 0 )
    {
        if( memAddr > 4095 )
        {
            memAddr = 0;
        }

        // Always get from mem first
        if( mode & HSC_MODE_FROMMEM )
        {
            *fromBufferP++ = *(memBaseP + memAddr);
        }

        if( mode & HSC_MODE_TOMEM )
        {
            *(memBaseP + memAddr) = *toBufferP++;
        }

        ++memAddr;
    }
}

int HSC_get_status(int chan)
{
int status;

    if( (chan < 1) || (chan > 3) )
    {
        return( HSC_ERR );
    }

    status = HSC_chans[chan - 1]->status;
    return( status );
}

// process one channel, one word.
// We do a read before a write if both are enabled.
// Caller will have locked.
static void
processChannel(PDP1 *pdp1P, HSC_ControlP controlP)
{
Word word;

    if( controlP->status != HSC_BUSY )
    {
        return;
    }

    // are we done?
    if( controlP->count <= 0 )
    {
        logger(LOG_HSC, "processChannel marking DONE\n");
        controlP->status = HSC_DONE;
        pdp1P->hsc = 0;
        return;
    }

    controlP->count--;
    pdp1P->hsc = 1;      // be sure our in-use light is on
    //updatelights(pdp1P, pdp1P->panel);

    if( controlP->memAddr > 4095 )
    {
        controlP->memAddr = 0;
    }

    // We do a read from memory before a write to memory, same as the original hardware
    if( controlP->mode & HSC_MODE_FROMMEM )
    {
        *(controlP->fromBufP++) = pdp1P->core[(controlP->memBank * 4096) + controlP->memAddr] & 0777777;
    }

    if( controlP->mode & HSC_MODE_TOMEM )
    {
        pdp1P->core[(controlP->memBank * 4096) + controlP->memAddr] = *(controlP->toBufP++) & 0777777;
    }

    controlP->memAddr++;
}
