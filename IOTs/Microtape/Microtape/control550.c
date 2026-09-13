/*
 * control550.c -- the DEC Type 550 Microtape Control.
 *
 * This implements a Type 550 with eight Type 555 drives as DEC's documents specify them: the
 * 1963 preliminary manual, the H-550 maintenance manual (1965), the DECUS paper (Hantman,
 * November 1963) and the F-03 brochure (1964).
 *
 * 11-Sep-2026 wje/claude - initial version
 * 13-Sep-2026 claude - write enable, All Halt, the block mark at space 0 and the D256 latch
 *                      (MiscTasks/Completed/TASK-TAPE-HALT-WRITE.md)
 */

#include <string.h>

#include "control550.h"

// Kinds of event runTo() can find due. The numeric order is the tie-break order for events
// of one unit at the same time: a ramp end, then the word boundary, then the end zone.
#define EV_NONE         0
#define EV_SELDIP       1       // selection delay over
#define EV_SEGEND       2       // a start, stop or turnaround ramp ends
#define EV_BOUNDARY     3       // the selected, cruising tape crosses a word boundary
#define EV_ENDZONE      4       // the selected, cruising tape is in the end zone ahead
#define EV_OFFREEL      5       // a deselected, cruising tape leaves its reel

static int effectiveMode(int mode);
static void requestBreak(Mt550P cP);
static void raiseFlag(Mt550P cP, bool blockEnd, bool load, uint32_t word);
static void setError(Mt550P cP, bool *conditionP);
static void clearErrors(Mt550P cP);
static void resetWritePipeline(Mt550P cP);
static Mt555UnitP selectedUnit(Mt550P cP);
static bool flagsEnabled(Mt550P cP, Mt555UnitP uP);
static int dirSlot(int dir, int slot);
static int headSlot(Mt555UnitP uP);
static bool atLastWord(Mt550P cP);
static void applyControl(Mt550P cP, uint64_t now, uint32_t io);
static void applyHeldControl(Mt550P cP, uint64_t now);
static uint32_t tapeWord(Mt555UnitP uP, int block, int r);
static void putTapeWord(Mt555UnitP uP, int block, int r, uint32_t word);
static uint64_t unitNextEvent(Mt550P cP, int unit, int *kindP);
static void runTo(Mt550P cP, uint64_t now);
static void handleEvent(Mt550P cP, int kind, int unit, uint64_t t);
static void onBoundary(Mt550P cP, Mt555UnitP uP, int64_t slot, uint64_t t);
static void raisePendingWriteFlag(Mt550P cP);
static void goOffReel(Mt550P cP, int unit, uint64_t t);

// Puts the control and all eight drives into their power-on state: nothing selected, no
// flags, mode 0, no tapes mounted. breakFnP (may be NULL) is called for every program break
// request, with breakCtxP.
// No return value.
void
mt550Init(Mt550P cP, Mt550BreakFnP breakFnP, void *breakCtxP)
{
int unit;

    memset(cP, 0, sizeof(*cP));
    for( unit = 0; unit <= MT_UNITS; ++unit )
    {
        mt555Init(&cP->units[unit]);
    }

    cP->breakFnP = breakFnP;
    cP->breakCtxP = breakCtxP;
    cP->nextEvent = 0;          // forces the first service call to look for work
}

// Brings the control and every drive up to time now, processing every event due by then in
// time order. Called from iotIOPoll() every main-loop iteration, and by every IOT entry point
// before it acts. A time earlier than the last one serviced is treated as no time passing.
// No return value.
void
mt550Service(Mt550P cP, uint64_t now)
{
    if( now < cP->lastTime )
    {
        now = cP->lastTime;
    }

    if( now < cP->nextEvent )
    {
        cP->lastTime = now;     // nothing can happen yet: the common case, one comparison
        return;
    }

    runTo(cP, now);
}

// ---- The five IOTs -------------------------------------------------------------------------------

// mse, 720301: SELECT. Connects the unit in IO bits 2-5 (1-7, and 010 for 8; anything else
// selects nothing). Clears the data, block end and error flags and with them MISS, END, MTE
// and UNABLE (manual p. 2-6 note). A unit that was selected keeps whatever motion it had,
// deselected: it is no longer watched and can run off its reel (p. 1-11). Changing the
// selection starts the selection delay, during which no flags are raised. An mlc held for a
// checksum (see mt550LoadControl()) is carried out first, on the unit it was given for; that
// unit is no longer watched, so its checksum is not written.
// No return value.
void
mt550Select(Mt550P cP, uint64_t now, uint32_t io)
{
int unit;

    mt550Service(cP, now);

    unit = (int)((io >> MT_SEL_SHIFT) & MT_SEL_MASK);
    if( (unit < 1) || (unit > MT_UNITS) )
    {
        unit = 0;
    }

    clearErrors(cP);

    if( unit != cP->selUnit )
    {
        applyHeldControl(cP, now);

        if( cP->selUnit )
        {
            mt555Flush(&cP->units[cP->selUnit]);
        }

        cP->selUnit = unit;
        resetWritePipeline(cP);
        cP->selDip = (unit != 0);
        cP->selDipEnd = (now + (uint64_t)MT_SELECT_DIP_NS);
    }

    runTo(cP, now);             // recompute the next event for the new selection
}

// mlc, 720401: LOAD CONTROL. Sets go, direction and mode from IO bits 12-17 (manual p. 2-5).
// Clears the same flags as mse, at once. The command is refused -- UNABLE and the error flag
// set, nothing started or changed -- if no unit is selected, it has no tape, its tape has left
// the reel, or it asks to write or erase on a write-locked unit. Modes 4-6 act as move.
// Mode 7 erases the reel to a blank tape and then acts as move. Go with write mode sets
// WRITE ENABLE; any other command clears it.
//
// The D256 latch: a command given while writing, after the control has asked for the last
// data word and before the trailing checksum is on the tape, is held and carried out as the
// checksum is written, so a program may change mode or stop straight after the checksum's
// mwr ("Change of mode commanded at last Data word (D256) is delayed while Check Sum is
// written", DECUS 1963 p. B-2, brochure F-03 p. 10). The flags are still cleared at once. A
// second command in that window replaces the first.
// No return value.
void
mt550LoadControl(Mt550P cP, uint64_t now, uint32_t io)
{
    mt550Service(cP, now);
    clearErrors(cP);

    if( atLastWord(cP) )
    {
        cP->ctlHeld = true;
        cP->heldCtl = io;
    }
    else
    {
        applyControl(cP, now, io);
    }

    runTo(cP, now);
}

// mrd, 720501: READ. Returns the buffer (the plugin puts it in IO, which the IOT clears
// first) and clears the data and block end flags.
uint32_t
mt550ReadBuffer(Mt550P cP, uint64_t now)
{
    mt550Service(cP, now);
    cP->df = false;
    cP->bef = false;
    return( cP->buffer & MT_WORDMASK );
}

// mwr, 720601: WRITE. Loads the buffer from IO and clears the data and block end flags.
// No return value.
void
mt550WriteBuffer(Mt550P cP, uint64_t now, uint32_t io)
{
    mt550Service(cP, now);
    cP->buffer = (io & MT_WORDMASK);
    cP->df = false;
    cP->bef = false;
}

// mrs, 720701: READ STATUS. Returns the status word for IO: the three flags in bits 0-2, then
// END, MISS, REV, GO, MTE and UNABLE in bits 3-8 (manual p. 2-6), bits 9-17 zero. REV and GO
// are the selected transport's latched motion commands. Clears nothing.
uint32_t
mt550Status(Mt550P cP, uint64_t now)
{
Mt555UnitP uP;
uint32_t status;

    mt550Service(cP, now);
    uP = selectedUnit(cP);

    status = 0;
    status |= (cP->df ? MT_ST_DF : 0);
    status |= (cP->bef ? MT_ST_BEF : 0);
    status |= (cP->erf ? MT_ST_ERF : 0);
    status |= (cP->end ? MT_ST_END : 0);
    status |= (cP->miss ? MT_ST_MISS : 0);
    status |= ((uP && uP->revCmd) ? MT_ST_REV : 0);
    status |= ((uP && uP->goCmd) ? MT_ST_GO : 0);
    status |= (cP->mte ? MT_ST_MTE : 0);
    status |= (cP->unable ? MT_ST_UNABLE : 0);
    return(status);
}

// ---- Housekeeping ----------------------------------------------------------------------------

// Returns drive unit (1-8), or NULL for any other number.
Mt555UnitP
mt550Unit(Mt550P cP, int unit)
{
    if( (unit < 1) || (unit > MT_UNITS) )
    {
        return(NULL);
    }

    return( &cP->units[unit] );
}

// Tells the control that the plugin has mounted, unmounted or replaced the reel on a unit
// (from microtapes.txt, a SIGHUP remount, or the mount IOT 201). If that unit is selected, a
// write in progress is abandoned.
// No return value.
void
mt550UnitRemounted(Mt550P cP, int unit, uint64_t now)
{
    if( unit == cP->selUnit )
    {
        resetWritePipeline(cP);
    }

    runTo(cP, now);
}

// Writes any unflushed block on every drive back to its image file.
// No return value.
void
mt550FlushAll(Mt550P cP)
{
int unit;

    for( unit = 1; unit <= MT_UNITS; ++unit )
    {
        mt555Flush(&cP->units[unit]);
    }
}

// All Halt (H-550 p. 2-17): the transports' go relays hold only while the computer's RUN
// flip-flop is 1, and RUN at 0 also clears the control's GO flip-flop, so when the computer
// stops, every moving drive stops -- selected or not, a deselected one heading off its reel
// included -- and stays stopped until the program gives an mlc with go. The plugin calls this
// when it sees RUN fall, at time now. Brings the control up to now, commands a stop on every
// moving drive (which clears its latched go, so GO reads 0), writes back a partly written
// block, switches the writers off, and forgets any write in progress, an mlc held for a
// checksum with it: the tape stops short of that checksum. The mode, the direction and the
// flags and errors are left as they were.
// No return value.
void
mt550AllHalt(Mt550P cP, uint64_t now)
{
Mt555UnitP uP;
int unit;

    mt550Service(cP, now);

    for( unit = 1; unit <= MT_UNITS; ++unit )
    {
        uP = &cP->units[unit];
        if( uP->mounted && !uP->offReel && (uP->motion != MT_STOPPED) )
        {
            mt555Command(uP, now, false, uP->revCmd);   // a turnaround in progress just stops
        }
    }

    if( (uP = selectedUnit(cP)) != NULL )
    {
        mt555Flush(uP);         // only the selected drive can hold an unflushed block
    }

    cP->wren = false;
    resetWritePipeline(cP);
    runTo(cP, now);
}

// ---- Internals -------------------------------------------------------------------------------

// Returns the mode as the control acts on it: 0-3 as given, 4-7 treated as move (mode 7's
// erase is done once, by mt550LoadControl(), before the tape moves).
static int
effectiveMode(int mode)
{
    return( ((mode >= 0) && (mode <= MT_MODE_WRITE)) ? mode : MT_MODE_MOVE );
}

// Counts and forwards one program break request.
// No return value.
static void
requestBreak(Mt550P cP)
{
    ++cP->breakCount;
    if( cP->breakFnP )
    {
        cP->breakFnP(cP->breakCtxP);
    }
}

// Raises the data flag (blockEnd false) or the block end flag (blockEnd true), optionally
// loading the buffer with word first. If either flag is still up from the previous request
// the program has missed it: MISS and the error flag are set (p. 3-9, "Should the computer
// fail to recognize the data flag before the next raise data flag pulse is issued"; H-550
// p. 2-31), and the new flag is raised anyway. Every raise requests a break.
// No return value.
static void
raiseFlag(Mt550P cP, bool blockEnd, bool load, uint32_t word)
{
    if( cP->df || cP->bef )
    {
        setError(cP, &cP->miss);
    }

    if( load )
    {
        cP->buffer = (word & MT_WORDMASK);
    }

    if( blockEnd )
    {
        cP->bef = true;
    }
    else
    {
        cP->df = true;
    }

    requestBreak(cP);
}

// Sets one error condition (*conditionP: MISS, END or UNABLE) and the error flag. The error
// conditions are ORed to hold the WRITE ENABLE flip-flop at 0 (H-550 p. 2-32), so once an
// error is found nothing more is written until it is cleared and write mode is commanded
// again ("Program too slow stops writing operation", DEC's Control Type 550 summary). The
// caller requests the break.
// No return value.
static void
setError(Mt550P cP, bool *conditionP)
{
    *conditionP = true;
    cP->erf = true;
    cP->wren = false;
}

// Clears the three flags and the error conditions they cover, as mse and mlc do.
// No return value.
static void
clearErrors(Mt550P cP)
{
    cP->df = false;
    cP->bef = false;
    cP->erf = false;
    cP->end = false;
    cP->miss = false;
    cP->mte = false;
    cP->unable = false;
}

// Forgets any write in progress: no word requested, no boundary to skip, no data flag
// waiting for the tape to reach speed, no mlc held for a checksum.
// No return value.
static void
resetWritePipeline(Mt550P cP)
{
    cP->writeReqOutstanding = false;
    cP->writeSkip = false;
    cP->writeDFPending = false;
    cP->ctlHeld = false;
}

// Returns the selected drive, or NULL if none is selected.
static Mt555UnitP
selectedUnit(Mt550P cP)
{
    return( cP->selUnit ? &cP->units[cP->selUnit] : NULL );
}

// Returns true if the control can raise flags for drive uP right now: it is the selected
// drive, it has a tape, it is at speed (starts and turnarounds are "delay in progress", and a
// stopping tape has its go flip-flop clear, p. 3-3), and no selection delay is running.
static bool
flagsEnabled(Mt550P cP, Mt555UnitP uP)
{
    return( (uP != NULL) && (uP == selectedUnit(cP)) && uP->mounted && !uP->offReel
        && (uP->motion == MT_CRUISE) && !cP->selDip );
}

// Returns the slot number counted in the direction of motion for physical slot slot.
static int
dirSlot(int dir, int slot)
{
    return( (dir > 0) ? slot : ((MT_SLOTS_PER_BLOCK - 1) - slot) );
}

// Returns the direction-relative slot (0-263) the head of the selected unit uP is passing now
// -- the one its next boundary ends -- or -1 in an end zone. Only meaningful while the control
// is taking that unit's boundaries (a mode other than move, flags enabled), which keeps the
// boundary pointer current.
static int
headSlot(Mt555UnitP uP)
{
int64_t slot;

    slot = ((uP->dir > 0) ? (uP->nextBoundary - 1) : uP->nextBoundary);
    if( (slot < 0) || (slot >= MT_TOTAL_SLOTS) )
    {
        return(-1);
    }

    return( dirSlot(uP->dir, (int)(slot % MT_SLOTS_PER_BLOCK)) );
}

// Returns true if the control is inside the D256 window: writing, with the last data word
// asked for (its data flag comes at the end of slot 257) and the trailing checksum not yet on
// the tape (it is taken at the end of slot 259). The head is then in slot 258 or 259.
static bool
atLastWord(Mt550P cP)
{
Mt555UnitP uP;
int r;

    uP = selectedUnit(cP);
    if( !cP->wren || (effectiveMode(cP->mode) != MT_MODE_WRITE) || !flagsEnabled(cP, uP) )
    {
        return(false);
    }

    r = headSlot(uP);
    return( (r == MT_SLOT_PREFINAL) || (r == MT_SLOT_FINAL) );
}

// Carries out a LOAD CONTROL word io at time now, as mt550LoadControl() describes, for a
// command given then or held until then. The caller has cleared the flags and brought the
// control up to now, and recomputes the next event afterwards (this is also called from
// inside an event, so it must not call runTo()).
// No return value.
static void
applyControl(Mt550P cP, uint64_t now, uint32_t io)
{
Mt555UnitP uP;
int newMode;
int prevMode;
int r;
bool skip;
bool erase;

    uP = selectedUnit(cP);
    newMode = effectiveMode((int)(io & MT_CTL_MODE));
    erase = ((int)(io & MT_CTL_MODE) == MT_MODE_ERASE);

    if( !uP || !uP->mounted || uP->offReel || (((newMode == MT_MODE_WRITE) || erase) && uP->locked) )
    {
        setError(cP, &cP->unable);
        requestBreak(cP);
        return;
    }

    prevMode = effectiveMode(cP->mode);
    mt555Flush(uP);             // any mode change or stop ends a block being written

    if( erase )
    {
        mt555Erase(uP);         // a failure to truncate the file shows up as ioError
    }

    // Read -> write while the head is inside a block's words costs one boundary (p. 3-10:
    // two word spaces are lost). Decide from the slot the head is passing now, before the
    // new motion bits take effect.
    skip = false;
    if( (newMode == MT_MODE_WRITE) && (prevMode == MT_MODE_READ) && flagsEnabled(cP, uP) )
    {
        r = headSlot(uP);
        skip = ((r >= MT_SLOT_REVCHECK) && (r <= MT_SLOT_FINAL));
    }

    cP->mode = (int)(io & MT_CTL_MODE);
    mt555Command(uP, now, ((io & MT_CTL_GO) != 0), ((io & MT_CTL_REV) != 0));
    mt555SyncBoundary(uP, now);
    resetWritePipeline(cP);

    // WRITE ENABLE: set by go with write mode, cleared by anything else. A held command can
    // meet an error found since it was given (a checksum missed); errors hold it at 0.
    cP->wren = (((io & MT_CTL_GO) != 0) && ((int)(io & MT_CTL_MODE) == MT_MODE_WRITE)
        && !(cP->end || cP->miss || cP->mte || cP->unable));

    if( newMode == MT_MODE_WRITE )
    {
        // "The commanding of the write mode ... results in the issuance of a data flag"
        // (p. 3-9). Below speed it waits until the tape gets there.
        if( flagsEnabled(cP, uP) )
        {
            cP->writeSkip = skip;
            raiseFlag(cP, false, false, 0);
            cP->writeReqOutstanding = true;
        }
        else
        {
            cP->writeDFPending = true;
        }
    }
}

// Carries out, at time now, an mlc held by the D256 latch, if there is one.
// No return value.
static void
applyHeldControl(Mt550P cP, uint64_t now)
{
    if( cP->ctlHeld )
    {
        cP->ctlHeld = false;
        applyControl(cP, now, cP->heldCtl);
    }
}

// Returns the stored word at direction-relative slot r (3-260) of block. Physical slot p
// holds stored word p - 3; dirSlot() is its own inverse, so it also maps r back to p.
static uint32_t
tapeWord(Mt555UnitP uP, int block, int r)
{
    return( mt555GetWord(uP, block, (dirSlot(uP->dir, r) - MT_SLOT_REVCHECK)) );
}

// Stores word at direction-relative slot r (3-260) of block.
// No return value.
static void
putTapeWord(Mt555UnitP uP, int block, int r, uint32_t word)
{
    mt555PutWord(uP, block, (dirSlot(uP->dir, r) - MT_SLOT_REVCHECK), word);
}

// Returns the time of the next event for one drive, and its kind in *kindP (EV_NONE and
// UINT64_MAX if nothing is scheduled). A drive with no tape, or off its reel, does nothing.
static uint64_t
unitNextEvent(Mt550P cP, int unit, int *kindP)
{
Mt555UnitP uP;
uint64_t best;
uint64_t t;
int64_t ahead;

    uP = &cP->units[unit];
    best = UINT64_MAX;
    *kindP = EV_NONE;

    if( !uP->mounted || uP->offReel )
    {
        return(best);
    }

    switch( uP->motion )
    {
    case MT_ACCEL:
    case MT_DECEL:
        best = mt555SegmentEnd(uP);
        *kindP = EV_SEGEND;
        break;

    case MT_CRUISE:
        if( unit == cP->selUnit )
        {
            if( cP->selDip )
            {
                break;          // nothing is sensed until the selection delay ends
            }

            if( effectiveMode(cP->mode) != MT_MODE_MOVE )
            {
                best = mt555BoundaryTime(uP);
                *kindP = EV_BOUNDARY;
            }

            // The end mark ahead: only the end the tape would come off (p. 1-10).
            ahead = ((uP->dir > 0) ? MT_FEZ : MT_BA);
            t = mt555CrossTime(uP, ahead);
            if( t < best )
            {
                best = t;
                *kindP = EV_ENDZONE;
            }
        }
        else
        {
            // Deselected: nobody watches the end marks, so it runs until it leaves the reel.
            best = mt555CrossTime(uP, ((uP->dir > 0) ? MT_TAPE_NS : 0));
            *kindP = EV_OFFREEL;
        }
        break;

    default:
        break;
    }

    return(best);
}

// Processes, in time order, every event due at or before now, then records the time of the
// next one so mt550Service() can skip calls until then. On return lastTime is now.
// No return value.
static void
runTo(Mt550P cP, uint64_t now)
{
uint64_t best;
uint64_t t;
int bestKind;
int bestUnit;
int kind;
int unit;

    for( ;; )
    {
        best = UINT64_MAX;
        bestKind = EV_NONE;
        bestUnit = 0;

        if( cP->selDip )
        {
            best = cP->selDipEnd;
            bestKind = EV_SELDIP;
        }

        for( unit = 1; unit <= MT_UNITS; ++unit )
        {
            t = unitNextEvent(cP, unit, &kind);
            if( t < best )
            {
                best = t;
                bestKind = kind;
                bestUnit = unit;
            }
        }

        if( (bestKind == EV_NONE) || (best > now) )
        {
            cP->nextEvent = best;
            break;
        }

        // An event can be found overdue: a tape selected while already in the end zone
        // ahead meets the end mark as soon as the selection delay ends, not in the past.
        // Events are carried out in time order, never before one already processed.
        if( best < cP->lastTime )
        {
            best = cP->lastTime;
        }

        handleEvent(cP, bestKind, bestUnit, best);
        cP->lastTime = best;
    }

    if( now > cP->lastTime )
    {
        cP->lastTime = now;
    }
}

// Carries out one event of the given kind for drive unit at time t.
// No return value.
static void
handleEvent(Mt550P cP, int kind, int unit, uint64_t t)
{
Mt555UnitP uP;
int64_t pos;

    uP = ((unit > 0) ? &cP->units[unit] : NULL);

    switch( kind )
    {
    case EV_SELDIP:
        cP->selDip = false;
        uP = selectedUnit(cP);
        if( uP )
        {
            mt555SyncBoundary(uP, t);       // start watching from where the head is now
        }
        raisePendingWriteFlag(cP);
        break;

    case EV_SEGEND:
        mt555EndSegment(uP);
        pos = mt555Position(uP, t);
        if( (pos < 0) || (pos > MT_TAPE_NS) )
        {
            goOffReel(cP, unit, t);         // came to rest (or turned) past the end of the tape
        }
        else if( unit == cP->selUnit )
        {
            raisePendingWriteFlag(cP);      // reached speed with a write waiting
        }
        break;

    case EV_BOUNDARY:
        onBoundary(cP, uP, mt555TakeBoundary(uP), t);
        break;

    case EV_ENDZONE:
        // "Sensing of the appropriate end mark stops the tape and raises the error flag if
        // the tape is in any of the normal modes" (p. 1-10). END switches the writers off.
        mt555Flush(uP);
        setError(cP, &cP->end);
        requestBreak(cP);
        mt555EndZoneStop(uP, t);
        break;

    case EV_OFFREEL:
        goOffReel(cP, unit, t);
        break;

    default:
        break;
    }
}

// Carries out what the control does as the selected, cruising tape's head finishes passing
// absolute word slot slot, at time t. Counted in the direction of motion (r):
//   search  at the end of the block mark (0): DF, the buffer holds the search word
//   read    at the end of 3-259: DF with the word just read; at the end of 260: BEF with it
//   write   at the end of the lock (2): -0 goes to 3, DF asks for data word 1 (unless entering
//           write already asked); at the end of 3-259: the buffer goes to the next space (the
//           interchange), then DF asks for the word two spaces on, BEF at the end of 258 for the
//           checksum, and at the end of 259 the block is done
// No return value.
static void
onBoundary(Mt550P cP, Mt555UnitP uP, int64_t slot, uint64_t t)
{
int block;
int r;
bool missed;

    if( (slot < 0) || (slot >= MT_TOTAL_SLOTS) )
    {
        return;                             // end zone: no marks but the end marks
    }

    block = (int)(slot / MT_SLOTS_PER_BLOCK);
    r = dirSlot(uP->dir, (int)(slot % MT_SLOTS_PER_BLOCK));

    switch( effectiveMode(cP->mode) )
    {
    case MT_MODE_SEARCH:
        if( r == MT_SLOT_BLOCKMARK )
        {
            raiseFlag(cP, false, true, (((uP->dir > 0) ? MT_MARK_FWD : MT_MARK_REV) | (uint32_t)block));
        }
        break;

    case MT_MODE_READ:
        if( (r >= MT_SLOT_REVCHECK) && (r <= MT_SLOT_FINAL) )
        {
            raiseFlag(cP, false, true, tapeWord(uP, block, r));
        }
        else if( r == MT_SLOT_CHECK )
        {
            raiseFlag(cP, true, true, tapeWord(uP, block, r));
        }
        break;

    case MT_MODE_WRITE:
        if( r == MT_SLOT_LOCK )
        {
            // The control writes the leading checksum, -0, itself (p. 1-8), and asks for the
            // first data word -- unless entering write already asked for it. A write entered
            // after the lock has passed gets no -0: space 3 keeps what the tape held.
            if( cP->wren )
            {
                putTapeWord(uP, block, MT_SLOT_REVCHECK, MT_MINUS_ZERO);
            }

            if( !cP->writeReqOutstanding )
            {
                raiseFlag(cP, false, false, 0);
                cP->writeReqOutstanding = true;
            }
        }
        else if( (r >= MT_SLOT_REVCHECK) && (r <= MT_SLOT_FINAL) )
        {
            if( cP->writeSkip )
            {
                cP->writeSkip = false;      // read -> write: this word space is lost
            }
            else
            {
                // Interchange: the buffer goes to the shift register and is written in the
                // slot now coming under the head. A data or block end flag still up means the
                // program never gave that word: MISS, and the writers go off before the stale
                // word can reach the tape. Nothing more is written; the rest of the block
                // keeps what the tape held, and it fails its check.
                missed = (cP->df || cP->bef);
                if( missed )
                {
                    setError(cP, &cP->miss);
                }

                if( cP->wren )
                {
                    putTapeWord(uP, block, (r + 1), cP->buffer);
                }

                cP->writeReqOutstanding = false;

                if( r < MT_SLOT_PREFINAL )
                {
                    raiseFlag(cP, false, false, 0);     // next data word, two slots ahead
                    cP->writeReqOutstanding = true;
                }
                else if( r == MT_SLOT_PREFINAL )
                {
                    raiseFlag(cP, true, false, 0);      // block end: load the trailing checksum
                    cP->writeReqOutstanding = true;
                }
                else
                {
                    mt555Flush(uP);                     // FINAL: checksum written, writers off
                    if( missed )
                    {
                        requestBreak(cP);               // no flag follows to carry the error's break
                    }
                }
            }

            if( r == MT_SLOT_FINAL )
            {
                applyHeldControl(cP, t);    // D256: the checksum is on the tape
            }
        }
        break;

    default:
        break;                              // move: no flags
    }
}

// Raises the data flag a write mode entered below speed was waiting for, if the control can
// now raise flags for the selected drive.
// No return value.
static void
raisePendingWriteFlag(Mt550P cP)
{
    if( cP->writeDFPending && (effectiveMode(cP->mode) == MT_MODE_WRITE) && flagsEnabled(cP, selectedUnit(cP)) )
    {
        cP->writeDFPending = false;
        raiseFlag(cP, false, false, 0);
        cP->writeReqOutstanding = true;
    }
}

// A drive's tape has run past the physical end of the reel: it stops, is unusable until it is
// remounted, and keeps its image. If it was the selected drive (only possible in the abnormal
// modes the manual names, none of which is implemented) the control reports tape unable.
// No return value.
static void
goOffReel(Mt550P cP, int unit, uint64_t t)
{
Mt555UnitP uP;
int64_t pos;

    uP = &cP->units[unit];
    pos = mt555Position(uP, t);
    mt555Flush(uP);
    mt555Park(uP, ((pos < 0) ? 0 : MT_TAPE_NS));
    uP->offReel = true;

    if( unit == cP->selUnit )
    {
        resetWritePipeline(cP);
        setError(cP, &cP->unable);
        requestBreak(cP);
    }
}
