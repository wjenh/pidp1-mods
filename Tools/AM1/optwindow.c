/*
 * The am1 optimizer's extend-window analysis (-O=window and the report's
 * "Extend window" section), and the deletions it licenses inside a
 * declaration.  It creates no finding.  Outside an optimize or speed region it
 * is advice only; inside one, -O1 and -O2 delete the redundant eems and lems,
 * the dead lems and the freed eems it finds, through relayout's delete.
 *
 * On the PDP-1D, eem opens the extend window and lem closes it.  With the
 * window closed an indirect word is cut to twelve bits in the current bank,
 * and one with 010000 set chains; with it open an indirect word is a 16-bit
 * address, one level only.  Direct references never change bank, and eem and
 * lem never change the current bank.  A jsp puts 0200000 in AC when the
 * caller had the window open.  What a program start and a halt and continue
 * do to the window is not documented, so both are unknown here.  The reverse
 * hazard rests on two closed-window facts: the bits above 010000 are ignored
 * and the reference stays in the current bank, so a 16-bit address reaches
 * its word only when its bank is the current one and even; and a jsp leaves
 * the full 16-bit return address whatever the state, so a dac-form return
 * made closed chains in every odd bank.
 *
 * Forward, every reached word gets the set of states the window may be in:
 * ON, OFF, or unknown with a reason.  The state crosses a call and its return
 * through a per-routine summary, so a return lands with the state its own
 * caller had wherever the callee leaves the window alone.  A routine gets a
 * summary only when control can enter its body nowhere but at its entry and
 * leave it only by its return word; any other routine's return carries every
 * state its return words see.  Backward, every word gets what depends on the
 * window after it: a proved dependence (an indirect reference whose pointer
 * gives a different address open and closed), or an unknown one with a reason.
 *
 * From the two:
 *   - an eem or lem whose state is already what it sets is redundant;
 *   - a lem nothing depends on before the window is set again is dead, and an
 *     eem is freed when it is redundant once every dead lem is gone;
 *   - an indirect reference in a bank other than 0, made while the window may
 *     be open, through a pointer holding a bare in-bank address, is a proved
 *     hazard: it reaches bank 0.  Through a pointer whose value is not known
 *     it is an unproved hazard.
 *   - the reverse: an indirect reference made while the window may be
 *     closed, through a pointer meant as a 16-bit address (bank-qualified, or
 *     a number above 07777) that the closed window reads as some other word,
 *     is a proved reverse hazard.  So is a dac-form routine's return word used
 *     closed where a caller's bank makes its saved address misread.  A
 *     pointer written at run time from such a constant is an unproved one.
 * A redundant or dead word is refused, with one reason, where deleting it
 * could not be shown safe: it is written or executed by an xct, it follows a
 * skip, H2 or H3 marks it, or a sequence-break handler could change the
 * window between any two words.  Sets are bit masks, so every pass is a
 * monotone fixpoint from empty.
 *
 * Why the deletions are safe in any combination but one.  At a word that
 * depends on the window, walk any path back to the first eem or lem that is
 * neither redundant nor dead: every word passed is redundant (the backward pass
 * saw through them, and a dead lem is one nothing after it depends on), and a
 * redundant word sets the state the path already has.  So deleting any subset
 * of the redundant words and dead lems leaves the state at every such word as
 * it was.  A freed eem is the exception: it is redundant only once certain
 * lems are gone, a dead lem or a redundant one that the dead lem made
 * redundant.  Which lems those are is found by putting each back alone and
 * running the forward pass again; one that leaves the eem's state other than
 * open is a lem it depends on.  Two put back together can do nothing one alone
 * does not, since on any path the last setter is one of them or an eem.  A
 * freed eem is deleted only with all of its lems.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"
#include "y.tab.h"
#include "optimizer.h"

// Instruction codes this file tests, as the decoder writes them.
#define WOP_XCT         0010
#define WOP_CAL         0016
#define WOP_LAC         0020
#define WOP_DAC         0024
#define WOP_DAP         0026
#define WOP_IDX         0044
#define WOP_ISP         0046
#define WOP_JMP         0060
#define WOP_JSP         0062

// The two window instructions, and every in-out transfer on their device, with
// the wait bit (5) masked off.
#define WV_MASK         0767777
#define WV_EEM          0724074
#define WV_LEM          0720074
#define WV_DEVMASK      0760077
#define WV_DEVICE       0720074

// Why a state is unknown, a dependence unproved or a candidate refused.  The
// order is the order of preference when one reason must be named: the gates
// first, since they refuse whatever the state.
typedef enum
{
    W_PATCHED,          // the word is written at run time
    W_XCT,              // an xct executes it, or the word is an xct
    W_SBS,              // a sequence-break handler may run between any two words
    W_TIMING,           // H2 or H3: a delay or device loop, whose time a deletion changes
    W_SKIP,             // a skip-class word may skip it, so deleting it moves the skip
    W_START,            // the start address: the window's state there is not recorded
    W_ENTRY,            // entered from outside the graph: an exported label, a taken
                        // address, or an indirect jump the graph could not follow
    W_HALT,             // after a halt and continue, whose effect is not recorded
    W_CALLEE,           // a call the graph could not follow
    W_RETURN,           // where a call returns, or what state it returns with, is not known
    W_LOST,             // control left the graph
    W_SHAPE,            // a routine's body is left other than by its return
    W_POINTER,          // an indirect reference through a pointer whose value is not known
    W_FLAG,             // a word that reads the window's flag: a call, whose AC gets 0200000
                        // when the window is open, or lap
    W_IOT,              // an in-out transfer on eem's device that is neither eem nor lem
    W_CALLERS,          // a return word the window may be closed at only on the paths
                        // from callers whose saved address it reads right
    W_COUNT
} WinWhy;

static const char *whyNames[W_COUNT] =
{
    "patched", "xct", "sbs", "timing", "skip", "start", "entry", "halt", "callee",
    "unknown-return", "lost", "shape", "pointer", "flag", "iot", "callers"
};

// Forward state bits, then backward dependence bits; both share the reason
// bits above bit 3.  ENTRY and EXIT are symbolic, used only in a summary: the
// state the routine was entered with, and what depends on the window after
// its return.
#define F_ON            0x1u
#define F_OFF           0x2u
#define F_ENTRY         0x4u
#define L_PROVED        0x1u
#define L_EXIT          0x4u
#define WB(why)         (1u << (4 + (why)))
#define WB_MASK         (~0xFu)

// What one eem or lem was found to be.
typedef enum
{
    WC_NONE,            // not an eem or lem
    WC_UNREACHED,       // no path reaches it
    WC_REDUNDANT,       // the window is already in the state it sets
    WC_FREED,           // an eem redundant once every dead lem is deleted
    WC_DEAD,            // a lem nothing depends on before the window is set again
    WC_NEEDED,          // it changes the state, and for a lem something depends on it
    WC_REFUSED,         // not shown to be either; whyP says why
    WC_COUNT
} WinClass;

static const char *classNames[WC_COUNT] =
{
    "-", "unreached", "redundant", "freed", "dead", "needed", "refused"
};

// What one indirect reference was found to be.
typedef enum
{
    WH_NONE,
    WH_PROVED,          // the window may be open and the pointer holds a bare address
    WH_UNPROVED,        // the window may be open and the pointer's value is not known
    WH_POSSIBLE,        // the pointer holds a bare address and the state is not known
    WH_COUNT
} WinHazard;

static const char *hazardNames[WH_COUNT] = { "-", "proved", "unproved", "possible" };

// What a pointer expression's address leaves are (leafKinds()).
#define LK_BARE         1           // a label or pool reference with no bank qualifier
#define LK_QUALIFIED    2           // a sym:N or sym:* reference

// How an indirect reference's pointer word is known.
typedef enum
{
    PC_NONE,            // not an indirect memory reference
    PC_CONST,           // a word never written: its assembled value is the value
    PC_RETURN,          // a routine's return word, saved with dac and stepped with idx
                        // or isp: a 16-bit address by the register contract
    PC_RUNTIME,         // anything else
    PC_DEBREAK          // "jmp i 1" in bank 0, the return from a sequence break
} WinPointer;

typedef struct winlink
{
    struct winlink *nextP;
    OptRoutineP routineP;       // a return list: the routine the block returns from
    OptBlockP blockP;           // a caller list: the block whose last word calls
} WinLink, *WinLinkP;

// The whole analysis.  The arrays are by word index, block id or routine id.
typedef struct optwindow
{
    OptTableP tableP;
    OptBlockP *blocksPP;
    int blockCount;
    OptRoutineP *routinesPP;
    int routineCount;
    OptRoutineP *entryOfPP;         // by block id: the routine whose entry block it is
    unsigned char *closedP;         // by routine id: the routine has summaries
    int *returnCountP;              // by routine id: body blocks ending in its return word
    WinLinkP *returnsPP;            // by block id: routines its last word returns from
    WinLinkP *callersPP;            // by routine id: blocks calling it
    OptRoutineP *returnWordOfPP;    // by word index: the dac-form routine it is the return word of
    int *stampP;                    // by block id: the routine whose body is being walked
    int sbs;                        // the program leaves sequence-break frames

    // The forward pass, filled once as written and once without the words
    // noopP marks.
    const unsigned char *noopP;
    unsigned int *finP;             // by block id
    unsigned int *fsumP;            // by routine id
    unsigned int *fwordP;           // by word index: the state before the word
    unsigned int *linP;             // by block id: a summary's local states

    // The backward pass.
    unsigned char *transparentP;    // by word index: a redundant word, deleted in thought
    unsigned int *binP;             // by block id
    unsigned int *bsumP;            // by routine id
    unsigned int *lafterP;          // by word index: what depends on the window after a lem

    // The verdicts.
    unsigned int *stateP;           // by word index: the forward state as written
    unsigned char *classP;          // by word index
    unsigned char *whyP;
    unsigned char *hazardP;
    unsigned char *hazardWhyP;
    unsigned char *reverseP;        // by word index: the reverse hazard, a WinHazard
    unsigned char *reverseWhyP;
    unsigned char *unexaminedP;     // by word index: an indirect reference an unreached eem or lem guards

    int present;                    // an eem or lem exists, reached or not
    int active;                     // a reached eem or lem exists
    int closedCount;
    int eemCounts[WC_COUNT];
    int lemCounts[WC_COUNT];
    int refusedWhy[W_COUNT];
    int hazardCounts[WH_COUNT];
    int reverseCounts[WH_COUNT];
    int unknownRuntime;            // references through an unknown pointer, state unknown
    int unexaminedCount;
} OptWindow, *OptWindowP;

static void *winAlloc(size_t count, size_t size);
static int isEem(OptWordP wordP);
static int isLem(OptWordP wordP);
static int isOtherDevice(OptWordP wordP);
static int isXct(OptWordP wordP);
static int hasExportedLabel(OptWordP wordP);
static int isReturnOf(OptRoutineP routineP, OptWordP wordP);
static int lowestWhy(unsigned int bits);
static OptRoutineP calleeOf(OptWindowP winP, OptWordP wordP);
static OptFlowEdgeP returnEdgeOf(OptBlockP blockP);
static void buildMaps(OptWindowP winP);
static unsigned int entryUnknown(OptWindowP winP, OptWordP wordP);
static int calledOnlyThrough(OptWordP pointerP);
static WinPointer pointerClass(OptWindowP winP, OptWordP wordP, OptWordP *pointerPP);
static int returnWordPointer(OptWindowP winP, OptWordP pointerP);
static int dapClean(OptWindowP winP, OptRoutineP routineP);

static unsigned int fwdWord(OptWindowP winP, OptWordP wordP, unsigned int state);
static unsigned int fwdBlock(OptWindowP winP, OptBlockP blockP, unsigned int state, int record);
static unsigned int fwdApply(unsigned int summary, unsigned int state);
static unsigned int routineExit(OptWindowP winP, OptRoutineP routineP, unsigned int state);
static unsigned int returnState(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP, unsigned int state);
static int fwdSummaries(OptWindowP winP);
static int fwdGlobal(OptWindowP winP);
static void forward(OptWindowP winP, const unsigned char *noopP);

static unsigned int useOf(OptWindowP winP, OptWordP wordP);
static unsigned int backBlock(OptWindowP winP, OptBlockP blockP, unsigned int live, int record);
static unsigned int backApply(unsigned int summary, unsigned int live);
static unsigned int callLive(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP, unsigned int *localP,
    OptRoutineP routineP);
static unsigned int returnLive(OptWindowP winP, OptRoutineP routineP);
static unsigned int sinkLive(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP);
static int backSummaries(OptWindowP winP);
static int backGlobal(OptWindowP winP);
static void backward(OptWindowP winP);

static int gateWhy(OptWindowP winP, OptWordP wordP);
static void classify(OptWindowP winP);
static void countClasses(OptWindowP winP);
static void markUnexamined(OptWindowP winP, OptBlockP blockP, int fromAddr);
static void findUnexamined(OptWindowP winP);
static int leafKinds(PNodeP nodeP);
static void findHazards(OptWindowP winP);
static int closedMisreads(int value, int bank);
static WinHazard returnMisread(OptWindowP winP, OptWordP pointerP, int bank, int *callerBankP);
static int writtenWide(OptWindowP winP, OptWordP pointerP, int bank);
static void findReverse(OptWindowP winP);
static void warnHazards(OptWindowP winP);
static const char *wordFile(OptWordP wordP);

static OptWinDelP addWinDel(OptTableP tableP, OptWordP wordP, OptWinDelKind kind);
static void findDeps(OptWindowP winP);
static OptWinDelFate winDelGate(OptTableP tableP, OptWinDelP delP);
static OptWinDelP winDelOf(OptTableP tableP, OptWordP wordP);

// Each deletion's name, and each fate's word for the dump and phrase for the
// report.  Indexed by OptWinDelKind and OptWinDelFate.
static const char *kindNames[OPTWD_KIND_COUNT] =
{
    "redundant eem", "redundant lem", "dead lem", "freed eem"
};

static const struct
{
    const char *tagP;
    const char *textP;
} winDelFates[OPTWD_COUNT] =
{
    { "fired",           "deleted" },
    { "handsoff",        "inside a nooptimize span" },
    { "guessed",         "left alone on -O2's guess" },
    { "used",            "read as data, so deleting it would change a value the program reads" },
    { "unrepresentable", "not one statement's whole, single word" },
    { "overlap",         "a word another rewrite makes, copies, moves or deletes, or one just after a deletion" },
    { "depends",         "a lem it depends on is not deleted" },
    { "off",             "switched off for bisection" }
};

// Allocate a zeroed array; out of memory is fatal.
// Returns the array.
static void *
winAlloc(size_t count, size_t size)
{
void *p;

    if( !(p = calloc((count)?count:1, size)) )
    {
        fprintf(stderr, "am1: optimizer: out of memory in the window analysis\n");
        exit(1);
    }

    return(p);
}

// Test for eem, with or without the wait bit.  Returns 1 if it is, 0 if not.
static int
isEem(OptWordP wordP)
{
    return( (wordP->decode.group == OPTG_IOT) && ((wordP->value & WV_MASK) == WV_EEM) );
}

// Test for lem, with or without the wait bit.  Returns 1 if it is, 0 if not.
static int
isLem(OptWordP wordP)
{
    return( (wordP->decode.group == OPTG_IOT) && ((wordP->value & WV_MASK) == WV_LEM) );
}

// Test for an in-out transfer on the window's device that is neither eem nor
// lem: what it does to the window is not recorded.  Returns 1 if it is, 0 if not.
static int
isOtherDevice(OptWordP wordP)
{
    return( (wordP->decode.group == OPTG_IOT) && ((wordP->value & WV_DEVMASK) == WV_DEVICE) &&
        !isEem(wordP) && !isLem(wordP) );
}

// Test for xct, direct or indirect.  Returns 1 if it is, 0 if not.
static int
isXct(OptWordP wordP)
{
    return( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == WOP_XCT) );
}

// Test for an exported label, as optflow.c's entry set does.  Returns 1 if
// the word carries one, 0 if not.
static int
hasExportedLabel(OptWordP wordP)
{
OptLabelP labelP;

    for( labelP = wordP->labelsP; labelP; labelP = labelP->nextP )
    {
        if( labelP->symP && (labelP->symP->flags & SYMF_EXPORTED) )
        {
            return(1);
        }
    }

    return(0);
}

// Test whether a word is one of a routine's returns: the patched "rtn, jmp ."
// word itself, or an unpatched "jmp i rtn".  Must match optroutine.c's
// isReturnWord() and optspeed.c's isReturnOf().  Returns 1 when it is, 0 otherwise.
static int
isReturnOf(OptRoutineP routineP, OptWordP wordP)
{
    if( !wordP || !inGraph(wordP) || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    if( (wordP->bank == routineP->returnBank) && (wordP->addr == routineP->returnAddr) )
    {
        return( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == WOP_JMP) );
    }

    if( (wordP->decode.group != OPTG_MEMREF) || (wordP->decode.opcode != WOP_JMP) ||
        !wordP->decode.memIndirect || (wordP->flags & OPTF_PATCHED) )
    {
        return(0);
    }

    return( (wordP->bank == routineP->returnBank) && (wordP->decode.address == routineP->returnAddr) );
}

// The reason to name for a set of reason bits: the first in WinWhy's order.
// Returns it, or W_COUNT when the set holds none.
static int
lowestWhy(unsigned int bits)
{
int why;

    for( why = 0; why < W_COUNT; ++why )
    {
        if( bits & WB(why) )
        {
            return(why);
        }
    }

    return(W_COUNT);
}

// The routine a call word enters.  Returns it, or NILP when the call's target
// is not one known routine entry.
static OptRoutineP
calleeOf(OptWindowP winP, OptWordP wordP)
{
OptWordP entryP;

    if( !wordP->isCallSite || (wordP->calleeBank < 0) || (wordP->calleeAddr < 0) )
    {
        return(NILP);
    }

    entryP = wordAt(winP->tableP, wordP->calleeBank, wordP->calleeAddr);

    if( !entryP || !entryP->blockP )
    {
        return(NILP);
    }

    return( winP->entryOfPP[entryP->blockP->id] );
}

// A call block's return edge.  Returns it, or NILP when the block has none.
static OptFlowEdgeP
returnEdgeOf(OptBlockP blockP)
{
OptFlowEdgeP edgeP;

    for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->kind == OPTFK_RETURN )
        {
            return(edgeP);
        }
    }

    return(NILP);
}

// Index the routines by id and by entry block, list each block's returns and
// each routine's callers, mark the dac-form return words, and decide which
// routines get summaries.
static void
buildMaps(OptWindowP winP)
{
OptTableP tableP;
OptRoutineP routineP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
OptWordP wordP;
WinLinkP linkP;
int i;
int id;

    tableP = winP->tableP;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        winP->routinesPP[routineP->id] = routineP;

        if( routineP->entryBlockP )
        {
            winP->entryOfPP[routineP->entryBlockP->id] = routineP;
        }

        for( i = 0; i < routineP->blockCount; ++i )
        {
            id = routineP->bodyIdsP[i];
            blockP = winP->blocksPP[id];

            if( blockP && isReturnOf(routineP, blockP->lastP) )
            {
                linkP = winAlloc(1, sizeof(WinLink));
                linkP->routineP = routineP;
                linkP->nextP = winP->returnsPP[id];
                winP->returnsPP[id] = linkP;
                ++winP->returnCountP[routineP->id];
            }
        }

        // The dac form's return word holds the caller's whole AC: a 16-bit
        // address, by the register contract.
        wordP = wordAt(tableP, routineP->bank, routineP->entryAddr);

        if( (routineP->returnWord == OPTRW_SAVED) && (routineP->returnBank >= 0) && wordP &&
            (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == WOP_DAC) &&
            !wordP->decode.memIndirect )
        {
            wordP = wordAt(tableP, routineP->returnBank, routineP->returnAddr);

            if( wordP )
            {
                winP->returnWordOfPP[wordP->index] = routineP;
            }
        }

        // A summary holds only where the body is entered at its entry alone
        // and left only by its return word.
        if( !(routineP->flags & (OPTRT_OPENBODY | OPTRT_SHARED | OPTRT_FALLIN | OPTRT_FALLSOUT)) &&
            (routineP->returnWord != OPTRW_NONE) && (routineP->returnAddr >= 0) && routineP->entryBlockP )
        {
            winP->closedP[routineP->id] = 1;
            ++winP->closedCount;
        }
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind != OPTFK_CALL) || !edgeP->toP || !(routineP = winP->entryOfPP[edgeP->toP->id]) )
            {
                continue;
            }

            linkP = winAlloc(1, sizeof(WinLink));
            linkP->blockP = blockP;
            linkP->nextP = winP->callersPP[routineP->id];
            winP->callersPP[routineP->id] = linkP;
        }
    }
}

// The state an entry of the reachability walk is entered with from outside
// the graph.  A word whose address is taken only by pointer words that
// followed indirect jumps and calls read is entered only along those edges,
// which carry their own state.
// Returns the reason bit, or 0 when every way in is an edge of the graph.
static unsigned int
entryUnknown(OptWindowP winP, OptWordP wordP)
{
OptEdgeP edgeP;

    if( wordP->flags & OPTF_START )
    {
        return( WB(W_START) );
    }

    if( optIsSbsEntry(winP->tableP, wordP->bank, wordP->addr) )
    {
        return( WB(W_SBS) );
    }

    if( hasExportedLabel(wordP) || (wordP->flags & OPTF_MAYBE_ENTERED) )
    {
        return( WB(W_ENTRY) );
    }

    if( wordP->flags & OPTF_TAKEN )
    {
        for( edgeP = wordP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( (edgeP->role == OPTR_TAKEN) && !calledOnlyThrough(edgeP->fromP) )
            {
                return( WB(W_ENTRY) );
            }
        }
    }

    return(0);
}

// Test whether a word that takes an address is a pointer used only by
// indirect jmps and jsps the graph followed through it: data, never written,
// never possibly read or written through an unknown pointer.
// Returns 1 if so, 0 if not.
static int
calledOnlyThrough(OptWordP pointerP)
{
OptEdgeP edgeP;
OptWordP fromP;

    if( !pointerP || inGraph(pointerP) ||
        (pointerP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN | OPTF_MAYBE_READ |
        OPTF_RESERVED | OPTF_DUPADDR)) )
    {
        return(0);
    }

    for( edgeP = pointerP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        fromP = edgeP->fromP;

        if( !(edgeP->flags & OPTEF_INDIRECT) ||
            (edgeP->flags & (OPTEF_VIAPOINTER | OPTEF_UNKNOWN | OPTEF_PLACEHOLDER)) ||
            (edgeP->role != OPTR_JUMP) || (fromP->decode.group != OPTG_MEMREF) ||
            ((fromP->decode.opcode != WOP_JMP) && (fromP->decode.opcode != WOP_JSP)) )
        {
            return(0);
        }
    }

    return(1);
}

// How an indirect reference's pointer word is known, and which word it is.
// Returns the class, with *pointerPP set to the word or NILP.
static WinPointer
pointerClass(OptWindowP winP, OptWordP wordP, OptWordP *pointerPP)
{
OptEdgeP edgeP;
OptWordP pointerP;

    *pointerPP = NILP;

    if( (wordP->decode.group != OPTG_MEMREF) || !wordP->decode.memIndirect )
    {
        return(PC_NONE);
    }

    if( (wordP->bank == 0) && (wordP->decode.opcode == WOP_JMP) && (wordP->decode.address == OPTDEBREAK_ADDR) )
    {
        return(PC_DEBREAK);
    }

    // A patched address field names a different pointer word each time.
    if( wordP->flags & OPTF_PATCHED )
    {
        return(PC_RUNTIME);
    }

    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->flags & OPTEF_INDIRECT) && !(edgeP->flags & (OPTEF_VIAPOINTER | OPTEF_PLACEHOLDER)) )
        {
            break;
        }
    }

    if( !edgeP || (edgeP->flags & OPTEF_NOWORD) || !(pointerP = edgeP->toP) )
    {
        return(PC_RUNTIME);
    }

    *pointerPP = pointerP;

    if( pointerP->flags & (OPTF_RESERVED | OPTF_DUPADDR) )
    {
        return(PC_RUNTIME);
    }

    if( returnWordPointer(winP, pointerP) )
    {
        return(PC_RETURN);
    }

    if( pointerP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN) )
    {
        return(PC_RUNTIME);
    }

    return(PC_CONST);
}

// Test whether a pointer word is a dac-form routine's return word, written
// only by the routine's entry save and stepped only by idx or isp.
// Returns 1 if so, 0 if not.
static int
returnWordPointer(OptWindowP winP, OptWordP pointerP)
{
OptRoutineP routineP;
OptEdgeP edgeP;
OptWordP fromP;

    if( !(routineP = winP->returnWordOfPP[pointerP->index]) ||
        (pointerP->flags & (OPTF_PATCHED | OPTF_MAYBE_WRITTEN)) )
    {
        return(0);
    }

    for( edgeP = pointerP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( (edgeP->role != OPTR_WRITE) || (edgeP->flags & OPTEF_INDIRECT) )
        {
            continue;
        }

        fromP = edgeP->fromP;

        if( (fromP->bank == routineP->bank) && (fromP->addr == routineP->entryAddr) )
        {
            continue;
        }

        if( (fromP->decode.group == OPTG_MEMREF) &&
            ((fromP->decode.opcode == WOP_IDX) || (fromP->decode.opcode == WOP_ISP)) )
        {
            continue;
        }

        return(0);
    }

    return(1);
}

// Test whether a routine's return ignores the window flag a call leaves in AC:
// its entry saves with a direct dap into its own "jmp ." return word, which
// keeps only the address bits, and nothing reads that word as data.
// Returns 1 if so, 0 if not.
static int
dapClean(OptWindowP winP, OptRoutineP routineP)
{
OptWordP entryP;
OptWordP returnP;

    if( routineP->returnWord != OPTRW_SAVED )
    {
        return(0);
    }

    entryP = wordAt(winP->tableP, routineP->bank, routineP->entryAddr);
    returnP = wordAt(winP->tableP, routineP->returnBank, routineP->returnAddr);

    if( !entryP || !returnP || (entryP->decode.group != OPTG_MEMREF) ||
        (entryP->decode.opcode != WOP_DAP) || entryP->decode.memIndirect )
    {
        return(0);
    }

    return( (returnP->decode.group == OPTG_MEMREF) && (returnP->decode.opcode == WOP_JMP) &&
        !returnP->inCounts[OPTR_READ] && !(returnP->flags & (OPTF_MAYBE_READ | OPTF_TAKEN)) );
}

// The forward pass.

// One word's effect on the window state.  A word noopP marks is deleted in
// thought.  An xct keeps the state only when its one target is a fixed word
// that is not itself an xct or on the window's device.
// Returns the state after the word.
static unsigned int
fwdWord(OptWindowP winP, OptWordP wordP, unsigned int state)
{
OptEdgeP edgeP;
OptWordP targetP;
int targets;

    if( !state || (winP->noopP && winP->noopP[wordP->index]) )
    {
        return(state);
    }

    if( isEem(wordP) )
    {
        return(F_ON);
    }

    if( isLem(wordP) )
    {
        return(F_OFF);
    }

    if( isOtherDevice(wordP) )
    {
        return( WB(W_IOT) );
    }

    if( !isXct(wordP) )
    {
        return(state);
    }

    targetP = NILP;
    targets = 0;

    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role == OPTR_EXECUTE) && !(edgeP->flags & (OPTEF_INDIRECT | OPTEF_PLACEHOLDER)) )
        {
            targetP = edgeP->toP;
            ++targets;
        }
    }

    if( (targets != 1) || !targetP || (wordP->flags & OPTF_PATCHED) ||
        (targetP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN | OPTF_RESERVED | OPTF_DUPADDR)) ||
        isXct(targetP) || (targetP->decode.group == OPTG_IOT && ((targetP->value & WV_DEVMASK) == WV_DEVICE)) )
    {
        return( WB(W_XCT) );
    }

    return(state);
}

// Walk a block's words from a state, recording each word's state before it
// when record is set.  Returns the state after the last word.
static unsigned int
fwdBlock(OptWindowP winP, OptBlockP blockP, unsigned int state, int record)
{
OptWordP wordP;
int addr;

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        if( !(wordP = wordAt(winP->tableP, blockP->bank, addr)) )
        {
            continue;
        }

        if( record )
        {
            winP->fwordP[wordP->index] |= state;
        }

        state = fwdWord(winP, wordP, state);
    }

    return(state);
}

// A forward summary applied to the state a call was made in.
// Returns the state the call returns with.
static unsigned int
fwdApply(unsigned int summary, unsigned int state)
{
    if( !state )
    {
        return(0);
    }

    return( (summary & ~F_ENTRY) | ((summary & F_ENTRY)?state:0) );
}

// The state a call to a routine returns with: through its summary, or else
// every state its return words are reached with.
// Returns the state.
static unsigned int
routineExit(OptWindowP winP, OptRoutineP routineP, unsigned int state)
{
OptBlockP blockP;
WinLinkP linkP;
unsigned int leave;
int i;

    if( !state )
    {
        return(0);
    }

    if( winP->closedP[routineP->id] )
    {
        return( fwdApply(winP->fsumP[routineP->id], state) );
    }

    if( (routineP->returnWord == OPTRW_NONE) || !winP->returnCountP[routineP->id] )
    {
        return( WB(W_RETURN) );
    }

    leave = 0;

    for( i = 0; i < routineP->blockCount; ++i )
    {
        blockP = winP->blocksPP[routineP->bodyIdsP[i]];

        for( linkP = winP->returnsPP[blockP->id]; linkP; linkP = linkP->nextP )
        {
            if( linkP->routineP == routineP )
            {
                leave |= winP->fwordP[blockP->lastP->index];
            }
        }
    }

    return(leave);
}

// The state a call block's return edge carries.  An assumed return edge
// carries an unknown state: the callee may return elsewhere.
// Returns the state.
static unsigned int
returnState(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP, unsigned int state)
{
OptRoutineP routineP;

    if( !state )
    {
        return(0);
    }

    if( edgeP->assumed )
    {
        return( WB(W_RETURN) );
    }

    if( !(routineP = calleeOf(winP, blockP->lastP)) )
    {
        return( WB(W_CALLEE) );
    }

    return( routineExit(winP, routineP, state) );
}

// One sweep over every routine with summaries, each to its own fixpoint from
// its entry state ENTRY.  Returns 1 if a summary grew, 0 if none did.
static int
fwdSummaries(OptWindowP winP)
{
OptRoutineP routineP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
unsigned int leave;
unsigned int state;
unsigned int out;
unsigned int carried;
int changed;
int grew;
int r;
int i;
int id;

    changed = 0;

    for( r = 1; r <= winP->routineCount; ++r )
    {
        if( !(routineP = winP->routinesPP[r]) || !winP->closedP[r] )
        {
            continue;
        }

        for( i = 0; i < routineP->blockCount; ++i )
        {
            id = routineP->bodyIdsP[i];
            winP->linP[id] = 0;
            winP->stampP[id] = r;
        }

        winP->linP[routineP->entryBlockP->id] = F_ENTRY;
        leave = 0;

        do
        {
            grew = 0;

            for( i = 0; i < routineP->blockCount; ++i )
            {
                blockP = winP->blocksPP[routineP->bodyIdsP[i]];

                if( !(state = winP->linP[blockP->id]) )
                {
                    continue;
                }

                out = fwdBlock(winP, blockP, state, 0);

                for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
                {
                    switch( edgeP->kind )
                    {
                    case OPTFK_CALL:
                        continue;

                    case OPTFK_RESUME:
                        carried = WB(W_HALT);
                        break;

                    case OPTFK_RETURN:
                        carried = returnState(winP, blockP, edgeP, out);
                        break;

                    default:
                        carried = out;
                        break;
                    }

                    if( !edgeP->toP )
                    {
                        if( edgeP->kind == OPTFK_RESUME )
                        {
                            continue;
                        }

                        if( (edgeP->kind != OPTFK_RETURN) && isReturnOf(routineP, blockP->lastP) )
                        {
                            leave |= out;
                        }
                        else
                        {
                            leave |= (edgeP->sink == OPTSK_DEBREAK)?WB(W_SBS):WB(W_LOST);
                        }

                        continue;
                    }

                    if( winP->stampP[edgeP->toP->id] != r )
                    {
                        leave |= WB(W_SHAPE);
                        continue;
                    }

                    if( (winP->linP[edgeP->toP->id] | carried) != winP->linP[edgeP->toP->id] )
                    {
                        winP->linP[edgeP->toP->id] |= carried;
                        grew = 1;
                    }
                }
            }
        } while( grew );

        for( i = 0; i < routineP->blockCount; ++i )
        {
            winP->stampP[routineP->bodyIdsP[i]] = 0;
        }

        if( (winP->fsumP[r] | leave) != winP->fsumP[r] )
        {
            winP->fsumP[r] |= leave;
            changed = 1;
        }
    }

    return(changed);
}

// One sweep over every block of the program, recording each word's state.
// Returns 1 if a block's state grew, 0 if none did.
static int
fwdGlobal(OptWindowP winP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
unsigned int state;
unsigned int out;
unsigned int carried;
int changed;

    changed = 0;

    for( blockP = winP->tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !(state = winP->finP[blockP->id]) )
        {
            continue;
        }

        out = fwdBlock(winP, blockP, state, 1);

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( !edgeP->toP )
            {
                continue;
            }

            switch( edgeP->kind )
            {
            case OPTFK_RESUME:
                carried = WB(W_HALT);
                break;

            case OPTFK_RETURN:
                carried = returnState(winP, blockP, edgeP, out);
                break;

            default:
                carried = out;
                break;
            }

            if( (winP->finP[edgeP->toP->id] | carried) != winP->finP[edgeP->toP->id] )
            {
                winP->finP[edgeP->toP->id] |= carried;
                changed = 1;
            }
        }
    }

    return(changed);
}

// The forward pass to its fixpoint, with the words noopP marks deleted in
// thought (NILP for none).  Leaves each word's state in fwordP.
static void
forward(OptWindowP winP, const unsigned char *noopP)
{
OptBlockP blockP;
int changed;

    winP->noopP = noopP;
    memset(winP->finP, 0, (size_t)(winP->blockCount + 1) * sizeof(unsigned int));
    memset(winP->fsumP, 0, (size_t)(winP->routineCount + 1) * sizeof(unsigned int));
    memset(winP->fwordP, 0, (size_t)winP->tableP->count * sizeof(unsigned int));

    for( blockP = winP->tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->isEntry )
        {
            winP->finP[blockP->id] = entryUnknown(winP, blockP->firstP);
        }
    }

    do
    {
        changed = fwdSummaries(winP);
        changed |= fwdGlobal(winP);
    } while( changed );

    winP->noopP = NILP;
}

// The backward pass.

// What depends on the window at one word, other than through a call's callee.
// Returns the dependence bits.
static unsigned int
useOf(OptWindowP winP, OptWordP wordP)
{
OptWordP pointerP;
OptRoutineP routineP;
unsigned int use;
int value;

    use = 0;

    if( isOtherDevice(wordP) )
    {
        return( WB(W_IOT) );
    }

    if( isXct(wordP) )
    {
        use |= WB(W_XCT);
    }

    if( (wordP->decode.group == OPTG_OPERATE) && (wordP->decode.microBits & OPTM_LAP) )
    {
        use |= WB(W_FLAG);
    }

    switch( pointerClass(winP, wordP, &pointerP) )
    {
    case PC_NONE:
        break;

    case PC_CONST:
        // In bank 0 a twelve-bit pointer reaches the same word open or closed.
        value = (pointerP->value & 0177777);

        if( (wordP->bank != 0) || (value & ~ADDRMASK) )
        {
            use |= L_PROVED;
        }

        break;

    case PC_DEBREAK:
        use |= WB(W_SBS);
        break;

    default:
        use |= WB(W_POINTER);
        break;
    }

    if( wordP->isCallSite )
    {
        if( !(routineP = calleeOf(winP, wordP)) )
        {
            use |= WB(W_CALLEE);
        }
        else if( !dapClean(winP, routineP) )
        {
            use |= WB(W_FLAG);
        }
    }

    return(use);
}

// Walk a block's words backward from what depends on the window after it.
// An eem or lem ends every dependence, unless it is transparent; with record
// set each lem's dependence after it is recorded.  Returns the dependence
// before the first word.
static unsigned int
backBlock(OptWindowP winP, OptBlockP blockP, unsigned int live, int record)
{
OptWordP wordP;
int addr;

    for( addr = blockP->endAddr; addr >= blockP->startAddr; --addr )
    {
        if( !(wordP = wordAt(winP->tableP, blockP->bank, addr)) )
        {
            continue;
        }

        if( (isEem(wordP) || isLem(wordP)) && !winP->transparentP[wordP->index] )
        {
            if( record )
            {
                winP->lafterP[wordP->index] |= live;
            }

            live = 0;
            continue;
        }

        live |= useOf(winP, wordP);
    }

    return(live);
}

// A backward summary applied to what depends on the window after a call returns.
// Returns what depends on it before the callee's entry.
static unsigned int
backApply(unsigned int summary, unsigned int live)
{
    return( (summary & ~L_EXIT) | ((summary & L_EXIT)?live:0) );
}

// What a call edge makes depend on the window: the callee, and after it the
// return target.  localP, when set, holds a summary walk's own dependences for
// the body of routineP; otherwise the program's are used.
// Returns the dependence bits.
static unsigned int
callLive(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP, unsigned int *localP, OptRoutineP routineP)
{
OptFlowEdgeP returnP;
OptRoutineP calleeP;
unsigned int after;

    after = 0;

    if( (returnP = returnEdgeOf(blockP)) )
    {
        if( !returnP->toP )
        {
            after = WB(W_LOST);
        }
        else if( localP )
        {
            after = (winP->stampP[returnP->toP->id] == routineP->id)?localP[returnP->toP->id]:WB(W_SHAPE);
        }
        else
        {
            after = winP->binP[returnP->toP->id];
        }
    }

    if( !edgeP->toP )
    {
        return( WB(W_CALLEE) | after );
    }

    if( (calleeP = winP->entryOfPP[edgeP->toP->id]) && winP->closedP[calleeP->id] )
    {
        return( backApply(winP->bsumP[calleeP->id], after) );
    }

    return( winP->binP[edgeP->toP->id] | after );
}

// What depends on the window after a routine returns: whatever depends on it
// at every caller's return target, and unknown when an indirect call the
// graph could not follow may have entered it.  Returns the dependence bits.
static unsigned int
returnLive(OptWindowP winP, OptRoutineP routineP)
{
WinLinkP linkP;
OptFlowEdgeP returnP;
OptWordP entryP;
unsigned int live;

    live = 0;

    for( linkP = winP->callersPP[routineP->id]; linkP; linkP = linkP->nextP )
    {
        if( !(returnP = returnEdgeOf(linkP->blockP)) )
        {
            continue;
        }

        live |= (returnP->toP)?winP->binP[returnP->toP->id]:WB(W_LOST);
    }

    entryP = wordAt(winP->tableP, routineP->bank, routineP->entryAddr);

    if( entryP && (entryP->flags & OPTF_MAYBE_ENTERED) )
    {
        live |= WB(W_CALLEE);
    }

    return(live);
}

// What depends on the window after a sink edge that is not a call: a return,
// the end of a sequence-break handler, or control lost.
// Returns the dependence bits.
static unsigned int
sinkLive(OptWindowP winP, OptBlockP blockP, OptFlowEdgeP edgeP)
{
WinLinkP linkP;
unsigned int live;

    if( edgeP->sink == OPTSK_DEBREAK )
    {
        return( WB(W_SBS) );
    }

    if( !(linkP = winP->returnsPP[blockP->id]) )
    {
        return( WB(W_LOST) );
    }

    live = 0;

    for( ; linkP; linkP = linkP->nextP )
    {
        live |= returnLive(winP, linkP->routineP);
    }

    return(live);
}

// One sweep over every routine with summaries, each to its own fixpoint, with
// EXIT standing for what depends on the window after its return.
// Returns 1 if a summary grew, 0 if none did.
static int
backSummaries(OptWindowP winP)
{
OptRoutineP routineP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
unsigned int live;
unsigned int in;
int changed;
int grew;
int r;
int i;
int id;

    changed = 0;

    for( r = 1; r <= winP->routineCount; ++r )
    {
        if( !(routineP = winP->routinesPP[r]) || !winP->closedP[r] )
        {
            continue;
        }

        for( i = 0; i < routineP->blockCount; ++i )
        {
            id = routineP->bodyIdsP[i];
            winP->linP[id] = 0;
            winP->stampP[id] = r;
        }

        do
        {
            grew = 0;

            for( i = routineP->blockCount - 1; i >= 0; --i )
            {
                blockP = winP->blocksPP[routineP->bodyIdsP[i]];
                live = 0;

                for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
                {
                    if( edgeP->kind == OPTFK_RETURN )
                    {
                        continue;
                    }

                    if( edgeP->kind == OPTFK_CALL )
                    {
                        live |= callLive(winP, blockP, edgeP, winP->linP, routineP);
                        continue;
                    }

                    if( !edgeP->toP )
                    {
                        if( edgeP->kind == OPTFK_RESUME )
                        {
                            continue;
                        }

                        if( isReturnOf(routineP, blockP->lastP) )
                        {
                            live |= L_EXIT;
                        }
                        else
                        {
                            live |= (edgeP->sink == OPTSK_DEBREAK)?WB(W_SBS):WB(W_LOST);
                        }

                        continue;
                    }

                    if( winP->stampP[edgeP->toP->id] != r )
                    {
                        live |= WB(W_SHAPE);
                        continue;
                    }

                    live |= winP->linP[edgeP->toP->id];
                }

                in = backBlock(winP, blockP, live, 0);

                if( (winP->linP[blockP->id] | in) != winP->linP[blockP->id] )
                {
                    winP->linP[blockP->id] |= in;
                    grew = 1;
                }
            }
        } while( grew );

        in = winP->linP[routineP->entryBlockP->id];

        for( i = 0; i < routineP->blockCount; ++i )
        {
            winP->stampP[routineP->bodyIdsP[i]] = 0;
        }

        if( (winP->bsumP[r] | in) != winP->bsumP[r] )
        {
            winP->bsumP[r] |= in;
            changed = 1;
        }
    }

    return(changed);
}

// One sweep over every block, last first, recording each lem's dependence.
// Returns 1 if a block's dependence grew, 0 if none did.
static int
backGlobal(OptWindowP winP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
unsigned int live;
unsigned int in;
int changed;
int id;

    changed = 0;

    for( id = winP->blockCount; id >= 1; --id )
    {
        if( !(blockP = winP->blocksPP[id]) )
        {
            continue;
        }

        live = 0;

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( edgeP->kind == OPTFK_RETURN )
            {
                continue;
            }

            if( edgeP->kind == OPTFK_CALL )
            {
                live |= callLive(winP, blockP, edgeP, NILP, NILP);
                continue;
            }

            if( !edgeP->toP )
            {
                if( edgeP->kind != OPTFK_RESUME )
                {
                    live |= sinkLive(winP, blockP, edgeP);
                }

                continue;
            }

            live |= winP->binP[edgeP->toP->id];
        }

        in = backBlock(winP, blockP, live, 1);

        if( (winP->binP[id] | in) != winP->binP[id] )
        {
            winP->binP[id] |= in;
            changed = 1;
        }
    }

    return(changed);
}

// The backward pass to its fixpoint.  Leaves each lem's dependence in lafterP.
static void
backward(OptWindowP winP)
{
int changed;

    do
    {
        changed = backSummaries(winP);
        changed |= backGlobal(winP);
    } while( changed );
}

// Verdicts.

// Whether deleting a word could not be shown safe whatever the state.
// Returns the reason, or -1 when none applies.
static int
gateWhy(OptWindowP winP, OptWordP wordP)
{
    if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN) )
    {
        return(W_PATCHED);
    }

    if( wordP->flags & (OPTF_XCTTARGET | OPTF_XCTONLY) )
    {
        return(W_XCT);
    }

    if( winP->sbs )
    {
        return(W_SBS);
    }

    if( optGuessWordBits(winP->tableP, wordP) & ((1u << OPTGH_H2) | (1u << OPTGH_H3)) )
    {
        return(W_TIMING);
    }

    if( wordP->flags & OPTF_AFTERSKIP )
    {
        return(W_SKIP);
    }

    return(-1);
}

// Classify every eem and lem in the graph: the forward pass as written, the
// backward pass with the redundant words deleted in thought, then the forward
// pass again without the dead lems for the eems they free.
static void
classify(OptWindowP winP)
{
OptTableP tableP;
OptBlockP blockP;
OptWordP wordP;
unsigned char *noopP;
unsigned int state;
unsigned int live;
int addr;
int gate;

    tableP = winP->tableP;
    forward(winP, NILP);
    memcpy(winP->stateP, winP->fwordP, (size_t)tableP->count * sizeof(unsigned int));

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || (!isEem(wordP) && !isLem(wordP)) )
            {
                continue;
            }

            winP->present = 1;
            state = winP->stateP[wordP->index];

            if( !state )
            {
                winP->classP[wordP->index] = WC_UNREACHED;
                continue;
            }

            winP->active = 1;

            if( state == (isEem(wordP)?F_ON:F_OFF) )
            {
                if( (gate = gateWhy(winP, wordP)) >= 0 )
                {
                    winP->classP[wordP->index] = WC_REFUSED;
                    winP->whyP[wordP->index] = (unsigned char)gate;
                }
                else
                {
                    winP->classP[wordP->index] = WC_REDUNDANT;
                    winP->transparentP[wordP->index] = 1;
                }

                continue;
            }

            if( isEem(wordP) )
            {
                if( state & WB_MASK )
                {
                    winP->classP[wordP->index] = WC_REFUSED;
                    winP->whyP[wordP->index] = (unsigned char)lowestWhy(state);
                }
                else
                {
                    winP->classP[wordP->index] = WC_NEEDED;
                }
            }
        }
    }

    // With none reached, the unreached ones are still counted.
    if( !winP->active )
    {
        countClasses(winP);
        return;
    }

    backward(winP);

    // A lem is dead when nothing depends on the window after it; needed on a
    // proved dependence; otherwise refused for the first unknown one.
    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || !isLem(wordP) ||
                (winP->classP[wordP->index] != WC_NONE) )
            {
                continue;
            }

            live = winP->lafterP[wordP->index];

            if( live & L_PROVED )
            {
                winP->classP[wordP->index] = WC_NEEDED;
            }
            else if( live & WB_MASK )
            {
                winP->classP[wordP->index] = WC_REFUSED;
                winP->whyP[wordP->index] = (unsigned char)lowestWhy(live);
            }
            else if( (gate = gateWhy(winP, wordP)) >= 0 )
            {
                winP->classP[wordP->index] = WC_REFUSED;
                winP->whyP[wordP->index] = (unsigned char)gate;
            }
            else
            {
                winP->classP[wordP->index] = WC_DEAD;
            }
        }
    }

    // The eems freed: redundant once the redundant words and the dead lems
    // are all deleted.
    noopP = winAlloc((size_t)tableP->count, 1);

    for( addr = 0; addr < tableP->count; ++addr )
    {
        noopP[addr] = (unsigned char)((winP->classP[addr] == WC_REDUNDANT) || (winP->classP[addr] == WC_DEAD));
    }

    forward(winP, noopP);

    for( addr = 0; addr < tableP->count; ++addr )
    {
        wordP = tableP->entriesPP[addr];

        if( (winP->classP[addr] == WC_NEEDED) && isEem(wordP) && (winP->fwordP[addr] == F_ON) &&
            (gateWhy(winP, wordP) < 0) )
        {
            winP->classP[addr] = WC_FREED;
        }
    }

    free(noopP);
    countClasses(winP);
}

// Count the eems and lems by class, and the refusals by reason.
static void
countClasses(OptWindowP winP)
{
OptTableP tableP;
OptWordP wordP;
int addr;
int why;

    tableP = winP->tableP;

    for( addr = 0; addr < tableP->count; ++addr )
    {
        wordP = tableP->entriesPP[addr];

        if( winP->classP[addr] == WC_NONE )
        {
            continue;
        }

        if( isEem(wordP) )
        {
            ++winP->eemCounts[winP->classP[addr]];
        }
        else
        {
            ++winP->lemCounts[winP->classP[addr]];
        }

        if( winP->classP[addr] == WC_REFUSED )
        {
            why = winP->whyP[addr];
            ++winP->refusedWhy[why];
        }
    }
}

// Mark the indirect references in one unreached block, from an address on.
static void
markUnexamined(OptWindowP winP, OptBlockP blockP, int fromAddr)
{
OptWordP wordP;
OptWordP pointerP;
WinPointer pc;
int addr;

    for( addr = fromAddr; addr <= blockP->endAddr; ++addr )
    {
        if( !(wordP = wordAt(winP->tableP, blockP->bank, addr)) || winP->stateP[wordP->index] ||
            winP->unexaminedP[wordP->index] )
        {
            continue;
        }

        pc = pointerClass(winP, wordP, &pointerP);

        if( (pc != PC_NONE) && (pc != PC_DEBREAK) )
        {
            winP->unexaminedP[wordP->index] = 1;
            ++winP->unexaminedCount;
        }
    }
}

// Mark the indirect references an unreached eem or lem guards: those its flow
// arrives at before it joins code the forward pass reached.  No hazard or
// reverse hazard is decided for them, and a dump that passed over them would
// read as a clean one.  A jump written at run time is the usual way in, and
// what the target of such a jump holds the analysis cannot know.
static void
findUnexamined(OptWindowP winP)
{
OptTableP tableP;
OptBlockP blockP;
OptBlockP *stackPP;
OptFlowEdgeP edgeP;
OptWordP wordP;
unsigned char *seenP;
int depth;
int addr;

    tableP = winP->tableP;
    stackPP = winAlloc((size_t)tableP->blockCount + 1, sizeof(OptBlockP));
    seenP = winAlloc((size_t)tableP->blockCount + 1, 1);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || (winP->classP[wordP->index] != WC_UNREACHED) )
            {
                continue;
            }

            // The words before it in its block are not guarded by it, so the
            // block is marked from here and not counted as seen: an edge back
            // to its start marks the rest.
            markUnexamined(winP, blockP, addr);
            stackPP[0] = blockP;
            depth = 1;

            while( depth > 0 )
            {
                for( edgeP = stackPP[--depth]->succP; edgeP; edgeP = edgeP->nextP )
                {
                    if( !edgeP->toP || seenP[edgeP->toP->id] || winP->stateP[edgeP->toP->firstP->index] )
                    {
                        continue;
                    }

                    seenP[edgeP->toP->id] = 1;
                    markUnexamined(winP, edgeP->toP, edgeP->toP->startAddr);
                    stackPP[depth++] = edgeP->toP;
                }
            }
        }
    }

    free(stackPP);
    free(seenP);
}

// What an expression's address leaves are, as LK_ bits: a label or pool
// reference written bare, and one qualified with a bank.
// Returns the bits, 0 for an expression of numbers alone.
static int
leafKinds(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case BINOP:
        return( leafKinds(nodeP->leftP) | leafKinds(nodeP->rightP) );

    case UNOP:
        return( leafKinds(nodeP->rightP) );

    case ADDR:
    case LCLADDR:
    case CONSTANT:
        return(LK_BARE);

    case BREF:
    case WILDREF:
        return(LK_QUALIFIED);

    default:
        return(0);
    }
}

// Find the indirect references made while the window may be open through a
// pointer not proved to hold a 16-bit address.  Only a bank other than 0 has
// them: in bank 0 a twelve-bit address reaches the same word either way.
static void
findHazards(OptWindowP winP)
{
OptTableP tableP;
OptBlockP blockP;
OptWordP wordP;
OptWordP pointerP;
unsigned int state;
int addr;
int value;

    tableP = winP->tableP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockP->bank == 0 )
        {
            continue;
        }

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || !(state = winP->stateP[wordP->index]) )
            {
                continue;
            }

            switch( pointerClass(winP, wordP, &pointerP) )
            {
            case PC_CONST:
                // A label written bare names the current bank only while the
                // window is closed.  A sym:N or sym:* qualifier names its bank,
                // and a plain number is a 16-bit address as written: farjmp(0)
                // means bank 0, address 0.
                value = (pointerP->value & 0177777);

                if( (value & ~ADDRMASK) || (leafKinds(pointerP->exprP) != LK_BARE) )
                {
                    break;
                }

                if( state & F_ON )
                {
                    winP->hazardP[wordP->index] = WH_PROVED;
                }
                else if( state & WB_MASK )
                {
                    winP->hazardP[wordP->index] = WH_POSSIBLE;
                    winP->hazardWhyP[wordP->index] = (unsigned char)lowestWhy(state);
                }

                break;

            case PC_RUNTIME:
                if( state & F_ON )
                {
                    winP->hazardP[wordP->index] = WH_UNPROVED;
                }
                else if( state & WB_MASK )
                {
                    ++winP->unknownRuntime;
                }

                break;

            default:
                break;
            }

            ++winP->hazardCounts[winP->hazardP[wordP->index]];
        }
    }
}

// Test whether a 16-bit address, read through with the window closed from
// code in a bank, misses its word.  Closed, the reference stays in the
// current bank, and 010000 (every odd bank) makes it chain.
// Returns 1 if it misses, 0 if it reaches the word it names.
static int
closedMisreads(int value, int bank)
{
    return( (value & 010000) || (((value >> 12) & 017) != bank) );
}

// Test whether a dac-form routine's return word, used with the window closed
// from code in a bank, misreads the address some known caller saved there.  A
// jsp leaves the caller's whole 16-bit return address, whatever the window's
// state.  The state at the use is a union over every caller, so a caller that
// misreads proves nothing unless its own call may be made closed: a call
// from another bank is made open, and reaches the use open unless the routine
// closes the window first.
// Returns WH_PROVED when a misreading caller's call may be made closed,
// WH_POSSIBLE when misreading callers exist but none may, and WH_NONE
// otherwise; *callerBankP is set to that caller's bank.
static WinHazard
returnMisread(OptWindowP winP, OptWordP pointerP, int bank, int *callerBankP)
{
OptRoutineP routineP;
WinLinkP linkP;
WinHazard found;

    found = WH_NONE;

    if( !(routineP = winP->returnWordOfPP[pointerP->index]) )
    {
        return(found);
    }

    for( linkP = winP->callersPP[routineP->id]; linkP; linkP = linkP->nextP )
    {
        if( !closedMisreads(linkP->blockP->bank << 12, bank) )
        {
            continue;
        }

        if( winP->stateP[linkP->blockP->lastP->index] & F_OFF )
        {
            *callerBankP = linkP->blockP->bank;
            return(WH_PROVED);
        }

        if( found == WH_NONE )
        {
            *callerBankP = linkP->blockP->bank;
            found = WH_POSSIBLE;
        }
    }

    return(found);
}

// Test whether a pointer written at run time is written, anywhere, from a
// constant meant as a 16-bit address that the closed window misreads from
// code in a bank: a direct dac preceded by a direct lac of a word never
// written, as in "lac [w:1]; dac p".  Any other writer says nothing.
// Returns 1 if some writer is, 0 if none is.
static int
writtenWide(OptWindowP winP, OptWordP pointerP, int bank)
{
OptEdgeP edgeP;
OptWordP fromP;
OptWordP loadP;
OptWordP sourceP;
int value;

    for( edgeP = pointerP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        fromP = edgeP->fromP;

        if( (edgeP->role != OPTR_WRITE) || (edgeP->flags & OPTEF_INDIRECT) ||
            (fromP->decode.group != OPTG_MEMREF) || (fromP->decode.opcode != WOP_DAC) ||
            fromP->decode.memIndirect || !(loadP = wordAt(winP->tableP, fromP->bank, fromP->addr - 1)) ||
            (loadP->decode.group != OPTG_MEMREF) || (loadP->decode.opcode != WOP_LAC) ||
            loadP->decode.memIndirect ||
            !(sourceP = wordAt(winP->tableP, loadP->bank, loadP->decode.address)) ||
            (sourceP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN | OPTF_RESERVED | OPTF_DUPADDR)) )
        {
            continue;
        }

        value = (sourceP->value & 0177777);

        if( ((leafKinds(sourceP->exprP) & LK_QUALIFIED) || (value & ~ADDRMASK)) && closedMisreads(value, bank) )
        {
            return(1);
        }
    }

    return(0);
}

// Find the reverse hazards: indirect references made while the window may be
// closed through a pointer meant as a 16-bit address, which the closed
// window reads as some other word.  A pointer is meant as one when a bank
// qualifier is among its leaves or its value is above 07777; a plain number
// no larger is taken as twelve bits here, where findHazards() takes it as
// sixteen, since a closed-window reading of it is the ordinary use.  Every bank has
// them, bank 0 included.
static void
findReverse(OptWindowP winP)
{
OptTableP tableP;
OptBlockP blockP;
OptWordP wordP;
OptWordP pointerP;
unsigned int state;
int addr;
int value;
int callerBank = 0;
int misread;
WinHazard viaReturn;

    tableP = winP->tableP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) || !(state = winP->stateP[wordP->index]) )
            {
                continue;
            }

            misread = 0;

            switch( pointerClass(winP, wordP, &pointerP) )
            {
            case PC_CONST:
                value = (pointerP->value & 0177777);
                misread = ((leafKinds(pointerP->exprP) & LK_QUALIFIED) || (value & ~ADDRMASK)) &&
                    closedMisreads(value, wordP->bank);
                break;

            case PC_RETURN:
                viaReturn = returnMisread(winP, pointerP, wordP->bank, &callerBank);
                misread = (viaReturn == WH_PROVED);

                // Closed here only on the paths from callers it reads right.
                if( (viaReturn == WH_POSSIBLE) && (state & F_OFF) )
                {
                    winP->reverseP[wordP->index] = WH_POSSIBLE;
                    winP->reverseWhyP[wordP->index] = W_CALLERS;
                }
                else if( viaReturn == WH_POSSIBLE )
                {
                    misread = 1;
                }

                break;

            case PC_RUNTIME:
                if( pointerP && (state & F_OFF) && writtenWide(winP, pointerP, wordP->bank) )
                {
                    winP->reverseP[wordP->index] = WH_UNPROVED;
                }

                break;

            default:
                break;
            }

            if( misread )
            {
                if( state & F_OFF )
                {
                    winP->reverseP[wordP->index] = WH_PROVED;
                }
                else if( state & WB_MASK )
                {
                    winP->reverseP[wordP->index] = WH_POSSIBLE;
                    winP->reverseWhyP[wordP->index] = (unsigned char)lowestWhy(state);
                }
            }

            ++winP->reverseCounts[winP->reverseP[wordP->index]];
        }
    }
}

// Warn on stderr about each proved hazard and each proved reverse hazard, on
// every -O run, since each is a possible live bug and not a saving.
static void
warnHazards(OptWindowP winP)
{
OptTableP tableP;
OptWordP wordP;
OptWordP pointerP;
int i;
int callerBank = 0;

    tableP = winP->tableP;

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( winP->hazardP[i] != WH_PROVED )
        {
            continue;
        }

        pointerClass(winP, wordP, &pointerP);
        fprintf(stderr, "am1: warning: extend window: %s line %d: bank %d %04o indirects through the bare address "
            "at bank %d %04o with the window possibly open, which reaches bank 0\n",
            wordFile(wordP), wordP->lineNo, wordP->bank, wordP->addr, pointerP->bank, pointerP->addr);
    }

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( winP->reverseP[i] != WH_PROVED )
        {
            continue;
        }

        if( pointerClass(winP, wordP, &pointerP) == PC_RETURN )
        {
            returnMisread(winP, pointerP, wordP->bank, &callerBank);
            fprintf(stderr, "am1: warning: extend window: %s line %d: bank %d %04o indirects through the return word "
                "at bank %d %04o with the window possibly closed, which misreads the address a call from bank %d "
                "saves there\n", wordFile(wordP), wordP->lineNo, wordP->bank, wordP->addr, pointerP->bank,
                pointerP->addr, callerBank);
        }
        else if( pointerP->value & 010000 )
        {
            fprintf(stderr, "am1: warning: extend window: %s line %d: bank %d %04o indirects through the 16-bit "
                "address at bank %d %04o with the window possibly closed, which chains through bank %d instead of "
                "reaching bank %d\n", wordFile(wordP), wordP->lineNo, wordP->bank, wordP->addr, pointerP->bank,
                pointerP->addr, wordP->bank, (pointerP->value >> 12) & 017);
        }
        else
        {
            fprintf(stderr, "am1: warning: extend window: %s line %d: bank %d %04o indirects through the 16-bit "
                "address at bank %d %04o with the window possibly closed, which reads bank %d instead of bank %d\n",
                wordFile(wordP), wordP->lineNo, wordP->bank, wordP->addr, pointerP->bank, pointerP->addr,
                wordP->bank, (pointerP->value >> 12) & 017);
        }
    }
}

// A word's source file for a message, or "-".
static const char *
wordFile(OptWordP wordP)
{
    return( (wordP->fileP)?wordP->fileP:"-" );
}

// Run the analysis over a table whose call graph and guess are built, and
// warn about the proved hazards.  Keeps its verdicts on the table.
void
optBuildWindow(OptTableP tableP)
{
OptWindowP winP;
size_t words;
size_t blocks;
size_t routines;

    winP = winAlloc(1, sizeof(OptWindow));
    tableP->windowP = winP;
    winP->tableP = tableP;
    winP->blockCount = tableP->blockCount;
    winP->routineCount = tableP->routineCount;
    winP->sbs = (optSbsChannels(tableP) > 0);

    words = (size_t)tableP->count;
    blocks = (size_t)tableP->blockCount + 1;
    routines = (size_t)tableP->routineCount + 1;

    winP->blocksPP = optBlockIndex(tableP);
    winP->routinesPP = winAlloc(routines, sizeof(OptRoutineP));
    winP->entryOfPP = winAlloc(blocks, sizeof(OptRoutineP));
    winP->closedP = winAlloc(routines, 1);
    winP->returnCountP = winAlloc(routines, sizeof(int));
    winP->returnsPP = winAlloc(blocks, sizeof(WinLinkP));
    winP->callersPP = winAlloc(routines, sizeof(WinLinkP));
    winP->returnWordOfPP = winAlloc(words, sizeof(OptRoutineP));
    winP->stampP = winAlloc(blocks, sizeof(int));
    winP->finP = winAlloc(blocks, sizeof(unsigned int));
    winP->fsumP = winAlloc(routines, sizeof(unsigned int));
    winP->fwordP = winAlloc(words, sizeof(unsigned int));
    winP->linP = winAlloc(blocks, sizeof(unsigned int));
    winP->transparentP = winAlloc(words, 1);
    winP->binP = winAlloc(blocks, sizeof(unsigned int));
    winP->bsumP = winAlloc(routines, sizeof(unsigned int));
    winP->lafterP = winAlloc(words, sizeof(unsigned int));
    winP->stateP = winAlloc(words, sizeof(unsigned int));
    winP->classP = winAlloc(words, 1);
    winP->whyP = winAlloc(words, 1);
    winP->hazardP = winAlloc(words, 1);
    winP->hazardWhyP = winAlloc(words, 1);
    winP->reverseP = winAlloc(words, 1);
    winP->reverseWhyP = winAlloc(words, 1);
    winP->unexaminedP = winAlloc(words, 1);

    buildMaps(winP);
    classify(winP);
    findUnexamined(winP);

    if( !winP->active )
    {
        return;
    }

    findHazards(winP);
    findReverse(winP);
    warnHazards(winP);
}

// The deletions.

// Name a deletion's kind, "dead lem".  Returns a static string, never NILP.
const char *
optWinDelKindName(OptWinDelKind kind)
{
    if( (kind < 0) || (kind >= OPTWD_KIND_COUNT) )
    {
        return("?");
    }

    return( kindNames[kind] );
}

// Name a deletion's fate as one word, for the dump.
// Returns a static string, never NILP.
const char *
optWinDelFateName(OptWinDelFate fate)
{
    if( (fate < 0) || (fate >= OPTWD_COUNT) )
    {
        return("?");
    }

    return( winDelFates[fate].tagP );
}

// Record every redundant, dead or freed word in an optimize or speed region,
// or anywhere under -O2 with -O=undeclared, find what each freed eem depends
// on, and decide every fate.  The redundant and dead words come first, so each
// freed eem's fate can read its lems'.  Only when the edits are made under a
// level; a source that declares no region, built without the switch, gets no
// record.
void
optPlanWindow(OptTableP tableP)
{
OptWindowP winP;
OptBlockP blockP;
OptWordP wordP;
OptWinDelP delP;
int pass;
int addr;
int c;

    if( !(winP = tableP->windowP) || !winP->active )
    {
        return;
    }

    for( pass = 0; pass < 2; ++pass )
    {
        for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
        {
            for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
            {
                if( !(wordP = wordAt(tableP, blockP->bank, addr)) ||
                    (!(wordP->flags & (OPTF_INREGION | OPTF_INSPEED)) && !optUndeclared()) )
                {
                    continue;
                }

                c = winP->classP[wordP->index];

                if( pass == 0 )
                {
                    if( c == WC_REDUNDANT )
                    {
                        addWinDel(tableP, wordP, (isEem(wordP))?OPTWD_REDUNDANT_EEM:OPTWD_REDUNDANT_LEM);
                    }
                    else if( c == WC_DEAD )
                    {
                        addWinDel(tableP, wordP, OPTWD_DEAD_LEM);
                    }
                }
                else if( c == WC_FREED )
                {
                    addWinDel(tableP, wordP, OPTWD_FREED_EEM);
                }
            }
        }
    }

    if( !tableP->windelsP )
    {
        return;
    }

    findDeps(winP);

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        delP->fate = winDelGate(tableP, delP);
        ++tableP->windelFates[delP->fate];
    }
}

// Append a record, keeping the list in the order made.
// Returns the record.
static OptWinDelP
addWinDel(OptTableP tableP, OptWordP wordP, OptWinDelKind kind)
{
OptWinDelP delP;

    delP = winAlloc(1, sizeof(OptWinDel));
    delP->wordP = wordP;
    delP->kind = kind;
    delP->outcome = -1;

    if( tableP->windelsTailP )
    {
        tableP->windelsTailP->nextP = delP;
    }
    else
    {
        tableP->windelsP = delP;
    }

    tableP->windelsTailP = delP;
    ++tableP->windelCount;
    return(delP);
}

// Find the lems each recorded freed eem depends on: put back, one at a time,
// every redundant or dead lem in the program, declared or not, and run the
// forward pass with the rest deleted in thought.  A lem that leaves the eem's
// state other than open is one it depends on.
static void
findDeps(OptWindowP winP)
{
OptTableP tableP;
OptWinDelP delP;
OptWordP lemP;
unsigned char *noopP;
int freed;
int i;

    tableP = winP->tableP;

    for( freed = 0, delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        freed += (delP->kind == OPTWD_FREED_EEM);
    }

    if( !freed )
    {
        return;
    }

    noopP = winAlloc((size_t)tableP->count, 1);

    for( i = 0; i < tableP->count; ++i )
    {
        noopP[i] = (unsigned char)((winP->classP[i] == WC_REDUNDANT) || (winP->classP[i] == WC_DEAD));
    }

    for( i = 0; i < tableP->count; ++i )
    {
        lemP = tableP->entriesPP[i];

        if( !noopP[i] || !isLem(lemP) )
        {
            continue;
        }

        noopP[i] = 0;
        forward(winP, noopP);
        noopP[i] = 1;

        for( delP = tableP->windelsP; delP; delP = delP->nextP )
        {
            if( (delP->kind != OPTWD_FREED_EEM) || (winP->fwordP[delP->wordP->index] == F_ON) )
            {
                continue;
            }

            if( !(delP->depsPP = (OptWordP *)realloc(delP->depsPP, (size_t)(delP->depCount + 1) * sizeof(OptWordP))) )
            {
                fprintf(stderr, "am1: optimizer: out of memory in the window analysis\n");
                exit(1);
            }

            delP->depsPP[delP->depCount++] = lemP;
        }
    }

    free(noopP);
}

// Decide one record: a nooptimize span, -O2's guess, a word read as data, a
// word relayout cannot delete, another rewrite's word, and for a freed eem a
// lem it depends on that is not deleted.  The timing loops (H2, H3) are
// already refused by the analysis, at every level.
// Returns OPTWD_FIRED or the refusing fate.
static OptWinDelFate
winDelGate(OptTableP tableP, OptWinDelP delP)
{
OptWordP wordP;
OptWinDelP depP;
unsigned int applies;
int i;

    wordP = delP->wordP;

    if( wordP->flags & OPTF_HANDSOFF )
    {
        return(OPTWD_HANDSOFF);
    }

    // A deletion moves every word after it, so H6 refuses at every level.
    if( optGuessWordBits(tableP, wordP) & OPTGH_LENGTH )
    {
        delP->assumed = (optGuessWordBits(tableP, wordP) & OPTGH_LENGTH);
        return(OPTWD_GUESSED);
    }

    // As for a space deletion: H1 alone in an optimize region, all five on a
    // word only a speed region declares.
    if( optTransformLevel() >= 2 )
    {
        applies = (wordP->flags & OPTF_INREGION)?OPTGH_INREGION:OPTGH_AUTHORIZED;

        if( optGuessWordBits(tableP, wordP) & applies )
        {
            delP->assumed = (optGuessWordBits(tableP, wordP) & applies);
            return(OPTWD_GUESSED);
        }
    }

    // A program that reads its own code, to copy or sum it, would read
    // something else.
    if( wordP->inCounts[OPTR_READ] )
    {
        return(OPTWD_USED);
    }

    if( !optWordRepresentable(wordP) || (wordP->nodeP->type == ORIGIN) )
    {
        return(OPTWD_UNREPRESENTABLE);
    }

    if( optWordTouched(tableP, wordP) )
    {
        return(OPTWD_OVERLAP);
    }

    for( i = 0; i < delP->depCount; ++i )
    {
        if( !(depP = winDelOf(tableP, delP->depsPP[i])) || (depP->fate != OPTWD_FIRED) )
        {
            return(OPTWD_DEPENDS);
        }
    }

    return(OPTWD_FIRED);
}

// The record for a word.  Returns it, or NILP when the word has none.
static OptWinDelP
winDelOf(OptTableP tableP, OptWordP wordP)
{
OptWinDelP delP;

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        if( delP->wordP == wordP )
        {
            return(delP);
        }
    }

    return(NILP);
}

// After bisection: a freed eem one of whose lems was switched off is switched
// off too, by the same modifiers, since deleting it alone would leave the
// window closed where the program had it open.
void
optWindowSettleOff(OptTableP tableP)
{
OptWinDelP delP;
OptWinDelP depP;
unsigned int bit;
int i;
int b;

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        if( delP->fate != OPTWD_FIRED )
        {
            continue;
        }

        for( i = 0; i < delP->depCount; ++i )
        {
            depP = winDelOf(tableP, delP->depsPP[i]);

            if( depP && (depP->fate == OPTWD_OFF) )
            {
                delP->fate = OPTWD_OFF;
                delP->offBy |= depP->offBy;
            }
        }

        if( delP->fate != OPTWD_OFF )
        {
            continue;
        }

        --tableP->windelFates[OPTWD_FIRED];
        ++tableP->windelFates[OPTWD_OFF];

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( delP->offBy & bit )
            {
                ++tableP->windelOffBy[b];
            }
        }
    }
}

// The report's section for the deletions, written after relayout: every
// licensed redundant, dead or freed word, its fate, and for each that fired
// whether relayout made it; one only the switch licensed is marked.  Nothing
// when no word was licensed.
void
writeWindowDeleteReport(FILE *fP, OptTableP tableP)
{
OptWinDelP delP;
OptWordP wordP;
int made;
int i;

    if( !tableP->windelsP )
    {
        return;
    }

    made = tableP->windelMade;

    fprintf(fP, "\nExtend window deletions (-O%d): %d %s, %d deleted\n",
        (optTransformLevel() >= 2)?2:1, tableP->windelCount, (optUndeclared())?"licensed":"declared", made);
    fprintf(fP, "  A redundant eem or lem, a dead lem and a freed eem are deleted where the word\n");

    if( optUndeclared() )
    {
        fprintf(fP, "  is in an optimize or speed region, or anywhere, since %s was given;\n",
            optUndeclaredSpelling());
        fprintf(fP, "  a word only the switch licensed is marked (undeclared).  A freed eem goes only\n");
        fprintf(fP, "  with every lem it depends on.  The deletions made the program %d word%s shorter\n",
            made, (made == 1)?"":"s");
        fprintf(fP, "  and save %d microseconds over one pass through each%s\n", 5 * made,
            (made)?";\n  every word after one has moved.":".");
    }
    else
    {
        fprintf(fP, "  is in an optimize or speed region.  A freed eem goes only with every lem it\n");
        fprintf(fP, "  depends on.  The deletions made the program %d word%s shorter and save %d\n",
            made, (made == 1)?"":"s", 5 * made);
        fprintf(fP, "  microseconds over one pass through each%s.\n", (made)?"; every word after one has moved":"");
    }

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        wordP = delP->wordP;
        fprintf(fP, "    %-13s bank %2d %04o  %s:%d  ", kindNames[delP->kind], wordP->bank, wordP->addr,
            wordFile(wordP), wordP->lineNo);

        if( delP->fate == OPTWD_OFF )
        {
            fprintf(fP, "not deleted: %s, rewrite #%d", winDelFates[delP->fate].textP, delP->ordinal);
        }
        else if( (delP->fate == OPTWD_GUESSED) && (optTransformLevel() < 2) )
        {
            // Below -O2 only H6 guesses.
            fprintf(fP, "not deleted: left alone on H6's guess");
        }
        else if( delP->fate != OPTWD_FIRED )
        {
            fprintf(fP, "not deleted: %s", winDelFates[delP->fate].textP);
        }
        else if( delP->outcome == 0 )
        {
            fprintf(fP, "deleted%s", (wordP->flags & (OPTF_INREGION | OPTF_INSPEED))?"":" (undeclared)");
        }
        else if( delP->outcomeP && !strcmp(delP->outcomeP, "skipped") )
        {
            // Only a freed eem is skipped, and only for its lems.
            fprintf(fP, "NOT deleted: a lem it depends on stays, so relayout did not try it");
        }
        else if( delP->outcome > 0 )
        {
            fprintf(fP, "NOT deleted: relayout refused it (%s), and it stays", (delP->outcomeP)?delP->outcomeP:"?");
        }
        else
        {
            fprintf(fP, "NOT deleted: relayout did not run");
        }

        if( delP->depCount )
        {
            fprintf(fP, "; with the lem%s at", (delP->depCount == 1)?"":"s");

            for( i = 0; i < delP->depCount; ++i )
            {
                fprintf(fP, " %d %04o", delP->depsPP[i]->bank, delP->depsPP[i]->addr);
            }
        }

        fprintf(fP, "\n");
    }
}

// The -O=window dump: one line per eem, lem, hazard and unexamined reference
// in bank and address order, then the totals.  Nothing but a "none" line when
// there is no eem or lem.  When every one is unreached the dump still says
// so: a program whose window nothing examined must not read as one without a
// window.
void
optDumpWindow(FILE *fP, OptTableP tableP)
{
OptWindowP winP;
OptBlockP blockP;
OptWordP wordP;
OptWordP pointerP;
OptWinDelP delP;
int addr;
int i;
int c;

    if( !(winP = tableP->windowP) || !winP->present )
    {
        fprintf(fP, "window: no reached eem or lem\n");
        return;
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                continue;
            }

            i = wordP->index;

            if( winP->classP[i] != WC_NONE )
            {
                fprintf(fP, "window %s %d %04o %s", (isEem(wordP))?"eem":"lem", wordP->bank, wordP->addr,
                    classNames[winP->classP[i]]);

                if( winP->classP[i] == WC_REFUSED )
                {
                    fprintf(fP, " %s", whyNames[winP->whyP[i]]);
                }

                fprintf(fP, " line %d\n", wordP->lineNo);
            }

            if( winP->hazardP[i] != WH_NONE )
            {
                pointerClass(winP, wordP, &pointerP);
                fprintf(fP, "window hazard %d %04o %s", wordP->bank, wordP->addr, hazardNames[winP->hazardP[i]]);

                if( winP->hazardP[i] == WH_POSSIBLE )
                {
                    fprintf(fP, " %s", whyNames[winP->hazardWhyP[i]]);
                }

                if( pointerP )
                {
                    fprintf(fP, " pointer %d %04o", pointerP->bank, pointerP->addr);
                }

                fprintf(fP, " line %d\n", wordP->lineNo);
            }

            if( winP->reverseP[i] != WH_NONE )
            {
                fprintf(fP, "window reverse %d %04o %s", wordP->bank, wordP->addr, hazardNames[winP->reverseP[i]]);

                if( winP->reverseP[i] == WH_POSSIBLE )
                {
                    fprintf(fP, " %s", whyNames[winP->reverseWhyP[i]]);
                }

                if( pointerClass(winP, wordP, &pointerP) == PC_RETURN )
                {
                    fprintf(fP, " return");
                }

                fprintf(fP, " pointer %d %04o line %d\n", pointerP->bank, pointerP->addr, wordP->lineNo);
            }

            if( winP->unexaminedP[i] )
            {
                pointerClass(winP, wordP, &pointerP);
                fprintf(fP, "window unexamined %d %04o", wordP->bank, wordP->addr);

                if( pointerP )
                {
                    fprintf(fP, " pointer %d %04o", pointerP->bank, pointerP->addr);
                }

                fprintf(fP, " line %d\n", wordP->lineNo);
            }
        }
    }

    fprintf(fP, "window totals: eem");

    for( c = WC_UNREACHED; c < WC_COUNT; ++c )
    {
        fprintf(fP, " %s %d", classNames[c], winP->eemCounts[c]);
    }

    fprintf(fP, "\nwindow totals: lem");

    for( c = WC_UNREACHED; c < WC_COUNT; ++c )
    {
        fprintf(fP, " %s %d", classNames[c], winP->lemCounts[c]);
    }

    fprintf(fP, "\nwindow totals: hazards proved %d unproved %d possible %d unknown-pointer-unknown-state %d\n",
        winP->hazardCounts[WH_PROVED], winP->hazardCounts[WH_UNPROVED], winP->hazardCounts[WH_POSSIBLE],
        winP->unknownRuntime);
    fprintf(fP, "window totals: reverse proved %d unproved %d possible %d\n", winP->reverseCounts[WH_PROVED],
        winP->reverseCounts[WH_UNPROVED], winP->reverseCounts[WH_POSSIBLE]);
    fprintf(fP, "window totals: unexamined references %d\n", winP->unexaminedCount);
    fprintf(fP, "window totals: refused");

    for( c = 0; c < W_COUNT; ++c )
    {
        fprintf(fP, " %s %d", whyNames[c], winP->refusedWhy[c]);
    }

    fprintf(fP, "\nwindow totals: routines %d with summaries %d\n", winP->routineCount, winP->closedCount);

    // The deletions, when a declaration made any record: "#n" only with a
    // bisection modifier, as the transform dump does it.
    if( !tableP->windelsP )
    {
        return;
    }

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        fprintf(fP, "window delete %d %04o %s %s", delP->wordP->bank, delP->wordP->addr,
            kindNames[delP->kind], winDelFates[delP->fate].tagP);

        if( tableP->xformBisect && delP->ordinal )
        {
            fprintf(fP, " #%d", delP->ordinal);
        }

        for( i = 0; i < delP->depCount; ++i )
        {
            fprintf(fP, "%s%04o", (i == 0)?" deps ":",", delP->depsPP[i]->addr);
        }

        fprintf(fP, " line %d\n", delP->wordP->lineNo);
    }

    fprintf(fP, "window totals: delete");

    for( c = 0; c < OPTWD_COUNT; ++c )
    {
        fprintf(fP, " %s %d", winDelFates[c].tagP, tableP->windelFates[c]);
    }

    fprintf(fP, "\n");
}

// The report's "Extend window" section: nothing when no eem or lem is reached
// and none guards an unexamined reference.  An eem or lem that is only data,
// or in code nothing runs, leaves nothing for the reader to check.
void
writeWindowReport(FILE *fP, OptTableP tableP)
{
OptWindowP winP;
OptBlockP blockP;
OptWordP wordP;
OptWordP pointerP;
int addr;
int i;
int c;
int listed;

    if( !(winP = tableP->windowP) || (!winP->active && !winP->unexaminedCount) )
    {
        return;
    }

    // Under -O=undeclared a deletion needs no declaration, so the heading
    // names what does license one.
    if( tableP->windelsP && optUndeclared() )
    {
        fprintf(fP, "\nExtend window: advisory, but for the deletions %s licenses, listed last\n",
            optUndeclaredSpelling());
    }
    else
    {
        fprintf(fP, "\nExtend window: advisory; nothing here is rewritten%s\n",
            (tableP->windelsP)?" outside a declaration":"");
    }
    fprintf(fP, "  The window state (open, closed or unknown) is followed through every reached\n");
    fprintf(fP, "  word and across calls; what depends on it is followed backward.  A redundant\n");
    fprintf(fP, "  eem or lem sets the state the window is already in; a dead lem is followed by\n");
    fprintf(fP, "  nothing that depends on the window before it is set again, and the eems it\n");
    fprintf(fP, "  frees are redundant once it is gone.  A hazard is an indirect reference in a\n");
    fprintf(fP, "  bank other than 0, made with the window possibly open, through a pointer not\n");
    fprintf(fP, "  proved to hold a 16-bit address: a bare address reaches bank 0 (proved); a\n");
    fprintf(fP, "  pointer whose value is not known may (unproved).  A reverse hazard is an\n");
    fprintf(fP, "  indirect reference made with the window possibly closed through a pointer\n");
    fprintf(fP, "  meant as a 16-bit address, or a dac-form return word, that the closed window\n");
    fprintf(fP, "  misreads: closed, the reference stays in the current bank, and chains when\n");
    fprintf(fP, "  010000 (an odd bank) is set.\n\n");

    fprintf(fP, "  eem: %d reached; %d redundant, %d freed, %d needed, %d refused (%d unreached)\n",
        winP->eemCounts[WC_REDUNDANT] + winP->eemCounts[WC_FREED] + winP->eemCounts[WC_NEEDED] +
        winP->eemCounts[WC_REFUSED], winP->eemCounts[WC_REDUNDANT], winP->eemCounts[WC_FREED],
        winP->eemCounts[WC_NEEDED], winP->eemCounts[WC_REFUSED], winP->eemCounts[WC_UNREACHED]);
    fprintf(fP, "  lem: %d reached; %d redundant, %d dead, %d needed, %d refused (%d unreached)\n",
        winP->lemCounts[WC_REDUNDANT] + winP->lemCounts[WC_DEAD] + winP->lemCounts[WC_NEEDED] +
        winP->lemCounts[WC_REFUSED], winP->lemCounts[WC_REDUNDANT], winP->lemCounts[WC_DEAD],
        winP->lemCounts[WC_NEEDED], winP->lemCounts[WC_REFUSED], winP->lemCounts[WC_UNREACHED]);
    fprintf(fP, "  hazards: %d proved, %d unproved; %d more through a bare address with the state unknown\n",
        winP->hazardCounts[WH_PROVED], winP->hazardCounts[WH_UNPROVED], winP->hazardCounts[WH_POSSIBLE]);
    fprintf(fP, "  reverse hazards: %d proved, %d unproved; %d more with the state unknown\n",
        winP->reverseCounts[WH_PROVED], winP->reverseCounts[WH_UNPROVED], winP->reverseCounts[WH_POSSIBLE]);

    if( winP->unexaminedCount )
    {
        fprintf(fP, "  unexamined: %d (indirect references after an unreached eem or lem; no hazard\n",
            winP->unexaminedCount);
        fprintf(fP, "  or reverse hazard is decided for them)\n");
    }

    if( tableP->windelsP )
    {
        fprintf(fP, "  %d of these words are in an optimize or speed region; \"Extend window\n",
            tableP->windelCount);
        fprintf(fP, "  deletions\", after the other rewrites, says which were deleted.\n");
    }

    if( winP->eemCounts[WC_REFUSED] + winP->lemCounts[WC_REFUSED] )
    {
        fprintf(fP, "  refused, by reason:");

        for( c = 0; c < W_COUNT; ++c )
        {
            if( winP->refusedWhy[c] )
            {
                fprintf(fP, " %s %d", whyNames[c], winP->refusedWhy[c]);
            }
        }

        fprintf(fP, "\n");
    }

    listed = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                continue;
            }

            i = wordP->index;
            c = winP->classP[i];

            if( (c == WC_REDUNDANT) || (c == WC_FREED) || (c == WC_DEAD) )
            {
                if( !listed++ )
                {
                    fprintf(fP, "\n");
                }

                fprintf(fP, "  bank %d %04o  %s %-9s  %s line %d\n", wordP->bank, wordP->addr,
                    (isEem(wordP))?"eem":"lem", classNames[c], wordFile(wordP), wordP->lineNo);
            }

            if( (winP->hazardP[i] == WH_PROVED) || (winP->hazardP[i] == WH_UNPROVED) )
            {
                if( !listed++ )
                {
                    fprintf(fP, "\n");
                }

                pointerClass(winP, wordP, &pointerP);
                fprintf(fP, "  bank %d %04o  hazard %-8s  %s line %d", wordP->bank, wordP->addr,
                    hazardNames[winP->hazardP[i]], wordFile(wordP), wordP->lineNo);

                if( pointerP )
                {
                    fprintf(fP, ", pointer at bank %d %04o", pointerP->bank, pointerP->addr);
                }

                fprintf(fP, "\n");
            }

            if( (winP->reverseP[i] == WH_PROVED) || (winP->reverseP[i] == WH_UNPROVED) )
            {
                if( !listed++ )
                {
                    fprintf(fP, "\n");
                }

                fprintf(fP, "  bank %d %04o  reverse %-8s  %s line %d", wordP->bank, wordP->addr,
                    hazardNames[winP->reverseP[i]], wordFile(wordP), wordP->lineNo);

                if( pointerClass(winP, wordP, &pointerP) == PC_RETURN )
                {
                    fprintf(fP, ", return word at bank %d %04o\n", pointerP->bank, pointerP->addr);
                }
                else
                {
                    fprintf(fP, ", pointer at bank %d %04o\n", pointerP->bank, pointerP->addr);
                }
            }

            if( winP->unexaminedP[i] )
            {
                if( !listed++ )
                {
                    fprintf(fP, "\n");
                }

                fprintf(fP, "  bank %d %04o  unexamined       %s line %d", wordP->bank, wordP->addr,
                    wordFile(wordP), wordP->lineNo);

                pointerClass(winP, wordP, &pointerP);

                if( pointerP )
                {
                    fprintf(fP, ", pointer at bank %d %04o", pointerP->bank, pointerP->addr);
                }

                fprintf(fP, "\n");
            }
        }
    }
}

// Release the analysis.  Safe on a table it never ran on.
void
freeWindow(OptTableP tableP)
{
OptWindowP winP;
WinLinkP linkP;
WinLinkP nextP;
OptWinDelP delP;
OptWinDelP nextDelP;
int i;

    for( delP = tableP->windelsP; delP; delP = nextDelP )
    {
        nextDelP = delP->nextP;
        free(delP->depsPP);
        free(delP);
    }

    tableP->windelsP = NILP;
    tableP->windelsTailP = NILP;

    if( !(winP = tableP->windowP) )
    {
        return;
    }

    for( i = 0; i <= winP->blockCount; ++i )
    {
        for( linkP = winP->returnsPP[i]; linkP; linkP = nextP )
        {
            nextP = linkP->nextP;
            free(linkP);
        }
    }

    for( i = 0; i <= winP->routineCount; ++i )
    {
        for( linkP = winP->callersPP[i]; linkP; linkP = nextP )
        {
            nextP = linkP->nextP;
            free(linkP);
        }
    }

    free(winP->blocksPP);
    free(winP->routinesPP);
    free(winP->entryOfPP);
    free(winP->closedP);
    free(winP->returnCountP);
    free(winP->returnsPP);
    free(winP->callersPP);
    free(winP->returnWordOfPP);
    free(winP->stampP);
    free(winP->finP);
    free(winP->fsumP);
    free(winP->fwordP);
    free(winP->linP);
    free(winP->transparentP);
    free(winP->binP);
    free(winP->bsumP);
    free(winP->lafterP);
    free(winP->stateP);
    free(winP->classP);
    free(winP->whyP);
    free(winP->hazardP);
    free(winP->hazardWhyP);
    free(winP->reverseP);
    free(winP->reverseWhyP);
    free(winP->unexaminedP);
    free(winP);
    tableP->windowP = NILP;
}
