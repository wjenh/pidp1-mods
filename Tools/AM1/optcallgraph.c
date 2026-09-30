/*
 * The am1 optimizer's call graph.  optroutine.c groups the control-flow
 * blocks into routines and builds the arcs between them; this file finds the
 * graph's strongly connected components, its cycles, its transitive closure
 * and the routines a sequence-break handler can reach, measures what
 * return-word sharing could recover, and prints it all under -O=calls.
 *
 * Cycles are found on the RESOLVED arcs alone, so one reported as a defect is
 * real recursion, which the dap rtn idiom cannot survive.  The universal
 * vertex for a routine that might call anything is applied afterwards and its
 * cycles are labeled artifacts: with it, every routine that can reach an
 * unfollowed pointer is on a cycle, and calling that recursion would accuse a
 * correct program.
 *
 * optBuildCallGraph() is called once per optimize() run, single threaded,
 * after optBuildFlow() and before optRunRules().  It reads the blocks, edges
 * and routines and writes only the call-graph fields of the OptTable.  Tarjan's
 * algorithm runs with an explicit stack, so a deep call graph cannot overflow
 * the C stack.  Everything allocated is freed before the pass returns except
 * the closure, which freeRoutineList() owns.
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

// Bits per word of a reachability vector; unsigned int is at least 32 bits
// everywhere this builds and the code never assumes more.
#define CG_BITS         32

// Tarjan's per-routine working state.  onStack keeps a completed component
// from being re-entered.
typedef struct
{
    int index;              // discovery order, 0 when not yet visited
    int lowlink;
    int onStack;
} CgNode, *CgNodeP;

// One frame of the explicit depth-first stack, which keeps the arc the frame
// has reached.  Tarjan is iterative here because a call graph can be as deep
// as the program.
typedef struct
{
    OptRoutineP routineP;
    OptCallArcP arcP;       // the next arc to consider, NILP when they are done
} CgFrame, *CgFrameP;

// The pass's working state.
typedef struct
{
    OptRoutineP *byIdPP;    // routineCount + 1 entries, index 0 unused
    CgNodeP nodesP;         // the same shape, Tarjan's state
    OptRoutineP *sccStackPP;    // Tarjan's component stack
    int sccTop;
    CgFrameP framesP;       // the depth-first stack
    int frameTop;
    int nextIndex;          // the next discovery order to hand out
    int routineCount;
} CgState, *CgStateP;

static void resetCounters(OptTableP tableP);
static int allocState(OptTableP tableP, CgStateP stateP);
static void freeState(CgStateP stateP);
static void findComponents(OptTableP tableP, CgStateP stateP);
static void closeComponent(OptTableP tableP, CgStateP stateP, OptRoutineP rootP);
static void buildClosure(OptTableP tableP);
static void markArtifactCycles(OptTableP tableP);
static void markSbsPool(OptTableP tableP);
static unsigned int *reachOf(OptTableP tableP, OptRoutineP routineP);
static void reachSet(OptTableP tableP, OptRoutineP routineP, int id);
static int reachTest(OptTableP tableP, OptRoutineP routineP, int id);
static void reachOr(OptTableP tableP, OptRoutineP intoP, OptRoutineP fromP);
static void dumpRoutineFlags(FILE *fP, OptRoutineP routineP);
static void dumpCategories(FILE *fP, OptTableP tableP);
static void dumpArcs(FILE *fP, OptTableP tableP);
static void dumpClosure(FILE *fP, OptTableP tableP);
static void dumpCycles(FILE *fP, OptTableP tableP);
static void dumpSbs(FILE *fP, OptTableP tableP);
static int ownsReturnWord(OptTableP tableP, OptRoutineP routineP);
static int isLeafRoutine(OptRoutineP routineP);
static int chainDepth(OptTableP tableP, int bank, int pool, int *sccDepthP);
static void fillRoutineDepths(OptTableP tableP, int *sccDepthP);
static void dumpDepth(FILE *fP, OptTableP tableP);

// Build the routines and the call graph over them.  A program with no call at
// all yields no routines and every counter zero, which is correct, not an
// error.
void
optBuildCallGraph(OptTableP tableP)
{
CgState state;

    if( !tableP )
    {
        return;
    }

    // optBuildRoutines() clears its own state and this clears the graph's, so
    // a second run over the same table gives the same answer.
    optBuildRoutines(tableP);
    resetCounters(tableP);

    // The sequence-break pool comes from the control-flow graph, not the arcs,
    // so it is found even with no routines.  A routine a handler can reach
    // interferes with every main-line routine, so it is a separate pool.
    markSbsPool(tableP);

    if( !tableP->routineCount )
    {
        return;
    }

    if( !allocState(tableP, &state) )
    {
        return;
    }

    findComponents(tableP, &state);
    freeState(&state);

    buildClosure(tableP);
    markArtifactCycles(tableP);

    // Last: the sharing measurement reads the components and the cycle
    // classification, and writes only its own fields.
    optMeasureSharing(tableP);
}

// Clear every counter this file owns.
static void
resetCounters(OptTableP tableP)
{
int i;

    tableP->sccCount = 0;
    tableP->cycleCount = 0;
    tableP->cycleRoutines = 0;
    tableP->defectCycles = 0;
    tableP->defectCycleRoutines = 0;
    tableP->escapeCycles = 0;
    tableP->escapeCycleRoutines = 0;
    tableP->artifactCycleRoutines = 0;
    tableP->closurePairs = 0;
    tableP->sbsHandlers = 0;
    tableP->sbsPoolRoutines = 0;

    // The sharing tallies.  optMeasureSharing() clears them too, but it is not
    // reached when there are no routines, and a second run over such a table
    // must not report the first run's numbers.
    tableP->returnWordCount = 0;
    tableP->sharedReturnWords = 0;
    tableP->callMaxDepth = 0;
    tableP->callDepthSaving = 0;
    tableP->bankSavingTotal = 0;
    tableP->leafRoutines = 0;
    tableP->leafSaving = 0;

    for( i = 0; i <= MAXBANK; ++i )
    {
        tableP->bankReturnWords[i] = 0;
        tableP->bankMaxDepth[i] = 0;
        tableP->bankSaving[i] = 0;
        tableP->bankLeafWords[i] = 0;
    }
}

// Allocate the working state and the closure's bit vectors.
// Returns 1; allocation failure is fatal, as elsewhere in am1.
static int
allocState(OptTableP tableP, CgStateP stateP)
{
OptRoutineP routineP;

    stateP->routineCount = tableP->routineCount;
    stateP->sccTop = 0;
    stateP->frameTop = 0;
    stateP->nextIndex = 0;

    tableP->callReachWords = ((tableP->routineCount + (CG_BITS - 1)) / CG_BITS);

    if( !(stateP->byIdPP = (OptRoutineP *)calloc((size_t)(stateP->routineCount + 1), sizeof(OptRoutineP))) ||
        !(stateP->nodesP = (CgNodeP)calloc((size_t)(stateP->routineCount + 1), sizeof(CgNode))) ||
        !(stateP->sccStackPP = (OptRoutineP *)calloc((size_t)stateP->routineCount, sizeof(OptRoutineP))) ||
        !(stateP->framesP = (CgFrameP)calloc((size_t)stateP->routineCount, sizeof(CgFrame))) ||
        !(tableP->callReachPP = (unsigned int *)calloc((size_t)stateP->routineCount * (size_t)tableP->callReachWords,
            sizeof(unsigned int))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's call graph\n");
        exit(1);
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        stateP->byIdPP[routineP->id] = routineP;
    }

    return(1);
}

// Release the working state.  The closure outlives the pass and
// freeRoutineList() frees it.
static void
freeState(CgStateP stateP)
{
    if( stateP->byIdPP )
    {
        free(stateP->byIdPP);
        stateP->byIdPP = NILP;
    }

    if( stateP->nodesP )
    {
        free(stateP->nodesP);
        stateP->nodesP = NILP;
    }

    if( stateP->sccStackPP )
    {
        free(stateP->sccStackPP);
        stateP->sccStackPP = NILP;
    }

    if( stateP->framesP )
    {
        free(stateP->framesP);
        stateP->framesP = NILP;
    }
}

// Find the strongly connected components of the resolved arcs, by Tarjan's
// algorithm written iteratively.  A component of more than one routine, or
// of one that calls itself, is a cycle.  The components close in reverse
// topological order, which lets buildClosure() be a single sweep.
static void
findComponents(OptTableP tableP, CgStateP stateP)
{
OptRoutineP routineP;
OptRoutineP currentP;
OptRoutineP targetP;
CgFrameP frameP;
CgNodeP nodeP;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( stateP->nodesP[routineP->id].index )
        {
            continue;
        }

        // Push the root of a new depth-first tree.
        stateP->nodesP[routineP->id].index = ++stateP->nextIndex;
        stateP->nodesP[routineP->id].lowlink = stateP->nodesP[routineP->id].index;
        stateP->nodesP[routineP->id].onStack = 1;
        stateP->sccStackPP[stateP->sccTop++] = routineP;

        stateP->framesP[stateP->frameTop].routineP = routineP;
        stateP->framesP[stateP->frameTop].arcP = routineP->callsP;
        ++stateP->frameTop;

        while( stateP->frameTop > 0 )
        {
            frameP = &stateP->framesP[stateP->frameTop - 1];
            currentP = frameP->routineP;

            if( frameP->arcP )
            {
                targetP = frameP->arcP->toP;
                frameP->arcP = frameP->arcP->nextP;

                nodeP = &stateP->nodesP[targetP->id];

                if( !nodeP->index )
                {
                    // An arc into unexplored ground: descend.
                    nodeP->index = ++stateP->nextIndex;
                    nodeP->lowlink = nodeP->index;
                    nodeP->onStack = 1;
                    stateP->sccStackPP[stateP->sccTop++] = targetP;

                    stateP->framesP[stateP->frameTop].routineP = targetP;
                    stateP->framesP[stateP->frameTop].arcP = targetP->callsP;
                    ++stateP->frameTop;
                }
                else if( nodeP->onStack )
                {
                    // A back edge into the component being built.
                    if( nodeP->index < stateP->nodesP[currentP->id].lowlink )
                    {
                        stateP->nodesP[currentP->id].lowlink = nodeP->index;
                    }
                }

                // An arc into an already closed component: it cannot reach
                // back here, or it would not have closed.
                continue;
            }

            // Every arc considered: pop, carry the lowlink up to the parent,
            // and close a component when this routine is its root.
            --stateP->frameTop;

            if( stateP->frameTop > 0 )
            {
                OptRoutineP parentP;

                parentP = stateP->framesP[stateP->frameTop - 1].routineP;

                if( stateP->nodesP[currentP->id].lowlink < stateP->nodesP[parentP->id].lowlink )
                {
                    stateP->nodesP[parentP->id].lowlink = stateP->nodesP[currentP->id].lowlink;
                }
            }

            if( stateP->nodesP[currentP->id].lowlink == stateP->nodesP[currentP->id].index )
            {
                closeComponent(tableP, stateP, currentP);
            }
        }
    }
}

// Pop one strongly connected component off Tarjan's stack, number it, and
// classify it.  A component of one routine is a cycle only if the routine
// calls itself, since a self arc does not enlarge the component.
static void
closeComponent(OptTableP tableP, CgStateP stateP, OptRoutineP rootP)
{
OptRoutineP memberP;
OptCallArcP arcP;
int size;
int isCycle;
int escapes;
int scc;
int i;
int first;

    scc = ++tableP->sccCount;
    first = stateP->sccTop;
    size = 0;

    // Find where the component starts on the stack without popping yet: the
    // cycle class must be known before the members are flagged.
    do
    {
        --first;
        ++size;
    }
    while( (first > 0) && (stateP->sccStackPP[first] != rootP) );

    isCycle = (size > 1);

    if( !isCycle )
    {
        for( arcP = rootP->callsP; arcP; arcP = arcP->nextP )
        {
            if( arcP->toP == rootP )
            {
                isCycle = 1;
                break;
            }
        }
    }

    // A cycle is real recursion when every arc on it is made from a block the
    // program reaches ONLY by entering the caller: the second call happens
    // with the first outstanding and overwrites its return address.  It is an
    // ESCAPE cycle when an arc back comes from a block reached without entering
    // the caller (openSites): a routine that jumps out of itself instead of
    // returning drags the jump's target into its body, and if that is main-line
    // code the body takes in the call sites.  That is no second call.  The test
    // is per arc, not per routine: two entries sharing a tail have open bodies
    // and no cycle.
    escapes = 0;

    for( i = first; (i < stateP->sccTop) && !escapes; ++i )
    {
        for( arcP = stateP->sccStackPP[i]->callsP; arcP; arcP = arcP->nextP )
        {
            // Only an arc inside the component is on the cycle; onStack is
            // still set exactly for this component's members.
            if( !stateP->nodesP[arcP->toP->id].onStack || !arcP->openSites )
            {
                continue;
            }

            escapes = 1;
            break;
        }
    }

    for( i = first; i < stateP->sccTop; ++i )
    {
        memberP = stateP->sccStackPP[i];
        stateP->nodesP[memberP->id].onStack = 0;
        memberP->scc = scc;

        if( !isCycle )
        {
            continue;
        }

        memberP->flags |= OPTRT_ONCYCLE;
        ++tableP->cycleRoutines;

        if( escapes )
        {
            memberP->flags |= OPTRT_ESCCYCLE;
            ++tableP->escapeCycleRoutines;
        }
        else
        {
            ++tableP->defectCycleRoutines;
        }
    }

    stateP->sccTop = first;

    if( isCycle )
    {
        ++tableP->cycleCount;

        if( escapes )
        {
            ++tableP->escapeCycles;
        }
        else
        {
            ++tableP->defectCycles;
        }
    }
}

// Build the transitive closure of the resolved arcs, one bit vector per
// routine.  Tarjan closed each component after every component it reaches,
// so an ascending sweep finds each callee's closure final, except inside a
// cyclic component, where every member reaches every other: there the
// members' vectors are OR-ed together once and shared.  No fixpoint needed.
static void
buildClosure(OptTableP tableP)
{
OptRoutineP routineP;
OptCallArcP arcP;
unsigned int *accP;
unsigned int *fromP;
int scc;
int w;
int i;

    if( !tableP->callReachPP || !tableP->callReachWords )
    {
        return;
    }

    if( !(accP = (unsigned int *)calloc((size_t)tableP->callReachWords, sizeof(unsigned int))) )
    {
        fprintf(stderr, "am1: out of memory closing the optimizer's call graph\n");
        exit(1);
    }

    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineP->scc != scc )
            {
                continue;
            }

            for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
            {
                reachSet(tableP, routineP, arcP->toP->id);
                reachOr(tableP, routineP, arcP->toP);
            }
        }

        // A cyclic component: everything its members reach, all of them
        // reach, including the component itself.
        memset(accP, 0, (size_t)tableP->callReachWords * sizeof(unsigned int));

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineP->scc != scc )
            {
                continue;
            }

            fromP = reachOf(tableP, routineP);

            for( w = 0; w < tableP->callReachWords; ++w )
            {
                accP[w] |= fromP[w];
            }
        }

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineP->scc != scc )
            {
                continue;
            }

            fromP = reachOf(tableP, routineP);

            for( w = 0; w < tableP->callReachWords; ++w )
            {
                fromP[w] = accP[w];
            }
        }
    }

    free(accP);

    // How many ordered pairs of routines can be on the call stack together.
    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        for( i = 1; i <= tableP->routineCount; ++i )
        {
            if( reachTest(tableP, routineP, i) )
            {
                ++tableP->closurePairs;
            }
        }
    }
}

// Mark and count the routines on a cycle only through the universal vertex.
// A routine flagged OPTRT_CALLSUNKNOWN has an arc to it and it has an arc to
// every routine, so a routine is on such a cycle exactly when it can reach an
// unresolved call, itself included.  That comes from an unfollowed pointer,
// not recursion, and is kept out of the report's recursion section.
static void
markArtifactCycles(OptTableP tableP)
{
OptRoutineP routineP;
OptRoutineP otherP;
int onIt;

    if( !tableP->unknownCallRoutines )
    {
        return;
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        onIt = ((routineP->flags & OPTRT_CALLSUNKNOWN) != 0);

        for( otherP = tableP->routinesP; otherP && !onIt; otherP = otherP->nextP )
        {
            if( (otherP->flags & OPTRT_CALLSUNKNOWN) && optRoutineReaches(tableP, routineP, otherP) )
            {
                onIt = 1;
            }
        }

        if( !onIt )
        {
            continue;
        }

        routineP->flags |= OPTRT_ARTCYCLE;

        // A routine on a genuine cycle is counted there, not here too.
        if( !(routineP->flags & OPTRT_ONCYCLE) )
        {
            ++tableP->artifactCycleRoutines;
        }
    }
}

// Find and count the routines a sequence-break handler can reach.  They run
// while the main line is suspended, so their return words interfere with
// every main-line routine's and form a pool of their own.  Handlers begin at
// word 4n+3 of each frame optSbsChannels() allows; the walk follows EVERY
// edge, calls included.
static void
markSbsPool(OptTableP tableP)
{
OptBlockP blockP;
OptBlockP *stackPP;
OptFlowEdgeP edgeP;
OptRoutineP routineP;
unsigned char *seenP;
int channel;
int addr;
int top;

    if( !tableP->blockCount )
    {
        return;
    }

    if( !(seenP = (unsigned char *)calloc((size_t)(tableP->blockCount + 1), 1)) ||
        !(stackPP = (OptBlockP *)calloc((size_t)tableP->blockCount, sizeof(OptBlockP))) )
    {
        fprintf(stderr, "am1: out of memory finding the optimizer's sequence-break pool\n");
        exit(1);
    }

    top = 0;

    for( channel = 0; channel < optSbsChannels(tableP); ++channel )
    {
        OptWordP wordP;

        addr = ((channel * OPTSBS_FRAMESIZE) + OPTSBS_ENTRYSLOT);

        if( !(wordP = wordAt(tableP, 0, addr)) || !inGraph(wordP) || !wordP->blockP )
        {
            continue;
        }

        ++tableP->sbsHandlers;

        if( seenP[wordP->blockP->id] )
        {
            continue;
        }

        seenP[wordP->blockP->id] = 1;
        stackPP[top++] = wordP->blockP;
    }

    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( !edgeP->toP || seenP[edgeP->toP->id] )
            {
                continue;
            }

            seenP[edgeP->toP->id] = 1;
            stackPP[top++] = edgeP->toP;
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !seenP[routineP->entryBlockP->id] )
        {
            continue;
        }

        routineP->flags |= OPTRT_SBSPOOL;
        ++tableP->sbsPoolRoutines;
    }

    free(seenP);
    free(stackPP);
}

// Where one routine's reachability bit vector starts.
// Returns a pointer into the table's closure, never NILP once the closure is
// allocated.
static unsigned int *
reachOf(OptTableP tableP, OptRoutineP routineP)
{
    return( tableP->callReachPP + ((size_t)(routineP->id - 1) * (size_t)tableP->callReachWords) );
}

// Record that a routine reaches the routine with a given 1-based id.
static void
reachSet(OptTableP tableP, OptRoutineP routineP, int id)
{
unsigned int *vecP;

    vecP = reachOf(tableP, routineP);
    vecP[(id - 1) / CG_BITS] |= (1u << ((unsigned int)((id - 1) % CG_BITS)));
}

// Does a routine reach the routine with a given 1-based id?
// Returns 1 when it does, 0 when it does not or the id is out of range.
static int
reachTest(OptTableP tableP, OptRoutineP routineP, int id)
{
unsigned int *vecP;

    if( (id < 1) || (id > tableP->routineCount) )
    {
        return(0);
    }

    vecP = reachOf(tableP, routineP);
    return( (vecP[(id - 1) / CG_BITS] & (1u << ((unsigned int)((id - 1) % CG_BITS)))) != 0 );
}

// Add everything one routine reaches to what another reaches.
static void
reachOr(OptTableP tableP, OptRoutineP intoP, OptRoutineP fromP)
{
unsigned int *dstP;
unsigned int *srcP;
int w;

    dstP = reachOf(tableP, intoP);
    srcP = reachOf(tableP, fromP);

    for( w = 0; w < tableP->callReachWords; ++w )
    {
        dstP[w] |= srcP[w];
    }
}

// Can one routine reach another through the resolved call arcs, directly or
// transitively?  Two return words interfere exactly when one routine can be
// on the call stack with the other.
// Returns 1 when fromP reaches toP, 0 when it does not, when either is NILP,
// or when the closure was never built.
int
optRoutineReaches(OptTableP tableP, OptRoutineP fromP, OptRoutineP toP)
{
    if( !tableP || !fromP || !toP || !tableP->callReachPP )
    {
        return(0);
    }

    return( reachTest(tableP, fromP, toP->id) );
}

// The -O=calls dump.  Every line is hand-writable, and each section has its
// own line prefix so a script can pull it out with one grep: routines:,
// returns:, routine N:, categories:, fallin, shared, opaque, unresolved,
// calls:, call, closure:, reach, cycles:, cycle, sbs enables:, sbs:, sbspool,
// depth:, depthbank, leaves:.  Addresses print as -O=flow prints them, bank
// and address in one six-digit octal field, so the two dumps read side by side.

// Print the routines and the call graph.
void
optDumpCalls(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;

    fprintf(fP, "routines: %d routine%s, %d jsp, %d jda, %d mixed; %d block%s and %d word%s in routines, %d and %d in none\n",
        tableP->routineCount, (tableP->routineCount == 1)?"":"s",
        tableP->routineFormCounts[OPTRF_JSP],
        tableP->routineFormCounts[OPTRF_JDA],
        tableP->routineFormCounts[OPTRF_MIXED],
        tableP->routineBlocks, (tableP->routineBlocks == 1)?"":"s",
        tableP->routineWords, (tableP->routineWords == 1)?"":"s",
        tableP->outsideBlocks, tableP->outsideWords);

    fprintf(fP, "returns: %d saved, %d jda, %d none; %d routine%s with no return word found, %d escaping\n",
        tableP->routineWordKinds[OPTRW_SAVED],
        tableP->routineWordKinds[OPTRW_JDA],
        tableP->routineWordKinds[OPTRW_NONE],
        tableP->noReturnRoutines, (tableP->noReturnRoutines == 1)?"":"s",
        tableP->openRoutines);

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        // The entry's first label makes a line checkable by hand against the
        // source; a routine with none prints "-", as the unreached list does.
        fprintf(fP, "routine %d: %02o%04o %s %s blocks %d, words %d, sites %d (%d jsp, %d jda, %d mainline), return ",
            routineP->id, routineP->bank, routineP->entryAddr,
            firstLabelName(routineP->entryBlockP->firstP),
            optRoutineFormName(routineP->form),
            routineP->blockCount, routineP->wordCount,
            (routineP->jspSites + routineP->jdaSites),
            routineP->jspSites, routineP->jdaSites, routineP->mainLineSites);

        if( routineP->returnAddr >= 0 )
        {
            fprintf(fP, "%02o%04o %s %d word%s",
                routineP->returnBank, routineP->returnAddr,
                optReturnWordName(routineP->returnWord),
                routineP->returnCount, (routineP->returnCount == 1)?"":"s");
        }
        else
        {
            fprintf(fP, "- none 0 words");
        }

        dumpRoutineFlags(fP, routineP);
        fprintf(fP, "\n");
    }

    dumpCategories(fP, tableP);
    dumpArcs(fP, tableP);
    dumpClosure(fP, tableP);
    dumpCycles(fP, tableP);
    dumpSbs(fP, tableP);
    dumpDepth(fP, tableP);
}

// Print one routine's flags after " | ", or " | -" when it has none, so every
// routine line has the same shape.
static void
dumpRoutineFlags(FILE *fP, OptRoutineP routineP)
{
static const struct
{
    unsigned int bit;
    const char *nameP;
}
flagNames[] =
{
    { OPTRT_FALLIN,         "fallin" },
    { OPTRT_FALLSOUT,       "fallsout" },
    { OPTRT_SHARED,         "shared" },
    { OPTRT_CALLSUNKNOWN,   "callsunknown" },
    { OPTRT_JUMPSUNKNOWN,   "jumpsunknown" },
    { OPTRT_NORETURN,       "noreturn" },
    { OPTRT_OPENBODY,       "open" },
    { OPTRT_ONCYCLE,        "cycle" },
    { OPTRT_ARTCYCLE,       "artcycle" },
    { OPTRT_SBSPOOL,        "sbs" },
};
int i;
int any;

    fprintf(fP, " | ");
    any = 0;

    for( i = 0; i < (int)(sizeof(flagNames) / sizeof(flagNames[0])); ++i )
    {
        if( routineP->flags & flagNames[i].bit )
        {
            fprintf(fP, "%s%s", (any)?" ":"", flagNames[i].nameP);
            any = 1;
        }
    }

    if( !any )
    {
        fprintf(fP, "-");
    }
}

// Print the three counted categories and the cases behind them.  The cases are
// re-derived from the blocks rather than kept in a list that would have to be
// kept in step.
static void
dumpCategories(FILE *fP, OptTableP tableP)
{
OptBlockP blockP;
OptFlowEdgeP edgeP;
OptRoutineP routineP;
OptRoutineP targetP;
int recovered;

    fprintf(fP, "categories: %d fall-in edge%s into %d routine%s, %d shared block%s in %d routine%s, %d unresolved call site%s in %d routine%s, %d opaque entr%s\n",
        tableP->fallInEdges, (tableP->fallInEdges == 1)?"":"s",
        tableP->fallInRoutines, (tableP->fallInRoutines == 1)?"":"s",
        tableP->sharedBlocks, (tableP->sharedBlocks == 1)?"":"s",
        tableP->sharedRoutines, (tableP->sharedRoutines == 1)?"":"s",
        tableP->callSiteSinks, (tableP->callSiteSinks == 1)?"":"s",
        tableP->unknownCallRoutines, (tableP->unknownCallRoutines == 1)?"":"s",
        tableP->opaqueEntries, (tableP->opaqueEntries == 1)?"y":"ies");

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind == OPTFK_CALL) || !edgeP->toP )
            {
                continue;
            }

            if( !(targetP = optRoutineOfBlock(tableP, edgeP->toP)) )
            {
                continue;
            }

            if( targetP->entryBlockP != edgeP->toP )
            {
                continue;
            }

            if( optRoutineOwnsBlock(targetP, blockP) )
            {
                continue;
            }

            fprintf(fP, "fallin %02o%04o from %02o%04o %s routine %d\n",
                targetP->bank, targetP->entryAddr,
                blockP->bank, blockP->endAddr,
                optFlowKindName(edgeP->kind), targetP->id);
        }
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( optBlockOwnerCount(tableP, blockP) < 2 )
        {
            continue;
        }

        fprintf(fP, "shared %02o%04o routines", blockP->bank, blockP->startAddr);

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( optRoutineOwnsBlock(routineP, blockP) )
            {
                fprintf(fP, " %d", routineP->id);
            }
        }

        fprintf(fP, "\n");
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( optBlockOwnerCount(tableP, blockP) )
        {
            continue;
        }

        if( !blockP->firstP || !(blockP->firstP->flags & (OPTF_TAKEN | OPTF_MAYBE_ENTERED)) )
        {
            continue;
        }

        fprintf(fP, "opaque %02o%04o %d word%s\n",
            blockP->bank, blockP->startAddr,
            blockP->wordCount, (blockP->wordCount == 1)?"":"s");
    }

    // The unresolved calls, so a routine flagged callsunknown can be traced
    // to the word that made it so.  A jsp stored whole can have targets the
    // code states beside the sink; they are counted, since the call is then
    // only partly unknown.
    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        recovered = 0;

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind == OPTFK_CALL) && (edgeP->recovered != OPTRV_NONE) )
            {
                ++recovered;
            }
        }

        for( edgeP = blockP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( (edgeP->kind != OPTFK_CALL) || edgeP->toP )
            {
                continue;
            }

            fprintf(fP, "unresolved %02o%04o sink:%s", blockP->bank, blockP->endAddr,
                optSinkName(edgeP->sink));

            if( recovered )
            {
                fprintf(fP, " beside %d stored", recovered);
            }

            fprintf(fP, "\n");
        }
    }
}

// Print the call arcs, in caller then callee id order.  A routine that calls
// nothing prints no line; the summary gives the arc count.
static void
dumpArcs(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;
OptCallArcP arcP;

    fprintf(fP, "calls: %d arc%s from %d site%s; %d site%s in no routine, %d unresolved\n",
        tableP->callArcCount, (tableP->callArcCount == 1)?"":"s",
        tableP->callSiteCount, (tableP->callSiteCount == 1)?"":"s",
        tableP->mainLineSites, (tableP->mainLineSites == 1)?"":"s",
        tableP->callSiteSinks);

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
        {
            fprintf(fP, "call %d -> %d sites %d\n", routineP->id, arcP->toP->id, arcP->sites);
        }
    }
}

// Print the transitive closure, one line per routine that reaches anything:
// the count, then the ids it reaches, ascending.
static void
dumpClosure(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;
OptRoutineP otherP;
int count;

    fprintf(fP, "closure: %d pair%s over %d routine%s\n",
        tableP->closurePairs, (tableP->closurePairs == 1)?"":"s",
        tableP->routineCount, (tableP->routineCount == 1)?"":"s");

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        count = 0;

        for( otherP = tableP->routinesP; otherP; otherP = otherP->nextP )
        {
            if( optRoutineReaches(tableP, routineP, otherP) )
            {
                ++count;
            }
        }

        if( !count )
        {
            continue;
        }

        fprintf(fP, "reach %d: %d |", routineP->id, count);

        for( otherP = tableP->routinesP; otherP; otherP = otherP->nextP )
        {
            if( optRoutineReaches(tableP, routineP, otherP) )
            {
                fprintf(fP, " %d", otherP->id);
            }
        }

        fprintf(fP, "\n");
    }
}

// Print the cycles, each classified, then the artifact count.  Defect and
// escape cycles name their members, "*" marking an open body.  An artifact
// cycle names none: its members are on it only because a pointer could not be
// followed, and listing them would invite the misreading the label exists to
// prevent.
static void
dumpCycles(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;
int scc;
int escapes;
int any;

    fprintf(fP, "cycles: %d defect over %d routine%s, %d escape over %d routine%s; %d routine%s on an artifact cycle\n",
        tableP->defectCycles,
        tableP->defectCycleRoutines, (tableP->defectCycleRoutines == 1)?"":"s",
        tableP->escapeCycles,
        tableP->escapeCycleRoutines, (tableP->escapeCycleRoutines == 1)?"":"s",
        tableP->artifactCycleRoutines, (tableP->artifactCycleRoutines == 1)?"":"s");

    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        escapes = 0;
        any = 0;

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( (routineP->scc != scc) || !(routineP->flags & OPTRT_ONCYCLE) )
            {
                continue;
            }

            if( !any )
            {
                escapes = ((routineP->flags & OPTRT_ESCCYCLE) != 0);
                fprintf(fP, "cycle %s:", (escapes)?"escape":"defect");
                any = 1;
            }

            fprintf(fP, " %d%s", routineP->id,
                (routineP->flags & OPTRT_OPENBODY)?"*":"");
        }

        if( any )
        {
            fprintf(fP, "\n");
        }
    }

    if( tableP->artifactCycleRoutines )
    {
        fprintf(fP, "cycle artifact: %d routine%s reach%s a routine that makes an unresolved indirect call\n",
            tableP->artifactCycleRoutines, (tableP->artifactCycleRoutines == 1)?"":"s",
            (tableP->artifactCycleRoutines == 1)?"es":"");
    }
}

// Print the sequence-break pool: the handler entries and the routines they
// can reach.  The lines are printed even when empty, since silence would not
// show that the pass ran.
static void
dumpSbs(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;
int any;

    // The evidence first.  With no enable command there are no frames, so the
    // summary below is zero whatever the start address.
    optDumpSbsEnables(fP, tableP);

    fprintf(fP, "sbs: %d handler%s, %d routine%s in the pool\n",
        tableP->sbsHandlers, (tableP->sbsHandlers == 1)?"":"s",
        tableP->sbsPoolRoutines, (tableP->sbsPoolRoutines == 1)?"":"s");

    fprintf(fP, "sbspool:");
    any = 0;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( routineP->flags & OPTRT_SBSPOOL )
        {
            fprintf(fP, " %d", routineP->id);
            any = 1;
        }
    }

    fprintf(fP, "%s\n", (any)?"":" none");
}

// Write the report's recursion section: the DEFECT cycles of resolved call
// arcs, and only those; silent when there are none.  Artifact and escape
// cycles are left out because neither is recursion and naming them would
// accuse a correct program; -O=calls shows both, labeled.
void
writeCycleList(FILE *fP, OptTableP tableP)
{
OptRoutineP routineP;
int scc;
int any;

    if( !tableP->defectCycles )
    {
        return;
    }

    fprintf(fP, "\nRecursion: %d cycle%s in the call graph, %d routine%s\n",
        tableP->defectCycles, (tableP->defectCycles == 1)?"":"s",
        tableP->defectCycleRoutines, (tableP->defectCycleRoutines == 1)?"":"s");
    fprintf(fP, "  Every call below was resolved to a single target, so this is a property of\n");
    fprintf(fP, "  the program and not of the analysis.  The \"dap rtn\" return idiom cannot\n");
    fprintf(fP, "  survive it: the second call overwrites the first call's return address, and\n");
    fprintf(fP, "  the first return then jumps wherever the second caller came from.  Either\n");
    fprintf(fP, "  the recursion is real and the routine needs a return stack of its own, or\n");
    fprintf(fP, "  one of the calls is not the one that was meant.\n");

    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        // Skip the whole component when the cycle is an escape, not just the
        // routine that escapes: its other members are ordinary routines, and
        // naming them alone would be a false accusation.
        any = 0;

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( (routineP->scc != scc) || !(routineP->flags & OPTRT_ONCYCLE) ||
                (routineP->flags & OPTRT_ESCCYCLE) )
            {
                continue;
            }

            if( !any )
            {
                fprintf(fP, "    cycle:");
                any = 1;
            }

            fprintf(fP, " bank %d %04o (%s)", routineP->bank, routineP->entryAddr,
                firstLabelName(routineP->entryBlockP->firstP));
        }

        if( any )
        {
            fprintf(fP, "\n");
        }
    }
}

// Return-word sharing, measured only: nothing below emits a finding, writes to
// the .opt report or changes a field another pass owns.
//
// Two rtn words interfere exactly when their routines can be on the call stack
// together.  Without recursion the call graph is a DAG and the interference
// graph is the comparability graph of its reachability order, which is
// perfect, so by Mirsky's theorem the minimum number of words is the longest
// chain, the maximum call depth, and each routine is colored by its longest
// path.  The saving is the candidate words minus that depth.  Each
// approximation below leans toward a smaller saving, never a larger one:
//   - A chain counts ROUTINES and the candidate set counts WORDS.  Two entries
//     into one body share a word, so a chain may count it twice: the depth is
//     too large, never too small.
//   - Chains run over the whole graph and only the counting is per bank, so a
//     chain that leaves a bank and comes back still counts both words.
//   - A cyclic component costs its whole size, since its members may all be
//     live at once.
// A return word is reached by an ordinary memory reference, so only words in
// one bank can be shared: bankSavingTotal is collectable, while
// callDepthSaving over the whole program is an upper bound no layout reaches.

// Does this routine hold the first claim on its return word?  Two entries into
// one body name the same word, and the candidates are WORDS; routines are in
// bank then entry-address order, so the first claimant is a stable choice.
// Returns 1 when no earlier routine names the same word, 0 when one does or
// when this routine has no return word at all.
static int
ownsReturnWord(OptTableP tableP, OptRoutineP routineP)
{
OptRoutineP otherP;

    if( !tableP || !routineP || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    for( otherP = tableP->routinesP; otherP && (otherP != routineP); otherP = otherP->nextP )
    {
        if( (otherP->returnAddr == routineP->returnAddr) &&
            (otherP->returnBank == routineP->returnBank) )
        {
            return(0);
        }
    }

    return(1);
}

// Is this routine a leaf: no call arc and no unresolved jump or call?  Every
// leaf in a bank can then share one return word, with no call graph needed;
// an unfollowed jump may have gone into another leaf, which breaks that.
// Returns 1 for a leaf holding a return word, 0 otherwise.
static int
isLeafRoutine(OptRoutineP routineP)
{
    if( !routineP || (routineP->returnAddr < 0) )
    {
        return(0);
    }

    if( routineP->callArcs )
    {
        return(0);
    }

    if( routineP->flags & (OPTRT_JUMPSUNKNOWN | OPTRT_CALLSUNKNOWN) )
    {
        return(0);
    }

    return(1);
}

// The longest chain of return-word-holding routines, counting only words in
// one bank (every bank when bank is negative) and routines of one pool: 0 the
// main line, 1 those a handler can reach, negative both.  The pools must be
// kept apart: a handler runs between any two words of what it interrupted, so
// sharing a word across them lets a break destroy it mid-call.  Component
// numbers are already reverse topological, so one ascending sweep finds every
// callee's answer finished.  sccDepthP is caller-owned scratch of sccCount + 1.
// Returns the longest chain, 0 when there are no components or no candidates.
static int
chainDepth(OptTableP tableP, int bank, int pool, int *sccDepthP)
{
OptRoutineP routineP;
OptCallArcP arcP;
int scc;
int own;
int best;
int deepest;

    deepest = 0;

    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        own = 0;
        best = 0;

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineP->scc != scc )
            {
                continue;
            }

            // Every member holding a return word in the bank and pool asked
            // about adds one.
            if( (routineP->returnAddr >= 0) &&
                ((bank < 0) || (routineP->returnBank == bank)) &&
                ((pool < 0) || (pool == ((routineP->flags & OPTRT_SBSPOOL)?1:0))) )
            {
                ++own;
            }

            for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
            {
                // An arc inside the component is not a step down the chain;
                // the component's size already counts its members.
                if( arcP->toP->scc == scc )
                {
                    continue;
                }

                if( sccDepthP[arcP->toP->scc] > best )
                {
                    best = sccDepthP[arcP->toP->scc];
                }
            }
        }

        sccDepthP[scc] = (own + best);

        if( sccDepthP[scc] > deepest )
        {
            deepest = sccDepthP[scc];
        }
    }

    return(deepest);
}

// Fill every routine's callDepth, the longest chain it heads counting every
// routine whether or not it holds a return word: the color it would get.
// Reported only; nothing reads it back.
static void
fillRoutineDepths(OptTableP tableP, int *sccDepthP)
{
OptRoutineP routineP;
OptCallArcP arcP;
int scc;
int size;
int best;

    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        size = 0;
        best = 0;

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineP->scc != scc )
            {
                continue;
            }

            ++size;

            for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
            {
                if( arcP->toP->scc == scc )
                {
                    continue;
                }

                if( sccDepthP[arcP->toP->scc] > best )
                {
                    best = sccDepthP[arcP->toP->scc];
                }
            }
        }

        sccDepthP[scc] = (size + best);
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        routineP->callDepth = (routineP->scc)?sccDepthP[routineP->scc]:0;
    }
}

// Measure what return-word sharing could recover; the block comment above
// ownsReturnWord() says what each figure means.
void
optMeasureSharing(OptTableP tableP)
{
OptRoutineP routineP;
int *sccDepthP;
int mainLeaf[MAXBANK + 1];
int poolLeaf[MAXBANK + 1];
int mainWords;
int mainDepth;
int poolDepth;
int saving;
int bank;
int i;

    if( !tableP )
    {
        return;
    }

    for( i = 0; i <= MAXBANK; ++i )
    {
        tableP->bankReturnWords[i] = 0;
        tableP->bankMaxDepth[i] = 0;
        tableP->bankSaving[i] = 0;
        tableP->bankLeafWords[i] = 0;
        tableP->bankPoolWords[i] = 0;
        tableP->bankPoolDepth[i] = 0;
        tableP->bankPoolLeafWords[i] = 0;
        mainLeaf[i] = 0;
        poolLeaf[i] = 0;
    }

    tableP->returnWordCount = 0;
    tableP->sharedReturnWords = 0;
    tableP->callMaxDepth = 0;
    tableP->callDepthSaving = 0;
    tableP->bankSavingTotal = 0;
    tableP->leafRoutines = 0;
    tableP->leafSaving = 0;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        routineP->callDepth = 0;
    }

    if( !tableP->routineCount || !tableP->sccCount )
    {
        return;
    }

    // The candidate words and the leaves among them.  Both count WORDS, so a
    // second entry into a body with a word adds nothing to either.
    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( isLeafRoutine(routineP) )
        {
            ++tableP->leafRoutines;
        }

        if( routineP->returnAddr < 0 )
        {
            continue;
        }

        if( !ownsReturnWord(tableP, routineP) )
        {
            ++tableP->sharedReturnWords;
            continue;
        }

        ++tableP->returnWordCount;
        bank = routineP->returnBank;

        if( (bank >= 0) && (bank <= MAXBANK) )
        {
            ++tableP->bankReturnWords[bank];

            if( routineP->flags & OPTRT_SBSPOOL )
            {
                ++tableP->bankPoolWords[bank];

                if( isLeafRoutine(routineP) )
                {
                    ++poolLeaf[bank];
                    ++tableP->bankPoolLeafWords[bank];
                }
            }
            else if( isLeafRoutine(routineP) )
            {
                ++mainLeaf[bank];
            }

            if( isLeafRoutine(routineP) )
            {
                ++tableP->bankLeafWords[bank];
            }
        }
    }

    if( !(sccDepthP = (int *)calloc((size_t)(tableP->sccCount + 1), sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory measuring the optimizer's call depth\n");
        exit(1);
    }

    // callMaxDepth is the longest chain over every bank and both pools.  The
    // per-bank savings below do not use it; callDepthSaving beside it is the
    // upper bound no layout can collect.  A bank with no return word is
    // skipped rather than printed as depth zero.
    tableP->callMaxDepth = chainDepth(tableP, -1, -1, sccDepthP);
    tableP->callDepthSaving = (tableP->returnWordCount - tableP->callMaxDepth);

    if( tableP->callDepthSaving < 0 )
    {
        tableP->callDepthSaving = 0;
    }

    for( i = 0; i <= MAXBANK; ++i )
    {
        if( !tableP->bankReturnWords[i] )
        {
            continue;
        }

        // Two pools colored independently, their savings added.  A bank with
        // no handler-reachable routine has an empty second pool.
        mainWords = (tableP->bankReturnWords[i] - tableP->bankPoolWords[i]);
        mainDepth = chainDepth(tableP, i, 0, sccDepthP);
        poolDepth = chainDepth(tableP, i, 1, sccDepthP);

        tableP->bankMaxDepth[i] = mainDepth;
        tableP->bankPoolDepth[i] = poolDepth;

        saving = 0;

        if( mainWords > mainDepth )
        {
            saving += (mainWords - mainDepth);
        }

        if( tableP->bankPoolWords[i] > poolDepth )
        {
            saving += (tableP->bankPoolWords[i] - poolDepth);
        }

        tableP->bankSaving[i] = saving;
        tableP->bankSavingTotal += saving;

        // The leaf cut: all leaves of a bank and pool share ONE word, saving
        // one less than the leaf words there.
        if( mainLeaf[i] > 1 )
        {
            tableP->leafSaving += (mainLeaf[i] - 1);
        }

        if( poolLeaf[i] > 1 )
        {
            tableP->leafSaving += (poolLeaf[i] - 1);
        }
    }

    fillRoutineDepths(tableP, sccDepthP);
    free(sccDepthP);
}

// Print the sharing measurement, last in the dump so the earlier sections
// read the same with or without it.
static void
dumpDepth(FILE *fP, OptTableP tableP)
{
int i;

    fprintf(fP, "depth: max call depth %d over %d return word%s in %d routine%s, %d shared; saving %d whole program, %d per bank\n",
        tableP->callMaxDepth,
        tableP->returnWordCount, (tableP->returnWordCount == 1)?"":"s",
        tableP->routineCount, (tableP->routineCount == 1)?"":"s",
        tableP->sharedReturnWords,
        tableP->callDepthSaving, tableP->bankSavingTotal);

    for( i = 0; i <= MAXBANK; ++i )
    {
        if( !tableP->bankReturnWords[i] )
        {
            continue;
        }

        // The pool figures are printed even when zero, so a reader can check
        // that (words - pool - depth) + (pool - pooldepth) equals the saving.
        fprintf(fP, "depthbank %d: %d word%s, depth %d, saving %d, %d leaf word%s, pool %d depth %d\n",
            i, tableP->bankReturnWords[i], (tableP->bankReturnWords[i] == 1)?"":"s",
            tableP->bankMaxDepth[i], tableP->bankSaving[i],
            tableP->bankLeafWords[i], (tableP->bankLeafWords[i] == 1)?"":"s",
            tableP->bankPoolWords[i], tableP->bankPoolDepth[i]);
    }

    fprintf(fP, "leaves: %d leaf routine%s, leaf cut alone saves %d word%s\n",
        tableP->leafRoutines, (tableP->leafRoutines == 1)?"":"s",
        tableP->leafSaving, (tableP->leafSaving == 1)?"":"s");
}
