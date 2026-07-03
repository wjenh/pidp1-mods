/*
 * hscharness.c -- standalone C test harness for src/blincolnlights/pdp1/highSpeedChannels.c,
 * covering exactly the one property that IOTs/TestGateway/Tests/T01-T05.am1 structurally
 * cannot: true multi-channel priority arbitration in NORMAL (non-IMMEDIATE, non-THREADED)
 * mode -- see README.md's "What this suite does NOT cover, and why" section for the full
 * explanation of that limitation.
 *
 * This links highSpeedChannels.c directly as a plain object file, bypassing the emulator
 * binary, SDL, dlopen, and the whole IOT plugin mechanism entirely. That is what makes this
 * test possible at all: a single-threaded PDP-1 test program can never have two HSC
 * channels genuinely busy at the same time (starting one freezes the emulated CPU until it
 * finishes -- see T03.am1/T04.am1's header comments), but nothing stops plain C code in a
 * throwaway harness from calling HSCexecute() twice in a row before ever calling
 * processHSCchannels(), which is exactly what real concurrent hardware devices would look
 * like to the scan logic.
 *
 * What is stubbed out: highSpeedChannels.c needs an `extern PDP1P pdp1P` (normally defined
 * in main.c) and calls updatelights()/updatelights_pwm() (normally in the panel code) purely
 * to manage the HSC front-panel indicator light -- irrelevant to the logic under test here,
 * so both are provided as trivial no-op stand-ins below. logger() compiles out entirely
 * (highSpeedChannels.c's own DOLOGGING is commented out), so no logging stub is needed.
 *
 * Build: see the "harness" target in this directory's Makefile.
 * Run: ./hscharness -- prints PASS/FAIL lines and exits nonzero on any failure.
 *
 * 02-Jul-2026 wje/claude -- written alongside the IOT 44 Test Gateway am1 suite, to close
 *    the one gap that suite documents but can't fill itself.
 */

#include <stdio.h>
#include <string.h>

#include "pdp1.h"
#include "highSpeedChannels.h"

// Not declared in highSpeedChannels.h -- it's called only from main.c in the real emulator,
// which presumably gets its own prototype some other way (or relies on old-style implicit
// declaration). Declared explicitly here since this harness builds with -Wall -Wextra.
bool processHSCchannels(void);

// highSpeedChannels.c's own extern -- normally defined and set up by main.c.
PDP1P pdp1P;
static PDP1 thePdp1;

static int failCount = 0;

// Trivial stand-ins for the panel-light update calls highSpeedChannels.c makes. The panel
// pointer is never dereferenced by that code, only passed through, so NULL is fine here.
// No return value.
void
updatelights(PDP1 *pdp, Panel *panel)
{
    (void)pdp;
    (void)panel;
}

// See updatelights() above. No return value.
void
updatelights_pwm(Panel *panel, int n)
{
    (void)panel;
    (void)n;
}

// Reports one check's result to stdout and updates the running failure count.
// No return value.
static void
check(const char *label, int gotOk)
{
    if( gotOk )
    {
        printf("%s pass\n", label);
    }
    else
    {
        printf("%s FAIL\n", label);
        ++failCount;
    }
}

// Runs the priority-arbitration scenario described in the file header comment.
// Returns 0 if every check passed, 1 otherwise.
static int
testPriorityArbitration(void)
{
HSCChannelP chan1P, chan5P;
HSCRequest req1, req5;
Word sentinel;
int i, chan1WordsSeen, chan5WordsSeen;
int chan1Addr, chan5Addr, chan1Count, chan5Count;
int steal;

    sentinel = 0242424;         // a value distinct from every real payload word below

    // memAddr is a bank-relative offset (0-4095), NOT a flat core index -- HSCexecute()
    // rejects anything outside that range (see its own memAddr > 4095 check). Both blocks
    // stay in bank 0, comfortably apart so they can't overlap.
    chan1Addr = 3000;
    chan5Addr = 3100;
    chan1Count = 4;             // higher priority (channel 1 = index 0), fewer words
    chan5Count = 10;            // lower priority (channel 5 = index 4), more words

    for( i = 0; i < chan1Count; ++i )
    {
        thePdp1.core[chan1Addr + i] = sentinel;
    }
    for( i = 0; i < chan5Count; ++i )
    {
        thePdp1.core[chan5Addr + i] = sentinel;
    }

    chan1P = HSCallocateChannel(1);
    chan5P = HSCallocateChannel(5);
    check("HSC-1 allocate channel 1", chan1P != NULL);
    check("HSC-2 allocate channel 5", chan5P != NULL);

    // Start the LOW-priority channel (5) first, exactly as a real lower-priority device
    // might issue its request first -- priority is about scan order, not request order.
    memset(&req5, 0, sizeof(req5));
    req5.mode = HSC_MODE_TOMEM;
    req5.count = chan5Count;
    req5.memBank = 0;
    req5.memAddr = chan5Addr;
    req5.toBufferP = (uint32_t[]){
        0151515, 0252525, 0353535, 0454545, 0555555,
        0656565, 0757575, 0101010, 0202020, 0303030 };
    check("HSC-3 chan5 hgx returns busy", HSCexecute(chan5P, &req5) == HSC_BUSY);

    // Now start the HIGH-priority channel (1). On real hardware this models a second,
    // independent device winning arbitration against the first because it has higher
    // priority, not because it asked first -- and this is the crux of what an am1 test
    // fundamentally cannot set up (see the file header comment).
    memset(&req1, 0, sizeof(req1));
    req1.mode = HSC_MODE_TOMEM;
    req1.count = chan1Count;
    req1.memBank = 0;
    req1.memAddr = chan1Addr;
    req1.toBufferP = (uint32_t[]){ 0606060, 0707070, 0111111, 0222222 };
    check("HSC-4 chan1 hgx returns busy", HSCexecute(chan1P, &req1) == HSC_BUSY);

    // Drive the scan exactly like main.c's main loop does, one processHSCchannels() call
    // per simulated 5us tick, for as many ticks as channel 1 alone needs.
    for( i = 0; i < chan1Count; ++i )
    {
        steal = processHSCchannels();
        check("HSC-5 steal returned true while chan1 draining", steal == 1);
    }

    // Channel 1 (higher priority) must be fully done now...
    check("HSC-6 chan1 status DONE after its own word count", HSCgetStatus(chan1P) == HSC_DONE);

    // ...and channel 5 (lower priority) must not have been touched AT ALL while channel 1
    // was busy -- this is the actual priority-arbitration property under test.
    chan5WordsSeen = 0;
    for( i = 0; i < chan5Count; ++i )
    {
        if( thePdp1.core[chan5Addr + i] != sentinel )
        {
            ++chan5WordsSeen;
        }
    }
    check("HSC-7 chan5 status still BUSY while chan1 was draining", HSCgetStatus(chan5P) == HSC_BUSY);
    check("HSC-8 chan5 untouched while chan1 was draining", chan5WordsSeen == 0);

    // Now channel 1 is out of the way; drive the scan for channel 5's remaining words. The
    // scan should find channel 5 this time (channel 1 no longer busy) and drain it exactly
    // the same way, one word per call.
    for( i = 0; i < chan5Count; ++i )
    {
        steal = processHSCchannels();
        check("HSC-9 steal returned true while chan5 draining", steal == 1);
    }

    check("HSC-10 chan5 status DONE after its own word count", HSCgetStatus(chan5P) == HSC_DONE);

    // One more tick with nothing busy should report no steal at all.
    steal = processHSCchannels();
    check("HSC-11 no steal once both channels are done", steal == 0);

    // And the actual data: verify every word landed correctly, for both channels.
    chan1WordsSeen = 0;
    {
    Word expect1[4] = { 0606060, 0707070, 0111111, 0222222 };
        for( i = 0; i < chan1Count; ++i )
        {
            if( thePdp1.core[chan1Addr + i] == expect1[i] )
            {
                ++chan1WordsSeen;
            }
        }
    }
    check("HSC-12 chan1 data correct", chan1WordsSeen == chan1Count);

    chan5WordsSeen = 0;
    {
    Word expect5[10] = { 0151515, 0252525, 0353535, 0454545, 0555555,
                          0656565, 0757575, 0101010, 0202020, 0303030 };
        for( i = 0; i < chan5Count; ++i )
        {
            if( thePdp1.core[chan5Addr + i] == expect5[i] )
            {
                ++chan5WordsSeen;
            }
        }
    }
    check("HSC-13 chan5 data correct", chan5WordsSeen == chan5Count);

    check("HSC-14 free chan1", HSCfreeChannel(chan1P));
    check("HSC-15 free chan5", HSCfreeChannel(chan5P));

    return(failCount != 0);
}

// audit H1 regression case: drives a THREADED (HSC_MODE_THREADED) request with count > 1
// through processHSCchannels() one word at a time, exactly like processChannel()'s
// brkCount>0 branch that used to leak the access semaphore (unlockControl() was only
// called on the FINAL word, when brkCount reached 0 -- every earlier call in this loop
// returned still holding the lock). Before the fix, the second processHSCchannels() call
// below would deadlock forever on the next lockControl() inside processChannel(); after
// the fix each call returns promptly. This harness has no per-check timeout, so a
// regression here would hang the whole process rather than print a FAIL -- that hang is
// itself the signal something regressed, but it does mean this case must stay last.
// Returns 0 if every check passed, 1 otherwise (mirrors testPriorityArbitration()'s contract).
static int
testThreadedDrainNoDeadlock(void)
{
HSCChannelP chanP;
HSCRequest req;
int i, steal, threadedCount;

    threadedCount = 3;      // > 1, so at least one intermediate processHSCchannels() call
                            // exercises the brkCount>0 branch before the final brkCount<=0 one

    chanP = HSCallocateChannel(2);
    check("HSC-16 allocate channel 2 for THREADED drain", chanP != NULL);

    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM | HSC_MODE_THREADED;
    req.count = threadedCount;
    req.memBank = 0;
    req.memAddr = 3200;
    req.toBufferP = (uint32_t[]){ 0111111, 0222222, 0333333 };
    check("HSC-17 THREADED hgx returns busy", HSCexecute(chanP, &req) == HSC_BUSY);

    // Drain brkCount one word per call, same cadence as the main loop's one-tick-per-call.
    // Every call here previously held the lock forever once it returned (leaked from the
    // very first call, not just the last), so this loop is the actual regression check.
    for( i = 0; i < threadedCount; ++i )
    {
        steal = processHSCchannels();
        check("HSC-18 steal returned true while THREADED chan2 draining", steal == 1);
    }

    check("HSC-19 chan2 status DONE after THREADED drain", HSCgetStatus(chanP) == HSC_DONE);
    check("HSC-20 free chan2", HSCfreeChannel(chanP));

    // Re-allocate the same channel NUMBER and issue a plain (non-THREADED) request on it.
    // HSCallocateChannel() does not re-init the semaphore (isInitialized latches true), so
    // this reuses the exact same accessSemaphore the drain loop above locked/unlocked. If any
    // of those processHSCchannels() calls had leaked the lock, HSCexecute() below would hang
    // on its own lockControl() rather than returning -- this is the real deadlock check.
    chanP = HSCallocateChannel(2);
    check("HSC-21 re-allocate channel 2 (reuses same semaphore)", chanP != NULL);

    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM;
    req.count = 1;
    req.memBank = 0;
    req.memAddr = 3210;
    req.toBufferP = (uint32_t[]){ 0444444 };
    check("HSC-22 post-drain HSCexecute on same channel does not hang", HSCexecute(chanP, &req) == HSC_BUSY);

    steal = processHSCchannels();
    check("HSC-23 post-drain steal completes normally", steal == 1);
    check("HSC-24 free chan2 (second time)", HSCfreeChannel(chanP));

    return(failCount != 0);
}

int
main(void)
{
    memset(&thePdp1, 0, sizeof(thePdp1));
    pdp1P = &thePdp1;

    testPriorityArbitration();
    testThreadedDrainNoDeadlock();

    printf("\n%d check%s failed\n", failCount, (failCount == 1) ? "" : "s");
    return( failCount != 0 );
}
