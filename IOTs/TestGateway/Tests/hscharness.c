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
 * It also covers what needs simtime, a second thread or the panel stubs, none of which an
 * am1 program can arrange: per-cycle arbitration with a TRUESTEAL channel, one word per steal,
 * words falling due across a long instruction or a throttle lag-cap firing, owed cycles for
 * THREADED and HSCsteal(), and the wake-ups from completion and from HSCreset(). pass() below
 * stands in for one main-loop pass: a scan, then 5us of simtime.
 *
 * Build with -DHSC_NO_STEAL to run the rest against an HSC that has no HSCsteal().
 *
 * 02-Jul-2026 wje/claude -- written alongside the IOT 44 Test Gateway am1 suite, to close
 *    the one gap that suite documents but can't fill itself.
 * 23-Sep-2026 claude -- THREADED status is now finished by HSCwait(), and checks HSC-25 on for
 *    the channel rework.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "pdp1.h"
#include "highSpeedChannels.h"

// Not declared in highSpeedChannels.h -- it's called only from main.c in the real emulator,
// which presumably gets its own prototype some other way (or relies on old-style implicit
// declaration). Declared explicitly here since this harness builds with -Wall -Wextra.
bool processHSCchannels(void);

// highSpeedChannels.c's own extern -- normally defined and set up by main.c.
PDP1P pdp1P;
static PDP1 thePdp1;

// Normally pdp1.c's; the HSC reads it to spot a throttle lag-cap firing.
long throttleCapFirings;

static int failCount = 0;

// Calls to the panel stubs; the HSC must make none (only main.c and cycle() touch the panel).
static int lightsCalls;
static int pwmCalls;

// A waiter thread's result: set to 1 when its HSCwait() has returned, with the status.
static atomic_int waiterReturned;
static atomic_int waiterStatus;

// Stand-in for the panel snapshot; counts calls. The panel pointer is never dereferenced.
void
updatelights(PDP1 *pdp, Panel *panel)
{
    (void)pdp;
    (void)panel;
    ++lightsCalls;
}

// Stand-in for the panel tally; counts calls.
void
updatelights_pwm(Panel *panel, int n)
{
    (void)panel;
    (void)n;
    ++pwmCalls;
}

// One main-loop pass: the channel scan, then the 5us every pass adds to simtime (main.c).
// Returns true if a channel stole the pass's cycle.
static bool
pass(void)
{
bool steal;

    steal = processHSCchannels();
    thePdp1.simtime += 5000;
    return(steal);
}

// Run passes until a normal or TRUESTEAL channel is no longer busy (at most 500), then
// HSCwait() on it, so a check that failed cannot leave the harness blocked in HSCwait().
static void
drainAndWait(HSCChannelP chanP)
{
int i;

    for( i = 0; (i < 500) && (HSCgetStatus(chanP) == HSC_BUSY); ++i )
    {
        pass();
    }
    HSCwait(chanP);
}

// Fill count words of bank 0 from addr with value.
static void
fillCore(int addr, int count, Word value)
{
int i;

    for( i = 0; i < count; ++i )
    {
        thePdp1.core[addr + i] = value;
    }
}

// Returns how many of the count words of bank 0 from addr no longer hold value.
static int
countChanged(int addr, int count, Word value)
{
int i;
int changed;

    changed = 0;
    for( i = 0; i < count; ++i )
    {
        if( thePdp1.core[addr + i] != value )
        {
            ++changed;
        }
    }

    return(changed);
}

// Thread body: wait on the channel passed in and record what HSCwait() returned.
static void *
waiterThread(void *argP)
{
    atomic_store(&waiterStatus, HSCwait((HSCChannelP)argP));
    atomic_store(&waiterReturned, 1);
    return(NULL);
}

// Poll for the waiter thread to return, up to ms milliseconds.
// Returns true if it returned in time.
static bool
waiterReturnedWithin(int ms)
{
int i;

    for( i = 0; i < ms; ++i )
    {
        if( atomic_load(&waiterReturned) )
        {
            return(true);
        }
        usleep(1000);
    }

    return( atomic_load(&waiterReturned) != 0 );
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

    // The requester finishes a THREADED transfer's status in HSCwait(); the scan only pays cycles.
    check("HSC-19 chan2 status DONE after THREADED drain",
        (HSCwait(chanP) == HSC_DONE) && (HSCgetStatus(chanP) == HSC_DONE));
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

// TRUESTEAL alone: no word moves at request time, one word reaches core per steal, exactly
// count steals, and the channel is done on its last tick. 4 words at 8.5us is 7 ticks.
static void
testTrueStealOneWordPerSteal(void)
{
HSCChannelP chanP;
HSCRequest req;
Word sentinel;
int passes, steals, badOrder;

    sentinel = 0242424;
    fillCore(3300, 4, sentinel);

    chanP = HSCallocateChannel(1);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM | HSC_MODE_TRUESTEAL;
    req.count = 4;
    req.memAddr = 3300;
    req.wordTime = 85;
    req.toBufferP = (uint32_t[]){ 0111111, 0222222, 0333333, 0444444 };
    check("HSC-25 TRUESTEAL request returns busy", HSCexecute(chanP, &req) == HSC_BUSY);
    check("HSC-26 TRUESTEAL moves no word at request time", countChanged(3300, 4, sentinel) == 0);

    passes = steals = badOrder = 0;
    while( (HSCgetStatus(chanP) == HSC_BUSY) && (passes < 50) )
    {
        if( pass() )
        {
            ++steals;
        }
        ++passes;

        if( countChanged(3300, 4, sentinel) != steals )
        {
            ++badOrder;
        }
    }

    check("HSC-27 one word reaches core per steal", badOrder == 0);
    check("HSC-28 TRUESTEAL steals exactly count cycles", steals == 4);
    check("HSC-29 TRUESTEAL done on its last tick", passes == 7);
    check("HSC-30 TRUESTEAL data correct",
        (thePdp1.core[3300] == 0111111) && (thePdp1.core[3303] == 0444444));
    drainAndWait(chanP);
    HSCfreeChannel(chanP);
}

// Per-cycle arbitration: a TRUESTEAL transfer on channel 1 (4 words over 16 ticks) leaves most
// passes free, and a normal transfer on channel 5 takes them; channel 1 still finishes on time.
static void
testArbitrationPerCycle(void)
{
HSCChannelP chan1P, chan5P;
HSCRequest req1, req5;
Word sentinel;
int passes, steals, chan5DoneFirst;

    sentinel = 0242424;
    fillCore(3400, 4, sentinel);
    fillCore(3500, 3, sentinel);

    chan1P = HSCallocateChannel(1);
    chan5P = HSCallocateChannel(5);

    memset(&req1, 0, sizeof(req1));
    req1.mode = HSC_MODE_TOMEM | HSC_MODE_TRUESTEAL;
    req1.count = 4;
    req1.memAddr = 3400;
    req1.wordTime = 200;
    req1.toBufferP = (uint32_t[]){ 0111111, 0222222, 0333333, 0444444 };

    memset(&req5, 0, sizeof(req5));
    req5.mode = HSC_MODE_TOMEM;
    req5.count = 3;
    req5.memAddr = 3500;
    req5.toBufferP = (uint32_t[]){ 0555555, 0666666, 0777777 };

    HSCexecute(chan1P, &req1);
    HSCexecute(chan5P, &req5);

    passes = steals = chan5DoneFirst = 0;
    while( (HSCgetStatus(chan1P) == HSC_BUSY) && (passes < 50) )
    {
        if( pass() )
        {
            ++steals;
        }
        ++passes;

        if( (HSCgetStatus(chan5P) == HSC_DONE) && (HSCgetStatus(chan1P) == HSC_BUSY) )
        {
            chan5DoneFirst = 1;
        }
    }

    check("HSC-31 lower channel is served while a TRUESTEAL channel is busy", chan5DoneFirst);
    check("HSC-32 both transfers complete, one steal per word",
        (steals == 7) && (countChanged(3400, 4, sentinel) == 4) && (countChanged(3500, 3, sentinel) == 3));
    check("HSC-33 TRUESTEAL still done on its last tick", passes == 16);
    drainAndWait(chan1P);
    drainAndWait(chan5P);
    HSCfreeChannel(chan1P);
    HSCfreeChannel(chan5P);
}

// Ticks come from simtime: a pass that covers 45us, as a mul or div does, makes the words of
// those ticks due at once, and they are stolen on the passes right after it. 4 words at 10us
// is 8 ticks; the long pass makes all of them due.
static void
testTrueStealAfterLongInstruction(void)
{
HSCChannelP chanP;
HSCRequest req;
int i, steals;

    fillCore(3600, 4, 0);
    chanP = HSCallocateChannel(1);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM | HSC_MODE_TRUESTEAL;
    req.count = 4;
    req.memAddr = 3600;
    req.wordTime = 100;
    req.toBufferP = (uint32_t[]){ 0111111, 0222222, 0333333, 0444444 };
    HSCexecute(chanP, &req);

    pass();                             // tick 1: no word due yet
    thePdp1.simtime += 40000;           // that pass was a 45us instruction

    steals = 0;
    for( i = 0; i < 4; ++i )
    {
        if( pass() )
        {
            ++steals;
        }
    }

    check("HSC-34 words due across a long instruction are stolen on the next passes", steals == 4);
    check("HSC-35 and the transfer is then done", HSCgetStatus(chanP) == HSC_DONE);
    drainAndWait(chanP);
    HSCfreeChannel(chanP);
}

// A throttle lag-cap firing forgives simtime; the words that fell due in it move without a steal.
static void
testTrueStealCapForgiven(void)
{
HSCChannelP chanP;
HSCRequest req;
bool steal;

    fillCore(3700, 4, 0);
    chanP = HSCallocateChannel(1);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM | HSC_MODE_TRUESTEAL;
    req.count = 4;
    req.memAddr = 3700;
    req.wordTime = 100;
    req.toBufferP = (uint32_t[]){ 0111111, 0222222, 0333333, 0444444 };
    HSCexecute(chanP, &req);

    pass();
    thePdp1.simtime += 100000;          // throttle() moved simtime over forgiven time
    ++throttleCapFirings;
    steal = pass();

    check("HSC-36 words due in forgiven time move without a steal",
        !steal && (countChanged(3700, 4, 0) == 4) && (HSCgetStatus(chanP) == HSC_DONE));
    drainAndWait(chanP);
    HSCfreeChannel(chanP);
}

// THREADED owes its cycles to the CPU: returning from HSCwait() no longer cancels them, and
// nothing is owed while the CPU is stopped. IMMEDIATE steals nothing.
static void
testOwedCycles(void)
{
HSCChannelP chanP;
HSCRequest req;
Word buf[8];
int i, steals;

    chanP = HSCallocateChannel(3);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_FROMMEM | HSC_MODE_THREADED;
    req.count = 3;
    req.memAddr = 3800;
    req.fromBufferP = buf;
    HSCexecute(chanP, &req);
    HSCwait(chanP);                     // the requester is done before the scan runs

    steals = 0;
    for( i = 0; i < 5; ++i )
    {
        if( pass() )
        {
            ++steals;
        }
    }
    check("HSC-37 THREADED cycles are stolen even after HSCwait() returned", steals == 3);

    // A request made while stopped; the scan runs before HSCwait(), so owed cycles would show.
    thePdp1.run = 0;
    req.count = 2;
    HSCexecute(chanP, &req);
    thePdp1.run = 1;
    steals = 0;
    for( i = 0; i < 3; ++i )
    {
        if( pass() )
        {
            ++steals;
        }
    }
    check("HSC-38 nothing is owed while the CPU is stopped", steals == 0);
    HSCwait(chanP);

    req.mode = HSC_MODE_FROMMEM | HSC_MODE_IMMEDIATE;
    req.count = 5;
    HSCexecute(chanP, &req);
    check("HSC-39 IMMEDIATE steals nothing", !pass());

#ifndef HSC_NO_STEAL
    check("HSC-40 HSCsteal accepts a count", HSCsteal(chanP, 2) == HSC_OK);
    steals = 0;
    for( i = 0; i < 4; ++i )
    {
        if( pass() )
        {
            ++steals;
        }
    }
    check("HSC-41 HSCsteal's cycles are stolen, one per pass", steals == 2);
    check("HSC-42 HSCsteal rejects a negative count", HSCsteal(chanP, -1) == HSC_ERR);
#endif
    HSCfreeChannel(chanP);
}

// Only the scan touches the lamp, and the HSC never calls the panel code: the requester's
// thread leaves the panel alone, the scan lights the lamp, and it goes out after its stretch.
static void
testLampOnEmulatorThreadOnly(void)
{
HSCChannelP chanP;
HSCRequest req;
Word buf[2];
int i;

    for( i = 0; i < 30; ++i )
    {
        pass();                         // let every earlier lamp request run out
    }

    lightsCalls = pwmCalls = 0;
    chanP = HSCallocateChannel(3);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_FROMMEM | HSC_MODE_THREADED | HSC_MODE_UPDATEPANEL;
    req.count = 1;
    req.memAddr = 3900;
    req.fromBufferP = buf;
    HSCexecute(chanP, &req);
    check("HSC-43 the requester does not touch the panel", (thePdp1.hsc == 0) && (pwmCalls == 0) && (lightsCalls == 0));

    pass();
    check("HSC-44 the scan lights the lamp", thePdp1.hsc == 1);
    HSCwait(chanP);

    for( i = 0; i < 25; ++i )
    {
        pass();
    }
    check("HSC-45 the lamp goes out after its stretch", thePdp1.hsc == 0);
    check("HSC-46 the HSC never calls the panel code", (pwmCalls == 0) && (lightsCalls == 0));
    HSCfreeChannel(chanP);
}

// Every TRUESTEAL transfer of 1 to 3000 words at the drum's 8.5us moves exactly its count and
// steals exactly its count. The spreading once made one word too many on about a quarter of
// these counts (7 words over 12 ticks, for one), which moved a word past the block.
static void
testTrueStealExactCount(void)
{
HSCChannelP chanP;
HSCRequest req;
Word buf[3000];
int n, i, passes, steals, overran, badBlock, badSteals;

    for( i = 0; i < 3000; ++i )
    {
        buf[i] = 0100000 + i;
    }

    chanP = HSCallocateChannel(1);
    overran = badBlock = badSteals = 0;
    for( n = 1; n <= 3000; ++n )
    {
        fillCore(4096, n + 1, 0777000);     // bank 1, clear of the other tests' words
        memset(&req, 0, sizeof(req));
        req.mode = HSC_MODE_TOMEM | HSC_MODE_TRUESTEAL;
        req.count = n;
        req.memBank = 1;
        req.wordTime = 85;
        req.toBufferP = buf;
        HSCexecute(chanP, &req);

        passes = steals = 0;
        while( (HSCgetStatus(chanP) == HSC_BUSY) && (passes < 6000) )
        {
            if( pass() )
            {
                ++steals;
            }
            ++passes;
        }
        HSCwait(chanP);

        if( thePdp1.core[4096 + n] != 0777000 )
        {
            ++overran;
        }
        for( i = 0; i < n; ++i )
        {
            if( thePdp1.core[4096 + i] != buf[i] )
            {
                ++badBlock;
                break;
            }
        }
        if( steals != n )
        {
            ++badSteals;
        }
    }

    check("HSC-50 TRUESTEAL moves exactly its count, 1 to 3000 words", (overran == 0) && (badBlock == 0));
    check("HSC-51 TRUESTEAL steals exactly its count, 1 to 3000 words", badSteals == 0);
    HSCfreeChannel(chanP);
}

// A thread blocked in HSCwait() on a normal transfer is woken by completion, and by HSCreset().
// Runs last: HSCreset() aborts every assigned channel.
static void
testWaiterWakeups(void)
{
HSCChannelP chanP;
HSCRequest req;
pthread_t tid;
int i;

    chanP = HSCallocateChannel(5);
    memset(&req, 0, sizeof(req));
    req.mode = HSC_MODE_TOMEM;
    req.count = 2;
    req.memAddr = 4000;
    req.toBufferP = (uint32_t[]){ 0123456, 0654321 };

    HSCexecute(chanP, &req);
    atomic_store(&waiterReturned, 0);
    pthread_create(&tid, NULL, waiterThread, chanP);
    pthread_detach(tid);
    usleep(20000);
    check("HSC-47 a waiter blocks while the transfer is busy", !atomic_load(&waiterReturned));
    for( i = 0; i < 2; ++i )
    {
        pass();
    }
    check("HSC-48 completion wakes the waiter with DONE",
        waiterReturnedWithin(500) && (atomic_load(&waiterStatus) == HSC_DONE));

    HSCexecute(chanP, &req);
    atomic_store(&waiterReturned, 0);
    pthread_create(&tid, NULL, waiterThread, chanP);
    pthread_detach(tid);
    usleep(20000);
    HSCreset();
    check("HSC-49 HSCreset wakes the waiter with ABORT",
        waiterReturnedWithin(500) && (atomic_load(&waiterStatus) == HSC_ABORT));
    HSCfreeChannel(chanP);
}

int
main(void)
{
    memset(&thePdp1, 0, sizeof(thePdp1));
    pdp1P = &thePdp1;
    thePdp1.run = 1;                    // the CPU is running: THREADED and HSCsteal owe cycles

    testPriorityArbitration();
    testThreadedDrainNoDeadlock();
    testTrueStealOneWordPerSteal();
    testArbitrationPerCycle();
    testTrueStealAfterLongInstruction();
    testTrueStealCapForgiven();
    testOwedCycles();
    testLampOnEmulatorThreadOnly();
    testTrueStealExactCount();
    testWaiterWakeups();

    printf("\n%d check%s failed\n", failCount, (failCount == 1) ? "" : "s");
    return( failCount != 0 );
}
