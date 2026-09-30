/*
 * The am1 optimizer's routines: groups of basic blocks, since am1 has no
 * procedure directive.  An ENTRY is a block whose first word is the target of
 * a call edge (jsp, jsp i, jda or cal); the BODY is the blocks reachable from
 * it without passing through another entry; the RETURN is the words that
 * leave through jmp i rtn (jsp form) or jmp i subr (jda form).  An entry a
 * plain edge also enters, two entries sharing a tail, and a body whose
 * callers cannot be resolved are all counted, not decided; optimizer.h
 * describes each beside its flag.  Nothing here computes sharing or emits a
 * finding.
 *
 * Called single threaded from optBuildCallGraph() in optcallgraph.c, after
 * the control-flow graph is complete.  It reads the blocks, their edges, the
 * decode and the word flags, and writes only the routine list and its
 * counters.  Each body is walked twice, once to claim its blocks and once to
 * count its return words, which are known only after the callers show which
 * idiom they use.  Side arrays indexed by block id are freed before the pass
 * returns; allocation failure is fatal, as elsewhere in am1.
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

// The memory reference operation codes used here: bits 0 to 4, two octal
// digits with bit 5 clear, as optdecode.c numbers them.
#define RTOP_DAC        0024
#define RTOP_DAP        0026
#define RTOP_JMP        0060
#define RTOP_JSP        0062
#define RTOP_CAL        0016    // and jda, when bit 5 is set

// What one body walk records.  Both walks visit the same blocks in the same
// order.
#define RTWALK_CLAIM    0       // claim the blocks and count them
#define RTWALK_RETURNS  1       // count the words that leave through the return word

// One routine that owns a block.  Two entries sharing a tail give the block
// two owners; the list is kept so the dump can name them.
typedef struct rtowner
{
    struct rtowner *nextP;
    OptRoutineP routineP;
} RtOwner, *RtOwnerP;

// Per-block side data, indexed by the block's 1-based id; nothing is written
// back into the block.
typedef struct
{
    unsigned char isEntry;  // the block is the target of a call edge
    OptBlockP blockP;       // the block itself, to turn an id back into one
    OptRoutineP entryOfP;   // the routine it is the entry of, NILP until made
    RtOwnerP ownersP;       // routines whose body holds it, in id order
    RtOwnerP ownersTailP;   // the last of them, for appending in that order
    int ownerCount;         // how many
    int stamp;              // the number of the walk that last visited it: a
                            // walk number, NOT the routine id, because a body is
                            // walked twice and the second walk must not mistake
                            // the first's stamp for its own
} RtBlock, *RtBlockP;

// The pass's working state, sized from the block count.
typedef struct
{
    RtBlockP blocksP;       // blockCount + 1 entries; index 0 is unused
    OptBlockP *stackPP;     // the depth-first stack, at most one slot per block
    int stackTop;
    int *bodyIdsP;          // one body's block ids while it is being claimed
    int bodyCount;
    int stampNext;          // the number the next body walk will stamp with
    int blockCount;
} RtState, *RtStateP;

static void resetCounters(OptTableP tableP);
static int allocState(OptTableP tableP, RtStateP stateP);
static void freeState(RtStateP stateP);
static void markEntryBlocks(OptTableP tableP, RtStateP stateP);
static OptRoutineP newRoutine(OptTableP tableP, OptBlockP blockP);
static void walkBody(OptTableP tableP, RtStateP stateP, OptRoutineP routineP, int mode);
static void claimBlock(RtStateP stateP, OptRoutineP routineP, OptBlockP blockP);
static void keepBody(OptRoutineP routineP, RtStateP stateP);
static int compareIds(const void *aP, const void *bP);
static void keepOwnerCounts(OptTableP tableP, RtStateP stateP);
static int ownedBy(RtStateP stateP, OptBlockP blockP, OptRoutineP routineP);
static void scanBlocks(OptTableP tableP, RtStateP stateP);
static void scanCallSite(OptTableP tableP, RtStateP stateP, OptBlockP blockP);
static void addArc(OptTableP tableP, OptRoutineP fromP, OptRoutineP toP);
static void chooseForms(OptTableP tableP);
static void findReturnWords(OptTableP tableP, RtStateP stateP);
static int savedReturnAddr(OptWordP entryP, int *addrP);
static int isReturnWord(OptRoutineP routineP, OptWordP entryP);
static void markEscapes(OptTableP tableP, RtStateP stateP);
static void markOpenArcs(OptTableP tableP, RtStateP stateP, OptRoutineP routineP, int blockId);
static void countCategories(OptTableP tableP, RtStateP stateP);

// Group the control-flow graph's blocks into routines and fill in everything
// that does not need the call graph.  With no blocks, or no calls, the routine
// list is empty and every counter zero, which is correct, not a failure.
void
optBuildRoutines(OptTableP tableP)
{
RtState state;
OptRoutineP routineP;

    if( !tableP )
    {
        return;
    }

    freeRoutineList(tableP);
    resetCounters(tableP);

    if( !tableP->blockCount )
    {
        return;
    }

    if( !allocState(tableP, &state) )
    {
        return;
    }

    // Every call target is an entry.  The whole entry set must be known
    // before any body is walked, since a body walk stops at another entry.
    markEntryBlocks(tableP, &state);

    // Each routine claims its body; a block claimed by two is a shared tail.
    // The body's block ids are kept on the routine for use after the side
    // arrays are freed.
    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        walkBody(tableP, &state, routineP, RTWALK_CLAIM);
        keepBody(routineP, &state);
    }

    keepOwnerCounts(tableP, &state);

    // With ownership settled: the arcs, call-site counts and fall-in edges
    // all need the owners of the block an edge LEAVES.
    scanBlocks(tableP, &state);

    // The call idiom follows from the callers counted above, and the return
    // word from the idiom.
    chooseForms(tableP);
    findReturnWords(tableP, &state);

    // Which bodies hold code reachable without calling the routine.
    markEscapes(tableP, &state);

    countCategories(tableP, &state);
    freeState(&state);
}

// Clear every routine counter on the table, so a second run gives the same
// answer as the first.
static void
resetCounters(OptTableP tableP)
{
    tableP->routineCount = 0;
    memset(tableP->routineFormCounts, 0, sizeof(tableP->routineFormCounts));
    memset(tableP->routineWordKinds, 0, sizeof(tableP->routineWordKinds));
    tableP->routineBlocks = 0;
    tableP->routineWords = 0;
    tableP->outsideBlocks = 0;
    tableP->outsideWords = 0;
    tableP->callSiteCount = 0;
    tableP->callSiteSinks = 0;
    tableP->mainLineSites = 0;
    tableP->fallInRoutines = 0;
    tableP->fallInEdges = 0;
    tableP->sharedBlocks = 0;
    tableP->sharedRoutines = 0;
    tableP->opaqueEntries = 0;
    tableP->unknownCallRoutines = 0;
    tableP->unknownJumpRoutines = 0;
    tableP->noReturnRoutines = 0;
    tableP->openRoutines = 0;
    tableP->callArcCount = 0;
    tableP->callArcSites = 0;
}

// Allocate the per-block side arrays and the walk stack, which needs at most
// one slot per block since a block is pushed at most once per walk.
// Returns 1; allocation failure is fatal and does not return.
static int
allocState(OptTableP tableP, RtStateP stateP)
{
    stateP->blockCount = tableP->blockCount;
    stateP->stackTop = 0;
    stateP->bodyCount = 0;
    stateP->stampNext = 0;

    if( !(stateP->blocksP = (RtBlockP)calloc((size_t)(stateP->blockCount + 1), sizeof(RtBlock))) ||
        !(stateP->stackPP = (OptBlockP *)calloc((size_t)stateP->blockCount, sizeof(OptBlockP))) ||
        !(stateP->bodyIdsP = (int *)calloc((size_t)stateP->blockCount, sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory grouping the optimizer's blocks into routines\n");
        exit(1);
    }

    return(1);
}

// Release the working state.
static void
freeState(RtStateP stateP)
{
int i;
RtOwnerP ownerP;
RtOwnerP nextP;

    if( stateP->blocksP )
    {
        for( i = 0; i <= stateP->blockCount; ++i )
        {
            for( ownerP = stateP->blocksP[i].ownersP; ownerP; ownerP = nextP )
            {
                nextP = ownerP->nextP;
                free(ownerP);
            }
        }

        free(stateP->blocksP);
        stateP->blocksP = NILP;
    }

    if( stateP->stackPP )
    {
        free(stateP->stackPP);
        stateP->stackPP = NILP;
    }

    if( stateP->bodyIdsP )
    {
        free(stateP->bodyIdsP);
        stateP->bodyIdsP = NILP;
    }
}

// Find the entry blocks and make a routine for each.  Only a call edge's
// target is an entry: a start address, exported label or taken address makes
// a reachability entry, which says control may arrive, not that anything
// called it.  A call edge to the sink makes no routine; scanCallSite() counts
// it.
static void
markEntryBlocks(OptTableP tableP, RtStateP stateP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        stateP->blocksP[blockP->id].blockP = blockP;

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind != OPTFK_CALL) || !edgeP->toP )
            {
                continue;
            }

            stateP->blocksP[edgeP->toP->id].isEntry = 1;
        }
    }

    // Create the routines in block order so that ids run in bank then address
    // order and the dump compares across runs.
    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( stateP->blocksP[blockP->id].isEntry )
        {
            stateP->blocksP[blockP->id].entryOfP = newRoutine(tableP, blockP);
        }
    }
}

// Make one routine for an entry block and append it to the table's list.
// Returns the new routine; allocation failure is fatal and does not return.
static OptRoutineP
newRoutine(OptTableP tableP, OptBlockP blockP)
{
OptRoutineP routineP;

    if( !(routineP = (OptRoutineP)calloc(1, sizeof(OptRoutine))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's routine list\n");
        exit(1);
    }

    routineP->id = ++tableP->routineCount;
    routineP->bank = blockP->bank;
    routineP->entryAddr = blockP->startAddr;
    routineP->entryBlockP = blockP;
    routineP->form = OPTRF_JSP;
    routineP->returnBank = -1;
    routineP->returnAddr = -1;
    routineP->returnWord = OPTRW_NONE;
    routineP->firstReturnAddr = -1;

    if( tableP->routinesTailP )
    {
        tableP->routinesTailP->nextP = routineP;
    }
    else
    {
        tableP->routinesP = routineP;
    }

    tableP->routinesTailP = routineP;
    return(routineP);
}

// Walk one routine's body depth-first from its entry block, stopping at any
// other routine's entry.  A call edge is not followed (the callee is its own
// routine) but its return edge is: it continues the caller after the call and
// any inline arguments.  Fall, skip, jump and resume edges are ordinary flow;
// a sink edge cannot be followed.  The stamp keeps each block to one visit.
static void
walkBody(OptTableP tableP, RtStateP stateP, OptRoutineP routineP, int mode)
{
OptBlockP blockP;
OptBlockP targetP;
OptFlowEdgeP edgeP;
OptWordP wordP;
int addr;
int stamp;

    stateP->stackTop = 0;
    stateP->bodyCount = 0;
    stamp = ++stateP->stampNext;
    stateP->blocksP[routineP->entryBlockP->id].stamp = stamp;
    stateP->stackPP[stateP->stackTop++] = routineP->entryBlockP;

    while( stateP->stackTop > 0 )
    {
        blockP = stateP->stackPP[--stateP->stackTop];

        if( mode == RTWALK_CLAIM )
        {
            claimBlock(stateP, routineP, blockP);
        }
        else
        {
            // Every word of the block, not just the last: a patched
            // "rtn, jmp ." return often sits mid-block, reached by fall-through.
            for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
            {
                if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
                {
                    continue;
                }

                if( isReturnWord(routineP, wordP) )
                {
                    ++routineP->returnCount;

                    if( routineP->firstReturnAddr < 0 )
                    {
                        routineP->firstReturnAddr = addr;
                    }
                }
            }

            // An unfollowable jump out of the body, other than this routine's
            // own return, is a hole: control leaves for somewhere unknown, as
            // in a dispatch through a pointer table.  The return must be
            // excluded, since "jmp i rtn" through a data word and a patched
            // "rtn, jmp ." are both unfollowable jumps too.
            for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
            {
                if( (edgeP->kind != OPTFK_JUMP) || edgeP->toP )
                {
                    continue;
                }

                if( isReturnWord(routineP, blockP->lastP) )
                {
                    continue;
                }

                routineP->flags |= OPTRT_JUMPSUNKNOWN;
            }
        }

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind == OPTFK_CALL) || !(targetP = edgeP->toP) )
            {
                continue;
            }

            // Another routine's entry ends this body; a jump back to this
            // routine's own entry is an internal loop.
            if( stateP->blocksP[targetP->id].isEntry && (targetP != routineP->entryBlockP) )
            {
                continue;
            }

            if( stateP->blocksP[targetP->id].stamp == stamp )
            {
                continue;
            }

            stateP->blocksP[targetP->id].stamp = stamp;
            stateP->stackPP[stateP->stackTop++] = targetP;
        }
    }
}

// Record that a routine's body holds a block and add the block's figures to
// the routine's.  Owners are appended, keeping the list in routine-id order.
static void
claimBlock(RtStateP stateP, OptRoutineP routineP, OptBlockP blockP)
{
RtBlockP slotP;
RtOwnerP ownerP;

    slotP = &stateP->blocksP[blockP->id];

    if( !(ownerP = (RtOwnerP)calloc(1, sizeof(RtOwner))) )
    {
        fprintf(stderr, "am1: out of memory claiming a block for an optimizer routine\n");
        exit(1);
    }

    ownerP->routineP = routineP;

    if( slotP->ownersTailP )
    {
        slotP->ownersTailP->nextP = ownerP;
    }
    else
    {
        slotP->ownersP = ownerP;
    }

    slotP->ownersTailP = ownerP;
    ++slotP->ownerCount;

    stateP->bodyIdsP[stateP->bodyCount++] = blockP->id;
    ++routineP->blockCount;
    routineP->wordCount += blockP->wordCount;
}

// Copy the body just claimed onto the routine, sorted by block id, which is
// bank then address order.
static void
keepBody(OptRoutineP routineP, RtStateP stateP)
{
    if( !stateP->bodyCount )
    {
        return;
    }

    if( !(routineP->bodyIdsP = (int *)calloc((size_t)stateP->bodyCount, sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory recording an optimizer routine's body\n");
        exit(1);
    }

    memcpy(routineP->bodyIdsP, stateP->bodyIdsP, (size_t)stateP->bodyCount * sizeof(int));
    qsort(routineP->bodyIdsP, (size_t)stateP->bodyCount, sizeof(int), compareIds);
}

// Order two block ids for qsort().
// Returns negative, zero or positive as the first id is below, equal to or
// above the second.
static int
compareIds(const void *aP, const void *bP)
{
    return( (*(const int *)aP) - (*(const int *)bP) );
}

// Copy the per-block owner counts onto the table so "is this block shared"
// stays a constant-time question after the side arrays are gone.  The count
// saturates at 255; callers only ask whether it exceeds one.
static void
keepOwnerCounts(OptTableP tableP, RtStateP stateP)
{
int i;

    tableP->blockOwnerSlots = stateP->blockCount + 1;

    if( !(tableP->blockOwnersP = (unsigned char *)calloc((size_t)tableP->blockOwnerSlots, 1)) )
    {
        fprintf(stderr, "am1: out of memory recording optimizer block ownership\n");
        exit(1);
    }

    for( i = 1; i <= stateP->blockCount; ++i )
    {
        tableP->blockOwnersP[i] = (unsigned char)((stateP->blocksP[i].ownerCount > 255)?255:stateP->blocksP[i].ownerCount);
    }
}

// Is a block in a particular routine's body?
// Returns 1 when the routine owns it, 0 when it does not or either is NILP.
static int
ownedBy(RtStateP stateP, OptBlockP blockP, OptRoutineP routineP)
{
RtOwnerP ownerP;

    if( !blockP || !routineP )
    {
        return(0);
    }

    for( ownerP = stateP->blocksP[blockP->id].ownersP; ownerP; ownerP = ownerP->nextP )
    {
        if( ownerP->routineP == routineP )
        {
            return(1);
        }
    }

    return(0);
}

// Sweep every block with ownership settled, which no body walk can know: a
// call arc leaves EVERY routine owning the calling block; a call from a block
// no routine owns is a main-line call; and a plain edge into an entry is a
// fall-in only when it leaves a block outside that entry's own body, or every
// loop back to a routine's first word would be a fall-in.
static void
scanBlocks(OptTableP tableP, RtStateP stateP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
RtBlockP slotP;
RtOwnerP ownerP;
OptRoutineP targetRoutineP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        slotP = &stateP->blocksP[blockP->id];

        if( slotP->ownerCount )
        {
            ++tableP->routineBlocks;
            tableP->routineWords += blockP->wordCount;
        }
        else
        {
            ++tableP->outsideBlocks;
            tableP->outsideWords += blockP->wordCount;

            // A block outside every routine whose first word's address is
            // TAKEN may be the body of a routine only ever called through a
            // pointer, which no call edge can reveal.  Counted as a
            // candidate, not a claim.
            if( blockP->firstP && (blockP->firstP->flags & (OPTF_TAKEN | OPTF_MAYBE_ENTERED)) )
            {
                ++tableP->opaqueEntries;
            }
        }

        if( blockP->endKind == OPTBE_CALL )
        {
            scanCallSite(tableP, stateP, blockP);
        }

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( edgeP->kind == OPTFK_CALL )
            {
                continue;
            }

            if( !edgeP->toP )
            {
                // A sink on an ordinary edge may be the routine's own return
                // ("jmp i rtn", or a patched "rtn, jmp ."), which is not known
                // until findReturnWords() runs; the RTWALK_RETURNS walk
                // decides whether it is a hole.
                continue;
            }

            if( !(targetRoutineP = stateP->blocksP[edgeP->toP->id].entryOfP) )
            {
                continue;
            }

            // An edge from inside the routine's own body is a loop, not a
            // fall-in.
            if( ownedBy(stateP, blockP, targetRoutineP) )
            {
                continue;
            }

            ++tableP->fallInEdges;
            targetRoutineP->flags |= OPTRT_FALLIN;

            for( ownerP = slotP->ownersP; ownerP; ownerP = ownerP->nextP )
            {
                ownerP->routineP->flags |= OPTRT_FALLSOUT;
            }
        }
    }
}

// Count one call site, count its callers on the callee, and add an arc from
// every routine owning the calling block.  Sites are counted once per call
// EDGE, but arcs multiply over a shared tail's owners on purpose: the tail
// runs with either entry's return word live, so the callee interferes with
// both.
static void
scanCallSite(OptTableP tableP, RtStateP stateP, OptBlockP blockP)
{
OptFlowEdgeP edgeP;
RtOwnerP ownerP;
OptRoutineP calleeP;
OptWordP callP;
int resolved;
int isJda;

    callP = blockP->lastP;
    isJda = (callP && (callP->decode.opcode == RTOP_CAL));
    resolved = 0;

    for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->kind != OPTFK_CALL )
        {
            continue;
        }

        if( !edgeP->toP )
        {
            // A call to the sink: this routine might call anything.
            ++tableP->callSiteSinks;

            for( ownerP = stateP->blocksP[blockP->id].ownersP; ownerP; ownerP = ownerP->nextP )
            {
                ownerP->routineP->flags |= OPTRT_CALLSUNKNOWN;
            }

            continue;
        }

        if( !(calleeP = stateP->blocksP[edgeP->toP->id].entryOfP) )
        {
            // Cannot happen: markEntryBlocks() made a routine for every
            // resolved call target.  Skipped rather than making an arc to
            // nothing.
            continue;
        }

        ++resolved;

        if( isJda )
        {
            ++calleeP->jdaSites;
        }
        else
        {
            ++calleeP->jspSites;
        }

        if( !stateP->blocksP[blockP->id].ownerCount )
        {
            ++calleeP->mainLineSites;
            ++tableP->mainLineSites;
        }

        for( ownerP = stateP->blocksP[blockP->id].ownersP; ownerP; ownerP = ownerP->nextP )
        {
            addArc(tableP, ownerP->routineP, calleeP);
        }
    }

    if( resolved )
    {
        ++tableP->callSiteCount;
    }
}

// Add one call site to the arc from a caller to a callee, making the arc on
// its first site.  The search is linear; out-degree is small.  Allocation
// failure is fatal.
static void
addArc(OptTableP tableP, OptRoutineP fromP, OptRoutineP toP)
{
OptCallArcP arcP;

    for( arcP = fromP->callsP; arcP; arcP = arcP->nextP )
    {
        if( arcP->toP == toP )
        {
            ++arcP->sites;
            ++tableP->callArcSites;
            return;
        }
    }

    if( !(arcP = (OptCallArcP)calloc(1, sizeof(OptCallArc))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's call graph\n");
        exit(1);
    }

    arcP->toP = toP;
    arcP->sites = 1;

    if( fromP->callsTailP )
    {
        fromP->callsTailP->nextP = arcP;
    }
    else
    {
        fromP->callsP = arcP;
    }

    fromP->callsTailP = arcP;
    ++fromP->callArcs;
    ++toP->callerArcs;
    ++tableP->callArcCount;
    ++tableP->callArcSites;
}

// Record which call idiom enters each routine.  A jda routine's return address
// lives at a fixed address beside its entry, so it has nothing separable to
// share.  A routine called both ways is recorded MIXED: a program can reach
// one include by "jda" from one place and by a dac/jsp pair from another.
static void
chooseForms(OptTableP tableP)
{
OptRoutineP routineP;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( routineP->jspSites && routineP->jdaSites )
        {
            routineP->form = OPTRF_MIXED;
        }
        else if( routineP->jdaSites )
        {
            routineP->form = OPTRF_JDA;
        }
        else
        {
            routineP->form = OPTRF_JSP;
        }

        ++tableP->routineFormCounts[routineP->form];
    }
}

// Name each routine's return word and count the words that leave through it.
// First choice: the entry word saves the return address with a direct,
// unpatched dac or dap, which is the jsp idiom and, in practice, the jda one
// too ("jda Y" is "dac Y" then "jsp Y+1", so the callee saves AC itself).
// Otherwise, with jda or cal callers, the hardware deposited it one below the
// entry (Y, or 0100 for cal) and the return is "jmp i" that word.  Neither is
// OPTRW_NONE, counted and not guessed: a handler, a routine that always jumps
// away, and one whose return word is written unseen all look alike.
static void
findReturnWords(OptTableP tableP, RtStateP stateP)
{
OptRoutineP routineP;
int addr;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( savedReturnAddr(routineP->entryBlockP->firstP, &addr) )
        {
            routineP->returnWord = OPTRW_SAVED;
            routineP->returnBank = routineP->bank;
            routineP->returnAddr = addr;
        }
        else if( routineP->jdaSites )
        {
            routineP->returnWord = OPTRW_JDA;
            routineP->returnBank = routineP->bank;
            routineP->returnAddr = ((routineP->entryAddr - 1) & ADDRMASK);
        }

        ++tableP->routineWordKinds[routineP->returnWord];

        // Always walked: the same walk finds the holes in the body, most
        // likely in a routine whose return could not be named.
        walkBody(tableP, stateP, routineP, RTWALK_RETURNS);

        if( !routineP->returnCount )
        {
            routineP->flags |= OPTRT_NORETURN;
            ++tableP->noReturnRoutines;
        }
    }
}

// Does a routine's entry word save the return address with a direct dac or
// dap?  Not indirect, which would need a pointer followed, and not patched,
// which would name a different word on different runs.
// Returns 1 with *addrP set to the word the return address is saved in, 0
// otherwise or when entryP is NILP.
static int
savedReturnAddr(OptWordP entryP, int *addrP)
{
    if( !entryP || !inGraph(entryP) )
    {
        return(0);
    }

    if( entryP->flags & OPTF_PATCHED )
    {
        return(0);
    }

    if( entryP->decode.group != OPTG_MEMREF )
    {
        return(0);
    }

    if( (entryP->decode.opcode != RTOP_DAC) && (entryP->decode.opcode != RTOP_DAP) )
    {
        return(0);
    }

    if( entryP->decode.memIndirect )
    {
        return(0);
    }

    *addrP = entryP->decode.address;
    return(1);
}

// Is a word of a routine's body one that leaves through its return word?
// Either "jmp i rtn", an indirect jmp naming the return word, or the patched
// "rtn, jmp ." whose execution IS the return.  A patched jmp need not be
// indirect and its assembled address says nothing, so that case is tested
// by ADDRESS, not by decode.
// Returns 1 when the word is a return of this routine, 0 otherwise.
static int
isReturnWord(OptRoutineP routineP, OptWordP entryP)
{
    if( !entryP || !inGraph(entryP) || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    if( (entryP->bank == routineP->returnBank) && (entryP->addr == routineP->returnAddr) )
    {
        // At the return address only a jmp leaves; anything else there is a
        // data word the routine returns THROUGH.
        return( (entryP->decode.group == OPTG_MEMREF) && (entryP->decode.opcode == RTOP_JMP) );
    }

    if( entryP->decode.group != OPTG_MEMREF )
    {
        return(0);
    }

    if( (entryP->decode.opcode != RTOP_JMP) || !entryP->decode.memIndirect )
    {
        return(0);
    }

    if( entryP->flags & OPTF_PATCHED )
    {
        return(0);
    }

    return( (entryP->bank == routineP->returnBank) && (entryP->decode.address == routineP->returnAddr) );
}

// Mark the routines whose body holds code the program can reach WITHOUT
// calling them.  The body walk cannot tell one jmp from another, so an arm
// that jumps out of the routine instead of returning grows the body into what
// follows; if that is the main line, it takes in the routine's own call sites
// and makes an arc back to itself.  Cycles through such a body are escapes,
// not recursion.  The test: is any body block reachable from the entry set
// without passing through this routine's entry?  For a well-formed routine,
// however large, it is not.  Costs one walk per routine.
static void
markEscapes(OptTableP tableP, RtStateP stateP)
{
OptRoutineP routineP;
OptBlockP blockP;
OptFlowEdgeP edgeP;
int stamp;
int i;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        stamp = ++stateP->stampNext;
        stateP->stackTop = 0;

        // Seed with every reachability entry except this routine's own, the
        // road the body is asked to be reachable without.
        for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
        {
            if( !blockP->isEntry || (blockP == routineP->entryBlockP) )
            {
                continue;
            }

            stateP->blocksP[blockP->id].stamp = stamp;
            stateP->stackPP[stateP->stackTop++] = blockP;
        }

        while( stateP->stackTop > 0 )
        {
            blockP = stateP->stackPP[--stateP->stackTop];

            for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
            {
                if( !edgeP->toP || (edgeP->toP == routineP->entryBlockP) )
                {
                    continue;
                }

                if( stateP->blocksP[edgeP->toP->id].stamp == stamp )
                {
                    continue;
                }

                stateP->blocksP[edgeP->toP->id].stamp = stamp;
                stateP->stackPP[stateP->stackTop++] = edgeP->toP;
            }
        }

        for( i = 0; i < routineP->blockCount; ++i )
        {
            if( stateP->blocksP[routineP->bodyIdsP[i]].stamp != stamp )
            {
                continue;
            }

            if( !(routineP->flags & OPTRT_OPENBODY) )
            {
                routineP->flags |= OPTRT_OPENBODY;
                ++tableP->openRoutines;
            }

            // Also mark the arcs made from this open block, which is what
            // tells an escape from recursion, so the loop runs on past the
            // first open block.
            markOpenArcs(tableP, stateP, routineP, routineP->bodyIdsP[i]);
        }
    }
}

// Mark the arcs made by one open block of a routine's body, one the program
// reaches without entering the routine.  Such an arc does not show the callee
// running inside the caller: real recursion's arc back is made from code only
// the call reaches, and an escape's is not.
static void
markOpenArcs(OptTableP tableP, RtStateP stateP, OptRoutineP routineP, int blockId)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
OptCallArcP arcP;
OptRoutineP calleeP;

    (void)tableP;

    blockP = stateP->blocksP[blockId].blockP;

    if( !blockP || (blockP->endKind != OPTBE_CALL) )
    {
        return;
    }

    for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( (edgeP->kind != OPTFK_CALL) || !edgeP->toP )
        {
            continue;
        }

        if( !(calleeP = stateP->blocksP[edgeP->toP->id].entryOfP) )
        {
            continue;
        }

        for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
        {
            if( arcP->toP == calleeP )
            {
                ++arcP->openSites;
                break;
            }
        }
    }
}

// Count the three categories once, after everything that sets their flags,
// so no routine is counted twice under the same head.
static void
countCategories(OptTableP tableP, RtStateP stateP)
{
OptRoutineP routineP;
OptBlockP blockP;
RtOwnerP ownerP;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( stateP->blocksP[blockP->id].ownerCount < 2 )
        {
            continue;
        }

        ++tableP->sharedBlocks;

        for( ownerP = stateP->blocksP[blockP->id].ownersP; ownerP; ownerP = ownerP->nextP )
        {
            ownerP->routineP->flags |= OPTRT_SHARED;
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( routineP->flags & OPTRT_FALLIN )
        {
            ++tableP->fallInRoutines;
        }

        if( routineP->flags & OPTRT_SHARED )
        {
            ++tableP->sharedRoutines;
        }

        if( routineP->flags & OPTRT_CALLSUNKNOWN )
        {
            ++tableP->unknownCallRoutines;
        }

        if( routineP->flags & OPTRT_JUMPSUNKNOWN )
        {
            ++tableP->unknownJumpRoutines;
        }
    }
}

// Does one routine's body hold a block?  A binary search of the body's
// ascending block ids.
// Returns 1 when the routine owns the block, 0 when it does not or either
// argument is NILP.
int
optRoutineOwnsBlock(OptRoutineP routineP, OptBlockP blockP)
{
int lo;
int hi;
int mid;

    if( !routineP || !blockP || !routineP->bodyIdsP )
    {
        return(0);
    }

    lo = 0;
    hi = routineP->blockCount - 1;

    while( lo <= hi )
    {
        mid = (lo + ((hi - lo) / 2));

        if( routineP->bodyIdsP[mid] == blockP->id )
        {
            return(1);
        }

        if( routineP->bodyIdsP[mid] < blockP->id )
        {
            lo = (mid + 1);
        }
        else
        {
            hi = (mid - 1);
        }
    }

    return(0);
}

// How many routines' bodies hold a block?
// Returns the count, saturated at 255, or 0 when the block is in no routine,
// when either argument is NILP, or when the routines were never built.
int
optBlockOwnerCount(OptTableP tableP, OptBlockP blockP)
{
    if( !tableP || !blockP || !tableP->blockOwnersP )
    {
        return(0);
    }

    if( (blockP->id < 1) || (blockP->id >= tableP->blockOwnerSlots) )
    {
        return(0);
    }

    return( (int)tableP->blockOwnersP[blockP->id] );
}

// Which routine's body is a block in?  With a shared tail, the first owner in
// id order, the one with the lowest entry address.
// Returns that routine, or NILP when the block is in none of them.
OptRoutineP
optRoutineOfBlock(OptTableP tableP, OptBlockP blockP)
{
OptRoutineP routineP;

    if( !tableP || !blockP )
    {
        return(NILP);
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( optRoutineOwnsBlock(routineP, blockP) )
        {
            return(routineP);
        }
    }

    return(NILP);
}

// Name a routine's call idiom.
// Returns a static string, never NILP.
const char *
optRoutineFormName(OptRoutineForm form)
{
    switch( form )
    {
    case OPTRF_JSP:     return("jsp");
    case OPTRF_JDA:     return("jda");
    case OPTRF_MIXED:   return("mixed");

    case OPTRF_COUNT:
    default:            return("?");
    }
}

// Name how a routine's return word was identified.
// Returns a static string, never NILP.
const char *
optReturnWordName(OptReturnWord word)
{
    switch( word )
    {
    case OPTRW_NONE:    return("none");
    case OPTRW_SAVED:   return("saved");
    case OPTRW_JDA:     return("jda");

    case OPTRW_COUNT:
    default:            return("?");
    }
}

// Release the routines, their arcs and the transitive closure, and clear the
// table's list.  Safe to call on a table whose graph was never built, and
// safe to call twice.
void
freeRoutineList(OptTableP tableP)
{
OptRoutineP routineP;
OptRoutineP nextRoutineP;
OptCallArcP arcP;
OptCallArcP nextArcP;

    if( !tableP )
    {
        return;
    }

    for( routineP = tableP->routinesP; routineP; routineP = nextRoutineP )
    {
        nextRoutineP = routineP->nextP;

        for( arcP = routineP->callsP; arcP; arcP = nextArcP )
        {
            nextArcP = arcP->nextP;
            free(arcP);
        }

        if( routineP->bodyIdsP )
        {
            free(routineP->bodyIdsP);
        }

        free(routineP);
    }

    tableP->routinesP = NILP;
    tableP->routinesTailP = NILP;

    if( tableP->callReachPP )
    {
        free(tableP->callReachPP);
        tableP->callReachPP = NILP;
    }

    tableP->callReachWords = 0;

    if( tableP->blockOwnersP )
    {
        free(tableP->blockOwnersP);
        tableP->blockOwnersP = NILP;
    }

    tableP->blockOwnerSlots = 0;
}
