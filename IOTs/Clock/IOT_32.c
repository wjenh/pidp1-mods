#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

// #define DOLOGGING
#include "Logger/iotLogger.h"

// An extended implementation of the PDP-1D timeshare clock.
// IOT 32 reads the current 1ms counter, 0-59999 dec.
// IOT 32 with bit 7 set is an extension, set clock, IOT 2032:
// AC register contains flags:
// 000 000 0eI iMM MMm mmm
// e enables the clock, 1 to enable, 0 is as a regular IOT 32, disabling also clears the interrupt enables.
// I enables the 1 min interrupt. 1 enables, 0 disables.
// i enables the 32 ms interrupt. 1 enables, 0 disables.
// MMMM is the channel to use for the 1 min interrupt.
// mmmm is the channel to use for the 32 ms interrupt.
// Why AC? Because all IOTs 30-37 automatically clear the IO register!

static int enabled;
static int counter;
static int enable32ms;
static int channel32ms;
static int enable1min;
static int channel1min;
static int completeNeeded;

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int op;

    if( pulse )
    {
        return(1);
    }

    if( (pdp1P->mb & 03700) == 02000 )     // IOT 2032, pay attention to rest
    {
        op = (pdp1P->ac >> 8) & 07;
        iotLog("In iot 2032 io %o op %o\n", pdp1P->ac, op);
        completeNeeded = 0;

        if( op & 04 )
        {
            enabled = 1;
            enablePolling(200); // every 200 cycles, 1ms
            iotLog("In iot 32 clk enabled\n");

            enable32ms = enable1min = 0;

            if( op & 02 )
            {
                enable1min = 1;
                channel1min = (pdp1P->ac & 0360) >> 4;
                iotLog("In iot 32 1min interrupt chan %o enabled\n", channel1min);
            }

            if( op & 01 )
            {
                enable32ms = 1;
                channel32ms = pdp1P->ac & 017;
                iotLog("In iot 32 32ms interrupt chan %o enabled\n", channel32ms);
            }

            if( !enable1min && !enable32ms && completion )
            {
                completeNeeded = 1;     // completion pulse when either done
                iotLog("In iot 32 completion needed\n");
            }
        }
        else
        {
            enabled = enable32ms = enable1min = 0;
        }
    }
    else
    {
        if( enabled )
        {
            pdp1P->io = counter;
        }
    }

    IOCOMPLETE_IFNEEDED(pdp1P, completion && !completeNeeded);
    return(1);
}

void iotPoll(PDP1 *pdp1P)
{
    // we are called every 1msec
    if( enabled )
    {
        if( enable32ms && ((counter & 0x3F) == 0x20) )  // 32 msecs
        {
            initiateBreak(channel32ms);
            completeNeeded = 0;
        }

        if( counter++ > 59999 ) // 1 min wraparound
        {
            counter = 0;
            if( enable1min )
            {
                initiateBreak(channel1min);
                completeNeeded = 0;
            }
        }
        
        if( completeNeeded && !enable32ms && !enable1min )
        {
            // just complete on 1ms tick
            completeNeeded = 0;
            IOCOMPLETE(pdp1P);
        }
    }
}
