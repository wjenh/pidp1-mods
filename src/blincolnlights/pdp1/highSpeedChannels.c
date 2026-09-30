/*
 * This is a loose implementation of the Type 19 High Speed Channel Control.
 * It is for use in IOTs or other emulator code to package up direct memory access
 * and simulate the behavior of the PDP-1 dma, hiding details of memory back wraparound, etc.
 *
 * Execution model. processHSCchannels() runs on the emulator thread once per main-loop pass
 * (one 5us memory cycle) while the machine runs. Each pass, the highest-priority channel that
 * wants a memory cycle on that pass takes it, and the CPU loses that cycle; a busy channel that
 * wants no cycle on this pass lets a lower one through, as the Type 19 arbitrated each memory
 * cycle (F25; F17 3-31). Every word reaches core at its own stolen cycle, except in IMMEDIATE
 * and THREADED modes, whose requester moves the data itself and leaves only the cycles owed.
 * Requests can come from any thread (the 340 has its own); the emulator thread never takes a
 * lock or waits on another thread here, so every field the two share is an atomic, and a
 * request is published by storing scanKind last. Only the emulator thread writes the panel's
 * hsc lamp, and only processHSCchannels() does; the tally belongs to main.c and cycle().
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
 * 23-Sep-2026 claude - per-cycle arbitration, owed cycles for THREADED and HSCsteal(), TRUESTEAL
 *    timed from simtime with one word per steal, lock-free publication and wake-up, lamp on the
 *    emulator thread only.
 *    The same day: TRUESTEAL's spreading is computed rather than accumulated. The accumulator
 *    made one word too many on about a quarter of transfer sizes, found by the DEC drum diagnostic.
*/

#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

//#define DOLOGGING
#define LOG_HSC 0
#define LOG_EXEC 0
#define LOG_DATA 0

#include "common.h"
#include "pdp1.h"
#include "logger.h"
#include "highSpeedChannels.h"

// Stretch any lamp request of < HSC_STRETCH passes to this.
// Otherwise, we would rarely see the light when using the type340 which
// does single-word transfers.
#define HSC_STRETCH 20

// One memory cycle in simtime units (ns).
#define HSC_CYCLE_NS 5000

// What the scan does for a channel besides paying owed cycles.
#define SCAN_IDLE       0
#define SCAN_NORMAL     1
#define SCAN_TRUESTEAL  2

// Original had three channels, priority ordered 1-3, we do 5, same priority order
#define NUMCHANS 5

// THe threshold where we usleep() instead of spin-wait.
#define HSC_SPIN_LIMIT_US 20

// Fields marked "emulator" are touched only by the emulator thread once a request is published;
// "requester" fields only by the thread that owns the channel.
typedef struct {
    bool isInitialized;
    bool isAssigned;
    atomic_int status;
    atomic_int scanKind;        // SCAN_*; stored last when a request is set up, so the scan sees all of it
    atomic_int owed;            // memory cycles owed to the CPU, paid one per pass
    atomic_int lampWanted;      // passes the requester wants the hsc lamp lit; taken by the scan
    atomic_int isWaiting;       // a thread is blocked, or about to block, on waitSemaphore
    int waitDelay;              // requester: THREADED delay, in us, HSCwait() still has to time
    int onCount;                // emulator: passes this channel keeps the lamp lit
    bool started;               // emulator: TRUESTEAL startSim has been taken
    uint64_t startSim;          // emulator: simtime the TRUESTEAL transfer's ticks count from
    long capSeen;               // emulator: throttleCapFirings when last looked at
    int ticksTotal;             // TRUESTEAL: 5us ticks the whole transfer occupies
    int ticksDone;              // emulator: ticks already run through the spreading
    int wordsMade;              // emulator: words the spreading has made due so far
    int wordsDue;               // emulator: words due but not yet moved (lost arbitration, or after a mul/div)
    sem_t waitSemaphore;        // how a waiter is told of completion
    HSCRequest request;         // the request, copied from the caller in normal and TRUESTEAL modes
    } HSCControl, *HSCControlP;

static HSCControl chan1;
static HSCControl chan2;
static HSCControl chan3;
static HSCControl chan4;
static HSCControl chan5;

static HSCControlP chans[] = {&chan1, &chan2, &chan3, &chan4, &chan5};

static HSCControlP getControlP(HSCChannelP chanP);
static void completeChannel(HSCControlP ctlP);
static void wakeWaiter(HSCControlP ctlP);
static void requestLamp(HSCControlP ctlP, int passes);
static void processImmediate(HSCRequestP requestP);
static void moveWord(HSCRequestP rqstP);
static void advanceTrueSteal(HSCControlP ctlP);
static bool serviceChannel(HSCControlP ctlP, bool mayTake);
static void hscSpinWait(int us);

extern PDP1P pdp1P;     // from main.c

// Service routine called from the run loop on the emulator thread, once per 5us pass.
// Returns true if a channel took this pass's memory cycle (the CPU must not cycle), else false.
bool
processHSCchannels()
{
int i;
int lamp;
bool steal;
bool lampOn;
HSCControlP ctlP;

    // Every channel is visited every pass, in priority order: a channel that has lost
    // arbitration still keeps its TRUESTEAL clock and its lamp running.
    steal = false;
    lampOn = false;
    for( i = 0; i < NUMCHANS; ++i )
    {
        ctlP = chans[i];

        if( atomic_load_explicit(&(ctlP->lampWanted), memory_order_relaxed) )
        {
            lamp = atomic_exchange(&(ctlP->lampWanted), 0);
            if( lamp > ctlP->onCount )
            {
                ctlP->onCount = lamp;
            }
        }

        if( serviceChannel(ctlP, !steal) )
        {
            steal = true;
        }

        if( ctlP->onCount > 0 )
        {
            --(ctlP->onCount);
            lampOn = true;
        }
    }

    // The lamp state is only recorded here; main.c and cycle() snapshot and tally it.
    pdp1P->hsc = (lampOn) ? 1 : 0;
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
        sem_init(&(ctlP->waitSemaphore), 0, 0);
        atomic_store(&(ctlP->status), HSC_OK);
        ctlP->isInitialized = true;
    }

    ctlP->isAssigned = true;
    atomic_store(&(ctlP->isWaiting), 0);
    ctlP->waitDelay = 0;

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

// Emulator says to stop everything in progress. Called on the emulator thread, so it takes no
// lock: a transfer stops at the word it had reached, and anyone blocked in HSCwait() is woken.
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
            atomic_store(&(ctlP->scanKind), SCAN_IDLE);
            atomic_store(&(ctlP->owed), 0);
            atomic_store(&(ctlP->lampWanted), 0);
            ctlP->onCount = 0;
            ctlP->wordsDue = 0;
            atomic_store(&(ctlP->status), HSC_ABORT);
            wakeWaiter(ctlP);
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
int ticks;

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

    if( atomic_load(&(ctlP->status)) == HSC_BUSY )
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

    // IMMEDIATE moves the data now and steals nothing.
    // THREADED moves the data now, owes the CPU one cycle per word, and HSCwait() times the
    // 5us per word in the requester's own thread.
    // TRUESTEAL, for a device slower than memory (wordTime > 50), spreads one steal per word
    // evenly across the transfer's simtime duration and moves each word at its steal.
    switch( rqstP->mode & (HSC_MODE_IMMEDIATE | HSC_MODE_THREADED | HSC_MODE_TRUESTEAL) )
    {
    case HSC_MODE_IMMEDIATE:
        logger(LOG_HSC, "request_channel immediate transfer\n");
        processImmediate(rqstP);
        atomic_store(&(ctlP->status), HSC_DONE);
        if( rqstP->mode & HSC_MODE_UPDATEPANEL )
        {
            requestLamp(ctlP, rqstP->count);
        }
        return( HSC_OK );

    case HSC_MODE_THREADED:
        logger(LOG_HSC, "request_channel threaded transfer\n");
        processImmediate(rqstP);
        ctlP->waitDelay = rqstP->count * 5;       // 5us per word
        atomic_store(&(ctlP->status), HSC_BUSY);

        // A stopped CPU has no cycles to lose, and the next start resets the channels anyway.
        if( pdp1P->run )
        {
            atomic_fetch_add(&(ctlP->owed), rqstP->count);
        }

        if( rqstP->mode & HSC_MODE_UPDATEPANEL )
        {
            requestLamp(ctlP, rqstP->count);
        }
        return(HSC_BUSY);

    case HSC_MODE_TRUESTEAL:
        logger(LOG_HSC, "request_channel TRUESTEAL transfer\n");
        if( rqstP->wordTime < 50 )
        {
            // This mode is for devices slower than one memory cycle, 5us, such as the drum.
            // A device at or faster than memory speed should use THREADED instead.
            logger(LOG_HSC, "request_channel TRUESTEAL wordTime %d too fast for TRUESTEAL\n",
                rqstP->wordTime);
            return( HSC_ERR );
        }

        memcpy(&(ctlP->request), rqstP, sizeof(HSCRequest));

        // Round count*word-transfer-time (in us/10) to the nearest whole 5us tick,
        // never fewer ticks than words.
        ticks = ((rqstP->count * rqstP->wordTime) + 25) / 50;
        if( ticks < rqstP->count )
        {
            ticks = rqstP->count;
        }

        ctlP->ticksTotal = ticks;
        ctlP->ticksDone = 0;
        ctlP->wordsMade = 0;
        ctlP->wordsDue = 0;
        ctlP->started = false;
        logger(LOG_HSC, "TRUESTEAL ticks %d, steals %d\n", ticks, rqstP->count);

        if( rqstP->mode & HSC_MODE_UPDATEPANEL )
        {
            requestLamp(ctlP, ticks);
        }

        // Unlike THREADED, this channel stays busy for the transfer's total tick count,
        // as the original hardware would do.
        atomic_store(&(ctlP->status), HSC_BUSY);
        atomic_store_explicit(&(ctlP->scanKind), SCAN_TRUESTEAL, memory_order_release);
        return(HSC_BUSY);

    case 0:
        break;              // normal mode, below

    default:
        logger(LOG_HSC, "request_channel illegal request\n");
        return( HSC_ERR );  // can't have both
    }

    // Normal mode: one word per stolen cycle, the lamp lit for the transfer.
    memcpy(&(ctlP->request), rqstP, sizeof(HSCRequest));
    logger(LOG_EXEC, "channel %d set to BUSY, addr %d:%o\n", chanP->chanNo+1, rqstP->memBank, rqstP->memAddr);
    requestLamp(ctlP, rqstP->count);
    atomic_store(&(ctlP->status), HSC_BUSY);
    atomic_store_explicit(&(ctlP->scanKind), SCAN_NORMAL, memory_order_release);
    return( HSC_BUSY );
}

// Charge count memory cycles to the CPU without moving any data, as if count words had been
// fetched through the channel now. For a device that prefetches with IMMEDIATE and uses the
// words later, such as the 340's cache. Lights the lamp as a THREADED fetch would.
// Returns HSC_OK, or HSC_ERR for an invalid or unassigned channel or a negative count.
int
HSCsteal(HSCChannelP chanP, int count)
{
HSCControlP ctlP;

    if( !(ctlP = getControlP(chanP)) || !ctlP->isAssigned || (count < 0) )
    {
        return( HSC_ERR );
    }

    if( pdp1P->run )
    {
        atomic_fetch_add(&(ctlP->owed), count);
    }

    requestLamp(ctlP, count);
    return( HSC_OK );
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
// Returns HSC_DONE when the transfer finished (or none was running and the last one finished),
// HSC_ABORT if an HSCreset() stopped it, HSC_OK if the channel never ran a transfer, and
// HSC_ERR if the chanP is invalid.
int
HSCwait(HSCChannelP chanP)
{
int status;
int expected;
HSCControlP ctlP;

    if( !(ctlP = getControlP(chanP)) )
    {
        return( HSC_ERR );
    }

    // Emulator said to stop any ongoing transfers
    if( atomic_load(&(ctlP->status)) == HSC_ABORT )
    {
        ctlP->waitDelay = 0;
        atomic_store(&(ctlP->status), HSC_DONE);
        return( HSC_ABORT );
    }

    // THREADED: the data has moved and the cycles are owed, what is left is the requester's
    // own 5us per word.
    if( ctlP->waitDelay > 0 )
    {
        // We aren't necessarily in the same thread as the main emulator,
        // just idle if it isn't in run state.
        while( !pdp1P->run )
        {
            usleep(100);
        }

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

        // An HSCreset() during the delay leaves HSC_ABORT, which is reported once.
        expected = HSC_BUSY;
        if( atomic_compare_exchange_strong(&(ctlP->status), &expected, HSC_DONE) )
        {
            return(HSC_DONE);
        }

        atomic_store(&(ctlP->status), HSC_DONE);
        return( (expected == HSC_ABORT) ? HSC_ABORT : HSC_DONE );
    }

    if( atomic_load(&(ctlP->status)) != HSC_BUSY )
    {
        return( atomic_load(&(ctlP->status)) );
    }

    // Announce the wait, then look again: completeChannel() and HSCreset() store the status
    // before they take isWaiting, so either we see their status here or they see our flag.
    // Whoever takes isWaiting back owns the wake-up; if it was them, their post is coming.
    atomic_store(&(ctlP->isWaiting), 1);
    if( atomic_load(&(ctlP->status)) != HSC_BUSY )
    {
        if( atomic_exchange(&(ctlP->isWaiting), 0) )
        {
            return( atomic_load(&(ctlP->status)) );
        }
    }

    sem_wait(&(ctlP->waitSemaphore));
    status = atomic_load(&(ctlP->status));
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

    return( atomic_load(&(ctlP->status)) );
}

// Finish a normal or TRUESTEAL transfer, on the emulator thread.
static void
completeChannel(HSCControlP ctlP)
{
    logger(LOG_HSC, "channel marking DONE\n");
    atomic_store(&(ctlP->scanKind), SCAN_IDLE);
    atomic_store(&(ctlP->status), HSC_DONE);
    wakeWaiter(ctlP);
}

// Post the waiter, if there is one and it has not already withdrawn. The status must be
// stored before this is called; see HSCwait().
static void
wakeWaiter(HSCControlP ctlP)
{
    if( atomic_exchange(&(ctlP->isWaiting), 0) )
    {
        sem_post(&(ctlP->waitSemaphore));
    }
}

// Ask the scan to keep the lamp lit for at least passes passes (HSC_STRETCH minimum).
static void
requestLamp(HSCControlP ctlP, int passes)
{
int cur;

    if( passes < HSC_STRETCH )
    {
        passes = HSC_STRETCH;
    }

    cur = atomic_load(&(ctlP->lampWanted));
    while( (passes > cur) && !atomic_compare_exchange_weak(&(ctlP->lampWanted), &cur, passes) )
    {
        ;
    }
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

// Move a whole request at once, in the caller's thread, for IMMEDIATE and THREADED.
// Works on a copy, so the caller's request is left as it was passed.
static void
processImmediate(HSCRequestP rqstP)
{
HSCRequest work;

    memcpy(&work, rqstP, sizeof(HSCRequest));

#ifdef DOLOGGING
    if( work.mode & HSC_MODE_FROMMEM )
    {
        logger(LOG_DATA,"Transfer %d words from core addr %06o\n",
            work.count, (work.memBank * 4096) + work.memAddr);
    }

    if( work.mode & HSC_MODE_TOMEM )
    {
        logger(LOG_DATA,"Transfer %d words to core addr %06o\n",
            work.count, (work.memBank * 4096) + work.memAddr);
    }
#endif

    while( work.count-- > 0 )
    {
        moveWord(&work);
    }
}

// Move one word between core and the request's buffers and advance the request's address
// and buffer pointers; the count is the caller's business.
// A read from memory comes before a write to memory, same as the original hardware.
static void
moveWord(HSCRequestP rqstP)
{
uint32_t fullAddr;
uint32_t data;

    // We wrap within the bank
    if( rqstP->memAddr > 4095 )
    {
        rqstP->memAddr = 0;
    }

    fullAddr = (rqstP->memBank * 4096) + rqstP->memAddr;

    if( rqstP->mode & HSC_MODE_FROMMEM )
    {
        data = pdp1P->core[fullAddr];
        *(rqstP->fromBufferP++) = data & 0777777;   // just for cleanliness
        logger(LOG_DATA,"%06o from core %o\n", data, fullAddr);
    }

    if( rqstP->mode & HSC_MODE_TOMEM )
    {
        data = *(rqstP->toBufferP++) & 0777777;
        logger(LOG_DATA,"%06o to core %o\n", data, fullAddr);
        pdp1P->core[fullAddr] = data;
    }

    ++(rqstP->memAddr);
}

// Run a TRUESTEAL transfer's spreading up to the ticks simtime says are due, making words due.
// Ticks come from simtime, not from passes, so a mul or div that spans several memory cycles
// in one pass makes the words it covered due at once, to be stolen right after it, as the
// hardware held its breaks until the instruction finished (F25; F17 3-31).
// The spreading is computed, not accumulated: after t of the transfer's ticks, t*count/ticks
// words (rounded down) are due. That is at most one new word a tick, since count <= ticks, and
// exactly count at the last tick. The running accumulator this replaced made one word too
// many on about a quarter of transfers (7 words over 12 ticks, for one), which moved a word
// past the end of the block.
static void
advanceTrueSteal(HSCControlP ctlP)
{
uint64_t due;
int made;

    // The clock starts on the first pass that sees the request, and that pass is its first
    // tick, so a transfer of n ticks is over on its nth pass. simtime is only read here, on
    // the emulator thread.
    if( !ctlP->started )
    {
        ctlP->started = true;
        ctlP->startSim = pdp1P->simtime - HSC_CYCLE_NS;
        ctlP->capSeen = throttleCapFirings;
    }

    due = (pdp1P->simtime - ctlP->startSim) / HSC_CYCLE_NS;
    if( due > (uint64_t)ctlP->ticksTotal )
    {
        due = (uint64_t)ctlP->ticksTotal;
    }

    // due > ticksDone >= 0 here, and due <= ticksTotal, so ticksTotal is not 0.
    if( (uint64_t)ctlP->ticksDone < due )
    {
        ctlP->ticksDone = (int)due;
        made = (int)(((uint64_t)ctlP->ticksDone * (uint64_t)ctlP->request.count) / (uint64_t)ctlP->ticksTotal);
        ctlP->wordsDue += made - ctlP->wordsMade;
        ctlP->wordsMade = made;
    }

    // A throttle lag-cap firing moved simtime forward over time the CPU never ran (pdp1.c,
    // throttle()). The words that fell due in it move now without a steal: the cycles they
    // would have taken are part of the forgiven time.
    if( throttleCapFirings != ctlP->capSeen )
    {
        ctlP->capSeen = throttleCapFirings;
        while( ctlP->wordsDue > 0 )
        {
            moveWord(&(ctlP->request));
            --(ctlP->wordsDue);
        }
    }
}

// Service one channel for this pass. mayTake is false once a higher-priority channel has
// taken the cycle; the channel then only keeps its bookkeeping and may still complete.
// Returns true if this channel took the pass's memory cycle, else false.
static bool
serviceChannel(HSCControlP ctlP, bool mayTake)
{
int kind;
bool took;

    took = false;

    // Cycles already owed by a THREADED fetch or HSCsteal() go first: their words have moved.
    if( mayTake && (atomic_load_explicit(&(ctlP->owed), memory_order_relaxed) > 0) )
    {
        atomic_fetch_sub(&(ctlP->owed), 1);
        took = true;
    }

    // The acquire pairs with HSCexecute()'s release, so the request is complete when seen.
    kind = atomic_load_explicit(&(ctlP->scanKind), memory_order_acquire);
    if( kind == SCAN_NORMAL )
    {
        if( !took && mayTake && (ctlP->request.count > 0) )
        {
            moveWord(&(ctlP->request));
            --(ctlP->request.count);
            took = true;
        }

        if( ctlP->request.count <= 0 )
        {
            completeChannel(ctlP);
        }
    }
    else if( kind == SCAN_TRUESTEAL )
    {
        advanceTrueSteal(ctlP);

        if( !took && mayTake && (ctlP->wordsDue > 0) )
        {
            moveWord(&(ctlP->request));
            --(ctlP->wordsDue);
            took = true;
        }

        if( (ctlP->ticksDone >= ctlP->ticksTotal) && (ctlP->wordsDue == 0) )
        {
            completeChannel(ctlP);
        }
    }

    return(took);
}
