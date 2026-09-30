/*
 * The am1 optimizer: where a call actually returns to.  Every call gets a
 * return edge to the word after it, which is wrong for the inline-argument
 * convention, where the callee reads argument words that follow the call and
 * steps its saved return address past them:
 *
 *       jsp i [subr:0]
 *       ARG:0                   // argument, read by the callee through rtn
 *       ...                     // the callee returns HERE
 *
 * For each call site this walks the callee and decides whether it returns to
 * call+1, to call+1+N, or cannot be told; an unknown site keeps its call+1
 * edge and records the reason.  Runs inside optBuildFlow() (optflow.c) before
 * any block is cut, because a stepped return target must begin a block.  It
 * reads the word table, the decode and the reference edges (optrefs.c), and
 * writes only the return fields of OptWord and OptTable.  Uses inGraph(),
 * wordAt() and optTerminator() from optflow.c so a callee is cut exactly as
 * the graph will cut it.  Each callee is walked once and cached by entry
 * address.
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

// The three cases, and what each demands before it may be claimed:
//
// 1. Returns after the call: the entry word saves the return address with a
//    direct dac or dap, and no path to the return changes it.  Edge to call+1.
// 2. Steps past N argument words: every path bumps the saved return address
//    by the same N before returning through it.  Edge to call+1+N.
// 3. Undecidable: paths disagree, the return word is written by something
//    this cannot follow, or the callee is unknown.  The edge stays at call+1.
//    Inventing an edge here is unsound: a caller's tail that looks unreached
//    makes the routines it calls look uncalled.
//
// This must be a walk of every path, not a scan: a callee with an early-out
// arm that skips the bump is case 3.  A routine may idx other words, such as
// an argument-block pointer, and never its return word, so each idx is asked
// which word it names.

// Memory reference opcodes as the handbook writes them (bits 0-4, bit 5
// clear), numbered as in optdecode.c.
#define RETOP_AND       0002
#define RETOP_CAL       0016    // and jda, when bit 5 is set
#define RETOP_LAC       0020
#define RETOP_DAC       0024
#define RETOP_DAP       0026
#define RETOP_DIP       0030
#define RETOP_DIO       0032
#define RETOP_DZM       0034
#define RETOP_ADD       0040
#define RETOP_IDX       0044
#define RETOP_ISP       0046
#define RETOP_JMP       0060
#define RETOP_JSP       0062

// Per-callee step budget, so a pathological program cannot hang the
// assembler; far above what any real walk needs.
#define RET_MAXSTEPS        200000

// A larger inline argument count means the analysis has not understood the
// routine, so it is refused rather than believed.
#define RET_MAXARGS         63

// Nested-call depth limit; a path through a nested call resumes after that
// call, so its return must be resolved too.
#define RET_MAXDEPTH        16

// A (bank, address) pair packed into one int: twelve address bits, bank above.
#define RET_KEY(b, a)       ((((b) << 12) | ((a) & ADDRMASK)))
#define RET_KEYBANK(k)      (((k) >> 12))
#define RET_KEYADDR(k)      (((k) & ADDRMASK))

// The state of one callee in the cache.
#define RETC_UNSEEN     0       // never asked about
#define RETC_BUSY       1       // being walked now: asking again is recursion
#define RETC_DONE       2       // walked, and the answer below is it

// One callee's cached answer, indexed by the address of its entry word.
typedef struct
{
    unsigned char state;    // RETC_UNSEEN, RETC_BUSY or RETC_DONE
    signed char kase;       // an OptReturnCase
    signed char reason;     // an OptReturnReason
    short steps;            // argument words stepped past, 0 unless kase is STEPPED
} RetCallee, *RetCalleeP;

// The cache: one array per bank that emitted words, indexed by address.
typedef struct
{
    RetCalleeP banksPP[MAXBANK + 1];    // NILP for a bank that emitted nothing
    int calleeCount;                    // distinct callees walked, for the statistics
} RetCache, *RetCacheP;

// One callee's walk.  seenPP holds the step count the walk first reached each
// address with, or -1; okPP marks words whose write of the return word has been
// accounted for.  Both are allocated only for banks the walk enters.
typedef struct
{
    int *seenPP[MAXBANK + 1];
    unsigned char *okPP[MAXBANK + 1];
    int *stackP;            // the depth-first stack: packed keys
    int *stackStepsP;       // and the step count each was pushed with
    int stackTop;
    int stackCap;
    int budget;             // steps left before RET_MAXSTEPS is reached
} RetWalk, *RetWalkP;

static void resolveCallee(OptTableP tableP, RetCacheP cacheP, int depth, int bank, int addr, RetCalleeP outP);
static void walkCallee(OptTableP tableP, RetCacheP cacheP, int depth, OptWordP entryP, RetCalleeP outP);
static void classifySite(OptTableP tableP, RetCacheP cacheP, OptWordP entryP);
static int calleeEntryOf(OptTableP tableP, OptWordP entryP, int *bankP, int *addrP);
static int refersDirectly(OptWordP entryP, int opcode, int bank, int addr);
static int writesDirectly(OptWordP entryP, int bank, int addr);
static int operandValue(OptTableP tableP, OptWordP entryP, int *valueP);
static int enteredFromElsewhere(OptWordP entryP);
static int matchBumpTriple(OptTableP tableP, OptWordP lacP, int rtnAddr, int *deltaP);
static int accountedWriters(OptTableP tableP, RetWalkP walkP, OptWordP rtnP, OptWordP saveP);
static RetCacheP newCache(OptTableP tableP);
static void freeCache(RetCacheP cacheP);
static void initWalk(RetWalkP walkP);
static void freeWalk(RetWalkP walkP);
static int walkBankMaps(OptTableP tableP, RetWalkP walkP, int bank);
static int walkSeen(OptTableP tableP, RetWalkP walkP, int bank, int addr);
static void walkSetSeen(OptTableP tableP, RetWalkP walkP, int bank, int addr, int steps);
static void walkAccount(OptTableP tableP, RetWalkP walkP, int bank, int addr);
static int walkIsAccounted(RetWalkP walkP, int bank, int addr);
static void walkPush(RetWalkP walkP, int bank, int addr, int steps);

// Classify every call site's return edge.  Clears the return fields of every
// word first, so a second run over the same table gives the same answer.
void
optResolveReturns(OptTableP tableP)
{
int i;
OptWordP entryP;
RetCacheP cacheP;

    if( !tableP )
    {
        return;
    }

    tableP->returnSiteCount = 0;
    tableP->returnJdaSites = 0;
    tableP->returnMovedSites = 0;
    tableP->returnCalleeCount = 0;
    memset(tableP->returnCaseCounts, 0, sizeof(tableP->returnCaseCounts));
    memset(tableP->returnReasonCounts, 0, sizeof(tableP->returnReasonCounts));

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        entryP->isCallSite = 0;
        entryP->returnAddr = 0;
        entryP->returnSteps = 0;
        entryP->calleeBank = -1;
        entryP->calleeAddr = -1;
        entryP->returnCase = OPTRC_UNKNOWN;
        entryP->returnReason = OPTRR_NONE;
    }

    cacheP = newCache(tableP);

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !inGraph(entryP) || (optTerminator(entryP) != OPTBE_CALL) )
        {
            continue;
        }

        classifySite(tableP, cacheP, entryP);
    }

    tableP->returnCalleeCount = cacheP->calleeCount;
    freeCache(cacheP);
}

// Classify one call word: find its callee, resolve what the callee does with
// its return word, and record the answer on the word.
static void
classifySite(OptTableP tableP, RetCacheP cacheP, OptWordP entryP)
{
int cBank;
int cAddr;
RetCallee answer;

    entryP->isCallSite = 1;
    ++tableP->returnSiteCount;

    // Twelve bits: the top of a bank wraps to its bottom, as in buildBlockEdges().
    entryP->returnAddr = ((entryP->addr + 1) & ADDRMASK);

    if( entryP->decode.opcode == RETOP_CAL )
    {
        ++tableP->returnJdaSites;
    }

    if( !calleeEntryOf(tableP, entryP, &cBank, &cAddr) )
    {
        entryP->returnCase = OPTRC_UNKNOWN;
        entryP->returnReason = OPTRR_CALLEE;
    }
    else
    {
        entryP->calleeBank = cBank;
        entryP->calleeAddr = cAddr;

        resolveCallee(tableP, cacheP, 0, cBank, cAddr, &answer);

        entryP->returnCase = (OptReturnCase)answer.kase;
        entryP->returnReason = (OptReturnReason)answer.reason;

        if( entryP->returnCase == OPTRC_STEPPED )
        {
            entryP->returnSteps = answer.steps;
            entryP->returnAddr = ((entryP->addr + 1 + answer.steps) & ADDRMASK);
            ++tableP->returnMovedSites;
        }
    }

    ++tableP->returnCaseCounts[entryP->returnCase];

    if( entryP->returnCase == OPTRC_UNKNOWN )
    {
        ++tableP->returnReasonCounts[entryP->returnReason];
    }
}

// Find a call's entry word: jsp's address field or, when indirect, the
// via-pointer jump edge; jda enters the word after the one it names; cal is
// jda 0100, entering 0101 of its own bank.  A patched address names nothing.
// Returns 1 with *bankP and *addrP set, 0 when there is no single known target.
static int
calleeEntryOf(OptTableP tableP, OptWordP entryP, int *bankP, int *addrP)
{
OptEdgeP edgeP;
int found;

    (void)tableP;

    if( entryP->flags & OPTF_PATCHED )
    {
        return(0);
    }

    if( entryP->decode.opcode == RETOP_CAL )
    {
        if( entryP->decode.indirect )
        {
            // jda: the routine's body starts one word past the word it names.
            *bankP = entryP->bank;
            *addrP = ((entryP->decode.address + 1) & ADDRMASK);
        }
        else
        {
            // cal: jda 0100, so the body is at 0101 of the calling bank.
            *bankP = entryP->bank;
            *addrP = CAL_JUMP_ADDR;
        }

        return(1);
    }

    if( !entryP->decode.memIndirect )
    {
        *bankP = entryP->bank;
        *addrP = entryP->decode.address;
        return(1);
    }

    // An indirect jsp resolved to more than one callee is refused: they need
    // not agree.
    found = 0;

    for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role != OPTR_JUMP) || !(edgeP->flags & OPTEF_VIAPOINTER) )
        {
            continue;
        }

        if( found )
        {
            return(0);
        }

        *bankP = edgeP->toBank;
        *addrP = edgeP->toAddr;
        found = 1;
    }

    return(found);
}

// Resolve one callee, from the cache or by walking it.  A callee reached
// again while still being walked is recursion and answers "nested".
static void
resolveCallee(OptTableP tableP, RetCacheP cacheP, int depth, int bank, int addr, RetCalleeP outP)
{
RetCalleeP slotP;
OptWordP entryP;

    outP->state = RETC_DONE;
    outP->kase = OPTRC_UNKNOWN;
    outP->reason = OPTRR_CALLEE;
    outP->steps = 0;

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        return;
    }

    if( !(slotP = cacheP->banksPP[bank]) )
    {
        return;
    }

    slotP += addr;

    if( slotP->state == RETC_DONE )
    {
        *outP = *slotP;
        return;
    }

    if( slotP->state == RETC_BUSY )
    {
        outP->reason = OPTRR_NESTED;
        return;
    }

    if( depth >= RET_MAXDEPTH )
    {
        outP->reason = OPTRR_NESTED;
        return;
    }

    if( !(entryP = wordAt(tableP, bank, addr)) || !inGraph(entryP) )
    {
        // Not a word believed executed, so no body to read; cache that.
        slotP->state = RETC_DONE;
        slotP->kase = OPTRC_UNKNOWN;
        slotP->reason = OPTRR_CALLEE;
        slotP->steps = 0;
        *outP = *slotP;
        ++cacheP->calleeCount;
        return;
    }

    slotP->state = RETC_BUSY;

    walkCallee(tableP, cacheP, depth, entryP, outP);

    slotP->state = RETC_DONE;
    slotP->kase = outP->kase;
    slotP->reason = outP->reason;
    slotP->steps = outP->steps;
    ++cacheP->calleeCount;
}

// Walk one callee from its entry word and decide what it does with its saved
// return address.  The two jsp-idiom shapes are recognized:
//
//   a. a data word:   dac rtn ... idx rtn ... jmp i rtn      rtn, 0
//   b. a patched jmp: dap rtn ... idx rtn ... falls into     rtn, jmp .
//
// In shape b executing the return word is the return, so reaching its address
// ends a path.  Every path must agree on the step count; reaching an address
// twice with different counts ends the analysis, which also guarantees the
// walk terminates.
static void
walkCallee(OptTableP tableP, RetCacheP cacheP, int depth, OptWordP entryP, RetCalleeP outP)
{
RetWalk walk;
RetCallee nested;
OptWordP wordP;
OptWordP rtnP;
OptEdgeP edgeP;
OptBlockEnd end;
int rtnBank;
int rtnAddr;
int bank;
int addr;
int steps;
int seen;
int delta;
int returnSteps;
int returned;
int made;
int nestBank;
int nestAddr;

    outP->state = RETC_DONE;
    outP->kase = OPTRC_UNKNOWN;
    outP->reason = OPTRR_NOSAVE;
    outP->steps = 0;

    // The entry word must save the return address with a direct, unpatched
    // dac or dap; otherwise there is no return word to follow.
    if( (entryP->decode.group != OPTG_MEMREF) ||
        ((entryP->decode.opcode != RETOP_DAC) && (entryP->decode.opcode != RETOP_DAP)) ||
        entryP->decode.memIndirect || (entryP->flags & OPTF_PATCHED) )
    {
        return;
    }

    rtnBank = entryP->bank;
    rtnAddr = entryP->decode.address;

    if( !(rtnP = wordAt(tableP, rtnBank, rtnAddr)) || (rtnP->flags & (OPTF_RESERVED | OPTF_DUPADDR)) )
    {
        return;
    }

    // An unfollowed pointer write may land on the return word unseen.
    if( rtnP->flags & OPTF_MAYBE_WRITTEN )
    {
        outP->reason = OPTRR_CLOBBER;
        return;
    }

    initWalk(&walk);

    // The entry word's store is the save, not a modification.
    walkAccount(tableP, &walk, entryP->bank, entryP->addr);

    returnSteps = -1;
    returned = 0;
    outP->reason = OPTRR_NORETURN;

    walkPush(&walk, entryP->bank, ((entryP->addr + 1) & ADDRMASK), 0);

    while( walk.stackTop > 0 )
    {
        if( --walk.budget <= 0 )
        {
            outP->reason = OPTRR_BUDGET;
            freeWalk(&walk);
            return;
        }

        --walk.stackTop;
        bank = RET_KEYBANK(walk.stackP[walk.stackTop]);
        addr = RET_KEYADDR(walk.stackP[walk.stackTop]);
        steps = walk.stackStepsP[walk.stackTop];

        if( (seen = walkSeen(tableP, &walk, bank, addr)) >= 0 )
        {
            if( seen != steps )
            {
                outP->reason = OPTRR_DISAGREE;
                freeWalk(&walk);
                return;
            }

            continue;
        }

        walkSetSeen(tableP, &walk, bank, addr, steps);

        // Shape b.  Tested before decoding: the word holds a placeholder the
        // entry word overwrites.
        if( (bank == rtnBank) && (addr == rtnAddr) )
        {
            if( returned && (returnSteps != steps) )
            {
                outP->reason = OPTRR_DISAGREE;
                freeWalk(&walk);
                return;
            }

            returnSteps = steps;
            returned = 1;
            continue;
        }

        if( !(wordP = wordAt(tableP, bank, addr)) || !inGraph(wordP) )
        {
            outP->reason = OPTRR_LOST;
            freeWalk(&walk);
            return;
        }

        // Shape a: only "jmp i rtn" returns through the word.
        if( (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == RETOP_JMP) &&
            wordP->decode.memIndirect && !(wordP->flags & OPTF_PATCHED) &&
            (wordP->bank == rtnBank) && (wordP->decode.address == rtnAddr) )
        {
            if( returned && (returnSteps != steps) )
            {
                outP->reason = OPTRR_DISAGREE;
                freeWalk(&walk);
                return;
            }

            returnSteps = steps;
            returned = 1;
            continue;
        }

        // "lac rtn / add [N] / dac rtn" or its masking form, as one step.
        // Tested before the write test below so its dac counts as accounted.
        if( refersDirectly(wordP, RETOP_LAC, rtnBank, rtnAddr) &&
            matchBumpTriple(tableP, wordP, rtnAddr, &delta) )
        {
            if( (steps + delta) > RET_MAXARGS )
            {
                outP->reason = OPTRR_CLOBBER;
                freeWalk(&walk);
                return;
            }

            // matchBumpTriple() proved nothing else enters these two words,
            // so marking them seen cannot hide a path.
            walkSetSeen(tableP, &walk, bank, ((addr + 1) & ADDRMASK), steps);
            walkSetSeen(tableP, &walk, bank, ((addr + 2) & ADDRMASK), steps);
            walkAccount(tableP, &walk, bank, ((addr + 2) & ADDRMASK));
            walkPush(&walk, bank, ((addr + 3) & ADDRMASK), (steps + delta));
            continue;
        }

        // Any other write of the return word must be idx or isp.
        if( writesDirectly(wordP, rtnBank, rtnAddr) )
        {
            if( (wordP->decode.opcode != RETOP_IDX) && (wordP->decode.opcode != RETOP_ISP) )
            {
                outP->reason = OPTRR_CLOBBER;
                freeWalk(&walk);
                return;
            }

            if( (steps + 1) > RET_MAXARGS )
            {
                outP->reason = OPTRR_CLOBBER;
                freeWalk(&walk);
                return;
            }

            walkAccount(tableP, &walk, bank, addr);
            ++steps;
        }

        // Follow the same terminators and targets the graph uses.
        end = optTerminator(wordP);

        if( end == OPTBE_COUNT )
        {
            // An xct falls through here, as it does in the graph.
            walkPush(&walk, bank, ((addr + 1) & ADDRMASK), steps);
            continue;
        }

        if( end == OPTBE_SKIP )
        {
            walkPush(&walk, bank, ((addr + 1) & ADDRMASK), steps);
            walkPush(&walk, bank, ((addr + 2) & ADDRMASK), steps);
            continue;
        }

        if( end == OPTBE_HALT )
        {
            // Continue resumes at the next word, as in the graph.
            walkPush(&walk, bank, ((addr + 1) & ADDRMASK), steps);
            continue;
        }

        if( end == OPTBE_CALL )
        {
            // The path resumes wherever the nested call returns to.
            if( !calleeEntryOf(tableP, wordP, &nestBank, &nestAddr) )
            {
                outP->reason = OPTRR_NESTED;
                freeWalk(&walk);
                return;
            }

            resolveCallee(tableP, cacheP, (depth + 1), nestBank, nestAddr, &nested);

            if( nested.kase == OPTRC_UNKNOWN )
            {
                outP->reason = OPTRR_NESTED;
                freeWalk(&walk);
                return;
            }

            walkPush(&walk, bank, ((addr + 1 + nested.steps) & ADDRMASK), steps);
            continue;
        }

        // OPTBE_JUMP.  A patched jump has a run-time target; an indirect one
        // is followed only where the reference pass followed its pointer.
        if( wordP->flags & OPTF_PATCHED )
        {
            outP->reason = OPTRR_LOST;
            freeWalk(&walk);
            return;
        }

        if( wordP->decode.memIndirect )
        {
            made = 0;

            for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
            {
                if( (edgeP->role != OPTR_JUMP) || !(edgeP->flags & OPTEF_VIAPOINTER) )
                {
                    continue;
                }

                walkPush(&walk, edgeP->toBank, edgeP->toAddr, steps);
                ++made;
            }

            if( !made )
            {
                outP->reason = OPTRR_LOST;
                freeWalk(&walk);
                return;
            }

            continue;
        }

        walkPush(&walk, bank, wordP->decode.address, steps);
    }

    if( !returned )
    {
        outP->reason = OPTRR_NORETURN;
        freeWalk(&walk);
        return;
    }

    // Every writer of the return word must have been accounted for.  The
    // walk sees only its own paths; the reference in-edges see every writer,
    // including one in another routine.
    if( !accountedWriters(tableP, &walk, rtnP, entryP) )
    {
        outP->reason = OPTRR_CLOBBER;
        freeWalk(&walk);
        return;
    }

    outP->kase = (returnSteps > 0)?OPTRC_STEPPED:OPTRC_AFTER;
    outP->reason = OPTRR_NONE;
    outP->steps = (short)returnSteps;

    freeWalk(&walk);
}

// Check that every writer of the return word was accounted for by this walk.
// The in-edges include a conservative edge for each unfollowed pointer store.
// Returns 1 when every writer is accounted for, 0 when one is not.
static int
accountedWriters(OptTableP tableP, RetWalkP walkP, OptWordP rtnP, OptWordP saveP)
{
OptEdgeP edgeP;

    (void)tableP;

    for( edgeP = rtnP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->role != OPTR_WRITE )
        {
            continue;
        }

        if( !edgeP->fromP )
        {
            return(0);
        }

        if( edgeP->fromP == saveP )
        {
            continue;
        }

        if( !walkIsAccounted(walkP, edgeP->fromP->bank, edgeP->fromP->addr) )
        {
            return(0);
        }
    }

    return(1);
}

// Test whether a word names bank/addr directly with the given opcode, no
// indirection and no run-time patch.
// Returns 1 when it does, 0 otherwise or for NILP.
static int
refersDirectly(OptWordP entryP, int opcode, int bank, int addr)
{
    if( !entryP || (entryP->decode.group != OPTG_MEMREF) )
    {
        return(0);
    }

    if( entryP->decode.opcode != opcode )
    {
        return(0);
    }

    if( entryP->decode.memIndirect || (entryP->flags & OPTF_PATCHED) )
    {
        return(0);
    }

    if( (entryP->bank != bank) || (entryP->decode.address != addr) )
    {
        return(0);
    }

    return(1);
}

// Test whether a word writes bank/addr directly: dac, dap, dip, dio, dzm, idx,
// isp, and jda.  Pointer writes are caught by OPTF_MAYBE_WRITTEN instead.
// Returns 1 when it does, 0 otherwise or for NILP.
static int
writesDirectly(OptWordP entryP, int bank, int addr)
{
    if( !entryP || (entryP->decode.group != OPTG_MEMREF) )
    {
        return(0);
    }

    if( entryP->decode.memIndirect || (entryP->flags & OPTF_PATCHED) )
    {
        return(0);
    }

    if( (entryP->bank != bank) || (entryP->decode.address != addr) )
    {
        return(0);
    }

    switch( entryP->decode.opcode )
    {
    case RETOP_DAC:
    case RETOP_DAP:
    case RETOP_DIP:
    case RETOP_DIO:
    case RETOP_DZM:
    case RETOP_IDX:
    case RETOP_ISP:
        return(1);

    case RETOP_CAL:
        // cal deposits in 0100, not the word its address field names.
        return( (entryP->decode.indirect)?1:0 );

    default:
        return(0);
    }
}

// Read the value a direct memory reference sees: the operand must exist, hold
// one value and never be written (a pool constant, not a variable).
// Returns 1 with *valueP set, 0 when the operand cannot be trusted.
static int
operandValue(OptTableP tableP, OptWordP entryP, int *valueP)
{
OptWordP operandP;

    if( entryP->decode.memIndirect || (entryP->flags & OPTF_PATCHED) )
    {
        return(0);
    }

    if( !(operandP = wordAt(tableP, entryP->bank, entryP->decode.address)) )
    {
        return(0);
    }

    if( operandP->flags & (OPTF_RESERVED | OPTF_DUPADDR) )
    {
        return(0);
    }

    if( operandP->flags & (OPTF_WRITTEN | OPTF_MAYBE_WRITTEN) )
    {
        return(0);
    }

    *valueP = (operandP->value & WRDMASK);

    return(1);
}

// Test whether control can reach a word other than by falling into it.
// Returns 1 when something else can enter the word, 0 when nothing can.
static int
enteredFromElsewhere(OptWordP entryP)
{
    if( !entryP )
    {
        return(1);
    }

    if( entryP->flags & (OPTF_JUMPTARGET | OPTF_TAKEN | OPTF_XCTTARGET | OPTF_MAYBE_ENTERED) )
    {
        return(1);
    }

    // A skip two words up jumps over this word, which OPTF_JUMPTARGET misses.
    if( entryP->flags & OPTF_AFTERSKIP )
    {
        return(1);
    }

    return(0);
}

// Match the three-word modification of the return word starting at "lac rtn":
//
//   lac rtn / add [N] / dac rtn     steps the return word on by N
//   lac rtn / and [M] / dac rtn     no step, when M's low twelve bits are all ones
//
// The masking form is how memset.ac and memcpy.ac clear the bank bits of a
// return address when extend mode is off; it moves the return nowhere.  The
// two words after the lac must be enterable only by falling in.
// Returns 1 with *deltaP set to the step, 0 when the shape does not match.
static int
matchBumpTriple(OptTableP tableP, OptWordP lacP, int rtnAddr, int *deltaP)
{
OptWordP opP;
OptWordP dacP;
int value;

    if( !(opP = wordAt(tableP, lacP->bank, ((lacP->addr + 1) & ADDRMASK))) || !inGraph(opP) )
    {
        return(0);
    }

    if( !(dacP = wordAt(tableP, lacP->bank, ((lacP->addr + 2) & ADDRMASK))) || !inGraph(dacP) )
    {
        return(0);
    }

    if( enteredFromElsewhere(opP) || enteredFromElsewhere(dacP) )
    {
        return(0);
    }

    if( !refersDirectly(dacP, RETOP_DAC, lacP->bank, rtnAddr) )
    {
        return(0);
    }

    if( (opP->decode.group != OPTG_MEMREF) || !operandValue(tableP, opP, &value) )
    {
        return(0);
    }

    if( opP->decode.opcode == RETOP_ADD )
    {
        // Only a small positive constant within the address field is a step.
        if( (value & ~ADDRMASK) || (value > RET_MAXARGS) )
        {
            return(0);
        }

        *deltaP = value;
        return(1);
    }

    if( opP->decode.opcode == RETOP_AND )
    {
        if( (value & ADDRMASK) != ADDRMASK )
        {
            return(0);
        }

        *deltaP = 0;
        return(1);
    }

    return(0);
}

// Allocate the callee cache, one array per bank that emitted words.
// Allocation failure is fatal.
static RetCacheP
newCache(OptTableP tableP)
{
RetCacheP cacheP;
int bank;

    if( !(cacheP = (RetCacheP)calloc(1, sizeof(RetCache))) )
    {
        fprintf(stderr, "am1: out of memory resolving the optimizer's return edges\n");
        exit(1);
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        if( !(cacheP->banksPP[bank] = (RetCalleeP)calloc(BANKSIZE, sizeof(RetCallee))) )
        {
            fprintf(stderr, "am1: out of memory resolving the optimizer's return edges\n");
            exit(1);
        }
    }

    return(cacheP);
}

// Release the callee cache and every bank array in it.
static void
freeCache(RetCacheP cacheP)
{
int bank;

    if( !cacheP )
    {
        return;
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        free(cacheP->banksPP[bank]);
    }

    free(cacheP);
}

// Start a walk: empty stack, nothing seen, full budget; per-bank maps are
// allocated as the walk enters each bank.
static void
initWalk(RetWalkP walkP)
{
    memset(walkP, 0, sizeof(RetWalk));
    walkP->budget = RET_MAXSTEPS;
    walkP->stackCap = 256;

    if( !(walkP->stackP = (int *)calloc(walkP->stackCap, sizeof(int))) ||
        !(walkP->stackStepsP = (int *)calloc(walkP->stackCap, sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory walking a callee for the optimizer's return edges\n");
        exit(1);
    }
}

// Release everything one callee's walk allocated.
static void
freeWalk(RetWalkP walkP)
{
int bank;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        free(walkP->seenPP[bank]);
        free(walkP->okPP[bank]);
        walkP->seenPP[bank] = NILP;
        walkP->okPP[bank] = NILP;
    }

    free(walkP->stackP);
    free(walkP->stackStepsP);
    walkP->stackP = NILP;
    walkP->stackStepsP = NILP;
}

// Ensure a bank's walk maps exist, with the seen map filled with -1.
// Returns 1 when the bank has maps, 0 when the bank emitted nothing.
static int
walkBankMaps(OptTableP tableP, RetWalkP walkP, int bank)
{
int addr;

    if( (bank < 0) || (bank > MAXBANK) )
    {
        return(0);
    }

    if( walkP->seenPP[bank] )
    {
        return(1);
    }

    if( !tableP->banksP[bank] )
    {
        return(0);
    }

    if( !(walkP->seenPP[bank] = (int *)calloc(BANKSIZE, sizeof(int))) ||
        !(walkP->okPP[bank] = (unsigned char *)calloc(BANKSIZE, sizeof(unsigned char))) )
    {
        fprintf(stderr, "am1: out of memory walking a callee for the optimizer's return edges\n");
        exit(1);
    }

    for( addr = 0; addr < BANKSIZE; ++addr )
    {
        walkP->seenPP[bank][addr] = -1;
    }

    return(1);
}

// Look up the step count the walk first reached an address with.
// Returns that count, or -1 when not reached or the bank emitted nothing.
static int
walkSeen(OptTableP tableP, RetWalkP walkP, int bank, int addr)
{
    if( !walkBankMaps(tableP, walkP, bank) )
    {
        return(-1);
    }

    return( walkP->seenPP[bank][addr & ADDRMASK] );
}

// Record the step count the walk reached an address with.  A bank that emitted
// nothing is ignored; the walk refuses that path at its inGraph() test.
static void
walkSetSeen(OptTableP tableP, RetWalkP walkP, int bank, int addr, int steps)
{
    if( !walkBankMaps(tableP, walkP, bank) )
    {
        return;
    }

    walkP->seenPP[bank][addr & ADDRMASK] = steps;
}

// Mark a word's write of the return word as accounted for.
static void
walkAccount(OptTableP tableP, RetWalkP walkP, int bank, int addr)
{
    if( !walkBankMaps(tableP, walkP, bank) )
    {
        return;
    }

    walkP->okPP[bank][addr & ADDRMASK] = 1;
}

// Test whether a word's write of the return word was accounted for.
// Returns 1 when it was, 0 when not or when its bank was never entered.
static int
walkIsAccounted(RetWalkP walkP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || !walkP->okPP[bank] )
    {
        return(0);
    }

    return( walkP->okPP[bank][addr & ADDRMASK] );
}

// Push an address and its step count on the walk stack, growing it as needed.
static void
walkPush(RetWalkP walkP, int bank, int addr, int steps)
{
int *newP;
int *newStepsP;

    if( walkP->stackTop >= walkP->stackCap )
    {
        walkP->stackCap *= 2;

        if( !(newP = (int *)realloc(walkP->stackP, (walkP->stackCap * sizeof(int)))) ||
            !(newStepsP = (int *)realloc(walkP->stackStepsP, (walkP->stackCap * sizeof(int)))) )
        {
            fprintf(stderr, "am1: out of memory walking a callee for the optimizer's return edges\n");
            exit(1);
        }

        walkP->stackP = newP;
        walkP->stackStepsP = newStepsP;
    }

    walkP->stackP[walkP->stackTop] = RET_KEY(bank, addr);
    walkP->stackStepsP[walkP->stackTop] = steps;
    ++walkP->stackTop;
}

// Name a return-edge case for the dump and the report.
// Returns a static string, never NILP.
const char *
optReturnCaseName(OptReturnCase kase)
{
    switch( kase )
    {
    case OPTRC_AFTER:
        return("after");

    case OPTRC_STEPPED:
        return("stepped");

    case OPTRC_UNKNOWN:
        return("unknown");

    default:
        return("unknown");
    }
}

// Name the reason an unknown return-edge case stopped.
// Returns a static string, never NILP.
const char *
optReturnReasonName(OptReturnReason reason)
{
    switch( reason )
    {
    case OPTRR_NONE:
        return("none");

    case OPTRR_CALLEE:
        return("notarget");

    case OPTRR_NOSAVE:
        return("nosave");

    case OPTRR_CLOBBER:
        return("clobber");

    case OPTRR_DISAGREE:
        return("disagree");

    case OPTRR_NORETURN:
        return("noreturn");

    case OPTRR_NESTED:
        return("nested");

    case OPTRR_LOST:
        return("lost");

    case OPTRR_BUDGET:
        return("budget");

    default:
        return("unknown");
    }
}
