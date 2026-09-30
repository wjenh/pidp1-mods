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

// A value no other place holds.
// Returns the new value number.
static long
freshVal(VPass *passP)
{
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
        makeHit(passP, VC_RLOAD, wordP, laterOf(passP->reg[reg].since, origin), wordP->addr, val, memAddr, "delete",
            allocPrintf("%s already holds the value of %04o (since %04o)", regName(reg), memAddr,
                laterOf(passP->reg[reg].since, origin)));
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
        hitP = makeHit(passP, cls, wordP, laterOf(passP->reg[other].since, origin), wordP->addr, val, memAddr,
            classTable[cls].nameP,
            allocPrintf("%s holds the value of %04o (since %04o)", regName(other), memAddr,
                laterOf(passP->reg[other].since, origin)));
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

    // A word the program writes or patches is not the assembled word.
    if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED) )
    {
        killAll(passP, wordP, VK_WRITTEN);
        return;
    }

    rowP = findRow(wordP);

    if( rowP->effects & VE_KILL )
    {
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
    passP->reg[VR_AC].val = freshVal(passP);
    passP->reg[VR_AC].since = -1;
    passP->reg[VR_IO].val = freshVal(passP);
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

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        free(passP->handlerWrittenP[bank]);
    }

    free(passP->handlerBlockP);
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
