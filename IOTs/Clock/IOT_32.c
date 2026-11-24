// An extended implementation of the PDP-1D timeshare clock.
// IOT 32 reads the current 1ms counter, 0-59999 dec.
//
// IOT 32 with bit 7 set is an extension, set clock, IOT 2032:
// AC register contains flags:
// 000 000 seI iMM MMm mmm
// s enables the SBS16 system, it will remain enabled regardless of this bit on subsequent calls
// e enables the clock, 1 to enable, 0 is as a regular IOT 32, disabling also clears the interrupt enables.
// I enables the 1 min interrupt. 1 enables, 0 disables.
// i enables the 32 ms interrupt. 1 enables, 0 disables.
// MMMM is the channel to use for the 1 min interrupt.
// mmmm is the channel to use for the 32 ms interrupt.
//
// IOT 32 with bits 7 amd 11 set is a nonstandard extension, set countdown timer, IOT 2132:
// AC register contains flags:
// ecc cct ttt ttt ttt ttt
// e enables interrupts for the timer
// cccc is the channel to use for the interupt
// ttttttttttttt is the count in milliseconds, 1-8191 dec, 0 to reset and disable
// Why AC? Because all IOTs 30-37 automatically clear the IO register!
//
// Both count milliseconds of run time (IOT_TIME_RUN): simtime while the machine runs, so stolen
// cycles and the whole of a mul or div count, and a halt, a single step's stop or a throttle
// lag-cap span does not. Each has its own phase: the clock's from when it was enabled, the
// countdown's from its IOT, so a count of N takes N ms. One deadline serves both, at the earlier
// of the two next times.
//
// The i and c bits: a read completes at once. An IOT 2032 that enables the clock with no interrupt
// completes on the next 1 ms tick, and an IOT 2132 that starts a count completes when it reaches 0;
// otherwise both complete at once. Each IOT decides from its own operation only: one owed by an
// earlier IOT does not hold back this one's, as the machine has one sync flip-flop for all devices.
//
// IOT 32 with bits 7 amd 10 set is a nonstandard extension, read simtime, IOT 2232:
// AC register contains the low 18 bits of simtime / 1000
// IO register contains the next higher 18 bits of simtime / 1000
// This is purely for emulator debugging, it is not at all historically relevant.
// Note that simtime is a 64 bit quantity and is in nanoseconds.
// The value here is simtime / 1000, microseconds
// The low 18 bits of the microsecond value is in AC.
// The next highest 18 bits are in IO.
// The value will wrap around approximately every 1145 minutes.
//
// 04-Oct-2026 Claude power clear stops the clock and the countdown and clears CKS 020.
// 22-Nov-2026 Claude an IOT's own completion no longer waits for one an earlier IOT still has pending.
// 24-Nov-2026 wje add simtime debugging 'clock'

#include "iotHandler.h"

//#define DOLOGGING
#include "iotLogger.h"

// cks bit 13 is set when the countdown timer completes, cleared when counter is set again or reset
#define COUNTER_CKS_FLAG 0000020

#define TICKNS MSTONS(1)

static int enabled;
static int counter;
static int enable32ms;
static int channel32ms;
static int enable1min;
static int channel1min;
static int completeNeeded;

static int countdown;
static int counterInterrupt;
static int counterChannel;
static int counterCompleteNeeded;

static uint64_t nextTick;           // run time of the clock's next 1 ms tick, while enabled
static uint64_t nextCount;          // run time of the countdown's next 1 ms count, while counting
static PDP1 *lastPdp1P;             // from the last call that had it, for the power clear; NULL
                                    // if none, and then CKS 020 cannot have been set

static void armDeadline(void);

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int op;
int i;
int deferred;       // this IOT's completion comes later, from iotDeadline()

    if( pulse )
    {
        return(1);
    }

    lastPdp1P = pdp1P;
    deferred = 0;
    iotLog("In clk iot mb %o dev %o\n", MB(pdp1P), dev);

    if( (MB(pdp1P) & 03700) == 02000 )     // IOT 2032, pay attention to rest
    {
        op = (AC(pdp1P) >> 8) & 017;
        iotLog("In iot 2032 io %o op %o\n", AC(pdp1P), op);
        completeNeeded = 0;

        if( op & 04 )
        {
            if( !enabled )
            {
                nextTick = iotTime(IOT_TIME_RUN) + TICKNS;  // an enabled clock keeps its phase
            }

            enabled = 1;
            iotLog("In iot 32 clk enabled\n");

            enable32ms = enable1min = 0;

            if( op & 02 )
            {
                enable1min = 1;
                channel1min = (AC(pdp1P) & 0360) >> 4;
                iotLog("In iot 32 1min interrupt chan %o enabled\n", channel1min);
            }

            if( op & 01 )
            {
                enable32ms = 1;
                channel32ms = AC(pdp1P) & 017;
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

        if( op & 010 )
        {
            pdp1P->sbs16 = 1;
        }

        deferred = completeNeeded;
        armDeadline();
    }
    else if( (MB(pdp1P) & 03700) == 02100 )     // IOT 2132, countdown timer
    {
        CKS(pdp1P) &= ~COUNTER_CKS_FLAG;   // clear in case it was set
        i = countdown;
        countdown = AC(pdp1P) & 017777;     // the count
        if( countdown )
        {
            counterCompleteNeeded = completion;
            counterChannel = (AC(pdp1P) >> 13) & 017;
            counterInterrupt = AC(pdp1P) & 0400000;
            iotLog("IOT 2132, countdown set to %d, completion %d\n", countdown, counterCompleteNeeded);
            nextCount = iotTime(IOT_TIME_RUN) + TICKNS;    // a new count starts its own phase
        }
        else
        {
            counterInterrupt = 0;
            counterCompleteNeeded = 0;
            iotLog("IOT 2132, countdown cleared\n");
        }

        deferred = counterCompleteNeeded;
        armDeadline();
        AC(pdp1P) = i;
    }
    else if( (MB(pdp1P) & 03700) == 02200 )     // IOT 2232, debug clock
    {
        AC(pdp1P) = (pdp1P->simtime / 1000) & 0777777;
        IO(pdp1P) = ((pdp1P->simtime / 1000) >> 18) & 0777777;
    }
    else
    {
        if( enabled )
        {
            IO(pdp1P) = counter;
        }
    }

    IOCOMPLETE_IFNEEDED(pdp1P, completion && !deferred);
    return(1);
}

// Called at the earlier of the clock's next tick and the countdown's next count. Each is
// checked against the run time now and, when due, done and moved on by exactly 1 ms, so neither
// drifts. A pass is far shorter than 1 ms, so each is due at most once a call.
void
iotDeadline(PDP1 *pdp1P)
{
uint64_t now;

    lastPdp1P = pdp1P;
    now = iotTime(IOT_TIME_RUN);

    if( enabled && (nextTick <= now) )
    {
        nextTick += TICKNS;
        ++counter;

        if( enable32ms && ((counter & 0x1F) == 0x10) )  // audit M6: fires every 32 ticks of the 1ms counter
        {
            initiateBreak(channel32ms);
            completeNeeded = 0;
        }

        if( counter > 59999 ) // 1 min wraparound
        {
            counter = 0;
            if( enable1min )
            {
                iotLog("IOT 2032 initiating 1 min break\n");
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

    if( countdown && (nextCount <= now) )
    {
        nextCount += TICKNS;
        if( --countdown == 0 )
        {
            iotLog("IOT 2132 poll, countdown reached\n");
            if( counterInterrupt )
            {
                iotLog("IOT 2132 poll, initiating break on %d\n", counterChannel);
                initiateBreak(counterChannel);
            }

            if( counterCompleteNeeded )
            {
                iotLog("IOT 2132 poll, issuing complete\n");
                IOCOMPLETE(pdp1P);
            }

            counterCompleteNeeded = 0;
            CKS(pdp1P) |= COUNTER_CKS_FLAG;
        }
    }

    armDeadline();
}

// Called once when the power switch is turned off.
// The clock is  disabled, its count set to 0, both interrupts off,
// no countdown and no completion pending, and the countdown's CKS 020 clear.
// No return value.
void
iotPowerClear(void)
{
    enabled = 0;
    counter = 0;
    enable32ms = channel32ms = 0;
    enable1min = channel1min = 0;
    completeNeeded = 0;

    countdown = 0;
    counterInterrupt = counterChannel = 0;
    counterCompleteNeeded = 0;

    if( lastPdp1P )
    {
        CKS(lastPdp1P) &= ~COUNTER_CKS_FLAG;
    }
}

// Arms the deadline at the earlier of the next tick and the next count or cancels it when
// neither the clock nor the countdown is running.
static void
armDeadline(void)
{
uint64_t next;

    next = UINT64_MAX;
    if( enabled )
    {
        next = nextTick;
    }

    if( countdown && (nextCount < next) )
    {
        next = nextCount;
    }

    if( next == UINT64_MAX )
    {
        iotPollCancel();
    }
    else
    {
        iotPollAt(IOT_TIME_RUN, next);
    }
}
