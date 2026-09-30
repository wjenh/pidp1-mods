/*
 * The am1 optimizer's guess in place of precondition P5: no location a rewrite
 * involves is shared with a device, a sequence-break handler, or code that is
 * there to take the time it takes.  P5 cannot be derived from the source; -O1
 * takes it from the author's optimize/endoptimize region, and -O2 guesses it
 * here.  Five heuristics each mark locations a P5 violation would make unsafe:
 *
 *     H1  handler territory   every block a sequence-break handler reaches,
 *                             calls included, every word it writes, and the
 *                             break frame words
 *     H2  delay loop          a cycle that writes only its own isp/idx
 *                             counters, with no call, halt or in-out transfer
 *     H3  device loop         a cycle holding an in-out transfer to a device,
 *                             or an xct of a word the program writes ("lac
 *                             [ioh]; dac wait; ... xct wait" is a display wait)
 *     H4  device buffer       a word whose address is loaded, directly or via
 *                             a pool word, where an in-out transfer runs
 *     H5  absolute address    a memory reference whose address is written as a
 *                             number, and the word it names (a shared mailbox)
 *     H6  computed address    the run of code from T to the next label, where
 *                             one block loads T's address as a value and
 *                             changes it before use ("law tbl; add ix; dap
 *                             go"), or stores it where an idx or isp walks it
 *
 * A heuristic refuses a finding when it marks any word of the finding's
 * footprint: the pattern's words, and for T3, T8a-d and T14 also the word each
 * reads through (T3's intermediate jmp, T8's pool word, T14's temporaries).
 *
 * H6 is not like the other five.  It refuses only a rewrite that changes the
 * program's length, since one word replaced by one word moves no entry, and it
 * refuses one at every level, in a region or not (OPTGH_LENGTH): the offset
 * the program adds is known only when it runs, so no declaration can vouch for
 * the run.  It is blind to arithmetic done in another block, except a stored
 * pointer an idx or isp walks, and to an entry past the run's first label.
 *
 * The loops are a loop-nesting forest over the blocks, built without dominators
 * so an irreducible loop is still a loop: the strongly connected components,
 * then inside each the components left once the edges into its header are
 * removed, and so on down.  Call edges are left out (a call site's return edge
 * is kept), and so is a halt's edge to the next word, since going round it
 * takes someone pressing Continue.
 *
 * H2 and H3 judge cycles, not whole loops.  A cycle is the natural loop of one
 * back edge: a header, one latch, and every block of the loop that reaches the
 * latch without passing the header.  Loops sharing a header are one loop in the
 * forest, so judging whole loops would lump a spin-wait into the main loop it
 * jumps back to.  The dump's "literal" line keeps the whole-loop counts.
 *
 * optBuildGuess() runs once per table, after the rules and regions and before
 * -O1's transform, so it describes the program as written.  It refuses nothing
 * itself and writes only its side table and each finding's guess fields;
 * opttransform.c leaves alone a finding whose guess bits hold a heuristic
 * optGuessApplies() says applies.  Tarjan is iterative, so a long chain of
 * blocks cannot overflow the C stack.  freeGuess() releases the side table.
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

// The most words a footprint holds: T14's four-word exchange and its two
// temporaries, with room to spare.
#define GUESS_MAXFOOT       (2 * OPTFIND_MAXWORDS)

// The longest run H6 marks from one T; a longer one is cut here.
#define GUESS_MAXRUN        512

// What one block does with an address it loads as a value (H6).
typedef enum
{
    GCM_NONE,               // used as loaded
    GCM_CONSTANT,           // an unwritten constant added or subtracted
    GCM_VARIABLE            // anything else added or subtracted
} GComputedMod;

// One run H6 marks.
typedef struct
{
    OptWordP siteP;         // the word that loads T's address
    OptWordP tP;            // T, the run's first word
    int length;             // words from T
} GRun;

// A loop's class, for the dump.  Exactly one per loop; a loop that is both
// would need an in-out transfer, which a delay loop may not hold.
typedef enum
{
    GLC_PLAIN,          // neither of the two below
    GLC_DELAY,          // H2 marks it
    GLC_DEVICE          // H3 marks it
} GLoopClass;

// One loop of the nesting forest.
typedef struct
{
    int id;                 // 1 based, in creation order: every parent before its children
    int parent;             // the enclosing loop's id, 0 for a loop at the top
    int depth;              // 1 for a loop at the top
    OptBlockP headerP;      // the block the loop is entered at
    int *memberIdsP;        // its blocks' ids, the nested loops' included
    int blockCount;
    int wordCount;
    int iotCount;           // in-out transfers in its words
    OptWordP firstIotP;     // the first of them, NILP if none
    int xctVarCount;        // xct words whose target the program writes, or that
                            // the reference pass could not name
    OptWordP firstXctP;     // the first of them, NILP if none
    int callCount;          // blocks ending in a call
    int callIotCount;       // of those, calls into a routine whose body, or a routine
                            // it reaches, holds an in-out transfer
    int callUnknownCount;   // of those, calls whose callee the graph could not name
    int haltCount;          // blocks ending in a halt
    int writeCount;         // direct or followed writes its words make
    int counterCount;       // distinct words its words isp or idx
    int otherWrites;        // writes to a word that is not one of those counters
    int unknownWrites;      // indirect writes the reference pass could not follow
    int readVars;           // reads of a word, not a counter, that something writes,
                            // and every read through a pointer
    GLoopClass kind;
    int cycleOf;            // for a cycle, the loop it is a cycle of; 0 for a loop
    OptBlockP latchP;       // for a cycle, its latch; NILP for a loop
} GLoop;

// The side table.
typedef struct optguess
{
    int blockCount;
    OptBlockP *blocksPP;            // by block id, index 0 unused

    // H1
    int frames;                     // channels whose frames exist (optSbsChannels)
    int handlerEntries;             // frame entry words in the graph
    OptWordP *handlerEntryPP;       // by block id: the entry word whose walk reached it
    int handlerBlocks;
    OptWordP *handlerWriterPP[MAXBANK + 1];     // by address: who writes it; the entry
                                                // word itself for a frame word
    int handlerWritten;             // words marked
    int handlerIndirect;            // unfollowed indirect writes in handler code

    // H2 and H3
    GLoop *loopsP;                  // index 0 unused: the loops, then the cycles
    int loopCount;                  // loops and cycles
    int forestCount;                // of those, the loops: ids 1 to forestCount
    int loopCap;
    int *innerLoopP;                // by block id: the innermost loop, 0 for none
    int *delayLoopP;                // by block id: the innermost delay loop holding it
    int *deviceLoopP;               // by block id: the innermost device loop holding it
    int *delayCycleP;               // by block id: the smallest delay cycle holding it
    int *deviceCycleP;              // by block id: the smallest device cycle holding it
    int *leafCycleP;                // by block id: the smallest device cycle with no call
    int *predStartP;                // by block id: where its predecessors start in
    int *predIdsP;                  // predIdsP, which lists every non-sink edge's source
    int delayLoops;
    int deviceLoops;
    int delayCycles;
    int deviceCycles;
    unsigned char *routineIotP;     // by routine id: the body holds an in-out transfer
    unsigned char *routineIotReachP;    // by routine id: that, or a routine it reaches does

    // H4
    OptWordP *bufferLoaderPP[MAXBANK + 1];      // by address: the word that loads it
    int bufferWords;
    int loaderBlocks;               // blocks that hold an in-out transfer
    int loaderRoutines;             // routines whose body does

    // H5
    OptWordP *absoluteRefPP[MAXBANK + 1];       // by address: the reference written as a number
    int absoluteRefs;               // references written as a number
    int absoluteWords;              // words marked, the references themselves included

    // H6
    OptWordP *computedSitePP[MAXBANK + 1];      // by address: the site whose run holds it
    GRun *runsP;                    // the runs marked, in table order
    int runCount;
    int runCap;
    int addressSites;               // words that load an address as a value
    int computedSites;              // of those, changed before use or walked
    int computedWords;              // words marked

    // The tallies, for the report and the dump.
    int live;                     // live findings
    int refused[OPTGH_COUNT];       // of those, refused by each heuristic
    int refusedAny;                 // by at least one
    int carriedLive;                // live T3 and T8a-d findings, -O2's first rules
    int carriedRefused[OPTGH_COUNT];
    int carriedAny;
    int ruleLive[OPTRULE_COUNT];
    int ruleAny[OPTRULE_COUNT];
    int variantCalls;               // live findings no heuristic refuses that lie in a loop
                                    // calling a routine that holds an in-out transfer
    int variantPure;                // H2 refusals that would stand if a delay loop also had
                                    // to read no word something writes
    int variantLeaf;                // H3 refusals that would stand if a device loop also had
                                    // to make no call
    int literalH2;                  // live findings H2 would refuse marking whole loops
    int literalH3;                  // and H3
    int literalAny;                 // and all five, so
} OptGuess, *OptGuessP;

// Tarjan's working state over the blocks, and the depth-first frames.
typedef struct
{
    int index;
    int lowlink;
    int onStack;
} GNode;

typedef struct
{
    int blockId;
    OptFlowEdgeP edgeP;     // the next successor to consider
} GFrame;

static const struct
{
    const char *tagP;
    const char *textP;
} guessTable[OPTGH_COUNT] =
{
    { "H1", "handler territory: code a sequence-break handler reaches, and every word it writes" },
    { "H2", "delay loop: a cycle that writes nothing but its own counters, with no call and no in-out transfer" },
    { "H3", "device loop: a cycle that holds an in-out transfer to a device, or executes a word the program writes" },
    { "H4", "device buffer: a word whose address is loaded where an in-out transfer runs" },
    { "H5", "absolute address: a memory reference written as a number, and the word it names" },
    { "H6", "computed address: a run of code an address changed at run time indexes, where a length change"
            " at any level would move an entry" }
};

static void *gAlloc(size_t count, size_t size);
static void markWord(OptWordP **mapPPP, int bank, int addr, OptWordP byP, int *countP);
static OptWordP wordMark(OptWordP **mapPPP, int bank, int addr);

static void findHandlers(OptTableP tableP, OptGuessP gP);
static void findLoops(OptTableP tableP, OptGuessP gP);
static void analyzeSet(OptTableP tableP, OptGuessP gP, int setId, int *setOfP, unsigned char *blockedP,
    GNode *nodesP, int *stackP, GFrame *framesP);
static int newLoop(OptGuessP gP, int parent, int *idsP, int count);
static void findCycles(OptTableP tableP, OptGuessP gP, int loop, unsigned char *inP, unsigned char *seenP,
    int *listP);
static int flowsTo(OptGuessP gP, int from, int to);
static void markCycle(OptGuessP gP, int *mapP, GLoop *cycleP);
static OptBlockP chooseHeader(OptTableP tableP, OptGuessP gP, GLoop *loopP, unsigned char *inP);
static int considered(OptFlowEdgeP edgeP, int setId, int *setOfP, unsigned char *blockedP);
static void findRoutineIots(OptTableP tableP, OptGuessP gP);
static int blockHasIot(OptTableP tableP, OptBlockP blockP, OptWordP *firstPP);
static int deviceIot(OptWordP wordP);
static void measureLoop(OptTableP tableP, OptGuessP gP, GLoop *loopP);
static void findBuffers(OptTableP tableP, OptGuessP gP);
static void loadsFrom(OptTableP tableP, OptGuessP gP, OptWordP wordP);
static void findAbsolutes(OptTableP tableP, OptGuessP gP);
static int numericField(PNodeP nodeP, int *numberP);
static void findComputed(OptTableP tableP, OptGuessP gP);
static OptWordP addressLoaded(OptWordP wordP);
static GComputedMod followAddress(OptTableP tableP, OptWordP siteP, int isAC, int *offsetP, OptWordP *storePP);
static OptEdgeP directEdge(OptWordP wordP, OptRole role);
static int unwrittenWord(OptWordP wordP);
static OptWordP addressHeld(OptWordP wordP);
static int storeWalked(OptWordP storeP);
static void markRun(OptTableP tableP, OptGuessP gP, OptWordP siteP, OptWordP tP);
static int footprint(OptTableP tableP, OptFindingP findingP, OptWordP *wordsPP);
static void judgeFinding(OptTableP tableP, OptGuessP gP, OptFindingP findingP);
static int ruleCarried(OptRuleId rule);
static void describeGuess(OptTableP tableP, OptFindingP findingP, int h, char *bufP, int size);

// Build the guess over a table whose rules and regions are complete, and mark
// every live finding with the heuristics that would refuse it.
void
optBuildGuess(OptTableP tableP)
{
OptGuessP gP;
OptBlockP blockP;
OptFindingP findingP;

    if( !tableP )
    {
        return;
    }

    freeGuess(tableP);

    gP = (OptGuessP)gAlloc(1, sizeof(OptGuess));
    tableP->guessP = gP;
    gP->blockCount = tableP->blockCount;
    gP->blocksPP = (OptBlockP *)gAlloc((size_t)(gP->blockCount + 1), sizeof(OptBlockP));

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        gP->blocksPP[blockP->id] = blockP;
    }

    findHandlers(tableP, gP);
    findRoutineIots(tableP, gP);
    findLoops(tableP, gP);
    findBuffers(tableP, gP);
    findAbsolutes(tableP, gP);
    findComputed(tableP, gP);

    for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        judgeFinding(tableP, gP, findingP);
    }
}

// Allocate zeroed memory.  Returns it, never NILP: out of memory exits.
static void *
gAlloc(size_t count, size_t size)
{
void *memP;

    if( !(memP = calloc((count)?count:1, (size)?size:1)) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's -O2 guess\n");
        exit(1);
    }

    return(memP);
}

// Record against a bank and address the word that caused a mark, keeping the
// first one, and count the address the first time it is marked.
static void
markWord(OptWordP **mapPPP, int bank, int addr, OptWordP byP, int *countP)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) || !byP )
    {
        return;
    }

    if( !mapPPP[bank] )
    {
        mapPPP[bank] = (OptWordP *)gAlloc(BANKSIZE, sizeof(OptWordP));
    }

    if( !mapPPP[bank][addr] )
    {
        mapPPP[bank][addr] = byP;
        ++*countP;
    }
}

// Look up the word that caused a mark at a bank and address.
// Returns it, or NILP when the address is not marked.
static OptWordP
wordMark(OptWordP **mapPPP, int bank, int addr)
{
    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) || !mapPPP[bank] )
    {
        return(NILP);
    }

    return( mapPPP[bank][addr] );
}

// H1: handler territory.

// Walk from every frame entry, calls included (as markSbsPool() and
// findHandlerSet() do), marking every word a reached block writes.  The three
// words below each entry are marked even with no code at the entry, because
// the break itself stores AC, the PC word and IO there (handbook page 26).
static void
findHandlers(OptTableP tableP, OptGuessP gP)
{
OptBlockP blockP;
OptFlowEdgeP flowP;
OptWordP entryP;
OptWordP wordP;
OptEdgeP edgeP;
int *stackP;
int top;
int channel;
int base;
int slot;
int addr;

    gP->frames = optSbsChannels(tableP);
    gP->handlerEntryPP = (OptWordP *)gAlloc((size_t)(gP->blockCount + 1), sizeof(OptWordP));
    stackP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));

    for( channel = 0; channel < gP->frames; ++channel )
    {
        base = (channel * OPTSBS_FRAMESIZE);
        entryP = wordAt(tableP, 0, (base + OPTSBS_ENTRYSLOT));

        for( slot = 0; slot < OPTSBS_ENTRYSLOT; ++slot )
        {
            // The report needs a word to name as the marker: the entry, else
            // the frame word itself, else no mark.
            markWord(gP->handlerWriterPP, 0, (base + slot),
                (entryP)?entryP:wordAt(tableP, 0, (base + slot)), &gP->handlerWritten);
        }

        if( !entryP || !inGraph(entryP) || !entryP->blockP )
        {
            continue;
        }

        ++gP->handlerEntries;

        if( gP->handlerEntryPP[entryP->blockP->id] )
        {
            continue;
        }

        // One walk per entry, so every block records the first entry that
        // reached it.
        top = 0;
        gP->handlerEntryPP[entryP->blockP->id] = entryP;
        stackP[top++] = entryP->blockP->id;

        while( top > 0 )
        {
            blockP = gP->blocksPP[stackP[--top]];

            for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
            {
                if( !flowP->toP || gP->handlerEntryPP[flowP->toP->id] )
                {
                    continue;
                }

                gP->handlerEntryPP[flowP->toP->id] = entryP;
                stackP[top++] = flowP->toP->id;
            }
        }
    }

    free(stackP);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !gP->handlerEntryPP[blockP->id] )
        {
            continue;
        }

        ++gP->handlerBlocks;

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
                    // The write lands on the followed edge, or somewhere
                    // unknown; the pointer word itself is only read.
                    if( edgeP->flags & OPTEF_UNKNOWN )
                    {
                        ++gP->handlerIndirect;
                    }

                    continue;
                }

                markWord(gP->handlerWriterPP, edgeP->toBank, edgeP->toAddr, wordP, &gP->handlerWritten);
            }
        }
    }
}

// H2 and H3: the loop-nesting forest and the two loop classes.

// Build the forest, measure every loop, and mark each block with the innermost
// delay loop and the innermost device loop that hold it.
static void
findLoops(OptTableP tableP, OptGuessP gP)
{
int *setOfP;
unsigned char *blockedP;
GNode *nodesP;
int *stackP;
GFrame *framesP;
int setId;
int i;
int b;
int total;
int *fillP;
OptFlowEdgeP edgeP;
GLoop *loopP;
unsigned char *inP;
unsigned char *seenP;
int *listP;

    gP->innerLoopP =(int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->delayLoopP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->deviceLoopP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->delayCycleP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->deviceCycleP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->leafCycleP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    gP->loopCap = 16;
    gP->loopsP = (GLoop *)gAlloc((size_t)gP->loopCap, sizeof(GLoop));

    if( !gP->blockCount )
    {
        return;
    }

    // setOfP is the set a block is analyzed in: 0 (the whole graph), then the
    // innermost loop found so far, so at the end it is each block's innermost.
    setOfP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    blockedP = (unsigned char *)gAlloc((size_t)(gP->blockCount + 1), 1);
    nodesP = (GNode *)gAlloc((size_t)(gP->blockCount + 1), sizeof(GNode));
    stackP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    framesP = (GFrame *)gAlloc((size_t)(gP->blockCount + 1), sizeof(GFrame));

    // Build the predecessor lists, every edge kind included: count, prefix-sum,
    // fill.
    gP->predStartP = (int *)gAlloc((size_t)(gP->blockCount + 2), sizeof(int));
    total = 0;

    for( b = 1; b <= gP->blockCount; ++b )
    {
        for( edgeP = gP->blocksPP[b]->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( edgeP->toP )
            {
                ++gP->predStartP[edgeP->toP->id + 1];
                ++total;
            }
        }
    }

    for( b = 1; b <= (gP->blockCount + 1); ++b )
    {
        gP->predStartP[b] += gP->predStartP[b - 1];
    }

    gP->predIdsP = (int *)gAlloc((size_t)total, sizeof(int));
    fillP = (int *)gAlloc((size_t)(gP->blockCount + 2), sizeof(int));

    for( b = 1; b <= gP->blockCount; ++b )
    {
        for( edgeP = gP->blocksPP[b]->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( edgeP->toP )
            {
                gP->predIdsP[gP->predStartP[edgeP->toP->id] + fillP[edgeP->toP->id]++] = b;
            }
        }
    }

    free(fillP);

    // Breadth first: the whole graph, then each loop in creation order.  A
    // loop's children are created while it is analyzed, so they come after it.
    analyzeSet(tableP, gP, 0, setOfP, blockedP, nodesP, stackP, framesP);

    for( setId = 1; setId <= gP->loopCount; ++setId )
    {
        analyzeSet(tableP, gP, setId, setOfP, blockedP, nodesP, stackP, framesP);
    }

    for( b = 1; b <= gP->blockCount; ++b )
    {
        gP->innerLoopP[b] = setOfP[b];
    }

    free(setOfP);
    free(blockedP);
    free(nodesP);
    free(stackP);
    free(framesP);

    // Measure and mark the loops.  Parents precede children, so id order leaves
    // each block with the innermost loop of each class.
    gP->forestCount = gP->loopCount;

    for( i = 1; i <= gP->forestCount; ++i )
    {
        loopP = &gP->loopsP[i];
        measureLoop(tableP, gP, loopP);

        if( loopP->kind == GLC_DELAY )
        {
            ++gP->delayLoops;
        }
        else if( loopP->kind == GLC_DEVICE )
        {
            ++gP->deviceLoops;
        }

        for( b = 0; b < loopP->blockCount; ++b )
        {
            if( loopP->kind == GLC_DELAY )
            {
                gP->delayLoopP[loopP->memberIdsP[b]] = loopP->id;
            }
            else if( loopP->kind == GLC_DEVICE )
            {
                gP->deviceLoopP[loopP->memberIdsP[b]] = loopP->id;
            }
        }
    }

    // Then every loop's cycles, appended after the loops.
    inP = (unsigned char *)gAlloc((size_t)(gP->blockCount + 1), 1);
    seenP = (unsigned char *)gAlloc((size_t)(gP->blockCount + 1), 1);
    listP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));

    for( i = 1; i <= gP->forestCount; ++i )
    {
        findCycles(tableP, gP, i, inP, seenP, listP);
    }

    free(inP);
    free(seenP);
    free(listP);
}

// Make a cycle of each latch of one loop, measure it and mark its blocks.  inP
// and seenP are zero on entry and on return; listP has room for every block.
static void
findCycles(OptTableP tableP, OptGuessP gP, int loop, unsigned char *inP, unsigned char *seenP, int *listP)
{
GLoop *loopP;
GLoop *cycleP;
int header;
int latch;
int count;
int next;
int q;
int r;
int p;
int k;
int m;
int id;

    loopP = &gP->loopsP[loop];
    header = loopP->headerP->id;

    for( m = 0; m < loopP->blockCount; ++m )
    {
        inP[loopP->memberIdsP[m]] = 1;
    }

    for( p = gP->predStartP[header]; p < gP->predStartP[header + 1]; ++p )
    {
        latch = gP->predIdsP[p];

        // A block with two edges to the header is listed twice; one cycle.
        for( k = gP->predStartP[header]; k < p; ++k )
        {
            if( gP->predIdsP[k] == latch )
            {
                break;
            }
        }

        if( (k < p) || !inP[latch] || !flowsTo(gP, latch, header) )
        {
            continue;
        }

        // Backward from the latch, inside the loop, stopping at the header.
        count = 0;
        seenP[header] = 1;
        listP[count++] = header;

        if( !seenP[latch] )
        {
            seenP[latch] = 1;
            listP[count++] = latch;
        }

        for( next = 1; next < count; ++next )
        {
            q = listP[next];

            for( k = gP->predStartP[q]; k < gP->predStartP[q + 1]; ++k )
            {
                r = gP->predIdsP[k];

                if( inP[r] && !seenP[r] && flowsTo(gP, r, q) )
                {
                    seenP[r] = 1;
                    listP[count++] = r;
                }
            }
        }

        for( k = 0; k < count; ++k )
        {
            seenP[listP[k]] = 0;
        }

        id = newLoop(gP, loop, listP, count);
        loopP = &gP->loopsP[loop];      // newLoop() may have moved the array
        cycleP = &gP->loopsP[id];
        cycleP->depth = loopP->depth;
        cycleP->cycleOf = loop;
        cycleP->headerP = loopP->headerP;
        cycleP->latchP = gP->blocksPP[latch];
        measureLoop(tableP, gP, cycleP);

        if( cycleP->kind == GLC_DELAY )
        {
            ++gP->delayCycles;
            markCycle(gP, gP->delayCycleP, cycleP);
        }
        else if( cycleP->kind == GLC_DEVICE )
        {
            ++gP->deviceCycles;
            markCycle(gP, gP->deviceCycleP, cycleP);

            if( !cycleP->callCount )
            {
                markCycle(gP, gP->leafCycleP, cycleP);
            }
        }
    }

    for( m = 0; m < loopP->blockCount; ++m )
    {
        inP[loopP->memberIdsP[m]] = 0;
    }
}

// Test for a loop edge (any but a call or a halt's resumption) between blocks.
// Returns 1 when there is one, 0 when there is not.
static int
flowsTo(OptGuessP gP, int from, int to)
{
OptFlowEdgeP edgeP;

    for( edgeP = gP->blocksPP[from]->succP; edgeP; edgeP = edgeP->nextP )
    {
        if( edgeP->toP && (edgeP->toP->id == to) && (edgeP->kind != OPTFK_CALL) &&
            (edgeP->kind != OPTFK_RESUME) )
        {
            return(1);
        }
    }

    return(0);
}

// Mark a cycle's blocks in a by-block map, keeping the smallest cycle, and the
// first made of equal ones, where one is already there.
static void
markCycle(OptGuessP gP, int *mapP, GLoop *cycleP)
{
int m;
int b;

    for( m = 0; m < cycleP->blockCount; ++m )
    {
        b = cycleP->memberIdsP[m];

        if( !mapP[b] || (cycleP->blockCount < gP->loopsP[mapP[b]].blockCount) )
        {
            mapP[b] = cycleP->id;
        }
    }
}

// Test whether a successor edge counts inside the set being analyzed.  Not a
// call; not a halt's resumption, or "hlt; jmp begin" would make one loop of a
// whole program; not an edge into a blocked header, which is what exposes the
// nested loops.  Returns 1 when the edge counts, 0 when it does not.
static int
considered(OptFlowEdgeP edgeP, int setId, int *setOfP, unsigned char *blockedP)
{
    if( !edgeP->toP || (edgeP->kind == OPTFK_CALL) || (edgeP->kind == OPTFK_RESUME) )
    {
        return(0);
    }

    if( (setOfP[edgeP->toP->id] != setId) || blockedP[edgeP->toP->id] )
    {
        return(0);
    }

    return(1);
}

// Make a loop of every strongly connected component with a cycle in one set:
// the whole graph when setId is 0, else one loop's blocks minus the edges into
// its header.  Iterative Tarjan.
static void
analyzeSet(OptTableP tableP, OptGuessP gP, int setId, int *setOfP, unsigned char *blockedP,
    GNode *nodesP, int *stackP, GFrame *framesP)
{
int *membersP;
int memberCount;
int *compP;
int compCount;
int firstNew;
int lastNew;
int rootIndex;
int m;
int b;
int top;
int frameTop;
int nextIndex;
int current;
int target;
int selfEdge;
OptFlowEdgeP edgeP;
GLoop *loopP;
unsigned char *inP;

    // The set's members, in id order.
    membersP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    memberCount = 0;

    if( setId == 0 )
    {
        for( b = 1; b <= gP->blockCount; ++b )
        {
            membersP[memberCount++] = b;
        }
    }
    else
    {
        for( m = 0; m < gP->loopsP[setId].blockCount; ++m )
        {
            membersP[memberCount++] = gP->loopsP[setId].memberIdsP[m];
        }
    }

    for( m = 0; m < memberCount; ++m )
    {
        nodesP[membersP[m]].index = 0;
        nodesP[membersP[m]].lowlink = 0;
        nodesP[membersP[m]].onStack = 0;
    }

    compP = (int *)gAlloc((size_t)(gP->blockCount + 1), sizeof(int));
    firstNew = (gP->loopCount + 1);
    top = 0;
    nextIndex = 0;

    for( rootIndex = 0; rootIndex < memberCount; ++rootIndex )
    {
        if( nodesP[membersP[rootIndex]].index )
        {
            continue;
        }

        frameTop = 0;
        b = membersP[rootIndex];
        nodesP[b].index = ++nextIndex;
        nodesP[b].lowlink = nodesP[b].index;
        nodesP[b].onStack = 1;
        stackP[top++] = b;
        framesP[frameTop].blockId = b;
        framesP[frameTop].edgeP = gP->blocksPP[b]->succP;
        ++frameTop;

        while( frameTop > 0 )
        {
            current = framesP[frameTop - 1].blockId;

            if( framesP[frameTop - 1].edgeP )
            {
                edgeP = framesP[frameTop - 1].edgeP;
                framesP[frameTop - 1].edgeP = edgeP->nextP;

                if( !considered(edgeP, setId, setOfP, blockedP) )
                {
                    continue;
                }

                target = edgeP->toP->id;

                if( !nodesP[target].index )
                {
                    nodesP[target].index = ++nextIndex;
                    nodesP[target].lowlink = nodesP[target].index;
                    nodesP[target].onStack = 1;
                    stackP[top++] = target;
                    framesP[frameTop].blockId = target;
                    framesP[frameTop].edgeP = gP->blocksPP[target]->succP;
                    ++frameTop;
                }
                else if( nodesP[target].onStack && (nodesP[target].index < nodesP[current].lowlink) )
                {
                    nodesP[current].lowlink = nodesP[target].index;
                }

                continue;
            }

            --frameTop;

            if( (frameTop > 0) && (nodesP[current].lowlink < nodesP[framesP[frameTop - 1].blockId].lowlink) )
            {
                nodesP[framesP[frameTop - 1].blockId].lowlink = nodesP[current].lowlink;
            }

            if( nodesP[current].lowlink != nodesP[current].index )
            {
                continue;
            }

            // A component closes.  Pop it.
            compCount = 0;

            do
            {
                b = stackP[--top];
                nodesP[b].onStack = 0;
                compP[compCount++] = b;
            }
            while( b != current );

            // One block is a loop only with an edge to itself that counts.
            selfEdge = 0;

            if( compCount == 1 )
            {
                for( edgeP = gP->blocksPP[current]->succP; edgeP; edgeP = edgeP->nextP )
                {
                    if( considered(edgeP, setId, setOfP, blockedP) && (edgeP->toP->id == current) )
                    {
                        selfEdge = 1;
                        break;
                    }
                }
            }

            if( (compCount > 1) || selfEdge )
            {
                newLoop(gP, setId, compP, compCount);
            }
        }
    }

    free(compP);
    lastNew = gP->loopCount;

    // Move the new loops' blocks into their sets and block their headers only
    // now: considered() must not change under the Tarjan pass using it.
    inP = (unsigned char *)gAlloc((size_t)(gP->blockCount + 1), 1);

    for( m = firstNew; m <= lastNew; ++m )
    {
        loopP = &gP->loopsP[m];

        for( b = 0; b < loopP->blockCount; ++b )
        {
            setOfP[loopP->memberIdsP[b]] = loopP->id;
            inP[loopP->memberIdsP[b]] = 1;
        }

        loopP->headerP = chooseHeader(tableP, gP, loopP, inP);
        blockedP[loopP->headerP->id] = 1;

        for( b = 0; b < loopP->blockCount; ++b )
        {
            inP[loopP->memberIdsP[b]] = 0;
        }
    }

    free(inP);
    free(membersP);
}

// Append a loop with the given blocks, sorted into id order.
// Returns the new loop's id.
static int
newLoop(OptGuessP gP, int parent, int *idsP, int count)
{
GLoop *loopP;
int i;
int j;
int t;

    if( (gP->loopCount + 1) >= gP->loopCap )
    {
        gP->loopCap = (gP->loopCap * 2);

        if( !(gP->loopsP = (GLoop *)realloc(gP->loopsP, ((size_t)gP->loopCap * sizeof(GLoop)))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer's -O2 guess\n");
            exit(1);
        }
    }

    loopP = &gP->loopsP[++gP->loopCount];
    memset(loopP, 0, sizeof(GLoop));
    loopP->id = gP->loopCount;
    loopP->parent = parent;
    loopP->depth = (parent)?(gP->loopsP[parent].depth + 1):1;
    loopP->blockCount = count;
    loopP->memberIdsP = (int *)gAlloc((size_t)count, sizeof(int));

    for( i = 0; i < count; ++i )
    {
        loopP->memberIdsP[i] = idsP[i];
    }

    // Insertion sort: components are small, and the dump wants id order.
    for( i = 1; i < count; ++i )
    {
        t = loopP->memberIdsP[i];

        for( j = i; (j > 0) && (loopP->memberIdsP[j - 1] > t); --j )
        {
            loopP->memberIdsP[j] = loopP->memberIdsP[j - 1];
        }

        loopP->memberIdsP[j] = t;
    }

    return(loopP->id);
}

// Choose a loop's header: its lowest block that is a program entry or has a
// predecessor of any kind outside the loop (inP marks the loop's blocks).
// Returns the header, never NILP: the lowest block when nothing enters.
static OptBlockP
chooseHeader(OptTableP tableP, OptGuessP gP, GLoop *loopP, unsigned char *inP)
{
OptBlockP blockP;
int m;
int p;
int b;

    (void)tableP;

    for( m = 0; m < loopP->blockCount; ++m )
    {
        b = loopP->memberIdsP[m];
        blockP = gP->blocksPP[b];

        if( blockP->isEntry )
        {
            return(blockP);
        }

        for( p = gP->predStartP[b]; p < gP->predStartP[b + 1]; ++p )
        {
            if( !inP[gP->predIdsP[p]] )
            {
                return(blockP);
            }
        }
    }

    return( gP->blocksPP[loopP->memberIdsP[0]] );
}

// Mark the routines whose body holds an in-out transfer, and those that reach
// one that does through the call graph's closure.
static void
findRoutineIots(OptTableP tableP, OptGuessP gP)
{
OptRoutineP routineP;
OptRoutineP otherP;
OptWordP firstP;
int i;

    gP->routineIotP = (unsigned char *)gAlloc((size_t)(tableP->routineCount + 1), 1);
    gP->routineIotReachP = (unsigned char *)gAlloc((size_t)(tableP->routineCount + 1), 1);

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        for( i = 0; i < routineP->blockCount; ++i )
        {
            if( blockHasIot(tableP, gP->blocksPP[routineP->bodyIdsP[i]], &firstP) )
            {
                gP->routineIotP[routineP->id] = 1;
                ++gP->loaderRoutines;
                break;
            }
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( gP->routineIotP[routineP->id] )
        {
            gP->routineIotReachP[routineP->id] = 1;
            continue;
        }

        for( otherP = tableP->routinesP; otherP; otherP = otherP->nextP )
        {
            if( gP->routineIotP[otherP->id] && optRoutineReaches(tableP, routineP, otherP) )
            {
                gP->routineIotReachP[routineP->id] = 1;
                break;
            }
        }
    }
}

// Test whether a block holds an in-out transfer to a device (deviceIot()).
// Returns 1 with the first such word in *firstPP, 0 when there is none.
static int
blockHasIot(OptTableP tableP, OptBlockP blockP, OptWordP *firstPP)
{
OptWordP wordP;
int addr;

    *firstPP = NILP;

    if( !blockP )
    {
        return(0);
    }

    for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
    {
        if( (wordP = wordAt(tableP, blockP->bank, addr)) && deviceIot(wordP) )
        {
            *firstPP = wordP;
            return(1);
        }
    }

    return(0);
}

// Test whether a word is an in-out transfer to a device, something that can be
// waited on, polled or paced.  Not device 074 (eem, lem: extend mode, common in
// any main line) nor 050-057 (the sequence-break controls).  Every other code
// counts, wait bit or not.  Returns 1 for a device transfer, 0 otherwise or NILP.
static int
deviceIot(OptWordP wordP)
{
    if( !wordP || (wordP->decode.group != OPTG_IOT) )
    {
        return(0);
    }

    if( wordP->decode.iotDevice == 074 )
    {
        return(0);
    }

    if( (wordP->decode.iotDevice >= 050) && (wordP->decode.iotDevice <= 057) )
    {
        return(0);
    }

    return(1);
}

// Measure one loop or cycle over all its blocks and classify it.  Device (H3):
// it holds a device transfer or an xct of a varying word.  Delay (H2): no call,
// no halt, no unfollowed indirect write, and it writes only its counters (words
// an isp or idx in it steps).  A loop that writes nothing, such as a spin on a
// skip, is a delay loop; readVars feeds the dump's stricter "pure" variant.
static void
measureLoop(OptTableP tableP, OptGuessP gP, GLoop *loopP)
{
OptBlockP blockP;
OptFlowEdgeP flowP;
OptWordP wordP;
OptEdgeP edgeP;
OptRoutineP routineP;
long *countersP;
long key;
int counterCap;
int m;
int c;
int addr;
int pass;
int isCounter;

    counterCap = 0;
    countersP = NILP;

    // Pass 0 finds the counters and per-block facts; pass 1 the writes and
    // reads, which need the complete counter list.
    for( pass = 0; pass < 2; ++pass )
    {
        for( m = 0; m < loopP->blockCount; ++m )
        {
            blockP = gP->blocksPP[loopP->memberIdsP[m]];

            if( pass == 0 )
            {
                loopP->wordCount += blockP->wordCount;

                if( blockP->endKind == OPTBE_HALT )
                {
                    ++loopP->haltCount;
                }

                if( blockP->endKind == OPTBE_CALL )
                {
                    ++loopP->callCount;

                    for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
                    {
                        if( flowP->kind != OPTFK_CALL )
                        {
                            continue;
                        }

                        if( !flowP->toP )
                        {
                            ++loopP->callUnknownCount;
                            break;
                        }

                        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
                        {
                            if( routineP->entryBlockP == flowP->toP )
                            {
                                if( gP->routineIotReachP[routineP->id] )
                                {
                                    ++loopP->callIotCount;
                                }

                                break;
                            }
                        }

                        break;
                    }
                }
            }

            for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
            {
                if( !(wordP = wordAt(tableP, blockP->bank, addr)) )
                {
                    continue;
                }

                if( pass == 0 )
                {
                    if( deviceIot(wordP) )
                    {
                        if( !loopP->iotCount || (wordP->index < loopP->firstIotP->index) )
                        {
                            loopP->firstIotP = wordP;
                        }

                        ++loopP->iotCount;
                    }

                    // An xct of a word that varies may execute a transfer.
                    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
                    {
                        if( (edgeP->role != OPTR_EXECUTE) || (edgeP->flags & OPTEF_PLACEHOLDER) )
                        {
                            continue;
                        }

                        if( !edgeP->toP || (edgeP->flags & OPTEF_UNKNOWN) ||
                            (edgeP->toP->flags & (OPTF_WRITTEN | OPTF_MAYBE_WRITTEN)) )
                        {
                            if( !loopP->xctVarCount || (wordP->index < loopP->firstXctP->index) )
                            {
                                loopP->firstXctP = wordP;
                            }

                            ++loopP->xctVarCount;
                            break;
                        }
                    }

                    if( (wordP->decode.group != OPTG_MEMREF) ||
                        ((wordP->decode.opcode != 044) && (wordP->decode.opcode != 046)) )
                    {
                        continue;
                    }

                    // idx or isp: every word it writes is a counter.
                    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
                    {
                        if( (edgeP->role != OPTR_WRITE) || (edgeP->flags & (OPTEF_PLACEHOLDER | OPTEF_INDIRECT)) )
                        {
                            continue;
                        }

                        key = (((long)edgeP->toBank << 12) | (long)edgeP->toAddr);

                        for( c = 0; c < loopP->counterCount; ++c )
                        {
                            if( countersP[c] == key )
                            {
                                break;
                            }
                        }

                        if( c < loopP->counterCount )
                        {
                            continue;
                        }

                        if( loopP->counterCount == counterCap )
                        {
                            counterCap = (counterCap)?(counterCap * 2):8;

                            if( !(countersP = (long *)realloc(countersP, ((size_t)counterCap * sizeof(long)))) )
                            {
                                fprintf(stderr, "am1: out of memory building the optimizer's -O2 guess\n");
                                exit(1);
                            }
                        }

                        countersP[loopP->counterCount++] = key;
                    }

                    continue;
                }

                for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
                {
                    if( edgeP->flags & OPTEF_PLACEHOLDER )
                    {
                        continue;
                    }

                    if( (edgeP->role != OPTR_WRITE) && (edgeP->role != OPTR_READ) )
                    {
                        continue;
                    }

                    if( edgeP->flags & OPTEF_INDIRECT )
                    {
                        // A followed pointer has a via edge of its own; an
                        // unfollowed write could land anywhere.  A read through
                        // a pointer reads a varying word.
                        if( (edgeP->role == OPTR_WRITE) && (edgeP->flags & OPTEF_UNKNOWN) )
                        {
                            ++loopP->unknownWrites;
                        }
                        else if( edgeP->role == OPTR_READ )
                        {
                            ++loopP->readVars;
                        }

                        continue;
                    }

                    key = (((long)edgeP->toBank << 12) | (long)edgeP->toAddr);
                    isCounter = 0;

                    for( c = 0; c < loopP->counterCount; ++c )
                    {
                        if( countersP[c] == key )
                        {
                            isCounter = 1;
                            break;
                        }
                    }

                    if( edgeP->role == OPTR_WRITE )
                    {
                        ++loopP->writeCount;

                        if( !isCounter )
                        {
                            ++loopP->otherWrites;
                        }
                    }
                    else if( !isCounter && edgeP->toP &&
                        (edgeP->toP->flags & (OPTF_WRITTEN | OPTF_MAYBE_WRITTEN)) )
                    {
                        ++loopP->readVars;
                    }
                }
            }
        }
    }

    if( countersP )
    {
        free(countersP);
    }

    if( loopP->iotCount || loopP->xctVarCount )
    {
        loopP->kind = GLC_DEVICE;
    }
    else if( !loopP->callCount && !loopP->haltCount && !loopP->unknownWrites && !loopP->otherWrites )
    {
        loopP->kind = GLC_DELAY;
    }
    else
    {
        loopP->kind = GLC_PLAIN;
    }
}

// H4: device buffers.

// Mark every address loaded as a value in a block holding a device transfer,
// or in the body of a routine that holds one: a block-transfer device is given
// its buffer's address in AC or IO, or via a control block.  Only the named
// word is marked, since the transfer's length is not in the source.
static void
findBuffers(OptTableP tableP, OptGuessP gP)
{
OptBlockP blockP;
OptRoutineP routineP;
OptWordP firstP;
OptWordP wordP;
unsigned char *loaderP;
int i;
int addr;

    loaderP = (unsigned char *)gAlloc((size_t)(gP->blockCount + 1), 1);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( blockHasIot(tableP, blockP, &firstP) )
        {
            loaderP[blockP->id] = 1;
            ++gP->loaderBlocks;
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !gP->routineIotP[routineP->id] )
        {
            continue;
        }

        for( i = 0; i < routineP->blockCount; ++i )
        {
            loaderP[routineP->bodyIdsP[i]] = 1;
        }
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !loaderP[blockP->id] )
        {
            continue;
        }

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            if( (wordP = wordAt(tableP, blockP->bank, addr)) )
            {
                loadsFrom(tableP, gP, wordP);
            }
        }
    }

    free(loaderP);
}

// Mark every address one word loads as a value: its own taken edges ("law
// buf"), and the taken edges of a constant-pool word it reads ("lio [buf]",
// "lac [scbset ctl]").
static void
loadsFrom(OptTableP tableP, OptGuessP gP, OptWordP wordP)
{
OptEdgeP edgeP;
OptEdgeP poolEdgeP;

    (void)tableP;

    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( edgeP->flags & OPTEF_PLACEHOLDER )
        {
            continue;
        }

        if( edgeP->role == OPTR_TAKEN )
        {
            markWord(gP->bufferLoaderPP, edgeP->toBank, edgeP->toAddr, wordP, &gP->bufferWords);
            continue;
        }

        if( (edgeP->role != OPTR_READ) || (edgeP->flags & OPTEF_INDIRECT) || !edgeP->toP ||
            (edgeP->toP->kind != OPTK_CONST) )
        {
            continue;
        }

        for( poolEdgeP = edgeP->toP->outP; poolEdgeP; poolEdgeP = poolEdgeP->nextOutP )
        {
            if( (poolEdgeP->role == OPTR_TAKEN) && !(poolEdgeP->flags & OPTEF_PLACEHOLDER) )
            {
                markWord(gP->bufferLoaderPP, poolEdgeP->toBank, poolEdgeP->toAddr, wordP, &gP->bufferWords);
            }
        }
    }
}

// H5: absolute addresses.

// Mark every memory reference whose address field names a number and no
// symbol, not even '.', and the word it names.  A #define that expands to a
// number counts: that is how a mailbox shared with another tape is spelled.
// Patched fields ("jmp 0") and words with no mnemonic are skipped; cal names
// no word, since it goes to 100 whatever its field says.
static void
findAbsolutes(OptTableP tableP, OptGuessP gP)
{
OptWordP wordP;
int i;
int number;

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( !inGraph(wordP) || (wordP->decode.group != OPTG_MEMREF) || (wordP->flags & OPTF_PATCHED) )
        {
            continue;
        }

        if( (wordP->decode.opSymCount < 1) || !wordP->exprP )
        {
            continue;
        }

        number = 0;

        if( !numericField(wordP->exprP, &number) || !number )
        {
            continue;
        }

        ++gP->absoluteRefs;
        markWord(gP->absoluteRefPP, wordP->bank, wordP->addr, wordP, &gP->absoluteWords);

        if( (wordP->decode.opcode == 016) && !wordP->decode.indirect )
        {
            continue;
        }

        markWord(gP->absoluteRefPP, wordP->bank, wordP->decode.address, wordP, &gP->absoluteWords);
    }
}

// Test whether an expression names no symbol; a number seen sets *numberP.
// Returns 1 when no address symbol, pool constant or '.' appears in it, 0 when
// one does.
static int
numericField(PNodeP nodeP, int *numberP)
{
    if( !nodeP )
    {
        return(1);
    }

    switch( nodeP->type )
    {
    case ADDR:
    case LCLADDR:
    case BREF:
    case WILDREF:
    case CONSTANT:
    case DOT:
        return(0);

    case INTEGER:
        *numberP = 1;
        return(1);

    default:
        break;
    }

    return( numericField(nodeP->leftP, numberP) && numericField(nodeP->rightP, numberP) );
}

// H6: computed addresses.

// Find every word that loads an address T as a value, follow the value
// through the rest of its block, and mark T's run when the block changes the
// value before it is used (an add or sub of anything but an unwritten zero),
// or stores it where an idx or isp walks it.  A run of data alone is not
// marked: no rewrite changes the length of data.
static void
findComputed(OptTableP tableP, OptGuessP gP)
{
OptWordP wordP;
OptWordP tP;
OptWordP storeP;
GComputedMod mod;
int offset;
int isAC;
int i;

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( !inGraph(wordP) || (wordP->flags & OPTF_DUPADDR) || !(tP = addressLoaded(wordP)) )
        {
            continue;
        }

        ++gP->addressSites;
        offset = 0;
        storeP = NILP;
        isAC = ((wordP->decode.group == OPTG_LAW) || strcmp(wordP->decode.mnemonicP, "lio"));

        // An add or sub of the address: whatever AC held is the offset.
        if( (wordP->decode.group == OPTG_MEMREF) &&
            (!strcmp(wordP->decode.mnemonicP, "add") || !strcmp(wordP->decode.mnemonicP, "sub")) )
        {
            mod = GCM_VARIABLE;
            (void)followAddress(tableP, wordP, isAC, &offset, &storeP);
        }
        else
        {
            mod = followAddress(tableP, wordP, isAC, &offset, &storeP);
        }

        if( (mod == GCM_VARIABLE) || ((mod == GCM_CONSTANT) && offset) || storeWalked(storeP) )
        {
            ++gP->computedSites;
            markRun(tableP, gP, wordP, tP);
        }
    }
}

// Test whether a word loads an address as a value: "law T", or lac, lio, add
// or sub of an unwritten word, a pool word included, that holds T's address.
// Returns T, or NILP when the word loads none.
static OptWordP
addressLoaded(OptWordP wordP)
{
OptEdgeP edgeP;
const char *mnP;

    if( (wordP->decode.group == OPTG_LAW) && !wordP->decode.indirect )
    {
        edgeP = directEdge(wordP, OPTR_TAKEN);
        return( (edgeP)?edgeP->toP:NILP );
    }

    if( (wordP->decode.group != OPTG_MEMREF) || wordP->decode.memIndirect || !(mnP = wordP->decode.mnemonicP) )
    {
        return(NILP);
    }

    if( strcmp(mnP, "lac") && strcmp(mnP, "lio") && strcmp(mnP, "add") && strcmp(mnP, "sub") )
    {
        return(NILP);
    }

    edgeP = directEdge(wordP, OPTR_READ);
    return( (edgeP)?addressHeld(edgeP->toP):NILP );
}

// Follow an address loaded into AC (isAC) or IO through the rest of the
// site's block, summing the unwritten constants added to it into *offsetP,
// until it is stored (*storePP gets the word stored into, when the reference
// names one), replaced, or the block ends.  Returns what was done to it.
static GComputedMod
followAddress(OptTableP tableP, OptWordP siteP, int isAC, int *offsetP, OptWordP *storePP)
{
OptWordP wordP;
OptEdgeP edgeP;
GComputedMod mod;
const char *mnP;
int end;
int addr;
int value;

    mod = GCM_NONE;
    end = (siteP->blockP)?siteP->blockP->endAddr:siteP->addr;

    for( addr = (siteP->addr + 1); addr <= end; ++addr )
    {
        if( !(wordP = wordAt(tableP, siteP->bank, addr)) )
        {
            return(mod);
        }

        if( wordP->decode.group == OPTG_SKIP )
        {
            continue;
        }

        // An operate word that changes no register is passed; any other
        // replaces or changes the value, and the analysis stops there.
        if( wordP->decode.group == OPTG_OPERATE )
        {
            if( wordP->decode.microBits & ~(unsigned int)(OPTM_FLAGFIELD) )
            {
                return(mod);
            }

            continue;
        }

        if( (wordP->decode.group != OPTG_MEMREF) || !(mnP = wordP->decode.mnemonicP) )
        {
            return(mod);
        }

        if( isAC && (!strcmp(mnP, "add") || !strcmp(mnP, "sub")) )
        {
            edgeP = directEdge(wordP, OPTR_READ);

            if( !wordP->decode.memIndirect && edgeP && unwrittenWord(edgeP->toP) && !addressHeld(edgeP->toP) )
            {
                value = (edgeP->toP->value & WRDMASK);
                value = (value & 0400000)?(-((~value) & WRDMASK)):value;
                *offsetP += (mnP[0] == 'a')?value:-value;

                if( mod == GCM_NONE )
                {
                    mod = GCM_CONSTANT;
                }
            }
            else
            {
                mod = GCM_VARIABLE;
            }

            continue;
        }

        // A compare reads the value and leaves it.
        if( !strcmp(mnP, "sad") || !strcmp(mnP, "sas") )
        {
            continue;
        }

        if( (isAC && (!strcmp(mnP, "dac") || !strcmp(mnP, "dap"))) || (!isAC && !strcmp(mnP, "dio")) )
        {
            if( !wordP->decode.memIndirect && (edgeP = directEdge(wordP, OPTR_WRITE)) )
            {
                *storePP = edgeP->toP;
            }

            return(mod);
        }

        // A store of the other register, or of zero, leaves the value alone.
        if( !strcmp(mnP, "dzm") || !strcmp(mnP, "dip") || (isAC && !strcmp(mnP, "dio")) ||
            (!isAC && (!strcmp(mnP, "dac") || !strcmp(mnP, "dap"))) )
        {
            continue;
        }

        return(mod);
    }

    return(mod);
}

// Find a word's first direct reference of a role that names a word.
// Returns the edge, or NILP when there is none.
static OptEdgeP
directEdge(OptWordP wordP, OptRole role)
{
OptEdgeP edgeP;

    for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( (edgeP->role == role) && edgeP->toP && !(edgeP->flags & (OPTEF_INDIRECT | OPTEF_VIAPOINTER)) )
        {
            return(edgeP);
        }
    }

    return(NILP);
}

// Test whether nothing writes a word.  Returns 1 if nothing does, 0 if
// something does or may.
static int
unwrittenWord(OptWordP wordP)
{
    return( !(wordP->flags & (OPTF_WRITTEN | OPTF_PATCHED | OPTF_MAYBE_WRITTEN)) );
}

// Report the address an unwritten word holds, through its taken edge.
// Returns the word at that address, or NILP when the word is written or
// holds no address.
static OptWordP
addressHeld(OptWordP wordP)
{
OptEdgeP edgeP;

    if( !wordP || !unwrittenWord(wordP) )
    {
        return(NILP);
    }

    edgeP = directEdge(wordP, OPTR_TAKEN);
    return( (edgeP)?edgeP->toP:NILP );
}

// Test whether an idx or isp writes a word: a pointer walked through a table.
// Returns 1 if one does, 0 if none does or storeP is NILP.
static int
storeWalked(OptWordP storeP)
{
OptEdgeP edgeP;
OptWordP fromP;

    for( edgeP = (storeP)?storeP->inP:NILP; edgeP; edgeP = edgeP->nextInP )
    {
        fromP = edgeP->fromP;

        if( (edgeP->role == OPTR_WRITE) && (fromP->decode.group == OPTG_MEMREF) && fromP->decode.mnemonicP &&
            (!strcmp(fromP->decode.mnemonicP, "idx") || !strcmp(fromP->decode.mnemonicP, "isp")) )
        {
            return(1);
        }
    }

    return(0);
}

// Mark the run from T: T and each word after it up to a gap, a pool word, or
// the next word that is labeled, a jump target or taken, at most
// GUESS_MAXRUN words.  Nothing is marked unless the run holds code.
static void
markRun(OptTableP tableP, OptGuessP gP, OptWordP siteP, OptWordP tP)
{
OptWordP wordP;
int length;
int code;
int n;

    code = 0;

    for( length = 0; length < GUESS_MAXRUN; ++length )
    {
        wordP = wordAt(tableP, tP->bank, tP->addr + length);

        if( !wordP || (wordP->kind == OPTK_CONST) ||
            ((length > 0) && (wordP->flags & (OPTF_HASLABEL | OPTF_JUMPTARGET | OPTF_TAKEN))) )
        {
            break;
        }

        code += inGraph(wordP);
    }

    if( !code )
    {
        return;
    }

    for( n = 0; n < length; ++n )
    {
        markWord(gP->computedSitePP, tP->bank, tP->addr + n, siteP, &gP->computedWords);
    }

    if( gP->runCount == gP->runCap )
    {
        gP->runCap = (gP->runCap)?(gP->runCap * 2):16;

        if( !(gP->runsP = (GRun *)realloc(gP->runsP, sizeof(GRun) * (size_t)gP->runCap)) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer's -O2 guess\n");
            exit(1);
        }
    }

    gP->runsP[gP->runCount].siteP = siteP;
    gP->runsP[gP->runCount].tP = tP;
    gP->runsP[gP->runCount].length = length;
    ++gP->runCount;
}

// The findings.

// Test whether -O2 carries a rule first: the layout-neutral rules -O1 rewrites.
// Returns 1 for T3 and T8a to T8d, 0 otherwise.
static int
ruleCarried(OptRuleId rule)
{
    switch( rule )
    {
    case OPTRULE_T3:
    case OPTRULE_T8A:
    case OPTRULE_T8B:
    case OPTRULE_T8C:
    case OPTRULE_T8D:
        return(1);

    default:
        return(0);
    }
}

// Collect a finding's footprint, no word twice (see the file header).
// Returns how many were stored in wordsPP, at most GUESS_MAXFOOT.
static int
footprint(OptTableP tableP, OptFindingP findingP, OptWordP *wordsPP)
{
OptWordP wordP;
OptWordP throughP;
int count;
int i;
int j;

    count = 0;

    for( i = 0; (i < findingP->wordCount) && (count < GUESS_MAXFOOT); ++i )
    {
        wordsPP[count++] = findingP->wordsP[i];
    }

    if( !ruleCarried(findingP->rule) && (findingP->rule != OPTRULE_T14) )
    {
        return(count);
    }

    for( i = 0; i < findingP->wordCount; ++i )
    {
        wordP = findingP->wordsP[i];

        if( (wordP->decode.group != OPTG_MEMREF) || wordP->decode.memIndirect || (wordP->decode.opcode == 016) )
        {
            continue;
        }

        if( !(throughP = wordAt(tableP, wordP->bank, wordP->decode.address)) )
        {
            continue;
        }

        for( j = 0; j < count; ++j )
        {
            if( wordsPP[j] == throughP )
            {
                break;
            }
        }

        if( (j == count) && (count < GUESS_MAXFOOT) )
        {
            wordsPP[count++] = throughP;
        }
    }

    return(count);
}

// Ask each heuristic about each word of a live finding's footprint, record the
// first word each one marks, and add the finding to the tallies.
static void
judgeFinding(OptTableP tableP, OptGuessP gP, OptFindingP findingP)
{
OptWordP footP[GUESS_MAXFOOT];
OptWordP wordP;
int count;
int i;
int h;
int loop;
int hit;
int pure;
int calls;
int literal2;
int literal3;
unsigned int bits;

    count = footprint(tableP, findingP, footP);
    findingP->guessBits = 0;

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        findingP->guessWordP[h] = NILP;
    }

    for( i = 0; i < count; ++i )
    {
        wordP = footP[i];
        bits = optGuessWordBits(tableP, wordP);

        // H6 refuses only a rewrite that changes the program's length.
        if( !optRuleDeletes(findingP->rule) )
        {
            bits &= ~OPTGH_LENGTH;
        }

        for( h = 0; h < OPTGH_COUNT; ++h )
        {
            hit = ((bits & (1u << h)) != 0);

            if( hit && !(findingP->guessBits & (1u << h)) )
            {
                findingP->guessBits |= (1u << h);
                findingP->guessWordP[h] = wordP;
            }
        }
    }

    // The literal H2 and H3: whole loops instead of cycles.
    literal2 = 0;
    literal3 = 0;

    for( i = 0; i < count; ++i )
    {
        if( footP[i]->blockP )
        {
            literal2 |= (gP->delayLoopP[footP[i]->blockP->id] != 0);
            literal3 |= (gP->deviceLoopP[footP[i]->blockP->id] != 0);
        }
    }

    gP->literalH2 += literal2;
    gP->literalH3 += literal3;

    if( literal2 || literal3 || (findingP->guessBits & ~((1u << OPTGH_H2) | (1u << OPTGH_H3))) )
    {
        ++gP->literalAny;
    }

    ++gP->live;
    ++gP->ruleLive[findingP->rule];

    if( ruleCarried(findingP->rule) )
    {
        ++gP->carriedLive;
    }

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        if( findingP->guessBits & (1u << h) )
        {
            ++gP->refused[h];

            if( ruleCarried(findingP->rule) )
            {
                ++gP->carriedRefused[h];
            }
        }
    }

    if( findingP->guessBits )
    {
        ++gP->refusedAny;
        ++gP->ruleAny[findingP->rule];

        if( ruleCarried(findingP->rule) )
        {
            ++gP->carriedAny;
        }
    }

    // The pure-delay variant: the H2 refusal stands if some footprint word's
    // delay cycle reads no word that something writes.
    if( findingP->guessBits & (1u << OPTGH_H2) )
    {
        pure = 0;

        for( i = 0; i < count; ++i )
        {
            if( footP[i]->blockP && (loop = gP->delayCycleP[footP[i]->blockP->id]) &&
                !gP->loopsP[loop].readVars )
            {
                pure = 1;
            }
        }

        gP->variantPure += pure;
    }

    // The leaf-device variant: the H3 refusal stands if some footprint word
    // lies in a device cycle that makes no call.
    if( findingP->guessBits & (1u << OPTGH_H3) )
    {
        pure = 0;

        for( i = 0; i < count; ++i )
        {
            if( footP[i]->blockP && gP->leafCycleP[footP[i]->blockP->id] )
            {
                pure = 1;
            }
        }

        gP->variantLeaf += pure;
    }

    // The calls variant: a finding nothing refuses, in a loop that calls a
    // routine holding an in-out transfer.
    if( !findingP->guessBits )
    {
        calls = 0;

        for( i = 0; (i < count) && !calls; ++i )
        {
            if( !footP[i]->blockP )
            {
                continue;
            }

            for( loop = gP->innerLoopP[footP[i]->blockP->id]; loop; loop = gP->loopsP[loop].parent )
            {
                if( gP->loopsP[loop].callIotCount )
                {
                    calls = 1;
                    break;
                }
            }
        }

        gP->variantCalls += calls;
    }
}

// Report which heuristics mark one word.  Shared by judgeFinding() and
// -O=speed so the two cannot disagree.  Returns bits (1 << OptGuessId), 0 when
// none marks it, when wordP is NILP, or when the guess has not been built.
unsigned int
optGuessWordBits(OptTableP tableP, OptWordP wordP)
{
OptGuessP gP;
unsigned int bits;

    if( !tableP || !(gP = tableP->guessP) || !wordP )
    {
        return(0);
    }

    bits = 0;

    if( (wordP->blockP && gP->handlerEntryPP[wordP->blockP->id]) ||
        wordMark(gP->handlerWriterPP, wordP->bank, wordP->addr) )
    {
        bits |= (1u << OPTGH_H1);
    }

    if( wordP->blockP && gP->delayCycleP[wordP->blockP->id] )
    {
        bits |= (1u << OPTGH_H2);
    }

    if( wordP->blockP && gP->deviceCycleP[wordP->blockP->id] )
    {
        bits |= (1u << OPTGH_H3);
    }

    if( wordMark(gP->bufferLoaderPP, wordP->bank, wordP->addr) != NILP )
    {
        bits |= (1u << OPTGH_H4);
    }

    if( wordMark(gP->absoluteRefPP, wordP->bank, wordP->addr) != NILP )
    {
        bits |= (1u << OPTGH_H5);
    }

    if( wordMark(gP->computedSitePP, wordP->bank, wordP->addr) != NILP )
    {
        bits |= (1u << OPTGH_H6);
    }

    return(bits);
}

// Write into bufP the sentence saying why one heuristic refuses a finding.
static void
describeGuess(OptTableP tableP, OptFindingP findingP, int h, char *bufP, int size)
{
OptGuessP gP;
OptWordP wordP;
OptWordP byP;
GLoop *loopP;

    gP = tableP->guessP;
    wordP = findingP->guessWordP[h];

    if( !gP || !wordP )
    {
        snprintf(bufP, (size_t)size, "-");
        return;
    }

    switch( h )
    {
    case OPTGH_H1:
        if( wordP->blockP && gP->handlerEntryPP[wordP->blockP->id] )
        {
            snprintf(bufP, (size_t)size, "%04o is reached from the break entry at %04o",
                wordP->addr, gP->handlerEntryPP[wordP->blockP->id]->addr);
        }
        else if( (wordP->bank == 0) && (wordP->addr < (gP->frames * OPTSBS_FRAMESIZE)) &&
            ((wordP->addr % OPTSBS_FRAMESIZE) != OPTSBS_ENTRYSLOT) )
        {
            snprintf(bufP, (size_t)size, "%04o is a break frame word the hardware stores into", wordP->addr);
        }
        else
        {
            byP = wordMark(gP->handlerWriterPP, wordP->bank, wordP->addr);
            snprintf(bufP, (size_t)size, "%04o is written by the handler code at %04o", wordP->addr,
                (byP)?byP->addr:0);
        }
        break;

    case OPTGH_H2:
        loopP = &gP->loopsP[gP->delayCycleP[wordP->blockP->id]];
        snprintf(bufP, (size_t)size, "%04o is in the delay loop from %04o back to %04o", wordP->addr,
            loopP->latchP->endAddr, loopP->headerP->startAddr);
        break;

    case OPTGH_H3:
        loopP = &gP->loopsP[gP->deviceCycleP[wordP->blockP->id]];

        if( loopP->firstIotP )
        {
            snprintf(bufP, (size_t)size, "%04o is in the loop from %04o back to %04o, which holds the in-out"
                " transfer at %04o", wordP->addr, loopP->latchP->endAddr, loopP->headerP->startAddr,
                loopP->firstIotP->addr);
        }
        else
        {
            snprintf(bufP, (size_t)size, "%04o is in the loop from %04o back to %04o, which executes a word"
                " the program writes at %04o", wordP->addr, loopP->latchP->endAddr, loopP->headerP->startAddr,
                loopP->firstXctP->addr);
        }
        break;

    case OPTGH_H4:
        byP = wordMark(gP->bufferLoaderPP, wordP->bank, wordP->addr);
        snprintf(bufP, (size_t)size, "the address %04o is loaded at %04o, where an in-out transfer runs",
            wordP->addr, (byP)?byP->addr:0);
        break;

    case OPTGH_H6:
        byP = wordMark(gP->computedSitePP, wordP->bank, wordP->addr);
        snprintf(bufP, (size_t)size, "%04o is in a run the address loaded at %04o indexes, changed at run time",
            wordP->addr, (byP)?byP->addr:0);
        break;

    case OPTGH_H5:
    default:
        byP = wordMark(gP->absoluteRefPP, wordP->bank, wordP->addr);

        if( byP == wordP )
        {
            snprintf(bufP, (size_t)size, "%04o names its address as a number", wordP->addr);
        }
        else
        {
            snprintf(bufP, (size_t)size, "%04o is named by a number at %04o", wordP->addr, (byP)?byP->addr:0);
        }
        break;
    }
}

// The report, the dump, and the rest of the interface.

// Name a heuristic.
// Returns "H1" to "H6", or "?" out of range.
const char *
optGuessName(OptGuessId h)
{
    if( ((int)h < 0) || (h >= OPTGH_COUNT) )
    {
        return("?");
    }

    return( guessTable[h].tagP );
}

// Name what an -O2 rewrite assumes when a heuristic found nothing, for the
// Transforms section.  Returns a static string, "?" out of range.
const char *
optGuessAssumption(OptGuessId h)
{
static const char *assumptionsPP[OPTGH_COUNT] =
{
    "no sequence-break handler reaches or writes it",
    "not in a delay loop",
    "not in a loop that transfers to a device",
    "not a device buffer",
    "no address involved is written as a number",
    "no address changed at run time indexes the words it moves"
};

    if( ((int)h < 0) || (h >= OPTGH_COUNT) )
    {
        return("?");
    }

    return( assumptionsPP[h] );
}

// Report the heuristics that apply to a finding under -O2.  Inside an optimize
// region the author's declaration stands in for H2 to H5, but a handler cannot
// be declared away, so H1 still applies.  H6 applies to a deleting rule
// everywhere, and to no other.  Returns bits (1 << OptGuessId).
unsigned int
optGuessApplies(OptFindingP findingP)
{
unsigned int applies;

    applies = (findingP->place == OPTREG_INSIDE)?(OPTGH_INREGION & OPTGH_AUTHORIZED):OPTGH_AUTHORIZED;

    if( optRuleDeletes(findingP->rule) )
    {
        applies |= OPTGH_LENGTH;
    }

    return(applies);
}

// Write the report's "What -O2 would assume" section: each heuristic, how many
// live findings it would refuse, and one line per refused finding saying why.
void
writeGuessReport(FILE *fP, OptTableP tableP)
{
OptGuessP gP;
OptFindingP findingP;
int h;
int first;
char why[OPTMSG_SIZE];

    if( !(gP = tableP->guessP) )
    {
        return;
    }

    fprintf(fP, "\nWhat -O2 would assume\n");

    if( optTransformLevel() >= 2 )
    {
        fprintf(fP, "  In place of P5, -O2 guessed where code is shared with a device, a handler or\n");
        fprintf(fP, "  a timing requirement, and left the findings there alone.  Inside an optimize\n");
        fprintf(fP, "  region only H1 applied.  The Transforms section below says what it did with\n");
        fprintf(fP, "  each finding; the counts here are the guess alone.\n");
    }
    else
    {
        fprintf(fP, "  In place of P5, -O2 guesses where code is shared with a device, a handler or\n");
        fprintf(fP, "  a timing requirement, and leaves the findings there alone.  This measures\n");
        fprintf(fP, "  that guess on this program.  It refuses nothing above and changes no word.\n");
    }

    fprintf(fP, "  H6 is asked only of a rewrite that changes the program's length, and at\n");
    fprintf(fP, "  every level, in a region or not.\n");

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        fprintf(fP, "    %s %s\n", guessTable[h].tagP, guessTable[h].textP);
    }

    if( !gP->live )
    {
        fprintf(fP, "  There is no finding for it to refuse.\n");
        return;
    }

    fprintf(fP, "  Of the %d finding%s, the heuristics would refuse:", gP->live, (gP->live == 1)?"":"s");

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        fprintf(fP, " %s %d", guessTable[h].tagP, gP->refused[h]);
    }

    fprintf(fP, "; %d in all.\n", gP->refusedAny);
    fprintf(fP, "  Of the %d T3 and T8 finding%s, the rules -O2 would carry first, %d.\n",
        gP->carriedLive, (gP->carriedLive == 1)?"":"s", gP->carriedAny);

    if( !gP->refusedAny )
    {
        return;
    }

    for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        if( !findingP->guessBits )
        {
            continue;
        }

        first = 1;

        for( h = 0; h < OPTGH_COUNT; ++h )
        {
            if( !(findingP->guessBits & (1u << h)) )
            {
                continue;
            }

            describeGuess(tableP, findingP, h, why, sizeof(why));

            if( first )
            {
                fprintf(fP, "    bank %2d %04o  %-4s %s: %s\n", findingP->bank, findingP->addr,
                    optRuleName(findingP->rule), guessTable[h].tagP, why);
                first = 0;
            }
            else
            {
                fprintf(fP, "                     %s: %s\n", guessTable[h].tagP, why);
            }
        }
    }
}

// Print the guess dump (-O=guess).  Every line begins "guess", then:
//   frames F entries E handler-blocks B handler-written W handler-indirect U
//   handler-block BBAAAA-AAAA entry BBAAAA
//   handler-written BBAAAA by BBAAAA
//   loops N delay D device V cycles C delay D device V
//   loop ID parent P depth D header BBAAAA blocks N words N iot N xct-var X calls N
//        calls-iot N calls-unknown N halts N writes N counters N
//        other-writes N unknown-writes N read-vars N class C [first-iot BBAAAA]
//        [first-xct BBAAAA]
//   loop-block ID BBAAAA-AAAA
//   cycle ID loop L header BBAAAA latch BBAAAA blocks N ..., as a loop line
//   cycle-block ID BBAAAA-AAAA
//   iot-routines N loader-blocks L buffers W untaken T absolute-refs R absolute-words A
//   buffer BBAAAA by BBAAAA
//   absolute BBAAAA by BBAAAA
//   computed address-sites N computed-sites C runs R words W
//   computed-run BBAAAA-AAAA by BBAAAA
//   finding RULE BBAAAA footprint BBAAAA,... refused H1,H3 | refused -
//   tally live N H1 a H2 b H3 c H4 d H5 e any f
//   carried live N H1 a H2 b H3 c H4 d H5 e any f
//   rule RULE live N refused M
//   variant calls-iot N pure-delay M leaf-device L
//   literal H2 a H3 b any f
// A loop line is printed on one line; it is wrapped above for reading only.
void
optDumpGuess(FILE *fP, OptTableP tableP)
{
OptGuessP gP;
OptBlockP blockP;
OptFindingP findingP;
OptWordP footP[GUESS_MAXFOOT];
OptWordP byP;
OptWordP wordP;
GLoop *loopP;
int i;
int m;
int h;
int bank;
int addr;
int count;
int any;
static const char *classNames[3] = { "plain", "delay", "device" };

    if( !(gP = tableP->guessP) )
    {
        return;
    }

    fprintf(fP, "guess frames %d entries %d handler-blocks %d handler-written %d handler-indirect %d\n",
        gP->frames, gP->handlerEntries, gP->handlerBlocks, gP->handlerWritten, gP->handlerIndirect);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( gP->handlerEntryPP[blockP->id] )
        {
            fprintf(fP, "guess handler-block %02o%04o-%04o entry %02o%04o\n", blockP->bank, blockP->startAddr,
                blockP->endAddr, gP->handlerEntryPP[blockP->id]->bank, gP->handlerEntryPP[blockP->id]->addr);
        }
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            if( (byP = wordMark(gP->handlerWriterPP, bank, addr)) )
            {
                fprintf(fP, "guess handler-written %02o%04o by %02o%04o\n", bank, addr, byP->bank, byP->addr);
            }
        }
    }

    fprintf(fP, "guess loops %d delay %d device %d cycles %d delay %d device %d\n", gP->forestCount,
        gP->delayLoops, gP->deviceLoops, (gP->loopCount - gP->forestCount), gP->delayCycles, gP->deviceCycles);

    for( i = 1; i <= gP->loopCount; ++i )
    {
        loopP = &gP->loopsP[i];

        if( loopP->cycleOf )
        {
            fprintf(fP, "guess cycle %d loop %d header %02o%04o latch %02o%04o", loopP->id, loopP->cycleOf,
                loopP->headerP->bank, loopP->headerP->startAddr, loopP->latchP->bank, loopP->latchP->startAddr);
        }
        else
        {
            fprintf(fP, "guess loop %d parent %d depth %d header %02o%04o", loopP->id, loopP->parent,
                loopP->depth, loopP->headerP->bank, loopP->headerP->startAddr);
        }

        fprintf(fP, " blocks %d words %d iot %d xct-var %d calls %d"
            " calls-iot %d calls-unknown %d halts %d writes %d counters %d other-writes %d unknown-writes %d"
            " read-vars %d class %s",
            loopP->blockCount, loopP->wordCount, loopP->iotCount, loopP->xctVarCount, loopP->callCount,
            loopP->callIotCount,
            loopP->callUnknownCount, loopP->haltCount, loopP->writeCount, loopP->counterCount,
            loopP->otherWrites, loopP->unknownWrites, loopP->readVars, classNames[loopP->kind]);

        if( loopP->firstIotP )
        {
            fprintf(fP, " first-iot %02o%04o", loopP->firstIotP->bank, loopP->firstIotP->addr);
        }

        if( loopP->firstXctP )
        {
            fprintf(fP, " first-xct %02o%04o", loopP->firstXctP->bank, loopP->firstXctP->addr);
        }

        fprintf(fP, "\n");

        for( m = 0; m < loopP->blockCount; ++m )
        {
            blockP = gP->blocksPP[loopP->memberIdsP[m]];
            fprintf(fP, "guess %s-block %d %02o%04o-%04o\n", (loopP->cycleOf)?"cycle":"loop", loopP->id,
                blockP->bank, blockP->startAddr, blockP->endAddr);
        }
    }

    // Count buffers P2 does not know as taken.  Every mark comes from a taken
    // edge, so this should be zero.
    count = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            if( wordMark(gP->bufferLoaderPP, bank, addr) && (wordP = wordAt(tableP, bank, addr)) &&
                !(wordP->flags & OPTF_TAKEN) )
            {
                ++count;
            }
        }
    }

    fprintf(fP, "guess iot-routines %d loader-blocks %d buffers %d untaken %d absolute-refs %d absolute-words %d\n",
        gP->loaderRoutines, gP->loaderBlocks, gP->bufferWords, count, gP->absoluteRefs, gP->absoluteWords);

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            if( (byP = wordMark(gP->bufferLoaderPP, bank, addr)) )
            {
                fprintf(fP, "guess buffer %02o%04o by %02o%04o\n", bank, addr, byP->bank, byP->addr);
            }
        }
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            if( (byP = wordMark(gP->absoluteRefPP, bank, addr)) )
            {
                fprintf(fP, "guess absolute %02o%04o by %02o%04o\n", bank, addr, byP->bank, byP->addr);
            }
        }
    }

    fprintf(fP, "guess computed address-sites %d computed-sites %d runs %d words %d\n", gP->addressSites,
        gP->computedSites, gP->runCount, gP->computedWords);

    for( i = 0; i < gP->runCount; ++i )
    {
        fprintf(fP, "guess computed-run %02o%04o-%04o by %02o%04o\n", gP->runsP[i].tP->bank,
            gP->runsP[i].tP->addr, gP->runsP[i].tP->addr + gP->runsP[i].length - 1,
            gP->runsP[i].siteP->bank, gP->runsP[i].siteP->addr);
    }

    for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        count = footprint(tableP, findingP, footP);
        fprintf(fP, "guess finding %s %02o%04o footprint", optRuleName(findingP->rule), findingP->bank,
            findingP->addr);

        for( i = 0; i < count; ++i )
        {
            fprintf(fP, "%s%02o%04o", (i)?",":" ", footP[i]->bank, footP[i]->addr);
        }

        fprintf(fP, " refused");
        any = 0;

        for( h = 0; h < OPTGH_COUNT; ++h )
        {
            if( findingP->guessBits & (1u << h) )
            {
                fprintf(fP, "%s%s", (any)?",":" ", guessTable[h].tagP);
                any = 1;
            }
        }

        fprintf(fP, "%s\n", (any)?"":" -");
    }

    fprintf(fP, "guess tally live %d", gP->live);

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        fprintf(fP, " %s %d", guessTable[h].tagP, gP->refused[h]);
    }

    fprintf(fP, " any %d\n", gP->refusedAny);
    fprintf(fP, "guess carried live %d", gP->carriedLive);

    for( h = 0; h < OPTGH_COUNT; ++h )
    {
        fprintf(fP, " %s %d", guessTable[h].tagP, gP->carriedRefused[h]);
    }

    fprintf(fP, " any %d\n", gP->carriedAny);

    for( i = 0; i < OPTRULE_COUNT; ++i )
    {
        if( gP->ruleLive[i] )
        {
            fprintf(fP, "guess rule %s live %d refused %d\n", optRuleName((OptRuleId)i), gP->ruleLive[i],
                gP->ruleAny[i]);
        }
    }

    fprintf(fP, "guess variant calls-iot %d pure-delay %d leaf-device %d\n", gP->variantCalls, gP->variantPure,
        gP->variantLeaf);
    fprintf(fP, "guess literal H2 %d H3 %d any %d\n", gP->literalH2, gP->literalH3, gP->literalAny);
}

// Release the side table and clear the table's pointer; safe when never built
// and safe twice.  The findings' guess fields point at table words and stay.
void
freeGuess(OptTableP tableP)
{
OptGuessP gP;
int i;

    if( !tableP || !(gP = tableP->guessP) )
    {
        return;
    }

    for( i = 1; i <= gP->loopCount; ++i )
    {
        free(gP->loopsP[i].memberIdsP);
    }

    for( i = 0; i <= MAXBANK; ++i )
    {
        free(gP->handlerWriterPP[i]);
        free(gP->bufferLoaderPP[i]);
        free(gP->absoluteRefPP[i]);
        free(gP->computedSitePP[i]);
    }

    free(gP->runsP);
    free(gP->blocksPP);
    free(gP->handlerEntryPP);
    free(gP->loopsP);
    free(gP->innerLoopP);
    free(gP->delayLoopP);
    free(gP->deviceLoopP);
    free(gP->delayCycleP);
    free(gP->deviceCycleP);
    free(gP->leafCycleP);
    free(gP->predStartP);
    free(gP->predIdsP);
    free(gP->routineIotP);
    free(gP->routineIotReachP);
    free(gP);
    tableP->guessP = NILP;
}
