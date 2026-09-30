/*
 * The am1 optimizer's scratch-pool measurement: how many of the storage
 * locals a program declares could share locations.  It names the population,
 * puts every word of it in exactly one class, bounds what sharing could
 * recover, and prints the -O=scratch dump and the .opt report's scratch-word
 * sharing section.
 *
 * Both outputs are advisory, never a finding and never acted on by any level:
 * making two words one changes an operand, and the space is only usable once
 * the author repacks the bank or relayout does.  It reads the word table and
 * the analyses built on it (optimizer.h), writes none of them and never
 * touches the parse tree.  optDumpScratch() and writeScratchSharingReport()
 * each run buildScratchPass() and free what it allocated, so the two agree.
 *
 * The population is every word with a local label, less the routines' return
 * words, text words and words classified as code.  A UNIT is what a word's
 * references are private to: a routine, joined to another when they share a
 * block or a plain (non-call) flow edge runs between them.  Blocks in no
 * routine form three pseudo-routines: the main line, the handler line, both.
 * A unit is in the main pool (reached from every entry but the handler slots)
 * or the handler pool (bank 0, 4n+3); a handler runs between any two words of
 * what it interrupts, so the pools are measured separately.
 *
 * The classes, in the order decided (the dump's numbers in parentheses):
 *   taken(3)  taken, patched, xct'd, jumped to, reached through a pointer, an
 *             inline argument a proved call steps past, or above a taken
 *             plain word in its data run (adjacentTaken())
 *   unplaced  referenced from outside the graph or unreached code, an
 *             overlaid address, or the word after a call whose return is
 *             undecided (the callee may read it as an argument)
 *   unused    no reference at all
 *   multi(4)  referenced from more than one unit (by routine: multiByRoutine())
 *   open(5)   the unit holds a routine flagged open, jumpsunknown or
 *             callsunknown, or calls or jumps indirectly into the sink
 *   both      the unit runs in both pools
 *   dead      never read
 *   sink      the class below depends on where a residual sink edge goes
 *   state     live on entry to the unit: it carries a value between calls
 *   call(2)   live across a call the unit makes
 *   local(1)  every reference in one block, the first a full write (dac, dio,
 *             dzm; not idx or isp), no xct between first and last; "once"
 *             is the subset with exactly one write and one read
 *   span      the rest: private, never live across a call or into the unit
 *
 * LIVENESS is per word, backward over its unit's blocks along every edge but
 * a call.  A return through a unit routine's return word and DEBREAK are dead
 * exits; any other sink edge is dead in the optimistic solve and reaches every
 * block of the unit in the pessimistic one.  An indirect reference reads the
 * word it lands on, since that word is the pointer.
 *
 * Prize A: the local class shares with itself; its live ranges are intervals
 * in one block, so the need is K, the widest overlap, and the prize |local|-K.
 * Prize B: the pool L = dead + local + span.  No word of L is live across a
 * call or on entry to its unit, so a bank needs only its neediest unit.
 * Within a unit words are colored greedily over their pessimistic occupancy:
 * the colors are an achievable need and the largest occupancy a floor.  call
 * and state are never colored: a word shared across a call is clobbered by
 * the callee, and state is a value the unit keeps.
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

// The two pools, measured separately; their needs add.
#define SPOOL_MAIN      0
#define SPOOL_HANDLER   1
#define SPOOL_COUNT     2

// What the two reachability walks leave on a block.
#define SREACH_MAIN     0x01
#define SREACH_HANDLER  0x02

// What an address holds for the call-argument map.
#define SARG_PROVED     1       // an inline argument a proved call steps past
#define SARG_UNKNOWN    2       // the word after a call whose return is undecided

// The classes, in the order they are decided.
typedef enum
{
    SC_TAKEN,
    SC_UNPLACED,
    SC_UNUSED,
    SC_MULTI,
    SC_OPEN,
    SC_BOTH,
    SC_DEAD,
    SC_SINK,
    SC_STATE,
    SC_CALL,
    SC_LOCAL,
    SC_SPAN,
    SC_COUNT
} SClass;

// One row per class: its name, its number in the dump's 1-5 scheme (0 for
// the unnumbered classes), and whether prize B pools it.
typedef struct
{
    const char *nameP;
    int dumpClass;
    int pooled;
} SClassInfo;

static const SClassInfo classInfo[SC_COUNT] =
{
    { "taken",    3, 0 },
    { "unplaced", 0, 0 },
    { "unused",   0, 0 },
    { "multi",    4, 0 },
    { "open",     5, 0 },
    { "both",     0, 0 },
    { "dead",     0, 1 },
    { "sink",     0, 0 },
    { "state",    0, 0 },
    { "call",     2, 0 },
    { "local",    1, 1 },
    { "span",     0, 1 }
};

// One word that references a population word, and what it does to it.  An
// idx or isp both reads and writes, and reads first.
typedef struct
{
    OptWordP fromP;
    int reads;
    int writes;
} SRef;

// One word of the population.
typedef struct
{
    OptWordP wordP;
    const char *nameP;          // the local label's name
    int onWord;                 // the label is on the word's own line
    SClass cls;
    char why[80];               // what decided the class, for the dump
    SRef *refsP;
    int refCount;
    int refCap;
    int readRefs;               // referencing words that read it
    int writeRefs;              // and that write it
    int blockCount;             // distinct blocks those words are in
    int unit;                   // the unit its references are in, -1 for none
    int local;                  // the block-local test holds, whatever the class
    int once;                   // and exactly one write then one read
    int firstAddr;              // the first and last reference, when local
    int lastAddr;
    int codeBank;               // the bank of the block they are in
    int adjTaken;               // a word of its data run has its address taken
    unsigned int *readBitsP;    // per position of the unit: read here
    unsigned int *writeBitsP;   // written here
    unsigned int *occP;         // occupied here (pooled classes only)
    int firstPos;               // the first occupied position, -1 for none
    int color;                  // prize B's greedy color, -1 until colored
    int byRoutine;              // multi(4) when the unit is cut by routine
} SWord;

// One unit.
typedef struct
{
    int id;                     // 1 based, in order of its first block
    OptRoutineP *routinesPP;    // its routines, in id order
    int routineCount;
    int routineCap;
    OptBlockP *blocksPP;        // its blocks, in id order
    int blockCount;
    int blockCap;
    int *startPosP;             // per block, the position of its first word
    unsigned char *entryP;      // per block, an entry of the unit
    int posCount;               // positions, the words of all its blocks
    int reach;                  // SREACH_ bits
    int outside;                // bit 0 main line, bit 1 handler line, bit 2 both
    int flagged;                // a routine is open, jumpsunknown or callsunknown
    int derived;                // a block calls or jumps indirectly into the sink
    int residual;               // other sink edges, which the two passes differ on
    int words;                  // population words placed in it
} SUnit;

// Per-word liveness over one unit, reused from word to word.
typedef struct
{
    unsigned char *genP;
    unsigned char *killP;
    unsigned char *inP;
    unsigned char *outP;
    int cap;
} SLive;

// Everything one run owns.
typedef struct
{
    OptTableP tableP;
    OptBlockP *byIdPP;              // blocks by 1-based id
    unsigned char *reachP;          // per block id, SREACH_ bits
    int *nodeOfBlockP;              // per block id, its union-find node, -1 for none
    int *parentP;                   // union-find over the nodes
    int nodeCount;                  // routines, then the three outside nodes
    int *unitOfNodeP;               // per representative node, its unit index
    int *unitOfBlockP;              // per block id, its unit index, -1 for none
    int *idxInUnitP;                // per block id, its index within its unit
    SUnit *unitsP;
    int unitCount;
    unsigned char *argP[MAXBANK + 1];       // per address, SARG_
    unsigned char *returnP[MAXBANK + 1];    // per address, a routine's return word
    SWord *wordsP;
    int wordCount;
    int wordCap;
    SLive live;
    int mismatches;                 // occupancy walks that disagreed with the solve

    // bookkeeping, per bank
    int labelled[MAXBANK + 1];      // words carrying a local label
    int returns[MAXBANK + 1];       // of those, return words
    int text[MAXBANK + 1];          // text, ascii and type340 words
    int code[MAXBANK + 1];          // words classified as code
    int population[MAXBANK + 1];
    int onWord[MAXBANK + 1];        // of the population, label on the word's line
    int classCount[MAXBANK + 1][SC_COUNT];
    int once[MAXBANK + 1];
    int heldOpen[MAXBANK + 1];      // words the local test holds for, held back by open
    int heldBoth[MAXBANK + 1];      // and by both
    int openFlagged[MAXBANK + 1];   // open words, by the routine flags
    int adjacent[MAXBANK + 1];      // words not taken whose data run holds a taken word
    int adjacentPooled[MAXBANK + 1];    // of those, pooled ones
    int above[MAXBANK + 1];         // class 3 words that are so only by sitting above a
                                    // taken plain word in their data run
    int unknownWrites[MAXBANK + 1];     // unfollowed indirect writes
    int byRoutine[MAXBANK + 1];     // placed words that would be class 4 by routine
    int byRoutinePooled[MAXBANK + 1];   // of those, pooled ones
    int byRoutineLocal[MAXBANK + 1];    // and local ones
    int localPool[MAXBANK + 1][SPOOL_COUNT];
    int kPool[MAXBANK + 1][SPOOL_COUNT];
    int pooled[MAXBANK + 1][SPOOL_COUNT];
    int needLo[MAXBANK + 1][SPOOL_COUNT];
    int needHi[MAXBANK + 1][SPOOL_COUNT];
    int residualWords;              // pooled words in a unit with a residual sink
} SPass;

// Small helpers.

// Allocate zeroed memory; out of memory is fatal.
// Returns the memory, never NILP.
static void *
sAlloc(size_t count, size_t size)
{
void *memP;

    if( !count )
    {
        count = 1;
    }

    if( !(memP = calloc(count, size)) )
    {
        fprintf(stderr, "am1: out of memory measuring the scratch pool\n");
        exit(1);
    }

    return(memP);
}

// Grow an array to hold at least one more element than it counts.
// Returns the array, perhaps moved.
static void *
sGrow(void *arrayP, int count, int *capP, size_t size)
{
    if( count < *capP )
    {
        return(arrayP);
    }

    *capP = (*capP)?(*capP * 2):8;

    if( !(arrayP = realloc(arrayP, ((size_t)*capP * size))) )
    {
        fprintf(stderr, "am1: out of memory measuring the scratch pool\n");
        exit(1);
    }

    return(arrayP);
}

// Test one bit of a bit set.
// Returns 1 when it is set, 0 when not.
static int
bitTest(unsigned int *bitsP, int pos)
{
    return( (bitsP[pos / 32] >> (pos % 32)) & 1 );
}

// Set one bit of a bit set.
static void
bitSet(unsigned int *bitsP, int pos)
{
    bitsP[pos / 32] |= (1u << (pos % 32));
}

// The name of a pool, for the dump.
// Returns a static string.
static const char *
reachName(int reach)
{
    switch( reach )
    {
    case SREACH_MAIN:
        return("main");

    case SREACH_HANDLER:
        return("handler");

    case (SREACH_MAIN | SREACH_HANDLER):
        return("both");

    default:
        return("none");
    }
}

// The local label a word carries.
// Returns the label's symbol, or NILP when it carries none.
static SymNodeP
localLabel(OptWordP wordP)
{
OptLabelP labelP;

    for( labelP = wordP->labelsP; labelP; labelP = labelP->nextP )
    {
        if( labelP->symP && ((labelP->symP->flags & SYM_MASK) == SYM_LOC) )
        {
            return(labelP->symP);
        }
    }

    return(NILP);
}

// Does a word decode as xct?
// Returns 1 when it does, 0 when not.
static int
isXct(OptWordP wordP)
{
    return( wordP && (wordP->decode.group == OPTG_MEMREF) && (wordP->decode.opcode == 010) );
}

// Units: the reachability walks, the union of routines, the unit table.

// Walk every successor edge from a set of root blocks, calls included, and
// leave a bit on every block reached.
static void
walkReach(SPass *passP, OptBlockP *rootsPP, int rootCount, int bit)
{
OptBlockP *stackPP;
OptBlockP blockP;
OptFlowEdgeP flowP;
int top;
int i;

    stackPP = (OptBlockP *)sAlloc((size_t)passP->tableP->blockCount + 1, sizeof(OptBlockP));
    top = 0;

    for( i = 0; i < rootCount; ++i )
    {
        if( !(passP->reachP[rootsPP[i]->id] & bit) )
        {
            passP->reachP[rootsPP[i]->id] |= bit;
            stackPP[top++] = rootsPP[i];
        }
    }

    while( top > 0 )
    {
        blockP = stackPP[--top];

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( !flowP->toP || (passP->reachP[flowP->toP->id] & bit) )
            {
                continue;
            }

            passP->reachP[flowP->toP->id] |= bit;
            stackPP[top++] = flowP->toP;
        }
    }

    free(stackPP);
}

// Is a block's first word a sequence-break handler entry (bank 0, 4n+3, in a
// frame below the start address)?
// Returns 1 when it is, 0 when not.
static int
isHandlerSlot(OptTableP tableP, OptBlockP blockP)
{
    return( optIsSbsEntry(tableP, blockP->bank, blockP->startAddr) );
}

// Run the two walks: the main line from every entry but the handler slots
// (a slot that is the start address included), the handler line from the slots.
static void
findReach(SPass *passP)
{
OptTableP tableP;
OptBlockP blockP;
OptBlockP *mainPP;
OptBlockP *handlerPP;
int mainCount;
int handlerCount;

    tableP = passP->tableP;
    mainPP = (OptBlockP *)sAlloc((size_t)tableP->blockCount + 1, sizeof(OptBlockP));
    handlerPP = (OptBlockP *)sAlloc((size_t)tableP->blockCount + 1, sizeof(OptBlockP));
    mainCount = 0;
    handlerCount = 0;

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( !blockP->isEntry )
        {
            continue;
        }

        if( isHandlerSlot(tableP, blockP) )
        {
            handlerPP[handlerCount++] = blockP;

            if( blockP->firstP && (blockP->firstP->flags & OPTF_START) )
            {
                mainPP[mainCount++] = blockP;
            }
        }
        else
        {
            mainPP[mainCount++] = blockP;
        }
    }

    walkReach(passP, mainPP, mainCount, SREACH_MAIN);
    walkReach(passP, handlerPP, handlerCount, SREACH_HANDLER);
    free(mainPP);
    free(handlerPP);
}

// Find a node's representative, compressing the path.
// Returns the representative.
static int
findNode(SPass *passP, int node)
{
int root;
int nextNode;

    root = node;

    while( passP->parentP[root] != root )
    {
        root = passP->parentP[root];
    }

    while( passP->parentP[node] != root )
    {
        nextNode = passP->parentP[node];
        passP->parentP[node] = root;
        node = nextNode;
    }

    return(root);
}

// Join two nodes' sets.  The lower representative survives, so the result
// does not depend on the order of the joins.
static void
joinNodes(SPass *passP, int a, int b)
{
    a = findNode(passP, a);
    b = findNode(passP, b);

    if( a == b )
    {
        return;
    }

    if( a < b )
    {
        passP->parentP[b] = a;
    }
    else
    {
        passP->parentP[a] = b;
    }
}

// Is a sink edge leaving a block a dead exit of its unit: DEBREAK, or a
// return through the return word of one of the unit's routines -- jmp i rtn,
// or the patched jmp that is the return word itself?
// Returns 1 when it is, 0 when not.
static int
deadExit(SUnit *unitP, OptBlockP blockP, OptFlowEdgeP flowP)
{
OptWordP lastP;
OptRoutineP routineP;
int i;

    if( flowP->sink == OPTSK_DEBREAK )
    {
        return(1);
    }

    lastP = blockP->lastP;

    for( i = 0; i < unitP->routineCount; ++i )
    {
        routineP = unitP->routinesPP[i];

        if( (routineP->returnBank != lastP->bank) || (routineP->returnAddr < 0) )
        {
            continue;
        }

        if( lastP->addr == routineP->returnAddr )
        {
            return(1);
        }

        if( (lastP->decode.group == OPTG_MEMREF) && (lastP->decode.opcode == 060)
            && lastP->decode.memIndirect && (lastP->decode.address == routineP->returnAddr) )
        {
            return(1);
        }
    }

    return(0);
}

// Build the units: a body block is its routine's node (a shared block joins
// them), any other reached block one of the three outside nodes, and a plain
// flow edge between two nodes joins them.
static void
buildUnits(SPass *passP)
{
OptTableP tableP;
OptBlockP blockP;
OptRoutineP routineP;
OptFlowEdgeP flowP;
SUnit *unitP;
int routineNodes;
int node;
int rep;
int i;
int id;
int u;
int pos;

    tableP = passP->tableP;
    routineNodes = tableP->routineCount;
    passP->nodeCount = (routineNodes + 3);
    passP->parentP = (int *)sAlloc((size_t)passP->nodeCount, sizeof(int));
    passP->nodeOfBlockP = (int *)sAlloc((size_t)tableP->blockCount + 1, sizeof(int));

    for( i = 0; i < passP->nodeCount; ++i )
    {
        passP->parentP[i] = i;
    }

    for( id = 0; id <= tableP->blockCount; ++id )
    {
        passP->nodeOfBlockP[id] = -1;
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        for( i = 0; i < routineP->blockCount; ++i )
        {
            id = routineP->bodyIdsP[i];

            if( passP->nodeOfBlockP[id] < 0 )
            {
                passP->nodeOfBlockP[id] = (routineP->id - 1);
            }
            else
            {
                joinNodes(passP, passP->nodeOfBlockP[id], (routineP->id - 1));
            }
        }
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( passP->nodeOfBlockP[blockP->id] >= 0 )
        {
            continue;
        }

        switch( passP->reachP[blockP->id] )
        {
        case SREACH_MAIN:
            passP->nodeOfBlockP[blockP->id] = routineNodes;
            break;

        case SREACH_HANDLER:
            passP->nodeOfBlockP[blockP->id] = (routineNodes + 1);
            break;

        case (SREACH_MAIN | SREACH_HANDLER):
            passP->nodeOfBlockP[blockP->id] = (routineNodes + 2);
            break;

        default:
            break;
        }
    }

    // Join across plain edges (fall-in, a jump into another entry, the main
    // line running into a routine); only a reached block's edges are real.
    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        if( ((node = passP->nodeOfBlockP[blockP->id]) < 0) || !passP->reachP[blockP->id] )
        {
            continue;
        }

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( !flowP->toP || (flowP->kind == OPTFK_CALL) || (passP->nodeOfBlockP[flowP->toP->id] < 0) )
            {
                continue;
            }

            joinNodes(passP, node, passP->nodeOfBlockP[flowP->toP->id]);
        }
    }

    // Dense unit numbers, in order of each unit's first block.
    passP->unitOfNodeP = (int *)sAlloc((size_t)passP->nodeCount, sizeof(int));
    passP->unitOfBlockP = (int *)sAlloc((size_t)tableP->blockCount + 1, sizeof(int));
    passP->idxInUnitP = (int *)sAlloc((size_t)tableP->blockCount + 1, sizeof(int));
    passP->unitsP = (SUnit *)sAlloc((size_t)passP->nodeCount, sizeof(SUnit));

    for( i = 0; i < passP->nodeCount; ++i )
    {
        passP->unitOfNodeP[i] = -1;
    }

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        passP->unitOfBlockP[blockP->id] = -1;

        if( (node = passP->nodeOfBlockP[blockP->id]) < 0 )
        {
            continue;
        }

        rep = findNode(passP, node);

        if( passP->unitOfNodeP[rep] < 0 )
        {
            passP->unitOfNodeP[rep] = passP->unitCount;
            passP->unitsP[passP->unitCount].id = (passP->unitCount + 1);
            ++passP->unitCount;
        }

        u = passP->unitOfNodeP[rep];
        unitP = &passP->unitsP[u];
        passP->unitOfBlockP[blockP->id] = u;
        passP->idxInUnitP[blockP->id] = unitP->blockCount;
        unitP->blocksPP = (OptBlockP *)sGrow(unitP->blocksPP, unitP->blockCount, &unitP->blockCap, sizeof(OptBlockP));
        unitP->blocksPP[unitP->blockCount++] = blockP;
        unitP->reach |= passP->reachP[blockP->id];

        if( node >= routineNodes )
        {
            unitP->outside |= (1 << (node - routineNodes));
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        u = passP->unitOfNodeP[findNode(passP, (routineP->id - 1))];

        if( u < 0 )
        {
            continue;
        }

        unitP = &passP->unitsP[u];
        unitP->routinesPP = (OptRoutineP *)sGrow(unitP->routinesPP, unitP->routineCount, &unitP->routineCap,
            sizeof(OptRoutineP));
        unitP->routinesPP[unitP->routineCount++] = routineP;

        if( routineP->flags & (OPTRT_OPENBODY | OPTRT_JUMPSUNKNOWN | OPTRT_CALLSUNKNOWN) )
        {
            unitP->flagged = 1;
        }
    }

    // Positions, entries and the unit's own look at its sink edges.
    for( u = 0; u < passP->unitCount; ++u )
    {
        unitP = &passP->unitsP[u];
        unitP->startPosP = (int *)sAlloc((size_t)unitP->blockCount, sizeof(int));
        unitP->entryP = (unsigned char *)sAlloc((size_t)unitP->blockCount, 1);
        pos = 0;

        for( i = 0; i < unitP->blockCount; ++i )
        {
            blockP = unitP->blocksPP[i];
            unitP->startPosP[i] = pos;
            pos += blockP->wordCount;

            if( blockP->isEntry )
            {
                unitP->entryP[i] = 1;
            }

            for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
            {
                if( flowP->toP )
                {
                    continue;
                }

                if( flowP->kind == OPTFK_CALL )
                {
                    unitP->derived = 1;
                }
                else if( deadExit(unitP, blockP, flowP) )
                {
                    continue;
                }
                else if( (flowP->sink == OPTSK_INDIRECT) || (flowP->sink == OPTSK_PATCHED) )
                {
                    unitP->derived = 1;
                }
                else
                {
                    ++unitP->residual;
                }
            }
        }

        unitP->posCount = pos;

        for( i = 0; i < unitP->routineCount; ++i )
        {
            blockP = unitP->routinesPP[i]->entryBlockP;

            if( blockP && (passP->unitOfBlockP[blockP->id] == u) )
            {
                unitP->entryP[passP->idxInUnitP[blockP->id]] = 1;
            }
        }
    }
}

// A unit's name for the dump: its first routine's entry label, or which
// outside line it is.
// Returns a static string or a label's name.
static const char *
unitName(SUnit *unitP)
{
    if( unitP->routineCount && unitP->routinesPP[0]->entryBlockP )
    {
        return( firstLabelName(unitP->routinesPP[0]->entryBlockP->firstP) );
    }

    if( unitP->outside & 4 )
    {
        return("(both lines)");
    }

    if( unitP->outside & 2 )
    {
        return("(handler line)");
    }

    return("(main line)");
}

// The population and its references.

// Mark the call-argument map and the return-word map.
static void
buildMaps(SPass *passP)
{
OptTableP tableP;
OptWordP wordP;
OptRoutineP routineP;
int i;
int step;
int bank;

    tableP = passP->tableP;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        passP->argP[bank] = (unsigned char *)sAlloc(BANKSIZE, 1);
        passP->returnP[bank] = (unsigned char *)sAlloc(BANKSIZE, 1);
    }

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( !wordP->isCallSite )
        {
            continue;
        }

        if( wordP->returnCase == OPTRC_STEPPED )
        {
            for( step = 1; step <= wordP->returnSteps; ++step )
            {
                passP->argP[wordP->bank][(wordP->addr + step) & (BANKSIZE - 1)] = SARG_PROVED;
            }
        }
        else if( wordP->returnCase == OPTRC_UNKNOWN )
        {
            if( !passP->argP[wordP->bank][(wordP->addr + 1) & (BANKSIZE - 1)] )
            {
                passP->argP[wordP->bank][(wordP->addr + 1) & (BANKSIZE - 1)] = SARG_UNKNOWN;
            }
        }
    }

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( (routineP->returnBank >= 0) && (routineP->returnBank <= MAXBANK) && (routineP->returnAddr >= 0) )
        {
            passP->returnP[routineP->returnBank][routineP->returnAddr & (BANKSIZE - 1)] = 1;
        }
    }
}

// Record one reference to a population word, merging it with any earlier
// one from the same word.
static void
addRef(SWord *swP, OptWordP fromP, int reads, int writes)
{
int i;

    for( i = 0; i < swP->refCount; ++i )
    {
        if( swP->refsP[i].fromP == fromP )
        {
            swP->refsP[i].reads |= reads;
            swP->refsP[i].writes |= writes;
            return;
        }
    }

    swP->refsP = (SRef *)sGrow(swP->refsP, swP->refCount, &swP->refCap, sizeof(SRef));
    swP->refsP[swP->refCount].fromP = fromP;
    swP->refsP[swP->refCount].reads = reads;
    swP->refsP[swP->refCount].writes = writes;
    ++swP->refCount;
}

// Gather a word's references from its in-edges.
// Returns why the edges alone make it taken(3), or NILP when they do not.
static const char *
gatherRefs(SWord *swP)
{
OptEdgeP edgeP;
const char *takenP;
int i;

    takenP = NILP;

    for( edgeP = swP->wordP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->flags & OPTEF_PLACEHOLDER )
        {
            continue;
        }

        if( edgeP->flags & OPTEF_VIAPOINTER )
        {
            takenP = (takenP)?takenP:"reached through a followed pointer";
            continue;
        }

        if( edgeP->flags & OPTEF_INDIRECT )
        {
            // The word is the pointer, and an indirection reads it.
            addRef(swP, edgeP->fromP, 1, 0);
            continue;
        }

        switch( edgeP->role )
        {
        case OPTR_READ:
            addRef(swP, edgeP->fromP, 1, 0);
            break;

        case OPTR_WRITE:
            if( edgeP->flags & OPTEF_PATCH )
            {
                takenP = (takenP)?takenP:"patched";
            }
            else
            {
                addRef(swP, edgeP->fromP, 0, 1);
            }
            break;

        case OPTR_JUMP:
            takenP = (takenP)?takenP:"jumped to";
            break;

        case OPTR_EXECUTE:
            takenP = (takenP)?takenP:"xct'd";
            break;

        default:
            takenP = (takenP)?takenP:"taken";
            break;
        }
    }

    for( i = 0; i < swP->refCount; ++i )
    {
        if( swP->refsP[i].reads )
        {
            ++swP->readRefs;
        }

        if( swP->refsP[i].writes )
        {
            ++swP->writeRefs;
        }
    }

    return(takenP);
}

// Apply the block-local test: every reference in one block, the first a full
// write, and no xct from the first to the last.  Sets local, once and the
// span, whatever the word's class turns out to be.
static void
localTest(SPass *passP, SWord *swP)
{
OptBlockP blockP;
SRef *firstP;
int i;
int j;
int addr;

    swP->local = 0;
    swP->once = 0;
    swP->blockCount = 0;

    for( i = 0; i < swP->refCount; ++i )
    {
        if( !swP->refsP[i].fromP->blockP )
        {
            swP->blockCount = 0;
            return;
        }

        for( j = 0; j < i; ++j )
        {
            if( swP->refsP[j].fromP->blockP == swP->refsP[i].fromP->blockP )
            {
                break;
            }
        }

        if( j == i )
        {
            ++swP->blockCount;
        }
    }

    if( swP->blockCount != 1 )
    {
        return;
    }

    blockP = swP->refsP[0].fromP->blockP;
    firstP = NILP;

    swP->firstAddr = BANKSIZE;
    swP->lastAddr = -1;

    for( i = 0; i < swP->refCount; ++i )
    {
        addr = swP->refsP[i].fromP->addr;

        if( addr < swP->firstAddr )
        {
            swP->firstAddr = addr;
            firstP = &swP->refsP[i];
        }

        if( addr > swP->lastAddr )
        {
            swP->lastAddr = addr;
        }
    }

    swP->codeBank = blockP->bank;

    if( !firstP || !firstP->writes || firstP->reads )
    {
        return;
    }

    for( addr = swP->firstAddr; addr <= swP->lastAddr; ++addr )
    {
        if( isXct(wordAt(passP->tableP, blockP->bank, addr)) )
        {
            return;
        }
    }

    swP->local = 1;

    if( (swP->refCount == 2) && (swP->writeRefs == 1) && (swP->readRefs == 1) )
    {
        swP->once = 1;
    }
}

// Does the word's data run (consecutive emitted words outside the graph)
// hold a taken word?  Pointer arithmetic reaches such words with no edge:
// "law track; jda drumRead" may read several words above track.  A
// plain taken word below is a record base of unstated length; a string or
// table below ends at its terminator or size, so is only counted.
// Returns 2 for a plain taken word below it in the run, with *basePP set to
// it; 1 for any other taken word in the run; 0 for none.
static int
adjacentTaken(SPass *passP, OptWordP wordP, OptWordP *basePP)
{
OptWordP otherP;
int addr;
int found;

    found = 0;
    *basePP = NILP;

    for( addr = wordP->addr - 1; addr >= 0; --addr )
    {
        if( !(otherP = wordAt(passP->tableP, wordP->bank, addr)) || inGraph(otherP) )
        {
            break;
        }

        if( otherP->flags & OPTF_TAKEN )
        {
            if( (otherP->kind != OPTK_TEXT) && (otherP->kind != OPTK_ASCII) && (otherP->kind != OPTK_TYPE340)
                && (otherP->kind != OPTK_TABLE) )
            {
                *basePP = otherP;
                return(2);
            }

            found = 1;
        }
    }

    for( addr = wordP->addr + 1; (addr < BANKSIZE) && !found; ++addr )
    {
        if( !(otherP = wordAt(passP->tableP, wordP->bank, addr)) || inGraph(otherP) )
        {
            break;
        }

        if( otherP->flags & OPTF_TAKEN )
        {
            found = 1;
        }
    }

    return(found);
}

// Collect the population, in bank then address order.
static void
collectPopulation(SPass *passP)
{
OptTableP tableP;
OptWordP wordP;
SymNodeP symP;
SWord *swP;
int i;

    tableP = passP->tableP;

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( !(symP = localLabel(wordP)) )
        {
            continue;
        }

        ++passP->labelled[wordP->bank];

        if( passP->returnP[wordP->bank][wordP->addr] )
        {
            ++passP->returns[wordP->bank];
            continue;
        }

        if( (wordP->kind == OPTK_TEXT) || (wordP->kind == OPTK_ASCII) || (wordP->kind == OPTK_TYPE340) )
        {
            ++passP->text[wordP->bank];
            continue;
        }

        if( wordP->flags & OPTF_CODE )
        {
            ++passP->code[wordP->bank];
            continue;
        }

        passP->wordsP = (SWord *)sGrow(passP->wordsP, passP->wordCount, &passP->wordCap, sizeof(SWord));
        swP = &passP->wordsP[passP->wordCount++];
        memset(swP, 0, sizeof(SWord));
        swP->wordP = wordP;
        swP->nameP = symP->name;
        swP->onWord = (wordP->nodeP && (wordP->nodeP->type == LCLLOCATION));
        swP->unit = -1;
        swP->firstPos = -1;
        swP->color = -1;
        ++passP->population[wordP->bank];

        if( swP->onWord )
        {
            ++passP->onWord[wordP->bank];
        }
    }
}

// Order the population by bank, address and emission.
// Returns negative, zero or positive, as qsort() expects.
static int
compareWords(const void *aP, const void *bP)
{
const SWord *swaP;
const SWord *swbP;

    swaP = (const SWord *)aP;
    swbP = (const SWord *)bP;

    if( swaP->wordP->bank != swbP->wordP->bank )
    {
        return( swaP->wordP->bank - swbP->wordP->bank );
    }

    if( swaP->wordP->addr != swbP->wordP->addr )
    {
        return( swaP->wordP->addr - swbP->wordP->addr );
    }

    return( swaP->wordP->index - swbP->wordP->index );
}

// Liveness.

// The position of a referencing word within its unit.
// Returns the position.
static int
refPos(SPass *passP, SUnit *unitP, OptWordP fromP)
{
int i;

    i = passP->idxInUnitP[fromP->blockP->id];
    return( unitP->startPosP[i] + (fromP->addr - fromP->blockP->startAddr) );
}

// Lay a word's reads and writes out over its unit's positions.
static void
layOutRefs(SPass *passP, SWord *swP, SUnit *unitP)
{
int words;
int i;
int pos;

    words = ((unitP->posCount + 31) / 32);
    swP->readBitsP = (unsigned int *)sAlloc((size_t)words, sizeof(unsigned int));
    swP->writeBitsP = (unsigned int *)sAlloc((size_t)words, sizeof(unsigned int));

    for( i = 0; i < swP->refCount; ++i )
    {
        pos = refPos(passP, unitP, swP->refsP[i].fromP);

        if( swP->refsP[i].reads )
        {
            bitSet(swP->readBitsP, pos);
        }

        if( swP->refsP[i].writes )
        {
            bitSet(swP->writeBitsP, pos);
        }
    }
}

// Make sure the liveness arrays hold a unit's blocks.
static void
sizeLive(SLive *liveP, int count)
{
    if( count <= liveP->cap )
    {
        return;
    }

    free(liveP->genP);
    free(liveP->killP);
    free(liveP->inP);
    free(liveP->outP);
    liveP->cap = count;
    liveP->genP = (unsigned char *)sAlloc((size_t)count, 1);
    liveP->killP = (unsigned char *)sAlloc((size_t)count, 1);
    liveP->inP = (unsigned char *)sAlloc((size_t)count, 1);
    liveP->outP = (unsigned char *)sAlloc((size_t)count, 1);
}

// Solve one word's liveness over its unit, optimistic or pessimistic about
// residual sink edges; set *stateP if live on entry, *callP if live across a
// call.  The solution is left in passP->live for the occupancy walk.
static void
solveLiveness(SPass *passP, SWord *swP, SUnit *unitP, int pessimistic, int *stateP, int *callP)
{
SLive *liveP;
OptBlockP blockP;
OptFlowEdgeP flowP;
int i;
int j;
int addr;
int pos;
int changed;
int anyLive;
int out;
int in;
int calls;

    liveP = &passP->live;
    sizeLive(liveP, unitP->blockCount);

    for( i = 0; i < unitP->blockCount; ++i )
    {
        blockP = unitP->blocksPP[i];
        liveP->genP[i] = 0;
        liveP->killP[i] = 0;
        liveP->inP[i] = 0;
        liveP->outP[i] = 0;

        for( addr = blockP->startAddr; addr <= blockP->endAddr; ++addr )
        {
            pos = (unitP->startPosP[i] + (addr - blockP->startAddr));

            if( bitTest(swP->readBitsP, pos) && !liveP->killP[i] )
            {
                liveP->genP[i] = 1;
            }

            if( bitTest(swP->writeBitsP, pos) )
            {
                liveP->killP[i] = 1;
            }
        }
    }

    do
    {
        changed = 0;
        anyLive = 0;

        for( i = 0; i < unitP->blockCount; ++i )
        {
            anyLive |= liveP->inP[i];
        }

        for( i = (unitP->blockCount - 1); i >= 0; --i )
        {
            blockP = unitP->blocksPP[i];
            out = 0;

            for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
            {
                if( flowP->kind == OPTFK_CALL )
                {
                    continue;
                }

                if( flowP->toP )
                {
                    if( passP->unitOfBlockP[flowP->toP->id] == (unitP->id - 1) )
                    {
                        j = passP->idxInUnitP[flowP->toP->id];
                        out |= liveP->inP[j];
                    }
                }
                else if( pessimistic && !deadExit(unitP, blockP, flowP) )
                {
                    out |= anyLive;
                }
            }

            in = (liveP->genP[i] | (out & !liveP->killP[i]));

            if( (in != liveP->inP[i]) || (out != liveP->outP[i]) )
            {
                liveP->inP[i] = (unsigned char)in;
                liveP->outP[i] = (unsigned char)out;
                changed = 1;
            }
        }
    }
    while( changed );

    *stateP = 0;
    *callP = 0;

    for( i = 0; i < unitP->blockCount; ++i )
    {
        blockP = unitP->blocksPP[i];

        if( unitP->entryP[i] && liveP->inP[i] )
        {
            *stateP = 1;
        }

        calls = 0;

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( flowP->kind == OPTFK_CALL )
            {
                calls = 1;
            }
        }

        if( !calls )
        {
            continue;
        }

        for( flowP = blockP->succP; flowP; flowP = flowP->nextP )
        {
            if( (flowP->kind != OPTFK_RETURN) || !flowP->toP
                || (passP->unitOfBlockP[flowP->toP->id] != (unitP->id - 1)) )
            {
                continue;
            }

            if( liveP->inP[passP->idxInUnitP[flowP->toP->id]] )
            {
                *callP = 1;
            }
        }
    }
}

// Mark every position where the word is occupied (live after it, or written
// there), walking each block backward; a walk that misses the solved live-in
// is counted as a mismatch.
static void
markOccupancy(SPass *passP, SWord *swP, SUnit *unitP)
{
SLive *liveP;
OptBlockP blockP;
int i;
int addr;
int pos;
int live;
int reads;
int writes;

    liveP = &passP->live;
    swP->occP = (unsigned int *)sAlloc((size_t)((unitP->posCount + 31) / 32), sizeof(unsigned int));
    swP->firstPos = -1;

    for( i = 0; i < unitP->blockCount; ++i )
    {
        blockP = unitP->blocksPP[i];
        live = liveP->outP[i];

        for( addr = blockP->endAddr; addr >= blockP->startAddr; --addr )
        {
            pos = (unitP->startPosP[i] + (addr - blockP->startAddr));
            reads = bitTest(swP->readBitsP, pos);
            writes = bitTest(swP->writeBitsP, pos);

            if( live || writes )
            {
                bitSet(swP->occP, pos);

                if( (swP->firstPos < 0) || (pos < swP->firstPos) )
                {
                    swP->firstPos = pos;
                }
            }

            live = (reads || (live && !writes));
        }

        if( live != liveP->inP[i] )
        {
            ++passP->mismatches;
        }
    }
}

// Classification.

// The class liveness gives a private, single-pool word.
// Returns the class.
static SClass
liveClass(SWord *swP, int state, int call)
{
    if( state )
    {
        return(SC_STATE);
    }

    if( call )
    {
        return(SC_CALL);
    }

    if( swP->local )
    {
        return(SC_LOCAL);
    }

    return(SC_SPAN);
}

// Put one word in its class.
static void
classify(SPass *passP, SWord *swP)
{
OptWordP wordP;
OptWordP baseP;
SUnit *unitP;
const char *takenP;
unsigned int flags;
int arg;
int i;
int u;
int state;
int call;
SClass optimistic;
SClass pessimistic;

    wordP = swP->wordP;
    flags = wordP->flags;
    takenP = gatherRefs(swP);
    localTest(passP, swP);
    arg = passP->argP[wordP->bank][wordP->addr];

    // taken(3)
    if( flags & (OPTF_TAKEN | OPTF_PATCHED | OPTF_XCTTARGET | OPTF_JUMPTARGET
                 | OPTF_MAYBE_READ | OPTF_MAYBE_WRITTEN | OPTF_MAYBE_ENTERED) )
    {
        swP->cls = SC_TAKEN;
        snprintf(swP->why, sizeof(swP->why), "%s",
            (flags & OPTF_TAKEN)?"taken":(flags & OPTF_PATCHED)?"patched":(flags & OPTF_XCTTARGET)?"xct'd":
            (flags & OPTF_JUMPTARGET)?"jumped to":"maybe reached through an unfollowed pointer");
        return;
    }

    if( takenP )
    {
        swP->cls = SC_TAKEN;
        snprintf(swP->why, sizeof(swP->why), "%s", takenP);
        return;
    }

    if( (swP->adjTaken = adjacentTaken(passP, wordP, &baseP)) == 2 )
    {
        swP->cls = SC_TAKEN;
        snprintf(swP->why, sizeof(swP->why), "above %s at %04o, taken, in its data run", firstLabelName(baseP),
            baseP->addr);
        return;
    }

    if( arg == SARG_PROVED )
    {
        swP->cls = SC_TAKEN;
        snprintf(swP->why, sizeof(swP->why), "inline argument of a proved call");
        return;
    }

    // references that cannot be placed
    if( flags & OPTF_DUPADDR )
    {
        swP->cls = SC_UNPLACED;
        snprintf(swP->why, sizeof(swP->why), "overlaid address");
        return;
    }

    if( arg == SARG_UNKNOWN )
    {
        swP->cls = SC_UNPLACED;
        snprintf(swP->why, sizeof(swP->why), "word after a call whose return is undecided");
        return;
    }

    for( i = 0; i < swP->refCount; ++i )
    {
        OptWordP fromP;

        fromP = swP->refsP[i].fromP;

        if( !fromP->blockP )
        {
            swP->cls = SC_UNPLACED;
            snprintf(swP->why, sizeof(swP->why), "referenced from %04o, outside the graph", fromP->addr);
            return;
        }

        if( passP->unitOfBlockP[fromP->blockP->id] < 0 )
        {
            swP->cls = SC_UNPLACED;
            snprintf(swP->why, sizeof(swP->why), "referenced from %04o, unreached", fromP->addr);
            return;
        }
    }

    if( !swP->refCount )
    {
        swP->cls = SC_UNUSED;
        snprintf(swP->why, sizeof(swP->why), "no reference");
        return;
    }

    // multi(4)
    u = passP->unitOfBlockP[swP->refsP[0].fromP->blockP->id];

    for( i = 1; i < swP->refCount; ++i )
    {
        if( passP->unitOfBlockP[swP->refsP[i].fromP->blockP->id] != u )
        {
            swP->cls = SC_MULTI;
            snprintf(swP->why, sizeof(swP->why), "units u%d and u%d", (u + 1),
                (passP->unitOfBlockP[swP->refsP[i].fromP->blockP->id] + 1));
            return;
        }
    }

    swP->unit = u;
    unitP = &passP->unitsP[u];
    ++unitP->words;

    if( !unitP->reach )
    {
        swP->cls = SC_UNPLACED;
        snprintf(swP->why, sizeof(swP->why), "unit u%d is unreached", unitP->id);
        return;
    }

    // open(5)
    if( unitP->flagged || unitP->derived )
    {
        swP->cls = SC_OPEN;
        snprintf(swP->why, sizeof(swP->why), "%s", (unitP->flagged)?"a routine of the unit is open or unknown":
            "the unit calls or jumps into the sink");
        return;
    }

    if( (unitP->reach == (SREACH_MAIN | SREACH_HANDLER)) || (unitP->outside & 4) )
    {
        swP->cls = SC_BOTH;
        snprintf(swP->why, sizeof(swP->why), "the unit runs on both lines");
        return;
    }

    if( !swP->readRefs )
    {
        swP->cls = SC_DEAD;
        snprintf(swP->why, sizeof(swP->why), "written %d time%s, never read", swP->writeRefs,
            (swP->writeRefs == 1)?"":"s");
        layOutRefs(passP, swP, unitP);
        solveLiveness(passP, swP, unitP, 1, &state, &call);
        markOccupancy(passP, swP, unitP);
        return;
    }

    // liveness
    layOutRefs(passP, swP, unitP);
    solveLiveness(passP, swP, unitP, 0, &state, &call);
    optimistic = liveClass(swP, state, call);
    solveLiveness(passP, swP, unitP, 1, &state, &call);
    pessimistic = liveClass(swP, state, call);

    if( optimistic != pessimistic )
    {
        swP->cls = SC_SINK;
        snprintf(swP->why, sizeof(swP->why), "%s optimistically, %s pessimistically",
            classInfo[optimistic].nameP, classInfo[pessimistic].nameP);
        return;
    }

    swP->cls = optimistic;

    switch( swP->cls )
    {
    case SC_STATE:
        snprintf(swP->why, sizeof(swP->why), "live on entry to the unit");
        break;

    case SC_CALL:
        snprintf(swP->why, sizeof(swP->why), "live across a call");
        break;

    case SC_LOCAL:
        snprintf(swP->why, sizeof(swP->why), "block %04o-%04o%s", swP->firstAddr, swP->lastAddr,
            (swP->once)?", once":"");
        break;

    default:
        // One block and still span: an xct inside the span, or a first
        // reference that is not a full write, defeated the local test.
        if( swP->blockCount == 1 )
        {
            snprintf(swP->why, sizeof(swP->why), "private, one block, but not block-local");
        }
        else
        {
            snprintf(swP->why, sizeof(swP->why), "private, across %d blocks", swP->blockCount);
        }
        break;
    }

    if( classInfo[swP->cls].pooled )
    {
        markOccupancy(passP, swP, unitP);
    }
}

// Would a placed word be multi(4) if its references had to be private to a
// routine rather than a unit (two routines, a routine and none, or a shared
// block)?  Counted to show what the finer unit cut gains.
// Returns 1 when it would, 0 when not.
static int
multiByRoutine(SPass *passP, SWord *swP)
{
OptRoutineP firstP;
OptBlockP blockP;
int i;

    if( (swP->unit < 0) || !swP->refCount )
    {
        return(0);
    }

    firstP = optRoutineOfBlock(passP->tableP, swP->refsP[0].fromP->blockP);

    for( i = 0; i < swP->refCount; ++i )
    {
        blockP = swP->refsP[i].fromP->blockP;

        if( (optBlockOwnerCount(passP->tableP, blockP) > 1) || (optRoutineOfBlock(passP->tableP, blockP) != firstP) )
        {
            return(1);
        }
    }

    return(0);
}

// The prizes.

// The pool a placed word is in.
// Returns SPOOL_MAIN or SPOOL_HANDLER.
static int
poolOf(SPass *passP, SWord *swP)
{
    return( (passP->unitsP[swP->unit].reach & SREACH_MAIN)?SPOOL_MAIN:SPOOL_HANDLER );
}

// Prize A: the widest overlap of the local class's intervals, per bank and
// pool.  Intervals in different blocks are at different addresses, so a
// sweep over addresses finds the widest overlap inside any one block.
static void
measureLocal(SPass *passP)
{
SWord *aP;
SWord *bP;
int i;
int j;
int depth;
int pool;

    for( i = 0; i < passP->wordCount; ++i )
    {
        aP = &passP->wordsP[i];

        if( aP->cls != SC_LOCAL )
        {
            continue;
        }

        pool = poolOf(passP, aP);
        ++passP->localPool[aP->wordP->bank][pool];
        depth = 0;

        for( j = 0; j < passP->wordCount; ++j )
        {
            bP = &passP->wordsP[j];

            if( (bP->cls != SC_LOCAL) || (bP->wordP->bank != aP->wordP->bank) || (poolOf(passP, bP) != pool)
                || (bP->codeBank != aP->codeBank) )
            {
                continue;
            }

            if( (bP->firstAddr <= aP->firstAddr) && (aP->firstAddr <= bP->lastAddr) )
            {
                ++depth;
            }
        }

        if( depth > passP->kPool[aP->wordP->bank][pool] )
        {
            passP->kPool[aP->wordP->bank][pool] = depth;
        }
    }
}

// Do two pooled words' occupancies meet?
// Returns 1 when they do, 0 when not.
static int
occupanciesMeet(SWord *aP, SWord *bP, int words)
{
int i;

    for( i = 0; i < words; ++i )
    {
        if( aP->occP[i] & bP->occP[i] )
        {
            return(1);
        }
    }

    return(0);
}

// Prize B: per unit and word bank, the largest occupancy count and a greedy
// coloring, and per bank and pool the neediest unit of each.
static void
measurePooled(SPass *passP, FILE *fP)
{
SUnit *unitP;
SWord *swP;
SWord *otherP;
SWord **listPP;
int *countP;
int u;
int bank;
int n;
int i;
int j;
int k;
int words;
int maxLive;
int colors;
int color;
int used;
int pool;

    listPP = (SWord **)sAlloc((size_t)passP->wordCount + 1, sizeof(SWord *));

    for( u = 0; u < passP->unitCount; ++u )
    {
        unitP = &passP->unitsP[u];
        words = ((unitP->posCount + 31) / 32);
        countP = NILP;

        for( bank = 0; bank <= MAXBANK; ++bank )
        {
            n = 0;

            for( i = 0; i < passP->wordCount; ++i )
            {
                swP = &passP->wordsP[i];

                if( (swP->unit == u) && classInfo[swP->cls].pooled && (swP->wordP->bank == bank) )
                {
                    listPP[n++] = swP;
                }
            }

            if( !n )
            {
                continue;
            }

            if( unitP->residual )
            {
                passP->residualWords += n;
            }

            // The floor: the most words occupied at any one position.
            if( !countP )
            {
                countP = (int *)sAlloc((size_t)unitP->posCount + 1, sizeof(int));
            }

            memset(countP, 0, ((size_t)unitP->posCount + 1) * sizeof(int));
            maxLive = 0;

            for( i = 0; i < n; ++i )
            {
                for( k = 0; k < unitP->posCount; ++k )
                {
                    if( bitTest(listPP[i]->occP, k) && (++countP[k] > maxLive) )
                    {
                        maxLive = countP[k];
                    }
                }
            }

            // The coloring: in order of first occupied position, each word
            // takes the lowest color no word it meets already holds.
            for( i = 1; i < n; ++i )
            {
                swP = listPP[i];

                for( j = i; (j > 0) && ((listPP[j - 1]->firstPos > swP->firstPos)
                     || ((listPP[j - 1]->firstPos == swP->firstPos) && (listPP[j - 1]->wordP->addr > swP->wordP->addr))); --j )
                {
                    listPP[j] = listPP[j - 1];
                }

                listPP[j] = swP;
            }

            colors = 0;

            for( i = 0; i < n; ++i )
            {
                swP = listPP[i];

                for( color = 0; ; ++color )
                {
                    used = 0;

                    for( j = 0; j < i; ++j )
                    {
                        otherP = listPP[j];

                        if( (otherP->color == color) && occupanciesMeet(swP, otherP, words) )
                        {
                            used = 1;
                            break;
                        }
                    }

                    if( !used )
                    {
                        break;
                    }
                }

                swP->color = color;

                if( (color + 1) > colors )
                {
                    colors = (color + 1);
                }
            }

            pool = poolOf(passP, listPP[0]);
            passP->pooled[bank][pool] += n;

            if( maxLive > passP->needLo[bank][pool] )
            {
                passP->needLo[bank][pool] = maxLive;
            }

            if( colors > passP->needHi[bank][pool] )
            {
                passP->needHi[bank][pool] = colors;
            }

            if( fP )
            {
                fprintf(fP, "color u%d b%d: pooled %d, occupancy %d, colors %d\n", unitP->id, bank, n, maxLive, colors);
            }
        }

        free(countP);
    }

    free(listPP);
}

// The dump.

// Release everything a run owns.
static void
freePass(SPass *passP)
{
int i;
int bank;

    for( i = 0; i < passP->wordCount; ++i )
    {
        free(passP->wordsP[i].refsP);
        free(passP->wordsP[i].readBitsP);
        free(passP->wordsP[i].writeBitsP);
        free(passP->wordsP[i].occP);
    }

    free(passP->wordsP);

    for( i = 0; i < passP->unitCount; ++i )
    {
        free(passP->unitsP[i].routinesPP);
        free(passP->unitsP[i].blocksPP);
        free(passP->unitsP[i].startPosP);
        free(passP->unitsP[i].entryP);
    }

    free(passP->unitsP);

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        free(passP->argP[bank]);
        free(passP->returnP[bank]);
    }

    free(passP->byIdPP);
    free(passP->reachP);
    free(passP->nodeOfBlockP);
    free(passP->parentP);
    free(passP->unitOfNodeP);
    free(passP->unitOfBlockP);
    free(passP->idxInUnitP);
    free(passP->live.genP);
    free(passP->live.killP);
    free(passP->live.inP);
    free(passP->live.outP);
}

// Build the pass up to the tallies and print nothing, so the dump and the
// report section read one measurement.  The caller runs measureLocal() and
// measurePooled() itself, since the dump prints between them, and must
// freePass() the SPass.
static void
buildScratchPass(SPass *passP, OptTableP tableP)
{
SWord *swP;
OptBlockP blockP;
OptWordP wordP;
OptEdgeP edgeP;
int i;

    memset(passP, 0, sizeof(*passP));
    passP->tableP = tableP;
    passP->byIdPP = (OptBlockP *)sAlloc((size_t)tableP->blockCount + 1, sizeof(OptBlockP));
    passP->reachP = (unsigned char *)sAlloc((size_t)tableP->blockCount + 1, 1);

    for( blockP = tableP->blocksP; blockP; blockP = blockP->nextP )
    {
        passP->byIdPP[blockP->id] = blockP;
    }

    findReach(passP);
    buildUnits(passP);
    buildMaps(passP);
    collectPopulation(passP);
    qsort(passP->wordsP, (size_t)passP->wordCount, sizeof(SWord), compareWords);

    for( i = 0; i < passP->wordCount; ++i )
    {
        classify(passP, &passP->wordsP[i]);
    }

    // The per-bank tallies, here so a consumer that prints no word lines has
    // them too.
    for( i = 0; i < passP->wordCount; ++i )
    {
        swP = &passP->wordsP[i];
        wordP = swP->wordP;
        ++passP->classCount[wordP->bank][swP->cls];

        if( swP->once && (swP->cls == SC_LOCAL) )
        {
            ++passP->once[wordP->bank];
        }

        if( swP->local && (swP->cls == SC_OPEN) )
        {
            ++passP->heldOpen[wordP->bank];
        }

        if( swP->local && (swP->cls == SC_BOTH) )
        {
            ++passP->heldBoth[wordP->bank];
        }

        if( (swP->cls == SC_OPEN) && passP->unitsP[swP->unit].flagged )
        {
            ++passP->openFlagged[wordP->bank];
        }

        if( (swP->cls == SC_TAKEN) && (swP->adjTaken == 2) )
        {
            ++passP->above[wordP->bank];
        }
        else if( (swP->cls != SC_TAKEN) && (swP->adjTaken == 1) )
        {
            ++passP->adjacent[wordP->bank];

            if( classInfo[swP->cls].pooled )
            {
                ++passP->adjacentPooled[wordP->bank];
            }
        }

        if( (swP->byRoutine = multiByRoutine(passP, swP)) )
        {
            ++passP->byRoutine[wordP->bank];

            if( classInfo[swP->cls].pooled )
            {
                ++passP->byRoutinePooled[wordP->bank];
            }

            if( swP->cls == SC_LOCAL )
            {
                ++passP->byRoutineLocal[wordP->bank];
            }
        }
    }

    for( i = 0; i < tableP->count; ++i )
    {
        wordP = tableP->entriesPP[i];

        if( !inGraph(wordP) )
        {
            continue;
        }

        for( edgeP = wordP->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( (edgeP->role == OPTR_WRITE) && (edgeP->flags & OPTEF_INDIRECT) && (edgeP->flags & OPTEF_UNKNOWN) )
            {
                ++passP->unknownWrites[wordP->bank];
            }
        }
    }
}

// Print the scratch dump (-O=scratch).  The format:
//
//   scratch: ...                         what the dump is, and its rules
//   unit uN NAME routines R blocks B words W pool P ... | population N
//   word bBANK ADDR NAME CLASS uUNIT rREADS wWRITES blocks N [flags] | why
//   color uUNIT bBANK: pooled N, occupancy M, colors C
//   labels bBANK: ...                    the population's bookkeeping
//   classes bBANK: ...                   the five classes and the rest
//   prize bBANK POOL: ...                prizes A and B
//   total, blind and reconcile lines
//
// Every word of the population gets exactly one word line, and the class
// counts of each bank add up to its population; the reconcile line says so.
void
optDumpScratch(FILE *fP, OptTableP tableP)
{
SPass pass;
SWord *swP;
SUnit *unitP;
OptWordP wordP;
const char *fileP;
int i;
int bank;
int pool;
int cls;
int sum;
int reconciled;
int total[SC_COUNT];
int totalPopulation;
int totalLabelled;
int totalLocal;
int totalK;
int totalPooled;
int totalLo;
int totalHi;
int totalOnce;
int totalHeldOpen;
int totalHeldBoth;
int totalAdjacent;
int totalAdjacentPooled;
int totalAbove;
int totalUnknownWrites;
int totalOpenFlagged;
int totalByRoutine;
int totalByRoutinePooled;
int totalByRoutineLocal;
int byRoutine;
int wrongLocal;

    buildScratchPass(&pass, tableP);

    fprintf(fP, "scratch: the population is every word carrying a local label, less return words, text and code\n");
    fprintf(fP, "scratch: classes in order: taken(3) unplaced unused multi(4) open(5) both dead sink state call(2) local(1) span\n");
    fprintf(fP, "scratch: local is the block-local test; once is class 1 inside it, one write then one read\n");
    fprintf(fP, "scratch: prize A = local - K (widest overlap); prize B pools dead + local + span, need = neediest unit\n");
    fprintf(fP, "scratch: devices that write memory (drum, DCS2, high-speed channel) are invisible: a limit, not a count\n");

    // the units that hold a word
    for( i = 0; i < pass.unitCount; ++i )
    {
        unitP = &pass.unitsP[i];

        if( !unitP->words )
        {
            continue;
        }

        fprintf(fP, "unit u%d %s routines %d blocks %d words %d pool %s%s%s%s | population %d\n", unitP->id,
            unitName(unitP), unitP->routineCount, unitP->blockCount, unitP->posCount, reachName(unitP->reach),
            (unitP->flagged)?" flagged":"", (unitP->derived)?" derived":"",
            (unitP->residual)?" residual":"", unitP->words);
    }

    // the words; buildScratchPass() took their tallies
    for( i = 0; i < pass.wordCount; ++i )
    {
        swP = &pass.wordsP[i];
        wordP = swP->wordP;
        byRoutine = swP->byRoutine;
        fileP = (wordP->fileP)?wordP->fileP:"-";

        if( strrchr(fileP, '/') )
        {
            fileP = (strrchr(fileP, '/') + 1);
        }

        fprintf(fP, "word b%d %04o %-16s %-8s u%-3d r%d w%d blocks %d%s%s%s | %s | %s:%d\n", wordP->bank, wordP->addr,
            swP->nameP, classInfo[swP->cls].nameP, (swP->unit >= 0)?(swP->unit + 1):0, swP->readRefs,
            swP->writeRefs, swP->blockCount, (swP->onWord)?"":" ownline",
            (swP->adjTaken == 2)?" above-taken":(swP->adjTaken)?" near-taken":"", (byRoutine)?" routines":"",
            swP->why, fileP, wordP->lineNo);
    }

    measureLocal(&pass);
    measurePooled(&pass, fP);

    // the tallies
    memset(total, 0, sizeof(total));
    reconciled = 1;
    totalPopulation = 0;
    totalLabelled = 0;
    totalLocal = 0;
    totalK = 0;
    totalPooled = 0;
    totalLo = 0;
    totalHi = 0;
    totalOnce = 0;
    totalHeldOpen = 0;
    totalHeldBoth = 0;
    totalAdjacent = 0;
    totalAdjacentPooled = 0;
    totalAbove = 0;
    totalUnknownWrites = 0;
    totalOpenFlagged = 0;
    totalByRoutine = 0;
    totalByRoutinePooled = 0;
    totalByRoutineLocal = 0;
    wrongLocal = 0;

    for( i = 0; i < pass.wordCount; ++i )
    {
        swP = &pass.wordsP[i];

        if( swP->local && ((swP->cls == SC_STATE) || (swP->cls == SC_CALL) || (swP->cls == SC_SPAN)) )
        {
            ++wrongLocal;
        }
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !pass.labelled[bank] && !pass.unknownWrites[bank] )
        {
            continue;
        }

        fprintf(fP, "labels b%d: %d = returns %d + text %d + code %d + population %d (label on its word %d, on the line above %d)\n",
            bank, pass.labelled[bank], pass.returns[bank], pass.text[bank], pass.code[bank], pass.population[bank],
            pass.onWord[bank], (pass.population[bank] - pass.onWord[bank]));

        if( pass.labelled[bank] != (pass.returns[bank] + pass.text[bank] + pass.code[bank] + pass.population[bank]) )
        {
            reconciled = 0;
            fprintf(fP, "reconcile: bank %d labels do not add up\n", bank);
        }

        sum = 0;

        for( cls = 0; cls < SC_COUNT; ++cls )
        {
            sum += pass.classCount[bank][cls];
            total[cls] += pass.classCount[bank][cls];
        }

        fprintf(fP, "classes b%d: %d = local(1) %d [once %d] + call(2) %d + taken(3) %d + multi(4) %d + open(5) %d"
            " + unclassified %d [unplaced %d, unused %d, both %d, dead %d, sink %d, state %d, span %d]\n",
            bank, pass.population[bank], pass.classCount[bank][SC_LOCAL], pass.once[bank], pass.classCount[bank][SC_CALL],
            pass.classCount[bank][SC_TAKEN], pass.classCount[bank][SC_MULTI], pass.classCount[bank][SC_OPEN],
            (pass.classCount[bank][SC_UNPLACED] + pass.classCount[bank][SC_UNUSED] + pass.classCount[bank][SC_BOTH]
             + pass.classCount[bank][SC_DEAD] + pass.classCount[bank][SC_SINK] + pass.classCount[bank][SC_STATE]
             + pass.classCount[bank][SC_SPAN]),
            pass.classCount[bank][SC_UNPLACED], pass.classCount[bank][SC_UNUSED], pass.classCount[bank][SC_BOTH],
            pass.classCount[bank][SC_DEAD], pass.classCount[bank][SC_SINK], pass.classCount[bank][SC_STATE],
            pass.classCount[bank][SC_SPAN]);

        if( sum != pass.population[bank] )
        {
            reconciled = 0;
            fprintf(fP, "reconcile: bank %d classes %d, population %d\n", bank, sum, pass.population[bank]);
        }

        for( pool = 0; pool < SPOOL_COUNT; ++pool )
        {
            if( !pass.localPool[bank][pool] && !pass.pooled[bank][pool] )
            {
                continue;
            }

            fprintf(fP, "prize b%d %s: local %d K %d A %d | pooled %d need %d..%d B %d..%d\n", bank,
                (pool == SPOOL_MAIN)?"main":"handler", pass.localPool[bank][pool], pass.kPool[bank][pool],
                (pass.localPool[bank][pool] - pass.kPool[bank][pool]), pass.pooled[bank][pool],
                pass.needLo[bank][pool], pass.needHi[bank][pool],
                (pass.pooled[bank][pool] - pass.needHi[bank][pool]), (pass.pooled[bank][pool] - pass.needLo[bank][pool]));

            if( (pass.kPool[bank][pool] > pass.localPool[bank][pool]) || (pass.needLo[bank][pool] > pass.needHi[bank][pool])
                || (pass.needHi[bank][pool] > pass.pooled[bank][pool]) )
            {
                reconciled = 0;
                fprintf(fP, "reconcile: bank %d pool %d bounds out of order\n", bank, pool);
            }

            totalLocal += pass.localPool[bank][pool];
            totalK += pass.kPool[bank][pool];
            totalPooled += pass.pooled[bank][pool];
            totalLo += pass.needLo[bank][pool];
            totalHi += pass.needHi[bank][pool];
        }

        fprintf(fP, "held b%d: local words class 5 holds back %d, both lines %d; open by the routine flags %d of %d;"
            " multi(4) by routine, not unit, would add %d (pooled %d, local %d)\n", bank,
            pass.heldOpen[bank], pass.heldBoth[bank], pass.openFlagged[bank], pass.classCount[bank][SC_OPEN],
            pass.byRoutine[bank], pass.byRoutinePooled[bank], pass.byRoutineLocal[bank]);
        fprintf(fP, "blind b%d: taken(3) by sitting above a taken record base %d; in a run with only a taken string"
            " or table below or any taken word above %d (pooled %d); unknown indirect writes %d\n", bank,
            pass.above[bank], pass.adjacent[bank], pass.adjacentPooled[bank], pass.unknownWrites[bank]);

        totalPopulation += pass.population[bank];
        totalLabelled += pass.labelled[bank];
        totalOnce += pass.once[bank];
        totalHeldOpen += pass.heldOpen[bank];
        totalHeldBoth += pass.heldBoth[bank];
        totalAdjacent += pass.adjacent[bank];
        totalAdjacentPooled += pass.adjacentPooled[bank];
        totalAbove += pass.above[bank];
        totalUnknownWrites += pass.unknownWrites[bank];
        totalOpenFlagged += pass.openFlagged[bank];
        totalByRoutine += pass.byRoutine[bank];
        totalByRoutinePooled += pass.byRoutinePooled[bank];
        totalByRoutineLocal += pass.byRoutineLocal[bank];
    }

    fprintf(fP, "total: labels %d, population %d; local(1) %d [once %d], call(2) %d, taken(3) %d, multi(4) %d, open(5) %d;"
        " unplaced %d, unused %d, both %d, dead %d, sink %d, state %d, span %d\n", totalLabelled, totalPopulation,
        total[SC_LOCAL], totalOnce, total[SC_CALL], total[SC_TAKEN], total[SC_MULTI], total[SC_OPEN], total[SC_UNPLACED],
        total[SC_UNUSED], total[SC_BOTH], total[SC_DEAD], total[SC_SINK], total[SC_STATE], total[SC_SPAN]);
    fprintf(fP, "total: prize A %d (local %d, K %d); prize B %d..%d (pooled %d, need %d..%d); unused %d\n",
        (totalLocal - totalK), totalLocal, totalK, (totalPooled - totalHi), (totalPooled - totalLo), totalPooled,
        totalLo, totalHi, total[SC_UNUSED]);
    fprintf(fP, "blind: taken(3) by sitting above a taken record base %d; in a run with only a taken string or table"
        " below or any taken word above %d (pooled %d); unknown indirect writes %d; sink-dependent %d;"
        " pooled words in a unit with a residual sink %d\n", totalAbove, totalAdjacent, totalAdjacentPooled,
        totalUnknownWrites, total[SC_SINK], pass.residualWords);
    fprintf(fP, "held: local words held back by class 5 %d, by both lines %d; open by the routine flags %d of %d;"
        " multi(4) by routine, not unit, would add %d (pooled %d, local %d)\n", totalHeldOpen, totalHeldBoth,
        totalOpenFlagged, total[SC_OPEN], totalByRoutine, totalByRoutinePooled, totalByRoutineLocal);
    sum = 0;

    for( i = 0; i < pass.unitCount; ++i )
    {
        if( pass.unitsP[i].words )
        {
            ++sum;
        }
    }

    fprintf(fP, "units: %d, holding a placed word %d\n", pass.unitCount, sum);

    // the reconciliation
    sum = 0;

    for( cls = 0; cls < SC_COUNT; ++cls )
    {
        sum += total[cls];
    }

    if( sum != totalPopulation )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: classes %d, population %d\n", sum, totalPopulation);
    }

    if( totalPopulation != pass.wordCount )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: population %d, words %d\n", totalPopulation, pass.wordCount);
    }

    if( pass.mismatches )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: %d occupancy walks disagree with the liveness solve\n", pass.mismatches);
    }

    if( wrongLocal )
    {
        reconciled = 0;
        fprintf(fP, "reconcile: %d words pass the local test and came out live across a boundary\n", wrongLocal);
    }

    fprintf(fP, "reconcile: %s\n", (reconciled)?"ok":"FAILED");
    freePass(&pass);
}

// The report section.

// Is this a pooled word the analysis cannot promise?  The same test as
// buildScratchPass()'s adjacentPooled tally, so the two cannot drift apart.
// Returns 1 when it is pooled and sits in a data run holding a taken word, 0 when not.
static int
nearTaken(SWord *swP)
{
    return( (swP->cls != SC_TAKEN) && (swP->adjTaken == 1) && classInfo[swP->cls].pooled );
}

// The pool a unit runs in, which poolOf() gives for each of its words.
// Returns SPOOL_MAIN or SPOOL_HANDLER.
static int
poolOfUnit(SPass *passP, int u)
{
    return( (passP->unitsP[u].reach & SREACH_MAIN)?SPOOL_MAIN:SPOOL_HANDLER );
}

// Does this word belong to one bank, pool and color of the pooled assignment?
// Returns 1 when it does, 0 when not.
static int
inSlot(SPass *passP, SWord *swP, int bank, int pool, int color)
{
    if( (swP->unit < 0) || !classInfo[swP->cls].pooled || (swP->wordP->bank != bank) )
    {
        return(0);
    }

    return( (swP->color == color) && (poolOf(passP, swP) == pool) );
}

// The words a unit holds in one bank of the pooled assignment, and the colors
// they take.
// Returns the count, and sets *colorsP to the colors when colorsP is not NILP.
static int
unitPooled(SPass *passP, int u, int bank, int *colorsP)
{
SWord *swP;
int count;
int colors;
int i;

    count = 0;
    colors = 0;

    for( i = 0; i < passP->wordCount; ++i )
    {
        swP = &passP->wordsP[i];

        if( (swP->unit != u) || !classInfo[swP->cls].pooled || (swP->wordP->bank != bank) )
        {
            continue;
        }

        ++count;

        if( (swP->color + 1) > colors )
        {
            colors = (swP->color + 1);
        }
    }

    if( colorsP )
    {
        *colorsP = colors;
    }

    return(count);
}

// Write the report's advisory scratch-word sharing section: its conditions,
// the block-local class first (a reader can check it by eye), the pooled
// assignment, the words not promised, the refused call class, and the cost
// of collecting.  Its pass is the dump's buildScratchPass(), so every figure
// matches the dump's.
void
writeScratchSharingReport(FILE *fP, OptTableP tableP)
{
SPass pass;
SWord *swP;
SUnit *unitP;
int bank;
int pool;
int color;
int u;
int i;
int any;
int population;
int pooledWords;
int localWords;
int localNeed;
int needLo;
int needHi;
int freed;
int mostFreed;
int marked;
int callWords;
int units;
int words;
int colors;
int keepAddr;

    buildScratchPass(&pass, tableP);
    measureLocal(&pass);
    measurePooled(&pass, NILP);

    population = 0;
    pooledWords = 0;
    localWords = 0;
    localNeed = 0;
    needLo = 0;
    needHi = 0;
    marked = 0;
    callWords = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        population += pass.population[bank];
        marked += pass.adjacentPooled[bank];
        callWords += pass.classCount[bank][SC_CALL];

        for( pool = 0; pool < SPOOL_COUNT; ++pool )
        {
            pooledWords += pass.pooled[bank][pool];
            localWords += pass.localPool[bank][pool];
            localNeed += pass.kPool[bank][pool];
            needLo += pass.needLo[bank][pool];
            needHi += pass.needHi[bank][pool];
        }
    }

    freed = (pooledWords - needHi);
    mostFreed = (pooledWords - needLo);

    fprintf(fP, "\nScratch-word sharing: %d word%s could be freed, from a population of %d\n",
        freed, (freed == 1)?"":"s", population);

    if( !population )
    {
        fprintf(fP, "  No word here carries a local label, so there is nothing to share.\n");
        freePass(&pass);
        return;
    }

    // what the section is, and the conditions on it

    fprintf(fP, "  ADVISORY ONLY, and permanently so.  This is not a finding, it is not counted\n");
    fprintf(fP, "  in any finding total, and no flag makes am1 act on it.  Two storage words can\n");
    fprintf(fP, "  share one location when they are never live at the same time.  Where that is\n");
    fprintf(fP, "  wrong, the second write destroys a value the first word's reader still needs,\n");
    fprintf(fP, "  and the program computes a wrong answer on that path.  That failure is an\n");
    fprintf(fP, "  ordinary bug a test can drive, unlike a wrongly shared return word -- but it\n");
    fprintf(fP, "  is yours to find, not am1's, so read the words below before changing one.\n");
    fprintf(fP, "  A word is shared only inside its own bank, since it is reached by an ordinary\n");
    fprintf(fP, "  memory reference, and only inside its own pool: a sequence-break handler runs\n");
    fprintf(fP, "  between any two words of what it interrupted, so the code a handler reaches is\n");
    fprintf(fP, "  a second line of control with its own supply.\n");
    fprintf(fP, "  The population is every word carrying a local label, less the routines' return\n");
    fprintf(fP, "  words, the words of a text string, and words that are code: %d here.  What is\n", population);
    fprintf(fP, "  offered below is only what is private to one unit -- one routine, or a group\n");
    fprintf(fP, "  of routines control passes between without returning -- and never live across\n");
    fprintf(fP, "  a call it makes or on entry to it.  A word whose address is taken, one that\n");
    fprintf(fP, "  two units reference, one in a unit that could not be read in full, one in code\n");
    fprintf(fP, "  both lines run, and one that carries a value between calls are all excluded.\n");
    fprintf(fP, "  A DEVICE THAT WRITES MEMORY ON ITS OWN IS INVISIBLE HERE.  The drum, DCS2 and\n");
    fprintf(fP, "  the high-speed channel write words this analysis never sees written, so a word\n");
    fprintf(fP, "  one of them fills is not known to be live and must not be shared.\n");

    if( mostFreed != freed )
    {
        fprintf(fP, "  %d word%s what the assignment below achieves.  No assignment can free more\n",
            freed, (freed == 1)?" is":"s are");
        fprintf(fP, "  than %d: that is what the occupancy counts put under every unit's need.\n", mostFreed);
    }

    // the block-local class, first and separately

    fprintf(fP, "\n  The block-local class, which needs no liveness: %d word%s\n",
        (localWords - localNeed), ((localWords - localNeed) == 1)?"":"s");
    fprintf(fP, "    Every reference to a word below lies in one block, the first of them a full\n");
    fprintf(fP, "    write, with no xct between the first and the last.  So the word is live only\n");
    fprintf(fP, "    between those two addresses, in straight-line code, and a bank needs as many\n");
    fprintf(fP, "    words as the widest overlap of those intervals.  That is the whole proof,\n");
    fprintf(fP, "    and every word below can be checked against the source by eye.  A line gives\n");
    fprintf(fP, "    the word's own bank and address, the addresses it is live between in the bank\n");
    fprintf(fP, "    its references lie in, and the unit that holds it.\n");
    any = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( pool = 0; pool < SPOOL_COUNT; ++pool )
        {
            if( !pass.localPool[bank][pool] )
            {
                continue;
            }

            any = 1;

            fprintf(fP, "    bank %2d%s: %d word%s need %d, freeing %d\n", bank,
                (pool == SPOOL_HANDLER)?", sequence-break pool":"", pass.localPool[bank][pool],
                (pass.localPool[bank][pool] == 1)?"":"s", pass.kPool[bank][pool],
                (pass.localPool[bank][pool] - pass.kPool[bank][pool]));

            for( i = 0; i < pass.wordCount; ++i )
            {
                swP = &pass.wordsP[i];

                if( (swP->cls != SC_LOCAL) || (swP->wordP->bank != bank) || (poolOf(&pass, swP) != pool) )
                {
                    continue;
                }

                fprintf(fP, "      b%d %04o  %-16s %-11s live %04o-%04o in bank %d, unit u%d %s\n",
                    swP->wordP->bank, swP->wordP->addr, swP->nameP, (nearTaken(swP))?"near-taken":"",
                    swP->firstAddr, swP->lastAddr, swP->codeBank, pass.unitsP[swP->unit].id,
                    unitName(&pass.unitsP[swP->unit]));
            }
        }
    }

    if( !any )
    {
        fprintf(fP, "    No word here is block-local, so this cut recovers nothing.\n");
    }

    // the pooled assignment, per bank and per unit

    fprintf(fP, "\n  The pooled assignment, per bank and per unit\n");
    fprintf(fP, "    Two words below that are in different units are never live together, since\n");
    fprintf(fP, "    neither is live across a call or on entry to its unit and control is in one\n");
    fprintf(fP, "    unit at a time.  So a bank needs only as many words as its neediest unit.\n");
    fprintf(fP, "    Inside a unit the words are colored over their occupancy under pessimistic\n");
    fprintf(fP, "    liveness, which makes the count of words an assignment that exists and not\n");
    fprintf(fP, "    an estimate.  Every shared word below names the units that would share it,\n");
    fprintf(fP, "    and the address to keep is the lowest of its group.  A line gives the word's\n");
    fprintf(fP, "    bank and address, the class it was put in, whether it is marked, and its\n");
    fprintf(fP, "    unit.\n");

    if( !pooledWords )
    {
        fprintf(fP, "    No word here can be pooled, so this recovers nothing.\n");
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( pool = 0; pool < SPOOL_COUNT; ++pool )
        {
            if( !pass.pooled[bank][pool] )
            {
                continue;
            }

            units = 0;

            for( u = 0; u < pass.unitCount; ++u )
            {
                if( (poolOfUnit(&pass, u) == pool) && unitPooled(&pass, u, bank, NILP) )
                {
                    ++units;
                }
            }

            fprintf(fP, "    bank %2d%s: %d word%s in %d unit%s need %d, freeing %d\n", bank,
                (pool == SPOOL_HANDLER)?", sequence-break pool":"", pass.pooled[bank][pool],
                (pass.pooled[bank][pool] == 1)?"":"s", units, (units == 1)?"":"s",
                pass.needHi[bank][pool], (pass.pooled[bank][pool] - pass.needHi[bank][pool]));

            if( pass.needLo[bank][pool] != pass.needHi[bank][pool] )
            {
                fprintf(fP, "      no assignment of this bank can need fewer than %d or free more than %d\n",
                    pass.needLo[bank][pool], (pass.pooled[bank][pool] - pass.needLo[bank][pool]));
            }

            for( u = 0; u < pass.unitCount; ++u )
            {
                if( (poolOfUnit(&pass, u) != pool) || !(words = unitPooled(&pass, u, bank, &colors)) )
                {
                    continue;
                }

                unitP = &pass.unitsP[u];

                fprintf(fP, "      u%-3d %-16s %d word%s, %d needed%s%s\n", unitP->id, unitName(unitP), words,
                    (words == 1)?"":"s", colors, (unitP->derived)?"  derived":"",
                    (unitP->residual)?"  residual sink":"");
            }

            for( color = 0; color < pass.needHi[bank][pool]; ++color )
            {
                units = 0;
                words = 0;
                keepAddr = -1;

                for( i = 0; i < pass.wordCount; ++i )
                {
                    swP = &pass.wordsP[i];

                    if( !inSlot(&pass, swP, bank, pool, color) )
                    {
                        continue;
                    }

                    ++words;

                    if( keepAddr < 0 )
                    {
                        keepAddr = swP->wordP->addr;
                    }
                }

                for( u = 0; u < pass.unitCount; ++u )
                {
                    for( i = 0; i < pass.wordCount; ++i )
                    {
                        if( (pass.wordsP[i].unit == u) && inSlot(&pass, &pass.wordsP[i], bank, pool, color) )
                        {
                            ++units;
                            break;
                        }
                    }
                }

                fprintf(fP, "      shared word %d of %d, keep %04o: %d unit%s, %d word%s, %d freed\n",
                    (color + 1), pass.needHi[bank][pool], keepAddr, units, (units == 1)?"":"s",
                    words, (words == 1)?"":"s", (words - 1));

                for( u = 0; u < pass.unitCount; ++u )
                {
                    for( i = 0; i < pass.wordCount; ++i )
                    {
                        swP = &pass.wordsP[i];

                        if( (swP->unit != u) || !inSlot(&pass, swP, bank, pool, color) )
                        {
                            continue;
                        }

                        fprintf(fP, "        b%d %04o  %-16s %-6s %-11s unit u%d %s\n", swP->wordP->bank,
                            swP->wordP->addr, swP->nameP, classInfo[swP->cls].nameP,
                            (nearTaken(swP))?"near-taken":"", pass.unitsP[u].id, unitName(&pass.unitsP[u]));
                    }
                }
            }
        }
    }

    // the words that are not promised

    if( marked )
    {
        fprintf(fP, "\n  NOT PROMISED: %d of the words above sit in a data run beside a taken word\n", marked);
        fprintf(fP, "    A word marked \"near-taken\" sits in a run of data words where some word's\n");
        fprintf(fP, "    address is taken.  What pointer arithmetic from that address reaches is\n");
        fprintf(fP, "    not something am1 can see, so it cannot prove that nothing reads the\n");
        fprintf(fP, "    marked word through it.  CONFIRM EACH ONE AGAINST THE SOURCE before you\n");
        fprintf(fP, "    count it.  Without every one of them the figure above is %d word%s, and\n",
            ((freed - marked) > 0)?(freed - marked):0, ((freed - marked) == 1)?"":"s");
        fprintf(fP, "    that is a floor: a marked word that is the one kept costs nothing to drop.\n");
    }
    else
    {
        fprintf(fP, "\n  NOT PROMISED: none.  No word above sits in a data run beside a taken word.\n");
        fprintf(fP, "    Every word of the figure above is offered without a condition on it.\n");
    }

    // the class that is refused

    fprintf(fP, "\n  The call class, refused: %d word%s\n", callWords, (callWords == 1)?"":"s");
    fprintf(fP, "    A word live across a call its unit makes is not offered here, and will not\n");
    fprintf(fP, "    be.  The callee, or anything the callee calls, may use the word it was\n");
    fprintf(fP, "    given: that is T10's failure mode in data form, and ruling it out soundly\n");
    fprintf(fP, "    needs the interprocedural liveness this analysis was chosen to avoid.  The\n");
    fprintf(fP, "    number is printed so that what is refused is known and not merely missing.\n");

    // what the author would have to do to collect it

    fprintf(fP, "\n  What this would cost to collect\n");
    fprintf(fP, "    A FREED WORD IS NOT A COLLECTED WORD.  Sharing turns N words into one and\n");
    fprintf(fP, "    frees N-1 -- but those are scattered addresses among the program's storage,\n");
    fprintf(fP, "    and on a hand-laid source the space becomes usable only when the author\n");
    fprintf(fP, "    repacks the bank, or relayout does it.  The figure above is what a layout\n");
    fprintf(fP, "    could collect, not what it collects by itself.\n");
    fprintf(fP, "    THE EDIT IS NOT LOCAL.  Every \"dac w\", \"lac w\", \"idx w\" and \"dzm w\" naming\n");
    fprintf(fP, "    a word that goes away has to name the word that is kept, which is why the\n");
    fprintf(fP, "    kept address is named above.\n");
    freePass(&pass);
}
