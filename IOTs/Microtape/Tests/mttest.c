/*
 * mttest.c -- host unit tests for the Type 550 Microtape control and Type 555 transport.
 *
 * Purpose: Phase 1 of Magtape/TASK-TYPE550.md. Drives control550.c and transport555.c with
 * synthetic time, as a PDP-1 program would drive the five IOTs, and checks every row of the
 * task's sections 5.2 and 5.3, the program deadlines of the research doc section 3.5 (each
 * hit and missed), the forward-write / reverse-read identity, the checksum total, a partial
 * rewrite, MISS, END, tape unable, the three delays, a tape running off its reel, image
 * file persistence, and (TASK-REWORK.md) mode 7's erase and deferred formatting.
 *
 * Architectural scope: a standalone host program. It links the two core files directly and
 * never touches the emulator, following IOTs/TestGateway/Tests/hscharness.c. Everything a test
 * observes is what a program could observe (the IOT results and the status word) plus the
 * stored image, which stands in for reading the block back.
 *
 * Dependencies: ../control550.[ch], ../transport555.[ch]; POSIX file calls for the file test.
 *
 * Execution model: single-threaded. "now" is the simulated time; at() moves it forward and
 * services the control, exactly as iotIOPoll() will. The simulated program answers each flag
 * RESPOND_NS after it is raised unless a test says otherwise.
 *
 * Usage: mttest [-v]. Prints one PASS/FAIL line per test group, every failed check, and a
 * summary; -v also prints the measured deadline windows. Exit status 0 if everything passed.
 * The file test creates and removes mttest-*.img in the current directory.
 *
 * 10-Sep-2026 Claude -- initial version, for the Type 550 task (Magtape/TASK-TYPE550.md).
 * 11-Sep-2026 Claude -- mode 7 and deferred formatting (Magtape/TASK-REWORK.md).
 * 13-Sep-2026 Claude -- DEC's block mark and deadlines, write enable, the D256 latch and All
 *                       Halt (MiscTasks/Completed/TASK-TAPE-HALT-WRITE.md).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "control550.h"

#define US              1000ULL
#define MS              1000000ULL
#define T_BASE          (1000ULL * MS)      // simulated time starts at 1 s, well clear of 0
#define RESPOND_NS      (20 * US)           // how long the simulated program takes per flag
#define MAX_FLAGS       600                 // flags recorded per helper call

// LOAD CONTROL words.
#define GO              MT_CTL_GO
#define REV             MT_CTL_REV
#define MOVE            MT_MODE_MOVE
#define SEARCH          MT_MODE_SEARCH
#define READ            MT_MODE_READ
#define WRITE           MT_MODE_WRITE

// Globals: the control under test, the clock, and the tallies.
static Mt550 ctl;
static bool ctlReady;
static uint64_t now;
static uint64_t flagTime;                   // when waitFlag() last saw a flag come up
static bool verbose;

static const char *testNameP;
static int checkCount;
static int failCount;

// What the last read/write helper saw: the time and kind ('D' data, 'B' block end) of each
// flag, and the words read.
static int flagCount;
static uint64_t flagTimes[MAX_FLAGS];
static char flagKinds[MAX_FLAGS];

static uint32_t imageA[MT_IMAGE_WORDS];     // the tape image the tests mount
static uint32_t dataW[MT_DATA_WORDS];       // a block of data to write
static uint32_t dataV[MT_DATA_WORDS];       // a second, different block of data
static uint32_t words[MT_STORED_WORDS + 8]; // words read back

// ---- Reporting --------------------------------------------------------------------------------

// Counts one check; on failure prints the test name and the printf-style message.
// No return value.
static void
check(bool ok, const char *fmtP, ...)
{
va_list args;

    ++checkCount;
    if( ok )
    {
        return;
    }

    ++failCount;
    printf("    FAIL [%s] ", testNameP);
    va_start(args, fmtP);
    vprintf(fmtP, args);
    va_end(args);
    printf("\n");
}

// Prints an informational line when -v was given. No return value.
static void
note(const char *fmtP, ...)
{
va_list args;

    if( !verbose )
    {
        return;
    }

    printf("    note [%s] ", testNameP);
    va_start(args, fmtP);
    vprintf(fmtP, args);
    va_end(args);
    printf("\n");
}

// Runs one test group and prints PASS or FAIL for it. No return value.
static void
runTest(void (*fnP)(void), const char *nameP)
{
int before;

    testNameP = nameP;
    before = failCount;
    fnP();
    printf("%s  %s\n", ((failCount == before) ? "PASS" : "FAIL"), nameP);
}

// ---- Clock and IOTs ---------------------------------------------------------------------------

// Takes every tape off, returns the control to its power-on state and restarts the clock.
// No return value.
static void
resetAll(void)
{
int unit;

    if( ctlReady )
    {
        for( unit = 1; unit <= MT_UNITS; ++unit )
        {
            mt555Unmount(&ctl.units[unit]);
        }
    }

    mt550Init(&ctl, NULL, NULL);
    ctlReady = true;
    now = T_BASE;
    mt550Service(&ctl, now);
}

// Moves the clock to time t and services the control, as the main loop's poll does.
// No return value.
static void
at(uint64_t t)
{
    now = t;
    mt550Service(&ctl, now);
}

// Moves the clock forward by ns. No return value.
static void
waitNs(uint64_t ns)
{
    at(now + ns);
}

// Returns true if the data, block end or error flag is up.
static bool
anyFlag(void)
{
    return( ctl.df || ctl.bef || ctl.erf );
}

// Advances the clock event by event until a flag is up or limitNs has passed.
// Returns true with now = flagTime = the time the flag came up, or false with the clock at
// the limit. A flag already up returns at once.
static bool
waitFlag(uint64_t limitNs)
{
uint64_t limit;
uint64_t t;

    limit = (now + limitNs);
    while( !anyFlag() )
    {
        t = ctl.nextEvent;
        if( t > limit )
        {
            at(limit);
            return(false);
        }

        at(t);
    }

    flagTime = now;
    return(true);
}

// As waitFlag(), but waits for the data or block end flag only: a program that has seen
// MISS and carries on keeps servicing data flags while the error flag stays up.
// Returns true with now = flagTime = the time the flag came up, false at the limit.
static bool
waitDataFlag(uint64_t limitNs)
{
uint64_t limit;
uint64_t t;

    limit = (now + limitNs);
    while( !(ctl.df || ctl.bef) )
    {
        t = ctl.nextEvent;
        if( t > limit )
        {
            at(limit);
            return(false);
        }

        at(t);
    }

    flagTime = now;
    return(true);
}

// The five IOTs, at the current time.
static void
mse(int unit)
{
    mt550Select(&ctl, now, ((uint32_t)unit << MT_SEL_SHIFT));
}

static void
mlc(uint32_t bits)
{
    mt550LoadControl(&ctl, now, bits);
}

// Returns the buffer, as mrd leaves it in IO.
static uint32_t
mrd(void)
{
    return( mt550ReadBuffer(&ctl, now) );
}

static void
mwr(uint32_t word)
{
    mt550WriteBuffer(&ctl, now, word);
}

// Returns the status word, as mrs leaves it in IO.
static uint32_t
mrs(void)
{
    return( mt550Status(&ctl, now) );
}

// ---- Images -----------------------------------------------------------------------------------

// Returns a pointer to the 258 stored words of block in image imgP.
static uint32_t *
blockP(uint32_t *imgP, int block)
{
    return( &imgP[block * MT_STORED_WORDS] );
}

// Fills imgP with a blank tape: leading checksum -0, data 0, trailing checksum 0 in every
// block. No return value.
static void
blankImage(uint32_t *imgP)
{
int block;

    memset(imgP, 0, (sizeof(uint32_t) * MT_IMAGE_WORDS));
    for( block = 0; block < MT_BLOCKS; ++block )
    {
        blockP(imgP, block)[0] = MT_MINUS_ZERO;
    }
}

// Returns the trailing checksum a program writes for 256 data words: the complement of the
// ring sum of the automatic -0 and the data, so that the whole block totals -0.
static uint32_t
checksumFor(const uint32_t *dataP)
{
uint32_t sum;
int i;

    sum = MT_MINUS_ZERO;
    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        sum = mt555RingAdd(sum, dataP[i]);
    }

    return( (~sum) & MT_WORDMASK );
}

// Fills dataP with 256 recognizable words that differ for each seed. No return value.
static void
makeData(uint32_t *dataP, int seed)
{
int i;

    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        dataP[i] = ((((uint32_t)seed << 12) ^ ((uint32_t)i * 02531u) ^ 0123456u) & MT_WORDMASK);
    }
}

// Stores a correctly checksummed block with data from seed into imgP. No return value.
static void
fillBlock(uint32_t *imgP, int block, int seed)
{
uint32_t data[MT_DATA_WORDS];
uint32_t *bP;

    makeData(data, seed);
    bP = blockP(imgP, block);
    bP[0] = MT_MINUS_ZERO;
    memcpy(&bP[1], data, sizeof(data));
    bP[MT_STORED_WORDS - 1] = checksumFor(data);
}

// Stores junk in all 258 words of a block, so any word the control writes is visible.
// No return value.
static void
junkBlock(uint32_t *imgP, int block)
{
uint32_t *bP;
int k;

    bP = blockP(imgP, block);
    for( k = 0; k < MT_STORED_WORDS; ++k )
    {
        bP[k] = ((0700000u | ((uint32_t)block << 9) | (uint32_t)k) & MT_WORDMASK);
    }
}

// Returns stored word k of block on unit.
static uint32_t
tapeWord(int unit, int block, int k)
{
    return( mt555GetWord(&ctl.units[unit], block, k) );
}

// Returns the ring sum of all 258 stored words of block on unit; -0 means it checks.
static uint32_t
tapeBlockSum(int unit, int block)
{
uint32_t buf[MT_STORED_WORDS];
int k;

    for( k = 0; k < MT_STORED_WORDS; ++k )
    {
        buf[k] = tapeWord(unit, block, k);
    }

    return( mt555BlockSum(buf) );
}

// Mounts a copy of imgP on unit and tells the control. No return value.
static void
mountImage(int unit, const uint32_t *imgP, bool locked)
{
    mt555MountMemory(&ctl.units[unit], imgP, locked);
    mt550UnitRemounted(&ctl, unit, now);
}

// ---- Positioning ------------------------------------------------------------------------------

// Returns the absolute slot index of physical slot slot of block.
static int64_t
absSlot(int block, int slot)
{
    return( ((int64_t)block * MT_SLOTS_PER_BLOCK) + slot );
}

// Test-only teleport: stops unit dead with its head at the start of absolute slot slot.
// No return value.
static void
parkSlot(int unit, int64_t slot)
{
    mt555Park(&ctl.units[unit], (MT_BA + (slot * MT_SLOT_NS)));
    mt550UnitRemounted(&ctl, unit, now);
}

// Selects unit and lets the selection delay run out. No return value.
static void
selectUnit(int unit)
{
    mse(unit);
    waitNs(MT_SELECT_DIP_NS);
}

// Puts the selected unit in search mode in direction rev (0 or REV) and takes search flags
// until the one for block. Returns true with that flag just taken by mrd at the moment it
// was raised (now == flagTime); false if an error flag came up or nothing came for 3 s.
static bool
searchTo(uint32_t rev, int block)
{
uint32_t want;

    want = ((rev ? MT_MARK_REV : MT_MARK_FWD) | (uint32_t)block);
    mlc(GO | rev | SEARCH);
    while( waitFlag(3000 * MS) )
    {
        if( ctl.erf )
        {
            return(false);
        }

        if( mrd() == want )
        {
            return(true);
        }
    }

    return(false);
}

// Parks the selected unit three blocks before block (in the direction of motion) and
// searches to it, so the block's search flag comes soon after the tape reaches speed.
// Returns what searchTo() returns.
static bool
gotoBlock(uint32_t rev, int block)
{
    parkSlot(ctl.selUnit, absSlot((rev ? (block + 3) : (block - 3)), 0));
    return( searchTo(rev, block) );
}

// ---- Reading and writing ----------------------------------------------------------------------

// Records the flag that just came up in flagTimes/flagKinds. No return value.
static void
recordFlag(void)
{
    if( flagCount < MAX_FLAGS )
    {
        flagTimes[flagCount] = flagTime;
        flagKinds[flagCount] = (ctl.bef ? 'B' : 'D');
    }

    ++flagCount;
}

// Takes read flags, answering each data flag with mrd after respondNs, until the block end
// flag comes up, which is left unanswered. The words go to words[]. Does not change mode.
// Returns the number of data words taken, or -1 if an error came first or a flag did not come.
static int
readUntilBef(uint64_t respondNs)
{
int n;

    n = 0;
    while( waitFlag(100 * MS) )
    {
        recordFlag();
        if( ctl.erf )
        {
            return(-1);
        }

        if( ctl.bef )
        {
            return(n);
        }

        waitNs(respondNs);
        words[n] = mrd();
        n = ((n < MT_STORED_WORDS) ? (n + 1) : n);
    }

    return(-1);
}

// Switches the selected unit (just past a block's search mark) to read in direction rev and
// reads the whole block into words[], answering every flag after respondNs.
// Returns the number of words read (258 for a clean block), or -1 on an error or a timeout.
static int
readBlock(uint32_t rev, uint64_t respondNs)
{
int n;

    flagCount = 0;
    mlc(GO | rev | READ);
    if( (n = readUntilBef(respondNs)) < 0 )
    {
        return(-1);
    }

    waitNs(respondNs);
    words[n] = mrd();
    return( n + 1 );
}

// Answers write flags with the words of dataP (the last word repeats if more are asked for)
// after respondNs each, until the block end flag comes up, which is left unanswered. Does
// not change mode. Returns the number of data flags answered, or -1 if an error came first
// or a flag did not come.
static int
feedUntilBef(const uint32_t *dataP, uint64_t respondNs)
{
int n;

    n = 0;
    while( waitFlag(100 * MS) )
    {
        recordFlag();
        if( ctl.erf )
        {
            return(-1);
        }

        if( ctl.bef )
        {
            return(n);
        }

        waitNs(respondNs);
        mwr(dataP[((n < MT_DATA_WORDS) ? n : (MT_DATA_WORDS - 1))]);
        ++n;
    }

    return(-1);
}

// Switches the selected unit (just past a block's search mark) to write in direction rev,
// writes dataP and then the checksum cs on the block end flag, lets the checksum reach the
// tape, and goes back to search in the same direction.
// Returns the number of data flags seen (256 for a clean block), or -1 on an error or timeout.
static int
writeBlock(uint32_t rev, const uint32_t *dataP, uint32_t cs, uint64_t respondNs)
{
int n;
uint64_t bef;

    flagCount = 0;
    mlc(GO | rev | WRITE);
    if( (n = feedUntilBef(dataP, respondNs)) < 0 )
    {
        return(-1);
    }

    bef = flagTime;
    waitNs(respondNs);
    mwr(cs);
    at(bef + (500 * US));           // past the checksum slot: the writers are off
    mlc(GO | rev | SEARCH);
    return(n);
}

// ---- 5.2: the IOTs ----------------------------------------------------------------------------

// Every row of section 5.2 that does not need a moving tape, plus the status bits, the unit
// number decoding, the flag clearing rules, modes 4-6 (move) and mode 7 (erase).
static void
testIots(void)
{
unsigned long breaks;
int mode;

    resetAll();
    blankImage(imageA);

    check((mrs() == 0), "power-on status %06o, want 0", mrs());

    // mlc with nothing selected is refused.
    mlc(GO | SEARCH);
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "mlc, no unit: status %06o, want 101000", mrs());
    check((ctl.breakCount == 1), "mlc, no unit: %lu break requests, want 1", ctl.breakCount);

    // mse clears ERF and UNABLE; a unit with no tape refuses mlc too.
    mse(1);
    check((mrs() == 0), "mse clears UNABLE and ERF: status %06o", mrs());
    mlc(GO | READ);
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "mlc, no tape: status %06o, want 101000", mrs());
    mlc(MOVE);
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "mlc stop, no tape: status %06o, want 101000", mrs());

    // Unit numbers from IO bits 2-5: 1-7, 010 = unit 8, 0 and 011-017 select nothing.
    mse(010);
    check((ctl.selUnit == 8), "mse 010 selects unit %d, want 8", ctl.selUnit);
    mse(7);
    check((ctl.selUnit == 7), "mse 7 selects unit %d, want 7", ctl.selUnit);
    mse(011);
    check((ctl.selUnit == 0), "mse 011 selects unit %d, want none", ctl.selUnit);
    mse(017);
    check((ctl.selUnit == 0), "mse 017 selects unit %d, want none", ctl.selUnit);
    mse(0);
    check((ctl.selUnit == 0), "mse 0 selects unit %d, want none", ctl.selUnit);
    mt550Select(&ctl, now, (0600000u | 0010000u | 0007777u));
    check((ctl.selUnit == 1), "mse ignores IO bits 0-1 and 6-17: unit %d, want 1", ctl.selUnit);

    // mwr / mrd move the buffer and clear only DF and BEF; ERF stays until mse or mlc.
    mse(3);
    mlc(GO | SEARCH);                   // no tape on unit 3: ERF + UNABLE
    mwr(01234567);
    check((mrd() == 0234567), "mwr then mrd returns the 18-bit buffer");
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "mrd/mwr leave ERF and UNABLE: status %06o", mrs());
    mse(3);
    check((mrs() == 0), "mse clears them: status %06o", mrs());

    // Modes 4-6 act as move: the tape runs, no flags, and the tape is not touched.
    fillBlock(imageA, 100, 100);
    mountImage(1, imageA, false);
    selectUnit(1);
    parkSlot(1, absSlot(100, 0));
    breaks = ctl.breakCount;
    for( mode = 4; mode <= 6; ++mode )
    {
        mlc(GO | (uint32_t)mode);
        check(!waitFlag(400 * MS), "mode %d raised a flag (status %06o)", mode, mrs());
        check(((mrs() & MT_ST_GO) != 0), "mode %d: GO not set", mode);
    }
    check((ctl.breakCount == breaks), "modes 4-6 requested %lu breaks", (ctl.breakCount - breaks));
    check((tapeWord(1, 100, 1) == blockP(imageA, 100)[1]), "modes 4-6 changed the tape");

    // Mode 7 (TASK-REWORK.md): the reel is erased to a blank tape at once, then it acts as move.
    mlc(GO | MT_MODE_ERASE);
    check(((tapeWord(1, 100, 0) == MT_MINUS_ZERO) && (tapeWord(1, 100, 1) == 0) && (tapeWord(1, 100, 257) == 0)),
        "mode 7: block 100 not blank");
    check((tapeBlockSum(1, 100) == MT_MINUS_ZERO), "mode 7: the erased block does not check");
    check(!waitFlag(400 * MS), "mode 7 raised a flag (status %06o)", mrs());
    check(((mrs() & MT_ST_GO) && !(mrs() & MT_ST_UNABLE)), "mode 7 with go: status %06o, want GO only", mrs());
    check((ctl.breakCount == breaks), "mode 7 requested %lu breaks", (ctl.breakCount - breaks));

    // A locked unit refuses mode 7 as it refuses write, and its tape is left alone; its
    // control leg is the unlocked unit above.
    mountImage(2, imageA, true);
    selectUnit(2);
    mlc(GO | MT_MODE_ERASE);
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "mode 7, locked unit: status %06o, want 101000", mrs());
    check((tapeWord(2, 100, 1) == blockP(imageA, 100)[1]), "mode 7 erased a locked tape");
    check((ctl.units[2].motion == MT_STOPPED), "mode 7, locked unit: the tape started");
    selectUnit(1);

    // REV and GO are the latched motion commands.
    mlc(GO | REV | MOVE);
    check((mrs() == (MT_ST_REV | MT_ST_GO)), "mlc go rev: status %06o, want 014000", mrs());
    mlc(MOVE);
    check((mrs() == 0), "mlc stop: status %06o, want 0", mrs());
    check((ctl.units[1].motion == MT_DECEL), "mlc stop: the tape is not slowing");
    check((ctl.mte == false), "MTE is never set");
}

// ---- 5.3 search, and the three delays ---------------------------------------------------------

// Search flags and their timing from the load position; the start, turnaround and selection
// delays; and the stop ramp.
static void
testSearchAndDelays(void)
{
uint64_t t0;
uint64_t t1;
uint64_t t2;
uint64_t want;
int64_t pos;
unsigned long breaks;

    resetAll();
    blankImage(imageA);
    mountImage(1, imageA, false);
    selectUnit(1);

    // From the load position (start of the reverse end zone): 200 ms start covering half its
    // time at speed, then the rest of the end zone, then the end of block 0's space 0, the
    // block mark.
    t0 = now;
    breaks = ctl.breakCount;
    mlc(GO | SEARCH);
    check(waitFlag(3000 * MS), "no search flag from the load position");
    want = (t0 + (uint64_t)MT_START_NS + (uint64_t)(MT_BA - (MT_LOAD_POS + (MT_START_NS / 2)))
        + (uint64_t)MT_SLOT_NS);
    check((flagTime == want), "first search flag at +%llu ns, want +%llu",
        (unsigned long long)(flagTime - t0), (unsigned long long)(want - t0));
    check((ctl.df && !ctl.bef && !ctl.erf), "search raises DF only");
    check((mrd() == (MT_MARK_FWD | 0)), "block 0 search word");
    check((ctl.breakCount == (breaks + 1)), "one break request per flag");
    check(waitFlag(100 * MS), "no second search flag");
    check((flagTime == (want + (uint64_t)MT_BLOCK_NS)), "block 1 not one block period (52.8 ms) later");
    check((mrd() == (MT_MARK_FWD | 1)), "block 1 search word");

    // Start delay: from rest inside the block area, the marks passed during the 200 ms ramp
    // raise nothing. Parked at block 10 slot 0, the ramp ends 500 slots on (block 11 slot
    // 236); block 11's mark went by during it, and block 12's (its space 0) ends 29 slots after.
    parkSlot(1, absSlot(10, 0));
    t0 = now;
    mlc(GO | SEARCH);
    check(waitFlag(1000 * MS), "no search flag after a start in the block area");
    check((flagTime == (t0 + (uint64_t)MT_START_NS + (29 * (uint64_t)MT_SLOT_NS))),
        "start delay: first flag at +%llu ns, want +205800000", (unsigned long long)(flagTime - t0));
    check((mrd() == (MT_MARK_FWD | 12)), "start delay: first flag is not block 12 (block 11 passed during the ramp)");

    // Turnaround delay: reversing at speed stops in 150 ms (375 slots) and comes back to speed
    // 150 ms later exactly where it began, the end of block 12's space 0. Moving in reverse the
    // head then passes that space and block 11's space 263, the reverse block mark: 2 slots.
    t1 = now;
    mlc(GO | REV | SEARCH);
    check(waitFlag(1000 * MS), "no search flag after a turnaround");
    check((flagTime == (t1 + (uint64_t)MT_STOP_NS + (uint64_t)MT_TURN_ACCEL_NS + (2 * (uint64_t)MT_SLOT_NS))),
        "turnaround: first flag at +%llu ns, want +300400000", (unsigned long long)(flagTime - t1));
    check((mrd() == (MT_MARK_REV | 11)), "turnaround: reverse search word for block 11");

    // Selection delay, control leg: reselecting 10 ms after a mark, the delay is over before
    // the next mark (52.8 ms), which is raised.
    t2 = now;
    at(t2 + (10 * MS));
    mse(2);
    mse(1);
    check(waitFlag(100 * MS), "selection delay control: no flag");
    check((flagTime == (t2 + (uint64_t)MT_BLOCK_NS)), "selection delay control: flag at +%llu ns",
        (unsigned long long)(flagTime - t2));
    check((mrd() == (MT_MARK_REV | 10)), "selection delay control: block 10");

    // Selection delay: reselecting 30 ms after a mark, the next mark falls inside the 34 ms
    // and is lost; the one after is raised.
    t2 = now;
    at(t2 + (30 * MS));
    mse(2);
    mse(1);
    check(waitFlag(200 * MS), "selection delay: no flag");
    check((flagTime == (t2 + (2 * (uint64_t)MT_BLOCK_NS))), "selection delay: flag at +%llu ns, want +105600000",
        (unsigned long long)(flagTime - t2));
    check((mrd() == (MT_MARK_REV | 8)), "selection delay: block 9 was not lost");

    // Selecting the unit that is already selected starts no delay.
    t2 = now;
    at(t2 + (30 * MS));
    mse(1);
    check(waitFlag(100 * MS), "reselecting the same unit: no flag");
    check((flagTime == (t2 + (uint64_t)MT_BLOCK_NS)), "reselecting the same unit started a delay");
    check((mrd() == (MT_MARK_REV | 7)), "reselecting the same unit: block 7");

    // Stop ramp: 150 ms, covering 375 slots.
    t2 = now;
    pos = mt555Position(&ctl.units[1], t2);
    mlc(REV | SEARCH);
    check(!waitFlag(400 * MS), "a stopping tape raised a flag");
    check((ctl.units[1].motion == MT_STOPPED), "not stopped 150 ms after go was cleared");
    check((mt555Position(&ctl.units[1], now) == (pos - (MT_STOP_NS / 2))), "stop ramp distance is not 375 slots");
    check(((mrs() & (MT_ST_GO | MT_ST_REV)) == MT_ST_REV), "stopped: status %06o, want REV only", mrs());
}

// ---- 5.3 read ---------------------------------------------------------------------------------

// Reads a checksummed block forward and in reverse: every flag, its timing, the words (as
// written, last first in reverse), and the checksum total.
static void
testRead(void)
{
uint32_t *bP;
uint64_t ts;
unsigned long breaks;
int n;
int i;
bool timingOk;
bool wordsOk;
uint32_t sum;

    resetAll();
    blankImage(imageA);
    fillBlock(imageA, 5, 1);
    bP = blockP(imageA, 5);
    mountImage(1, imageA, false);
    selectUnit(1);

    // Forward: DF at the end of slot 3 (the leading checksum), 600 us after the block mark's
    // search flag, 256 data DFs 200 us apart, BEF at the end of slot 260 (the trailing checksum).
    check(gotoBlock(0, 5), "forward search to block 5 failed");
    ts = flagTime;
    breaks = ctl.breakCount;
    n = readBlock(0, RESPOND_NS);
    check((n == MT_STORED_WORDS), "forward read took %d words, want 258", n);
    check((ctl.breakCount == (breaks + MT_STORED_WORDS)), "forward read: %lu break requests, want 258",
        (ctl.breakCount - breaks));
    timingOk = true;
    wordsOk = true;
    for( i = 0; i < MT_STORED_WORDS; ++i )
    {
        timingOk = (timingOk && (flagTimes[i] == (ts + (600 * US) + ((uint64_t)i * 200 * US))));
        timingOk = (timingOk && (flagKinds[i] == ((i == (MT_STORED_WORDS - 1)) ? 'B' : 'D')));
        wordsOk = (wordsOk && (words[i] == bP[i]));
    }
    check(timingOk, "forward read: flags not DF at the end of slots 3-259 and BEF at 260");
    check(wordsOk, "forward read: words differ from the block");
    check((mt555BlockSum(words) == MT_MINUS_ZERO), "forward read: block does not total -0");
    check(!ctl.miss, "forward read raised MISS");

    // Reverse: the same block, every word as written, last word first (decision D3).
    check(gotoBlock(REV, 5), "reverse search to block 5 failed");
    ts = flagTime;
    n = readBlock(REV, RESPOND_NS);
    check((n == MT_STORED_WORDS), "reverse read took %d words, want 258", n);
    timingOk = true;
    wordsOk = true;
    sum = 0;
    for( i = 0; i < MT_STORED_WORDS; ++i )
    {
        timingOk = (timingOk && (flagTimes[i] == (ts + (600 * US) + ((uint64_t)i * 200 * US))));
        wordsOk = (wordsOk && (words[i] == bP[(MT_STORED_WORDS - 1) - i]));
        sum = mt555RingAdd(sum, words[i]);
    }
    check(timingOk, "reverse read: flag timing differs from forward");
    check(wordsOk, "reverse read: words are not the block last-first");
    check((flagKinds[MT_STORED_WORDS - 1] == 'B'), "reverse read: last flag is not BEF");
    check((sum == MT_MINUS_ZERO), "reverse read: block does not total -0");
}

// ---- 5.3 write --------------------------------------------------------------------------------

// Writes a block forward and one in reverse; checks the flags, the stored words, the checksum
// total, the forward-write / reverse-read identity, and that a partial rewrite leaves the
// block failing its checksum.
static void
testWrite(void)
{
uint32_t cs;
uint32_t cs2;
uint64_t ts;
unsigned long flushes;
int n;
int i;
bool ok;

    resetAll();
    blankImage(imageA);
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 7);
    cs = checksumFor(dataW);

    // Forward write of block 7. Entering write raises DF at once (the request for word 1);
    // the second DF comes at the end of slot 3 (600 us after the search flag, which ends slot
    // 0) as word 1 goes to slot 4, then every 200 us; BEF at the end of slot 258 (PREFINAL)
    // asks for the checksum.
    check(gotoBlock(0, 7), "forward search to block 7 failed");
    ts = flagTime;
    flushes = ctl.units[1].flushCount;
    n = writeBlock(0, dataW, cs, RESPOND_NS);
    check((n == MT_DATA_WORDS), "forward write: %d data flags, want 256", n);
    check((flagTimes[0] == ts), "forward write: entering write did not raise DF at once");
    ok = true;
    for( i = 1; i < MT_DATA_WORDS; ++i )
    {
        ok = (ok && (flagTimes[i] == (ts + (600 * US) + ((uint64_t)(i - 1) * 200 * US))) && (flagKinds[i] == 'D'));
    }
    check(ok, "forward write: data flags not at the ends of slots 3-257");
    check(((flagKinds[MT_DATA_WORDS] == 'B') && (flagTimes[MT_DATA_WORDS] == (ts + (258 * 200 * US)))),
        "forward write: BEF not at the end of slot 258");
    check((tapeWord(1, 7, 0) == MT_MINUS_ZERO), "forward write: leading checksum %06o, want -0", tapeWord(1, 7, 0));
    ok = true;
    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        ok = (ok && (tapeWord(1, 7, (i + 1)) == dataW[i]));
    }
    check(ok, "forward write: data not stored in slots 4-259 in order");
    check((tapeWord(1, 7, 257) == cs), "forward write: trailing checksum %06o, want %06o", tapeWord(1, 7, 257), cs);
    check((tapeBlockSum(1, 7) == MT_MINUS_ZERO), "forward write: block does not total -0");
    check(((tapeBlockSum(1, 6) == MT_MINUS_ZERO) && (tapeBlockSum(1, 8) == MT_MINUS_ZERO)
        && (tapeWord(1, 8, 1) == 0) && (tapeWord(1, 6, 256) == 0)), "forward write disturbed blocks 6 or 8");
    check((ctl.units[1].flushCount == (flushes + 1)), "forward write: %lu flushes, want 1",
        (ctl.units[1].flushCount - flushes));

    // Read it back in reverse: the words exactly as written, last first.
    check(gotoBlock(REV, 7), "reverse search to block 7 failed");
    n = readBlock(REV, RESPOND_NS);
    ok = (n == MT_STORED_WORDS) && (words[0] == cs) && (words[MT_STORED_WORDS - 1] == MT_MINUS_ZERO);
    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        ok = (ok && (words[1 + i] == dataW[(MT_DATA_WORDS - 1) - i]));
    }
    check(ok, "forward write / reverse read: words are not the written ones last-first");

    // Reverse write of block 9: -0 goes to the checksum slot met first (physical 260), word 1
    // to physical 259, and the program's checksum to physical 3. Read forward it checks.
    makeData(dataV, 9);
    cs2 = checksumFor(dataV);
    check(gotoBlock(REV, 9), "reverse search to block 9 failed");
    n = writeBlock(REV, dataV, cs2, RESPOND_NS);
    check((n == MT_DATA_WORDS), "reverse write: %d data flags, want 256", n);
    ok = (tapeWord(1, 9, 257) == MT_MINUS_ZERO) && (tapeWord(1, 9, 0) == cs2);
    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        ok = (ok && (tapeWord(1, 9, (256 - i)) == dataV[i]));
    }
    check(ok, "reverse write: words not stored in mirror order");
    check((tapeBlockSum(1, 9) == MT_MINUS_ZERO), "reverse write: block does not total -0");
    check(gotoBlock(0, 9), "forward search to block 9 failed");
    n = readBlock(0, RESPOND_NS);
    check(((n == MT_STORED_WORDS) && (mt555BlockSum(words) == MT_MINUS_ZERO) && (words[0] == cs2)
        && (words[1] == dataV[255]) && (words[256] == dataV[0])), "reverse write / forward read mismatch");

    // Partial rewrite: new words for the first 99 data slots, then back to search. The old
    // checksum no longer matches.
    makeData(dataV, 77);
    check(gotoBlock(0, 7), "forward search to block 7 (rewrite) failed");
    flushes = ctl.units[1].flushCount;
    mlc(GO | WRITE);
    for( n = 0; (n < 100) && waitFlag(10 * MS); ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataV[n]);
    }
    waitNs(RESPOND_NS);
    mlc(GO | SEARCH);
    ok = true;
    for( i = 0; i < 99; ++i )
    {
        ok = (ok && (tapeWord(1, 7, (i + 1)) == dataV[i]));
    }
    check(ok, "partial rewrite: slots 4-102 do not hold the new words");
    check((tapeWord(1, 7, 100) == dataW[99]), "partial rewrite: word 100 was written (it was never interchanged)");
    check((tapeWord(1, 7, 257) == cs), "partial rewrite: checksum slot changed");
    check((tapeBlockSum(1, 7) != MT_MINUS_ZERO), "partial rewrite: block still totals -0");
    check((ctl.units[1].flushCount == (flushes + 1)), "partial rewrite: leaving write did not flush the block");
}

// ---- 3.5 deadlines ----------------------------------------------------------------------------

// Reads block (forward) to its block end flag, answering the data flags. Returns true with
// now = flagTime = the BEF time.
static bool
readToBef(int block)
{
    if( !gotoBlock(0, block) )
    {
        return(false);
    }

    flagCount = 0;
    mlc(GO | READ);
    return( readUntilBef(RESPOND_NS) == MT_DATA_WORDS + 1 );
}

// Writes dataW (forward) into block up to its block end flag. Returns true with now =
// flagTime = the BEF time.
static bool
writeToBef(int block)
{
    if( !gotoBlock(0, block) )
    {
        return(false);
    }

    flagCount = 0;
    mlc(GO | WRITE);
    return( feedUntilBef(dataW, RESPOND_NS) == MT_DATA_WORDS );
}

// The block-end deadlines: read BEF -> next DF 1.4 ms, write BEF -> next DF 1.6 ms,
// read BEF -> search 800 us, write BEF -> search 1.2 ms, read BEF -> write 1.2 ms (DEC's
// figures, DECUS 1963 p. B-2 and brochure F-03 p. 10). Each is hit and missed.
static void
testBlockEndDeadlines(void)
{
static const uint64_t readSearch[3] = { 700 * US, 799 * US, 801 * US };
static const uint64_t writeSearch[3] = { 1100 * US, 1199 * US, 1201 * US };
uint64_t tb;
uint32_t word;
int i;
int n;
bool hit;

    resetAll();
    blankImage(imageA);
    junkBlock(imageA, 32);
    junkBlock(imageA, 33);
    junkBlock(imageA, 51);
    junkBlock(imageA, 55);
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 3);

    // Read BEF: the next block's first DF (its leading checksum) comes 1.4 ms later.
    check(readToBef(20), "read to block 20's BEF failed");
    tb = flagTime;
    at(tb + (1300 * US));
    mrd();
    check((waitFlag(10 * MS) && (flagTime == (tb + (1400 * US)))), "read: next DF not 1.4 ms after BEF");
    check(!ctl.miss, "read: BEF answered at 1.3 ms still raised MISS");
    check(readToBef(20), "read to block 20's BEF failed (miss leg)");
    tb = flagTime;
    at((tb + (1400 * US)) - 1);
    check(!ctl.miss, "read: MISS before the next DF was due");
    at(tb + (1400 * US));
    check((ctl.miss && ctl.erf && ctl.df), "read: BEF unanswered at 1.4 ms did not raise MISS");
    check((mrd() == MT_MINUS_ZERO), "read MISS: the buffer was not overwritten by the next word");
    check((ctl.units[1].motion == MT_CRUISE), "read MISS stopped the tape");

    // Write BEF: the next block's first DF comes 1.6 ms later.
    check(writeToBef(30), "write to block 30's BEF failed");
    tb = flagTime;
    waitNs(RESPOND_NS);
    mwr(checksumFor(dataW));
    check((waitFlag(10 * MS) && (flagTime == (tb + (1600 * US)))), "write: next DF not 1.6 ms after BEF");
    check(!ctl.miss, "write: BEF answered in time still raised MISS");
    mlc(GO | SEARCH);
    check((tapeBlockSum(1, 30) == MT_MINUS_ZERO), "write: block 30 does not check");
    // Write BEF unanswered: the checksum is due at the interchange 200 us later. The flag is
    // still up there, so MISS, and the writers go off before the stale buffer reaches the
    // tape: the checksum space keeps what the tape held. Flags keep coming, and the next
    // block's leading -0 is not written either.
    check(writeToBef(32), "write to block 32's BEF failed (miss leg)");
    tb = flagTime;
    at((tb + (200 * US)) - 1);
    check(!ctl.miss, "write: MISS before the checksum was due");
    at(tb + (200 * US));
    check((ctl.miss && ctl.erf && !ctl.wren), "write: BEF unanswered at 200 us did not raise MISS and stop writing");
    check((tapeWord(1, 32, 257) == blockP(imageA, 32)[257]), "write MISS: the checksum space was written (%06o)",
        tapeWord(1, 32, 257));
    check(((tapeWord(1, 32, 256) == dataW[255]) && (tapeBlockSum(1, 32) != MT_MINUS_ZERO)),
        "write MISS: the last data word missing, or the block still checks");
    at((tb + (1600 * US)) - 1);
    check(!ctl.df, "write MISS: a DF before the next block's lock");
    at(tb + (1600 * US));
    check(ctl.df, "write MISS: the next DF did not come at 1.6 ms");
    check((tapeWord(1, 33, 0) == blockP(imageA, 33)[0]), "write MISS: the next block's leading checksum was written");
    check(((mrs() & MT_ST_GO) && (ctl.units[1].motion == MT_CRUISE)), "write MISS stopped the tape");
    mlc(GO | SEARCH);

    // Read BEF -> search, DEC's 800 us: the next mark (the end of its space 0) is caught if
    // search is set before it, 800 us after BEF.
    for( i = 0; i < 3; ++i )
    {
        check(readToBef(40), "read to block 40's BEF failed");
        tb = flagTime;
        at(tb + readSearch[i]);
        mlc(GO | SEARCH);
        word = (waitFlag(100 * MS) ? mrd() : 0);
        hit = (i < 2);
        check((word == (MT_MARK_FWD | (hit ? 41u : 42u))), "read BEF -> search at %llu us: got %06o, want block %d",
            (unsigned long long)(readSearch[i] / US), word, (hit ? 41 : 42));
        if( hit )
        {
            check((flagTime == (tb + (800 * US))), "read BEF -> search: block 41's flag not 800 us after BEF");
        }
    }
    note("read BEF -> search window: 800 us, as DEC gives it");

    // Write BEF -> search, DEC's 1.2 ms.
    for( i = 0; i < 3; ++i )
    {
        check(writeToBef(44), "write to block 44's BEF failed");
        tb = flagTime;
        waitNs(RESPOND_NS);
        mwr(checksumFor(dataW));
        at(tb + writeSearch[i]);
        mlc(GO | SEARCH);
        word = (waitFlag(100 * MS) ? mrd() : 0);
        hit = (i < 2);
        check((word == (MT_MARK_FWD | (hit ? 45u : 46u))), "write BEF -> search at %llu us: got %06o, want block %d",
            (unsigned long long)(writeSearch[i] / US), word, (hit ? 45 : 46));
        check((tapeBlockSum(1, 44) == MT_MINUS_ZERO), "write BEF -> search at %llu us: block 44 does not check",
            (unsigned long long)(writeSearch[i] / US));
    }
    note("write BEF -> search window: 1200 us, as DEC gives it");

    // Read BEF -> write the next block, within 1.2 ms (manual and model agree).
    check(readToBef(50), "read to block 50's BEF failed");
    tb = flagTime;
    waitNs(RESPOND_NS);
    mrd();
    at(tb + (1150 * US));
    flagCount = 0;
    mlc(GO | WRITE);
    n = feedUntilBef(dataW, RESPOND_NS);
    waitNs(RESPOND_NS);
    mwr(checksumFor(dataW));
    waitNs(500 * US);
    mlc(GO | SEARCH);
    check((n == MT_DATA_WORDS), "read BEF -> write at 1.15 ms: %d data flags", n);
    check(((tapeWord(1, 51, 0) == MT_MINUS_ZERO) && (tapeWord(1, 51, 1) == dataW[0])
        && (tapeBlockSum(1, 51) == MT_MINUS_ZERO)), "read BEF -> write at 1.15 ms: block 51 not written correctly");

    check(readToBef(54), "read to block 54's BEF failed (miss leg)");
    tb = flagTime;
    waitNs(RESPOND_NS);
    mrd();
    at(tb + (1250 * US));
    flagCount = 0;
    mlc(GO | WRITE);
    n = feedUntilBef(dataW, RESPOND_NS);
    waitNs(RESPOND_NS);
    mwr(checksumFor(dataW));
    waitNs(500 * US);
    mlc(GO | SEARCH);
    check(((tapeWord(1, 55, 0) != MT_MINUS_ZERO) && (tapeWord(1, 55, 1) == blockP(imageA, 55)[1])
        && (tapeWord(1, 55, 2) == dataW[0]) && (tapeBlockSum(1, 55) != MT_MINUS_ZERO)),
        "read BEF -> write at 1.25 ms: the block was not missed as expected (k0 %06o k1 %06o k2 %06o)",
        tapeWord(1, 55, 0), tapeWord(1, 55, 1), tapeWord(1, 55, 2));
}

// The search-flag deadlines (DECUS 1963 Figs. 4 and 5): switch to write within 400 us of a
// block's search flag, with the first mwr within 600 us; switch to read within 600 us.
static void
testSearchDeadlines(void)
{
static const uint64_t toWrite[3] = { 150 * US, 390 * US, 410 * US };
static const uint64_t toRead[3] = { 390 * US, 599 * US, 601 * US };
uint64_t ts;
uint32_t *bP;
uint32_t word;
int block;
int i;
int k;
int n;
bool ok;

    resetAll();
    blankImage(imageA);
    for( block = 60; block < 65; ++block )
    {
        junkBlock(imageA, block);
    }
    fillBlock(imageA, 70, 5);
    bP = blockP(imageA, 70);
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 11);

    // Search -> write: an mlc before the lock (the end of space 2, 400 us after the search
    // flag, which ends space 0) gets the control's -0 leading checksum. After the lock, space
    // 3 keeps what the tape held (junk here), and word 1 still goes to space 4.
    for( i = 0; i < 3; ++i )
    {
        block = (60 + i);
        check(gotoBlock(0, block), "search to block %d failed", block);
        ts = flagTime;
        at(ts + toWrite[i]);
        flagCount = 0;
        mlc(GO | WRITE);
        n = feedUntilBef(dataW, (10 * US));
        waitNs(10 * US);
        mwr(checksumFor(dataW));
        waitNs(500 * US);
        mlc(GO | SEARCH);
        if( i < 2 )
        {
            check(((n == MT_DATA_WORDS) && (tapeWord(1, block, 0) == MT_MINUS_ZERO) && (tapeWord(1, block, 1) == dataW[0])
                && (tapeBlockSum(1, block) == MT_MINUS_ZERO)),
                "search -> write at %llu us: block %d not written correctly", (unsigned long long)(toWrite[i] / US), block);
        }
        else
        {
            check(((n == MT_DATA_WORDS) && (tapeWord(1, block, 0) == blockP(imageA, block)[0])
                && (tapeWord(1, block, 1) == dataW[0]) && (tapeBlockSum(1, block) != MT_MINUS_ZERO)),
                "search -> write at 410 us: -0 written after the lock, or word 1 not in space 4");
        }
    }

    // The first mwr is due by the interchange at the end of space 3, 600 us after the search
    // flag. At 590 us the block is written correctly; at 610 us word 1 is missed, and nothing
    // is written from space 4 on.
    check(gotoBlock(0, 63), "search to block 63 failed");
    ts = flagTime;
    at(ts + (150 * US));
    mlc(GO | WRITE);
    at(ts + (590 * US));
    mwr(dataW[0]);
    n = feedUntilBef(&dataW[1], (10 * US));
    waitNs(10 * US);
    mwr(checksumFor(dataW));
    waitNs(500 * US);
    mlc(GO | SEARCH);
    check(((n == (MT_DATA_WORDS - 1)) && (tapeWord(1, 63, 1) == dataW[0]) && (tapeBlockSum(1, 63) == MT_MINUS_ZERO)),
        "search -> write, first mwr at 590 us: block 63 not written correctly");

    check(gotoBlock(0, 64), "search to block 64 failed");
    ts = flagTime;
    at(ts + (150 * US));
    mlc(GO | WRITE);
    at(ts + (610 * US));
    check((ctl.miss && ctl.erf && !ctl.wren), "search -> write, first mwr at 610 us: no MISS");
    mwr(dataW[0]);
    for( n = 1; (n < 50) && waitDataFlag(10 * MS); ++n )
    {
        waitNs(10 * US);
        mwr(dataW[n]);
    }
    mlc(GO | SEARCH);
    ok = (tapeWord(1, 64, 0) == MT_MINUS_ZERO);
    for( k = 1; k < MT_STORED_WORDS; ++k )
    {
        ok = (ok && (tapeWord(1, 64, k) == blockP(imageA, 64)[k]));
    }
    check(ok, "search -> write, first mwr at 610 us: something was written after the lock's -0");

    // Search -> read: the leading checksum (slot 3) is caught if read is set within 600 us of
    // the search flag, DEC's figure; later, the first word is data word 1.
    for( i = 0; i < 3; ++i )
    {
        check(gotoBlock(0, 70), "search to block 70 failed");
        ts = flagTime;
        at(ts + toRead[i]);
        mlc(GO | READ);
        word = (waitFlag(10 * MS) ? mrd() : 0);
        if( i < 2 )
        {
            check(((flagTime == (ts + (600 * US))) && (word == bP[0])),
                "search -> read at %llu us: first word not the leading checksum", (unsigned long long)(toRead[i] / US));
        }
        else
        {
            check(((flagTime == (ts + (800 * US))) && (word == bP[1])), "search -> read at 601 us: first word not data word 1");
        }
    }
    note("search -> read window: 600 us for the leading checksum, as DEC gives it");
}

// The per-word deadline: each data flag answered within 200 us, in read and in write; and
// MISS in search.
static void
testWordDeadlinesAndMiss(void)
{
uint32_t *bP;
uint64_t ts;
int n;
bool ok;

    resetAll();
    blankImage(imageA);
    fillBlock(imageA, 80, 8);
    junkBlock(imageA, 81);
    bP = blockP(imageA, 80);
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 12);

    // Read: word 100 answered at 199 us is fine; word 150 at 201 us has been overwritten.
    check(gotoBlock(0, 80), "search to block 80 failed");
    mlc(GO | READ);
    ok = true;
    for( n = 0; (n <= 150) && waitFlag(10 * MS); ++n )
    {
        waitNs((n == 100) ? (199 * US) : ((n == 150) ? (201 * US) : RESPOND_NS));
        if( n == 150 )
        {
            check((ctl.miss && ctl.erf), "read: word 150 answered at 201 us did not raise MISS");
            check((mrd() == bP[151]), "read MISS: buffer not overwritten by word 151");
        }
        else
        {
            ok = (ok && (mrd() == bP[n]) && !ctl.miss);
        }
    }
    check(ok, "read: words up to 150 (word 100 answered at 199 us) wrong or MISS");
    check((ctl.units[1].motion == MT_CRUISE), "read MISS stopped the tape");

    // Write: flag 100 answered at 199 us is fine; flag 150 at 201 us is too late. MISS is
    // raised at the interchange where word 150 was due, and the writers go off before the
    // stale buffer (word 149) can be written: word 150's space and every one after it keep
    // what the tape held, and the block fails its check. The flags keep coming, the tape keeps
    // moving and GO stays set.
    check(gotoBlock(0, 81), "search to block 81 failed");
    mlc(GO | WRITE);
    for( n = 0; (n < 200) && waitDataFlag(10 * MS); ++n )
    {
        waitNs((n == 99) ? (199 * US) : ((n == 149) ? (201 * US) : RESPOND_NS));
        if( n == 149 )
        {
            check((ctl.miss && ctl.erf && !ctl.wren), "write: flag answered at 201 us did not raise MISS and stop writing");
        }
        mwr(dataW[n]);
    }
    check((n == 200), "write MISS: the flags stopped after %d", n);
    check(((mrs() & MT_ST_GO) && (ctl.units[1].motion == MT_CRUISE)), "write MISS stopped the tape");
    mlc(GO | SEARCH);
    check((tapeWord(1, 81, 100) == dataW[99]), "write: word 100 answered at 199 us not written");
    check((tapeWord(1, 81, 149) == dataW[148]), "write: word 149, the last answered in time, not written");
    ok = true;
    for( n = 150; n < MT_STORED_WORDS; ++n )
    {
        ok = (ok && (tapeWord(1, 81, n) == blockP(imageA, 81)[n]));
    }
    check(ok, "write MISS: something was written from word 150's space on (word 150's space holds %06o)",
        tapeWord(1, 81, 150));
    check((tapeBlockSum(1, 81) != MT_MINUS_ZERO), "write MISS: the block still totals -0");

    // Search: a search flag not taken before the next block's mark raises MISS; the tape runs on.
    check(gotoBlock(0, 90), "search to block 90 failed");
    ts = flagTime;
    at(ts + (uint64_t)MT_BLOCK_NS + 1);
    check((ctl.df && !ctl.miss), "search: block 91's flag missing, or MISS too early");
    at(ts + (2 * (uint64_t)MT_BLOCK_NS));
    check((ctl.miss && ctl.erf && ctl.df), "search: block 92's flag over an untaken one did not raise MISS");
    check((mrd() == (MT_MARK_FWD | 92)), "search MISS: buffer does not hold block 92");
    check(((mrs() & MT_ST_GO) && (ctl.units[1].motion == MT_CRUISE)), "search MISS stopped the tape");
}

// ---- END, UNABLE, off the reel ----------------------------------------------------------------

// END forward and in reverse: the flag, its time, the stop; and turning around in an end zone.
static void
testEnd(void)
{
uint64_t t0;
int64_t pos;
int seen;

    resetAll();
    blankImage(imageA);
    mountImage(1, imageA, false);
    selectUnit(1);

    // Forward from block 573: search flags up to block 575, then END as the head enters the
    // forward end zone, 292 slots after reaching speed.
    parkSlot(1, absSlot(573, 0));
    t0 = now;
    mlc(GO | SEARCH);
    seen = -1;
    while( waitFlag(1000 * MS) && !ctl.erf )
    {
        seen = (int)(mrd() & 07777);
    }
    check((seen == 575), "forward: last search flag before END was block %d, want 575", seen);
    check((ctl.end && ctl.erf), "forward: no END at the forward end zone");
    check((flagTime == (t0 + (uint64_t)MT_START_NS + (292 * (uint64_t)MT_SLOT_NS))), "forward END at +%llu ns",
        (unsigned long long)(flagTime - t0));
    check(((mrs() & (MT_ST_END | MT_ST_ERF | MT_ST_GO)) == (MT_ST_END | MT_ST_ERF)), "forward END: status %06o", mrs());
    waitNs(MT_STOP_NS);
    pos = mt555Position(&ctl.units[1], now);
    check(((ctl.units[1].motion == MT_STOPPED) && (pos == (MT_FEZ + (MT_STOP_NS / 2)))), "forward END: tape not stopped 375 slots in");

    // Turning around inside the end zone is not an error: reverse search finds block 574
    // (575's reverse mark passed during the start). At speed the head is at the start of
    // block 575's space 139; spaces 138-0 and then 574's space 263 go by: 140 slots.
    t0 = now;
    mlc(GO | REV | SEARCH);
    check((waitFlag(1000 * MS) && !ctl.erf), "reverse out of the end zone raised an error");
    check((mrd() == (MT_MARK_REV | 574)), "reverse out of the end zone: first block not 574");
    check((flagTime == (t0 + (uint64_t)MT_START_NS + (140 * (uint64_t)MT_SLOT_NS))), "reverse out of the end zone: flag timing");

    // Reverse from the load position: already in the reverse end zone heading off it, so END
    // comes the moment the tape is at speed.
    parkSlot(1, ((MT_LOAD_POS - MT_BA) / MT_SLOT_NS));
    t0 = now;
    mlc(GO | REV | SEARCH);
    check((waitFlag(1000 * MS) && ctl.end && ctl.erf), "reverse from the load position: no END");
    check((flagTime == (t0 + (uint64_t)MT_START_NS)), "reverse from the load position: END not on reaching speed");

    // Reverse from block 4: blocks 1 and 0, then END at the start of the block area.
    parkSlot(1, absSlot(4, 0));
    t0 = now;
    mlc(GO | REV | SEARCH);
    seen = -1;
    while( waitFlag(1000 * MS) && !ctl.erf )
    {
        seen = (int)(mrd() & 07777);
    }
    check((seen == 0), "reverse: last search flag before END was block %d, want 0", seen);
    check((ctl.end && (flagTime == (t0 + (uint64_t)MT_START_NS + (556 * (uint64_t)MT_SLOT_NS)))), "reverse END timing");

    // A tape selected while already inside the end zone ahead meets the end mark when the
    // selection delay ends -- not earlier, when it entered the zone unwatched -- and stops
    // from there.
    parkSlot(1, absSlot(575, 0));
    mlc(GO | MOVE);
    t0 = now;
    mse(2);                             // deselected, it runs on into the forward end zone
    at(t0 + (400 * MS));
    check((mt555Position(&ctl.units[1], now) > MT_FEZ), "deselected tape not in the end zone yet");
    mse(1);
    pos = mt555Position(&ctl.units[1], (now + (uint64_t)MT_SELECT_DIP_NS));
    t0 = now;
    check((waitFlag(100 * MS) && ctl.end && (flagTime == (t0 + (uint64_t)MT_SELECT_DIP_NS))),
        "selected in the end zone: END not at the end of the selection delay");
    waitNs(MT_STOP_NS);
    check(((ctl.units[1].motion == MT_STOPPED) && (mt555Position(&ctl.units[1], now) == (pos + (MT_STOP_NS / 2)))),
        "selected in the end zone: stop not measured from the end of the selection delay");

    // Forward out of the reverse end zone (the load position) is not an error: covered by
    // testSearchAndDelays(), whose first flag is block 0.
}

// Tape unable: no unit, no tape, a write to a locked unit; a refused mlc changes nothing.
static void
testUnable(void)
{
uint64_t t0;

    resetAll();
    blankImage(imageA);
    mountImage(4, imageA, true);

    mlc(GO | READ);
    check((ctl.unable && ctl.erf), "no unit selected: not refused");
    selectUnit(2);
    mlc(GO | READ);
    check((ctl.unable && ctl.erf), "unit with no tape: not refused");
    check((ctl.units[2].motion == MT_STOPPED), "unit with no tape: started");

    // A locked unit refuses write and starts nothing, but reads.
    selectUnit(4);
    parkSlot(4, absSlot(10, 0));
    mlc(GO | WRITE);
    check((mrs() == (MT_ST_ERF | MT_ST_UNABLE)), "locked write: status %06o, want 101000", mrs());
    check((ctl.units[4].motion == MT_STOPPED), "locked write: the tape started");
    mlc(GO | READ);
    check((!ctl.unable && (ctl.units[4].motion == MT_ACCEL)), "locked read: refused or not started");

    // A refused mlc leaves the motion and mode alone: still reading forward at speed. The
    // flags read so far went unanswered, so clear them first (mse of the same unit).
    t0 = now;
    at(t0 + (300 * MS));
    mse(4);
    mlc(GO | REV | WRITE);
    check((ctl.unable && (ctl.units[4].motion == MT_CRUISE) && (ctl.units[4].dir == 1) && (ctl.mode == READ)),
        "refused mlc changed the motion or mode");
    mrd();
    waitNs(250 * US);
    check(ctl.df, "after a refused mlc, reading stopped");
    mse(4);
    check(!ctl.unable, "mse did not clear UNABLE");
}

// A deselected moving tape: no flags, no END, runs off its reel, is unable until remounted.
static void
testOffReel(void)
{
uint64_t t0;
unsigned long breaks;

    resetAll();
    blankImage(imageA);
    mountImage(1, imageA, false);
    selectUnit(1);

    t0 = now;
    mlc(GO | SEARCH);
    at(t0 + (1000 * MS));
    mrd();
    mse(2);                             // unit 1 runs on, deselected
    breaks = ctl.breakCount;
    at(t0 + (36000 * MS));
    check(((ctl.units[1].motion == MT_CRUISE) && (mt555Position(&ctl.units[1], now) > MT_FEZ)),
        "deselected tape stopped at the end zone");
    at(t0 + (40000 * MS));
    check((ctl.units[1].offReel && (ctl.units[1].motion == MT_STOPPED)), "deselected tape did not run off the reel");
    check(((ctl.breakCount == breaks) && !anyFlag() && !ctl.end), "a deselected tape raised flags or END");

    selectUnit(1);
    mlc(GO | SEARCH);
    check((ctl.unable && ctl.erf), "off the reel: mlc not refused");
    check((ctl.units[1].motion == MT_STOPPED), "off the reel: the tape moved");

    // Remounting recovers the unit (the plugin does this on SIGHUP).
    mountImage(1, imageA, false);
    mlc(GO | SEARCH);
    check((!ctl.unable && (ctl.units[1].motion == MT_ACCEL)), "remount did not recover the unit");
    check((waitFlag(3000 * MS) && (mrd() == (MT_MARK_FWD | 0))), "remounted unit does not search from the load position");
}

// Write entered below speed, and read -> write inside a block with the manual's exceptions.
static void
testWriteEntry(void)
{
uint64_t t0;
uint64_t td;
int n;
int k;

    resetAll();
    blankImage(imageA);
    for( k = 100; k < 106; ++k )
    {
        fillBlock(imageA, k, k);
    }
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 21);

    // From rest: the entry DF waits for the tape to reach speed.
    parkSlot(1, absSlot(99, 100));
    t0 = now;
    mlc(GO | WRITE);
    check(!ctl.df, "write from rest raised DF during the start delay");
    check((waitFlag(1000 * MS) && (flagTime == (t0 + (uint64_t)MT_START_NS))), "write from rest: DF not on reaching speed");
    mlc(GO | SEARCH);

    // Read -> write after data word 50 (slot 53): the first word written lands in slot 56,
    // the third space after; slots 54 and 55 are lost. The first flag after the switch asks
    // for the second word.
    check(gotoBlock(0, 101), "search to block 101 failed");
    mlc(GO | READ);
    for( n = 0; (n <= 50) && waitFlag(10 * MS); ++n )
    {
        if( n < 50 )
        {
            waitNs(RESPOND_NS);
            mrd();
        }
    }
    td = flagTime;                      // DF for data word 50, end of slot 53
    waitNs(RESPOND_NS);
    mlc(GO | WRITE);
    check((mrd() == blockP(imageA, 101)[50]), "read -> write: mrd after mlc does not return the last word read");
    mwr(dataW[0]);
    check((waitFlag(10 * MS) && (flagTime == (td + (2 * 200 * US)))), "read -> write: first flag not at the end of slot 55");
    waitNs(RESPOND_NS);
    mwr(dataW[1]);
    waitFlag(10 * MS);
    mlc(GO | SEARCH);
    check(((tapeWord(1, 101, 51) == blockP(imageA, 101)[51]) && (tapeWord(1, 101, 52) == blockP(imageA, 101)[52])),
        "read -> write: the two lost spaces were written");
    check(((tapeWord(1, 101, 53) == dataW[0]) && (tapeWord(1, 101, 54) == dataW[1])),
        "read -> write: first word not in the third space after the last read");

    // Exception 1: switching after data word 253 puts the first word in the last data slot
    // (259), so the first flag after the switch is BEF.
    check(gotoBlock(0, 103), "search to block 103 failed");
    mlc(GO | READ);
    for( n = 0; (n <= 253) && waitFlag(10 * MS); ++n )
    {
        if( n < 253 )
        {
            waitNs(RESPOND_NS);
            mrd();
        }
    }
    waitNs(RESPOND_NS);
    mlc(GO | WRITE);
    mrd();
    mwr(dataW[0]);
    check((waitFlag(10 * MS) && ctl.bef && !ctl.df), "read -> write into slot 259: first flag is not BEF");
    waitNs(RESPOND_NS);
    mwr(0);
    waitNs(500 * US);
    mlc(GO | SEARCH);
    check((tapeWord(1, 103, 256) == dataW[0]), "read -> write into slot 259: word not in the last data slot");

    // Exception 2: switching after data word 254 puts the first word in the checksum slot
    // (260); no flag until the start of the next block (its slot 2).
    check(gotoBlock(0, 104), "search to block 104 failed");
    mlc(GO | READ);
    for( n = 0; (n <= 254) && waitFlag(10 * MS); ++n )
    {
        if( n < 254 )
        {
            waitNs(RESPOND_NS);
            mrd();
        }
    }
    td = flagTime;                      // end of slot 257
    waitNs(RESPOND_NS);
    mlc(GO | WRITE);
    mrd();
    mwr(dataW[0]);
    check((waitFlag(10 * MS) && ctl.df && (flagTime == (td + (9 * 200 * US)))),
        "read -> write into slot 260: first flag not at the next block's slot 2");
    mlc(GO | SEARCH);
    check((tapeWord(1, 104, 257) == dataW[0]), "read -> write into slot 260: word not in the checksum slot");
}

// ---- Write enable, the D256 latch, All Halt (TASK-TAPE-HALT-WRITE) ----------------------------

// Returns the stored-word index k of the space the next write interchange on the selected,
// cruising unit (moving forward) would fill: the space after the one the head is in.
static int
nextWriteK(void)
{
    return( (int)((ctl.units[ctl.selUnit].nextBoundary - 1) % MT_SLOTS_PER_BLOCK) + 1 - MT_SLOT_REVCHECK );
}

// WRITE ENABLE (H-550 p. 2-32): a MISS switches the writers off; mse clears the error but the
// writers stay off; only an mlc with go and write mode turns them on again. END during a
// write switches them off too.
static void
testWriteEnable(void)
{
uint32_t cs;
int n;
int k;
int kOn;
bool ok;

    resetAll();
    blankImage(imageA);
    junkBlock(imageA, 120);
    mountImage(1, imageA, false);
    selectUnit(1);
    makeData(dataW, 31);
    cs = checksumFor(dataW);

    // Write block 120, answering the flag for data word 11 at 201 us: MISS, writers off.
    check(gotoBlock(0, 120), "search to block 120 failed");
    mlc(GO | WRITE);
    check(ctl.wren, "mlc go write did not set WRITE ENABLE");
    for( n = 0; (n < 20) && waitDataFlag(10 * MS); ++n )
    {
        waitNs((n == 10) ? (201 * US) : RESPOND_NS);
        mwr(dataW[n]);
    }
    check((ctl.miss && ctl.erf && !ctl.wren), "a missed word did not raise MISS and stop writing");

    // mse alone clears the error; the flags keep coming and are answered in time, and still
    // nothing is written.
    mse(1);
    check((!ctl.miss && !ctl.erf && !ctl.wren), "mse did not clear MISS, or turned writing back on");
    for( ; (n < 40) && waitDataFlag(10 * MS); ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataW[n]);
    }
    check(((n == 40) && !ctl.miss), "after mse, flags stopped or answered ones gave MISS");
    ok = (tapeWord(1, 120, 10) == dataW[9]);
    for( k = 11; k < MT_STORED_WORDS; ++k )
    {
        ok = (ok && (tapeWord(1, 120, k) == blockP(imageA, 120)[k]));
    }
    check(ok, "words were written between the MISS and the next write mlc");

    // An mlc with go and write mode turns writing on again: it asks for a word at once, and
    // that word lands in the next space.
    mlc(GO | WRITE);
    check((ctl.wren && ctl.df), "mlc go write after mse: writing not enabled, or no DF");
    kOn = nextWriteK();
    waitNs(RESPOND_NS);
    mwr(0123456);
    check(waitDataFlag(10 * MS), "no DF after the write mlc");
    check(((tapeWord(1, 120, kOn) == 0123456) && (tapeWord(1, 120, (kOn - 1)) == blockP(imageA, 120)[kOn - 1])),
        "after the write mlc the word is not in space k=%d (holds %06o)", kOn, tapeWord(1, 120, kOn));
    mlc(GO | SEARCH);
    check(!ctl.wren, "mlc search left writing enabled");

    // END during a write: the last block is written whole, then the end zone stops the tape
    // with END, and the writers are off.
    check(gotoBlock(0, (MT_BLOCKS - 1)), "search to the last block failed");
    check((writeBlock(0, dataW, cs, RESPOND_NS) == MT_DATA_WORDS), "write of the last block failed");
    check((tapeBlockSum(1, (MT_BLOCKS - 1)) == MT_MINUS_ZERO), "the last block does not check");
    mlc(GO | WRITE);
    check(ctl.wren, "write mlc before the end zone: writing not enabled");
    check((waitFlag(1000 * MS) && ctl.df), "no DF after the write mlc before the end zone");
    mwr(0);
    check((waitFlag(1000 * MS) && ctl.end && ctl.erf && !ctl.wren), "END during a write did not stop writing");
    check(((mrs() & MT_ST_GO) == 0), "END during a write: GO still set");
    check((tapeBlockSum(1, (MT_BLOCKS - 1)) == MT_MINUS_ZERO), "the last block was changed after it was written");
}

// The D256 latch (DECUS 1963 p. B-2, brochure F-03 p. 10): an mlc given while writing, after
// the control has asked for the last data word and before the checksum is on the tape, waits
// for the checksum; one given earlier takes effect at once.
static void
testD256Latch(void)
{
uint32_t cs;
uint64_t tb;
uint64_t tf;
int block;
int n;
bool ok;

    resetAll();
    blankImage(imageA);
    for( block = 130; block < 140; ++block )
    {
        junkBlock(imageA, block);
    }
    mountImage(1, imageA, false);
    mountImage(2, imageA, false);
    selectUnit(1);
    makeData(dataW, 41);
    cs = checksumFor(dataW);

    // Search at once after the checksum's mwr: held (the mode and GO read as before), the
    // checksum is written, the block checks, and the next block's search flag comes 1.2 ms
    // after the BEF, as if search had been given as the checksum was written.
    check(writeToBef(130), "write to block 130's BEF failed");
    tb = flagTime;
    waitNs(RESPOND_NS);
    mwr(cs);
    waitNs(10 * US);
    mlc(GO | SEARCH);
    check((ctl.ctlHeld && (ctl.mode == WRITE) && (mrs() & MT_ST_GO)), "search after the checksum's mwr was not held");
    check((waitFlag(10 * MS) && (flagTime == (tb + (1200 * US))) && (mrd() == (MT_MARK_FWD | 131))),
        "held search: block 131's flag not 1.2 ms after BEF");
    check(((tapeWord(1, 130, 257) == cs) && (tapeBlockSum(1, 130) == MT_MINUS_ZERO) && !ctl.ctlHeld),
        "held search: the checksum was not written, or the block does not check");

    // Stop at the last data word's flag: held; the BEF still comes and is answered, and the
    // tape stays at speed until the checksum is written, then begins to stop.
    check(gotoBlock(0, 132), "search to block 132 failed");
    mlc(GO | WRITE);
    for( n = 0; (n < MT_DATA_WORDS) && waitDataFlag(10 * MS) && !ctl.bef; ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataW[n]);
    }
    check((n == MT_DATA_WORDS), "block 132: %d data flags before the stop", n);
    mlc(MOVE);
    check((ctl.ctlHeld && (ctl.units[1].motion == MT_CRUISE)), "stop at the last data word was not held");
    check((waitFlag(10 * MS) && ctl.bef), "held stop: no BEF");
    tf = (flagTime + (200 * US));
    waitNs(RESPOND_NS);
    mwr(cs);
    at(tf - 1);
    check((ctl.units[1].motion == MT_CRUISE), "held stop: the tape slowed before the checksum");
    at(tf);
    check(((ctl.units[1].motion == MT_DECEL) && ((mrs() & MT_ST_GO) == 0) && (ctl.mode == MOVE)),
        "held stop: not carried out as the checksum was written");
    check(((tapeWord(1, 132, 256) == dataW[255]) && (tapeBlockSum(1, 132) == MT_MINUS_ZERO)),
        "held stop: block 132 does not check");

    // Control leg: search given in the space before the window (at the flag for data word
    // 255) takes effect at once. Neither word 255, word 256 nor the checksum is written.
    check(gotoBlock(0, 134), "search to block 134 failed");
    mlc(GO | WRITE);
    for( n = 0; (n < 255) && waitDataFlag(10 * MS); ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataW[n]);
    }
    mlc(GO | SEARCH);
    check((!ctl.ctlHeld && (ctl.mode == SEARCH)), "search at word 255's flag was held");
    waitNs(1 * MS);
    ok = true;
    for( n = 255; n < MT_STORED_WORDS; ++n )
    {
        ok = (ok && (tapeWord(1, 134, n) == blockP(imageA, 134)[n]));
    }
    check((ok && (tapeWord(1, 134, 254) == dataW[253])), "search at word 255's flag: the block's end was written");

    // Two commands in the window: the later one is carried out.
    check(writeToBef(136), "write to block 136's BEF failed");
    tb = flagTime;
    waitNs(RESPOND_NS);
    mwr(cs);
    mlc(GO | MOVE);
    mlc(GO | SEARCH);
    check((waitFlag(10 * MS) && (flagTime == (tb + (1200 * US))) && (mrd() == (MT_MARK_FWD | 137))),
        "two held commands: the later one (search) was not carried out");

    // A selection change in the window carries the held command out at once, on the drive
    // it was given for; that drive is no longer watched, so its checksum is not written.
    check(writeToBef(138), "write to block 138's BEF failed");
    waitNs(RESPOND_NS);
    mwr(cs);
    mlc(GO | MOVE);
    mse(2);
    check((!ctl.ctlHeld && (ctl.mode == MOVE) && ctl.units[1].goCmd && !ctl.wren),
        "held command not carried out by the selection change");
    waitNs(1 * MS);
    check((tapeWord(1, 138, 257) == blockP(imageA, 138)[257]), "selection change in the window: the checksum was written");
}

// All Halt (H-550 p. 2-17) through mt550AllHalt(), as the plugin calls it when RUN falls.
static void
testAllHalt(void)
{
uint64_t t0;
int64_t pos1;
int64_t pos2;
unsigned long breaks;
unsigned long flushes;
int n;
bool ok;

    resetAll();
    blankImage(imageA);
    junkBlock(imageA, 140);
    junkBlock(imageA, 141);
    junkBlock(imageA, 142);
    mountImage(1, imageA, false);
    mountImage(2, imageA, false);
    mountImage(3, imageA, false);
    makeData(dataW, 51);

    // The selected drive writing block 140, and a deselected drive at speed.
    selectUnit(2);
    parkSlot(2, absSlot(300, 0));
    mlc(GO | SEARCH);
    selectUnit(1);
    check(gotoBlock(0, 140), "search to block 140 failed");
    mlc(GO | WRITE);
    for( n = 0; (n < 50) && waitDataFlag(10 * MS); ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataW[n]);
    }
    check(((ctl.units[1].motion == MT_CRUISE) && (ctl.units[2].motion == MT_CRUISE)), "setup: drives 1 and 2 not at speed");
    flushes = ctl.units[1].flushCount;
    breaks = ctl.breakCount;
    t0 = now;
    mt550AllHalt(&ctl, now);
    check(((ctl.units[1].motion == MT_DECEL) && (ctl.units[2].motion == MT_DECEL)), "All Halt: a drive is not stopping");
    check((!ctl.units[1].goCmd && !ctl.units[2].goCmd && ((mrs() & MT_ST_GO) == 0)), "All Halt: GO still set");
    check(((ctl.mode == WRITE) && !ctl.wren && !ctl.erf), "All Halt: mode changed, writing enabled, or an error");
    check(((ctl.units[1].flushCount == (flushes + 1)) && (ctl.units[1].dirtyBlock < 0)), "All Halt: the partial block was not flushed");
    at(t0 + (1000 * MS));
    check(((ctl.units[1].motion == MT_STOPPED) && (ctl.units[2].motion == MT_STOPPED)), "All Halt: not stopped after 1 s");
    check((ctl.breakCount == breaks), "All Halt: %lu flags raised after the halt", (ctl.breakCount - breaks));
    ok = (tapeWord(1, 140, 49) == dataW[48]);
    for( n = 50; n < MT_STORED_WORDS; ++n )
    {
        ok = (ok && (tapeWord(1, 140, n) == blockP(imageA, 140)[n]));
    }
    check((ok && (tapeBlockSum(1, 140) != MT_MINUS_ZERO)), "All Halt: block 140 not written up to the halt and no further");
    for( n = 0; n < MT_STORED_WORDS; ++n )
    {
        ok = (ok && (tapeWord(1, 141, n) == blockP(imageA, 141)[n]));
    }
    check(ok, "All Halt: block 141 was touched");
    pos1 = mt555Position(&ctl.units[1], now);
    pos2 = mt555Position(&ctl.units[2], now);
    at(t0 + (60000 * MS));
    check(((mt555Position(&ctl.units[1], now) == pos1) && (mt555Position(&ctl.units[2], now) == pos2)
        && !ctl.units[2].offReel), "All Halt: a drive moved again, or the deselected one ran off its reel");

    // Nothing moves until an mlc with go.
    mlc(GO | SEARCH);
    check(((ctl.units[1].motion == MT_ACCEL) && waitFlag(3000 * MS) && ctl.df), "after All Halt, an mlc go did not move drive 1");
    mlc(MOVE);

    // A drive still accelerating (selected), and a deselected one part way through a
    // turnaround, both stop; the turnaround does not complete.
    selectUnit(3);
    parkSlot(3, absSlot(200, 0));
    mlc(GO | SEARCH);
    waitNs(300 * MS);
    mlc(GO | REV | SEARCH);
    check(((ctl.units[3].motion == MT_DECEL) && ctl.units[3].pendingAccel), "setup: drive 3 not turning around");
    selectUnit(1);                      // 34 ms into drive 3's 150 ms stop
    mlc(GO | SEARCH);
    waitNs(50 * MS);
    check((ctl.units[1].motion == MT_ACCEL), "setup: drive 1 not accelerating");
    breaks = ctl.breakCount;
    mt550AllHalt(&ctl, now);
    check(((ctl.units[1].motion == MT_DECEL) && (ctl.units[3].motion == MT_DECEL) && !ctl.units[3].pendingAccel),
        "All Halt: an accelerating or turning drive did not simply stop");
    waitNs(1000 * MS);
    check(((ctl.units[1].motion == MT_STOPPED) && (ctl.units[3].motion == MT_STOPPED) && (ctl.units[3].dir == 1)
        && (ctl.breakCount == breaks)), "All Halt: drives 1 and 3 not stopped forward with no flags");

    // An mlc held for a checksum is dropped: the tape stops short of the checksum.
    check(gotoBlock(0, 142), "search to block 142 failed");
    mlc(GO | WRITE);
    for( n = 0; (n < MT_DATA_WORDS) && waitDataFlag(10 * MS) && !ctl.bef; ++n )
    {
        waitNs(RESPOND_NS);
        mwr(dataW[n]);
    }
    check((waitFlag(10 * MS) && ctl.bef), "block 142: no BEF");
    waitNs(RESPOND_NS);
    mwr(checksumFor(dataW));
    mlc(GO | SEARCH);
    check(ctl.ctlHeld, "setup: search after the checksum's mwr not held");
    mt550AllHalt(&ctl, now);
    waitNs(1000 * MS);
    check((!ctl.ctlHeld && (ctl.mode == WRITE) && (ctl.units[1].motion == MT_STOPPED)),
        "All Halt: the held command survived the halt");
    check((tapeWord(1, 142, 257) == blockP(imageA, 142)[257]), "All Halt: the checksum was written after the halt");
}

// ---- Image files ------------------------------------------------------------------------------

// Writes MT_IMAGE_WORDS words of imgP to pathP, or bytes bytes if that is smaller. Returns
// true on success.
static bool
writeImageFile(const char *pathP, const uint32_t *imgP, size_t bytes)
{
int fd;
ssize_t put;

    if( (fd = open(pathP, (O_WRONLY | O_CREAT | O_TRUNC), 0644)) < 0 )
    {
        return(false);
    }

    put = write(fd, imgP, bytes);
    close(fd);
    return( put == (ssize_t)bytes );
}

// Returns the size of the file at pathP in bytes, or -1 if it does not exist.
static long long
fileSize(const char *pathP)
{
struct stat st;

    return( (stat(pathP, &st) == 0) ? (long long)st.st_size : -1 );
}

// Reads block of the image file at pathP into wordsP (MT_STORED_WORDS words).
// Returns true if the whole block was there to read.
static bool
readFileBlock(const char *pathP, int block, uint32_t *wordsP)
{
int fd;
bool ok;

    ok = false;
    if( (fd = open(pathP, O_RDONLY)) >= 0 )
    {
        ok = (pread(fd, wordsP, MT_BLOCK_BYTES, ((off_t)block * MT_BLOCK_BYTES)) == (ssize_t)MT_BLOCK_BYTES);
        close(fd);
    }

    return(ok);
}

// Returns true if the MT_STORED_WORDS words at wordsP are a blank block.
static bool
isBlankBlock(const uint32_t *wordsP)
{
int k;

    for( k = 1; k < MT_STORED_WORDS; ++k )
    {
        if( wordsP[k] != 0 )
        {
            return(false);
        }
    }

    return( wordsP[0] == MT_MINUS_ZERO );
}

// Mounting from a file, write-through at the end of a block, persistence across a remount,
// locked and read-only files, and bad files.
static void
testImageFile(void)
{
static const char *pathP = "mttest-unit.img";
static const char *shortP = "mttest-short.img";
uint32_t fileWords[MT_STORED_WORDS];
char err[200];
uint32_t cs;
int fd;
int i;
bool ok;

    resetAll();
    blankImage(imageA);
    check(writeImageFile(pathP, imageA, MT_IMAGE_BYTES), "cannot create %s", pathP);
    check(mt555MountFile(&ctl.units[1], pathP, false, false, err, sizeof(err)), "mount failed: %s", err);
    check(!ctl.units[1].locked, "writable file mounted locked");
    check((ctl.units[1].fileBlocks == MT_BLOCKS), "full image: %d blocks in the file, want 576", ctl.units[1].fileBlocks);
    selectUnit(1);

    makeData(dataW, 33);
    cs = checksumFor(dataW);
    check(gotoBlock(0, 3), "search to block 3 failed");
    check((writeBlock(0, dataW, cs, RESPOND_NS) == MT_DATA_WORDS), "write of block 3 failed");
    check((ctl.units[1].flushCount == 1), "%lu flushes, want 1", ctl.units[1].flushCount);

    // The block is in the file as soon as its write finished.
    ok = false;
    if( (fd = open(pathP, O_RDONLY)) >= 0 )
    {
        ok = (pread(fd, fileWords, sizeof(fileWords), (off_t)(3 * sizeof(fileWords))) == (ssize_t)sizeof(fileWords));
        close(fd);
    }
    ok = (ok && (fileWords[0] == MT_MINUS_ZERO) && (fileWords[257] == cs));
    for( i = 0; i < MT_DATA_WORDS; ++i )
    {
        ok = (ok && (fileWords[1 + i] == dataW[i]));
    }
    check(ok, "block 3 not in the file after its write");
    check(!ctl.units[1].ioError, "I/O error reported");

    // Unmount and remount: the data is still there.
    mt555Unmount(&ctl.units[1]);
    check(mt555MountFile(&ctl.units[1], pathP, false, false, err, sizeof(err)), "remount failed: %s", err);
    check(((tapeWord(1, 3, 1) == dataW[0]) && (tapeBlockSum(1, 3) == MT_MINUS_ZERO)), "data lost across a remount");
    check(mt555SameFile(&ctl.units[1], pathP), "mt555SameFile does not know the mounted file");
    check(!mt555SameFile(&ctl.units[1], "mttest-nonexistent.img"), "mt555SameFile matches a missing file");

    // Mounted locked: write refused.
    check(mt555MountFile(&ctl.units[1], pathP, true, false, err, sizeof(err)), "locked mount failed: %s", err);
    mt550UnitRemounted(&ctl, 1, now);
    mlc(GO | WRITE);
    check(ctl.unable, "locked file: write not refused");
    check(mt555MountFile(&ctl.units[1], "mttest-nonexistent.img", false, false, err, sizeof(err)) == false, "missing file mounted");
    check(!ctl.units[1].mounted, "failed mount left a tape mounted");
    check((fileSize("mttest-nonexistent.img") < 0), "a mount without create made a file");

    // Locked and missing: create does not apply; nothing is made.
    check(mt555MountFile(&ctl.units[1], "mttest-nonexistent.img", true, true, err, sizeof(err)) == false,
        "missing file mounted locked");
    check((fileSize("mttest-nonexistent.img") < 0), "a locked mount made a file");

    // Not a whole number of blocks, or longer than a tape.
    check(writeImageFile(shortP, imageA, (MT_IMAGE_BYTES - 4)), "cannot create %s", shortP);
    check((mt555MountFile(&ctl.units[1], shortP, false, false, err, sizeof(err)) == false), "part-block file mounted");
    check((strstr(err, "size") != NULL), "part-block file: message does not mention the size: %s", err);
    check(writeImageFile(shortP, imageA, (MT_BLOCK_BYTES + 4)), "cannot create %s", shortP);
    check((mt555MountFile(&ctl.units[1], shortP, false, false, err, sizeof(err)) == false), "block-and-a-word file mounted");
    unlink(shortP);
    ok = writeImageFile(shortP, imageA, MT_IMAGE_BYTES);
    if( ok && ((fd = open(shortP, (O_WRONLY | O_APPEND))) >= 0) )
    {
        ok = (write(fd, imageA, MT_BLOCK_BYTES) == (ssize_t)MT_BLOCK_BYTES);
        close(fd);
    }
    check(ok, "cannot make an oversize file");
    check((mt555MountFile(&ctl.units[1], shortP, false, false, err, sizeof(err)) == false), "oversize file mounted");
    check((fileSize(shortP) == (MT_IMAGE_BYTES + MT_BLOCK_BYTES)), "a refused mount changed the file");
    check((mt555MountFile(&ctl.units[1], ".", false, true, err, sizeof(err)) == false), "a directory mounted");

    // A read-only file is a locked reel (not checkable as root, who can write anything).
    if( geteuid() != 0 )
    {
        chmod(pathP, 0444);
        check(mt555MountFile(&ctl.units[1], pathP, false, false, err, sizeof(err)), "read-only file did not mount: %s", err);
        check(ctl.units[1].locked, "read-only file not mounted locked");
        chmod(pathP, 0644);
    }
    else
    {
        note("running as root: read-only file check skipped");
    }

    mt555Unmount(&ctl.units[1]);
    unlink(pathP);
    unlink(shortP);
}

// Deferred formatting (TASK-REWORK.md): a missing file is created empty, which is a blank tape;
// a short file's missing blocks read blank; writing past the end grows the file with blank
// blocks, never a hole; mode 7 truncates the file to empty.
static void
testDeferredImage(void)
{
static const char *pathP = "mttest-deferred.img";
uint32_t fileWords[MT_STORED_WORDS];
char err[200];
uint32_t cs;
uint32_t csV;
int block;
bool ok;

    resetAll();
    unlink(pathP);

    // Created: an empty file, every block blank and checking.
    check(mt555MountFile(&ctl.units[1], pathP, false, true, err, sizeof(err)), "create failed: %s", err);
    check((ctl.units[1].created && (fileSize(pathP) == 0) && (ctl.units[1].fileBlocks == 0)),
        "a created image is not an empty file (%lld bytes)", fileSize(pathP));
    ok = true;
    for( block = 0; block < MT_BLOCKS; ++block )
    {
        ok = (ok && (tapeWord(1, block, 0) == MT_MINUS_ZERO) && (tapeWord(1, block, 128) == 0)
            && (tapeBlockSum(1, block) == MT_MINUS_ZERO));
    }
    check(ok, "an empty image does not read as a blank tape");
    mt550UnitRemounted(&ctl, 1, now);
    selectUnit(1);

    // Write block 5 of the empty file: the file grows to 6 blocks, 0-4 blank, none a hole.
    makeData(dataW, 55);
    cs = checksumFor(dataW);
    check(gotoBlock(0, 5), "search to block 5 failed");
    check((writeBlock(0, dataW, cs, RESPOND_NS) == MT_DATA_WORDS), "write of block 5 failed");
    check((fileSize(pathP) == (6 * MT_BLOCK_BYTES)), "after block 5: file is %lld bytes, want %d",
        fileSize(pathP), (6 * MT_BLOCK_BYTES));
    ok = true;
    for( block = 0; block < 5; ++block )
    {
        ok = (ok && readFileBlock(pathP, block, fileWords) && isBlankBlock(fileWords));
    }
    check(ok, "the blocks before the written one are not blank in the file (a hole?)");
    check((readFileBlock(pathP, 5, fileWords) && (fileWords[0] == MT_MINUS_ZERO) && (fileWords[1] == dataW[0])
        && (fileWords[257] == cs)), "block 5 not in the file after its write");

    // Writing block 2, inside the file, rewrites it in place: no growth.
    makeData(dataV, 22);
    csV = checksumFor(dataV);
    check(gotoBlock(0, 2), "search to block 2 failed");
    check((writeBlock(0, dataV, csV, RESPOND_NS) == MT_DATA_WORDS), "write of block 2 failed");
    check((fileSize(pathP) == (6 * MT_BLOCK_BYTES)), "rewriting block 2 changed the size to %lld", fileSize(pathP));
    check((readFileBlock(pathP, 2, fileWords) && (fileWords[1] == dataV[0]) && (fileWords[257] == csV)),
        "block 2 not in the file after its write");

    // Remount the short file: its blocks are back, and those past its end read blank.
    mt555Unmount(&ctl.units[1]);
    check(mt555MountFile(&ctl.units[1], pathP, false, true, err, sizeof(err)), "remount failed: %s", err);
    check(!ctl.units[1].created, "remounting an existing file says it was created");
    check((ctl.units[1].fileBlocks == 6), "remount: %d blocks in the file, want 6", ctl.units[1].fileBlocks);
    check(((tapeWord(1, 5, 1) == dataW[0]) && (tapeWord(1, 2, 1) == dataV[0])), "short file: data lost across a remount");
    check(((tapeWord(1, 6, 0) == MT_MINUS_ZERO) && (tapeBlockSum(1, 300) == MT_MINUS_ZERO)),
        "short file: blocks past the end are not blank");

    // Mode 7 erases the reel and truncates the file to empty.
    mt550UnitRemounted(&ctl, 1, now);
    selectUnit(1);
    mlc(MT_MODE_ERASE);
    check((fileSize(pathP) == 0), "mode 7 left the file at %lld bytes", fileSize(pathP));
    check(((tapeWord(1, 5, 1) == 0) && (tapeWord(1, 5, 0) == MT_MINUS_ZERO)), "mode 7 did not blank the reel");
    check((!ctl.unable && !ctl.units[1].ioError), "mode 7 on a file: refused or an I/O error");

    // The last block of the tape can be written into an empty file: it grows to full size.
    check(gotoBlock(0, (MT_BLOCKS - 1)), "search to the last block failed");
    check((writeBlock(0, dataW, cs, RESPOND_NS) == MT_DATA_WORDS), "write of the last block failed");
    check((fileSize(pathP) == MT_IMAGE_BYTES), "after the last block: file is %lld bytes, want %d",
        fileSize(pathP), MT_IMAGE_BYTES);
    check((readFileBlock(pathP, 300, fileWords) && isBlankBlock(fileWords)), "block 300 is not blank in the file");

    // A drive with no file behind it: mode 7 still blanks the memory copy.
    mt555Unmount(&ctl.units[1]);
    blankImage(imageA);
    fillBlock(imageA, 9, 9);
    mountImage(1, imageA, false);
    check(mt555Erase(&ctl.units[1]) && (tapeWord(1, 9, 1) == 0), "erase of an in-memory reel failed");
    mountImage(1, imageA, true);
    check(!mt555Erase(&ctl.units[1]) && (tapeWord(1, 9, 1) == blockP(imageA, 9)[1]), "a locked reel was erased");

    mt555Unmount(&ctl.units[1]);
    unlink(pathP);
}

// ---- main -------------------------------------------------------------------------------------

// Runs every test group. Returns 0 if every check passed, 1 otherwise.
int
main(int argc, char **argv)
{
    verbose = ((argc > 1) && (strcmp(argv[1], "-v") == 0));

    runTest(testIots, "5.2 IOTs, status word, unit numbers, modes 4-6; mode 7 erases");
    runTest(testSearchAndDelays, "5.3 search; start, turnaround and selection delays; stop");
    runTest(testRead, "5.3 read forward and reverse, checksum total");
    runTest(testWrite, "5.3 write forward and reverse, reverse-read identity, partial rewrite");
    runTest(testBlockEndDeadlines, "3.5 block-end deadlines (1.4 ms, 1.6 ms, 800 us, 1.2 ms), hit and missed");
    runTest(testSearchDeadlines, "3.5 search-flag deadlines (write 400 us, read 600 us)");
    runTest(testWordDeadlinesAndMiss, "3.5 200 us word deadline in read and write; MISS in search");
    runTest(testEnd, "END forward and reverse; turnaround in an end zone");
    runTest(testUnable, "tape unable: no unit, no tape, locked write");
    runTest(testOffReel, "deselected tape runs off the reel; remount recovers");
    runTest(testWriteEntry, "write entered below speed; read -> write and its two exceptions");
    runTest(testWriteEnable, "write enable: off on MISS and END, mse keeps it off, a write mlc turns it on");
    runTest(testD256Latch, "D256 latch: an mlc at the last data word waits for the checksum");
    runTest(testAllHalt, "All Halt: every moving drive stops, GO 0, block flushed, nothing moves until mlc");
    runTest(testImageFile, "image file mount, write-through, remount, locked, bad files");
    runTest(testDeferredImage, "deferred formatting: create, grow without holes, short files, mode 7 truncates");

    resetAll();
    printf("%d checks, %d failed\n", checkCount, failCount);
    return( (failCount == 0) ? 0 : 1 );
}
