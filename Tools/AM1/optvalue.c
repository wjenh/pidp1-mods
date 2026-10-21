/*
 * The am1 optimizer's value measurement (-O=values): what AC and IO hold at
 * each word, asked of a whole basic block at a time.  T4, T6, T7, T8a-d and
 * T14 are special cases of that question that look only at a neighboring
 * pair; this counts what the block-wide answer finds.  Advisory only: it
 * creates no finding, adds nothing to the .opt report, and no optimization
 * level reads anything it computes.
 *
 * It reads the word table, the decode, the reference edges and flags
 * (optrefs.c), the basic blocks (optflow.c) and the region flag, and never
 * the parse tree.  optDumpValues() runs the pass twice, once as the effect
 * table says and once with every unknown in-out transfer assumed to touch IO
 * alone (only counted, as a blind spot), and frees everything before it
 * returns.
 *
 * Each block starts with AC and IO unknown.  A register holds a value number,
 * a constant or an opaque value two places can share.  A memory fact (word W
 * of the block's bank holds value v) dies at a write to W, at any indirect
 * write, xct, unknown in-out transfer or spare code, at a word the program
 * writes or patches (its run-time bits are not the assembled ones), and at
 * the end of the block, so at every skip.  A word never written, patched or
 * taken is read as its assembled constant without a fact.
 *
 * The dead classes (dload, dclear) are the one place the pass looks forward:
 * a word is held until its register is read (needed) or written (dead), and
 * the block's end counts as a read, so nothing is called dead because the
 * pass lost sight of it.  Every hit in a block can be taken at once: a
 * redundant word changes neither register, a lai or lia counts as reading
 * the register it copies, and a swap's first word counts as read.
 *
 * The carried pass (-O=carry, task A52) is the same walk with what a block
 * knows at its entry carried over the edges into it: a single predecessor's
 * state, or at a merge what every predecessor agrees on, solved over loops by
 * iterating until nothing changes.  Its values are named, not counted: a
 * value is named by the word and slot that made it, a block's unknown entry
 * value by the block and register.  Each name stands for the latest instance
 * of its value, so making a value again first takes the name from every place
 * that still holds the older one; a name that cannot mean one value on every
 * path is never kept.  A call's return knows nothing (leg 1), what the callee's
 * summary proves (leg 2, optpreserve.c), or what an oracle grants (leg 3); leg
 * 0 resets every block and is A21's pass, the control.  optCarryBody() runs
 * the same walk over one routine's body for its summary.  Each main leg also
 * counts the words optscratch.c calls private that could be kept in IO
 * instead of memory, and the stores its hits leave dead (parkLeg()).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "optimizer.h"

// The effect table.  Each row cites the F-15D handbook page that settles it
// (printed page numbers; the PDF page is one higher) or, for a PDP-1D word,
// the 1D docs.  Every device iot is unknown for both registers and memory,
// which is always safe; only the wait, eem and lem do nothing.

// What a row's instruction does.  KILL: effect unknown, so both registers
// become unknown and every memory fact dies.  ENDS: the word ends its basic
// block, so what it does after that does not matter here.
#define VE_RAC      0x0001      // reads AC
#define VE_RIO      0x0002      // reads IO
#define VE_RMEM     0x0004      // reads the memory word it addresses
#define VE_WAC      0x0008      // writes AC
#define VE_WIO      0x0010      // writes IO
#define VE_WMEM     0x0020      // writes the whole memory word it addresses
#define VE_PMEM     0x0040      // writes part of the memory word it addresses
#define VE_KILL     0x0080      // effect unknown: both registers and every fact die
#define VE_ENDS     0x0100      // ends the basic block
#define VE_FLAGS    0x0200      // reads or writes the program flags only

// What value a written register or word receives.
typedef enum
{
    VS_NONE,        // nothing the pass tracks: jmp, the flags, eem, a wait
    VS_FRESH,       // every register and word written receives a value no other
                    // place holds (add, a shift, jsp, mul)
    VS_LOADAC,      // lac: AC receives the value of the memory word
    VS_LOADIO,      // lio: IO receives it
    VS_STOREAC,     // dac: the memory word receives AC's value
    VS_STOREIO,     // dio: the memory word receives IO's value
    VS_ZERO,        // dzm: the memory word receives zero
    VS_PARTIAL,     // dap, dip: the memory word receives a value no place holds
    VS_INDEX,       // idx, isp: the word and AC receive the same new value
    VS_LAW,         // law: AC receives the constant in the word
    VS_OPERATE,     // the operate group: evaluated micro-op by micro-op
    VS_SPECIAL,     // the PDP-1D special operate group, likewise
    VS_UNKNOWN,     // an unknown effect: VE_KILL
    VS_COUNT
} VSem;

// A row the lookup never matches: a micro-op of the operate or special
// group, whose word is evaluated as a whole, or the sequence break.
#define VG_NONE     (-1)

typedef struct
{
    const char *nameP;      // the mnemonic, or the name of a class of words
    int group;              // the OptGroup the lookup matches, or VG_NONE
    int code;               // memory reference: the decoded opcode;
                            // shift: the register field; in-out: the exact
                            // word, -1 for "any other"; operate and special:
                            // the micro-op bits
    int ibit;               // memory reference: the bit 5 that selects the row,
                            // -1 when either does
    unsigned int effects;   // VE_ bits
    VSem sem;
    const char *whatP;      // the effect, in one line
    const char *citeP;      // where it is settled
} VRow;

// The citations.  Handbook pages are the printed ones.
#define CITE_1D     "Docs/UsingPDP-1DInstructions.md"
#define CITE_1DTIME "Docs/UsingPDP-1DInstructions.md"

static const VRow effectRows[] =
{
    // memory reference group
    { "and", OPTG_MEMREF, 002, -1, (VE_RAC | VE_RMEM | VE_WAC), VS_FRESH,
      "AC := AC and C(Y)", "F-15D p.16" },
    { "ior", OPTG_MEMREF, 004, -1, (VE_RAC | VE_RMEM | VE_WAC), VS_FRESH,
      "AC := AC or C(Y)", "F-15D p.16" },
    { "xor", OPTG_MEMREF, 006, -1, (VE_RAC | VE_RMEM | VE_WAC), VS_FRESH,
      "AC := AC xor C(Y)", "F-15D p.16" },
    { "xct", OPTG_MEMREF, 010, -1, VE_KILL, VS_UNKNOWN,
      "executes C(Y) in place; its effect is the target's, which this pass does not follow", "F-15D p.17" },
    { "cal", OPTG_MEMREF, 016, 0, (VE_RAC | VE_WAC | VE_ENDS), VS_FRESH,
      "jda 100: C(AC) to 100, the return point to AC, jump to 101", "F-15D p.18" },
    { "jda", OPTG_MEMREF, 016, 1, (VE_RAC | VE_WAC | VE_ENDS), VS_FRESH,
      "C(AC) to Y, the return point to AC, jump to Y+1", "F-15D p.18" },
    { "lac", OPTG_MEMREF, 020, -1, (VE_RMEM | VE_WAC), VS_LOADAC,
      "AC := C(Y)", "F-15D p.17" },
    { "lio", OPTG_MEMREF, 022, -1, (VE_RMEM | VE_WIO), VS_LOADIO,
      "IO := C(Y)", "F-15D p.17" },
    { "dac", OPTG_MEMREF, 024, -1, (VE_RAC | VE_WMEM), VS_STOREAC,
      "C(Y) := AC", "F-15D p.17" },
    { "dap", OPTG_MEMREF, 026, -1, (VE_RAC | VE_PMEM), VS_PARTIAL,
      "bits 6-17 of Y := bits 6-17 of AC; the rest of Y is kept", "F-15D p.17" },
    { "dip", OPTG_MEMREF, 030, -1, (VE_RAC | VE_PMEM), VS_PARTIAL,
      "bits 0-5 of Y := bits 0-5 of AC; the rest of Y is kept", "F-15D p.17" },
    { "dio", OPTG_MEMREF, 032, -1, (VE_RIO | VE_WMEM), VS_STOREIO,
      "C(Y) := IO", "F-15D p.17" },
    { "dzm", OPTG_MEMREF, 034, -1, VE_WMEM, VS_ZERO,
      "C(Y) := 0", "F-15D p.17" },
    { "add", OPTG_MEMREF, 040, -1, (VE_RAC | VE_RMEM | VE_WAC), VS_FRESH,
      "AC := AC + C(Y), overflow may set", "F-15D p.15" },
    { "sub", OPTG_MEMREF, 042, -1, (VE_RAC | VE_RMEM | VE_WAC), VS_FRESH,
      "AC := AC - C(Y), overflow may set", "F-15D p.15" },
    { "idx", OPTG_MEMREF, 044, -1, (VE_RMEM | VE_WMEM | VE_WAC), VS_INDEX,
      "C(Y) := C(Y) + 1, and the result to AC as well", "F-15D p.16" },
    { "isp", OPTG_MEMREF, 046, -1, (VE_RMEM | VE_WMEM | VE_WAC | VE_ENDS), VS_INDEX,
      "as idx, then skip if the result is positive", "F-15D p.16" },
    { "sad", OPTG_MEMREF, 050, -1, (VE_RAC | VE_RMEM | VE_ENDS), VS_NONE,
      "skip if AC differs from C(Y)", "F-15D p.18" },
    { "sas", OPTG_MEMREF, 052, -1, (VE_RAC | VE_RMEM | VE_ENDS), VS_NONE,
      "skip if AC equals C(Y)", "F-15D p.18" },
    { "mul", OPTG_MEMREF, 054, -1, (VE_RAC | VE_RIO | VE_RMEM | VE_WAC | VE_WIO), VS_FRESH,
      "AC:IO := AC x C(Y); mus, the step without the option, reads IO too, so IO is read", "F-15D p.15; mus p.28" },
    { "div", OPTG_MEMREF, 056, -1, (VE_RAC | VE_RIO | VE_RMEM | VE_WAC | VE_WIO | VE_ENDS), VS_FRESH,
      "AC:IO / C(Y), quotient to AC, remainder to IO, skip unless it overflows; dis likewise", "F-15D p.15; dis p.28" },
    { "jmp", OPTG_MEMREF, 060, -1, VE_ENDS, VS_NONE,
      "PC := Y", "F-15D p.17" },
    { "jsp", OPTG_MEMREF, 062, -1, (VE_WAC | VE_ENDS), VS_FRESH,
      "AC := the return point (with overflow and extend), PC := Y; AC is written, not read", "F-15D pp.17-18" },

    // law, the skip group and the shift group
    { "law", OPTG_LAW, 070, -1, VE_WAC, VS_LAW,
      "AC := N, or with bit 5 its complement -N", "F-15D p.18" },
    { "skip", OPTG_SKIP, 064, -1, (VE_RAC | VE_RIO | VE_ENDS), VS_NONE,
      "sza spa sma read AC, spi (and the 1D sni) read IO, szo szs szf neither; all end the block", "F-15D pp.20-21" },
    { "shift ac", OPTG_SHIFT, 1, -1, (VE_RAC | VE_WAC), VS_FRESH,
      "ral rar sal sar: AC rotated or shifted", "F-15D pp.18-19" },
    { "shift io", OPTG_SHIFT, 2, -1, (VE_RIO | VE_WIO), VS_FRESH,
      "ril rir sil sir: IO rotated or shifted", "F-15D pp.18-19" },
    { "shift both", OPTG_SHIFT, 3, -1, (VE_RAC | VE_RIO | VE_WAC | VE_WIO), VS_FRESH,
      "rcl rcr scl scr: AC and IO as one 36-bit register", "F-15D pp.19-20" },
    { "shift none", OPTG_SHIFT, 0, -1, 0, VS_NONE,
      "the register field selects neither register", "F-15D p.18" },

    // the operate group, one row per micro-op
    { "opr", OPTG_OPERATE, 0, -1, 0, VS_OPERATE,
      "micro-ops act in phase order: cla cli; cmi lat lap flags; cma hlt; lai lia next fetch",
      "F-15D p.21 (opr 3200); " CITE_1DTIME },
    { "cla", VG_NONE, 0000200, -1, VE_WAC, VS_OPERATE,
      "AC := 0, first (TP7)", "F-15D p.22" },
    { "cli", VG_NONE, 0004000, -1, VE_WIO, VS_OPERATE,
      "IO := 0, first (TP7)", "F-15D p.21" },
    { "lat", VG_NONE, 0002000, -1, (VE_RAC | VE_WAC), VS_OPERATE,
      "AC := AC or TW (TP8)", "F-15D p.21" },
    { "lap", VG_NONE, 0000100, -1, (VE_RAC | VE_WAC), VS_OPERATE,
      "AC := AC or PC and overflow (TP8)", "F-15D p.21" },
    { "cma", VG_NONE, 0001000, -1, (VE_RAC | VE_WAC), VS_OPERATE,
      "AC := not AC (TP9)", "F-15D p.21" },
    { "hlt", VG_NONE, 0000400, -1, (VE_RAC | VE_RIO | VE_ENDS), VS_OPERATE,
      "halt; the console shows both registers, so both are read", "F-15D p.22" },
    { "clf stf", VG_NONE, 0000017, -1, VE_FLAGS, VS_OPERATE,
      "clear or set a program flag (TP8)", "F-15D p.22" },
    { "nop", VG_NONE, 0000000, -1, 0, VS_OPERATE,
      "opr with no micro-op does nothing", "F-15D p.22" },
    { "cmi", VG_NONE, 0010000, -1, (VE_RIO | VE_WIO), VS_OPERATE,
      "PDP-1D: IO := not IO (TP8); cli cmi is all ones", CITE_1DTIME },
    { "lai", VG_NONE, 0000040, -1, (VE_RIO | VE_WAC), VS_OPERATE,
      "PDP-1D: AC := IO, after every other micro-op of the word", CITE_1DTIME },
    { "lia", VG_NONE, 0000020, -1, (VE_RAC | VE_WIO), VS_OPERATE,
      "PDP-1D: IO := AC, after every other micro-op of the word", CITE_1DTIME },
    { "swp", VG_NONE, 0000060, -1, (VE_RAC | VE_RIO | VE_WAC | VE_WIO), VS_OPERATE,
      "PDP-1D: lai and lia together are one exchange", CITE_1DTIME },

    // the PDP-1D special operate group
    { "sop", OPTG_1D, 0, -1, 0, VS_SPECIAL,
      "event 1 sci scf; event 2 iif ifi from one copy of IO; event 3 ida", CITE_1DTIME },
    { "sci", VG_NONE, 0000100, -1, VE_WIO, VS_SPECIAL,
      "PDP-1D: IO := 0 (event 1)", CITE_1D },
    { "scf", VG_NONE, 0000040, -1, VE_FLAGS, VS_SPECIAL,
      "PDP-1D: clear program flags 1-6 (event 1)", CITE_1D },
    { "iif", VG_NONE, 0004000, -1, (VE_RIO | VE_WIO | VE_FLAGS), VS_SPECIAL,
      "PDP-1D: IO bits 12-17 := IO or the flags (event 2)", CITE_1D },
    { "ifi", VG_NONE, 0002000, -1, (VE_RIO | VE_FLAGS), VS_SPECIAL,
      "PDP-1D: the flags := the flags or IO bits 12-17 (event 2)", CITE_1D },
    { "ida", VG_NONE, 0000400, -1, (VE_RAC | VE_WAC), VS_SPECIAL,
      "PDP-1D: AC := AC + 1 (event 3)", CITE_1D },

    // in-out transfers
    { "iot", OPTG_IOT, -1, -1, VE_KILL, VS_UNKNOWN,
      "any other device: its effect on IO, AC and memory is the device's, so unknown for both registers",
      "F-15D p.22" },
    { "wait", OPTG_IOT, 0730000, -1, 0, VS_NONE,
      "iot with no device: waits for a completion pulse and moves nothing", "F-15D p.22" },
    { "eem", OPTG_IOT, 0724074, -1, 0, VS_NONE,
      "enter extend mode: neither register", "F-15D p.29" },
    { "lem", OPTG_IOT, 0720074, -1, 0, VS_NONE,
      "leave extend mode: neither register", "F-15D p.29" },
    { "tyi", OPTG_IOT, 0720004, -1, VE_KILL, VS_UNKNOWN,
      "clears IO and reads the typed character into it; treated as unknown like every device", "F-15D p.25" },
    { "tyo", OPTG_IOT, 0730003, -1, VE_KILL, VS_UNKNOWN,
      "types IO bits 12-17; treated as unknown like every device", "F-15D p.25" },
    { "rpa", OPTG_IOT, 0730001, -1, VE_KILL, VS_UNKNOWN,
      "reads a line of tape into IO; treated as unknown like every device", "F-15D p.23" },
    { "rpb", OPTG_IOT, 0730002, -1, VE_KILL, VS_UNKNOWN,
      "reads three lines of tape into IO; treated as unknown like every device", "F-15D p.24" },
    { "rrb", OPTG_IOT, 0720030, -1, VE_KILL, VS_UNKNOWN,
      "reader buffer to IO; treated as unknown like every device", "F-15D p.24" },
    { "ppa", OPTG_IOT, 0730005, -1, VE_KILL, VS_UNKNOWN,
      "punches IO bits 10-17; treated as unknown like every device", "F-15D p.24" },
    { "ppb", OPTG_IOT, 0730006, -1, VE_KILL, VS_UNKNOWN,
      "punches IO bits 0-5; treated as unknown like every device", "F-15D p.25" },
    { "cks", OPTG_IOT, 0720033, -1, VE_KILL, VS_UNKNOWN,
      "status bits into IO; treated as unknown like every device", "F-15D p.27" },
    { "dpy", OPTG_IOT, 0730007, -1, VE_KILL, VS_UNKNOWN,
      "displays the point AC, IO; treated as unknown like every device", "F-15D p.34" },

    // spare codes and the sequence break
    { "spare", OPTG_UNKNOWN, -1, -1, VE_KILL, VS_UNKNOWN,
      "codes 00, 12, 14 and 36 do nothing defined", "F-15D p.66" },
    { "break", VG_NONE, 0, -1, VE_WMEM, VS_NONE,
      "a sequence break stores AC, PC and IO in its frame and the handler restores them; "
      "the frame words are marked handler-written", "F-15D p.26" }
};

#define VROW_COUNT  ((int)(sizeof(effectRows) / sizeof(effectRows[0])))

// The pass's state.

// A value number.  Constants are themselves, 0 to 0777777; every other value
// is numbered from VAL_FRESH upward, and two places hold the same value
// exactly when they hold the same number.
#define VAL_FRESH   01000000L

// The carried pass's names (task A52), in ranges no two kinds share.  They
// need a 64-bit long, which every host am1 builds on has.
#define VAL_CARRY   (1L << 32)      // a word's values: + index * VSLOTS + slot
#define VAL_ENTRY   (1L << 44)      // a block's entry: + id * 2 + register
#define VAL_SYM     (1L << 52)      // a summary run's symbols: + register
#define VAL_LOST    (1L << 56)      // an older instance whose name was taken
#define VSLOTS      8               // names one word can make; more are lost

typedef char vLongHolds64Bits[(sizeof(long) >= 8)?1:-1];

#define VR_AC       0
#define VR_IO       1

// The classes of hit, one per word, in dump order.  Layout-neutral, one word
// for one word:
//   lai    lac W while IO holds W's value (10 -> 5 us); lia likewise for lio
//   law    lac/lio K, K an unwritten word an augmented instruction can load
//   lawk   the same, the constant known by tracking ("law 5; dac t; lac t")
// Deleting a word each, pending relayout:
//   rload  a load whose register already holds the value, already read
//   rclear a cla, cli or sci whose register is already zero
//   swap   the second load of a crosswise pair: "lac b" made lai, then
//          "lio a" loading what AC held before; the two are one swp
//   dload, dclear  a load or clear the block overwrites before reading it
//          (the T6 and T7 shape, anywhere in the block)
// A layout-neutral hit that turns out dead is counted once, as dead.
typedef enum
{
    VC_LAI,
    VC_LIA,
    VC_LAW,
    VC_LAWK,
    VC_RLOAD,
    VC_RCLEAR,
    VC_SWAP,
    VC_DLOAD,
    VC_DCLEAR,
    VC_COUNT
} VClass;

static const struct
{
    const char *nameP;
    int neutral;            // one word for one word, no relayout
} classTable[VC_COUNT] =
{
    { "lai",    1 },
    { "lia",    1 },
    { "law",    1 },
    { "lawk",   1 },
    { "rload",  0 },
    { "rclear", 0 },
    { "swap",   0 },
    { "dload",  0 },
    { "dclear", 0 }
};

// Where a fact died, for the kill lines and their counts.
typedef enum
{
    VK_NONE,
    VK_INDIRECT,    // an indirect write: every memory fact
    VK_PARTIAL,     // a dap or dip: the fact about its word
    VK_XCT,         // an xct: everything
    VK_IOT,         // an in-out transfer the table marks unknown: everything
    VK_SPARE,       // a spare instruction code: everything
    VK_WRITTEN,     // a word the program writes or patches: everything
    VK_SKIP,        // a skip ends the block: everything
    VK_COUNT
} VKill;

static const char *killNames[VK_COUNT] =
{
    "-", "indirect", "partial", "xct", "iot", "spare", "written", "skip"
};

// The safety flags on a hit.
#define VF_HANDLER      0x01    // the value rests on a word a sequence-break
                                // handler writes (P5, which the tool cannot check)
#define VF_AFTERSKIP    0x02    // the hit word is the first word after a skip (P4)
#define VF_XCT          0x04    // a word of the span is an xct target (P3)
#define VF_TAKEN        0x08    // the hit word's address is used as a value, or it
                                // may be reached through an unresolved pointer (P2)
#define VF_LABEL        0x10    // an interior word of the span carries a label (P1)
#define VF_UNREACHED    0x20    // no entry reaches the hit word
#define VF_COUNT        6

static const char *flagNames[VF_COUNT] =
{
    "handler", "afterskip", "xct", "taken", "label", "unreached"
};

// What a register holds.
typedef struct
{
    long val;               // its value number
    int since;              // the address of the word that put it there, -1 when
                            // it has held it since the block was entered
} VReg;

// One memory fact: the word at addr, in the block's bank, holds val.
typedef struct
{
    int addr;
    long val;
    int origin;             // the address of the word that made the fact
} VFact;

// One hit.
typedef struct vhit
{
    struct vhit *nextP;     // next hit, in bank then address order
    VClass cls;             // the class it is counted in
    VClass firstCls;        // the class it was found as: differs from cls only
                            // for a layout-neutral hit that turned out dead
    OptWordP wordP;         // the word the hit rewrites or deletes
    int spanFrom;           // the words the hit rests on, in the word's bank
    int spanTo;
    long val;               // the value loaded, or -1
    int memAddr;            // the memory word the value rests on, -1 for none
    unsigned int flags;     // VF_ bits
    OptRegionPlace place;
    int words;              // words it would save
    int us;                 // microseconds it would save, one execution
    char *replaceP;         // what the word would become
    char *whyP;             // the evidence
} VHit, *VHitP;

// A word whose result is waiting to be read.
typedef struct
{
    int active;
    int addr;               // the word
    OptWordP wordP;
    VClass deadClass;       // what it is if the register is overwritten unread
    VHitP hitP;             // a layout-neutral hit at the word, NILP if none
    int isSwp;              // the word is a swp, watched for the annihilation count
} VPending;

// The first word of a possible crosswise pair: a lai or lia hit.
typedef struct
{
    int active;
    int reg;                // the register the first word loaded
    long oldVal;            // what that register held before it
    VHitP hitP;             // the first word's hit
} VExchange;

typedef struct
{
    OptTableP tableP;
    FILE *fP;               // where the dump goes, NILP for a counting run
    int optimistic;         // an unknown in-out transfer touches IO only

    // the block being walked
    OptBlockP blockP;
    int bank;
    VReg reg[2];
    VFact *factsP;
    int factCount;
    int factCap;
    VPending pend[2];
    VExchange exch;
    int swpAddr;            // the swp whose two results are both still unread
    int swpDeadMask;        // which of them have been overwritten unread
    VHitP *blockHitsPP;     // per word of the block, its hit
    VKill *blockKillsP;     // per word of the block, the kill it made
    int *blockKillNP;       // and how many facts died there
    int blockCap;
    long nextFresh;

    // every hit, kept for the control leg
    VHitP hitsP;
    VHitP hitsTailP;
    VHitP *hitOfWordPP;     // by the word's index in emission order

    // the handler set, from the sequence-break walk
    unsigned char *handlerBlockP;           // by block id
    unsigned char *handlerWrittenP[MAXBANK + 1];    // by address, NILP for a bank
                                                    // no handler writes in
    int handlerWords;       // words marked
    int handlerIndirect;    // unfollowed indirect writes in handler code

    // tallies
    int bankUsed[MAXBANK + 1];
    int bankHits[MAXBANK + 1];
    int classHits[MAXBANK + 1][VC_COUNT];
    int classWords[MAXBANK + 1][VC_COUNT];
    int classUs[MAXBANK + 1][VC_COUNT];
    int totalHits;
    int neutralToo;         // dead hits that were found layout-neutral first
    int knownHits;
    int knownRule[OPTRULE_COUNT];
    int knownSupp;          // of the known hits, the ones whose finding is suppressed
    int placeHits[OPTREG_COUNT];
    int flagHits[VF_COUNT];
    int kills[VK_COUNT];
    int swpSwp;
    int swpDead;
    int blocks;
    int blockSingle;        // blocks entered by exactly one edge, from a block,
                            // and not an entry: the reset forfeits these
    int blockMerge;         // blocks entered by more than one edge
    int blockEntry;         // blocks in the entry set
    int blockNone;          // blocks nothing enters

    // The carried pass (-O=carry).  carry is 0 for -O=values, whose values
    // are counted from nextFresh as they always were.
    int carry;
    int curIndex;           // the word being walked, whose values are named for it
    int curSlot;            // the next of its names
    long nextLost;
    int iotFree;            // an unknown in-out transfer touches nothing
    int borrowed;           // the handler set is another pass's: not freed here
} VPass;

// Which finding, if any, names a word: filled once from the finding lists.
typedef struct
{
    int rule;               // OptRuleId + 1, 0 when no finding names the word
    int supp;               // the finding is suppressed
} VKnown;

static VKnown *knownOfWordP;

// Small helpers.

// Allocate zeroed memory; out of memory is fatal.
// Returns the memory, never NILP.
static void *
vAlloc(size_t count, size_t size)
{
void *memP;

    if( !(memP = calloc((count)?count:1, size)) )
    {
        fprintf(stderr, "am1: out of memory measuring values\n");
        exit(1);
    }

    return(memP);
}

// Is a value number a known constant?
// Returns 1 for a constant, 0 for an opaque value.
static int
isConst(long val)
{
    return( (val >= 0) && (val < VAL_FRESH) );
}

// A value no other place holds.  The carried pass names it for the word being
// walked and the slot; doWord() has already retired the word's older names.
// Returns the new value number.
static long
freshVal(VPass *passP)
{
    if( passP->carry )
    {
        if( (passP->curIndex < 0) || (passP->curSlot >= VSLOTS) )
        {
            return( VAL_LOST + passP->nextLost++ );
        }

        return( VAL_CARRY + ((long)passP->curIndex * VSLOTS) + passP->curSlot++ );
    }

    return( passP->nextFresh++ );
}

// The complement of a value: a constant's is a constant, an opaque value's
// a new opaque value, since the pass has no "complement of v".
// Returns the value number.
static long
complementVal(VPass *passP, long val)
{
    if( isConst(val) )
    {
        return( (~val) & WRDMASK );
    }

    return( freshVal(passP) );
}

// The other register.
// Returns VR_IO for VR_AC and VR_AC for VR_IO.
static int
otherReg(int reg)
{
    return( (reg == VR_AC)?VR_IO:VR_AC );
}

// The name of a register, for the evidence.
// Returns a static string.
static const char *
regName(int reg)
{
    return( (reg == VR_AC)?"AC":"IO" );
}

// Can one augmented instruction load a constant into a register, and how is
// it spelled?  AC takes cla, cla cma, law and law i, spelled as T8 spells them;
// IO takes only cli and the PDP-1D cli cmi.
// Returns 1 and fills bufP when it can, 0 when it cannot.
static int
constSpelling(int reg, long val, char *bufP, int size)
{
    if( !isConst(val) )
    {
        return(0);
    }

    if( reg == VR_IO )
    {
        if( val == 0 )
        {
            snprintf(bufP, (size_t)size, "cli");
            return(1);
        }

        if( val == WRDMASK )
        {
            snprintf(bufP, (size_t)size, "cli cmi");
            return(1);
        }

        return(0);
    }

    if( val == 0 )
    {
        snprintf(bufP, (size_t)size, "cla");
    }
    else if( val == WRDMASK )
    {
        snprintf(bufP, (size_t)size, "cla cma");
    }
    else if( val <= ADDRMASK )
    {
        snprintf(bufP, (size_t)size, "law 0o%lo", val);
    }
    else if( ((~val) & WRDMASK) <= ADDRMASK )
    {
        snprintf(bufP, (size_t)size, "law i 0o%lo", ((~val) & WRDMASK));
    }
    else
    {
        return(0);
    }

    return(1);
}

// Find the effect-table row for a word; an in-out transfer matches its
// exact value first, then the generic row.
// Returns the row, never NILP.
static const VRow *
findRow(OptWordP wordP)
{
int i;
const VRow *genericP;
OptDecodeP decodeP;

    decodeP = &wordP->decode;
    genericP = NILP;

    for( i = 0; i < VROW_COUNT; ++i )
    {
        const VRow *rowP = &effectRows[i];

        if( rowP->group != (int)decodeP->group )
        {
            continue;
        }

        switch( decodeP->group )
        {
        case OPTG_MEMREF:
            if( (rowP->code == decodeP->opcode) && ((rowP->ibit < 0) || (rowP->ibit == decodeP->indirect)) )
            {
                return(rowP);
            }
            break;

        case OPTG_SHIFT:
            if( rowP->code == decodeP->shiftRegs )
            {
                return(rowP);
            }
            break;

        case OPTG_IOT:
            if( rowP->code == (wordP->value & WRDMASK) )
            {
                return(rowP);
            }

            if( rowP->code < 0 )
            {
                genericP = rowP;
            }
            break;

        default:
            return(rowP);
        }
    }

    if( genericP )
    {
        return(genericP);
    }

    // Unreachable while every group has a row; the spare row is safe.
    for( i = 0; i < VROW_COUNT; ++i )
    {
        if( effectRows[i].group == OPTG_UNKNOWN )
        {
            return(&effectRows[i]);
        }
    }

    return(&effectRows[0]);
}

// Does a word make every memory fact die, and why (optpreserve.c's write
// sets)?  An in-out transfer the table calls unknown, an xct or a spare code
// does, and so does a code word the program writes whole (not just patches),
// whose run-time word could be a store.
// Returns OPTPW_NONE, OPTPW_IOT, OPTPW_XCT or OPTPW_SPARE.
int
optValueKills(OptWordP wordP)
{
const VRow *rowP;

    if( (wordP->flags & OPTF_WRITTEN) && !(wordP->flags & OPTF_PATCHED) )
    {
        return(OPTPW_XCT);
    }

    rowP = findRow(wordP);

    if( !(rowP->effects & VE_KILL) )
    {
        return(OPTPW_NONE);
    }

    if( wordP->decode.group == OPTG_IOT )
    {
        return(OPTPW_IOT);
    }

    return( (wordP->decode.group == OPTG_UNKNOWN)?OPTPW_SPARE:OPTPW_XCT );
}

// Spell a row's effect bits for the dump.
static void
spellEffects(unsigned int effects, char *bufP, int size)
{
static const struct
{
    unsigned int bit;
    const char *nameP;
} names[] =
{
    { VE_RAC, "RAC" }, { VE_RIO, "RIO" }, { VE_RMEM, "RMEM" }, { VE_WAC, "WAC" },
    { VE_WIO, "WIO" }, { VE_WMEM, "WMEM" }, { VE_PMEM, "PMEM" }, { VE_KILL, "KILL" },
    { VE_ENDS, "ENDS" }, { VE_FLAGS, "FLAGS" }
};
int i;
int at;

    at = 0;
    bufP[0] = '\0';

    for( i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i )
    {
        if( effects & names[i].bit )
        {
            at += snprintf((bufP + at), (size_t)(size - at), "%s%s", (at)?",":"", names[i].nameP);

            if( at >= size )
            {
                return;
            }
        }
    }

    if( !at )
    {
        snprintf(bufP, (size_t)size, "none");
    }
}

// The handler set: every block the sequence-break walk reaches, and every
// memory word something in those blocks writes.

// Mark one memory word as written by handler code.
static void
markHandlerWord(VPass *passP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return;
    }

    if( !passP->handlerWrittenP[bank] )
    {
        passP->handlerWrittenP[bank] = (unsigned char *)vAlloc(BANKSIZE, 1);
    }

    if( !passP->handlerWrittenP[bank][addr] )
    {
        passP->handlerWrittenP[bank][addr] = 1;
        ++passP->handlerWords;
    }
}

// Find the handler set: markSbsPool()'s walk (optcallgraph.c), repeated here,
// from every in-graph entry word (bank 0, 4n+3, frames per optSbsChannels())
// along every successor edge.  Each write from a reached block marks its
// target, as do each entry's three frame words, where the break stores AC,
// PC and IO (F-15D p.26).
static void
findHandlerSet(VPass *passP)
{
OptTableP tableP;
OptBlockP blockP;
OptBlockP *stackPP;
OptFlowEdgeP flowP;
OptWordP wordP;
OptEdgeP edgeP;
int channel;
int addr;
int top;
int slot;

    tableP = passP->tableP;
    passP->handlerBlockP = (unsigned char *)vAlloc((size_t)(tableP->blockCount + 1), 1);

    if( !tableP->blockCount )
    {
        return;
    }

    stackPP = (OptBlockP *)vAlloc((size_t)tableP->blockCount, sizeof(OptBlockP));
    top = 0;

    for( channel = 0; channel < optSbsChannels(tableP); ++channel )
    {
        addr = ((channel * OPTSBS_FRAMESIZE) + OPTSBS_ENTRYSLOT);

        if( !(wordP = wordAt(tableP, 0, addr)) || !inGraph(wordP) || !wordP->blockP )
        {
            continue;
        }

        for( slot = 0; slot < OPTSBS_ENTRYSLOT; ++slot )
        {
            markHandlerWord(passP, 0, ((channel * OPTSBS_FRAMESIZE) + slot));
        }

        if( passP->handlerBlockP[wordP->blockP->id] )
        {
            continue;
        }

        passP->handlerBlockP[wordP->blockP->id] = 1;
        stackPP[top++] = wordP->blockP;
    }

    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( !flowP->toP || passP->handlerBlockP[flowP->toP->id] )
            {
                continue;
            }

            passP->handlerBlockP[flowP->toP->id] = 1;
            stackPP[top++] = flowP->toP;
        }
    }

    free(stackPP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !passP->handlerBlockP[blockP->id] )
        {
            continue;
        }

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                continue;
            }

            for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
            {
                if( (edgeP->role != OPTR_WRITE) || (edgeP->flags & OPTEF_PLACEHOLDER) )
                {
                    continue;
                }

                if( edgeP->flags & OPTEF_INDIRECT )
                {
                    // The pointer word is only read; the write lands on the
                    // followed edge, or somewhere unknown.
                    if( edgeP->flags & OPTEF_UNKNOWN )
                    {
                        ++passP->handlerIndirect;
                    }

                    continue;
                }

                markHandlerWord(passP, edgeP->toBank, edgeP->toAddr);
            }
        }
    }
}

// Is a memory word written by handler code?
// Returns 1 when it is, 0 when it is not.
static int
handlerWritten(VPass *passP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) || !passP->handlerWrittenP[bank] )
    {
        return(0);
    }

    return( passP->handlerWrittenP[bank][addr] );
}

// Memory facts.

// Find the fact about a memory word.
// Returns its index, or -1 when there is none.
static int
findFact(VPass *passP, int addr)
{
int i;

    for( i = 0; i < passP->factCount; ++i )
    {
        if( passP->factsP[i].addr == addr )
        {
            return(i);
        }
    }

    return(-1);
}

// Record that a memory word now holds a value, replacing any earlier fact.
static void
setFact(VPass *passP, int addr, long val, int origin)
{
int i;

    if( (i = findFact(passP, addr)) < 0 )
    {
        if( passP->factCount == passP->factCap )
        {
            passP->factCap = (passP->factCap)?(passP->factCap * 2):32;

            if( !(passP->factsP = (VFact *)realloc(passP->factsP, ((size_t)passP->factCap * sizeof(VFact)))) )
            {
                fprintf(stderr, "am1: out of memory measuring values\n");
                exit(1);
            }
        }

        i = passP->factCount++;
    }

    passP->factsP[i].addr = addr;
    passP->factsP[i].val = val;
    passP->factsP[i].origin = origin;
}

// The carried pass: a name about to be made again stops meaning what the
// places holding it hold.  Every name from first to first + count - 1 is
// replaced, wherever it is, by a lost name of its own, one per name so that
// places sharing an older instance still agree with each other.
static void
retireNames(VPass *passP, long first, int count)
{
long lost[VSLOTS];
long *valP;
int i;
int k;

    for( k = 0; k < count; ++k )
    {
        lost[k] = -1;
    }

    for( i = -3; i < passP->factCount; ++i )
    {
        if( i == -3 )
        {
            if( !passP->exch.active )
            {
                continue;
            }

            valP = &passP->exch.oldVal;
        }
        else
        {
            valP = (i < 0)?&passP->reg[i + 2].val:&passP->factsP[i].val;
        }

        if( (*valP < first) || (*valP >= (first + count)) )
        {
            continue;
        }

        k = (int)(*valP - first);

        if( lost[k] < 0 )
        {
            lost[k] = VAL_LOST + passP->nextLost++;
        }

        *valP = lost[k];
    }
}

// Is a memory word constant for the whole run, as far as the reference
// edges show: not overlaid, written, patched, taken, possibly written
// through a pointer, or stored into by a sequence break or its handler?
// Returns 1 and sets *valP when it is, 0 when it is not.
static int
constantWord(VPass *passP, int addr, long *valP)
{
OptWordP wordP;

    if( !(wordP = wordAt(passP->tableP, passP->bank, addr)) )
    {
        return(0);
    }

    if( wordP->flags & (OPTF_RESERVED | OPTF_DUPADDR | OPTF_WRITTEN | OPTF_PATCHED
                        | OPTF_TAKEN | OPTF_MAYBE_WRITTEN) )
    {
        return(0);
    }

    if( handlerWritten(passP, passP->bank, addr) )
    {
        return(0);
    }

    *valP = (long)(wordP->value & WRDMASK);
    return(1);
}

// Read a memory word directly: a fact answers first, then a constant word
// (no fact needed, nothing can kill it); otherwise a fresh value and a fact,
// so a second read before any write agrees with the first.
// Returns the value number; *originP is the address the answer rests on, -1
// for a constant word, and *constWordP says whether it was one.
static long
readMem(VPass *passP, int addr, int readAddr, int *originP, int *constWordP)
{
int i;
long val;

    *constWordP = 0;

    if( (i = findFact(passP, addr)) >= 0 )
    {
        *originP = passP->factsP[i].origin;
        return(passP->factsP[i].val);
    }

    if( constantWord(passP, addr, &val) )
    {
        *constWordP = 1;
        *originP = -1;
        return(val);
    }

    val = freshVal(passP);
    setFact(passP, addr, val, readAddr);
    *originP = readAddr;
    return(val);
}

// Hits.

// The offset of an address within the block being walked.
// Returns the offset, 0 for the first word.
static int
blockOffset(VPass *passP, int addr)
{
    return( addr - passP->blockP->startAddr );
}

// Create a hit at a word; its class decides what it saves.
// Returns the hit, which the block's per-word array now holds.
static VHitP
makeHit(VPass *passP, VClass cls, OptWordP wordP, int spanFrom, int spanTo, long val,
        int memAddr, const char *replaceP, char *whyP)
{
VHitP hitP;

    hitP = (VHitP)vAlloc(1, sizeof(VHit));
    hitP->cls = cls;
    hitP->firstCls = cls;
    hitP->wordP = wordP;
    hitP->spanFrom = (spanFrom < 0)?passP->blockP->startAddr:spanFrom;
    hitP->spanTo = spanTo;
    hitP->val = val;
    hitP->memAddr = memAddr;
    hitP->replaceP = allocPrintf("%s", replaceP);
    hitP->whyP = whyP;

    if( classTable[cls].neutral )
    {
        hitP->words = 0;
        hitP->us = (optWordTime(wordP) - 5);    // lai, lia, law, cla: one cycle (p.10)
    }
    else
    {
        hitP->words = 1;
        hitP->us = optWordTime(wordP);
    }

    passP->blockHitsPP[blockOffset(passP, wordP->addr)] = hitP;
    return(hitP);
}

// A waiting word's register was overwritten unread: it is dead.  A layout-
// neutral hit at the word becomes the dead class and remembers what it was.
// A swp is only counted, for the annihilation figure.
static void
markDead(VPass *passP, VPending *pendP, int reg, int overAddr)
{
VHitP hitP;
char spell[OPTMSG_SIZE];

    if( pendP->isSwp )
    {
        if( passP->swpAddr == pendP->addr )
        {
            passP->swpDeadMask |= (1 << reg);

            if( passP->swpDeadMask == 3 )
            {
                ++passP->swpDead;

                if( passP->fP )
                {
                    spellWord(pendP->wordP, spell, sizeof(spell));
                    fprintf(passP->fP, "annihilate swp-dead b%d %04o | %s | both results overwritten unread by %04o\n",
                        passP->bank, pendP->addr, spell, overAddr);
                }

                passP->swpAddr = -1;
            }
        }

        return;
    }

    if( (hitP = pendP->hitP) )
    {
        free(hitP->replaceP);
        free(hitP->whyP);
        hitP->cls = pendP->deadClass;
        hitP->words = 1;
        hitP->us = optWordTime(pendP->wordP);
        hitP->spanFrom = pendP->addr;
        hitP->spanTo = overAddr;
        hitP->replaceP = allocPrintf("delete");
        hitP->whyP = allocPrintf("%s is overwritten at %04o before it is read (found as %s first)",
            regName(reg), overAddr, classTable[hitP->firstCls].nameP);
        return;
    }

    makeHit(passP, pendP->deadClass, pendP->wordP, pendP->addr, overAddr, -1, -1, "delete",
        allocPrintf("%s is overwritten at %04o before it is read", regName(reg), overAddr));
}

// A word reads a register, so whatever was waiting on it was needed.
// Reading the register a crosswise pair's second word would fill ends the
// pair, since a swp would already have changed it.
static void
useReg(VPass *passP, int reg)
{
VPending *pendP;

    pendP = &passP->pend[reg];

    if( pendP->active )
    {
        if( pendP->isSwp && (passP->swpAddr == pendP->addr) )
        {
            passP->swpAddr = -1;
        }

        pendP->active = 0;
    }

    if( passP->exch.active && (reg == otherReg(passP->exch.reg)) )
    {
        passP->exch.active = 0;
    }
}

// A word writes a register without reading it, so whatever was waiting on
// it is dead.  Any write ends a crosswise pair: the second load was not
// next, or the first word could be called dead underneath its partner.
static void
writeReg(VPass *passP, int reg, long val, int addr)
{
    if( passP->pend[reg].active )
    {
        markDead(passP, &passP->pend[reg], reg, addr);
        passP->pend[reg].active = 0;
    }

    if( passP->exch.active )
    {
        passP->exch.active = 0;
    }

    passP->reg[reg].val = val;
    passP->reg[reg].since = addr;
}

// Hold a word's result until its register is read or overwritten.
static void
setPending(VPass *passP, int reg, OptWordP wordP, VClass deadClass, VHitP hitP, int isSwp)
{
VPending *pendP;

    pendP = &passP->pend[reg];
    pendP->active = 1;
    pendP->addr = wordP->addr;
    pendP->wordP = wordP;
    pendP->deadClass = deadClass;
    pendP->hitP = hitP;
    pendP->isSwp = isSwp;
}

// Record a kill at a word, for the dump and the count, if it ended a fact.
static void
noteKill(VPass *passP, OptWordP wordP, VKill kind, int facts)
{
int offset;

    if( facts <= 0 )
    {
        return;
    }

    offset = blockOffset(passP, wordP->addr);
    passP->blockKillsP[offset] = kind;
    passP->blockKillNP[offset] = facts;
    ++passP->kills[kind];
}

// Count the facts held: memory facts, plus each register set in this block.
// Returns the count.
static int
factsHeld(VPass *passP)
{
    return( passP->factCount + ((passP->reg[VR_AC].since >= 0)?1:0) + ((passP->reg[VR_IO].since >= 0)?1:0) );
}

// A word of unknown effect: everything waiting counts as read, every fact
// dies and both registers become unknown.
static void
killAll(VPass *passP, OptWordP wordP, VKill kind)
{
int facts;

    facts = factsHeld(passP);
    useReg(passP, VR_AC);
    useReg(passP, VR_IO);
    passP->factCount = 0;
    writeReg(passP, VR_AC, freshVal(passP), wordP->addr);
    writeReg(passP, VR_IO, freshVal(passP), wordP->addr);
    noteKill(passP, wordP, kind, facts);
}

// An indirect write: every memory fact dies, since where it lands is
// unknown.  Registers are untouched.
static void
killMemory(VPass *passP, OptWordP wordP)
{
int facts;

    facts = passP->factCount;
    passP->factCount = 0;
    noteKill(passP, wordP, VK_INDIRECT, facts);
}

// The later of two addresses, -1 standing for the block's entry.
// Returns the address.
static int
laterOf(int a, int b)
{
    return( (a > b)?a:b );
}

// The words.

// Would a load of val into reg be redundant?  Not while the word that put
// it there waits unread: only one can go, and the earlier (now dead) one
// saves as much or more and is the word T6 and T7 name; preferring the load
// would miss T6 on "cla; lac tmp".  A waiting swp does not count.
// Returns 1 if redundant, 0 otherwise.
static int
alreadyHolds(VPass *passP, int reg, long val)
{
    if( passP->reg[reg].val != val )
    {
        return(0);
    }

    return( !passP->pend[reg].active || passP->pend[reg].isSwp );
}

// A direct lac or lio, tried in order as rload, swap, law, lai or lia,
// lawk; then the register is written and the word waits to be read.
static void
doLoad(VPass *passP, OptWordP wordP, int reg)
{
int other;
int memAddr;
int origin;
int constWord;
int since;
long val;
long oldVal;
VClass cls;
VHitP hitP;
char spell[OPTMSG_SIZE];

    other = otherReg(reg);
    memAddr = wordP->decode.address;
    val = readMem(passP, memAddr, wordP->addr, &origin, &constWord);

    if( alreadyHolds(passP, reg, val) )
    {
        // Only a value carried into the block can be held since its entry
        // and rest on a constant word.
        since = laterOf(passP->reg[reg].since, origin);
        makeHit(passP, VC_RLOAD, wordP, since, wordP->addr, val, memAddr, "delete",
            allocPrintf("%s already holds the value of %04o (since %04o)", regName(reg), memAddr,
                (since < 0)?passP->blockP->startAddr:since));
        return;
    }

    if( passP->exch.active && (reg == otherReg(passP->exch.reg)) && (val == passP->exch.oldVal) )
    {
        // Count the first word's register as read, so the first word is
        // never deleted as dead from under its partner.
        hitP = passP->exch.hitP;
        passP->exch.active = 0;
        useReg(passP, passP->exch.reg);
        hitP = makeHit(passP, VC_SWAP, wordP, hitP->spanFrom, wordP->addr, val, memAddr, "delete",
            allocPrintf("%04o loaded %s from %s; with it made swp, %s already holds what %s held before",
                hitP->wordP->addr, regName(passP->exch.reg), regName(reg), regName(reg), regName(passP->exch.reg)));
        writeReg(passP, reg, val, wordP->addr);
        setPending(passP, reg, wordP, VC_DLOAD, hitP, 0);
        return;
    }

    cls = VC_COUNT;
    hitP = NILP;

    if( constWord && constSpelling(reg, val, spell, sizeof(spell)) )
    {
        cls = VC_LAW;
        hitP = makeHit(passP, cls, wordP, wordP->addr, wordP->addr, val, -1, spell,
            allocPrintf("%04o is never written and never taken, and holds 0o%lo", memAddr, val));
    }
    else if( passP->reg[other].val == val )
    {
        cls = (reg == VR_AC)?VC_LAI:VC_LIA;
        since = laterOf(passP->reg[other].since, origin);
        hitP = makeHit(passP, cls, wordP, since, wordP->addr, val, memAddr,
            classTable[cls].nameP,
            allocPrintf("%s holds the value of %04o (since %04o)", regName(other), memAddr,
                (since < 0)?passP->blockP->startAddr:since));
        useReg(passP, other);
    }
    else if( isConst(val) && constSpelling(reg, val, spell, sizeof(spell)) )
    {
        cls = VC_LAWK;
        hitP = makeHit(passP, cls, wordP, laterOf(origin, -1), wordP->addr, val, memAddr, spell,
            allocPrintf("%04o holds 0o%lo (since %04o)", memAddr, val, origin));
    }

    oldVal = passP->reg[reg].val;
    writeReg(passP, reg, val, wordP->addr);
    setPending(passP, reg, wordP, VC_DLOAD, hitP, 0);

    if( (cls == VC_LAI) || (cls == VC_LIA) )
    {
        passP->exch.active = 1;
        passP->exch.reg = reg;
        passP->exch.oldVal = oldVal;
        passP->exch.hitP = hitP;
    }
}

// A word that loads a computed value (law, cla, cli, lai, lia, sci ...):
// redundant if the register holds it, else written and held until read.
// readReg is the register a kept word reads (IO for lai, AC for lia), or -1.
static void
doCandidate(VPass *passP, OptWordP wordP, int reg, long val, VClass redundant, VClass dead, int readReg)
{
int since;

    if( alreadyHolds(passP, reg, val) )
    {
        since = (passP->reg[reg].since < 0)?passP->blockP->startAddr:passP->reg[reg].since;

        if( isConst(val) )
        {
            makeHit(passP, redundant, wordP, since, wordP->addr, val, -1, "delete",
                allocPrintf("%s already holds 0o%lo (since %04o)", regName(reg), val, since));
        }
        else
        {
            makeHit(passP, redundant, wordP, since, wordP->addr, val, -1, "delete",
                allocPrintf("%s already holds that value (since %04o)", regName(reg), since));
        }

        return;
    }

    if( readReg >= 0 )
    {
        useReg(passP, readReg);
    }

    writeReg(passP, reg, val, wordP->addr);
    setPending(passP, reg, wordP, dead, NILP, 0);
}

// A value and the registers it came from, for the operate evaluation.
typedef struct
{
    long val;
    int src;                // 1 the word's incoming AC, 2 its incoming IO
} VSym;

// The operate group: an exact load or clear shape is a candidate; anything
// else is evaluated micro-op by micro-op in the hardware's phase order.
static void
doOperate(VPass *passP, OptWordP wordP)
{
unsigned int bits;
VSym ac;
VSym io;
VSym newAc;
VSym newIo;
int writesAc;
int writesIo;
int readsAc;
int readsIo;
long swapped;

    bits = (wordP->decode.microBits & OPTM_ALLBITS);

    switch( bits )
    {
    case OPTM_CLA:
        doCandidate(passP, wordP, VR_AC, 0, VC_RCLEAR, VC_DCLEAR, -1);
        return;

    case OPTM_CLI:
        doCandidate(passP, wordP, VR_IO, 0, VC_RCLEAR, VC_DCLEAR, -1);
        return;

    case (OPTM_CLA | OPTM_CMA):
        doCandidate(passP, wordP, VR_AC, WRDMASK, VC_RLOAD, VC_DLOAD, -1);
        return;

    case (OPTM_CLI | OPTM_CMI):
        doCandidate(passP, wordP, VR_IO, WRDMASK, VC_RLOAD, VC_DLOAD, -1);
        return;

    case OPTM_LAI:
        doCandidate(passP, wordP, VR_AC, passP->reg[VR_IO].val, VC_RLOAD, VC_DLOAD, VR_IO);
        return;

    case OPTM_LIA:
        doCandidate(passP, wordP, VR_IO, passP->reg[VR_AC].val, VC_RLOAD, VC_DLOAD, VR_AC);
        return;

    case (OPTM_LAI | OPTM_LIA):
        if( passP->reg[VR_AC].val == passP->reg[VR_IO].val )
        {
            makeHit(passP, VC_RLOAD, wordP, laterOf(passP->reg[VR_AC].since, passP->reg[VR_IO].since),
                wordP->addr, passP->reg[VR_AC].val, -1, "delete",
                allocPrintf("AC and IO already hold the same value, so the exchange changes nothing"));
            return;
        }

        useReg(passP, VR_AC);
        useReg(passP, VR_IO);
        swapped = passP->reg[VR_AC].val;
        writeReg(passP, VR_AC, passP->reg[VR_IO].val, wordP->addr);
        writeReg(passP, VR_IO, swapped, wordP->addr);
        setPending(passP, VR_AC, wordP, VC_DLOAD, NILP, 1);
        setPending(passP, VR_IO, wordP, VC_DLOAD, NILP, 1);
        passP->swpAddr = wordP->addr;
        passP->swpDeadMask = 0;
        return;

    default:
        break;
    }

    // The general word, in phase order (optimizer.h, OptPhase).
    ac.val = passP->reg[VR_AC].val;
    ac.src = 1;
    io.val = passP->reg[VR_IO].val;
    io.src = 2;
    writesAc = 0;
    writesIo = 0;

    if( bits & OPTM_CLA )           // TP7
    {
        ac.val = 0;
        ac.src = 0;
        writesAc = 1;
    }

    if( bits & OPTM_CLI )           // TP7
    {
        io.val = 0;
        io.src = 0;
        writesIo = 1;
    }

    if( bits & OPTM_CMI )           // TP8
    {
        io.val = complementVal(passP, io.val);
        writesIo = 1;
    }

    if( bits & (OPTM_LAT | OPTM_LAP) )  // TP8: an OR into AC, value unknown
    {
        ac.val = freshVal(passP);
        writesAc = 1;
    }

    if( bits & OPTM_CMA )           // TP9
    {
        ac.val = complementVal(passP, ac.val);
        writesAc = 1;
    }

    newAc = ac;
    newIo = io;

    if( bits & OPTM_LAI )           // the next fetch, from the registers as they now are
    {
        newAc = io;
        writesAc = 1;
    }

    if( bits & OPTM_LIA )
    {
        newIo = ac;
        writesIo = 1;
    }

    readsAc = ((writesAc && (newAc.src & 1)) || (writesIo && (newIo.src & 1)));
    readsIo = ((writesAc && (newAc.src & 2)) || (writesIo && (newIo.src & 2)));

    if( bits & OPTM_HLT )
    {
        readsAc = 1;
        readsIo = 1;
    }

    if( readsAc )
    {
        useReg(passP, VR_AC);
    }

    if( readsIo )
    {
        useReg(passP, VR_IO);
    }

    if( writesAc )
    {
        writeReg(passP, VR_AC, newAc.val, wordP->addr);
    }

    if( writesIo )
    {
        writeReg(passP, VR_IO, newIo.val, wordP->addr);
    }
}

// The PDP-1D special operate group, by its event times: sci and scf, then
// iif and ifi from one copy of IO taken after sci has acted, then ida.
static void
doSpecial(VPass *passP, OptWordP wordP)
{
unsigned int bits;

    bits = (wordP->decode.specialBits & OPTX_ALLBITS);

    if( bits == OPTX_SCI )
    {
        doCandidate(passP, wordP, VR_IO, 0, VC_RCLEAR, VC_DCLEAR, -1);
        return;
    }

    if( (bits & (OPTX_IIF | OPTX_IFI)) && !(bits & OPTX_SCI) )
    {
        useReg(passP, VR_IO);
    }

    if( bits & OPTX_IDA )
    {
        useReg(passP, VR_AC);
    }

    if( bits & OPTX_IIF )
    {
        writeReg(passP, VR_IO, freshVal(passP), wordP->addr);
    }
    else if( bits & OPTX_SCI )
    {
        writeReg(passP, VR_IO, 0, wordP->addr);
    }

    if( bits & OPTX_IDA )
    {
        writeReg(passP, VR_AC, freshVal(passP), wordP->addr);
    }
}

// One word of the block.
static void
doWord(VPass *passP, OptWordP wordP)
{
const VRow *rowP;
OptDecodeP decodeP;
int facts;
long val;

    decodeP = &wordP->decode;

    // The carried pass names this word's values afresh, so whatever still
    // holds its names from an earlier walk of it holds an older instance.
    if( passP->carry )
    {
        passP->curIndex = wordP->index;
        passP->curSlot = 0;
        retireNames(passP, (VAL_CARRY + ((long)wordP->index * VSLOTS)), VSLOTS);
    }

    // A word the program writes or patches is not the assembled word.
    if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED) )
    {
        killAll(passP, wordP, VK_WRITTEN);
        return;
    }

    rowP = findRow(wordP);

    if( rowP->effects & VE_KILL )
    {
        if( (decodeP->group == OPTG_IOT) && passP->iotFree )
        {
            useReg(passP, VR_AC);
            useReg(passP, VR_IO);
            return;
        }

        if( (decodeP->group == OPTG_IOT) && passP->optimistic )
        {
            useReg(passP, VR_AC);
            useReg(passP, VR_IO);
            writeReg(passP, VR_IO, freshVal(passP), wordP->addr);
            return;
        }

        killAll(passP, wordP, (decodeP->group == OPTG_IOT)?VK_IOT:
            ((decodeP->group == OPTG_UNKNOWN)?VK_SPARE:VK_XCT));
        return;
    }

    switch( rowP->sem )
    {
    case VS_OPERATE:
        doOperate(passP, wordP);
        return;

    case VS_SPECIAL:
        doSpecial(passP, wordP);
        return;

    case VS_LAW:
        val = (decodeP->indirect)?((~(long)decodeP->address) & WRDMASK):(long)decodeP->address;
        doCandidate(passP, wordP, VR_AC, val, VC_RLOAD, VC_DLOAD, -1);
        return;

    case VS_LOADAC:
    case VS_LOADIO:
        if( !decodeP->memIndirect )
        {
            doLoad(passP, wordP, (rowP->sem == VS_LOADAC)?VR_AC:VR_IO);
            return;
        }
        break;

    default:
        break;
    }

    // jda and cal deposit AC in Y (cal in 0100) before they jump (F-15D p.18).
    // The word ends its block, so only the carried pass sees the fact.
    if( passP->carry && (decodeP->group == OPTG_MEMREF) && (decodeP->opcode == 016) )
    {
        setFact(passP, ((decodeP->indirect)?decodeP->address:0100), passP->reg[VR_AC].val, wordP->addr);
    }

    // Everything else follows its row: the reads, then the memory write,
    // then the register writes.
    if( rowP->effects & VE_RAC )
    {
        useReg(passP, VR_AC);
    }

    if( rowP->effects & VE_RIO )
    {
        useReg(passP, VR_IO);
    }

    if( (rowP->effects & (VE_WMEM | VE_PMEM)) && !(rowP->effects & VE_ENDS) )
    {
        if( decodeP->memIndirect )
        {
            killMemory(passP, wordP);
        }
        else
        {
            switch( rowP->sem )
            {
            case VS_STOREAC:
                setFact(passP, decodeP->address, passP->reg[VR_AC].val, wordP->addr);
                break;

            case VS_STOREIO:
                setFact(passP, decodeP->address, passP->reg[VR_IO].val, wordP->addr);
                break;

            case VS_ZERO:
                setFact(passP, decodeP->address, 0, wordP->addr);
                break;

            case VS_PARTIAL:
                facts = (findFact(passP, decodeP->address) >= 0)?1:0;
                setFact(passP, decodeP->address, freshVal(passP), wordP->addr);
                noteKill(passP, wordP, VK_PARTIAL, facts);
                break;

            default:
                break;
            }
        }
    }

    if( rowP->sem == VS_INDEX )
    {
        val = freshVal(passP);

        if( decodeP->memIndirect )
        {
            killMemory(passP, wordP);
        }
        else
        {
            setFact(passP, decodeP->address, val, wordP->addr);
        }

        writeReg(passP, VR_AC, val, wordP->addr);
        return;
    }

    if( rowP->effects & VE_WAC )
    {
        writeReg(passP, VR_AC, freshVal(passP), wordP->addr);
    }

    if( rowP->effects & VE_WIO )
    {
        writeReg(passP, VR_IO, freshVal(passP), wordP->addr);
    }
}

// The block walk.

// Where a hit's span sits with respect to the declared regions.
// Returns the place.
static OptRegionPlace
spanPlace(VPass *passP, VHitP hitP)
{
OptWordP wordP;
int addr;
int in;
int out;

    if( !passP->tableP->regionCount )
    {
        return(OPTREG_NOREGIONS);
    }

    in = 0;
    out = 0;

    for( addr = hitP->spanFrom; addr <= hitP->spanTo; ++addr )
    {
        if( !(wordP = wordAt(passP->tableP, passP->bank, addr)) )
        {
            continue;
        }

        if( wordP->flags & OPTF_INREGION )
        {
            ++in;
        }
        else
        {
            ++out;
        }
    }

    if( in && !out )
    {
        return(OPTREG_INSIDE);
    }

    if( out && !in )
    {
        return(OPTREG_OUTSIDE);
    }

    return(OPTREG_PARTIAL);
}

// The safety flags of a hit.
// Returns the VF_ bits.
static unsigned int
spanFlags(VPass *passP, VHitP hitP)
{
OptWordP wordP;
unsigned int flags;
int addr;

    flags = 0;

    if( (hitP->memAddr >= 0) && handlerWritten(passP, passP->bank, hitP->memAddr) )
    {
        flags |= VF_HANDLER;
    }

    if( hitP->wordP->flags & OPTF_AFTERSKIP )
    {
        flags |= VF_AFTERSKIP;
    }

    if( hitP->wordP->flags & (OPTF_TAKEN | OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN | OPTF_MAYBE_ENTERED) )
    {
        flags |= VF_TAKEN;
    }

    if( hitP->wordP->flags & (OPTF_UNREACHED | OPTF_XCTONLY) )
    {
        flags |= VF_UNREACHED;
    }

    for( addr = hitP->spanFrom; addr <= hitP->spanTo; ++addr )
    {
        if( !(wordP = wordAt(passP->tableP, passP->bank, addr)) )
        {
            continue;
        }

        if( wordP->flags & OPTF_XCTTARGET )
        {
            flags |= VF_XCT;
        }

        if( (addr > hitP->spanFrom) && (wordP->flags & OPTF_HASLABEL) )
        {
            flags |= VF_LABEL;
        }
    }

    return(flags);
}

// Start a block: both registers unknown, no facts, nothing waiting.
static void
beginBlock(VPass *passP, OptBlockP blockP)
{
int i;

    passP->blockP = blockP;
    passP->bank = blockP->bank;

    // The carried pass names a block's unknown entry values for the block.
    if( passP->carry )
    {
        passP->reg[VR_AC].val = (VAL_ENTRY + ((long)blockP->id * 2) + VR_AC);
        passP->reg[VR_IO].val = (VAL_ENTRY + ((long)blockP->id * 2) + VR_IO);
    }
    else
    {
        passP->reg[VR_AC].val = freshVal(passP);
        passP->reg[VR_IO].val = freshVal(passP);
    }

    passP->reg[VR_AC].since = -1;
    passP->reg[VR_IO].since = -1;
    passP->factCount = 0;
    passP->pend[VR_AC].active = 0;
    passP->pend[VR_IO].active = 0;
    passP->exch.active = 0;
    passP->swpAddr = -1;
    passP->swpDeadMask = 0;

    if( blockP->wordCount > passP->blockCap )
    {
        free(passP->blockHitsPP);
        free(passP->blockKillsP);
        free(passP->blockKillNP);
        passP->blockCap = blockP->wordCount;
        passP->blockHitsPP = (VHitP *)vAlloc((size_t)passP->blockCap, sizeof(VHitP));
        passP->blockKillsP = (VKill *)vAlloc((size_t)passP->blockCap, sizeof(VKill));
        passP->blockKillNP = (int *)vAlloc((size_t)passP->blockCap, sizeof(int));
    }

    for( i = 0; i < blockP->wordCount; ++i )
    {
        passP->blockHitsPP[i] = NILP;
        passP->blockKillsP[i] = VK_NONE;
        passP->blockKillNP[i] = 0;
    }
}

// Count and print one finished hit, and keep it for the control leg.
static void
finishHit(VPass *passP, VHitP hitP)
{
VKnown *knownP;
int bank;
int i;
char spell[OPTMSG_SIZE];
char flagText[OPTMSG_SIZE];
char knownText[64];
int at;

    bank = passP->bank;
    hitP->place = spanPlace(passP, hitP);
    hitP->flags = spanFlags(passP, hitP);

    ++passP->bankHits[bank];
    ++passP->classHits[bank][hitP->cls];
    passP->classWords[bank][hitP->cls] += hitP->words;
    passP->classUs[bank][hitP->cls] += hitP->us;
    ++passP->totalHits;
    ++passP->placeHits[hitP->place];

    if( (hitP->cls != hitP->firstCls) && classTable[hitP->firstCls].neutral )
    {
        ++passP->neutralToo;
    }

    for( i = 0; i < VF_COUNT; ++i )
    {
        if( hitP->flags & (1u << i) )
        {
            ++passP->flagHits[i];
        }
    }

    knownP = &knownOfWordP[hitP->wordP->index];
    knownText[0] = '\0';

    if( knownP->rule )
    {
        ++passP->knownHits;
        ++passP->knownRule[knownP->rule - 1];

        if( knownP->supp )
        {
            ++passP->knownSupp;
        }

        snprintf(knownText, sizeof(knownText), "%s%s", optRuleName((OptRuleId)(knownP->rule - 1)),
            (knownP->supp)?"/supp":"");
    }
    else
    {
        snprintf(knownText, sizeof(knownText), "new");
    }

    if( !passP->hitsTailP )
    {
        passP->hitsP = hitP;
    }
    else
    {
        passP->hitsTailP->nextP = hitP;
    }

    passP->hitsTailP = hitP;
    passP->hitOfWordPP[hitP->wordP->index] = hitP;

    if( !passP->fP )
    {
        return;
    }

    at = 0;
    flagText[0] = '\0';

    for( i = 0; i < VF_COUNT; ++i )
    {
        if( hitP->flags & (1u << i) )
        {
            at += snprintf((flagText + at), (sizeof(flagText) - (size_t)at), "%s%s", (at)?",":"", flagNames[i]);
        }
    }

    spellWord(hitP->wordP, spell, sizeof(spell));
    fprintf(passP->fP, "hit %s b%d %04o w%d u%d span %04o-%04o %s %s %s | %06o %s => %s | %s\n",
        classTable[hitP->cls].nameP, bank, hitP->wordP->addr, hitP->words, hitP->us,
        hitP->spanFrom, hitP->spanTo, optRegionPlaceName(hitP->place), (at)?flagText:"-", knownText,
        (hitP->wordP->value & WRDMASK), spell, hitP->replaceP, hitP->whyP);
}

// Finish a block: a skip ends it and so kills everything; print the kills
// and hits in address order.
static void
endBlock(VPass *passP)
{
OptBlockP blockP;
OptWordP wordP;
int i;
int facts;
char spell[OPTMSG_SIZE];

    blockP = passP->blockP;

    if( blockP->endKind == OPTBE_SKIP )
    {
        facts = factsHeld(passP);

        if( (facts > 0) && (passP->blockKillsP[blockP->wordCount - 1] == VK_NONE) )
        {
            noteKill(passP, blockP->lastP, VK_SKIP, facts);
        }
    }

    for( i = 0; i < blockP->wordCount; ++i )
    {
        if( passP->blockKillsP[i] != VK_NONE )
        {
            if( passP->fP && (wordP = wordAt(passP->tableP, passP->bank, (blockP->startAddr + i))) )
            {
                spellWord(wordP, spell, sizeof(spell));
                fprintf(passP->fP, "kill %s b%d %04o facts %d | %06o %s\n", killNames[passP->blockKillsP[i]],
                    passP->bank, wordP->addr, passP->blockKillNP[i], (wordP->value & WRDMASK), spell);
            }
        }

        if( passP->blockHitsPP[i] )
        {
            finishHit(passP, passP->blockHitsPP[i]);
        }
    }
}

// Classify a block's entry for the blind-spot count of what its reset loses.
static void
countEntry(VPass *passP, OptBlockP blockP)
{
    ++passP->blocks;

    if( blockP->isEntry )
    {
        ++passP->blockEntry;
    }
    else if( blockP->predCount > 1 )
    {
        ++passP->blockMerge;
    }
    else if( blockP->predCount == 1 )
    {
        ++passP->blockSingle;
    }
    else
    {
        ++passP->blockNone;
    }
}

// Walk every block of the table.
static void
runPass(VPass *passP)
{
OptTableP tableP;
OptBlockP blockP;
OptWordP wordP;
int addr;

    tableP = passP->tableP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        passP->bankUsed[blockP->bank] = 1;
        countEntry(passP, blockP);
        beginBlock(passP, blockP);

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( (wordP = wordAt(tableP, blockP->bank, addr)) && (wordP->blockP == blockP) )
            {
                doWord(passP, wordP);
            }
        }

        endBlock(passP);
    }
}

// Release what a pass allocated.
static void
freePass(VPass *passP)
{
VHitP hitP;
VHitP nextP;
int bank;

    for( hitP = passP->hitsP; hitP; hitP = nextP )
    {
        nextP = hitP->nextP;
        free(hitP->replaceP);
        free(hitP->whyP);
        free(hitP);
    }

    if( !passP->borrowed )
    {
        for( bank = 0; bank <= MAXBANK; ++bank )
        {
            free(passP->handlerWrittenP[bank]);
        }

        free(passP->handlerBlockP);
    }

    free(passP->hitOfWordPP);
    free(passP->factsP);
    free(passP->blockHitsPP);
    free(passP->blockKillsP);
    free(passP->blockKillNP);
}

// Set a pass up over a table.
static void
initPass(VPass *passP, OptTableP tableP, FILE *fP, int optimistic)
{
    memset(passP, 0, sizeof(*passP));
    passP->tableP = tableP;
    passP->fP = fP;
    passP->optimistic = optimistic;
    passP->nextFresh = VAL_FRESH;
    passP->swpAddr = -1;
    passP->hitOfWordPP = (VHitP *)vAlloc((size_t)tableP->count, sizeof(VHitP));
    findHandlerSet(passP);
}

// The existing rules, for the overlap count and the control leg.

// Mark the word of each T6, T7, T8 and T14 finding, live or suppressed, that
// the pass would name: the clear, the load, or T14's load(s).
static void
markKnown(OptTableP tableP)
{
OptFindingP findingP;
int pass;
int first;
int last;
int i;

    knownOfWordP = (VKnown *)vAlloc((size_t)tableP->count, sizeof(VKnown));

    for( pass = 0; pass < 2; ++pass )
    {
        for( findingP = (pass)?tableP->suppressedP:tableP->findingsP; findingP; findingP = findingP->nextP )
        {
            switch( findingP->rule )
            {
            case OPTRULE_T6:
            case OPTRULE_T7:
            case OPTRULE_T8A:
            case OPTRULE_T8B:
            case OPTRULE_T8C:
            case OPTRULE_T8D:
                first = 0;
                last = 0;
                break;

            case OPTRULE_T14:
                first = (findingP->wordCount == 4)?2:1;
                last = (findingP->wordCount == 4)?3:1;
                break;

            default:
                continue;
            }

            for( i = first; i <= last; ++i )
            {
                if( (i < findingP->wordCount) && findingP->wordsP[i] && !knownOfWordP[findingP->wordsP[i]->index].rule )
                {
                    knownOfWordP[findingP->wordsP[i]->index].rule = ((int)findingP->rule + 1);
                    knownOfWordP[findingP->wordsP[i]->index].supp = pass;
                }
            }
        }
    }
}

// Did the pass re-find one finding?  T6 and T7: a clear hit, dead or
// redundant, at the clear.  T14: a lai or lia hit (whatever it was later
// counted as) at the load, and for the exchange a swap at the second load.
// Returns 1 when it did, 0 when it did not; *whatPP names what was found.
static int
refound(VPass *passP, OptFindingP findingP, const char **whatPP)
{
VHitP hitP;
VHitP secondP;

    *whatPP = "nothing";

    switch( findingP->rule )
    {
    case OPTRULE_T6:
    case OPTRULE_T7:
        if( !(hitP = passP->hitOfWordPP[findingP->wordsP[0]->index]) )
        {
            return(0);
        }

        *whatPP = classTable[hitP->cls].nameP;
        return( (hitP->cls == VC_DCLEAR) || (hitP->cls == VC_RCLEAR) );

    case OPTRULE_T14:
        if( findingP->wordCount == 4 )
        {
            hitP = passP->hitOfWordPP[findingP->wordsP[2]->index];
            secondP = passP->hitOfWordPP[findingP->wordsP[3]->index];

            if( !hitP || !secondP )
            {
                *whatPP = (hitP)?classTable[hitP->cls].nameP:((secondP)?classTable[secondP->cls].nameP:"nothing");
                return(0);
            }

            *whatPP = classTable[secondP->cls].nameP;
            return( ((hitP->firstCls == VC_LAI) || (hitP->firstCls == VC_LIA)) && (secondP->firstCls == VC_SWAP) );
        }

        if( !(hitP = passP->hitOfWordPP[findingP->wordsP[1]->index]) )
        {
            return(0);
        }

        *whatPP = classTable[hitP->firstCls].nameP;
        return( (hitP->firstCls == VC_LAI) || (hitP->firstCls == VC_LIA) );

    default:
        return(0);
    }
}

// The control leg: one line per T6, T7 and T14 finding, live then suppressed.
// A live finding the pass does not re-find means the pass is wrong: MISSED.
static void
writeControl(FILE *fP, VPass *passP, int *liveP, int *liveFoundP, int *suppP, int *suppFoundP)
{
OptFindingP findingP;
const char *whatP;
int pass;
int found;

    *liveP = 0;
    *liveFoundP = 0;
    *suppP = 0;
    *suppFoundP = 0;

    for( pass = 0; pass < 2; ++pass )
    {
        for( findingP = (pass)?passP->tableP->suppressedP:passP->tableP->findingsP; findingP; findingP = findingP->nextP )
        {
            if( (findingP->rule != OPTRULE_T6) && (findingP->rule != OPTRULE_T7) && (findingP->rule != OPTRULE_T14) )
            {
                continue;
            }

            found = refound(passP, findingP, &whatP);

            if( pass )
            {
                ++*suppP;
                *suppFoundP += found;
                fprintf(fP, "control supp %s b%d %04o %s %s %s\n", optRuleName(findingP->rule), findingP->bank,
                    findingP->addr, optWhyName(findingP->why), (found)?"refound":"notfound", whatP);
            }
            else
            {
                ++*liveP;
                *liveFoundP += found;
                fprintf(fP, "control live %s b%d %04o %s %s\n", optRuleName(findingP->rule), findingP->bank,
                    findingP->addr, (found)?"refound":"MISSED", whatP);
            }
        }
    }
}

// The dump.

// Print one line of class tallies with its prefix.
static void
writeTally(FILE *fP, const char *prefixP, int *hitsP, int *wordsP, int *usP)
{
int i;
int hits;
int words;
int us;

    hits = 0;
    words = 0;
    us = 0;
    fprintf(fP, "%s:", prefixP);

    for( i = 0; i < VC_COUNT; ++i )
    {
        fprintf(fP, " %s %d", classTable[i].nameP, hitsP[i]);
        hits += hitsP[i];
        words += wordsP[i];
        us += usP[i];
    }

    fprintf(fP, " = %d hits, %d words, %d us\n", hits, words, us);
}

// Count the swp;swp pairs: two exchanges in a row, the second not the start
// of a block, so the pair always runs together and changes nothing.
static void
countSwpSwp(FILE *fP, VPass *passP)
{
OptTableP tableP;
OptBlockP blockP;
OptWordP wordP;
OptWordP nextP;
int addr;
char spell[OPTMSG_SIZE];

    tableP = passP->tableP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr < blockP->endAddr; ++addr )
        {
            wordP = wordAt(tableP, blockP->bank, addr);
            nextP = wordAt(tableP, blockP->bank, (addr + 1));

            if( !wordP || !nextP || (wordP->decode.group != OPTG_OPERATE) || (nextP->decode.group != OPTG_OPERATE) )
            {
                continue;
            }

            if( ((wordP->decode.microBits & OPTM_ALLBITS) != (OPTM_LAI | OPTM_LIA))
                || ((nextP->decode.microBits & OPTM_ALLBITS) != (OPTM_LAI | OPTM_LIA)) )
            {
                continue;
            }

            ++passP->swpSwp;

            if( fP )
            {
                spellWord(wordP, spell, sizeof(spell));
                fprintf(fP, "annihilate swp-swp b%d %04o | %s ; %s\n", blockP->bank, addr, spell, spell);
            }
        }
    }
}

// Print the value dump (-O=values).  The format:
//
//   values: ...                          what the dump is, and the pass's rules
//   effect NAME GROUP CODE EFFECTS SEM | what | citation     one per row
//   kill KIND bBANK ADDR facts N | value spelling           a fact died here
//   hit CLASS bBANK ADDR wWORDS uUS span FROM-TO PLACE FLAGS KNOWN | value spelling => replacement | evidence
//   annihilate swp-swp|swp-dead bBANK ADDR | ...
//   control live|supp RULE bBANK ADDR [WHY] refound|MISSED|notfound CLASS
//   bank N: CLASS n ... = H hits, W words, U us
//   the summary lines, ending with reconcile
//
// FLAGS is "-" or a comma list of handler, afterskip, xct, taken, label,
// unreached; KNOWN is the rule of a finding that names the same word (with
// /supp when that finding is suppressed) or "new".
void
optDumpValues(FILE *fP, OptTableP tableP)
{
static const char *groupNames[] = { "spare", "memref", "law", "skip", "shift", "operate", "iot", "1d" };
VPass pass;
VPass optimisticPass;
const VRow *rowP;
int i;
int bank;
int uncited;
int live;
int liveFound;
int supp;
int suppFound;
int neutralHits;
int neutralUs;
int deletingHits;
int deletingWords;
int deletingUs;
int sum;
int reconciled;
int totalHits[VC_COUNT];
int totalWords[VC_COUNT];
int totalUs[VC_COUNT];
char effects[128];
char code[32];

    markKnown(tableP);

    // the effect table
    uncited = 0;

    for( i = 0; i < VROW_COUNT; ++i )
    {
        if( !effectRows[i].citeP || !effectRows[i].citeP[0] )
        {
            ++uncited;
        }
    }

    fprintf(fP, "values: effect table %d rows, %d without a citation\n", VROW_COUNT, uncited);

    for( i = 0; i < VROW_COUNT; ++i )
    {
        rowP = &effectRows[i];
        spellEffects(rowP->effects, effects, sizeof(effects));

        if( rowP->group == VG_NONE )
        {
            snprintf(code, sizeof(code), "%s:%05o", (rowP->sem == VS_SPECIAL)?"1d":"operate", rowP->code);
        }
        else if( rowP->group == OPTG_MEMREF )
        {
            snprintf(code, sizeof(code), "memref:%02o%s", rowP->code, (rowP->ibit > 0)?"i":"");
        }
        else if( rowP->group == OPTG_SHIFT )
        {
            snprintf(code, sizeof(code), "shift:regs%d", rowP->code);
        }
        else if( (rowP->group == OPTG_IOT) && (rowP->code >= 0) )
        {
            snprintf(code, sizeof(code), "iot:%06o", rowP->code);
        }
        else
        {
            snprintf(code, sizeof(code), "%s", groupNames[rowP->group]);
        }

        fprintf(fP, "effect %-10s %-16s %s | %s | %s\n", rowP->nameP, code, effects, rowP->whatP,
            (rowP->citeP)?rowP->citeP:"-");
    }

    fprintf(fP, "values: each block starts with AC and IO unknown; a fact dies at a write to its word,\n");
    fprintf(fP, "values: an indirect write, xct, an unknown iot, a spare code, a word the program writes, a skip\n");
    fprintf(fP, "values: devices that write memory (drum, DCS2, high-speed channel) are invisible: a limit, not a count\n");

    // the pass, as the table says
    initPass(&pass, tableP, fP, 0);
    runPass(&pass);
    countSwpSwp(fP, &pass);
    writeControl(fP, &pass, &live, &liveFound, &supp, &suppFound);

    // and again, with an unknown iot touching IO alone
    initPass(&optimisticPass, tableP, NILP, 1);
    runPass(&optimisticPass);

    // the tallies
    memset(totalHits, 0, sizeof(totalHits));
    memset(totalWords, 0, sizeof(totalWords));
    memset(totalUs, 0, sizeof(totalUs));
    reconciled = 1;
    sum = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        int bankSum;

        if( !pass.bankUsed[bank] )
        {
            continue;
        }

        fprintf(fP, "bank %d", bank);
        writeTally(fP, "", pass.classHits[bank], pass.classWords[bank], pass.classUs[bank]);
        bankSum = 0;

        for( i = 0; i < VC_COUNT; ++i )
        {
            totalHits[i] += pass.classHits[bank][i];
            totalWords[i] += pass.classWords[bank][i];
            totalUs[i] += pass.classUs[bank][i];
            bankSum += pass.classHits[bank][i];
        }

        if( bankSum != pass.bankHits[bank] )
        {
            reconciled = 0;
            fprintf(fP, "reconcile: bank %d classes %d, hits %d\n", bank, bankSum, pass.bankHits[bank]);
        }

        sum += pass.bankHits[bank];
    }

    writeTally(fP, "total", totalHits, totalWords, totalUs);

    neutralHits = 0;
    neutralUs = 0;
    deletingHits = 0;
    deletingWords = 0;
    deletingUs = 0;

    for( i = 0; i < VC_COUNT; ++i )
    {
        if( classTable[i].neutral )
        {
            neutralHits += totalHits[i];
            neutralUs += totalUs[i];
        }
        else
        {
            deletingHits += totalHits[i];
            deletingWords += totalWords[i];
            deletingUs += totalUs[i];
        }
    }

    fprintf(fP, "neutral: %d hits, %d us (lai %d, lia %d, law %d, lawk %d)\n", neutralHits, neutralUs,
        totalHits[VC_LAI], totalHits[VC_LIA], totalHits[VC_LAW], totalHits[VC_LAWK]);
    fprintf(fP, "deleting: %d hits, %d words, %d us (%d of them found layout-neutral first)\n",
        deletingHits, deletingWords, deletingUs, pass.neutralToo);
    fprintf(fP, "known: %d hits (T6 %d, T7 %d, T8 %d, T14 %d; %d of them suppressed findings), new %d\n",
        pass.knownHits, pass.knownRule[OPTRULE_T6], pass.knownRule[OPTRULE_T7],
        (pass.knownRule[OPTRULE_T8A] + pass.knownRule[OPTRULE_T8B] + pass.knownRule[OPTRULE_T8C] + pass.knownRule[OPTRULE_T8D]),
        pass.knownRule[OPTRULE_T14], pass.knownSupp, (pass.totalHits - pass.knownHits));
    fprintf(fP, "region: noregions %d, inside %d, outside %d, partial %d\n", pass.placeHits[OPTREG_NOREGIONS],
        pass.placeHits[OPTREG_INSIDE], pass.placeHits[OPTREG_OUTSIDE], pass.placeHits[OPTREG_PARTIAL]);
    fprintf(fP, "flags:");

    for( i = 0; i < VF_COUNT; ++i )
    {
        fprintf(fP, "%s %s %d", (i)?",":"", flagNames[i], pass.flagHits[i]);
    }

    fprintf(fP, "\nkills:");

    for( i = 1; i < VK_COUNT; ++i )
    {
        fprintf(fP, "%s %s %d", (i > 1)?",":"", killNames[i], pass.kills[i]);
    }

    fprintf(fP, "\nblind: blocks %d reset at entry (single-predecessor %d, merge %d, entry %d, unentered %d);"
        " handler-written hits %d; handler words %d, handler indirect writes %d; iot-optimistic %+d hits\n",
        pass.blocks, pass.blockSingle, pass.blockMerge, pass.blockEntry, pass.blockNone,
        pass.flagHits[0], pass.handlerWords, pass.handlerIndirect, (optimisticPass.totalHits - pass.totalHits));
    fprintf(fP, "annihilate: swp-swp %d, swp-dead %d\n", pass.swpSwp, pass.swpDead);
    fprintf(fP, "control: live %d/%d refound, suppressed %d/%d refound\n", liveFound, live, suppFound, supp);

    // the reconciliation
    if( sum != pass.totalHits )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: banks %d, hits %d\n", sum, pass.totalHits);
    }

    if( (neutralHits + deletingHits) != pass.totalHits )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: neutral %d + deleting %d, hits %d\n", neutralHits, deletingHits, pass.totalHits);
    }

    if( (pass.placeHits[OPTREG_NOREGIONS] + pass.placeHits[OPTREG_INSIDE] + pass.placeHits[OPTREG_OUTSIDE]
         + pass.placeHits[OPTREG_PARTIAL]) != pass.totalHits )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: region places do not add up to %d\n", pass.totalHits);
    }

    fprintf(fP, "reconcile: %s\n", (reconciled)?"ok":"FAILED");

    freePass(&pass);
    freePass(&optimisticPass);
    free(knownOfWordP);
    knownOfWordP = NILP;
}

// The carried pass (-O=carry, task A52).  The walk is the one above; what
// changes is what a block knows at its entry.

// The most rounds the solver makes before it calls a run unsettled.
#define VMAXROUNDS  200

// The rounds after which what a block starts from may only fall.  A walk is
// not monotone: a register unknown at a block's entry leaves under the
// block's own name, which disagrees at the next merge where a value would
// not, so two merges in one loop can hand each other agreement and
// disagreement in turn for ever (Sketchpad's wkClose and wkStep did).  Past
// this many rounds a block starts from the meet of everything it has been
// given since, which can only lose registers and facts, so the run ends; what
// it finds is still true, only less.  Every run that settles sooner is
// untouched.
#define VWIDEN      40

// The symbols a summary run starts with: IO at the entry, and the AC jda
// deposited below it.
#define SYM_AC      (VAL_SYM + VR_AC)
#define SYM_IO      (VAL_SYM + VR_IO)

// One edge into a block.
typedef struct vpred
{
    struct vpred *nextP;
    OptBlockP fromP;
    OptFlowEdgeP edgeP;
} VPred;

// One carried run: a leg over every block, or a summary run over one body.
typedef struct
{
    VPass *passP;
    OptTableP tableP;
    OptCarryMapsP mapsP;
    OptPreserveP sumsP;
    int leg;                // 0 to 3, or -1 for a summary run
    unsigned int mode;      // OPTCM_ bits
    OptRoutineP routineP;   // a summary run's routine
    OptBodyRun *runP;       // and what its last walk finds
    int count;              // blockCount
    unsigned char *scopeP;  // by block id: the block is walked
    unsigned char *fixedP;  // by block id: the block starts knowing nothing
    unsigned char *outsideP;    // by block id: an edge arrives from outside the scope
    VPred **predsPP;        // by block id
    VPred *predPoolP;
    OptCarryState *outP;    // by block id: what the block knows at its end
    OptCarryState *preP;    // and just before its call word
    OptCarryState *widenP;  // by block id: what it starts from once the run widens
    OptCarryState seed;     // a summary run's entry
    int rounds;
    int settled;
    int widened;            // the run went past VWIDEN rounds

    // the blocks, for the dump
    int walked;
    int carried;
    int fixedEntry;
    int fixedUnreached;
    int fixedTaken;
    int fixedAfterCall;
} VCarry;

// The dump's own pass, whose handler set every carried run borrows.
static VPass *carryHomeP;

// Carried states.

// Make room for need facts.
static void
stateRoom(OptCarryState *stateP, int need)
{
    if( need <= stateP->factCap )
    {
        return;
    }

    stateP->factCap = (need < 16)?16:(need * 2);

    if( !(stateP->factsP = (OptCarryFact *)realloc(stateP->factsP, ((size_t)stateP->factCap * sizeof(OptCarryFact)))) )
    {
        fprintf(stderr, "am1: out of memory measuring values\n");
        exit(1);
    }
}

// Free a carried state's facts.
void
optCarryStateFree(OptCarryState *stateP)
{
    free(stateP->factsP);
    stateP->factsP = NILP;
    stateP->factCount = 0;
    stateP->factCap = 0;
    stateP->known = 0;
}

// Set a carried state to know nothing: both registers unknown, no facts.
void
optCarryStateUnknown(OptCarryState *stateP)
{
    stateP->known = 1;
    stateP->reg[VR_AC] = OPTVAL_UNKNOWN;
    stateP->reg[VR_IO] = OPTVAL_UNKNOWN;
    stateP->factCount = 0;
}

// Record that a word holds a value, replacing any fact about it; an unknown
// value removes the fact.
void
optCarryStateSetFact(OptCarryState *stateP, int addr, long val)
{
int i;

    for( i = 0; i < stateP->factCount; ++i )
    {
        if( stateP->factsP[i].addr == addr )
        {
            break;
        }
    }

    if( val == OPTVAL_UNKNOWN )
    {
        if( i < stateP->factCount )
        {
            stateP->factsP[i] = stateP->factsP[--stateP->factCount];
        }

        return;
    }

    if( i == stateP->factCount )
    {
        stateRoom(stateP, (stateP->factCount + 1));
        ++stateP->factCount;
    }

    stateP->factsP[i].addr = addr;
    stateP->factsP[i].val = val;
}

// Copy one carried state over another.
void
optCarryStateCopy(OptCarryState *toP, const OptCarryState *fromP)
{
    if( toP == fromP )
    {
        return;
    }

    stateRoom(toP, fromP->factCount);

    if( fromP->factCount )
    {
        memcpy(toP->factsP, fromP->factsP, ((size_t)fromP->factCount * sizeof(OptCarryFact)));
    }

    toP->known = fromP->known;
    toP->reg[VR_AC] = fromP->reg[VR_AC];
    toP->reg[VR_IO] = fromP->reg[VR_IO];
    toP->factCount = fromP->factCount;
}

// The value a state gives a word, OPTVAL_UNKNOWN for none.
// Returns the value.
static long
stateFact(const OptCarryState *stateP, int addr)
{
int i;

    for( i = 0; i < stateP->factCount; ++i )
    {
        if( stateP->factsP[i].addr == addr )
        {
            return(stateP->factsP[i].val);
        }
    }

    return(OPTVAL_UNKNOWN);
}

// Do two states say the same, facts in any order?
// Returns 1 when they do, 0 when they do not.
static int
stateEqual(const OptCarryState *aP, const OptCarryState *bP)
{
int i;

    if( aP->known != bP->known )
    {
        return(0);
    }

    if( !aP->known )
    {
        return(1);
    }

    if( (aP->reg[VR_AC] != bP->reg[VR_AC]) || (aP->reg[VR_IO] != bP->reg[VR_IO])
        || (aP->factCount != bP->factCount) )
    {
        return(0);
    }

    for( i = 0; i < aP->factCount; ++i )
    {
        if( stateFact(bP, aP->factsP[i].addr) != aP->factsP[i].val )
        {
            return(0);
        }
    }

    return(1);
}

// The meet: what both states say.  A state not known yet adds nothing.
static void
stateMeet(OptCarryState *intoP, const OptCarryState *fromP)
{
int i;
int at;

    if( !fromP->known )
    {
        return;
    }

    if( !intoP->known )
    {
        optCarryStateCopy(intoP, fromP);
        return;
    }

    if( intoP->reg[VR_AC] != fromP->reg[VR_AC] )
    {
        intoP->reg[VR_AC] = OPTVAL_UNKNOWN;
    }

    if( intoP->reg[VR_IO] != fromP->reg[VR_IO] )
    {
        intoP->reg[VR_IO] = OPTVAL_UNKNOWN;
    }

    for( i = 0, at = 0; i < intoP->factCount; ++i )
    {
        if( stateFact(fromP, intoP->factsP[i].addr) == intoP->factsP[i].val )
        {
            intoP->factsP[at++] = intoP->factsP[i];
        }
    }

    intoP->factCount = at;
}

// What the walk holds now, as a carried state; a lost name is unknown, since
// nothing past this block can tell its instances apart.
static void
captureState(VPass *passP, OptCarryState *stateP)
{
int r;
int i;

    optCarryStateUnknown(stateP);

    for( r = VR_AC; r <= VR_IO; ++r )
    {
        stateP->reg[r] = (passP->reg[r].val >= VAL_LOST)?OPTVAL_UNKNOWN:passP->reg[r].val;
    }

    stateRoom(stateP, passP->factCount);

    for( i = 0; i < passP->factCount; ++i )
    {
        if( passP->factsP[i].val < VAL_LOST )
        {
            stateP->factsP[stateP->factCount].addr = passP->factsP[i].addr;
            stateP->factsP[stateP->factCount].val = passP->factsP[i].val;
            ++stateP->factCount;
        }
    }
}

// Start a walk from a carried state: its facts, its registers, and the
// block's entry names for a register it does not know.  The state may hold
// the block's entry names from an earlier walk round a loop: older instances.
static void
applyIn(VPass *passP, OptBlockP blockP, const OptCarryState *inP)
{
long entry;
int i;
int r;

    if( !inP->known )
    {
        return;
    }

    entry = (VAL_ENTRY + ((long)blockP->id * 2));

    // A carried fact has held since the block's entry.
    for( i = 0; i < inP->factCount; ++i )
    {
        setFact(passP, inP->factsP[i].addr, inP->factsP[i].val, blockP->startAddr);
    }

    passP->reg[VR_AC].val = inP->reg[VR_AC];
    passP->reg[VR_IO].val = inP->reg[VR_IO];
    retireNames(passP, entry, 2);

    for( r = VR_AC; r <= VR_IO; ++r )
    {
        if( passP->reg[r].val == OPTVAL_UNKNOWN )
        {
            passP->reg[r].val = (entry + r);
        }
    }
}

// Does any register or fact hold a value?
// Returns 1 when one does, 0 otherwise.
static int
holdsAnywhere(VPass *passP, long val)
{
int i;

    if( (passP->reg[VR_AC].val == val) || (passP->reg[VR_IO].val == val) )
    {
        return(1);
    }

    for( i = 0; i < passP->factCount; ++i )
    {
        if( passP->factsP[i].val == val )
        {
            return(1);
        }
    }

    return(0);
}

// Does a memory fact hold a value?
// Returns 1 when one does, 0 otherwise.
static int
factHolds(VPass *passP, long val)
{
int i;

    for( i = 0; i < passP->factCount; ++i )
    {
        if( passP->factsP[i].val == val )
        {
            return(1);
        }
    }

    return(0);
}

// The value the walk gives a word, OPTVAL_UNKNOWN for none.
// Returns the value.
static long
factOf(VPass *passP, int addr)
{
int i;

    return( ((i = findFact(passP, addr)) >= 0)?passP->factsP[i].val:OPTVAL_UNKNOWN );
}

// A summary run's read check for one word that is not a call: is IO's entry
// value (SYM_IO, wherever it has been moved) read other than to move it?
// lac, lio, dac and dio direct, and lai, lia and swp alone, move it; a word
// that may do anything reads it.
// Returns 1 when the word reads it, 0 otherwise.
static int
readsEntryIo(VPass *passP, OptWordP wordP)
{
const VRow *rowP;
OptDecodeP decodeP;
unsigned int bits;
int acSym;
int ioSym;

    decodeP = &wordP->decode;
    acSym = (passP->reg[VR_AC].val == SYM_IO);
    ioSym = (passP->reg[VR_IO].val == SYM_IO);

    if( (wordP->flags & OPTF_WRITTEN) && !(wordP->flags & OPTF_PATCHED) )
    {
        return( holdsAnywhere(passP, SYM_IO) );
    }

    if( wordP->flags & OPTF_PATCHED )
    {
        // A patched jmp is a jmp to somewhere; anything else may be anything.
        if( (decodeP->group == OPTG_MEMREF) && (decodeP->opcode == 060) )
        {
            return(0);
        }

        return( holdsAnywhere(passP, SYM_IO) );
    }

    rowP = findRow(wordP);

    if( rowP->effects & VE_KILL )
    {
        if( (decodeP->group == OPTG_IOT) && passP->iotFree )
        {
            return(0);
        }

        return( holdsAnywhere(passP, SYM_IO) );
    }

    switch( rowP->sem )
    {
    case VS_LOADAC:
    case VS_LOADIO:
        if( !decodeP->memIndirect )
        {
            return(0);
        }

        // Through a pointer: the word read is not known.
        return( factHolds(passP, SYM_IO) );

    case VS_STOREAC:
        return( decodeP->memIndirect && acSym );

    case VS_STOREIO:
        return( decodeP->memIndirect && ioSym );

    case VS_OPERATE:
        bits = (decodeP->microBits & OPTM_ALLBITS);

        if( (bits == OPTM_LAI) || (bits == OPTM_LIA) || (bits == (OPTM_LAI | OPTM_LIA)) || (bits == OPTM_CLA)
            || (bits == OPTM_CLI) || (bits == (OPTM_CLA | OPTM_CMA)) || (bits == (OPTM_CLI | OPTM_CMI)) || !bits )
        {
            return(0);
        }

        return( ((bits & (OPTM_LAT | OPTM_LAP | OPTM_CMA | OPTM_LIA | OPTM_HLT)) && acSym)
                || ((bits & (OPTM_CMI | OPTM_LAI | OPTM_HLT)) && ioSym) );

    case VS_SPECIAL:
        bits = (decodeP->specialBits & OPTX_ALLBITS);
        return( ((bits & (OPTX_IIF | OPTX_IFI)) && !(bits & OPTX_SCI) && ioSym) || ((bits & OPTX_IDA) && acSym) );

    default:
        break;
    }

    if( decodeP->group == OPTG_SKIP )
    {
        return( ((decodeP->skipConds & (OPTC_SZA | OPTC_SPA | OPTC_SMA)) && acSym)
                || ((decodeP->skipConds & (OPTC_SPI | OPTC_SNI)) && ioSym) );
    }

    if( ((rowP->effects & VE_RAC) && acSym) || ((rowP->effects & VE_RIO) && ioSym) )
    {
        return(1);
    }

    if( rowP->effects & VE_RMEM )
    {
        if( decodeP->memIndirect )
        {
            return( factHolds(passP, SYM_IO) );
        }

        return( factOf(passP, decodeP->address) == SYM_IO );
    }

    return(0);
}

// A summary run's read check at a call word: IO's entry value passes to the
// callee in IO, in a word the callee reads, or (jda) in the word below its
// entry; a callee not known may read it anywhere.
// Returns 1 when the callee may read it, 0 otherwise.
static int
callReadsEntryIo(VCarry *cP, OptBlockP blockP, OptWordP wordP)
{
VPass *passP;
OptFlowEdgeP edgeP;
OptRoutineP calleeP;
OptPreserveP sumP;
const OptWordSet *readsP;
int freeSums;
int i;

    passP = cP->passP;

    if( !holdsAnywhere(passP, SYM_IO) )
    {
        return(0);
    }

    freeSums = ((cP->mode & OPTCM_FREESUMS) != 0);

    for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->kind != OPTFK_CALL )
        {
            continue;
        }

        calleeP = (edgeP->toP)?cP->mapsP->routineOfEntryPP[edgeP->toP->id]:NILP;

        if( !calleeP || !cP->sumsP[calleeP->id].done || edgeP->crossbank )
        {
            return(1);
        }

        sumP = &cP->sumsP[calleeP->id];
        readsP = (freeSums)?&sumP->freeReads:&sumP->reads;

        if( (passP->reg[VR_IO].val == SYM_IO) && ((freeSums)?sumP->freeIoRead:sumP->ioRead) )
        {
            return(1);
        }

        for( i = 0; i < passP->factCount; ++i )
        {
            if( (passP->factsP[i].val == SYM_IO) && optWordSetHas(readsP, passP->bank, passP->factsP[i].addr) )
            {
                return(1);
            }
        }

        if( (wordP->decode.opcode == 016) && (passP->reg[VR_AC].val == SYM_IO)
            && optWordSetHas(readsP, passP->bank, ((wordP->decode.indirect)?wordP->decode.address:0100)) )
        {
            return(1);
        }
    }

    return(0);
}

// A summary run's last walk, before each word: a return word checks the
// symbols, any other word whether it reads IO's entry value.
static void
bodyHooks(VCarry *cP, OptBlockP blockP, OptWordP wordP)
{
VPass *passP;
OptBodyRun *runP;

    passP = cP->passP;
    runP = cP->runP;

    if( optPreserveIsReturn(cP->routineP, wordP) )
    {
        ++runP->returns;

        if( passP->reg[VR_IO].val != SYM_IO )
        {
            runP->ioKept = 0;
        }

        if( passP->reg[VR_AC].val != SYM_AC )
        {
            runP->acKept = 0;
        }

        return;
    }

    if( (wordP == blockP->lastP) && (blockP->endKind == OPTBE_CALL) )
    {
        if( callReadsEntryIo(cP, blockP, wordP) )
        {
            runP->ioRead = 1;
        }

        return;
    }

    if( readsEntryIo(passP, wordP) )
    {
        runP->ioRead = 1;
    }
}

// Drop a walk's hits without counting them: a round of the solver.
static void
discardBlock(VPass *passP)
{
VHitP hitP;
int i;

    for( i = 0; i < passP->blockP->wordCount; ++i )
    {
        if( (hitP = passP->blockHitsPP[i]) )
        {
            free(hitP->replaceP);
            free(hitP->whyP);
            free(hitP);
            passP->blockHitsPP[i] = NILP;
        }
    }
}

// Walk one block from a carried state.  last is the final walk: a leg's hits
// are kept, a summary run's hooks run.
static void
carryWalk(VCarry *cP, OptBlockP blockP, const OptCarryState *inP, int last, OptCarryState *outP,
          OptCarryState *preP)
{
VPass *passP;
OptWordP wordP;
int addr;

    passP = cP->passP;
    beginBlock(passP, blockP);
    applyIn(passP, blockP, inP);

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        if( !(wordP = wordAt(cP->tableP, blockP->bank, addr)) || (wordP->blockP != blockP) )
        {
            continue;
        }

        if( (wordP == blockP->lastP) && (blockP->endKind == OPTBE_CALL) )
        {
            captureState(passP, preP);
        }

        if( last && cP->runP )
        {
            bodyHooks(cP, blockP, wordP);
        }

        doWord(passP, wordP);
    }

    if( last && (cP->leg >= 0) )
    {
        endBlock(passP);
    }
    else
    {
        discardBlock(passP);
    }

    captureState(passP, outP);
}

// What a call returns to the word after it: the meet over the call word's
// callees of optCarryCall()'s answer.
static void
callReturn(VCarry *cP, OptBlockP fromP, OptCarryState *stateP)
{
OptFlowEdgeP edgeP;
OptRoutineP calleeP;
OptCarryState one;
int any;

    memset(&one, 0, sizeof(one));
    stateP->known = 0;
    stateP->factCount = 0;
    any = 0;

    for( edgeP = fromP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->kind != OPTFK_CALL )
        {
            continue;
        }

        calleeP = (edgeP->toP)?cP->mapsP->routineOfEntryPP[edgeP->toP->id]:NILP;
        optCarryCall(cP->tableP, cP->sumsP, ((cP->leg > 0)?cP->leg:0), cP->mode, cP->routineP, fromP->lastP,
            calleeP, &cP->preP[fromP->id], &one);

        // A jda into another bank deposits AC somewhere these facts do not name.
        if( edgeP->crossbank )
        {
            one.factCount = 0;
        }

        stateMeet(stateP, &one);
        any = 1;
    }

    if( !any )
    {
        optCarryStateUnknown(stateP);
    }

    optCarryStateFree(&one);
}

// Does a summary run take a block's call as harmless: every call, or (for the
// call test) one whose every callee is in the caller's own component?
// Returns 1 when it does, 0 otherwise.
static int
harmlessCall(VCarry *cP, OptBlockP fromP)
{
OptFlowEdgeP edgeP;
OptRoutineP calleeP;
int any;

    if( cP->leg >= 0 )
    {
        return(0);
    }

    if( cP->mode & OPTCM_CALLSOPT )
    {
        return(1);
    }

    if( !(cP->mode & OPTCM_SCCOPT) || !cP->routineP->scc )
    {
        return(0);
    }

    any = 0;

    for( edgeP = fromP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->kind != OPTFK_CALL )
        {
            continue;
        }

        calleeP = (edgeP->toP)?cP->mapsP->routineOfEntryPP[edgeP->toP->id]:NILP;

        if( !calleeP || (calleeP->scc != cP->routineP->scc) )
        {
            return(0);
        }

        any = 1;
    }

    return(any);
}

// What one edge brings into its block.
static void
edgeState(VCarry *cP, VPred *predP, OptCarryState *stateP)
{
OptFlowEdgeP edgeP;
int id;

    edgeP = predP->edgeP;
    id = predP->fromP->id;

    switch( edgeP->kind )
    {
    case OPTFK_FALL:
    case OPTFK_SKIP:
    case OPTFK_JUMP:
    case OPTFK_CALL:
        optCarryStateCopy(stateP, &cP->outP[id]);

        // Facts name words of the bank they were made in.
        if( edgeP->crossbank )
        {
            stateP->factCount = 0;
        }

        return;

    case OPTFK_RETURN:
        // Leg 1 knows nothing after a call, and an assumed return may land
        // elsewhere, but for a summary run's tests that take the call as
        // harmless (a call within the component is always assumed: the
        // recursion clobbers its return word).
        if( (cP->leg == 1) || (edgeP->assumed && !harmlessCall(cP, predP->fromP)) )
        {
            optCarryStateUnknown(stateP);
            return;
        }

        if( !cP->preP[id].known )
        {
            stateP->known = 0;
            stateP->factCount = 0;
            return;
        }

        callReturn(cP, predP->fromP, stateP);
        return;

    default:
        // A resumed hlt: the operator may have changed anything.
        optCarryStateUnknown(stateP);
        return;
    }
}

// What a block knows at its entry, from what is known now.
static void
computeIn(VCarry *cP, OptBlockP blockP, OptCarryState *inP)
{
OptCarryState edge;
VPred *predP;

    if( cP->fixedP[blockP->id] )
    {
        optCarryStateUnknown(inP);
        return;
    }

    inP->known = 0;
    inP->factCount = 0;

    if( cP->routineP && (blockP == cP->routineP->entryBlockP) )
    {
        optCarryStateCopy(inP, &cP->seed);
    }

    memset(&edge, 0, sizeof(edge));

    for( predP = cP->predsPP[blockP->id]; predP; predP = predP->nextP )
    {
        edgeState(cP, predP, &edge);
        stateMeet(inP, &edge);
    }

    optCarryStateFree(&edge);

    if( cP->outsideP[blockP->id] )
    {
        if( !inP->known )
        {
            optCarryStateUnknown(inP);
        }
        else
        {
            inP->reg[VR_AC] = OPTVAL_UNKNOWN;
            inP->reg[VR_IO] = OPTVAL_UNKNOWN;
            inP->factCount = 0;
        }
    }
}

// Set a run up: every array by block id, nothing in scope yet.
static void
carryInit(VCarry *cP, VPass *passP, OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumsP, int leg,
          unsigned int mode, OptRoutineP routineP, OptBodyRun *runP)
{
size_t n;

    memset(cP, 0, sizeof(*cP));
    cP->passP = passP;
    cP->tableP = tableP;
    cP->mapsP = mapsP;
    cP->sumsP = sumsP;
    cP->leg = leg;
    cP->mode = mode;
    cP->routineP = routineP;
    cP->runP = runP;
    cP->count = tableP->blockCount;
    n = (size_t)(cP->count + 1);
    cP->scopeP = (unsigned char *)vAlloc(n, 1);
    cP->fixedP = (unsigned char *)vAlloc(n, 1);
    cP->outsideP = (unsigned char *)vAlloc(n, 1);
    cP->predsPP = (VPred **)vAlloc(n, sizeof(VPred *));
    cP->outP = (OptCarryState *)vAlloc(n, sizeof(OptCarryState));
    cP->preP = (OptCarryState *)vAlloc(n, sizeof(OptCarryState));
    cP->widenP = (OptCarryState *)vAlloc(n, sizeof(OptCarryState));
}

// Release a run.
static void
carryFree(VCarry *cP)
{
int id;

    for( id = 0; id <= cP->count; ++id )
    {
        optCarryStateFree(&cP->outP[id]);
        optCarryStateFree(&cP->preP[id]);
        optCarryStateFree(&cP->widenP[id]);
    }

    optCarryStateFree(&cP->seed);
    free(cP->scopeP);
    free(cP->fixedP);
    free(cP->outsideP);
    free(cP->predsPP);
    free(cP->predPoolP);
    free(cP->outP);
    free(cP->preP);
    free(cP->widenP);
}

// List the edges into each block in scope from blocks in scope.  A summary
// run ignores call edges (the callee's entry is not in the body) and marks a
// block entered from outside the body, but for the entry, whose callers the
// seed stands for.
static void
carryPreds(VCarry *cP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
VPred *predP;
int *insideP;
int id;
int n;
int pass;

    insideP = (int *)vAlloc((size_t)(cP->count + 1), sizeof(int));
    predP = NILP;

    for( pass = 0; pass < 2; ++pass )
    {
        n = 0;

        for( id = 1; id <= cP->count; ++id )
        {
            if( !cP->scopeP[id] || !(blockP = cP->mapsP->blockOfIdPP[id]) )
            {
                continue;
            }

            for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
            {
                if( !edgeP->toP || !cP->scopeP[edgeP->toP->id] )
                {
                    continue;
                }

                if( pass )
                {
                    ++insideP[edgeP->toP->id];
                }

                if( (cP->leg < 0) && (edgeP->kind == OPTFK_CALL) )
                {
                    continue;
                }

                if( pass )
                {
                    predP = &cP->predPoolP[n];
                    predP->fromP = blockP;
                    predP->edgeP = edgeP;
                    predP->nextP = cP->predsPP[edgeP->toP->id];
                    cP->predsPP[edgeP->toP->id] = predP;
                }

                ++n;
            }
        }

        if( !pass )
        {
            cP->predPoolP = (VPred *)vAlloc((size_t)n, sizeof(VPred));
        }
    }

    if( cP->leg < 0 )
    {
        for( id = 1; id <= cP->count; ++id )
        {
            if( cP->scopeP[id] && (blockP = cP->mapsP->blockOfIdPP[id]) && (blockP != cP->routineP->entryBlockP)
                && (blockP->predCount > insideP[id]) )
            {
                cP->outsideP[id] = 1;
            }
        }
    }

    free(insideP);
}

// Which blocks of a leg start knowing nothing.  Leg 0 resets every block,
// as A21's pass does.  The others reset a block the program may enter other
// than by an edge: an entry, an unreached block, one whose address is taken
// or may be jumped to through a pointer, and one starting a word or three
// after a call whose return could not be decided.
static void
carryFixed(VCarry *cP)
{
OptBlockP blockP;
OptWordP callP;
OptWordP wordP;
unsigned char *afterP;
int k;

    afterP = (unsigned char *)vAlloc((size_t)(cP->count + 1), 1);

    for( blockP = cP->tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        callP = blockP->lastP;

        if( (blockP->endKind != OPTBE_CALL) || !callP || !callP->isCallSite || (callP->returnCase != OPTRC_UNKNOWN) )
        {
            continue;
        }

        for( k = 1; k <= 3; ++k )
        {
            if( (wordP = wordAt(cP->tableP, blockP->bank, ((callP->addr + k) & ADDRMASK))) && wordP->blockP
                && (wordP->blockP->firstP == wordP) )
            {
                afterP[wordP->blockP->id] = 1;
            }
        }
    }

    for( blockP = cP->tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        ++cP->walked;

        if( cP->leg == 0 )
        {
            cP->fixedP[blockP->id] = 1;
            continue;
        }

        if( blockP->isEntry )
        {
            ++cP->fixedEntry;
        }
        else if( !blockP->reached )
        {
            ++cP->fixedUnreached;
        }
        else if( blockP->firstP->flags & (OPTF_TAKEN | OPTF_MAYBE_ENTERED) )
        {
            ++cP->fixedTaken;
        }
        else if( afterP[blockP->id] )
        {
            ++cP->fixedAfterCall;
        }
        else
        {
            ++cP->carried;
            continue;
        }

        cP->fixedP[blockP->id] = 1;
    }

    free(afterP);
}

// What a block starts from in a round: what its predecessors give now, or,
// once the run has widened, the meet of that with everything given since.
// Returns the state to walk from.
static const OptCarryState *
startState(VCarry *cP, OptBlockP blockP, OptCarryState *inP)
{
    computeIn(cP, blockP, inP);

    if( !cP->widened )
    {
        return(inP);
    }

    stateMeet(&cP->widenP[blockP->id], inP);
    return(&cP->widenP[blockP->id]);
}

// Solve a run: walk every block in scope, in id order, from what its
// predecessors last gave, until no block's end changes; then walk them once
// more for the answer.  A block no predecessor has reached yet is not walked:
// it gives nothing, as the meet expects, where a walk would hand on its own
// names as if they were known.  Past VWIDEN rounds the run widens.
static void
carrySolve(VCarry *cP)
{
OptBlockP blockP;
OptCarryState in;
OptCarryState out;
OptCarryState pre;
const OptCarryState *fromP;
int changed;
int id;
int i;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    memset(&pre, 0, sizeof(pre));
    changed = 1;

    for( cP->rounds = 0; changed && (cP->rounds < VMAXROUNDS); ++cP->rounds )
    {
        changed = 0;

        if( cP->rounds == VWIDEN )
        {
            cP->widened = 1;
        }

        for( id = 1; id <= cP->count; ++id )
        {
            if( !cP->scopeP[id] || !(blockP = cP->mapsP->blockOfIdPP[id]) )
            {
                continue;
            }

            if( !(fromP = startState(cP, blockP, &in))->known )
            {
                continue;
            }

            carryWalk(cP, blockP, fromP, 0, &out, &pre);

            if( !stateEqual(&out, &cP->outP[id]) )
            {
                optCarryStateCopy(&cP->outP[id], &out);
                changed = 1;
            }

            if( (blockP->endKind == OPTBE_CALL) && !stateEqual(&pre, &cP->preP[id]) )
            {
                optCarryStateCopy(&cP->preP[id], &pre);
                changed = 1;
            }
        }
    }

    cP->settled = !changed;

    // The answer.  A leg counts only what this walk finds.
    for( i = 0; i < VK_COUNT; ++i )
    {
        cP->passP->kills[i] = 0;
    }

    cP->passP->swpDead = 0;

    for( id = 1; id <= cP->count; ++id )
    {
        if( !cP->scopeP[id] || !(blockP = cP->mapsP->blockOfIdPP[id]) )
        {
            continue;
        }

        if( cP->leg >= 0 )
        {
            cP->passP->bankUsed[blockP->bank] = 1;
            countEntry(cP->passP, blockP);
        }

        if( cP->widened )
        {
            fromP = &cP->widenP[id];
        }
        else
        {
            computeIn(cP, blockP, &in);
            fromP = &in;
        }

        carryWalk(cP, blockP, fromP, 1, &out, &pre);
    }

    optCarryStateFree(&in);
    optCarryStateFree(&out);
    optCarryStateFree(&pre);
}

// Set a carried pass up: the handler set is the dump's, when there is one.
static void
initCarryPass(VPass *passP, OptTableP tableP, int iotFree)
{
int bank;

    memset(passP, 0, sizeof(*passP));
    passP->tableP = tableP;
    passP->carry = 1;
    passP->curIndex = -1;
    passP->iotFree = iotFree;
    passP->nextFresh = VAL_FRESH;
    passP->swpAddr = -1;
    passP->hitOfWordPP = (VHitP *)vAlloc((size_t)tableP->count, sizeof(VHitP));

    if( !carryHomeP )
    {
        findHandlerSet(passP);
        return;
    }

    passP->borrowed = 1;
    passP->handlerBlockP = carryHomeP->handlerBlockP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        passP->handlerWrittenP[bank] = carryHomeP->handlerWrittenP[bank];
    }

    passP->handlerWords = carryHomeP->handlerWords;
    passP->handlerIndirect = carryHomeP->handlerIndirect;
}

// Run the value pass over one routine's body, IO (and AC deposited by jda)
// holding a symbol at the entry, and say what every return word sees.  A run
// that does not settle proves nothing and counts as reading IO.
void
optCarryBody(OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumsP, OptRoutineP routineP,
             unsigned int mode, OptBodyRun *runP)
{
VPass pass;
VCarry carry;
int i;

    runP->returns = 0;
    runP->ioKept = 1;
    runP->acKept = 1;
    runP->ioRead = 0;

    if( !routineP->entryBlockP )
    {
        runP->ioKept = 0;
        runP->acKept = 0;
        runP->ioRead = 1;
        return;
    }

    initCarryPass(&pass, tableP, ((mode & OPTCM_IOTFREE) != 0));
    carryInit(&carry, &pass, tableP, mapsP, sumsP, -1, mode, routineP, runP);

    for( i = 0; i < routineP->blockCount; ++i )
    {
        if( (routineP->bodyIdsP[i] > 0) && (routineP->bodyIdsP[i] <= carry.count) )
        {
            carry.scopeP[routineP->bodyIdsP[i]] = 1;
        }
    }

    carryPreds(&carry);
    optCarryStateUnknown(&carry.seed);
    carry.seed.reg[VR_IO] = SYM_IO;

    if( routineP->form == OPTRF_JDA )
    {
        optCarryStateSetFact(&carry.seed, ((routineP->entryAddr - 1) & ADDRMASK), SYM_AC);
    }

    carrySolve(&carry);

    if( !carry.settled )
    {
        runP->ioKept = 0;
        runP->acKept = 0;
        runP->ioRead = 1;
    }

    carryFree(&carry);
    freePass(&pass);
}

// The legs and their variants, in dump order.  base is the leg a leg's hit
// lines are against.
static const struct
{
    const char *nameP;
    int leg;
    unsigned int mode;
    int iotFree;
    int base;
} carryLegs[] =
{
    { "0",  0, 0, 0, -1 },
    { "1",  1, 0, 0, 0 },
    { "2",  2, 0, 0, 1 },
    { "3",  3, OPTCM_ORACLE, 0, 2 },
    { "2i", 2, (OPTCM_IOTFREE | OPTCM_FREESUMS), 1, 2 },
    { "3i", 3, (OPTCM_IOTFREE | OPTCM_ORACLE), 1, 3 },
    { "2w", 2, OPTCM_NOWRITES, 0, 2 },
    { "3w", 3, (OPTCM_ORACLE | OPTCM_NOWRITES), 0, 3 }
};

#define VLEG_MAIN   4       // legs 0 to 3; the rest are variants
#define VLEG_COUNT  ((int)(sizeof(carryLegs) / sizeof(carryLegs[0])))

// Print a leg's hits that its base lacks or has in another class, and the
// base's hits it lacks, in emission order.
static void
writeLegDiff(FILE *fP, OptTableP tableP, int l, VPass *passesP)
{
VHitP hitP;
VHitP baseP;
OptWordP wordP;
const char *kindP;
int base;
int i;
int k;
int at;
char spell[OPTMSG_SIZE];
char flagText[OPTMSG_SIZE];

    base = carryLegs[l].base;
    kindP = (l < VLEG_MAIN)?"":"v";

    for( i = 0; i < tableP->count; ++i )
    {
        hitP = passesP[l].hitOfWordPP[i];
        baseP = passesP[base].hitOfWordPP[i];

        if( hitP && (!baseP || (baseP->cls != hitP->cls)) )
        {
            wordP = hitP->wordP;
            spellWord(wordP, spell, sizeof(spell));
            at = 0;
            flagText[0] = '\0';

            for( k = 0; k < VF_COUNT; ++k )
            {
                if( hitP->flags & (1u << k) )
                {
                    at += snprintf((flagText + at), (sizeof(flagText) - (size_t)at), "%s%s", (at)?",":"", flagNames[k]);
                }
            }

            fprintf(fP, "%shit %s %s %04o | b%d %s => %s | was %s | %s | %s\n", kindP, carryLegs[l].nameP,
                classTable[hitP->cls].nameP, wordP->addr, wordP->bank, spell, hitP->replaceP,
                (baseP)?classTable[baseP->cls].nameP:"-", (at)?flagText:"-", hitP->whyP);
        }
        else if( !hitP && baseP )
        {
            wordP = baseP->wordP;
            spellWord(wordP, spell, sizeof(spell));
            fprintf(fP, "%slost %s %s %04o | b%d %s\n", kindP, carryLegs[l].nameP, classTable[baseP->cls].nameP,
                wordP->addr, wordP->bank, spell);
        }
    }
}

// The hits, words and microseconds of a leg, over every bank.
static void
legTotals(VPass *passP, int *hitsP, int *wordsP, int *usP)
{
int bank;
int i;

    *hitsP = 0;
    *wordsP = 0;
    *usP = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( i = 0; i < VC_COUNT; ++i )
        {
            *hitsP += passP->classHits[bank][i];
            *wordsP += passP->classWords[bank][i];
            *usP += passP->classUs[bank][i];
        }
    }
}

// The parking and dead-store counts (sections 3.3 and 3.4 of task A52).
//
// The candidates are the words optscratch.c classes local or span: private
// to their unit, live neither into it nor across a call, so every read sees a
// store the unit made.  Its call(2) words, live across a call, are counted
// apart, since only they can park across one.
//
// IO parking keeps a word t in IO: every write of t a direct dac t, made lia,
// and every read a direct lac t, made lai.  A leg allows it when IO is dead
// just after every store, and at every reload no word has written IO since
// the last store on every path into it: a must-solve over the leg's edges, in
// which a block the leg resets knows nothing and a call's return keeps IO
// when the leg's summary (leg 2) or oracle (leg 3) does and the callee does not
// write t.  The twin, dio t ... lio t kept in AC, is the same with the
// registers exchanged; no call keeps it, since every call word writes AC.  A
// program with a sequence-break handler has its candidates counted apart: no
// handler is proved to restore IO.  Each candidate is judged alone: two whose
// parked values are live at one word cannot both park and are counted as
// overlapping, and a swp pair is that overlap in the shape "dac u; lac t".
//
// IO's liveness is over the whole program.  A call reads IO in legs 0 and 1,
// and in legs 2 and 3 when the callee's summary says it reads IO's entry
// value; a return word is live as its callers' return points are; every other
// exit to the sink, an undecided return and a word of unknown effect is live.
//
// A dead store: a word whose every read is a hit of the leg that deletes it
// (rload, swap, dload) needs none of its stores, and so does one whose reads
// are each deleted or rewritten not to read memory (lai, lia, law, lawk).

// What a candidate parks.
#define VP_IO       1           // dac t ... lac t, kept in IO
#define VP_AC       2           // dio t ... lio t, kept in AC

// The parking flags.
#define VPF_CALL    0x01        // a reload is reached only across a call's return
#define VPF_OVERLAP 0x02        // another candidate's parked value is live at one of its words
#define VPF_HANDLER 0x04        // the program has a sequence-break handler

// What a dead store's reads become.
#define VD_DELETED      1       // every one is deleted
#define VD_REWRITTEN    2       // every one is deleted or no longer reads memory

// The microseconds one rewritten word saves: dac and lac take 10, lia and lai 5.
#define VPARK_US    5

// One link of a list.
typedef struct vlink
{
    struct vlink *nextP;
    void *itemP;
} VLink;

// One word the counts consider.
typedef struct
{
    OptWordP wordP;
    int cls;                    // OPTSP_LOCAL, OPTSP_SPAN or OPTSP_CALL
    int usable;                 // every reference is a plain read or write
    int kind;                   // VP_IO or VP_AC when its references have that shape, else 0
    OptWordP *readsPP;          // the words that read it
    int readCount;
    OptWordP *writesPP;         // and that write it
    int writeCount;
} VCand;

// What one leg found for one candidate.
typedef struct
{
    int park;                   // VP_IO, VP_AC or 0
    unsigned int flags;         // VPF_ bits, when it parks
    int dead;                   // VD_ code, or 0
} VCandLeg;

// The counts' state, over every leg.
typedef struct
{
    OptTableP tableP;
    OptCarryMapsP mapsP;
    OptPreserveP sumsP;
    VCand *candsP;
    int candCount;
    int *candOfIndexP;          // by emission index: the candidate the word reads or
                                // writes, -1 for none
    unsigned int *effP;         // by emission index: regEffects()
    VLink **retOfPP;            // by block id: the routines whose return word ends it
    VLink **sitesOfPP;          // by routine id: the blocks that call it
    int handler;                // the program has a sequence-break handler
    int spelled1D;              // the program spells a PDP-1D mnemonic
    VCandLeg *legsP[VLEG_MAIN]; // by candidate, per main leg
    int lost[VLEG_MAIN];        // candidates a leg lost from its base

    // the leg being counted
    int leg;
    VCarry *cP;
    unsigned char *liveP[2];    // by block id: AC, IO live at the block's entry
    unsigned char *retP[2];     // by routine id: live at its callers' return points
    unsigned char *inRP;        // by block id: a store reaches it
    unsigned char *pOutP;       // by block id: the parked value holds at its end
    unsigned char *tLiveP;      // by block id: t is live at its entry
    int *listP;                 // the blocks inRP marks
    int listCount;
    int *ownerP[2];             // by emission index: the candidate parked in AC, IO there
} VPark;

// Allocate one link and push it on a list.
static void
pushLink(VLink **headPP, void *itemP)
{
VLink *linkP;

    linkP = (VLink *)vAlloc(1, sizeof(VLink));
    linkP->itemP = itemP;
    linkP->nextP = *headPP;
    *headPP = linkP;
}

// Free a list.
static void
freeLinks(VLink *linkP)
{
VLink *nextP;

    for( ; linkP; linkP = nextP )
    {
        nextP = linkP->nextP;
        free(linkP);
    }
}

// What a word does to the registers, as doWord() walks it: VE_RAC, VE_RIO,
// VE_WAC and VE_WIO, with VE_KILL for a word whose effect is unknown, which
// reads and writes both.  A patched jmp is a jmp to somewhere, as
// readsEntryIo() takes it.
// Returns the bits.
static unsigned int
regEffects(OptWordP wordP)
{
const VRow *rowP;
OptDecodeP decodeP;
unsigned int bits;
unsigned int eff;
int acSrc;
int ioSrc;
int newAcSrc;
int newIoSrc;
int writesAc;
int writesIo;

    decodeP = &wordP->decode;

    if( (wordP->flags & OPTF_PATCHED) && (decodeP->group == OPTG_MEMREF) && (decodeP->opcode == 060) )
    {
        return(0);
    }

    if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED) )
    {
        return(VE_KILL | VE_RAC | VE_RIO | VE_WAC | VE_WIO);
    }

    rowP = findRow(wordP);

    if( rowP->effects & VE_KILL )
    {
        return(VE_KILL | VE_RAC | VE_RIO | VE_WAC | VE_WIO);
    }

    switch( rowP->sem )
    {
    case VS_OPERATE:
        // doOperate()'s phase order; a source is 1 for AC, 2 for IO, 0 for neither.
        bits = (decodeP->microBits & OPTM_ALLBITS);
        acSrc = 1;
        ioSrc = 2;
        writesAc = 0;
        writesIo = 0;

        if( bits & OPTM_CLA )
        {
            acSrc = 0;
            writesAc = 1;
        }

        if( bits & OPTM_CLI )
        {
            ioSrc = 0;
            writesIo = 1;
        }

        if( bits & (OPTM_CMI) )
        {
            writesIo = 1;
        }

        if( bits & (OPTM_LAT | OPTM_LAP | OPTM_CMA) )
        {
            writesAc = 1;
        }

        newAcSrc = acSrc;
        newIoSrc = ioSrc;

        if( bits & OPTM_LAI )
        {
            newAcSrc = ioSrc;
            writesAc = 1;
        }

        if( bits & OPTM_LIA )
        {
            newIoSrc = acSrc;
            writesIo = 1;
        }

        eff = 0;

        if( writesAc )
        {
            eff |= VE_WAC;
        }

        if( writesIo )
        {
            eff |= VE_WIO;
        }

        if( (writesAc && (newAcSrc & 1)) || (writesIo && (newIoSrc & 1)) )
        {
            eff |= VE_RAC;
        }

        if( (writesAc && (newAcSrc & 2)) || (writesIo && (newIoSrc & 2)) )
        {
            eff |= VE_RIO;
        }

        if( bits & OPTM_HLT )
        {
            eff |= (VE_RAC | VE_RIO);
        }

        return(eff);

    case VS_SPECIAL:
        bits = (decodeP->specialBits & OPTX_ALLBITS);
        eff = 0;

        if( (bits & (OPTX_IIF | OPTX_IFI)) && !(bits & OPTX_SCI) )
        {
            eff |= VE_RIO;
        }

        if( bits & (OPTX_IIF | OPTX_SCI) )
        {
            eff |= VE_WIO;
        }

        if( bits & OPTX_IDA )
        {
            eff |= (VE_RAC | VE_WAC);
        }

        return(eff);

    case VS_LAW:
    case VS_LOADAC:
        return(VE_WAC);

    case VS_LOADIO:
        return(VE_WIO);

    default:
        break;
    }

    if( decodeP->group == OPTG_SKIP )
    {
        eff = 0;

        if( decodeP->skipConds & (OPTC_SZA | OPTC_SPA | OPTC_SMA) )
        {
            eff |= VE_RAC;
        }

        if( decodeP->skipConds & (OPTC_SPI | OPTC_SNI) )
        {
            eff |= VE_RIO;
        }

        return(eff);
    }

    return( rowP->effects & (VE_RAC | VE_RIO | VE_WAC | VE_WIO) );
}

// The read and write bits for a register.
#define VREADS(reg)     (((reg) == VR_IO)?VE_RIO:VE_RAC)
#define VWRITES(reg)    (((reg) == VR_IO)?VE_WIO:VE_WAC)

// Is a word a direct memory reference with one operation code?
// Returns 1 when it is, 0 otherwise.
static int
directOp(OptWordP wordP, int opcode)
{
    return( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == opcode) && !wordP->decode.memIndirect );
}

// Is a word one the parking rewrite could change: in the graph, its run-time
// word the assembled one, and not executed out of line?
// Returns 1 when it is, 0 otherwise.
static int
plainWord(OptWordP wordP)
{
    return( (wordP->blockP != NILP) && !(wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_XCTTARGET)) );
}

// Add a word to a list of words, once.
static void
addCandRef(OptWordP **listPPP, int *countP, OptWordP wordP)
{
int i;

    for( i = 0; i < *countP; ++i )
    {
        if( (*listPPP)[i] == wordP )
        {
            return;
        }
    }

    if( !(*listPPP = (OptWordP *)realloc(*listPPP, ((size_t)(*countP + 1) * sizeof(OptWordP)))) )
    {
        fprintf(stderr, "am1: out of memory measuring values\n");
        exit(1);
    }

    (*listPPP)[(*countP)++] = wordP;
}

// The shape a candidate's references have: VP_IO when every write is a
// direct dac and every read a direct lac, VP_AC for dio and lio, else 0.
// Returns the shape.
static int
candKind(VCand *candP)
{
static const int stores[2] = { 024, 032 };
static const int loads[2] = { 020, 022 };
static const int kinds[2] = { VP_IO, VP_AC };
int k;
int i;
int ok;

    if( !candP->usable || !candP->readCount || !candP->writeCount )
    {
        return(0);
    }

    for( k = 0; k < 2; ++k )
    {
        ok = 1;

        for( i = 0; ok && (i < candP->writeCount); ++i )
        {
            ok = (directOp(candP->writesPP[i], stores[k]) && plainWord(candP->writesPP[i]));
        }

        for( i = 0; ok && (i < candP->readCount); ++i )
        {
            ok = (directOp(candP->readsPP[i], loads[k]) && plainWord(candP->readsPP[i]));
        }

        if( ok )
        {
            return(kinds[k]);
        }
    }

    return(0);
}

// Set the counts up: the candidates and their references, the word effects,
// the return words and call sites, and the program's handler and 1D facts.
static void
parkInit(VPark *pkP, OptTableP tableP, OptCarryMapsP mapsP, OptPreserveP sumsP, VPass *homeP)
{
unsigned char *codesP;
OptWordP wordP;
OptEdgeP edgeP;
OptBlockP blockP;
OptRoutineP routineP;
OptRoutineP calleeP;
OptFlowEdgeP flowP;
VCand *candP;
size_t n;
int i;
int k;
int l;
int id;

    memset(pkP, 0, sizeof(*pkP));
    pkP->tableP = tableP;
    pkP->mapsP = mapsP;
    pkP->sumsP = sumsP;
    codesP = optScratchPrivate(tableP);
    pkP->candOfIndexP = (int *)vAlloc(((size_t)tableP->count + 1), sizeof(int));
    pkP->effP = (unsigned int *)vAlloc(((size_t)tableP->count + 1), sizeof(unsigned int));

    for( i = 0; i < tableP->count; ++i )
    {
        pkP->candOfIndexP[i] = -1;
        wordP = tableP->entriesPP[i];
        pkP->effP[wordP->index] = regEffects(wordP);

        if( codesP[wordP->index] != OPTSP_NONE )
        {
            ++pkP->candCount;
        }

        if( inGraph(wordP) && wordP->decode.spelled1D )
        {
            pkP->spelled1D = 1;
        }
    }

    pkP->candsP = (VCand *)vAlloc((size_t)pkP->candCount, sizeof(VCand));
    k = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( codesP[wordP->index] == OPTSP_NONE )
        {
            continue;
        }

        candP = &pkP->candsP[k++];
        candP->wordP = wordP;
        candP->cls = codesP[wordP->index];
        candP->usable = 1;

        // The references, as optscratch.c's gatherRefs() reads them: an
        // indirection reads the pointer word it lands on.
        for( edgeP = wordP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( edgeP->flags & OPTEF_PLACEHOLDER )
            {
                continue;
            }

            if( edgeP->flags & OPTEF_VIAPOINTER )
            {
                candP->usable = 0;
            }
            else if( (edgeP->flags & OPTEF_INDIRECT) || (edgeP->role == OPTR_READ) )
            {
                addCandRef(&candP->readsPP, &candP->readCount, edgeP->fromP);
            }
            else if( (edgeP->role == OPTR_WRITE) && !(edgeP->flags & OPTEF_PATCH) )
            {
                addCandRef(&candP->writesPP, &candP->writeCount, edgeP->fromP);
            }
            else
            {
                candP->usable = 0;
            }
        }

        if( (candP->kind = candKind(candP)) )
        {
            for( l = 0; l < candP->readCount; ++l )
            {
                pkP->candOfIndexP[candP->readsPP[l]->index] = (k - 1);
            }

            for( l = 0; l < candP->writeCount; ++l )
            {
                pkP->candOfIndexP[candP->writesPP[l]->index] = (k - 1);
            }
        }
    }

    free(codesP);

    // The return words, and who calls each routine.
    n = (size_t)(tableP->blockCount + 1);
    pkP->retOfPP = (VLink **)vAlloc(n, sizeof(VLink *));
    pkP->sitesOfPP = (VLink **)vAlloc(((size_t)tableP->routineCount + 1), sizeof(VLink *));

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        for( i = 0; i < routineP->blockCount; ++i )
        {
            id = routineP->bodyIdsP[i];

            if( (id > 0) && (id <= tableP->blockCount) && (blockP = mapsP->blockOfIdPP[id])
                && optPreserveIsReturn(routineP, blockP->lastP) )
            {
                pushLink(&pkP->retOfPP[id], routineP);
            }
        }
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( (flowP->kind == OPTFK_CALL) && flowP->toP && (calleeP = mapsP->routineOfEntryPP[flowP->toP->id]) )
            {
                pushLink(&pkP->sitesOfPP[calleeP->id], blockP);
            }
        }

        if( homeP->handlerBlockP && homeP->handlerBlockP[blockP->id] )
        {
            pkP->handler = 1;
        }
    }

    for( l = 0; l < VLEG_MAIN; ++l )
    {
        pkP->legsP[l] = (VCandLeg *)vAlloc((size_t)pkP->candCount, sizeof(VCandLeg));
    }

    pkP->liveP[VR_AC] = (unsigned char *)vAlloc(n, 1);
    pkP->liveP[VR_IO] = (unsigned char *)vAlloc(n, 1);
    pkP->retP[VR_AC] = (unsigned char *)vAlloc(((size_t)tableP->routineCount + 1), 1);
    pkP->retP[VR_IO] = (unsigned char *)vAlloc(((size_t)tableP->routineCount + 1), 1);
    pkP->inRP = (unsigned char *)vAlloc(n, 1);
    pkP->pOutP = (unsigned char *)vAlloc(n, 1);
    pkP->tLiveP = (unsigned char *)vAlloc(n, 1);
    pkP->listP = (int *)vAlloc(n, sizeof(int));
    pkP->ownerP[VR_AC] = (int *)vAlloc(((size_t)tableP->count + 1), sizeof(int));
    pkP->ownerP[VR_IO] = (int *)vAlloc(((size_t)tableP->count + 1), sizeof(int));
}

// Release the counts.
static void
parkFree(VPark *pkP)
{
int i;

    for( i = 0; i < pkP->candCount; ++i )
    {
        free(pkP->candsP[i].readsPP);
        free(pkP->candsP[i].writesPP);
    }

    free(pkP->candsP);

    for( i = 0; i <= pkP->tableP->blockCount; ++i )
    {
        freeLinks(pkP->retOfPP[i]);
    }

    for( i = 0; i <= pkP->tableP->routineCount; ++i )
    {
        freeLinks(pkP->sitesOfPP[i]);
    }

    for( i = 0; i < VLEG_MAIN; ++i )
    {
        free(pkP->legsP[i]);
    }

    free(pkP->retOfPP);
    free(pkP->sitesOfPP);
    free(pkP->candOfIndexP);
    free(pkP->effP);
    free(pkP->liveP[VR_AC]);
    free(pkP->liveP[VR_IO]);
    free(pkP->retP[VR_AC]);
    free(pkP->retP[VR_IO]);
    free(pkP->inRP);
    free(pkP->pOutP);
    free(pkP->tLiveP);
    free(pkP->listP);
    free(pkP->ownerP[VR_AC]);
    free(pkP->ownerP[VR_IO]);
}

// Does a call edge's callee read a register's entry value, as the leg sees it?
// AC is always taken as read: jda and cal read it at the call word, and jsp
// overwrites it there, so the answer never matters.
// Returns 1 when it may, 0 when it does not.
static int
callReadsReg(VPark *pkP, OptFlowEdgeP flowP, int reg)
{
OptRoutineP calleeP;
OptPreserveP sumP;

    if( (reg == VR_AC) || (pkP->leg < 2) || !flowP->toP || flowP->crossbank
        || !(calleeP = pkP->mapsP->routineOfEntryPP[flowP->toP->id]) )
    {
        return(1);
    }

    sumP = &pkP->sumsP[calleeP->id];
    return( !sumP->done || sumP->ioRead );
}

// Is a register live at a call block's return points?
// Returns 1 when it is or the return is not decided, 0 otherwise.
static int
returnPointLive(VPark *pkP, OptBlockP blockP, int reg)
{
OptFlowEdgeP flowP;
int any;

    if( !blockP->lastP || (blockP->lastP->returnCase == OPTRC_UNKNOWN) )
    {
        return(1);
    }

    any = 0;

    for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
    {
        if( flowP->kind != OPTFK_RETURN )
        {
            continue;
        }

        any = 1;

        if( !flowP->toP || flowP->assumed || pkP->liveP[reg][flowP->toP->id] )
        {
            return(1);
        }
    }

    return(!any);
}

// Is a register live just after a block's last word?
// Returns 1 when it is, 0 otherwise.
static int
blockLiveOut(VPark *pkP, OptBlockP blockP, int reg)
{
OptFlowEdgeP flowP;
VLink *linkP;
int isReturn;
int out;

    isReturn = (pkP->retOfPP[blockP->id] != NILP);
    out = (!blockP->succP && !isReturn);

    for( linkP = pkP->retOfPP[blockP->id]; linkP; linkP = linkP->nextP )
    {
        out |= pkP->retP[reg][((OptRoutineP)linkP->itemP)->id];
    }

    for( flowP = blockP->succP; flowP && !out; flowP = flowP->nextP )
    {
        switch( flowP->kind )
        {
        case OPTFK_CALL:
            out = callReadsReg(pkP, flowP, reg);
            break;

        case OPTFK_RETURN:
            out = (!flowP->toP || flowP->assumed || pkP->liveP[reg][flowP->toP->id]);
            break;

        default:
            if( flowP->toP )
            {
                out = pkP->liveP[reg][flowP->toP->id];
            }
            else if( !isReturn )
            {
                out = 1;
            }
            break;
        }
    }

    return(out);
}

// Walk a block backward from a liveness at its end, down to (not through) a
// word, or through the whole block when stopP is NILP.
// Returns the liveness there.
static int
liveBack(VPark *pkP, OptBlockP blockP, int reg, int live, OptWordP stopP)
{
OptWordP wordP;
unsigned int eff;
int addr;

    for( addr = blockP->endAddr; addr >= blockP->startAddr; --addr )
    {
        if( !(wordP = wordAt(pkP->tableP, blockP->bank, addr)) || (wordP->blockP != blockP) )
        {
            continue;
        }

        if( wordP == stopP )
        {
            break;
        }

        eff = pkP->effP[wordP->index];
        live = ((eff & VREADS(reg)) || (live && !(eff & VWRITES(reg))));
    }

    return(live);
}

// Solve a register's liveness over the program, as the leg sees calls.
static void
solveLive(VPark *pkP, int reg)
{
OptTableP tableP;
OptRoutineP routineP;
OptBlockP blockP;
VLink *linkP;
int changed;
int id;
int live;
int any;

    tableP = pkP->tableP;
    memset(pkP->liveP[reg], 0, ((size_t)tableP->blockCount + 1));

    do
    {
        changed = 0;

        // A return word is live as its callers' return points are; a routine
        // the program may run outside a call returns anywhere.
        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            live = ((routineP->flags & (OPTRT_FALLIN | OPTRT_OPENBODY)) != 0);
            any = 0;

            for( linkP = pkP->sitesOfPP[routineP->id]; linkP && !live; linkP = linkP->nextP )
            {
                any = 1;
                live = returnPointLive(pkP, (OptBlockP)linkP->itemP, reg);
            }

            pkP->retP[reg][routineP->id] = (unsigned char)(live || !any);
        }

        for( id = tableP->blockCount; id >= 1; --id )
        {
            if( !(blockP = pkP->mapsP->blockOfIdPP[id]) )
            {
                continue;
            }

            live = liveBack(pkP, blockP, reg, blockLiveOut(pkP, blockP, reg), NILP);

            if( live != pkP->liveP[reg][id] )
            {
                pkP->liveP[reg][id] = (unsigned char)live;
                changed = 1;
            }
        }
    }
    while( changed );
}

// Does a call block's every callee keep a register and leave a word alone, as
// the leg says: the summary (leg 2) or the oracle (leg 3)?
// Returns 1 when it does, 0 otherwise.
static int
callKeeps(VPark *pkP, OptBlockP fromP, OptWordP tP)
{
OptFlowEdgeP flowP;
OptRoutineP calleeP;
OptPreserveP sumP;
int any;

    any = 0;

    for( flowP = fromP->succP; flowP; flowP = flowP->nextP )
    {
        if( flowP->kind != OPTFK_CALL )
        {
            continue;
        }

        any = 1;

        if( !flowP->toP || flowP->crossbank || !(calleeP = pkP->mapsP->routineOfEntryPP[flowP->toP->id]) )
        {
            return(0);
        }

        sumP = &pkP->sumsP[calleeP->id];

        if( pkP->leg == 3 )
        {
            if( optWordSetHas(&sumP->direct, tP->bank, tP->addr) )
            {
                return(0);
            }
        }
        else if( !sumP->done || (sumP->io != OPTPR_YES) || optWordSetHas(&sumP->writes, tP->bank, tP->addr) )
        {
            return(0);
        }
    }

    return(any);
}

// Does a word store a candidate, as its parking shape counts a store?
// Returns 1 when it does, 0 otherwise.
static int
isCandStore(VPark *pkP, int c, OptWordP wordP)
{
    return( (pkP->candOfIndexP[wordP->index] == c)
            && directOp(wordP, (pkP->candsP[c].kind == VP_IO)?024:032) );
}

// Does a word reload a candidate?
// Returns 1 when it does, 0 otherwise.
static int
isCandLoad(VPark *pkP, int c, OptWordP wordP)
{
    return( (pkP->candOfIndexP[wordP->index] == c)
            && directOp(wordP, (pkP->candsP[c].kind == VP_IO)?020:022) );
}

// Walk a block forward: the parked value holds from a store until a word
// writes its register.  Stops before stopP when it is not NILP.
// Returns whether it holds there.
static int
holdForward(VPark *pkP, int c, OptBlockP blockP, int reg, int hold, OptWordP stopP)
{
OptWordP wordP;
int addr;

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        if( !(wordP = wordAt(pkP->tableP, blockP->bank, addr)) || (wordP->blockP != blockP) )
        {
            continue;
        }

        if( wordP == stopP )
        {
            break;
        }

        if( isCandStore(pkP, c, wordP) )
        {
            hold = 1;
        }
        else if( pkP->effP[wordP->index] & VWRITES(reg) )
        {
            hold = 0;
        }
    }

    return(hold);
}

// Does the parked value hold at a block's entry: on every edge into it from
// a block a store reaches?  A block the leg resets knows nothing, and a call's
// return holds it only when calls may (acrossCalls) and callKeeps() says so.
// Returns 1 when it holds, 0 otherwise.
static int
holdIn(VPark *pkP, int c, OptBlockP blockP, int acrossCalls)
{
VPred *predP;
int any;

    if( pkP->cP->fixedP[blockP->id] )
    {
        return(0);
    }

    any = 0;

    for( predP = pkP->cP->predsPP[blockP->id]; predP; predP = predP->nextP )
    {
        any = 1;

        if( !pkP->inRP[predP->fromP->id] || !pkP->pOutP[predP->fromP->id] )
        {
            return(0);
        }

        switch( predP->edgeP->kind )
        {
        case OPTFK_FALL:
        case OPTFK_SKIP:
        case OPTFK_JUMP:
            break;

        case OPTFK_RETURN:
            if( !acrossCalls || predP->edgeP->assumed || !callKeeps(pkP, predP->fromP, pkP->candsP[c].wordP) )
            {
                return(0);
            }
            break;

        default:
            return(0);
        }
    }

    return(any);
}

// The must-solve for one candidate: the blocks its stores reach, and whether
// its value is still in the register at every reload.
// Returns 1 when it is at every one, 0 otherwise.
static int
holdSolve(VPark *pkP, int c, int reg, int acrossCalls)
{
VCand *candP;
OptBlockP blockP;
OptFlowEdgeP flowP;
OptWordP wordP;
int changed;
int hold;
int i;
int k;

    candP = &pkP->candsP[c];

    for( k = 0; k < pkP->listCount; ++k )
    {
        pkP->inRP[pkP->listP[k]] = 0;
    }

    pkP->listCount = 0;

    for( i = 0; i < candP->writeCount; ++i )
    {
        blockP = candP->writesPP[i]->blockP;

        if( !pkP->inRP[blockP->id] )
        {
            pkP->inRP[blockP->id] = 1;
            pkP->listP[pkP->listCount++] = blockP->id;
        }
    }

    for( k = 0; k < pkP->listCount; ++k )
    {
        blockP = pkP->mapsP->blockOfIdPP[pkP->listP[k]];

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( !flowP->toP || pkP->inRP[flowP->toP->id] || pkP->cP->fixedP[flowP->toP->id]
                || (flowP->kind == OPTFK_CALL) || (flowP->kind == OPTFK_RESUME)
                || ((flowP->kind == OPTFK_RETURN) && !acrossCalls) )
            {
                continue;
            }

            pkP->inRP[flowP->toP->id] = 1;
            pkP->listP[pkP->listCount++] = flowP->toP->id;
        }
    }

    for( k = 0; k < pkP->listCount; ++k )
    {
        pkP->pOutP[pkP->listP[k]] = 1;
    }

    do
    {
        changed = 0;

        for( k = 0; k < pkP->listCount; ++k )
        {
            blockP = pkP->mapsP->blockOfIdPP[pkP->listP[k]];
            hold = holdForward(pkP, c, blockP, reg, holdIn(pkP, c, blockP, acrossCalls), NILP);

            if( hold != pkP->pOutP[blockP->id] )
            {
                pkP->pOutP[blockP->id] = (unsigned char)hold;
                changed = 1;
            }
        }
    }
    while( changed );

    for( i = 0; i < candP->readCount; ++i )
    {
        wordP = candP->readsPP[i];
        blockP = wordP->blockP;

        if( !pkP->inRP[blockP->id]
            || !holdForward(pkP, c, blockP, reg, holdIn(pkP, c, blockP, acrossCalls), wordP) )
        {
            return(0);
        }
    }

    return(1);
}

// Mark the words at which a parked candidate's value is live, over the blocks
// holdSolve() last reached; a word another candidate holds makes both overlap.
static void
markParked(VPark *pkP, int c, int reg, int acrossCalls, VCandLeg *resP)
{
OptBlockP blockP;
OptFlowEdgeP flowP;
OptWordP wordP;
int *ownerP;
int changed;
int live;
int k;
int addr;
int pass;

    ownerP = pkP->ownerP[reg];

    for( k = 0; k < pkP->listCount; ++k )
    {
        pkP->tLiveP[pkP->listP[k]] = 0;
    }

    // Pass 0 solves t's liveness backward over the reached blocks; pass 1 marks.
    for( pass = 0; pass < 2; ++pass )
    {
        do
        {
            changed = 0;

            for( k = (pkP->listCount - 1); k >= 0; --k )
            {
                blockP = pkP->mapsP->blockOfIdPP[pkP->listP[k]];
                live = 0;

                for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
                {
                    if( flowP->toP && pkP->inRP[flowP->toP->id] && (flowP->kind != OPTFK_CALL)
                        && (flowP->kind != OPTFK_RESUME) && ((flowP->kind != OPTFK_RETURN) || acrossCalls) )
                    {
                        live |= pkP->tLiveP[flowP->toP->id];
                    }
                }

                for( addr = blockP->endAddr; addr >= blockP->startAddr; --addr )
                {
                    if( !(wordP = wordAt(pkP->tableP, blockP->bank, addr)) || (wordP->blockP != blockP) )
                    {
                        continue;
                    }

                    if( pass && live )
                    {
                        if( ownerP[wordP->index] < 0 )
                        {
                            ownerP[wordP->index] = c;
                        }
                        else if( ownerP[wordP->index] != c )
                        {
                            resP[c].flags |= VPF_OVERLAP;
                            resP[ownerP[wordP->index]].flags |= VPF_OVERLAP;
                        }
                    }

                    if( isCandLoad(pkP, c, wordP) )
                    {
                        live = 1;
                    }
                    else if( isCandStore(pkP, c, wordP) )
                    {
                        live = 0;
                    }
                }

                if( !pass && (live != pkP->tLiveP[blockP->id]) )
                {
                    pkP->tLiveP[blockP->id] = (unsigned char)live;
                    changed = 1;
                }
            }
        }
        while( changed && !pass );
    }
}

// Count one leg: each candidate's parking and dead stores.  passP is the
// leg's own pass, whose hits the dead stores read.
static void
parkLeg(VPark *pkP, VCarry *cP, int l, VPass *passP)
{
VCand *candP;
VCandLeg *resP;
VHitP hitP;
OptWordP wordP;
int c;
int i;
int reg;
int ok;
int deleted;
int calls;

    pkP->leg = l;
    pkP->cP = cP;
    pkP->listCount = 0;
    memset(pkP->inRP, 0, ((size_t)pkP->tableP->blockCount + 1));
    resP = pkP->legsP[l];
    solveLive(pkP, VR_IO);
    solveLive(pkP, VR_AC);

    for( i = 0; i < pkP->tableP->count; ++i )
    {
        pkP->ownerP[VR_AC][i] = -1;
        pkP->ownerP[VR_IO][i] = -1;
    }

    for( c = 0; c < pkP->candCount; ++c )
    {
        candP = &pkP->candsP[c];

        // A dead store: local and span words only, as the task counts them.
        if( candP->usable && candP->readCount && (candP->cls != OPTSP_CALL) )
        {
            ok = 1;
            deleted = 1;

            for( i = 0; ok && (i < candP->readCount); ++i )
            {
                wordP = candP->readsPP[i];

                if( !(directOp(wordP, 020) || directOp(wordP, 022)) || !(hitP = passP->hitOfWordPP[wordP->index]) )
                {
                    ok = 0;
                    break;
                }

                switch( hitP->cls )
                {
                case VC_RLOAD:
                case VC_SWAP:
                case VC_DLOAD:
                    break;

                case VC_LAI:
                case VC_LIA:
                case VC_LAW:
                case VC_LAWK:
                    deleted = 0;
                    break;

                default:
                    ok = 0;
                    break;
                }
            }

            resP[c].dead = (ok)?((deleted)?VD_DELETED:VD_REWRITTEN):0;
        }

        if( !candP->kind )
        {
            continue;
        }

        // Parking: the value holds at every reload, and the register is dead
        // after every store.
        reg = (candP->kind == VP_IO)?VR_IO:VR_AC;
        calls = 0;
        ok = holdSolve(pkP, c, reg, 0);

        if( !ok && (l >= 2) && (reg == VR_IO) && holdSolve(pkP, c, reg, 1) )
        {
            ok = 1;
            calls = 1;
        }

        for( i = 0; ok && (i < candP->writeCount); ++i )
        {
            wordP = candP->writesPP[i];
            ok = !liveBack(pkP, wordP->blockP, reg, blockLiveOut(pkP, wordP->blockP, reg), wordP);
        }

        if( !ok )
        {
            continue;
        }

        resP[c].park = candP->kind;
        resP[c].flags |= (((calls)?VPF_CALL:0) | ((pkP->handler)?VPF_HANDLER:0));

        // The last holdSolve() is the one that passed, so its reach is the one
        // markParked() walks.
        markParked(pkP, c, reg, calls, resP);
    }
}

// The name of a scratch class.
// Returns the name.
static const char *
candClassName(int cls)
{
    return( (cls == OPTSP_LOCAL)?"local":(cls == OPTSP_SPAN)?"span":"call" );
}

// Print a leg's parking and dead-store lines against the leg before it, and
// its tallies.
static void
writePark(FILE *fP, VPark *pkP, int l, const char *nameP)
{
static const char *parkNames[3] = { "-", "io", "ac" };
static const char *deadNames[3] = { "-", "deleted", "rewritten" };
VCand *candP;
VCandLeg *resP;
VCandLeg none;
const VCandLeg *baseP;
OptWordP wordP;
OptWordP nextP;
int c;
int i;
int t;
int at;
int words;
int counts[12];
char flagText[64];

    memset(&none, 0, sizeof(none));
    memset(counts, 0, sizeof(counts));
    resP = pkP->legsP[l];

    for( c = 0; c < pkP->candCount; ++c )
    {
        candP = &pkP->candsP[c];
        baseP = (l > 0)?&pkP->legsP[l - 1][c]:&none;
        wordP = candP->wordP;
        words = (candP->readCount + candP->writeCount);

        if( resP[c].park && (resP[c].park != baseP->park) )
        {
            at = 0;
            flagText[0] = '\0';

            if( resP[c].flags & VPF_CALL )
            {
                at += snprintf((flagText + at), (sizeof(flagText) - (size_t)at), "%scall", (at)?",":"");
            }

            if( resP[c].flags & VPF_OVERLAP )
            {
                at += snprintf((flagText + at), (sizeof(flagText) - (size_t)at), "%soverlap", (at)?",":"");
            }

            if( resP[c].flags & VPF_HANDLER )
            {
                at += snprintf((flagText + at), (sizeof(flagText) - (size_t)at), "%shandler", (at)?",":"");
            }

            fprintf(fP, "park %s %s %04o | b%d %s %s stores %d reloads %d | was %s | %s\n", nameP,
                parkNames[resP[c].park], wordP->addr, wordP->bank, firstLabelName(wordP), candClassName(candP->cls),
                candP->writeCount, candP->readCount, parkNames[baseP->park], (at)?flagText:"-");
        }
        else if( !resP[c].park && baseP->park )
        {
            fprintf(fP, "unpark %s %s %04o | b%d %s\n", nameP, parkNames[baseP->park], wordP->addr, wordP->bank,
                firstLabelName(wordP));
            ++pkP->lost[l];
        }

        if( resP[c].dead && (resP[c].dead != baseP->dead) )
        {
            fprintf(fP, "dead %s %s %04o | b%d %s %s stores %d reads %d | was %s\n", nameP, deadNames[resP[c].dead],
                wordP->addr, wordP->bank, firstLabelName(wordP), candClassName(candP->cls), candP->writeCount,
                candP->readCount, deadNames[baseP->dead]);
        }
        else if( !resP[c].dead && baseP->dead )
        {
            fprintf(fP, "undead %s %s %04o | b%d %s\n", nameP, deadNames[baseP->dead], wordP->addr, wordP->bank,
                firstLabelName(wordP));
        }

        // The tallies: 0-3 and 5 IO parking, 6-7 the twin, 8-9 and 4 call(2),
        // 10 handler.  Only a call(2) word can park across a call: local and
        // span words are live across none.
        if( resP[c].park && (resP[c].flags & VPF_HANDLER) )
        {
            ++counts[10];
        }
        else if( (resP[c].park == VP_IO) && (candP->cls == OPTSP_CALL) )
        {
            ++counts[8];
            counts[9] += words;
            counts[4] += ((resP[c].flags & VPF_CALL) != 0);
        }
        else if( resP[c].park == VP_IO )
        {
            ++counts[0];
            counts[(candP->cls == OPTSP_LOCAL)?1:2] += 1;
            counts[3] += words;
            counts[5] += ((resP[c].flags & VPF_OVERLAP) != 0);
        }
        else if( resP[c].park == VP_AC )
        {
            ++counts[6];
            counts[7] += words;
        }
    }

    // A swp pair: "dac u; lac t", u and t both parked in IO.
    counts[11] = 0;

    for( c = 0; c < pkP->candCount; ++c )
    {
        if( resP[c].park != VP_IO )
        {
            continue;
        }

        candP = &pkP->candsP[c];

        for( i = 0; i < candP->writeCount; ++i )
        {
            wordP = candP->writesPP[i];

            if( (nextP = wordAt(pkP->tableP, wordP->bank, ((wordP->addr + 1) & ADDRMASK))) && (nextP->blockP == wordP->blockP)
                && ((t = pkP->candOfIndexP[nextP->index]) >= 0) && (t != c) && (resP[t].park == VP_IO)
                && isCandLoad(pkP, t, nextP) )
            {
                ++counts[11];
            }
        }
    }

    fprintf(fP, "leg %s park io: %d words (local %d, span %d), %d words rewritten, %d us; overlapping %d\n", nameP,
        counts[0], counts[1], counts[2], counts[3], (counts[3] * VPARK_US), counts[5]);
    fprintf(fP, "leg %s park ac: %d words, %d words rewritten, %d us; swp pairs %d\n", nameP, counts[6], counts[7],
        (counts[7] * VPARK_US), counts[11]);
    fprintf(fP, "leg %s park apart: call(2) %d words, %d words rewritten, %d across a call; handler %d words;"
        " program %s a 1D mnemonic\n", nameP, counts[8], counts[9], counts[4], counts[10],
        (pkP->spelled1D)?"spells":"does not spell");

    memset(counts, 0, sizeof(counts));

    for( c = 0; c < pkP->candCount; ++c )
    {
        if( resP[c].dead == VD_DELETED )
        {
            ++counts[0];
            counts[1] += pkP->candsP[c].writeCount;
        }
        else if( resP[c].dead == VD_REWRITTEN )
        {
            ++counts[2];
            counts[3] += pkP->candsP[c].writeCount;
        }
    }

    fprintf(fP, "leg %s dead: reads deleted %d words, %d stores; reads rewritten %d words, %d stores\n", nameP,
        counts[0], counts[1], counts[2], counts[3]);
}

// Print the carry dump (-O=carry).  The format:
//
//   carry: ...                               what the dump is
//   routine NAME io X ac Y ioread Z writes W free io X ac Y writes W | ...
//   summary: ...                             the routine counts
//   hit L CLASS ADDR | bB spelling => replacement | was BASE | FLAGS | evidence
//   lost L CLASS ADDR | bB spelling          a hit of leg L's base that leg L lacks
//   vhit, vlost                              the same for a variant leg
//   leg L bank N: CLASS n ... = H hits, W words, U us
//   leg L total: ...
//   leg L flags: ...                         -O=values's safety flags, over the leg's hits
//   leg L blocks: ...                        what was carried and what reset
//   park L io|ac ADDR | bB NAME CLASS stores S reloads R | was K | FLAGS
//                                            a word main leg L parks and the leg before does not
//   unpark L ...                             one the leg before parks and leg L does not
//   dead L deleted|rewritten ADDR | ...      a word whose stores leg L's hits leave dead; undead
//   leg L park io:, park ac:, park apart:, dead:   the main leg's parking and dead-store tallies
//   gain:, a55:, writes:                     the differences between legs
//   control: ...                             leg 0 against -O=values's pass
//   reconcile: ok|FAILED
//
// Leg 0 resets every block (A21's pass); leg 1 carries over every edge but a
// call's return; leg 2 carries over a return what the callee's summary
// proves; leg 3 what an oracle grants: IO kept, AC kept for jda and cal, only
// the words the callee names directly written.  2i and 3i take an unknown
// in-out transfer to touch nothing (A55's ceiling); 2w and 3w apply no
// callee's writes (what a marker for writes could buy at most).
void
optDumpCarry(FILE *fP, OptTableP tableP)
{
VPass home;
VPass values;
VPass *passesP;
VCarry carry;
VPark park;
OptCarryMaps maps;
OptPreserveP sumsP;
VHitP aP;
VHitP bP;
int hits[VLEG_COUNT];
int words[VLEG_COUNT];
int us[VLEG_COUNT];
int l;
int bank;
int i;
int sum;
int diffs;
int reconciled;
char prefix[64];

    markKnown(tableP);
    initCarryPass(&home, tableP, 0);
    carryHomeP = &home;
    optCarryMapsBuild(tableP, &maps);
    sumsP = optPreserveBuild(tableP, &maps);

    fprintf(fP, "carry: what AC and IO hold across blocks and calls (task A52); a measurement, nothing is rewritten\n");
    fprintf(fP, "carry: leg 0 resets every block (A21's pass), 1 carries every edge but a call's return,\n");
    fprintf(fP, "carry: 2 carries a return by the callee's summary, 3 by an oracle; 2i 3i in-out transfers touch nothing,\n");
    fprintf(fP, "carry: 2w 3w a callee's writes are not applied\n");
    optPreserveDump(fP, tableP, sumsP);

    passesP = (VPass *)vAlloc((size_t)VLEG_COUNT, sizeof(VPass));
    parkInit(&park, tableP, &maps, sumsP, &home);
    reconciled = 1;

    for( l = 0; l < VLEG_COUNT; ++l )
    {
        initCarryPass(&passesP[l], tableP, carryLegs[l].iotFree);
        carryInit(&carry, &passesP[l], tableP, &maps, sumsP, carryLegs[l].leg, carryLegs[l].mode, NILP, NILP);

        for( i = 1; i <= carry.count; ++i )
        {
            carry.scopeP[i] = (maps.blockOfIdPP[i] != NILP);
        }

        carryPreds(&carry);
        carryFixed(&carry);
        carrySolve(&carry);

        if( l > 0 )
        {
            writeLegDiff(fP, tableP, l, passesP);
        }

        sum = 0;

        for( bank = 0; bank <= MAXBANK; ++bank )
        {
            if( !passesP[l].bankUsed[bank] )
            {
                continue;
            }

            snprintf(prefix, sizeof(prefix), "leg %s bank %d", carryLegs[l].nameP, bank);
            writeTally(fP, prefix, passesP[l].classHits[bank], passesP[l].classWords[bank], passesP[l].classUs[bank]);
            sum += passesP[l].bankHits[bank];
        }

        legTotals(&passesP[l], &hits[l], &words[l], &us[l]);
        fprintf(fP, "leg %s total: %d hits, %d words, %d us\n", carryLegs[l].nameP, hits[l], words[l], us[l]);
        fprintf(fP, "leg %s flags:", carryLegs[l].nameP);

        for( i = 0; i < VF_COUNT; ++i )
        {
            fprintf(fP, "%s %s %d", (i)?",":"", flagNames[i], passesP[l].flagHits[i]);
        }

        fprintf(fP, "\n");
        fprintf(fP, "leg %s blocks: %d, carried %d, reset %d (entry %d, unreached %d, taken %d, after an undecided call %d);"
            " rounds %d, %s%s\n", carryLegs[l].nameP, carry.walked, carry.carried,
            (carry.walked - carry.carried), carry.fixedEntry,
            carry.fixedUnreached, carry.fixedTaken, carry.fixedAfterCall, carry.rounds,
            (carry.settled)?"settled":"UNSETTLED", (carry.widened)?", widened":"");

        if( !carry.settled || (sum != passesP[l].totalHits) || (hits[l] != passesP[l].totalHits) )
        {
            reconciled = 0;
            fprintf(fP, "reconcile: leg %s banks %d, classes %d, hits %d, %s\n", carryLegs[l].nameP, sum, hits[l],
                passesP[l].totalHits, (carry.settled)?"settled":"unsettled");
        }

        if( l < VLEG_MAIN )
        {
            parkLeg(&park, &carry, l, &passesP[l]);
            writePark(fP, &park, l, carryLegs[l].nameP);
        }

        carryFree(&carry);
    }

    fprintf(fP, "gain: leg 1 %+d hits %+d words %+d us over leg 0; leg 2 %+d hits %+d words %+d us over leg 1;"
        " leg 3 %+d hits %+d words %+d us over leg 2\n",
        (hits[1] - hits[0]), (words[1] - words[0]), (us[1] - us[0]), (hits[2] - hits[1]), (words[2] - words[1]),
        (us[2] - us[1]), (hits[3] - hits[2]), (words[3] - words[2]), (us[3] - us[2]));
    fprintf(fP, "a55: leg 2i %+d hits %+d us over leg 2; leg 3i %+d hits %+d us over leg 3\n",
        (hits[4] - hits[2]), (us[4] - us[2]), (hits[5] - hits[3]), (us[5] - us[3]));
    fprintf(fP, "writes: leg 2w %+d hits %+d us over leg 2; leg 3w %+d hits %+d us over leg 3\n",
        (hits[6] - hits[2]), (us[6] - us[2]), (hits[7] - hits[3]), (us[7] - us[3]));

    // The parking legs nest: each finds every candidate of the leg before it.
    for( l = 1; l < VLEG_MAIN; ++l )
    {
        if( park.lost[l] )
        {
            reconciled = 0;
            fprintf(fP, "reconcile: park leg %s loses %d of leg %s's candidates\n", carryLegs[l].nameP, park.lost[l],
                carryLegs[l - 1].nameP);
        }
    }

    parkFree(&park);

    // The control: leg 0 must be A21's pass, hit for hit.
    initPass(&values, tableP, NILP, 0);
    runPass(&values);
    diffs = 0;

    for( i = 0; i < tableP->count; ++i )
    {
        aP = values.hitOfWordPP[i];
        bP = passesP[0].hitOfWordPP[i];

        if( (!aP != !bP) || (aP && ((aP->cls != bP->cls) || (aP->firstCls != bP->firstCls))) )
        {
            ++diffs;
            fprintf(fP, "control: differs at b%d %04o: values %s, leg 0 %s\n", (aP)?aP->wordP->bank:bP->wordP->bank,
                (aP)?aP->wordP->addr:bP->wordP->addr, (aP)?classTable[aP->cls].nameP:"-",
                (bP)?classTable[bP->cls].nameP:"-");
        }
    }

    fprintf(fP, "control: leg 0 equals -O=values on %d hits: %s\n", values.totalHits, (diffs)?"DIFFERS":"ok");
    fprintf(fP, "reconcile: %s\n", (reconciled)?"ok":"FAILED");

    freePass(&values);

    for( l = 0; l < VLEG_COUNT; ++l )
    {
        freePass(&passesP[l]);
    }

    free(passesP);
    optPreserveFree(tableP, sumsP);
    optCarryMapsFree(&maps);
    carryHomeP = NILP;
    freePass(&home);
    free(knownOfWordP);
    knownOfWordP = NILP;
}
