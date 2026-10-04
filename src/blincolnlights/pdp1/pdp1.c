/*
 * This PDP-1 emulator was written originally by Angelo Papenhoff, aap.
 * It has been modified by Bill Ezell, wje, pdp1@quackers.net to:
 * Add new features.
 * Make it readable. :)
 * It uses the One True Formatting Style, keep it.
 * The formatting is based on research done at Stanford many years ago that determined the major causes
 * of coding errors, the formatting reduced that. It works.
 *
 * wje 05-Jan-26 break from original repo, now independent. Initial reformatting.
 *    Prior to this break, dynamic IOTs, new audio, high speed channel support, light pen support,
 *    and other minor changes were done.
 * wje 07-Feb-26 sdb does not draw a dot, only sets position and intensity
 *    Special dpy mod, dpy 3000, sets lightpen enable and the aperture from IO
 * wje 08-Feb-26 rework lightpen, was overthought, now much more accurate
 * wje 10-Feb-26 work around poll() returning data ready when it's not
 * wje 10-Feb-26 style cleanup, remove conditionals for light pen, origin shift, lai, lia
 * wje 22-Feb-26 fix breakage from previous commit
 * wje 18-Mar-26 sorry, not going to use aap's high speed channel, there isn't any real interface to it, other issues
 * wje 19-Mar-26 add additional 1D instructions scf, sci, iif, ifi, ida
 * wje 19-Mar-26 add finer-grained control over 1D instuctions, lailia, core1D, all1D in config file
 * wje 20-Mar-26 resolve conflict between progs that use dpy origin setting and dpy lightpen aperture setting
 *    Note that if both are on, the heuristic might fail, doing both when only one was desired.
 *    This will only happen if a dpy with shift but no wait or completion bits is done.
 *    If a program seems to be doing both, turn one off in the config file, depending upon which one you want active.
 * wje 28-Mar-26 simplify the last one, change the aperture dpy to xx3407, update am1 include file.
 * wje 11-Apr-26 the light pen really doesn't need a listener thread, just use nonblocking reads.
 * wje 12-Apr-26 major rework, move all the display stuff except fd management into IOT 7 and display.c,
 *    doesn't belong here.
 * wje 19-Apr-26 even more major, all display logic is gone from the emulator, including the low-level code
 *    The communication with the external display is now in its own thread and runs completely independently
 *    of the emulator, which is how it should have worked. This allows for much more accurate timing in
 *    the symbol generator and in the Type 340 display.
 * wje 12-Jun-26 detailed commentary added, hopefully accurate.
 * wje 16-Jun-26 many changes so the CHM simple test program displays like the real PDP-1
 * wje/claude 20-Jun-26 many changes to move the typewriter, tape reader, and tape punch into IOTs where
 *   they belong. Bugfixes in original code to fix tyo C hang, nac logic not correct
 *   causing a permanent I/O halt. Fix: pdp->tcp = !!(MB & (B5 | B6)).
 * wje/claude 28-Jun-26 another bug fix in B5/B6 bit handling from original version
 * wje 3-Jul-26 minor cosmetic fix in help message
 * wje 14-Jul-2026 wje some more cleanup done
 * 24-Sep-2026 claude b2 is changed atomically, since the Type 340's thread requests breaks too.
 *   A new reader or punch fd is handed to the emulator thread instead of replacing r_fd/p_fd
 *   from the network thread.
 *   isb now requests the break on the single-channel system too; before, it did nothing there.
 * 27-Sep-2026 Claude the console commands and the RIM loader moved to console.c, and the reader
 *   and punch fd hand-off to papertape.c.
 * 4-Oct-2026 Claude pwrclr() clears run, so a power cycle leaves the machine halted.
*/
#include "common.h"
#include "pdp1.h"
#include "ad1server.h"
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/prctl.h>

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_LP 0
#define LOG_APERTURE 0
#define LOG_DPYSHIFT 0
#define LOG_SDB 0
#define LOG_STARTUP 0
#define LOG_BREAK 0
#define LOG_WATCH 0
#define LOG_1D 0
#define LOG_IOT 0
#define LOG_RIM 0
#define LOG_TYPEWRITER 0

#define NOTIOTH
#include "dynamicIots.h"
#include "configuration.h"

bool audioEnabled = false;
bool lailiaEnabled = false;
bool core1DEnabled = false;
bool all1DEnabled = false;

// Flat core address and contents of the word the current memory cycle's
// readmem() read. writemem() compares against them to skip a write-back
// that would store the same word to the same place (see readmem()).
// File-scope statics rather than PDP1 fields, so pdp1.h and the IOT
// plugin ABI are untouched. Only the CPU thread (cycle()) touches them.
static unsigned int lastReadAddr;
static Word lastReadWord;

// PDP-1 words are 18 bits wide, numbered left to right as bit 0 (sign,
// most significant) through bit 17 (least significant). These masks
// pick out a single bit of an 18-bit word (AC, IO, MB, ...) using the
// machine's own bit numbering, e.g. B5 is "bit 5", not "bit 5 from the LSB".
#define B0 0400000
#define B1 0200000
#define B2 0100000
#define B3 0040000
#define B4 0020000
#define B5 0010000
#define B6 0004000
#define B7 0002000
#define B8 0001000
#define B9 0000400
#define B10 0000200
#define B11 0000100
#define B12 0000040
#define B13 0000020
#define B14 0000010
#define B15 0000004
#define B16 0000002
#define B17 0000001

// b2, the raw break requests, is also set from the Type 340's own thread: its initiateBreak()
// ends in req(). So every change to b2 is one atomic operation, and neither thread's update can
// be lost in the middle of the other's read-modify-write. A request is released and sbs_sync()'s
// load acquires it, so a device's state written before the request is visible to the program's
// break routine. b2 is still a plain field, so pdp1.h and the plugin ABI are unchanged.
#define B2_LOAD(pdp) __atomic_load_n(&(pdp)->b2, __ATOMIC_ACQUIRE)
#define B2_STORE(pdp, v) __atomic_store_n(&(pdp)->b2, (v), __ATOMIC_RELAXED)
#define B2_OR(pdp, v) __atomic_fetch_or(&(pdp)->b2, (v), __ATOMIC_RELEASE)
#define B2_AND(pdp, v) __atomic_fetch_and(&(pdp)->b2, (v), __ATOMIC_RELAXED)

int decflg(int flg);

static void iot_pulse(PDP1 *pdp, int pulse, int dev, int nac);
static void iot(PDP1 *pdp, int pulse);

// The emulator duplicates all of the original hardware
// subcycles. Impressive.
// TP   length, ns  end tine in main cycle
//  0   200         200
//  1   300         500
//  2   550         1050
//  3   300         1350
//  4   200         1550
//  5   250         1800
//  6   250         2050
//  6a  400         2450
//  7   200         2650
//  8   200         2850
//  9   1200        4050
//  9a  750         4800
//  10  200         5000

enum
{
    TP0_end = 200,
    TP1_end = 500,
    TP2_end = 1050,
    TP3_end = 1350,
    TP4_end = 1550,
    TP5_end = 1800,
    TP6_end = 2050,
    TP6a_end = 2450,
    TP7_end = 2650,
    TP8_end = 2850,
    TP9_end = 4050,
    TP9a_end = 4800,
    TP10_end = 5000,
    TP_unreachable = TP10_end
};

// TP(n): at TP-n, if the random "panel sample point" for this cycle
// (pdp->timernd) falls before TPn_end, latch the indicator lights from
// the current machine state and remember that we've done it (by setting
// timernd to an unreachable value so later TP(n) calls in the same
// cycle are no-ops). This is how the emulator gets a realistic, jittery
// snapshot of the front-panel lights once per cycle.
#define TP(n) if(pdp->timernd < TP##n##_end) { updatelights(pdp, pdp->panel); pdp->timernd = TP_unreachable; }

// --- "Is the current instruction finished?" / "can a sequence break
// interrupt right now?" tests, broken down by which kind of cycle we're
// currently in (cycle 0 = fetch, defer = indirect-address cycle).
//
// CY0_INST_DONE:  in cycle 0, the instruction is complete unless it's
//                 one that needs a cycle 1 (opcodes < 030 -- memory
//                 reference instructions) or a defer cycle.
// CY0_MIDBRK_PERMIT: in cycle 0, a sequence break may be inserted
//                 mid-instruction only for memory reference instructions
//                 (opcodes < 030), which haven't altered PC/AC yet.
// DF_INST_DONE:   in a defer cycle, the instruction is complete unless
//                 it's a memory reference instruction (needs cycle 1) --
//                 same test as CY0_INST_DONE, restated for df2.
// DF_MIDBRK_PERMIT: in a defer cycle, a break may be inserted
//                 mid-instruction for memory reference instructions, or
//                 for jmp/jsp while still chasing indirect addresses
//                 (df2 set).
#define CY0_INST_DONE ((!pdp->df1) && (pdp->ir >= 030))
#define CY0_MIDBRK_PERMIT (pdp->ir < 030)
#define DF_INST_DONE ((!pdp->df2) && (pdp->ir >= 030))
#define DF_MIDBRK_PERMIT ((pdp->ir < 030) || ((IR_JMP || IR_JSP) && pdp->df2))

// Have to be careful with those at TP10
// because that's where we choose the next type of cycle.
// So the exact state of  cyc, df1, df2, bc, hsc  is crucial.
//
// CY1: we are about to run (or are running) cycle 1, the execute cycle
//      of a memory reference instruction (cyc set, not deferring, and
//      not in a break sequence).
// DF:  we are in a defer (indirect addressing) cycle.
// INST_DONE: true once the current instruction has fully retired --
//      i.e. cycle 0 finished an instruction that needed no cycle 1/defer,
//      or a defer cycle finished one that needed no cycle 1, or cycle 1
//      itself just ran -- and we're not in the middle of a break sequence.
// MIDBRK_PERMIT: true if a sequence break is allowed to interrupt the
//      instruction in progress (rather than waiting for it to finish),
//      based on which cycle we're in.
#define CY1 (pdp->cyc && (!pdp->df1) && (pdp->bc == 0))
#define DF (pdp->cyc && pdp->df1)
#define INST_DONE (((!pdp->cyc && CY0_INST_DONE) || (DF && DF_INST_DONE) || CY1) && (!pdp->bc))
#define MIDBRK_PERMIT ((!pdp->cyc && CY0_MIDBRK_PERMIT) || (DF && DF_MIDBRK_PERMIT))

// SBS_BREAK_REQ / SBS_BREAK: a sequence break (SBS) is being requested --
// the sequence break system is enabled (sbm) and some channel has a
// synchronized request pending (sbs_seq). SBS_BREAK is just an alias used
// at the point where the break is actually taken.
#define SBS_BREAK_REQ (pdp->sbm && pdp->sbs_seq)
#define SBS_BREAK (SBS_BREAK_REQ)

// MANUAL_RUN: the front-panel switches call for the machine to stop after
// this cycle -- either SINGLE CYCLE is down (stop after every cycle), or
// SINGLE INSTRUCTION is down and the current instruction has just finished.
#define MANUAL_RUN (pdp->single_cyc_sw || (pdp->single_inst_sw && INST_DONE))

// STOP: the run flip-flop should clear after this cycle -- the
// instruction register holds an illegal/unassigned opcode (IR_INCORR,
// which does NOT apply to break cycles -- those don't load IR the same
// way), or the panel switches call for a stop (MANUAL_RUN), or RUN ENABLE
// has been cleared some other way.
#define STOP (IR_INCORR || MANUAL_RUN || !pdp->run_enable)

// Read the core word addressed by (ema|MA) into MB (OR'd in -- callers
// are expected to have cleared MB first), and record the address and
// the word in lastReadAddr/lastReadWord for writemem() later in the same
// machine cycle.
//
// The real core read is destructive: it leaves the location at 0, and
// the write half of the same memory cycle puts the (possibly modified)
// word back. This used to be modeled literally, by zeroing core here.
// Core is shared memory, though, and ad1 and the Type 340 display thread
// read and write it concurrently with this thread, so for part of every
// cycle they could see that transient 0, and a store they made in the
// window was reverted by the write-back. The read no longer zeroes the
// word. Paired with writemem() skipping an unchanged write-back, core
// holds exactly what the old model left at every cycle boundary; only a
// concurrent observer can tell the difference.
static void
readmem(PDP1 *pdp)
{
unsigned int addr;

    addr = ((pdp->ema | MA) % MAXMEM);
    lastReadAddr = addr;
    lastReadWord = pdp->core[addr];
    MB |= lastReadWord;
}

// Write MB to the core location addressed by (ema|MA): the write half of
// the memory cycle readmem() started. The store is skipped when the
// address is the one readmem() read and MB still equals the word it read,
// because core already holds that word (readmem() no longer zeroes it).
// Any other case stores, exactly as the unconditional write-back did, so
// the skip can only ever drop a store of the same word to the same place.
// The inhibit flip-flop (pdp->i) is not consulted: it is set in every
// memory cycle, and it is how 0 bits are written, not a suppression of
// the write (see inhibit()).
static void
writemem(PDP1 *pdp)
{
unsigned int addr;

    addr = ((pdp->ema | MA) % MAXMEM);

    if((addr != lastReadAddr) || (MB != lastReadWord))
    {
        pdp->core[addr] = MB;
    }
}

// Advance the core-memory read/write/restore/inhibit flip-flop chain by
// one step: i<-w, w<-rs, rs<-r, r<-!w (computed from the old w, before it
// was overwritten). These four flags (r, rs, w, i) track where in its
// read/write cycle each core memory access currently is; this is called
// once per timing pulse to shift them along. memclr()/inhibit() below
// reset or perturb the same chain.
static void mop2379(PDP1 *pdp)
{
    pdp->i = pdp->w;
    pdp->w = pdp->rs;
    pdp->rs = pdp->r;
    pdp->r = !pdp->w;   // actually !pdp->rs but already clobbered
}

// Force the core memory "inhibit" flip-flop on. Every memory cycle sets
// it at TP8 (F17, Docs/F17_PDP1Maint.pdf, section 3-10). It does not
// suppress the write: during the write half, inhibit current keeps the
// cores for MB's 0 bits at 0 while the write current sets the rest to 1
// (F17 section 8-5f), so inhibit is how zeros are written. writemem()
// does not consult it.
static void inhibit(PDP1 *pdp)
{
    pdp->i = 1;
}

// Clear the whole core read/write/restore/inhibit flip-flop chain back
// to idle, ready for the next memory cycle.
static void memclr(PDP1 *pdp)
{
    pdp->r = 0;
    pdp->rs = 0;
    pdp->w = 0;
    pdp->i = 0;
}

// POWER CLEAR: the real PDP-1's flip-flops come up in an unpredictable
// state when power is applied, so initialize every register and flag to
// a random value (this also helps shake out code that assumes a clean
// reset). The block at the end picks a random but *self-consistent*
// cyc/df1/df2/bc combination, since cycle()/cycle0()/defer()/cycle1()
// below assert on certain combinations being impossible and a fully
// random combination could violate that.
void
pwrclr(PDP1 *pdp)
{
    IR = rand() & 077;
    PC = rand() & 07777;
    MA = rand() & 07777;
    MB = rand() & 0777777;
    AC = rand() & 0777777;
    IO = rand() & 0777777;

    pdp->cyc = rand() & 1;
    pdp->df1 = rand() & 1;
    pdp->df2 = rand() & 1;
    pdp->bc = rand() & 3;
    pdp->ov1 = rand() & 1;
    pdp->ov2 = rand() & 1;
    pdp->rim = rand() & 1;
    pdp->sbm = rand() & 1;
    pdp->ioc = rand() & 1;
    pdp->ihs = rand() & 1;
    pdp->ios = rand() & 1;
    pdp->ioh = rand() & 1;
    pdp->pf = rand() & 077;
    memclr(pdp);

    // A memory cycle follows another only when one cycle's TP10 triggers the next one's TP0,
    // and only a console operation (SP4), the multiply/divide restart or read-in starts the
    // chain again (F17 6-2c and 6-2d, pages 6-5 and 6-6). A power cycle breaks it, so the
    // machine comes up halted whatever its run flip-flop holds; here, run is what keeps the
    // chain going.
    pdp->run = 0;

    if(pdp->sbs16)
    {
        pdp->b4 = rand() & 0177777;
        pdp->b3 = rand() & 0177777;
        B2_STORE(pdp, rand() & 0177777);
        pdp->b1 = rand() & 0177777;
    }
    else
    {
        pdp->b4 = rand() & 1;
        pdp->b3 = rand() & 1;
        B2_STORE(pdp, rand() & 1);
        pdp->b1 = rand() & 1;
    }

    pdp->sbs_seq = 0;

    pdp->hsc = 0;

    pdp->emc = rand() & 1;
    pdp->exd = rand() & 1;
    pdp->ema = rand() & EXTMASK;
    pdp->epc = rand() & EXTMASK;

    pdp->rc = 0;
    pdp->rby = 0;
    pdp->rcl = 0;
    pdp->r_time = NEVER;
    pdp->rim_return = 0;
    pdp->rim_cycle = 0;

    pdp->punon = 0;
    pdp->p_time = NEVER;
    pdp->feed_time = 0;

    pdp->tbs = 0;
    pdp->tbb = 0;
    pdp->tyo = 0;
    pdp->typ_time = NEVER;
    pdp->tyi_wait = 0;

    // HACK: on power on the next cycle is undefined
    // pressing continue would try to execute from an invalid state.
    // since this emulation does not support such madness
    // we try for some randomized rudimentary consistency here as not to hit asserts
    pdp->cyc = pdp->df1 = pdp->df2 = pdp->bc = pdp->hsc = 0;
    switch(rand() % 10) {
    case 0: pdp->cyc = 1; break;
    case 1: pdp->cyc = pdp->df1 = 1; break;
    case 2: pdp->cyc = 1; break;
    case 3: pdp->cyc = 1; pdp->bc = 1; break;
    case 4: pdp->cyc = 1; pdp->bc = 2; break;
    case 5: pdp->cyc = 1; pdp->bc = 3; break;
    // 6-9: cyc0
    }
}

// Multiply-step shift: shift the 36-bit AC:IO pair one place right
// (AC's bit 17 feeds into IO's bit 0), used once per step of the
// multiply algorithm in multiply().
static void
mul_shift(PDP1 *pdp)
{
int ac;

    ac = AC >> 1;
    IO = ((AC & B17) << 17) | (IO >> 1);
    AC = ac;
}

// Divide-step shift: shift the 36-bit AC:IO pair one place left, with
// IO's old sign feeding AC's bit 17 and AC's old (complemented) sign
// feeding IO's bit 17. AC's sign bit (B0) is preserved across the shift.
// Used once per step of the divide algorithm in divide().
static void
div_shift(PDP1 *pdp)
{
int ac;

    ac = ((AC & ~B0) << 1) | ((IO & B0) >> 17);
    IO = ((IO & ~B0) << 1) | ((~AC & B0) >> 17);
    AC = ac;
}

// End-around carry add: AC += MB using the PDP-1's one's-complement
// "end-around carry" rule -- any carry out of bit 0 wraps back into
// bit 17 -- then mask to 18 bits.
static void
carry(PDP1 *pdp)
{
    AC += (~AC & MB) << 1;

    if(AC & 01000000)
    {
        AC++;
    }

    AC &= WORDMASK;
}

// Increment AC, with the one's-complement quirk that +0 (0777777)
// increments to -0 (0000000) rather than 1, and -0 increments to +1.
static void
inc_ac(PDP1 *pdp)
{
    if(AC == 0777776)
    {
        AC = 0;
    }
    else if(AC == 0777777)
    {
        AC = 1;
    }
    else
    {
        AC++;
    }
}
// JSP/OPR "store program counter": OR the overflow flag, extend mode
// flag, extension-of-PC field, and PC itself into AC (bits 0, 1, and
// the address bits respectively).
static void
pc_to_ac(PDP1 *pdp)
{
    AC |= (pdp->ov1 << 17) | (pdp->exd << 16) | pdp->epc | PC;
}

// Clear the memory address register (and its extension) to 0.
static void
clr_ma(PDP1 *pdp)
{
    MA = 0;
    pdp->ema = 0;
}

// Load MA's address field from MB. The extension (bank) field of MA
// comes either from MB itself (if extended-memory addressing, "emc", is
// active for this operand) or is carried forward from the current
// extended PC (epc) otherwise.
static void
mb_to_ma(PDP1 *pdp)
{
    MA |= MB & ADDRMASK;

    if(pdp->emc)
    {
        pdp->ema |= MB & EXTMASK;
    }
    else
    {
        pdp->ema |= pdp->epc;
    }
}

// Load MA from PC (for instruction fetch), carrying the current
// extended-PC bank into MA's extension.
static void
pc_to_ma(PDP1 *pdp)
{
    MA |= PC;
    pdp->ema |= pdp->epc;
}


// Clear the program counter (and its extension) to 0.
static void
clr_pc(PDP1 *pdp)
{
    PC = 0;
    pdp->epc = 0;
}

// Load PC's address field from MB (for jmp/jsp). The extension (bank)
// field of PC comes either from MB itself (if extended addressing is
// active) or is carried forward from the current effective-address bank
// (ema) otherwise.
static void
mb_to_pc(PDP1 *pdp)
{
    PC |= MB & ADDRMASK;

    if(pdp->emc)
    {
        pdp->epc |= MB & EXTMASK;
    }
    else
    {
        pdp->epc |= pdp->ema;
    }
}

// Load PC from MA (used by cal/jda to jump to the computed address),
// carrying MA's bank into PC's extension.
static void
ma_to_pc(PDP1 *pdp)
{
    PC |= MA;
    pdp->epc |= pdp->ema;
}

// Advance the program counter by one, wrapping within the 12-bit
// address field (bank/extension is untouched).
static void
pc_inc(PDP1 *pdp)
{
    PC = (PC + 1) & ADDRMASK;
}

// Undo the speculative pc_inc() done while fetching this instruction,
// and drop out of any in-progress defer (indirect addressing) chase.
// Used when a sequence break needs to re-run the interrupted
// instruction from scratch.
static void
inst_cancel(PDP1 *pdp)
{
    PC = (PC - 1) & ADDRMASK;
    pdp->df1 = 0;
    pdp->df2 = 0;
}

// Whether a high-speed channel may take the next memory cycle, set at every cycle's TP10
// (F17 6-18, 6-19, 3-49, 3-50). main.c's scan runs between cycles, so it asks here.
#define HSCGRANT_NONE       0   // inside a sequence break's entry, or after cycle 0 of a once-deferred jmp/jsp
#define HSCGRANT_BOUNDARY   1   // the instruction is done, or a sequence break has just been taken
#define HSCGRANT_RESTART    2   // mid-instruction: the break restarts the instruction from cycle 0
static int hscGrant = HSCGRANT_BOUNDARY;

// True if a channel may take the next memory cycle.
bool
hscBreakAllowed(void)
{
    return(hscGrant != HSCGRANT_NONE);
}

// A channel has taken the next memory cycle. Mid-instruction, the hardware backs PC up and
// begins the instruction again after the break (F17 6-18). The cycles it had run were real
// machine cycles, so the panel tally keeps them.
void
hscBreak(PDP1 *pdp)
{
    if(hscGrant == HSCGRANT_RESTART)
    {
        inst_cancel(pdp);
        pdp->cyc = 0;
        updatelights_pwm(pdp->panel, pdp->inst_cyc);
        pdp->inst_cyc = 0;
    }

    // After a channel cycle any further break request is granted (F17 3-49).
    hscGrant = HSCGRANT_BOUNDARY;
}

// TP9A
// TP10

// Recompute pdp->sbs_seq, the single highest-priority pending-and-not-yet-
// held sequence break: it's the lowest set bit of b3 (synchronized
// requests) that is below the lowest set bit of b4 (breaks already being
// held) -- i.e. the highest-priority request not currently masked by an
// in-progress higher- or equal-priority break.
static void
sbs_calc_req(PDP1 *pdp)
{
	pdp->sbs_seq = (pdp->b3 & (~pdp->b3 + 1)) & ((pdp->b4 & (~pdp->b4 + 1)) - 1);
}

// CBS (Clear Sequence Breaks): clear all pending/held break state
// (b3 synchronized requests, b4 held breaks, and for 16-channel SBS also
// b2 raw requests), then recompute sbs_seq.
static void
clr_sbs(PDP1 *pdp)
{
	pdp->b4 = 0;
	pdp->b3 = 0;
	B2_STORE(pdp, 0);	// clear in all SBS modes: CBS clears all break state (hardware-correct)
	sbs_calc_req(pdp);
}

// Called at TP4 of every cycle: synchronize raw channel requests (b2)
// into b3, and, if a break is currently being taken (bc==1, the "hold
// break" sub-cycle), mark the chosen channel(s) (sbs_seq) as held in b4
// and clear them from b3 (16-channel SBS) or clear all of b2 (SBS256).
// Then recompute sbs_seq for the next cycle's priority decision.
// SBS256 works differently here
static void
sbs_sync(PDP1 *pdp)
{
	pdp->b3 |= B2_LOAD(pdp);
	if(pdp->bc == 1) {
		// HOLD BREAK
		pdp->b4 |= pdp->sbs_seq;
		if(pdp->sbs16)
			pdp->b3 &= ~pdp->sbs_seq;
		else
			B2_STORE(pdp, 0);
	}
	sbs_calc_req(pdp);
}

// Called at TP10 of every cycle (16-channel SBS only): clear any raw
// request bits (b2) that have already been synchronized into b3, so they
// don't get synchronized again next cycle.
// The atomic AND is done only when there is a bit to clear, so an idle cycle costs one load.
static void
sbs_reset_sync(PDP1 *pdp)
{
	if(pdp->sbs16 && (__atomic_load_n(&pdp->b2, __ATOMIC_RELAXED) & pdp->b3))
		B2_AND(pdp, (uint16_t)~pdp->b3);
}

// SC (clear): the machine's general "clear" pulse, issued on start,
// deposit, examine, and at the end of each break-cycle sequence. Resets
// the cycle/defer/break flip-flops, overflow, I/O sync flags, IR, PC,
// and sequence-break state to their idle values, and clears the I/O
// extension flags (lai/lia/emc/exd) and typewriter-output flag.
static void
sc(PDP1 *pdp)
{
    pdp->df1 = 0;
    pdp->df2 = 0;
    pdp->bc = 0;
    hscGrant = HSCGRANT_BOUNDARY;   // the cycle state is cleared, so no instruction is in progress
    pdp->ov1 = 0;
    pdp->ov2 = 0;
    pdp->ihs = 0;
    pdp->ioc = 1;
    pdp->ios = 0;
    pdp->ioh = 0;
    pdp->ir = 0;
    clr_pc(pdp);
    pdp->sbm = pdp->sbm_start_sw;
    clr_sbs(pdp);
    B2_STORE(pdp, 0);

    pdp->lai = 0;
    pdp->lia = 0;
    pdp->emc = 0;
    pdp->exd = 0;
    pdp->tyo = 0;
}

// Handle a front-panel pulse from the START, STOP, CONTINUE, EXAMINE, or
// DEPOSIT switches (whichever of pdp->*_sw is set when this is called).
// Mirrors the console logic's SP1-SP4 pulse sequence: stop the machine,
// optionally clear AC/sequence state, load PC/AC from the test
// address/word switches, fake up the right IR for examine/deposit, and
// (for start/continue) set run.
void
spec(PDP1 *pdp)
{
    // PB
    pdp->run = 0;

    if(pdp->start_sw || pdp->deposit_sw || pdp->examine_sw)
    {
        pdp->rim = 0;
    }

    // SP1
    pdp->run_enable = 1;
    clr_ma(pdp);

    if(pdp->start_sw)
    {
        pdp->cyc = 0;
    }

    if(pdp->deposit_sw)
    {
        pdp->ac = 0;
    }

    if(pdp->start_sw ||  pdp->deposit_sw || pdp->examine_sw)
    {
        sc(pdp);
    }

    // SP2
    if(pdp->start_sw || pdp->deposit_sw || pdp->examine_sw)
    {
        PC |= pdp->ta;
        pdp->epc |= pdp->eta;
    }

    if(pdp->deposit_sw || pdp->examine_sw || pdp->rim)
    {
        pdp->cyc = 1;
    }

    if(pdp->examine_sw)
    {
        pdp->ir |= 020 >> 1;
    }

    if(pdp->deposit_sw)
    {
        pdp->ir |= 024 >> 1;
        AC |= pdp->tw;
    }

    // SP3
    if(pdp->deposit_sw || pdp->examine_sw)
    {
        pc_to_ma(pdp);
        pdp->cychack = 1;
    }

    if(pdp->start_sw && pdp->extend_sw)
    {
        pdp->exd = 1;
    }

    // SP4
    if(pdp->start_sw || pdp->continue_sw)
    {
        pdp->run = 1;
    }
}

// Handle a pulse from the READ IN switch: stop the machine, enter
// "reading in" (rim) mode with sequence breaks disabled, and arm the
// special read-in cycle (readin1/readin2) to run on the next cycle.
void
start_readin(PDP1 *pdp)
{
    // PB
    pdp->run = 0;
    pdp->rim = 1;
    pdp->sbm = 0;

    pdp->rim_cycle = 1;
}

// First half of the special read-in cycle (replaces a normal cycle 0
// while rim_cycle is set): clear MA, perform an SC clear, and pulse the
// reader (channel 2, "rpb") to start loading the first word from tape
// into IO.
void
readin1(PDP1 *pdp)
{
    pdp->rim_cycle = 0;

    // SP1
    pdp->run_enable = 1;
    clr_ma(pdp);

    if(pdp->rim)
        // guaranteed
    {
        MB = 0;
        sc(pdp);
        iot_pulse(pdp, 1, 2, 0);
    }
}

// Second half of the special read-in cycle: take the word just read into
// IO, decode it as an instruction into IR/MA. If it's a "dio" (deposit
// in/out), pulse the reader again to fetch the data word and store it
// via the dio IOT; if it's a "jmp", treat it as the end-of-tape transfer
// address, load PC and start the machine running from there.
void
readin2(PDP1 *pdp)
{
    // SP2
    pdp->cyc = 1;
    MB |= IO;
    pdp->epc |= pdp->eta;

    // SP3
    IR |= MB >> 13;
    mb_to_ma(pdp);

    if(pdp->extend_sw)
    {
        pdp->exd = 1;
    }

    // SP4
    if(IR_DIO)
    {
        iot_pulse(pdp, 1, 2, 0);
    }

    if(IR_JMP)
    {
        pdp->run = 1;
        pdp->cyc = 0;
        pdp->rim = 0;
        mb_to_pc(pdp);
        pdp->cychack = 1;   // actually TP0 should work too
    }
}

// Clear the type-10 multiply/divide unit's step counter (scr) and sign
// bookkeeping flags (smb, srm). Called at TP9/TP10 of every normal cycle
// so they're clean before the next mul/div, if any, runs in cycle1().
static void
clrmd(PDP1 *pdp)
{
    pdp->scr = 0;
    pdp->smb = 0;
    pdp->srm = 0;
}

// Type-10 option: run the entire shift-and-add multiply algorithm
// (MDP-3..MDP-6 in the option's flow chart) to completion. AC holds one
// operand, MB the other; the 36-bit product ends up in AC:IO. Also
// accumulates the extra simulated time the real hardware would have
// taken for the asynchronous multiply steps.
static void
multiply(PDP1 *pdp)
{
int lastlong;

    pdp->simtime += 150;
    goto start;

    do
    {
        if( (lastlong = (IO & B17)) )      // deliberate assignment-and-test, not a stray ==
        {
            // MDP-3
            AC ^= MB;
            pdp->simtime += 200;

            // MDP-4
            carry(pdp);
            pdp->simtime += 500;
        }

        // MDP-5
        pdp->scr = (pdp->scr + 1) & 037;
        mul_shift(pdp);
        pdp->simtime += 150;

start:
        // MDP-6
        // if(scr==021) MD_RESTART
        // but we run synchronously
        ;
    }
    while(pdp->scr != 022);

    // last cycle is asynchronous
    pdp->simtime -= (lastlong * (200 + 500)) + 150;

    // MDP-11
    if((pdp->srm != pdp->smb) && (!((AC == 0) && (IO == 0))))
    {
        AC ^= WORDMASK;
        IO ^= WORDMASK;
    }
}

// Type-10 option: run the entire non-restoring divide algorithm
// (MDP-1..MDP-14 in the option's flow chart) to completion. AC:IO holds
// the 36-bit dividend, MB the divisor; the quotient ends up in IO and the
// remainder in AC. Also accumulates the extra simulated time the real
// hardware would have taken for the asynchronous divide steps.
static void
divide(PDP1 *pdp)
{
int t;
int done;

    pdp->simtime += 150;
    goto start;

    do
    {
        // MDP-1
        if(!(AC & B0))
        {
            MB ^= WORDMASK;
        }

        div_shift(pdp);
        pdp->simtime += 150;

        if(!(IO & B17))
        {
            // MDP-2
            inc_ac(pdp);
            pdp->simtime += 500;
        }

start:
        // MDP-3
        AC ^= MB;
        pdp->simtime += 200;

        // MDP-4
        if(AC == 0777777)
        {
            AC ^= WORDMASK;
        }
        else
        {
            carry(pdp);
        }

        // delayed 0.05us
        if(MB & B0)
        {
            MB ^= WORDMASK;
        }

        pdp->simtime += 500;

        // MDP-5
        done = (pdp->scr == 022) || ((pdp->scr == 0) && (!(AC & B0)));
        pdp->scr = (pdp->scr + 1) & 037;
    }
    while(!done);

    // MDP-7
    AC ^= MB;

    if(pdp->scr & 2)
    {
        pc_inc(pdp);
    }

    pdp->simtime += 150;

    // MDP-8
    if(AC == 0777777)
    {
        AC ^= WORDMASK;
    }
    else
    {
        carry(pdp);
    }

    pdp->simtime += 500;

    // MDP-9
    if(pdp->scr & 020)
    {
        AC >>= 1;
    }

    // MD_RESTART, but we run synchronously
    // so don't add to simtime from now on

    // MDP-10
    if((!(pdp->scr & 020) && pdp->srm) ||
            ((pdp->scr & 020) && (IO != 0) && (pdp->srm != pdp->smb)))
    {
        IO ^= WORDMASK;
    }

    if((AC != 0) && pdp->srm)
    {
        AC ^= WORDMASK;
    }

    MB = 0;

    // swap AC and IO
    if(pdp->scr & 020)
    {
        // MDP-12
        MB |= IO;

        // MDP-13
        t = MB;
        MB = AC;
        AC = t;
        IO = 0;

        // MDP-14
        IO |= MB;
    }
}

// Map a 3-bit "skip on program flag" / "set program flag" field (the low
// 3 bits of MB, as used by the skp and opr program-flag instructions)
// to the corresponding program-flag bitmask. Field value 0 means "no
// flag selected"; 1-6 select individual flags pf1-pf6; 7 selects all six.
int
decflg(int n)
{
    switch(n & 7)
    {
    case 1:
        return 040;
    case 2:
        return 020;
    case 3:
        return 010;
    case 4:
        return 004;
    case 5:
        return 002;
    case 6:
        return 001;
    case 7:
        return 077;
    }

    return 0;
}

// Perform one step of a "shift or rotate" (sho/shr-class) instruction.
// Which of the 12 shift/rotate variants (RAL/RIL/RCL/SAL/SIL/SCL/RAR/
// RIR/RCR/SAR/SIR/SCR -- rotate/shift, AC/IO/combined, left/right) runs
// is selected by bits 9-12 of MB, decoded once per call; this is invoked
// once for each of the (up to 9) shift-count bits set in the instruction
// word, across several timing pulses in cycle0().
static void
shro(PDP1 *pdp)
{
int ac, io;

    ac = AC;
    io = IO;

    switch((MB >> 9) & 017)
    {
    case 001:   // RAL
        ac = ((AC & ~B0) << 1) | ((AC & B0) >> 17);
        break;

    case 002:   // RIL
        io = ((IO & ~B0) << 1) | ((IO & B0) >> 17);
        break;

    case 003:   // RCL
        ac = ((AC & ~B0) << 1) | ((IO & B0) >> 17);
        io = ((IO & ~B0) << 1) | ((AC & B0) >> 17);
        break;

    case 005:   // SAL
        ac = (AC & B0) | ((AC & ~(B0 | B1)) << 1) | ((AC & B0) >> 17);
        break;

    case 006:   // SIL
        io = (IO & B0) | ((IO & ~(B0 | B1)) << 1) | ((IO & B0) >> 17);
        break;

    case 007:   // SCL
        ac = (AC & B0) | ((AC & ~(B0 | B1)) << 1) | ((IO & B0) >> 17);
        io = ((IO & ~B0) << 1) | ((AC & B0) >> 17);
        break;

    case 011:   // RAR
        ac = ((AC & B17) << 17) | (AC >> 1);
        break;

    case 012:   // RIR
        io = ((IO & B17) << 17) | (IO >> 1);
        break;

    case 013:   // RCR
        ac = ((IO & B17) << 17) | (AC >> 1);
        io = ((AC & B17) << 17) | (IO >> 1);
        break;

    case 015:   // SAR
        ac = (AC & B0) | (AC >> 1);
        break;

    case 016:   // SIR
        io = (IO & B0) | (IO >> 1);
        break;

    case 017:   // SCR
        ac = (AC & B0) | (AC >> 1);
        io = ((AC & B17) << 17) | (IO >> 1);
        break;
    }

    AC = ac;
    IO = io;
}

// Move the "deferred" overflow indication (ov2, set mid-instruction by
// add/sub/etc.) into the latched overflow flip-flop (ov1) that the skip-
// on-overflow test and the overflow indicator lamp actually look at, then
// clear ov2. Called at TP10 of every cycle.
static void
syncov(PDP1 *pdp)
{
    if(pdp->ov2)
    {
        pdp->ov1 = 1;
    }

    pdp->ov2 = 0;
}

// CYCLE 0: instruction fetch. Reads the next instruction word from the
// location addressed by PC into MB/IR, increments PC, and for memory
// reference instructions either finishes the instruction immediately
// (operate/skip/etc-class opcodes) or sets up df1/cyc so defer() or
// cycle1() runs next. Also handles the multi-pulse "sho"/"shr" shift-
// rotate instructions (one shro() call per set shift-count bit, spread
// across TP0-TP9A) and the IOT instruction's TP6A/TP7/TP10 pulses.
//
// The switch(hack)/case labels let cycle1() jump in here partway through
// at TP4, via cychack to execute an "xct" instruction's target as if it
// were a normal cycle-0 fetch starting from TP4.
// Rather messy.
static void
cycle0(PDP1 *pdp)
{
int hack;

    hack = pdp->cychack;
    pdp->cychack = 0;

    switch(hack)
    {
    default:
        // TP0: if "sho" with shift-count bit 12 set, do one shift step.
        // Point MA at PC to fetch the instruction word.
        if(IR_SHRO && (MB & B12))
        {
            shro(pdp);
        }

        pc_to_ma(pdp);
        TP(0)
        __attribute__((fallthrough));      // deliberate TP-state fall-through, not missing a break

    case 1:
        // TP1: if "sho" with shift-count bit 11 set, do one shift step.
        if(IR_SHRO && (MB & B11))
        {
            shro(pdp);
        }

        pdp->emc = 0;

        TP(1)

        // TP2: advance the core read/write chain, do another shro() step
        // (bit 10), advance PC past the instruction just fetched, and for
        // IOT decide whether the I/O completion pulse can fire this
        // cycle (ioc) based on the in-out sync/hold flags.
        mop2379(pdp);

        if(IR_SHRO && (MB & B10))
        {
            shro(pdp);
        }

        pc_inc(pdp);

        if(IR_IOT)
        {
            pdp->ioc = !pdp->ioh && !pdp->ihs;
        }

        pdp->ihs = 0;
        TP(2)

        // TP3: advance the core read/write chain, do another shro() step
        // (bit 9), and clear MB ready to receive the operand word read at
        // TP4.
        mop2379(pdp);

        if(IR_SHRO && (MB & B9))
        {
            shro(pdp);
        }

        MB = 0;

        // If this cycle is completing the last 4 shifts (B12-B9) of a SHRO
        // instruction deferred from TP10 of the previous cycle, snapshot the
        // now-settled AC/IO state and flush the deferred tally.
        if(pdp->sho_deferred)
        {
            updatelights(pdp, pdp->panel);
            updatelights_pwm(pdp->panel, pdp->sho_deferred);
            pdp->sho_deferred = 0;
        }

        TP(3)
        __attribute__((fallthrough));      // deliberate TP-state fall-through, not missing a break

    case 4:
        // TP4: synchronize sequence-break requests, read the operand
        // word from the address PC points to into MB, and decode its top
        // 5 bits into IR (cleared first, then OR'd in at TP5 -- IR isn't
        // fully valid until TP5).
        sbs_sync(pdp);
        readmem(pdp);
        IR = 0;
        TP(4)

        // TP5: finish decoding IR from MB.
        IR |= MB >> 13;
        TP(5)

        // TP6: if the indirect-address bit (B5, bit 5) is set and this
        // is a memory-reference instruction (not sho/skip/law/opr/iot/
        // cal-jda, which use bit 5 for other purposes), enter the defer
        // (indirect-addressing) cycle next.
        if((MB & B5) && !IR_SHRO && !IR_SKIP &&
            !IR_LAW && !IR_OPR && !IR_IOT && !IR_CALJDA)
        {
            pdp->df1 = 1;
        }

        TP(6)

        // TP6a: for IOT with the no-wait bit (B5) clear, if the I/O
        // device hasn't yet signalled "in-out halt" (ioh), request the
        // completion pulse now (ioc) and mark in-out-halt-set (ihs) so
        // TP7 won't request it again.
        if(IR_IOT && !(MB & B5) && pdp->ioh)
        {
            pdp->ioc = 1;
            pdp->ihs = 1;
            pdp->ioh = 0;
        }

        TP(6a)

        // TP7: advance the core read/write chain and do another shro()
        // step (bit 17, the last one). Clear AC for jsp/law/opr-clear-AC,
        // clear IO for opr-clear-IO. For IOT, set the in-out-halt flag
        // if this iot waits for completion (B5 set) and nothing else has
        // claimed the cycle, and fire the device's TP7 pulse if ioc is
        // set.
        mop2379(pdp);

        if(IR_SHRO && (MB & B17))
        {
            shro(pdp);
        }

        if((IR_JSP && (!pdp->df1)) || IR_LAW || (IR_OPR && (MB & B10)))
        {
            AC = 0;
        }

        if(IR_OPR && (MB & B6))
        {
            IO = 0;
        }

        if(IR_IOT)
        {
            if((MB & B5) && !pdp->ioh && !pdp->ihs)
            {
                pdp->ioh = 1;
            }

            if(pdp->ioc)
            {
                iot(pdp, 0);
            }
        }

        TP(7)

        // TP8: set inhibit, as every memory cycle does -- it is how the
        // write half stores MB's 0 bits, not a suppression of the write
        // (see inhibit()). For "jsp"/"jmp" with no indirect addressing,
        // store/clear PC now (if deferring, defer() does this instead).
        // Run the "skp" skip tests, the remaining shro()/law/opr effects,
        // and (if enabled) the 1D OPR1D extension instructions.
        inhibit(pdp);

        if(!pdp->df1)
        {
            if(IR_JSP)
            {
                pc_to_ac(pdp), clr_pc(pdp);
            }

            if(IR_JMP)
            {
                clr_pc(pdp);
            }
        }

        if(IR_SKIP)
        {
            int skip = 0;

            if( core1DEnabled && (MB & B6) && IO)
            {
                logger(LOG_1D, "sni\n");
                skip = 1;    // wje - pdp-1D sni, skip on nonzero IO
            }

            if((MB & B7) && !(IO & B0))
            {
                skip = 1;
            }

            if((MB & B8) && !pdp->ov1)
            {
                skip = 1;
            }

            if((MB & B9) && (AC & B0))
            {
                skip = 1;
            }

            if((MB & B10) && !(AC & B0))
            {
                skip = 1;
            }

            if((MB & B11) && (AC == 0))
            {
                skip = 1;
            }

            if((MB & 070) && !(pdp->ss & decflg(MB >> 3)))
            {
                skip = 1;
            }

            if((MB & 007) && !(pdp->pf & decflg(MB)))
            {
                skip = 1;
            }

            if(MB & B5)
            {
                skip = !skip;
            }

            if(skip)
            {
                pc_inc(pdp);
            }
        }

        if(IR_SHRO && (MB & B16))
        {
            shro(pdp);
        }

        if(IR_LAW)
        {
            AC |= MB & 0007777;
        }

        if(IR_OPR)
        {
            if( core1DEnabled && (MB & B5) )
            {
                logger(LOG_1D, "cmi\n");
                IO = ~IO;    // wje - pdp-1D cmi, complement IO
            }

            if(MB & B7)
            {
                AC |= pdp->tw;
            }

            if(MB & B11)
            {
                pc_to_ac(pdp);
            }

            if( lailiaEnabled && (MB & B12) )
            {
                logger(LOG_1D, "lai\n");
                pdp->lai = 1;
            }

            if( lailiaEnabled && (MB & B13) )
            {
                logger(LOG_1D, "lia\n");
                pdp->lia = 1;
            }

            if(MB & B14)
            {
                pdp->pf |= decflg(MB);
            }
            else
            {
                pdp->pf &= ~decflg(MB);
            }
        }

        if( all1DEnabled && IR_OPR1D )  // wje - 1D new instructions, most of no use but some are
        {
        int tmp;                // might be needed for IIF, IFI
            // Not actually sure which TP these occurred in, would have to dig out of the schematics.
            // That's Angelo's thing, but not mine.
            // Here should be OK.
            // The order is important, though.
            // We're faking what the 1D-45 documentation calls event times 1-4,
            // these are in event time order so they do the correct thing.
            // et 1
            logger(LOG_1D,"opr %o ", IR);
            if( MB & B11 )      // SCI, special clear IO
            {
                logger(LOG_1D, "sci");
                pdp->io = 0;
            }
            if( MB & B12 )      // SCF, special clear pfs
            {
                logger(LOG_1D, "scf");
                pdp->pf = 0;
            }

            // et 2
            // Note that also using SCI and SCF can clear the appropriate targets.
            tmp = pdp->io;
            if( MB & B6 )       // IIF, or pf1-6 into IO, no link or ring bits
            {
                logger(LOG_1D, "iif");
                pdp->io |= pdp->pf;
            }
            if( MB & B7 )       // IFI, or IO into pf1-6, no link or ring bits
            {
                logger(LOG_1D, "ifi");
                pdp->pf |= tmp;
            }

            // et 3
            if( MB & B9 )       // IDA, increment AC
            {
                logger(LOG_1D, "ida");
                inc_ac(pdp);
            }

            logger(LOG_1D, "\n");
        }

        TP(8)

        // TP9: advance the core read/write chain and write MB back to
        // core, completing the instruction word's memory cycle. MB still
        // holds the word just fetched unless an IOT changed it, so
        // writemem() normally skips the store: core already holds that
        // word (readmem() no longer zeroes it). For "jmp"/"jsp" with no
        // indirect addressing, load PC from MB now. Apply the rest of
        // "skp"'s overflow-clear, the last shro() step, opr/law AC
        // complement, the 1D lai/lia transfer, iot's in-out-halt clear,
        // and decide whether to halt
        // (illegal opcode, opr-halt bit, or a stop condition).
        mop2379(pdp);
        writemem(pdp);      // usually skipped: MB is the unchanged instruction word

        if(!pdp->df1 && (IR_JMP || IR_JSP))
        {
            mb_to_pc(pdp);
        }

        if(IR_SKIP && (MB & B8))
        {
            pdp->ov1 = 0;
        }

        if(IR_SHRO && (MB & B15))
        {
            shro(pdp);
        }

        if((IR_OPR && (MB & B8)) || (IR_LAW && (MB & B5)))
        {
            AC ^= WORDMASK;
        }

        // Complete the 1D "lai"/"lia" transfers armed at TP8, after every
        // other operate micro-op, so they see the cleared or complemented
        // registers. They finish in this cycle: left to the next fetch, a
        // sequence break taken in between saved AC and IO without them and
        // the load was lost, and a stop in between showed the old value.
        // Both together swap AC and IO.
        if(pdp->lai || pdp->lia)
        {
            Word t = AC;

            if(pdp->lai)
            {
                AC = IO;
            }

            if(pdp->lia)
            {
                IO = t;
            }

            pdp->lai = 0;
            pdp->lia = 0;
        }

        if(IR_IOT && !pdp->ihs && pdp->ios)
        {
            pdp->ioh = 0;
        }

	if( (IR_OPR && (MB & B9)) || STOP )
        {
            logger(LOG_1D, "hlt, mb %06o IR %o\n", MB, IR);
            pdp->run = 0;
        }

        clrmd(pdp);
        TP(9)

        // TP9A: do another shro() step (bit 14). For an IOT that asked to
        // halt until completion (ioh set), cancel this instruction (it will be retried once the device finishes).
        if(IR_SHRO && (MB & B14))
        {
            shro(pdp);
        }

        if(IR_IOT && pdp->ioh)
        {
            inst_cancel(pdp);
        }

        TP(9a)

        // TP10: end-of-cycle housekeeping common to all cycle types --
        // resync sequence-break requests, clear the core read/write
        // chain, latch the overflow flag, and (if still running) clear MA
        // for the next fetch. Do the final shro() step (bit 13). For
        // IOT, update the in-out-halt/sync flags and fire the device's
        // TP10 completion pulse if ioc is set. Set "sign of MB seen"
        // (smb) for the multiply/divide unit. Finally, decide the next
        // cycle: if a sequence break is pending and may be taken now (see
        // below), take it, cancelling this instruction first if
        // mid-instruction breaks are allowed; otherwise, if this
        // instruction isn't done (it's a memory-reference opcode needing
        // defer/cycle1), set cyc so defer()/cycle1() runs next.
        sbs_reset_sync(pdp);
        memclr(pdp);
        syncov(pdp);

        if(pdp->run)
        {
            clr_ma(pdp);
        }

        if(IR_SHRO && (MB & B13))
        {
            shro(pdp);
        }

        if(IR_IOT)
        {
            if(pdp->ihs)
            {
                pdp->ioh = 1;
            }
            else if(!pdp->ioh)
            {
                pdp->ios = 0;
            }

            if(pdp->ioc)
            {
                iot(pdp, 1);
            }
        }

        if(MB & B0)
        {
            pdp->smb = 1;
        }

        // F17 6-19: a break is taken at the end of any cycle except cycle 0 of a jmp or jsp
        // deferred once. That instruction is neither done nor cancellable here, so its defer
        // cycle runs first and finishes the jump; defer() then takes the break.
        // A channel may take the next cycle at an instruction's end, or mid-instruction with a
        // restart, but not after cycle 0 of a once-deferred jump (hscBreak()).
        if(SBS_BREAK && (CY0_INST_DONE || MIDBRK_PERMIT))
        {
            if(MIDBRK_PERMIT)
            {
                inst_cancel(pdp);
            }
            pdp->bc |= SBS_BREAK;
            pdp->cyc = 1;
            // The cycles run so far were real, cancelled or not; the panel tally keeps them.
            updatelights_pwm(pdp->panel, pdp->inst_cyc);
            pdp->inst_cyc = 0;
            hscGrant = HSCGRANT_BOUNDARY;
        }
        else if(!CY0_INST_DONE)
        {
            hscGrant = (CY0_MIDBRK_PERMIT) ? HSCGRANT_RESTART : HSCGRANT_NONE;
            pdp->cyc = 1;           // multi-cycle instruction continues; keep accumulating inst_cyc
        }
        else
        {
            hscGrant = HSCGRANT_BOUNDARY;

            // Instruction complete: tally settled register state n times (once per
            // cycle the instruction held this state) into pwmcount, then reset.
            if(IR_SHRO)
            {
                // SHRO's remaining 4 shifts (B12-B9) happen at TP0-TP3 of the
                // next cycle0() call using the OLD MB/IR before the new instruction
                // is fetched.  Defer the tally until those shifts finish so that
                // the panel sees the fully-shifted AC/IO, not the intermediate state.
                pdp->sho_deferred = pdp->inst_cyc;
                pdp->inst_cyc = 0;   // reset now; the next cycle's count belongs to the following instruction
            }
            else
            {
                updatelights_pwm(pdp->panel, pdp->inst_cyc);
                pdp->inst_cyc = 0;
            }
        }

        TP(10)
    }
}

// DEFER (indirect addressing) CYCLE: runs when cycle0() set df1, meaning
// the instruction's address word had its indirect bit set. Reads the
// word MA points to; if *its* indirect bit is also set, df2 stays/gets
// set and this cycle repeats (chasing the indirect chain) -- otherwise
// the word becomes the final operand address (for non-jmp/jsp opcodes)
// or the jump target (for jmp/jsp, which finish here rather than in
// cycle1()). Also handles the 16-channel-SBS "DEBREAK" special case: a
// "jmp 1" or "jmp 4n+1" encountered while sbm is set and PC's bank is 0
// is interpreted as a return-from-break instruction rather than a normal
// jump.
static void
defer(PDP1 *pdp)
{
int sbs_restore = 0;

    // TP0: point MA at the address word (from MB, the instruction word
    // fetched in cycle0).
    mb_to_ma(pdp);
    TP(0)

    // TP1
    pdp->emc = 0;
    TP(1)

    // TP2: advance the core read/write chain. Check for the SBS
    // "DEBREAK" pseudo-instruction: "jmp" to address 1 (SBS256) or to a
    // multiple-of-4-plus-1 address naming a channel (16-channel SBS),
    // executed while in a break (epc==0) with sequence breaks enabled.
    // If matched, mark the corresponding break as no longer held (b4) and
    // arrange to restore ov1/exd from the operand word at TP9A instead of
    // treating this as a real jump.
    mop2379(pdp);

    if( pdp->sbm && IR_JMP && (pdp->epc == 0) )
    {
        if(pdp->sbs16)
        {
            if((MB & 07703) == 1)
            {
                pdp->b4 &= ~(1 << ((MB & 074) >> 2));
                pdp->exd = 1;	// DEBREAK - not in #49 because SBS256
                sbs_restore = 1;
                sbs_calc_req(pdp);
            }
        }
        else
        {
            if((MB & 07777) == 1)
            {
                pdp->b3 = 0;
                pdp->b4 = 0;
                pdp->exd = 1;	// same but #55 has it
                sbs_restore = 1;
                sbs_calc_req(pdp);
            }
        }
    }
    TP(2)

    // TP3: advance the core read/write chain and clear MB ready for the
    // address word read at TP4.
    mop2379(pdp);
    MB = 0;
    TP(3)

    // TP4: synchronize sequence-break requests and read the word MA
    // points to into MB -- this is the (possibly still-indirect) address
    // word.
    sbs_sync(pdp);
    readmem(pdp);
    TP(4)

    // TP5: if extend mode is active, extended-memory addressing applies
    // to this operand too.
    if(pdp->exd)
    {
        pdp->emc = 1;
    }

    TP(5)

    // If the word just read also has its indirect bit (B5) set, and we're
    // not in extend mode, chase the indirect chain another level: set df2
    // so this defer cycle repeats, and set inhibit at TP8 as every memory
    // cycle does (see inhibit()) -- nothing else to do this time around.
    if((MB & B5) && (!pdp->exd))
    {
        // TP6
        pdp->df2 = 1;
        TP(6)
        TP(6a)
        mop2379(pdp);
        TP(7)
        inhibit(pdp);
        TP(8)
    }
    else
    {
        // This is the final address: for "jsp" clear AC now (its old
        // value will be stored to AC at TP8 along with PC); advance the
        // core read/write chain.
        TP(6)
        TP(6a)

        // TP7
        if(IR_JSP)
        {
            AC = 0;
        }

        mop2379(pdp);
        TP(7)

        // TP8: set inhibit, as every memory cycle does (see inhibit()).
        // For "jsp"/"jmp", store/clear PC now (cycle0() skipped this
        // because df1 was set).
        inhibit(pdp);

        if(IR_JSP)
        {
            pc_to_ac(pdp), clr_pc(pdp);
        }

        if(IR_JMP)
        {
            clr_pc(pdp);
        }

        TP(8)

        // TP9: for "jsp"/"jmp", the word just read (MB) is the jump
        // target -- load it into PC, completing the instruction here
        // (cycle1() will not run for jmp/jsp).
        if(IR_JSP || IR_JMP)
        {
            mb_to_pc(pdp);
        }

        clrmd(pdp);
    }

    // TP9 (both paths): advance the core read/write chain and write MB
    // back to core, completing the address word's memory cycle. Nothing
    // here changes MB after the read, so writemem() skips the store: core
    // already holds that word (readmem() no longer zeroes it). Stop the
    // machine if a stop condition applies.
    mop2379(pdp);
    writemem(pdp);      // skipped: MB is the unchanged address word
    if( STOP )
    {
        pdp->run = 0;
    }

    TP(9)

    // 3.5us after TP2, shortly before TP9A
    // If this was a DEBREAK, restore ov1/exd from the operand word's
    // bits 0/1 (saved by the interrupted program at break time).
    if(sbs_restore)
    {
        pdp->ov1 = !!(MB & B0);
        pdp->exd = !!(MB & B1);
    }

    TP(9a)

    // TP10: standard end-of-cycle housekeeping (see cycle0() TP10). If a
    // sequence break is pending, take it; otherwise, if the instruction
    // is now fully done (non-jmp/jsp memory reference -- needs cycle1()
    // is decided by INST_DONE), drop cyc back to 0 for cycle0() to run
    // next... unless cycle1() still needs to run, in which case cyc stays
    // set. Either way, df1 is cleared unless we're chasing another level
    // of indirection (df2 set), and df2 itself is always cleared.
    sbs_reset_sync(pdp);
    memclr(pdp);
    syncov(pdp);

    if(pdp->run)
    {
        clr_ma(pdp);
    }

    if(MB & B0)
    {
        pdp->smb = 1;
    }

    if(SBS_BREAK)
    {
        if(MIDBRK_PERMIT)
        {
            inst_cancel(pdp);
        }

        pdp->bc |= SBS_BREAK;
        // The cycles run so far were real, cancelled or not; the panel tally keeps them.
        updatelights_pwm(pdp->panel, pdp->inst_cyc);
        pdp->inst_cyc = 0;
        hscGrant = HSCGRANT_BOUNDARY;
    }
    else if(INST_DONE)
    {
        pdp->cyc = 0;
        updatelights_pwm(pdp->panel, pdp->inst_cyc);
        pdp->inst_cyc = 0;
        hscGrant = HSCGRANT_BOUNDARY;
    }
    else
    {
        // A break is always granted at the end of a defer cycle (F17 3-49), here mid-instruction.
        hscGrant = HSCGRANT_RESTART;
    }

    if(!pdp->df2)
    {
        pdp->df1 = 0;
    }
    pdp->df2 = 0;
    TP(10)
}

// CYCLE 1: execute cycle for memory-reference instructions (opcodes
// < 030) that were not jmp/jsp (those finish in cycle0()/defer()).
// Reads the operand from the (now-resolved) effective address into MB,
// performs the opcode-specific ALU operation against AC/IO, and writes
// any result back to MB/core. "xct" is special-cased at TP3 to jump
// straight into cycle0() (via cychack==4, i.e. starting at TP4) on the
// target instruction instead of doing a normal cycle1. "mul"/"div"
// (type 10 option) run their full algorithms at TP10 once the
// add/subtract-style setup for this cycle has completed.
static void
cycle1(PDP1 *pdp)
{
int hack;

    hack = pdp->cychack;
    pdp->cychack = 0;

    switch(hack)
    {
    default:
        // TP0: "cal"/"jda" force the effective address to 0100 (in bank 0
        // unless extend mode is active) regardless of the address field;
        // other memory-reference instructions use the resolved address
        // from MB. "dis" (divide step) does one div_shift() here as part
        // of its setup.
        if(IR_CALJDA && !(MB & B5))
        {
            MA |= 0100;

            if(!pdp->exd)
            {
                pdp->ema |= pdp->epc;
            }
        }
        else
        {
            mb_to_ma(pdp);
        }

        // EMA stuff
        if(IR_DIS)
        {
            div_shift(pdp);
        }

        TP(0)
        __attribute__((fallthrough));      // deliberate TP-state fall-through, not missing a break

    case 1:
        // TP1
        pdp->emc = 0;
        TP(1)

        // TP2: advance the core read/write chain. "dis" increments AC if
        // the shifted-out IO bit was 0 (part of the divide-step
        // correction). "mul" (single-step multiply-by-1 setup) copies AC
        // into MB and clears IO.
        mop2379(pdp);

        if(IR_DIS && !(IO & B17))
        {
            if(AC == 0777777)
            {
                AC = 1;
            }
            else
            {
                AC++;
            }
        }

        if(IR_MUL)
        {
            MB = AC;
            IO = 0;
        }

        TP(2)

        // TP3: advance the core read/write chain. "mul" ORs the saved AC
        // (now in MB) into IO. Clear MB ready for the operand read at
        // TP4. "xct": instead of reading an operand here, restart this
        // cycle as a cycle-0 fetch (from TP4) of the instruction at the
        // resolved address -- "xct" never reaches its own TP4 onward.
        mop2379(pdp);

        if(IR_MUL)
        {
            IO |= MB;
        }

        MB = 0;

        if(IR_XCT)
        {
            pdp->cyc = 0;
            pdp->cychack = 4;
            cycle0(pdp);
            return;
        }

        TP(3)

        // TP4: synchronize sequence-break requests and read the operand
        // into MB. "sub" (and "dis" when its shifted-out bit was 1)
        // complements AC, turning the upcoming add into a subtract.
        // "lio" clears IO first so the operand can be OR'd in at TP5.
        sbs_sync(pdp);
        readmem(pdp);

        if(IR_SUB || (IR_DIS && (IO & B17)))
        {
            AC ^= WORDMASK;
        }

        if(IR_LIO)
        {
            IO = 0;
        }

        TP(4)

        // TP5: the main ALU step -- and/ior/load-AC-class instructions
        // combine MB into AC, "lio" loads IO, dio/dzm clear MB ready to
        // receive AC/zero at TP7, and add/sub/xor-class instructions
        // detect overflow (ov2) and/or XOR MB into AC (the "add" half of
        // the end-around-carry add, completed by carry() at TP6).
        if(IR_AND)
        {
            AC &= MB;
        }

        if(IR_IOR)
        {
            AC |= MB;
        }

        if(IR_IDX || IR_ISP || IR_LAC)
        {
            AC = MB;
        }

        if(IR_DIO || IR_DZM)
        {
            MB = 0;
        }

        if(IR_LIO)
        {
            IO |= MB;
        }

        if((IR_ADD || IR_SUB) && ((AC & B0) == (MB & B0)))
        {
            pdp->ov2 = 1;
        }

        if(IR_XOR || IR_ADD || IR_SUB || IR_SAD || IR_SAS ||
            IR_DIS || (IR_MUS && (IO & B17)))
        {
            AC ^= MB;
        }

        TP(5)

        // TP6: complete the end-around-carry add for add/sub/dis/mus
        // (the latter only on the final multiply-shift step, IO bit 17
        // set). idx/isp increment AC (the "index" step).
        if(IR_ADD || IR_SUB || IR_DIS || (IR_MUS && (IO & B17)))
        {
            carry(pdp);
        }

        if(IR_IDX || IR_ISP)
        {
            inc_ac(pdp);
        }

        TP(6)
        TP(6a)

        // TP7: advance the core read/write chain and assemble the value
        // that will be written back to core at TP9 -- dac/idx/isp store
        // AC; cal/jda store AC (then clear it, since AC will hold the
        // saved PC after pc_to_ac() at TP8); dap/dio replace just the
        // address/instruction half of MB with AC's address field /
        // instruction-code half; dio replaces all of MB with IO.
        mop2379(pdp);

        if(IR_DAC || IR_IDX || IR_ISP)
        {
            MB = AC;
        }

        if(IR_CALJDA)
        {
            MB = AC, AC = 0;
        }

        if(IR_DAP)
        {
            MB = (MB & 0770000) | (AC & 0007777);
        }

        if(IR_DIP)
        {
            MB = (MB & 0007777) | (AC & 0770000);
        }

        if(IR_DIO)
        {
            MB = IO;
        }

        TP(7)

        // TP8: set inhibit, as every memory cycle does, storing or not
        // -- it is how the write half stores MB's 0 bits, not a
        // suppression of the write (see inhibit()). "mus"
        // does one multiply-shift step. "cal"/"jda" save PC into AC (now
        // cleared) and clear PC, ready to be replaced by the call address
        // at TP9. The skip-class tests (sas/sad/isp) bump PC past the
        // next instruction when their condition holds.
        inhibit(pdp);

        if(IR_MUS)
        {
            mul_shift(pdp);
        }

        if(IR_CALJDA)
        {
            pc_to_ac(pdp), clr_pc(pdp);
        }

        if((IR_SAS && (AC == 0)) || (IR_SAD && (AC != 0)) || (IR_ISP && (!(AC & B0))))
        {
            pc_inc(pdp);
        }

        TP(8)

        // TP9: advance the core read/write chain and write MB back to
        // core. dac/dap/dio/dip/idx/isp/cal/jda (and dzm) put a new word
        // in MB at TP5/TP7, so writemem() stores it; for the rest MB is
        // normally still the operand read at TP4, and writemem() skips
        // the store because core already holds it. "cal"/"jda" load PC
        // from the call address (MA, set up at TP0). add/sub clear the
        // deferred-overflow flag if the result's sign matches the
        // operand's (no overflow after all). sub/dis-with-borrow
        // complement AC back. sad/sas complete their compare by XORing
        // the operand back in (restoring AC, with the skip at TP8 already
        // having recorded the outcome). Check for a stop condition.
        mop2379(pdp);
        writemem(pdp);      // stores only if MB now differs from the operand read

        if(IR_CALJDA)
        {
            ma_to_pc(pdp);
        }

        if((IR_ADD || IR_SUB) && ((AC & B0) == (MB & B0)))
        {
            pdp->ov2 = 0;
        }

        if(IR_SUB || (IR_DIS && (IO & B17)))
        {
            AC ^= WORDMASK;
        }

        if(IR_SAD || IR_SAS)
        {
            AC ^= MB;
        }

        if( STOP )
        {
            pdp->run = 0;
        }

        clrmd(pdp);
        TP(9)

        // TP9A: add/dis normalize a result of all-ones (negative zero)
        // back to positive zero. cal/jda finish by bumping PC past the
        // call instruction (so the called routine's "return" via that
        // location resumes after the call).
        if((IR_ADD || IR_DIS) && (AC == 0777777))
        {
            AC = 0;
        }

        if(IR_CALJDA)
        {
            pc_inc(pdp);
        }

        TP(9a)

        // TP10: standard end-of-cycle housekeeping (see cycle0() TP10).
        // cycle1 always returns to cycle0 next (cyc cleared) unless a
        // sequence break is being taken, in which case bc records it for
        // brkcycle(). If "mul"/"div" (type 10 option), this is where
        // their full algorithms actually run: set up AC/IO/MB signs per
        // the option's flow chart and call multiply()/divide(), then
        // back out the 200ns "delay to TP0" that would otherwise be
        // double-counted.
        sbs_reset_sync(pdp);
        memclr(pdp);
        syncov(pdp);
        pdp->cyc = 0;

        if(pdp->run)
        {
            clr_ma(pdp);
        }

        if(MB & B0)
        {
            pdp->smb = 1;
        }

        if(SBS_BREAK)
        {
            pdp->bc |= SBS_BREAK;
        }
        else
        {
            pdp->cyc = 0;
        }

        // The instruction is done either way, so its cycles are tallied. A multiply or divide
        // finishes inside this cycle, so a channel request made during it is granted after it
        // (F17 6-19).
        updatelights_pwm(pdp->panel, pdp->inst_cyc);
        pdp->inst_cyc = 0;
        hscGrant = HSCGRANT_BOUNDARY;

        if(IR_MUL)
        {
            if(MB & B0)
            {
                MB ^= WORDMASK;
            }

            if(IO & B0)
            {
                IO ^= WORDMASK;
                pdp->srm = 1;
            }

            pdp->scr |= 1;
            AC = 0;
            multiply(pdp);
            // without delay to TP0
            pdp->simtime -= 200;
        }

        if(IR_DIV)
        {
            if(!(MB & B0))
            {
                MB ^= WORDMASK;
            }

            if(AC & B0)
            {
                AC ^= WORDMASK;
                IO ^= WORDMASK;
                pdp->srm = 1;
            }

            divide(pdp);
            // without delay to TP0
            pdp->simtime -= 200;
        }

        TP(10)
    }
}

// SEQUENCE BREAK CYCLE: runs (instead of cycle0/defer/cycle1) when bc is
// nonzero, i.e. a sequence break is being serviced. bc walks 1->2->3->0
// over four consecutive cycles (one brkcycle() call each):
//   bc==1: save AC and PC to the channel's two memory locations (for
//          16-channel SBS, also compute the break-entry address from the
//          requesting channel number).
//   bc==2: save IO to the next location.
//   bc==3: save ov1/exd flags to the next location, then return to cyc0.
//   bc==0: (not actually entered here) normal operation resumes.
// Each sub-cycle also increments PC, so after all three save cycles PC
// points at the location after the three saved words -- the start of the
// channel's interrupt routine.
static void
brkcycle(PDP1 *pdp)
{
int be;
int r;

    // TP0: do a shro() step if mid-instruction (bit 12). For bc==1
    // (16-channel SBS), compute the break entry address: bits 2-5 of MA
    // become the requesting channel's number (the position of the lowest
    // set bit in sbs_seq). For bc==2/3, point MA at PC (the next save
    // location, following on from where bc==1 left MA).
    if(IR_SHRO && (MB & B12))
    {
        shro(pdp);
    }

    if((pdp->bc == 1) && pdp->sbs16)
    {
        be = 0;

        for(r = pdp->sbs_seq; !(r & 1); r >>= 1)
        {
            be++;
        }

        MA |= be << 2;
    }

    if(pdp->bc == 2 || pdp->bc == 3)
    {
        pc_to_ma(pdp);
    }

    TP(0)

    // TP1: another shro() step (bit 11) if mid-instruction.
    if(IR_SHRO && (MB & B11))
    {
        shro(pdp);
    }

    pdp->emc = 0;
    TP(1)

    // TP2: advance the core read/write chain, another shro() step
    // (bit 10), and the same iot in-out sync bookkeeping as cycle0()
    // TP2 (in case an iot was interrupted by this break).
    mop2379(pdp);

    if(IR_SHRO && (MB & B10))
    {
        shro(pdp);
    }

    if(IR_IOT)
    {
        pdp->ioc = !pdp->ioh && !pdp->ihs;
    }

    pdp->ihs = 0;
    TP(2)

    // TP3: advance the core read/write chain, another shro() step
    // (bit 9), and clear MB for the read at TP4.
    mop2379(pdp);

    if(IR_SHRO && (MB & B9))
    {
        shro(pdp);
    }

    MB = 0;
    TP(3)

    // TP4: synchronize sequence-break requests and read the word at the
    // save location into MB (its old contents, about to be overwritten).
    // For bc==1, also clear IR -- the break-entry instruction slot starts
    // fresh.
    sbs_sync(pdp);
    readmem(pdp);
    if(pdp->bc == 1)
    {
        IR = 0;
    }
    TP(4)

    // TP5: for bc==3 (saving ov1/exd), start with a clear word -- the
    // flag bits get OR'd in at TP9A.
    if(pdp->bc == 3)
    {
        MB = 0;
    }

    TP(5)

    // TP6
    TP(6)
    TP(6a)

    // TP7: advance the core read/write chain and load the value to be
    // saved into MB: bc==1 saves AC (and clears it, ready for pc_to_ac()
    // at TP8); bc==2 saves AC again... actually IO is saved at bc==2 via
    // the OR below for bc==3; bc==2 saves AC's *replacement*, which here
    // is just AC (IO is saved the cycle after); bc==3 ORs IO into the
    // cleared MB from TP5.
    mop2379(pdp);

    if(pdp->bc == 1)
    {
        MB = AC, AC = 0;
    }

    if(pdp->bc == 2)
    {
        MB = AC;
    }

    if(pdp->bc == 3)
    {
        MB |= IO;
    }

    TP(7)

    // TP8: set inhibit, as every memory cycle does (see inhibit()); the
    // save *is* the write-back at TP9. For bc==1, store the
    // (overflow/extend/PC) "saved PC" word into AC and clear PC, ready to
    // be replaced by the break-entry address at TP9.
    inhibit(pdp);

    if(pdp->bc == 1)
    {
        pc_to_ac(pdp), clr_pc(pdp);
    }

    TP(8)

    // TP9: advance the core read/write chain and write the saved value
    // (MB) to the save location. For bc==1, load PC with the break-entry
    // address (MA, computed at TP0). Honor SINGLE CYCLE / RUN ENABLE OFF
    // by stopping after this sub-cycle.
    mop2379(pdp);
    writemem(pdp);      // stores unless the saved value equals the word already there

    if(pdp->bc == 1)
    {
        ma_to_pc(pdp);
    }

    if(pdp->single_cyc_sw || !pdp->run_enable)
    {
        pdp->run = 0;
    }

    clrmd(pdp);
    TP(9)

    // TP9A: advance PC to the next save location (or, after bc==3, to the
    // first instruction of the interrupt routine).
    pc_inc(pdp);
    TP(9a)

    // TP10: standard end-of-cycle housekeeping (see cycle0() TP10). After
    // bc==1, the entered routine starts in extend mode off. After bc==3,
    // the break sequence is complete and normal cycle0() execution
    // resumes (cyc cleared) at the address now in PC. Advance bc to the
    // next sub-cycle (1->2->3->0).
    sbs_reset_sync(pdp);
    memclr(pdp);
    syncov(pdp);

    if(pdp->run)
    {
        clr_ma(pdp);
    }

    if(MB & B0)
    {
        pdp->smb = 1;
    }

    if( pdp->bc == 1 )
    {
        pdp->exd = 0;
        pdp->sho_deferred = 0;      // discard any pending deferred SHRO tally
        // inst_cyc already counts from break cycle 1: the break was taken with it at 0.
    }

    // SBS256: 1->EXD, HOLD BREAK, JSP->IR, 1->df1, JE->MA (delayed)
    if(pdp->bc == 3)
    {
        pdp->cyc = 0;
        updatelights_pwm(pdp->panel, pdp->inst_cyc);    // tally break sequence cycles
        pdp->inst_cyc = 0;
    }

    // No break of any kind from break cycle 1 until the jump into the routine is done (F17 6-19,
    // 3-50); that jump's cycle 0 sets the grant at its own TP10.
    hscGrant = HSCGRANT_NONE;

    pdp->bc = (pdp->bc + 1) & 3;
    TP(10)
}

// Fast, non-cryptographic 32-bit PRNG (Marsaglia xorshift), used only to
// pick cycle()'s per-cycle panel sample point (see TP() above and
// cycle() below). Statistical quality is irrelevant for that purpose;
// the point is to avoid rand()/random()'s process-wide glibc lock and a
// modulo divide on this 200kHz hot path (audit O1, Phase 5). Statically
// seeded with a fixed nonzero constant -- true randomness is not needed
// here, and a zero seed would leave xorshift stuck at zero forever.
// Returns the next 32-bit value in the sequence.
static uint32_t
xorshift32(void)
{
    static uint32_t state = 2463534242u;    // fixed nonzero seed

    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return(state);
}

// Top-level dispatcher: run exactly one 5us machine cycle. Picks a
// random "panel sample point" (timernd) for this cycle so the TP(n)
// macros latch the front-panel lights at a different timing pulse each
// time, then runs whichever of brkcycle/cycle0/defer/cycle1 applies to
// the machine's current state (sequence-break cycle, fetch, indirect
// defer, or execute, respectively).
void
cycle(PDP1 *pdp)
{
    pdp->inst_cyc++;
    // audit O1: xorshift32() plus a multiply-shift range reduction
    // (Lemire-style) replaces rand() % TP_unreachable here; see
    // xorshift32()'s header comment above for the rationale.
    pdp->timernd = (int)(((uint64_t)xorshift32() * TP_unreachable) >> 32);

    if(pdp->bc)
    {
        brkcycle(pdp);
    }
    else if(!pdp->cyc)
    {
        cycle0(pdp);
    }
    else if(pdp->df1)
    {
        defer(pdp);
    }
    else
    {
        cycle1(pdp);
    }

    // update any IOTs regardless of cycle type
    dynamicIotProcessorDoPoll(pdp);
}

// ---- Throttle pacing (22-Sep-2026) ----
//
// The pacing policy (the wait/spin/cap loop below) lives here, where main.c's other timing code
// expects it. Only the mechanics of an ad1/fastload request cutting a wait short are in
// ad1server.c, behind ad1ThrottleWait(), since that needs wakeFd and serverActive, both private
// to that file.
static uint64_t throttleQuantumNs = 1000000;    // 1000us: today's cadence by default
static uint64_t throttleSpinNs;                 // 0 = off by default
static uint64_t throttleMaxLagNs = 20000000;    // 20ms: see throttle()'s header comment below
static uint64_t throttleBurstNs = 100000;       // 100us: see throttleConfigure()
long throttleCapFirings;

#define THROTTLE_BURST_DEFAULT_US 100
#define THROTTLE_BURST_MAX_US 250       // the 340's PACE_MAX_LAG (type340emu.c)

// Set the pacing tunables and reduce this thread's timer slack so a short wait lands close
// to where it was asked for (precedent: newpanel.c's absolute-deadline clock_nanosleep loop,
// panel_pidp1/newpanel.c). Called from configure() (main.c), which parses the throttlequantum/
// throttlespin/throttlemaxlag config extras and runs on the emulator thread -- prctl(
// PR_SET_TIMERSLACK) is per-thread, so it must land there -- both at startup and on every SIGHUP
// reconfigure. quantumNs of 0 is treated as 1000ns rather than literally zero, which would turn
// every iteration below into a syscall instead of a wait.
//
// throttleburst is read here, as audio.c reads audiodepth, rather than passed in: that keeps this
// function's declaration in pdp1.h, and so every plugin build, as it is. It is how far, in
// microseconds, simtime must lead the wall clock before throttle() waits at all; 0 waits at any
// lead. Its ceiling is the 340's PACE_MAX_LAG: that thread is paced by the wall clock and reads
// the CPU's flags and core at wall time, and it absorbs a program-time lead only while the lead
// and an oversleep together stay under that.
void
throttleConfigure(uint64_t quantumNs, uint64_t spinNs, uint64_t maxLagNs)
{
ConfigurationSettingP settingP;

    throttleQuantumNs = (quantumNs) ? quantumNs : 1000;
    throttleSpinNs = spinNs;
    throttleMaxLagNs = maxLagNs;

    throttleBurstNs = (THROTTLE_BURST_DEFAULT_US * 1000);
    if( (settingP = findConfigurationSetting(getConfiguration(), "throttleburst")) )
    {
        if( settingP->strvalueP || (settingP->ivalue < 0) )
        {
            fprintf(stderr, "pidp1: throttleburst must be a non-negative number; using %d\n",
                THROTTLE_BURST_DEFAULT_US);
        }
        else if( settingP->ivalue > THROTTLE_BURST_MAX_US )
        {
            fprintf(stderr, "pidp1: throttleburst is at most %d; using %d\n",
                THROTTLE_BURST_MAX_US, THROTTLE_BURST_MAX_US);
            throttleBurstNs = (THROTTLE_BURST_MAX_US * 1000);
        }
        else
        {
            throttleBurstNs = ((uint64_t)settingP->ivalue * 1000);
        }
    }

    prctl(PR_SET_TIMERSLACK, 1);
}

// The throttle wait: sleep or spin until real time catches up with simulated time (the lag cap
// may move simtime first), in chunks of at most throttlequantum so a waiting ad1/fastload
// request is served within one quantum rather than within a whole millisecond (the request
// itself is served through ad1ThrottleWait() -> ad1Service(), ad1server.c). Simulated time is
// not advanced here, only clamped forward by the lag cap when badly behind.
//
// Each chunk's length is the deadline minus a freshly measured "now", not a fixed guess, so a
// chunk never overshoots the deadline by more than the current spin budget: this is the
// equivalent of a clock_nanosleep(TIMER_ABSTIME) deadline sleep, adapted because the underlying
// wait needs to be cut short by a request, and ppoll's timeout -- which is what makes that
// possible -- is always relative; Linux has no absolute-deadline variant of an fd-interruptible
// wait.
//
// The lag cap: if real time has pulled more than throttlemaxlag ahead of simtime -- a host
// stall, a SIGSTOP, anything that leaves the machine far behind -- the rest of the deficit is
// forgiven by moving simtime forward instead of running an unpaced burst to close the whole
// gap. Every firing counts in throttleCapFirings, read by main.c's timing report: silently
// forgiving simulated time would otherwise look like nothing more than a small shortfall in
// the average-rate measurement.
void
throttle(PDP1 *pdp)
{
uint64_t remaining;
uint64_t sleepNs;
int64_t lag;

    lag = ((int64_t)pdp->realtime - (int64_t)pdp->simtime);
    if( lag > (int64_t)throttleMaxLagNs )
    {
        pdp->simtime = (pdp->realtime - throttleMaxLagNs);
        ++throttleCapFirings;
    }

    // Run on until simtime leads by throttleburst, so one wait covers a burst of cycles rather
    // than the one to five a wait at any lead allowed, each a syscall. realtime is the last
    // wake-up's, never later than the wall clock, so the true lead here never exceeds
    // throttleburst.
    if( (pdp->simtime > pdp->realtime) && ((pdp->simtime - pdp->realtime) < throttleBurstNs) )
    {
        return;
    }

    while( pdp->realtime < pdp->simtime )
    {
        remaining = (pdp->simtime - pdp->realtime);
        if( remaining <= throttleSpinNs )
        {
            // Final stretch: spin rather than pay a syscall's imprecision. Still let a waiting
            // request in cheaply -- ad1WorkPending() is one relaxed atomic load -- rather than
            // making it wait out the spin.
            if( ad1WorkPending() )
            {
                ad1Service(pdp);
            }

            pdp->realtime = gettime();
            continue;
        }

        sleepNs = (remaining - throttleSpinNs);
        if( sleepNs > throttleQuantumNs )
        {
            sleepNs = throttleQuantumNs;
        }

        ad1ThrottleWait(pdp, sleepNs);
        pdp->realtime = gettime();
    }
}

// Fire one pulse (TP7 if pulse==0, TP10 if pulse==1) of the IOT device
// numbered "dev" (the low 6 bits of the iot instruction's MB), with nac
// indicating whether the wait bits (B5/B6) were // set.
// "ch" (bits 6-11 of MB) is the sub-channel/argument field used by devices,
// the meaning is specific to the device.
// Each case below implements one device's reaction to this pulse; devices handled by a dynamically
// loaded IOT sare tried first so it can override any.
// If not, fall through to built-ins below.
// If none matches, the IOT is an illegal one and is ignored.
//
// pulse=0: TP7
// pulse=1: TP10
static void
iot_pulse(PDP1 *pdp, int pulse, int dev, int nac)
{
int ch;

    ch = (MB >> 6) & 077;

    // Try to find a dynamically-loaded IOT first, allows overriding all of the silly baked-in stuff.
    if( !dynamicIotProcessor(pdp, dev, pulse, nac) )
    {
        switch(dev)
        {
        case 000:
            break;

        case 011:   // spacewar controllers -- read the two controller boxes' switches into IO
            // simple but stupid version for now
            if(pulse)
            {
                // LRTF
                IO |= (pdp->spcwar1 << 14) | pdp->spcwar2;
            }
            break;

        case 030:   // rrb -- reader read buffer: copy the assembled reader byte/word into IO
            if(pulse)
            {
                IO |= pdp->rb;
                pdp->rbs = 0;
            }
            break;

        case 033:   // cks -- check status: report reader/typewriter/punch/SBS status bits into IO
            if(pulse)
            {
                IO |= pdp->rbs << 16;
                IO |= (!pdp->tyo) << 15;
                IO |= pdp->tbs << 14;
                IO |= (!pdp->punon) << 13;
                IO |= pdp->sbm << 11;
                IO |= pdp->cksflags;        // wje - needed to generalize use, many devices use it
            }
            break;

        case 050:   // dsc -- Disable Sequence-break Channel "ch" (16-channel SBS)
            if(!pulse)
            {
                if(pdp->sbs16 && ch < 16)
                {
                    pdp->b1 &= ~(1 << ch);
                }
            }
            break;

        case 051:   // asc -- Allow (enable) Sequence-break Channel "ch" (16-channel SBS)
            if(!pulse)
            {
                if(pdp->sbs16 && ch < 16)
                {
                    pdp->b1 |= (1 << ch);
                }
            }
            break;

        case 052:   // isb -- Initiate Sequence Break, software-triggered request
            // Not gated by the channel's "on" bit (b1): F25 says the Type 20's isb is unconditional.
            // The single-channel system has isb too (the CHM sequence break tech note, from drawing
            // D9-1). It has no channels, so any channel field is ignored there.
            if(!pulse)
            {
                if(pdp->sbs16)
                {
                    if(ch < 16)
                    {
                        B2_OR(pdp, (1 << ch));
                    }
                }
                else
                {
                    B2_OR(pdp, 1);
                }
            }
            break;

        case 053:   // cac -- Clear All Channels (disable all SBS channels, 16-channel SBS)
            if(!pulse)
            {
                if(pdp->sbs16)
                {
                    pdp->b1 = 0;
                }
            }
            break;

        case 054:   // lsm -- Leave Sequence break Mode (disable sequence breaks)
            if(!pulse)
            {
                pdp->sbm = 0;
            }
            break;

        case 055:   // esm -- Enter Sequence break Mode (enable sequence breaks)
            if(!pulse)
            {
                pdp->sbm = 1;
            }
            break;

        case 056:   // cbs -- Clear sequence Breaks System (clear all pending/held requests)
            if(!pulse)
            {
                clr_sbs(pdp);
            }
            break;

        case 074:   // lem/eem -- Leave/Enter Extend Mode, selected by MB bit 6
            if(pulse)
            {
                pdp->exd = !!(MB & B6);
            }
            break;

        default:
            logger(LOG_IOT, "unknown IOT %06o\n", MB);
            break;
        }
    }
}

// Decode an IOT instruction's device number and nac, clear, B5/B6) bits from MB,
// clear IO at TP7 for the 030-037 ("group 3") devices that share that convention,
// and dispatch the pulse to iot_pulse().
static void
iot(PDP1 *pdp, int pulse)
{
int nac;
int dev;

    nac = ((MB & (B5 | B6)) == B5) || ((MB & (B5 | B6)) == B6);
    dev = MB & 077;

    // 0 -> IO ON IOT also available for other devices
    if( !pulse && ((dev & 070) == 030) )
    {
        IO = 0;
    }

    iot_pulse(pdp, pulse, dev, nac);
}

// Request a sequence break on channel "chan": for 16-channel SBS, set
// the channel's bit in b2 (the raw-request register) only if that
// channel is currently enabled (b1); for SBS256, there's only one
// request line, so just set b2.
// Runs on the emulator thread and on the Type 340's thread (see B2_OR), so b1 is read atomically too.
static void
req(PDP1 *pdp, int chan)
{
    if(pdp->sbs16)
    {
        B2_OR(pdp, __atomic_load_n(&pdp->b1, __ATOMIC_RELAXED) & (1 << chan));
    }
    else
    {
        B2_OR(pdp, 1);
    }
}

// Let the dynamic IOT code trigger a break
void
dynamicReq(PDP1 *pdp, int chan)
{
    req(pdp, chan);             // wje - because req() is private
}
