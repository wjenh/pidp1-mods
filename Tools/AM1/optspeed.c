/*
 * The am1 optimizer's speed-mode measurement, printed by -O=speed.  Four
 * candidates; every site of each is eligible or refused for the first reason
 * that applies, and an eligible site carries its saving per execution and
 * its cost in words:
 *
 *     S1 inline   a call replaced by a copy of the routine's body; saves the
 *                 call, the return-address save and the return.  A routine
 *                 with one call site can then be deleted.
 *     S2 unroll   a loop "law i n; dac c; H, ...; isp c; jmp H" written out n
 *                 times; saves the setup, every isp and every jump back.
 *     S3 chain    a live T3 finding followed past its first hop; each further
 *                 hop saves a jump cycle.  Length-neutral.
 *     S4 place    a direct jmp to a run nothing else enters, deleted by moving
 *                 the run to follow it.  After a skip the move would need the
 *                 skip reversed on a guess, so that shape is counted apart.
 *
 * Times come from optWordTime() (handbook figures).  With no profile a saving
 * is per execution; how often a site runs is not in the source.  A site is
 * refused "guessed" when optGuessWordBits() marks a word it touches, so the
 * dump agrees with -O2's heuristics.  The bank budget closes the dump; gaps
 * below a bank's highest word are not counted as free, since only relayout
 * could use them.
 *
 * The dump creates no finding and writes nothing it reads.  The judges are
 * shared with opttransform.c, which acts on them inside an optimize or speed
 * region: optInlineJudge(), optUnrollJudge() and optPlaceJudge().  So what
 * -O=speed calls eligible is what -O1 and -O2 may rewrite.  S3 uses the same
 * optChainWalk() as the rewrite.
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

// Memory reference operation codes as the handbook writes them (bits 0 to 4,
// bit 5 clear), numbered as optdecode.c numbers them.
#define SPOP_AND        0002
#define SPOP_IOR        0004
#define SPOP_XOR        0006
#define SPOP_XCT        0010
#define SPOP_CAL        0016    // and jda, when bit 5 is set
#define SPOP_LAC        0020
#define SPOP_LIO        0022
#define SPOP_DAC        0024
#define SPOP_DAP        0026
#define SPOP_DIP        0030
#define SPOP_DIO        0032
#define SPOP_DZM        0034
#define SPOP_ADD        0040
#define SPOP_SUB        0042
#define SPOP_IDX        0044
#define SPOP_ISP        0046
#define SPOP_SAD        0050
#define SPOP_SAS        0052
#define SPOP_MUL        0054
#define SPOP_DIV        0056
#define SPOP_JMP        0060
#define SPOP_JSP        0062

// Replacement costs in microseconds, as optWordTime() counts: a direct jmp is
// one cycle; the "dac Y" an inlined jda becomes is two.
#define SPTIME_JMP      5
#define SPTIME_DAC      10

// The load-time ceiling of each bank: the highest address a word on the tape
// may occupy.  Bank 0's read-in loader occupies 07751 to 07774 while it loads.
#define SP_CEILING0     07750
#define SP_CEILING      07777

// The longest straight-line walk followed before giving up: one bank.
#define SP_MAXWALK      4096

// Body-size classes of the inline histogram: 1-4, 5-8, 9-16, 17 or more.
#define SP_HISTCLASSES  4

// How a word uses AC, for the acentry and acexit refusals.
#define SPAC_NEUTRAL    0       // neither reads nor loads it
#define SPAC_KILL       1       // loads it without reading what was there
#define SPAC_READ       2       // reads what was there
#define SPAC_STOP       3       // ends the straight-line walk: a jump, a skip, a halt

// The reasons, one enum per candidate, in the order they are decided.  Each
// ends with the eligible outcome, so a site's outcome indexes one array.

typedef enum
{
    SI_CALLEE,
    SI_ARGS,
    SI_UNKNOWN,
    SI_BANK,
    SI_NOROUTINE,
    SI_CAL,
    SI_NORETURN,
    SI_JDAWORD,
    SI_OPEN,
    SI_RECURSIVE,
    SI_HANDLER,
    SI_DISPATCH,
    SI_RTNUSED,
    SI_TAKEN,
    SI_ACENTRY,
    SI_NOTCOPYABLE,
    SI_GUESSED,
    SI_ELIGIBLE,
    SI_COUNT
} SpInlineWhy;

static const char *inlineNames[SI_COUNT] =
{
    "callee", "args", "unknown", "bank", "noroutine", "cal", "noreturn", "jdaword",
    "open", "recursive", "handler", "dispatch", "rtnused", "taken", "acentry", "notcopyable",
    "guessed", "eligible"
};

typedef enum
{
    SU_RUNTIME,
    SU_ENTRY,
    SU_COUNTER,
    SU_CALL,
    SU_EXIT,
    SU_INTERNAL,            // this and the next three: the copy must be safe
    SU_USED,
    SU_ACENTRY,
    SU_ACEXIT,
    SU_GUESSED,
    SU_ELIGIBLE,
    SU_COUNT
} SpUnrollWhy;

static const char *unrollNames[SU_COUNT] =
{
    "runtime", "entry", "counter", "call", "exit", "internal", "used", "acentry", "acexit", "guessed",
    "eligible"
};

typedef enum
{
    SC_CYCLE,
    SC_ONEHOP,
    SC_HANDSOFF,
    SC_GUESSED,
    SC_LONG,                // printed in a tally only when non-zero
    SC_ELIGIBLE,
    SC_COUNT
} SpChainWhy;

static const char *chainNames[SC_COUNT] =
{
    "cycle", "onehop", "handsoff", "guessed", "long", "eligible"
};

typedef enum
{
    SPL_AFTERSKIP,          // the guessed class, not a refusal
    SPL_JMPUSED,            // J is read, executed or its address used
    SPL_ENTRY,
    SPL_INRUN,
    SPL_FALLS,
    SPL_GUESSED,
    SPL_ELIGIBLE,
    SPL_COUNT
} SpPlaceWhy;

static const char *placeNames[SPL_COUNT] =
{
    "afterskip", "jmpused", "entry", "inrun", "falls", "guessed", "eligible"
};

// The tallies, per bank.

typedef struct
{
    int sites;                      // counted when the site is found, before it is judged
    int why[SI_COUNT];              // counted when it is judged
    int lines;                      // counted when its line is written
    int single;                     // eligible sites whose callee has no other caller
    int spent;                      // words, single-site callees netted
    int saveBest;                   // microseconds per execution, summed over the sites
    int saveWorst;
    int histCount[SP_HISTCLASSES];
    int histSpent[SP_HISTCLASSES];
} SpInlineTally;

typedef struct
{
    int sites;
    int why[SU_COUNT];
    int lines;
    int spent;
    int save;                       // microseconds per pass through each loop, summed
} SpUnrollTally;

typedef struct
{
    int sites;
    int why[SC_COUNT];
    int lines;
    int extra;                      // hops beyond the first, summed
    int save;
} SpChainTally;

typedef struct
{
    int sites;
    int why[SPL_COUNT];
    int lines;
    int moved;                      // words the eligible runs move
    int save;
    int shared;                     // context: direct jmps whose target has other predecessors
    int mutual;                     // pairs of eligible sites each inside the other's run
} SpPlaceTally;

// One eligible placement, kept for the mutual-conflict count.
typedef struct
{
    int bank;
    int jmpAddr;
    int runStart;
    int runEnd;
} SpRun;

// The pass.
typedef struct
{
    OptTableP tableP;
    FILE *fP;
    OptBlockP *blocksPP;            // by block id, index 0 unused
    SpInlineTally inl[MAXBANK + 1];
    SpUnrollTally unr[MAXBANK + 1];
    SpChainTally chn[MAXBANK + 1];
    SpPlaceTally plc[MAXBANK + 1];
    SpRun *runsP;
    int runCount;
    int runCap;
} SpPass;

static void *spAlloc(size_t count, size_t size);
static int isDirectJmp(OptWordP wordP);
static int isReached(OptWordP wordP);
static int hasExport(OptWordP wordP);
static const char *whereOf(OptWordP wordP, char *bufP, int size);
static void guessText(unsigned int bits, char *bufP, int size);

static OptRoutineP routineAt(OptTableP tableP, int bank, int addr);
static int isReturnOf(OptRoutineP routineP, OptWordP wordP);
static int inBody(OptBlockP *blocksPP, OptRoutineP routineP, int bank, int addr);
static int acUse(OptWordP wordP);
static void inlineSite(SpPass *passP, OptWordP siteP);
static void measureInline(SpPass *passP);

static void unrollSite(SpPass *passP, OptWordP ispP);
static void measureUnroll(SpPass *passP);
static int predOutside(OptTableP tableP, OptBlockP blockP, int bank, int lo, int hi, OptBlockP okAP,
    OptBlockP okBP);

static void chainSite(SpPass *passP, OptFindingP findingP);
static void measureChains(SpPass *passP);

static void placeSite(SpPass *passP, OptWordP jmpP);
static void measurePlace(SpPass *passP);

static void writeBudget(SpPass *passP);
static int writeTotals(SpPass *passP);

// Allocate zeroed memory.  Returns it, never NILP: out of memory exits.
static void *
spAlloc(size_t count, size_t size)
{
void *memP;

    if( !(memP = calloc(((count)?count:1), size)) )
    {
        fprintf(stderr, "am1: out of memory measuring the optimizer's speed candidates\n");
        exit(1);
    }

    return(memP);
}

// Print the speed dump: one line per site, then the tallies, in this format.
//   inline bB AAAA OUTCOME REASON | callee bB AAAA NAME body B copy C spent S save BEST..WORST [single net N] | FILE:LINE
//   unroll bB HHHH OUTCOME REASON | isp IIII counter NAME trips N body B spent S save T | FILE:LINE
//   chain  bB AAAA OUTCOME REASON | hops H extra E save T via AAAA AAAA ... | FILE:LINE
//   place  bB AAAA OUTCOME REASON | target TTTT NAME run R words save T | FILE:LINE
// OUTCOME is "eligible", "refused" or, after a skip, "guess"; REASON is "-"
// when eligible.  AAAA is the call word, the loop header, T3's jmp, or the jmp
// to delete.  A "guessed" refusal adds the heuristics.  The reconcile line
// says "ok" when every section's sites equal its outcomes and lines.
void
optDumpSpeed(FILE *fP, OptTableP tableP)
{
SpPass pass;
int reconciled;

    if( !tableP )
    {
        return;
    }

    memset(&pass, 0, sizeof(pass));
    pass.tableP = tableP;
    pass.fP = fP;
    pass.blocksPP = optBlockIndex(tableP);

    fprintf(fP, "speed: times per execution of a site, from the handbook: jmp and jsp 5, other memory references 10, +5 indirect\n");
    fprintf(fP, "speed: no profile; how often a site runs is not in the source\n");
    fprintf(fP, "speed: each site is eligible or refused for the first reason that applies; guessed means -O2's H1-H5 mark a word it touches\n");
    fprintf(fP, "speed: inline and unroll and place change the program's length and need relayout; chain does not\n");

    measureInline(&pass);
    measureUnroll(&pass);
    measureChains(&pass);
    measurePlace(&pass);
    reconciled = writeTotals(&pass);
    writeBudget(&pass);
    fprintf(fP, "reconcile: %s\n", (reconciled)?"ok":"MISMATCH");

    free(pass.blocksPP);

    if( pass.runsP )
    {
        free(pass.runsP);
    }
}

// Test for a direct jmp.  Returns 1 when it is, 0 otherwise or for NILP.
static int
isDirectJmp(OptWordP wordP)
{
    return( wordP && (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == SPOP_JMP) &&
        !wordP->decode.memIndirect );
}

// Test whether a word is in a reached block.
// Returns 1 when it is, 0 otherwise or for NILP.
static int
isReached(OptWordP wordP)
{
    return( wordP && wordP->blockP && wordP->blockP->reached );
}

// Test whether a word carries an exported label, so another program may
// enter it.  Returns 1 when it does, 0 otherwise.
static int
hasExport(OptWordP wordP)
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

// Spell where a word came from, "file:line", for the end of a site line.
// Returns bufP.
static const char *
whereOf(OptWordP wordP, char *bufP, int size)
{
    snprintf(bufP, (size_t)size, "%s:%d", (wordP->fileP)?wordP->fileP:"-", wordP->lineNo);
    return(bufP);
}

// Spell a set of heuristic bits as " H1 H3", or an empty string.
static void
guessText(unsigned int bits, char *bufP, int size)
{
int h;
int used;

    used = 0;
    bufP[0] = '\0';

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        if( (bits & (1u << h)) && (used < (size - 4)) )
        {
            used += snprintf((bufP + used), (size_t)(size - used), " %s", optGuessName((OptGuessId)h));
        }
    }
}

// S1: inline expansion.

// Find the routine starting at a bank and address.
// Returns the routine, or NILP when no routine starts there.
static OptRoutineP
routineAt(OptTableP tableP, int bank, int addr)
{
OptRoutineP routineP;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( (routineP->bank == bank) && (routineP->entryAddr == addr) )
        {
            return(routineP);
        }
    }

    return(NILP);
}

// Test whether a word is one of a routine's returns: the patched "rtn, jmp ."
// word itself, or an unpatched "jmp i rtn".  Must match optroutine.c's
// isReturnWord().  Returns 1 when it is, 0 otherwise.
static int
isReturnOf(OptRoutineP routineP, OptWordP wordP)
{
    if( !wordP || !inGraph(wordP) || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    if( (wordP->bank == routineP->returnBank) && (wordP->addr == routineP->returnAddr) )
    {
        return( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == SPOP_JMP) );
    }

    if( (wordP->decode.group != OPTG_MEMREF) || (wordP->decode.opcode != SPOP_JMP) ||
        !wordP->decode.memIndirect || (wordP->flags & OPTF_PATCHED) )
    {
        return(0);
    }

    return( (wordP->bank == routineP->returnBank) && (wordP->decode.address == routineP->returnAddr) );
}

// Test whether an address lies in one of a routine's body blocks, which need
// not be contiguous.  Returns 1 when it does, 0 otherwise.
static int
inBody(OptBlockP *blocksPP, OptRoutineP routineP, int bank, int addr)
{
OptBlockP blockP;
int m;

    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        if( (blockP->bank == bank) && (addr >= blockP->startAddr) && (addr <= blockP->endAddr) )
        {
            return(1);
        }
    }

    return(0);
}

// Classify how one word uses AC, erring toward refusal: an in-out transfer,
// an xct and anything unknown read AC; a skip, a jump and a halt end the walk.
// Returns SPAC_NEUTRAL, SPAC_KILL, SPAC_READ or SPAC_STOP.
static int
acUse(OptWordP wordP)
{
OptDecodeP dP;

    dP = &wordP->decode;

    switch( dP->group )
    {
    case OPTG_LAW:
        return(SPAC_KILL);

    case OPTG_SKIP:
        // A skip ends a block, so the walk stops there whatever it tests; one
        // that tests AC reads it first.
        return( (dP->skipConds & (OPTC_SZA | OPTC_SPA | OPTC_SMA))?SPAC_READ:SPAC_STOP );

    case OPTG_SHIFT:
        return( (dP->shiftRegs & 1)?SPAC_READ:SPAC_NEUTRAL );

    case OPTG_OPERATE:
        // cla acts before anything that could read AC (phase order in
        // optimizer.h), so a word with cla loads it outright.
        if( dP->microBits & OPTM_CLA )
        {
            return(SPAC_KILL);
        }

        if( dP->microBits & (OPTM_CMA | OPTM_LIA | OPTM_LAT | OPTM_LAP) )
        {
            return(SPAC_READ);
        }

        if( dP->microBits & OPTM_LAI )
        {
            return(SPAC_KILL);
        }

        if( dP->microBits & OPTM_HLT )
        {
            return(SPAC_STOP);
        }

        return(SPAC_NEUTRAL);

    case OPTG_1D:
        return( (dP->specialBits & OPTX_IDA)?SPAC_READ:SPAC_NEUTRAL );

    case OPTG_MEMREF:
        break;

    case OPTG_IOT:
        // Extend mode (074: eem, lem) and the sequence-break controls (050
        // to 057) move no data, as in optguess.c's deviceIot(); farenter's
        // "dac rtn; lem" depends on it.  Every other transfer may take AC.
        if( (dP->iotDevice == 074) || ((dP->iotDevice >= 050) && (dP->iotDevice <= 057)) )
        {
            return(SPAC_NEUTRAL);
        }

        return(SPAC_READ);

    case OPTG_UNKNOWN:
    default:
        return(SPAC_READ);
    }

    switch( dP->opcode )
    {
    case SPOP_LAC:
    case SPOP_IDX:
    case SPOP_ISP:
    case SPOP_JSP:
        return(SPAC_KILL);

    case SPOP_LIO:
    case SPOP_DIO:
    case SPOP_DZM:
        return(SPAC_NEUTRAL);

    case SPOP_JMP:
        return(SPAC_STOP);

    default:
        // and ior xor xct cal/jda dac dap dip add sub sad sas mul div
        return(SPAC_READ);
    }
}

// Judge every call site in bank then address order.
static void
measureInline(SpPass *passP)
{
OptTableP tableP;
OptWordP wordP;
int bank;
int addr;

    tableP = passP->tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            if( wordP && wordP->isCallSite && isReached(wordP) )
            {
                inlineSite(passP, wordP);
            }
        }
    }
}

// Build an array of every block by id, index 0 unused; the caller frees it.
// Returns the array, never NILP: out of memory exits.
OptBlockP *
optBlockIndex(OptTableP tableP)
{
OptBlockP *blocksPP;
OptBlockP blockP;

    blocksPP = (OptBlockP *)spAlloc((size_t)(tableP->blockCount + 1), sizeof(OptBlockP));

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        blocksPP[blockP->id] = blockP;
    }

    return(blocksPP);
}

// Get the last word of a bank the budgets count as free.  A test-only build
// with -DOPTCEILING_BLIND_BUDGET ignores a declared ceiling.
// Returns the declared '%%ceiling' less one, else 07750 or 07777.
int
optBankCeiling(OptTableP tableP, int bank)
{
#ifndef OPTCEILING_BLIND_BUDGET
    if( tableP && tableP->bankCeiling[bank] )
    {
        return(tableP->bankCeiling[bank] - 1);
    }
#else
    (void)tableP;
#endif

    return( (bank == 0)?SP_CEILING0:SP_CEILING );
}

// Get the bank's declared ceiling as written, the first word no rewrite may
// reach.  Returns it, or 0 when the bank declares none.
int
optBankCeilingDeclared(OptTableP tableP, int bank)
{
    return( (tableP)?tableP->bankCeiling[bank]:0 );
}

// Give the inline reason whose line names the word that broke the copy.
// Returns SI_NOTCOPYABLE.
int
optInlineNotCopyable(void)
{
    return(SI_NOTCOPYABLE);
}

// Name an inline reason as the dump prints it.
// Returns a static string, "?" out of range.
const char *
optInlineWhyName(int why)
{
    if( (why < 0) || (why >= SI_COUNT) )
    {
        return("?");
    }

    return(inlineNames[why]);
}

// Test whether a copied word leaves early for the caller (a return that is not
// the dropped last word, or a direct jmp to the dropped return) and, being its
// statement's whole expression, can become "jmp .+k".  Returns 1 if so, else 0.
int
optInlineRetargets(OptInlineShapeP sP, OptWordP wordP)
{
    if( !wordP || !sP->routineP || !sP->dropsRtn || (wordP == sP->lastP) || (wordP == sP->entryP) )
    {
        return(0);
    }

    if( (wordP->kind != OPTK_EXPR) || !wordP->nodeP || (wordP->nodeP->type != EXPR) ||
        (wordP->nodeP->rightP != wordP->exprP) )
    {
        return(0);
    }

    if( isReturnOf(sP->routineP, wordP) )
    {
        return(1);
    }

    return( isDirectJmp(wordP) && (wordP->decode.address == sP->lastP->addr) &&
        (wordP->bank == sP->lastP->bank) && !(wordP->flags & (OPTF_PATCHED | OPTF_WRITTEN)) );
}

// Judge one call site for S1 into *sP: the first refusal, else the copy's
// shape and figures.  The guess is recorded in guessBits, not applied; the
// caller decides which heuristics apply.
void
optInlineJudge(OptTableP tableP, OptBlockP *blocksPP, OptWordP siteP, OptInlineShapeP sP)
{
OptRoutineP routineP;
OptWordP entryP;
OptWordP rtnP;
OptWordP wordP;
OptWordP lastP;
OptBlockP blockP;
OptEdgeP edgeP;
OptEdgeP poolInP;
SpInlineWhy why;
unsigned int bits;
int isJda;
int isCal;
int dapForm;
int m;
int addr;
int use;
int allowed;
int body;
int copy;
int tCall;
int tSave;
int gain;
int best;
int worst;
int returns;
int freedConsts;
int dropsRtn;

    memset(sP, 0, sizeof(OptInlineShape));
    routineP = NILP;
    entryP = NILP;
    rtnP = NILP;
    lastP = NILP;
    dropsRtn = 0;
    isCal = ((siteP->decode.opcode == SPOP_CAL) && !siteP->decode.indirect);
    isJda = ((siteP->decode.opcode == SPOP_CAL) && siteP->decode.indirect);
    dapForm = 0;
    sP->isJda = isJda;

    // The refusals, in SpInlineWhy order; each "goto judged" leaves why set.
    if( siteP->calleeBank < 0 )
    {
        why = SI_CALLEE;
        goto judged;
    }

    if( siteP->returnCase == OPTRC_STEPPED )
    {
        why = SI_ARGS;
        goto judged;
    }

    if( siteP->returnCase != OPTRC_AFTER )
    {
        why = SI_UNKNOWN;
        goto judged;
    }

    if( siteP->calleeBank != siteP->bank )
    {
        why = SI_BANK;
        goto judged;
    }

    if( !(routineP = routineAt(tableP, siteP->calleeBank, siteP->calleeAddr)) )
    {
        why = SI_NOROUTINE;
        goto judged;
    }

    sP->routineP = routineP;
    entryP = routineP->entryBlockP->firstP;
    sP->entryP = entryP;

    if( isCal )
    {
        why = SI_CAL;
        goto judged;
    }

    if( (routineP->returnWord == OPTRW_NONE) || (routineP->flags & OPTRT_NORETURN) )
    {
        why = SI_NORETURN;
        goto judged;
    }

    if( routineP->returnWord == OPTRW_JDA )
    {
        why = SI_JDAWORD;
        goto judged;
    }

    if( routineP->flags & (OPTRT_OPENBODY | OPTRT_SHARED | OPTRT_FALLIN | OPTRT_CALLSUNKNOWN) )
    {
        why = SI_OPEN;
        goto judged;
    }

    if( routineP->flags & OPTRT_ONCYCLE )
    {
        why = SI_RECURSIVE;
        goto judged;
    }

    if( routineP->flags & OPTRT_SBSPOOL )
    {
        why = SI_HANDLER;
        goto judged;
    }

    // A body block in another bank would move every memory reference in it.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        if( blocksPP[routineP->bodyIdsP[m]]->bank != routineP->bank )
        {
            why = SI_BANK;
            goto judged;
        }
    }

    rtnP = wordAt(tableP, routineP->returnBank, routineP->returnAddr);
    dapForm = (rtnP && inGraph(rtnP) && isReturnOf(routineP, rtnP));
    sP->rtnP = rtnP;
    sP->dapForm = dapForm;

    // An indirect jump that is not a return is a dispatch the copy could not
    // be checked against.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            wordP = wordAt(tableP, blockP->bank, addr);

            if( wordP && (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == SPOP_JMP) &&
                wordP->decode.memIndirect && !isReturnOf(routineP, wordP) )
            {
                why = SI_DISPATCH;
                goto judged;
            }
        }
    }

    // The return word may be touched by the save, by the returns, and in the
    // patched form by the body's own jumps to it.  Anything else is a use the
    // copy would lose.
    if( !rtnP || (rtnP->flags & (OPTF_TAKEN | OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN | OPTF_XCTTARGET)) )
    {
        why = SI_RTNUSED;
        goto judged;
    }

    for( edgeP = rtnP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        allowed = 0;

        if( edgeP->fromP == entryP )
        {
            allowed = 1;
        }
        else if( edgeP->fromP->blockP && optRoutineOwnsBlock(routineP, edgeP->fromP->blockP) )
        {
            if( isReturnOf(routineP, edgeP->fromP) )
            {
                allowed = 1;
            }
            else if( dapForm && isDirectJmp(edgeP->fromP) && (edgeP->role == OPTR_JUMP) )
            {
                allowed = 1;
            }
        }

        if( !allowed )
        {
            why = SI_RTNUSED;
            goto judged;
        }
    }

    // A body word something else names, executes or writes.  The entry may be
    // taken -- "jsp i [sub]" takes it -- and the patched return word is written
    // by the save, which is the idiom.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                continue;
            }

            if( wordP == entryP )
            {
                if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_XCTTARGET | OPTF_MAYBE_WRITTEN) )
                {
                    why = SI_TAKEN;
                    goto judged;
                }

                continue;
            }

            if( dapForm && (wordP == rtnP) )
            {
                if( wordP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_WRITTEN | OPTF_MAYBE_ENTERED) )
                {
                    why = SI_TAKEN;
                    goto judged;
                }

                continue;
            }

            if( wordP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_WRITTEN | OPTF_PATCHED |
                OPTF_MAYBE_WRITTEN | OPTF_MAYBE_ENTERED) )
            {
                why = SI_TAKEN;
                goto judged;
            }
        }
    }

    // AC at entry held the return address; inlined, it holds what the caller
    // left.  Walk on from the word after the save: the first word that uses AC
    // must load it outright.  Only a label boundary lets the walk continue.
    use = SPAC_STOP;
    addr = (routineP->entryAddr + 1);

    for( m = 0; m < SP_MAXWALK; ++m )
    {
        if( !(wordP = wordAt(tableP, routineP->bank, addr)) || !inGraph(wordP) )
        {
            use = SPAC_STOP;
            break;
        }

        if( (use = acUse(wordP)) != SPAC_NEUTRAL )
        {
            break;
        }

        if( (wordP->blockP->lastP == wordP) && (wordP->blockP->endKind != OPTBE_BOUNDARY) )
        {
            use = SPAC_STOP;
            break;
        }

        addr = ((addr + 1) & ADDRMASK);
    }

    if( use != SPAC_KILL )
    {
        why = SI_ACENTRY;
        goto judged;
    }

    // notcopyable: a label in the copy would be defined twice; a copied word
    // naming a body word (or the dropped return, unless optInlineRetargets()
    // retargets it) would still name the original's; and a body whose last
    // word is not a return has nowhere for the copy to end.  The site's line
    // names the offending word.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        if( !lastP || (blockP->endAddr > lastP->addr) )
        {
            lastP = blockP->lastP;
        }
    }

    dropsRtn = (lastP && isReturnOf(routineP, lastP));
    sP->lastP = lastP;
    sP->dropsRtn = dropsRtn;

    // Labels and internal names are tested before the last-word and one-run
    // tests, so a routine with both is reported by the first.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                continue;
            }

            if( (wordP == entryP) || (wordP == lastP) )
            {
                continue;       // not copied
            }

            if( wordP->labelsP )
            {
                snprintf(sP->detail, sizeof(sP->detail), " at %04o label %s", wordP->addr,
                    firstLabelName(wordP));
                why = SI_NOTCOPYABLE;
                goto judged;
            }

            if( optInlineRetargets(sP, wordP) )
            {
                continue;       // becomes "jmp .+k" in the copy; names nothing of the body
            }

            for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
            {
                if( (lastP && (edgeP->toBank == lastP->bank) && (edgeP->toAddr == lastP->addr)) ||
                    inBody(blocksPP, routineP, edgeP->toBank, edgeP->toAddr) )
                {
                    snprintf(sP->detail, sizeof(sP->detail), " at %04o names %04o", wordP->addr,
                        edgeP->toAddr);
                    why = SI_NOTCOPYABLE;
                    goto judged;
                }
            }
        }
    }

    if( !dropsRtn )
    {
        snprintf(sP->detail, sizeof(sP->detail), " at %04o no return last", (lastP)?lastP->addr:0);
        why = SI_NOTCOPYABLE;
        goto judged;
    }

    // The copy is made from the source's text, the run of statements from the
    // word after the entry to the word before the return, so the body has to
    // be that run: no block before the entry, and no word between the entry
    // and the return that is not the body's.
    for( m = 0; m < routineP->blockCount; ++m )
    {
        if( blocksPP[routineP->bodyIdsP[m]]->startAddr < routineP->entryAddr )
        {
            break;
        }
    }

    if( (m < routineP->blockCount) || (routineP->wordCount != (lastP->addr - routineP->entryAddr + 1)) )
    {
        snprintf(sP->detail, sizeof(sP->detail), " at %04o not one run", routineP->entryAddr);
        why = SI_NOTCOPYABLE;
        goto judged;
    }

    // The heuristics over the site and every body word, recorded: -O=speed
    // refuses on any, -O2 on those that apply there, -O1 on none.
    bits = optGuessWordBits(tableP, siteP);

    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            bits |= optGuessWordBits(tableP, wordAt(tableP, blockP->bank, addr));
        }
    }

    sP->guessBits = bits;
    sP->eligible = 1;
    why = SI_ELIGIBLE;

    // The figures.  The body's last word is the dropped return: it falls
    // through into the caller's next word and is not copied.
    body = routineP->wordCount;
    copy = (body - 2);

    // What the program grows by at the site.  A jsp is replaced by the copy;
    // a jda still deposits AC in its entry's data word, so it stays as "dac Y"
    // and the copy follows it.
    sP->spent = (copy - ((isJda)?0:1));

    tCall = optWordTime(siteP);

    if( isJda )
    {
        tCall -= SPTIME_DAC;
    }

    tSave = optWordTime(entryP);
    returns = 0;
    best = 0;
    worst = 0;

    for( m = 0; m < routineP->blockCount; ++m )
    {
        blockP = blocksPP[routineP->bodyIdsP[m]];

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            wordP = wordAt(tableP, blockP->bank, addr);

            if( !isReturnOf(routineP, wordP) )
            {
                continue;
            }

            // A return that is the last word falls through: all of its time is
            // saved.  Any other becomes a direct jmp to the caller's next word.
            gain = (optWordTime(wordP) - ((wordP == lastP)?0:SPTIME_JMP));

            if( !returns || (gain > best) )
            {
                best = gain;
            }

            if( !returns || (gain < worst) )
            {
                worst = gain;
            }

            ++returns;
        }
    }

    sP->best = (best + tCall + tSave);
    sP->worst = (worst + tCall + tSave);
    sP->body = body;
    sP->copy = copy;

    // A callee with this one call site, no other way in, and no exported label
    // can be deleted once its only call is inlined.  Its data return word goes
    // too, and so does a constant-pool word only this site names.
    if( ((routineP->jspSites + routineP->jdaSites) == 1) && !hasExport(entryP) &&
        !(entryP->flags & OPTF_START) )
    {
        sP->single = 1;
        freedConsts = 0;

        for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( edgeP->role != OPTR_TAKEN )
            {
                continue;
            }

            if( edgeP->fromP->kind != OPTK_CONST )
            {
                sP->single = 0;
                break;
            }

            // The pool word must be named by this site alone.
            allowed = 1;

            for( poolInP = edgeP->fromP->inP; poolInP; poolInP = poolInP->nextInP )
            {
                if( poolInP->fromP != siteP )
                {
                    allowed = 0;
                }
            }

            if( !allowed )
            {
                sP->single = 0;
                break;
            }

            ++freedConsts;
        }

        if( sP->single )
        {
            sP->freedConsts = freedConsts;
            sP->net = (sP->spent - body - ((dapForm)?0:1) - freedConsts);
        }
    }

judged:
    sP->why = why;
}

// Judge one call site and write its line.
static void
inlineSite(SpPass *passP, OptWordP siteP)
{
OptTableP tableP;
SpInlineTally *tP;
OptInlineShape shape;
OptWordP wordP;
SpInlineWhy why;
int hist;
char where[OPTMSG_SIZE];
char guess[64];
char callee[OPTMSG_SIZE];

    tableP = passP->tableP;
    tP = &passP->inl[siteP->bank];
    ++tP->sites;
    guess[0] = '\0';

    optInlineJudge(tableP, passP->blocksPP, siteP, &shape);
    why = (SpInlineWhy)shape.why;

    // Every heuristic refuses here, wherever the site is.
    if( shape.eligible && shape.guessBits )
    {
        guessText(shape.guessBits, guess, sizeof(guess));
        why = SI_GUESSED;
    }

    ++tP->why[why];

    if( siteP->calleeBank >= 0 )
    {
        wordP = wordAt(tableP, siteP->calleeBank, siteP->calleeAddr);
        snprintf(callee, sizeof(callee), "callee b%d %04o %s", siteP->calleeBank, siteP->calleeAddr,
            (wordP)?firstLabelName(wordP):"-");
    }
    else
    {
        snprintf(callee, sizeof(callee), "callee -");
    }

    if( why != SI_ELIGIBLE )
    {
        fprintf(passP->fP, "inline b%d %04o refused %s%s%s | %s | %s\n", siteP->bank, siteP->addr,
            inlineNames[why], guess, (why == SI_NOTCOPYABLE)?shape.detail:"", callee,
            whereOf(siteP, where, sizeof(where)));
        ++tP->lines;
        return;
    }

    hist = (shape.body <= 4)?0:((shape.body <= 8)?1:((shape.body <= 16)?2:3));
    ++tP->histCount[hist];
    tP->histSpent[hist] += (shape.single)?shape.net:shape.spent;
    tP->spent += (shape.single)?shape.net:shape.spent;
    tP->saveBest += shape.best;
    tP->saveWorst += shape.worst;
    tP->single += shape.single;

    fprintf(passP->fP, "inline b%d %04o eligible - | %s body %d copy %d spent %d save %d..%d",
        siteP->bank, siteP->addr, callee, shape.body, shape.copy, shape.spent, shape.best, shape.worst);

    if( shape.single )
    {
        fprintf(passP->fP, " single net %d", shape.net);
    }

    fprintf(passP->fP, " | %s\n", whereOf(siteP, where, sizeof(where)));
    ++tP->lines;
}

// S2: loop unrolling.

// Test for a loop latch: a reached, direct, unpatched isp followed by a direct,
// unpatched, unwritten jmp back to a header H no higher than the isp.  Shared
// with the rewrite.  Returns 1 when it is, 0 otherwise.
int
optUnrollSite(OptTableP tableP, OptWordP ispP)
{
OptWordP jmpP;

    if( !ispP || !isReached(ispP) || (ispP->decode.group != OPTG_MEMREF) ||
        (ispP->decode.opcode != SPOP_ISP) || ispP->decode.memIndirect ||
        (ispP->flags & OPTF_PATCHED) || (ispP->addr >= (BANKSIZE - 1)) )
    {
        return(0);
    }

    jmpP = tableP->banksP[ispP->bank]->wordsP[ispP->addr + 1];

    if( !isDirectJmp(jmpP) || !inGraph(jmpP) || (jmpP->flags & (OPTF_PATCHED | OPTF_WRITTEN)) ||
        (jmpP->decode.address > ispP->addr) )
    {
        return(0);
    }

    return(1);
}

// Find every latch, in bank then address order, and judge the loop it closes.
static void
measureUnroll(SpPass *passP)
{
OptTableP tableP;
OptWordP wordP;
int bank;
int addr;

    tableP = passP->tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < (BANKSIZE - 1); ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            if( optUnrollSite(tableP, wordP) )
            {
                unrollSite(passP, wordP);
            }
        }
    }
}

// Judge the loop an "isp c" at ispP and the "jmp H" after it close, and write
// its line.
static void
unrollSite(SpPass *passP, OptWordP ispP)
{
SpUnrollTally *tP;
OptUnrollShape shape;
int bank;
int head;
char where[OPTMSG_SIZE];
char guess[64];

    bank = ispP->bank;
    tP = &passP->unr[bank];
    ++tP->sites;

    optUnrollJudge(passP->tableP, ispP, &shape);
    head = shape.jmpP->decode.address;
    ++tP->why[shape.why];

    if( shape.why == SU_ELIGIBLE )
    {
        tP->spent += shape.spent;
        tP->save += shape.save;
        fprintf(passP->fP, "unroll b%d %04o eligible - | isp %04o counter %s trips %d body %d spent %d save %d | %s\n",
            bank, head, ispP->addr, (shape.ctrP)?firstLabelName(shape.ctrP):"-", shape.trips, shape.body,
            shape.spent, shape.save, whereOf(ispP, where, sizeof(where)));
    }
    else
    {
        guess[0] = '\0';

        if( shape.why == SU_GUESSED )
        {
            guessText(shape.guessBits, guess, sizeof(guess));
        }

        fprintf(passP->fP, "unroll b%d %04o refused %s%s | isp %04o counter %s trips %d | %s\n",
            bank, head, unrollNames[shape.why], (shape.why == SU_GUESSED)?guess:shape.detail, ispP->addr,
            (shape.ctrP)?firstLabelName(shape.ctrP):"-", shape.trips, whereOf(ispP, where, sizeof(where)));
    }

    ++tP->lines;
}

// Test for a predecessor, other than okAP and okBP, lying outside lo..hi of a
// bank (lo above hi makes every one count).  Scans every block's successors,
// having no predecessor lists.  Returns 1 when one exists, 0 when none does.
static int
predOutside(OptTableP tableP, OptBlockP blockP, int bank, int lo, int hi, OptBlockP okAP, OptBlockP okBP)
{
OptBlockP predP;
OptFlowEdgeP flowP;

    for( predP = tableP->blocksP; predP; predP = predP->nextP )
    {
        for( flowP = predP->succP; flowP; flowP = flowP->nextP )
        {
            if( (flowP->toP != blockP) || (predP == okAP) || (predP == okBP) )
            {
                continue;
            }

            if( (predP->bank != bank) || (predP->startAddr < lo) || (predP->endAddr > hi) )
            {
                return(1);
            }
        }
    }

    return(0);
}

// Judge the loop a latch closes into *shapeP.  S is the law or lac setup, D
// the dac, I the isp and J the jmp; the refusals, in order:
//   runtime   the count is not a constant loaded just above the header
//   entry     something other than the fall from the setup and the jump back
//             enters the loop, or the setup can be skipped or entered between
//             its two words
//   counter   the counter is touched by anything but the dac and the isp
//   call      the body makes a call
//   exit      the body leaves the loop other than by the isp's skip
//   internal  a body word other than H carries a label, or names a word from
//             S to J: a copy would redefine the label or name the original's
//             word; only straight-line bodies are copied
//   used      a word from S to J may be written, so the copies would not see
//             the change, or one of S, D, I and J, which the rewrite deletes,
//             is read, executed or its address taken
//   acentry   the body reads AC before it loads it: in the loop it sees the
//             count or what the isp left; unrolled, what came before
//   acexit    the code after the loop reads AC before it loads it: the isp
//             leaves +0 there; unrolled, the last copy's value
//   guessed   a heuristic marks a word from S to J; a delay loop must keep its
//             timing
// isp leaves the overflow flag and the other registers alone.
void
optUnrollJudge(OptTableP tableP, OptWordP ispP, OptUnrollShapeP shapeP)
{
OptWordP jmpP;
OptWordP headP;
OptWordP setP;
OptWordP depP;
OptWordP ctrP;
OptWordP constP;
OptWordP wordP;
OptBlockP blockP;
OptFlowEdgeP flowP;
OptEdgeP edgeP;
unsigned int bits;
int bank;
int head;
int trips;
int addr;
int use;
int m;

    memset(shapeP, 0, sizeof(*shapeP));
    bank = ispP->bank;
    jmpP = wordAt(tableP, bank, (ispP->addr + 1));
    head = jmpP->decode.address;
    headP = wordAt(tableP, bank, head);
    setP = (head >= 2)?wordAt(tableP, bank, (head - 2)):NILP;
    depP = (head >= 1)?wordAt(tableP, bank, (head - 1)):NILP;
    ctrP = wordAt(tableP, bank, ispP->decode.address);
    trips = 0;

    shapeP->ispP = ispP;
    shapeP->jmpP = jmpP;
    shapeP->headP = headP;
    shapeP->setP = setP;
    shapeP->depP = depP;
    shapeP->ctrP = ctrP;

    // runtime: the count is not a constant loaded just above the header.
    shapeP->why = SU_RUNTIME;

    if( !headP || !inGraph(headP) || !setP || !depP || !inGraph(setP) || !inGraph(depP) )
    {
        return;
    }

    if( (depP->decode.group != OPTG_MEMREF) || (depP->decode.opcode != SPOP_DAC) || depP->decode.memIndirect ||
        (depP->decode.address != ispP->decode.address) || (depP->flags & OPTF_PATCHED) )
    {
        return;
    }

    if( setP->decode.group == OPTG_LAW )
    {
        // law i n loads -n: n trips, since isp on -1 gives +0 and skips.
        if( setP->decode.indirect )
        {
            trips = setP->decode.address;
        }
    }
    else if( (setP->decode.group == OPTG_MEMREF) && (setP->decode.opcode == SPOP_LAC) &&
        !setP->decode.memIndirect && !(setP->flags & OPTF_PATCHED) )
    {
        constP = wordAt(tableP, bank, setP->decode.address);

        if( constP && !inGraph(constP) && !(constP->flags & (OPTF_WRITTEN | OPTF_MAYBE_WRITTEN | OPTF_RESERVED)) &&
            (constP->value & 0400000) && (constP->value != 0777777) )
        {
            trips = (0777777 - constP->value);
        }
    }

    shapeP->trips = trips;

    if( trips < 1 )
    {
        return;
    }

    // entry: the header's only predecessors must be the fall from the dac and
    // the jump back, and the setup must not be skippable or split.
    shapeP->why = SU_ENTRY;

    if( (setP->flags & OPTF_AFTERSKIP) || (depP->flags & OPTF_BLOCKSTART) || (depP->blockP->lastP != depP) ||
        (headP->blockP->predCount != 2) ||
        predOutside(tableP, headP->blockP, bank, 1, 0, depP->blockP, jmpP->blockP) )
    {
        return;
    }

    for( addr = head; addr <= jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);

        if( !wordP || !inGraph(wordP) || (wordP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_ENTERED)) )
        {
            return;
        }

        if( (addr > head) && (wordP->flags & OPTF_BLOCKSTART) &&
            predOutside(tableP, wordP->blockP, bank, head, jmpP->addr, NILP, NILP) )
        {
            return;
        }
    }

    // counter: the loop's counter is set by the dac, stepped by the isp, and
    // touched by nothing else.
    shapeP->why = SU_COUNTER;

    if( !ctrP || inGraph(ctrP) || (ctrP->flags & (OPTF_TAKEN | OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN | OPTF_XCTTARGET)) )
    {
        return;
    }

    for( edgeP = ctrP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( (edgeP->fromP != depP) && (edgeP->fromP != ispP) )
        {
            return;
        }
    }

    // call, then exit: every edge out of a body block stays in the loop, bar
    // the isp's skip out.
    for( addr = head; addr <= jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);
        blockP = wordP->blockP;

        if( blockP->startAddr != addr )
        {
            continue;
        }

        if( blockP->endKind == OPTBE_CALL )
        {
            shapeP->why = SU_CALL;
            return;
        }
    }

    shapeP->why = SU_EXIT;

    for( addr = head; addr <= jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);
        blockP = wordP->blockP;

        if( blockP->startAddr != addr )
        {
            continue;
        }

        if( (blockP->endKind == OPTBE_HALT) || (blockP->endAddr > jmpP->addr) )
        {
            return;
        }

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( (blockP->lastP == ispP) && (flowP->kind == OPTFK_SKIP) )
            {
                continue;
            }

            if( !flowP->toP || (flowP->toP->bank != bank) || (flowP->toP->startAddr < head) ||
                (flowP->toP->startAddr > jmpP->addr) )
            {
                return;
            }
        }
    }

    // internal: H's label stays on the first copy.  The edges include names
    // through a pointer or written as a number, not just symbols.
    shapeP->why = SU_INTERNAL;

    for( addr = head; addr < ispP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);

        if( (addr > head) && wordP->labelsP )
        {
            snprintf(shapeP->detail, sizeof(shapeP->detail), " at %04o label %s", addr, firstLabelName(wordP));
            return;
        }

        for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( (edgeP->toBank == bank) && (edgeP->toAddr >= setP->addr) && (edgeP->toAddr <= jmpP->addr) )
            {
                snprintf(shapeP->detail, sizeof(shapeP->detail), " at %04o names %04o", addr, edgeP->toAddr);
                return;
            }
        }
    }

    // used.  A jump to S is not a use: S's label moves to the first copy.
    shapeP->why = SU_USED;

    for( addr = setP->addr; addr <= jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);

        if( wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN) )
        {
            snprintf(shapeP->detail, sizeof(shapeP->detail), " at %04o written", addr);
            return;
        }

        if( ((addr < head) || (addr >= ispP->addr)) &&
            (wordP->flags & (OPTF_READ | OPTF_MAYBE_READ | OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_ENTERED)) )
        {
            snprintf(shapeP->detail, sizeof(shapeP->detail), " at %04o read", addr);
            return;
        }
    }

    // acentry: walk the body from H.  A read before a load, or a block ending
    // other than at a label boundary, refuses; a body that never touches AC
    // passes.
    shapeP->why = SU_ACENTRY;
    use = SPAC_NEUTRAL;

    for( addr = head; addr < ispP->addr; ++addr )
    {
        wordP = wordAt(tableP, bank, addr);

        if( (use = acUse(wordP)) != SPAC_NEUTRAL )
        {
            break;
        }

        if( (wordP->blockP->lastP == wordP) && (wordP->blockP->endKind != OPTBE_BOUNDARY) )
        {
            use = SPAC_STOP;
            break;
        }
    }

    if( (use == SPAC_READ) || (use == SPAC_STOP) )
    {
        return;
    }

    // acexit: walk on from where the last copy falls, following direct jmps;
    // any other block end stops the walk, and stopping before a load refuses.
    shapeP->why = SU_ACEXIT;
    use = SPAC_STOP;
    addr = ((jmpP->addr + 1) & ADDRMASK);

    for( m = 0; m < SP_MAXWALK; ++m )
    {
        if( !(wordP = wordAt(tableP, bank, addr)) || !inGraph(wordP) )
        {
            use = SPAC_STOP;
            break;
        }

        if( isDirectJmp(wordP) && !(wordP->flags & (OPTF_PATCHED | OPTF_WRITTEN | OPTF_MAYBE_WRITTEN)) )
        {
            use = SPAC_STOP;
            addr = wordP->decode.address;
            continue;
        }

        if( (use = acUse(wordP)) != SPAC_NEUTRAL )
        {
            break;
        }

        if( (wordP->blockP->lastP == wordP) && (wordP->blockP->endKind != OPTBE_BOUNDARY) )
        {
            use = SPAC_STOP;
            break;
        }

        addr = ((addr + 1) & ADDRMASK);
    }

    if( use != SPAC_KILL )
    {
        return;
    }

    // n copies of the body replace S, D, the body, I and J; the counter word
    // stays.  One pass saves the setup, n isps and n - 1 jumps back.
    shapeP->eligible = 1;
    shapeP->body = (ispP->addr - head);
    shapeP->unrolled = (trips * shapeP->body);
    shapeP->spent = (((trips - 1) * shapeP->body) - 4);
    shapeP->save = (optWordTime(setP) + optWordTime(depP) + (trips * optWordTime(ispP)) +
        ((trips - 1) * optWordTime(jmpP)));

    // guessed.
    bits = 0;

    for( addr = setP->addr; addr <= jmpP->addr; ++addr )
    {
        bits |= optGuessWordBits(tableP, wordAt(tableP, bank, addr));
    }

    shapeP->guessBits = bits;
    shapeP->why = (bits)?SU_GUESSED:SU_ELIGIBLE;
}

// Name an unroll reason as the dump prints it.
// Returns a static string, "?" out of range.
const char *
optUnrollWhyName(int why)
{
    if( (why < 0) || (why >= SU_COUNT) )
    {
        return("?");
    }

    return(unrollNames[why]);
}

// Give the unroll judge's guessed reason.  Returns SU_GUESSED.
int
optUnrollGuessed(void)
{
    return(SU_GUESSED);
}

// S3: T3 chains followed to their end.

// Judge every live T3 finding, in the finding list's bank then address order.
static void
measureChains(SpPass *passP)
{
OptFindingP findingP;

    for( findingP = passP->tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        if( findingP->rule == OPTRULE_T3 )
        {
            chainSite(passP, findingP);
        }
    }
}

// Follow one T3 finding's chain with optChainWalk() under every heuristic,
// judge it and write its line.  A span or heuristic on the jmp or its first
// intermediate refuses the site; further along it only stops the chain there.
static void
chainSite(SpPass *passP, OptFindingP findingP)
{
OptTableP tableP;
SpChainTally *tP;
OptWordP jmpP;
OptWordP stopP;
OptWordP chainPP[OPT_MAXCHAIN];
OptChainEnd end;
SpChainWhy why;
unsigned int bits;
int hops;
int handsOff;
int save;
int i;
int used;
char where[OPTMSG_SIZE];
char guess[64];
char via[OPTMSG_SIZE];
char stopped[64];

    tableP = passP->tableP;
    jmpP = findingP->wordsP[0];
    tP = &passP->chn[jmpP->bank];
    ++tP->sites;
    guess[0] = '\0';
    stopped[0] = '\0';

    // The first intermediate is the one T3 already checked.
    chainPP[0] = wordAt(tableP, jmpP->bank, jmpP->decode.address);
    end = optChainWalk(tableP, jmpP, ~0u, chainPP, &hops, &stopP);

    handsOff = (((jmpP->flags | chainPP[0]->flags) & OPTF_HANDSOFF) != 0);
    bits = (optGuessWordBits(tableP, jmpP) | optGuessWordBits(tableP, chainPP[0]));
    save = 0;

    for( i = 1; i < hops; ++i )
    {
        save += optWordTime(chainPP[i]);
    }

    if( end == OPTCHAIN_CYCLE )
    {
        why = SC_CYCLE;
    }
    else if( end == OPTCHAIN_LONG )
    {
        why = SC_LONG;
    }
    else if( hops < 2 )
    {
        why = SC_ONEHOP;
    }
    else if( handsOff )
    {
        why = SC_HANDSOFF;
    }
    else if( bits )
    {
        guessText(bits, guess, sizeof(guess));
        why = SC_GUESSED;
    }
    else
    {
        why = SC_ELIGIBLE;
        tP->extra += (hops - 1);
        tP->save += save;
    }

    ++tP->why[why];

    used = 0;
    via[0] = '\0';

    for( i = 0; (i < hops) && (used < (OPTMSG_SIZE - 16)); ++i )
    {
        used += snprintf((via + used), (size_t)(OPTMSG_SIZE - used), " %04o", chainPP[i]->addr);
    }

    if( stopP )
    {
        snprintf(stopped, sizeof(stopped), " stopped %04o %s", stopP->addr,
            (stopP->flags & OPTF_HANDSOFF)?"handsoff":"guessed");
    }

    if( why == SC_ELIGIBLE )
    {
        fprintf(passP->fP, "chain b%d %04o eligible - | hops %d extra %d save %d via%s%s | %s\n", jmpP->bank,
            jmpP->addr, hops, (hops - 1), save, via, stopped, whereOf(jmpP, where, sizeof(where)));
    }
    else
    {
        fprintf(passP->fP, "chain b%d %04o refused %s%s | hops %d via%s%s | %s\n", jmpP->bank, jmpP->addr,
            chainNames[why], guess, hops, via, stopped, whereOf(jmpP, where, sizeof(where)));
    }

    ++tP->lines;
}

// S4: fall-through placement.

// Judge every placement site in bank then address order, count shared targets
// for context, then count mutually conflicting eligible pairs.
static void
measurePlace(SpPass *passP)
{
OptTableP tableP;
OptWordP wordP;
SpRun *aP;
SpRun *bP;
int bank;
int addr;
int a;
int b;

    tableP = passP->tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            switch( optPlaceSiteKind(tableP, wordP) )
            {
            case 2:
                placeSite(passP, wordP);
                break;

            case 1:
                ++passP->plc[bank].shared;
                break;

            default:
                break;
            }
        }
    }

    // Two eligible sites each inside the other's run cannot both move.
    for( a = 0; a < passP->runCount; ++a )
    {
        aP = &passP->runsP[a];

        for( b = (a + 1); b < passP->runCount; ++b )
        {
            bP = &passP->runsP[b];

            if( (aP->bank == bP->bank) &&
                (aP->jmpAddr >= bP->runStart) && (aP->jmpAddr <= bP->runEnd) &&
                (bP->jmpAddr >= aP->runStart) && (bP->jmpAddr <= aP->runEnd) )
            {
                ++passP->plc[aP->bank].mutual;
            }
        }
    }
}

// Classify a word as a placement site; shared with the rewrite.  Returns 2 for
// a site; 1 for a direct jmp whose target block is also entered some other
// way; 0 otherwise.
int
optPlaceSiteKind(OptTableP tableP, OptWordP wordP)
{
OptWordP targetP;
OptFlowEdgeP flowP;

    if( !isDirectJmp(wordP) || !isReached(wordP) || (wordP->blockP->lastP != wordP) ||
        (wordP->flags & (OPTF_PATCHED | OPTF_WRITTEN)) )
    {
        return(0);
    }

    targetP = wordAt(tableP, wordP->bank, wordP->decode.address);

    // Not a site: a target outside the graph, a jmp to the next word (T13's),
    // or a jump to its own block.
    if( !targetP || !inGraph(targetP) || !targetP->blockP || (targetP->blockP->startAddr != targetP->addr) ||
        (targetP->addr == (wordP->addr + 1)) || (targetP->blockP == wordP->blockP) )
    {
        return(0);
    }

    // The target block's one predecessor must be the jmp's block.
    if( targetP->blockP->predCount != 1 )
    {
        return(1);
    }

    for( flowP = wordP->blockP->succP; flowP; flowP = flowP->nextP )
    {
        if( flowP->toP == targetP->blockP )
        {
            return(2);
        }
    }

    return(1);
}

// Judge one placement site into *sP: the first refusal (afterskip, jmpused,
// entry, inrun, falls), else the run and its figures, with the guess recorded
// in guessBits but not applied.  A used jmp is refused because once deleted its
// users would find the run's first word in its place.
void
optPlaceJudge(OptTableP tableP, OptWordP jmpP, OptPlaceShapeP sP)
{
OptWordP targetP;
OptWordP wordP;
OptBlockP blockP;
int bank;
int runEnd;
int steps;
int addr;

    memset(sP, 0, sizeof(*sP));
    bank = jmpP->bank;
    targetP = wordAt(tableP, bank, jmpP->decode.address);
    sP->jmpP = jmpP;
    sP->targetP = targetP;
    runEnd = -1;

    if( jmpP->flags & OPTF_AFTERSKIP )
    {
        sP->why = SPL_AFTERSKIP;
        return;
    }

    if( jmpP->flags & (OPTF_READ | OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN |
        OPTF_MAYBE_ENTERED) )
    {
        sP->why = SPL_JMPUSED;
        return;
    }

    if( (targetP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_ENTERED)) || targetP->blockP->isEntry )
    {
        sP->why = SPL_ENTRY;
        return;
    }

    // The run: address-contiguous blocks from the target until one ends with
    // a jmp.  A halt resumes at the next word on Continue, so a run reaching
    // one falls.  A jmp just past a skip ends no run, since the skip passes it
    // ("sad [1 / jmp a / jmp b" moves as a whole or not at all).
    blockP = targetP->blockP;

    for( steps = 0; steps < SP_MAXWALK; ++steps )
    {
        if( (jmpP->addr >= blockP->startAddr) && (jmpP->addr <= blockP->endAddr) )
        {
            sP->why = SPL_INRUN;
            return;
        }

        if( (blockP->endKind == OPTBE_JUMP) && !(blockP->lastP->flags & OPTF_AFTERSKIP) )
        {
            runEnd = blockP->endAddr;
            break;
        }

        if( (blockP->endKind == OPTBE_HALT) || (blockP->endKind == OPTBE_NOTCODE) ||
            (blockP->endKind == OPTBE_GAP) ||
            !(wordP = wordAt(tableP, bank, (blockP->endAddr + 1))) || !inGraph(wordP) || !wordP->blockP )
        {
            sP->why = SPL_FALLS;
            return;
        }

        blockP = wordP->blockP;
    }

    if( runEnd < 0 )
    {
        sP->why = SPL_FALLS;
        return;
    }

    sP->endP = wordAt(tableP, bank, runEnd);
    sP->run = ((runEnd - targetP->addr) + 1);
    sP->save = optWordTime(jmpP);
    sP->guessBits = optGuessWordBits(tableP, jmpP);

    for( addr = targetP->addr; addr <= runEnd; ++addr )
    {
        sP->guessBits |= optGuessWordBits(tableP, wordAt(tableP, bank, addr));
    }

    sP->eligible = 1;
    sP->why = (sP->guessBits)?SPL_GUESSED:SPL_ELIGIBLE;
}

// Name a placement reason as the dump prints it.
// Returns a static string, "?" out of range.
const char *
optPlaceWhyName(int why)
{
    if( (why < 0) || (why >= SPL_COUNT) )
    {
        return("?");
    }

    return(placeNames[why]);
}

// Give the placement reason that is the guessed class, not a refusal.
// Returns SPL_AFTERSKIP.
int
optPlaceAfterSkip(void)
{
    return(SPL_AFTERSKIP);
}

// Judge one jmp whose target run nothing else enters, and write its line.
static void
placeSite(SpPass *passP, OptWordP jmpP)
{
OptTableP tableP;
SpPlaceTally *tP;
OptWordP targetP;
OptPlaceShape shape;
SpPlaceWhy why;
int bank;
int runEnd;
char where[OPTMSG_SIZE];
char guess[64];

    tableP = passP->tableP;
    bank = jmpP->bank;
    tP = &passP->plc[bank];
    ++tP->sites;
    guess[0] = '\0';

    optPlaceJudge(tableP, jmpP, &shape);
    why = (SpPlaceWhy)shape.why;
    targetP = shape.targetP;
    runEnd = (shape.endP)?shape.endP->addr:-1;

    if( why == SPL_GUESSED )
    {
        guessText(shape.guessBits, guess, sizeof(guess));
    }

    ++tP->why[why];

    if( why == SPL_ELIGIBLE )
    {
        tP->moved += ((runEnd - targetP->addr) + 1);
        tP->save += optWordTime(jmpP);

        if( passP->runCount == passP->runCap )
        {
            passP->runCap = (passP->runCap)?(passP->runCap * 2):64;

            if( !(passP->runsP = (SpRun *)realloc(passP->runsP, ((size_t)passP->runCap * sizeof(SpRun)))) )
            {
                fprintf(stderr, "am1: out of memory measuring the optimizer's speed candidates\n");
                exit(1);
            }
        }

        passP->runsP[passP->runCount].bank = bank;
        passP->runsP[passP->runCount].jmpAddr = jmpP->addr;
        passP->runsP[passP->runCount].runStart = targetP->addr;
        passP->runsP[passP->runCount].runEnd = runEnd;
        ++passP->runCount;

        fprintf(passP->fP, "place b%d %04o eligible - | target %04o %s run %d save %d | %s\n", bank, jmpP->addr,
            targetP->addr, firstLabelName(targetP), ((runEnd - targetP->addr) + 1), optWordTime(jmpP),
            whereOf(jmpP, where, sizeof(where)));
    }
    else
    {
        fprintf(passP->fP, "place b%d %04o %s %s%s | target %04o %s | %s\n", bank, jmpP->addr,
            (why == SPL_AFTERSKIP)?"guess":"refused", placeNames[why], guess, targetP->addr,
            firstLabelName(targetP), whereOf(jmpP, where, sizeof(where)));
    }

    ++tP->lines;
}

// The tallies, the budget and the reconcile line.

// Write the per-bank and total tallies, checking that every site was judged
// and written once and every live T3 finding followed.
// Returns 1 when everything adds up, 0 when anything does not.
static int
writeTotals(SpPass *passP)
{
FILE *fP;
SpInlineTally inl;
SpUnrollTally unr;
SpChainTally chn;
SpPlaceTally plc;
SpInlineTally *iP;
SpUnrollTally *uP;
SpChainTally *cP;
SpPlaceTally *pP;
char label[16];
int bank;
int w;
int h;
int sum;
int bad;
int pass;

    fP = passP->fP;
    bad = 0;

    // Pass 0 writes each bank that has a site; pass 1 sums every bank into one
    // row and writes it as "total".
    memset(&inl, 0, sizeof(inl));
    memset(&unr, 0, sizeof(unr));
    memset(&chn, 0, sizeof(chn));
    memset(&plc, 0, sizeof(plc));

    for( pass = 0; pass < 2; ++pass )
    {
        for( bank = 0; bank <= MAXBANK; ++bank )
        {
            if( pass == 0 )
            {
                iP = &passP->inl[bank];
                uP = &passP->unr[bank];
                cP = &passP->chn[bank];
                pP = &passP->plc[bank];
                snprintf(label, sizeof(label), "b%d", bank);

                if( !iP->sites && !uP->sites && !cP->sites && !pP->sites && !pP->shared )
                {
                    continue;
                }

                // Accumulate the total.
                inl.sites += iP->sites;
                inl.lines += iP->lines;
                inl.single += iP->single;
                inl.spent += iP->spent;
                inl.saveBest += iP->saveBest;
                inl.saveWorst += iP->saveWorst;
                unr.sites += uP->sites;
                unr.lines += uP->lines;
                unr.spent += uP->spent;
                unr.save += uP->save;
                chn.sites += cP->sites;
                chn.lines += cP->lines;
                chn.extra += cP->extra;
                chn.save += cP->save;
                plc.sites += pP->sites;
                plc.lines += pP->lines;
                plc.moved += pP->moved;
                plc.save += pP->save;
                plc.shared += pP->shared;
                plc.mutual += pP->mutual;

                for( w = 0; w < SI_COUNT; ++w )
                {
                    inl.why[w] += iP->why[w];
                }

                for( h = 0; h < SP_HISTCLASSES; ++h )
                {
                    inl.histCount[h] += iP->histCount[h];
                    inl.histSpent[h] += iP->histSpent[h];
                }

                for( w = 0; w < SU_COUNT; ++w )
                {
                    unr.why[w] += uP->why[w];
                }

                for( w = 0; w < SC_COUNT; ++w )
                {
                    chn.why[w] += cP->why[w];
                }

                for( w = 0; w < SPL_COUNT; ++w )
                {
                    plc.why[w] += pP->why[w];
                }
            }
            else
            {
                if( bank > 0 )
                {
                    break;
                }

                iP = &inl;
                uP = &unr;
                cP = &chn;
                pP = &plc;
                snprintf(label, sizeof(label), "total");
            }

            fprintf(fP, "inline %s: sites %d eligible %d single %d spent %d save %d..%d; refused", label,
                iP->sites, iP->why[SI_ELIGIBLE], iP->single, iP->spent, iP->saveBest, iP->saveWorst);

            for( w = 0; w < SI_ELIGIBLE; ++w )
            {
                fprintf(fP, " %s %d", inlineNames[w], iP->why[w]);
            }

            fprintf(fP, "\n");
            fprintf(fP, "inline %s: by body size 1-4 %d (spent %d), 5-8 %d (spent %d), 9-16 %d (spent %d), 17+ %d (spent %d)\n",
                label, iP->histCount[0], iP->histSpent[0], iP->histCount[1], iP->histSpent[1],
                iP->histCount[2], iP->histSpent[2], iP->histCount[3], iP->histSpent[3]);

            fprintf(fP, "unroll %s: sites %d eligible %d spent %d save %d; refused", label, uP->sites,
                uP->why[SU_ELIGIBLE], uP->spent, uP->save);

            for( w = 0; w < SU_ELIGIBLE; ++w )
            {
                fprintf(fP, " %s %d", unrollNames[w], uP->why[w]);
            }

            fprintf(fP, "\n");
            fprintf(fP, "chain %s: sites %d eligible %d extra %d save %d; refused", label, cP->sites,
                cP->why[SC_ELIGIBLE], cP->extra, cP->save);

            for( w = 0; w < SC_ELIGIBLE; ++w )
            {
                if( (w == SC_LONG) && !cP->why[w] )
                {
                    continue;   // expected never to fire
                }

                fprintf(fP, " %s %d", chainNames[w], cP->why[w]);
            }

            fprintf(fP, "\n");
            fprintf(fP, "place %s: sites %d eligible %d words %d save %d moved %d; guess afterskip %d; refused",
                label, pP->sites, pP->why[SPL_ELIGIBLE], pP->why[SPL_ELIGIBLE], pP->save, pP->moved,
                pP->why[SPL_AFTERSKIP]);

            for( w = (SPL_AFTERSKIP + 1); w < SPL_ELIGIBLE; ++w )
            {
                fprintf(fP, " %s %d", placeNames[w], pP->why[w]);
            }

            fprintf(fP, "; mutual %d; not sites: shared targets %d\n", pP->mutual, pP->shared);

            // Every site judged once and written once.
            for( sum = 0, w = 0; w < SI_COUNT; ++w )
            {
                sum += iP->why[w];
            }

            bad |= ((sum != iP->sites) || (iP->lines != iP->sites));

            for( sum = 0, w = 0; w < SU_COUNT; ++w )
            {
                sum += uP->why[w];
            }

            bad |= ((sum != uP->sites) || (uP->lines != uP->sites));

            for( sum = 0, w = 0; w < SC_COUNT; ++w )
            {
                sum += cP->why[w];
            }

            bad |= ((sum != cP->sites) || (cP->lines != cP->sites));

            for( sum = 0, w = 0; w < SPL_COUNT; ++w )
            {
                sum += pP->why[w];
            }

            bad |= ((sum != pP->sites) || (pP->lines != pP->sites));

            // The histogram must hold every eligible inline site and its words.
            for( sum = 0, h = 0; h < SP_HISTCLASSES; ++h )
            {
                sum += iP->histCount[h];
            }

            bad |= (sum != iP->why[SI_ELIGIBLE]);

            for( sum = 0, h = 0; h < SP_HISTCLASSES; ++h )
            {
                sum += iP->histSpent[h];
            }

            bad |= (sum != iP->spent);
        }
    }

    // The live T3 findings must all have been judged.
    bad |= (chn.sites != passP->tableP->findingCounts[OPTRULE_T3]);

    return(!bad);
}

// Write the bank budget: per bank that emitted a word, the highest address
// used, the load-time ceiling, the words free above the highest address, and
// what each candidate spends against them.
static void
writeBudget(SpPass *passP)
{
OptTableP tableP;
OptWordP wordP;
int highest[MAXBANK + 1];
int bank;
int i;
int ceiling;
int freeWords;

    tableP = passP->tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        highest[bank] = -1;
    }

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( wordP->addr > highest[wordP->bank] )
        {
            highest[wordP->bank] = wordP->addr;
        }
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( highest[bank] < 0 )
        {
            continue;
        }

        ceiling = optBankCeiling(tableP, bank);
        freeWords = (ceiling - highest[bank]);

        // "(declared)" only for a bank with a %%ceiling.
        fprintf(passP->fP, "budget b%d: highest %04o ceiling %04o%s free %d; inline spent %d %s; unroll spent %d %s; "
            "place frees %d; chain 0\n", bank, highest[bank], ceiling,
            (optBankCeilingDeclared(tableP, bank))?" (declared)":"", freeWords,
            passP->inl[bank].spent, (passP->inl[bank].spent <= freeWords)?"fits":"OVER",
            passP->unr[bank].spent, (passP->unr[bank].spent <= freeWords)?"fits":"OVER",
            passP->plc[bank].why[SPL_ELIGIBLE]);
    }
}
