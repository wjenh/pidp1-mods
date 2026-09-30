/*
 * The am1 optimizer's return-word sharing proposal (T10): an advisory report
 * naming, for each return word that could be shared, the routines that would
 * share it, the address to keep and the addresses freed.
 *
 * T10 is always advisory.  A wrongly shared return word fails as a wild jump
 * at run time, only under the one call ordering that overlaps, with no trap to
 * catch it.  So nothing here touches findingsP, suppressedP or findingCounts[],
 * and nothing is reachable from optRunRules() or any -O level.
 *
 * optBuildSharing() runs after optBuildCallGraph(), which fills the call graph,
 * its transitive closure (optRoutineReaches()) and the per-bank tallies.  The
 * groups are released by freeShareGroups() from optFreeTable().
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

// Two return words interfere exactly when their routines can be on the call
// stack together, which is reachability in the call graph.  Coloring each
// routine by its longest-path depth gives the minimum number of words.  Four
// refinements keep the proposal safe:
//
//   1. The color is per bank and per pool.  A return word is reached by an
//      ordinary memory reference, so it can only be shared inside its own
//      bank; a sequence-break handler runs between any two words of what it
//      interrupted, so the routines it reaches are a second stack colored from
//      their own supply.
//   2. Each member of a strongly connected component gets its own word: the
//      members may all be live at once, so they take a block of consecutive
//      colors.
//   3. Two entries into one body name one word; it is shared only when every
//      namer has the same color, otherwise it is split into a group of its own.
//   4. A jda routine is not a candidate: its return word is the word below its
//      entry, fixed by where the routine sits.  It is excluded and counted.
//
// Every group is also checked pairwise with optRoutineReaches() rather than
// trusting the coloring.

// The per-sweep working state for one (bank, pool).
typedef struct
{
    int *depthP;        // per component, the longest chain of qualifying routines
                        // it heads (chainDepth()'s recurrence in optcallgraph.c)
    int *bestP;         // per component, the best chain strictly below it, which
                        // is where a component's block of colors starts
    int *colorP;        // per routine id, its color, 0 when it does not qualify
} ShSweep, *ShSweepP;

// One distinct return word inside one (bank, pool), while the groups are built.
typedef struct
{
    int addr;           // the word's address within the bank
    int color;          // the color every namer agrees on, 0 when they do not
    int split;          // set when they do not: excluded from sharing, its own group
    int unknown;        // a namer is open, jumpsunknown or callsunknown
    int leaf;           // every namer is a leaf
    int cyclic;         // a namer is on a cycle of resolved call arcs
} ShWord, *ShWordP;

static int qualifies(OptRoutineP routineP, int bank, int pool);
static int shareCandidate(OptRoutineP routineP);
static int shareLeaf(OptRoutineP routineP);
static int shareUnknown(OptRoutineP routineP);
static int colorSweep(OptTableP tableP, ShSweepP sweepP, int bank, int pool);
static void buildBankPool(OptTableP tableP, ShSweepP sweepP, int bank, int pool, int depth);
static OptShareGroupP newGroup(OptTableP tableP, int bank, int pool, int color);
static void fillGroup(OptTableP tableP, OptShareGroupP groupP, ShSweepP sweepP,
    ShWordP wordsP, int wordCount, int bank, int pool);
static void checkGroup(OptTableP tableP, OptShareGroupP groupP);
static void countJdaWords(OptTableP tableP);
static const char *addrName(OptTableP tableP, int bank, int addr);
static const char *routineName(OptRoutineP routineP);
static int leafKeepAddr(OptTableP tableP, int bank, int pool);
static int leafWords(OptTableP tableP, int bank, int pool);
static int groupsInBank(OptTableP tableP, int bank);
static int colorOfRoutine(OptTableP tableP, OptRoutineP routineP, int *groupNoP);

// Test whether a routine's shareable return word is in this bank (the word's
// bank, not the entry's) and this sequence-break pool.  chainDepth()'s test
// plus the jda exclusion.  Returns 1 when it qualifies, 0 otherwise.
static int
qualifies(OptRoutineP routineP, int bank, int pool)
{
    if( !shareCandidate(routineP) )
    {
        return(0);
    }

    if( routineP->returnBank != bank )
    {
        return(0);
    }

    if( pool != ((routineP->flags & OPTRT_SBSPOOL)?1:0) )
    {
        return(0);
    }

    return(1);
}

// Test whether a routine has a return word that could be shared: not one with
// none, and not jda, whose word is fixed below the entry.  A routine that
// escapes still counts.  Returns 1 for a candidate, 0 otherwise.
static int
shareCandidate(OptRoutineP routineP)
{
    if( !routineP || (routineP->returnAddr < 0) || (routineP->returnBank < 0) )
    {
        return(0);
    }

    if( routineP->returnWord == OPTRW_JDA )
    {
        return(0);
    }

    return(1);
}

// Test for a leaf: no call arc and no unfollowed indirect jump.  Must match
// isLeafRoutine() in optcallgraph.c so the leaf cut agrees with leafSaving.
// Returns 1 for a leaf, 0 otherwise.
static int
shareLeaf(OptRoutineP routineP)
{
    if( !routineP )
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

// Test whether the call graph does not fully know a routine: an open body, an
// unfollowed indirect jump, or a call to the unknown sink.  Interference for
// these may be understated, so the report marks them.  Returns 1 if so, else 0.
static int
shareUnknown(OptRoutineP routineP)
{
    if( !routineP )
    {
        return(0);
    }

    if( routineP->flags & (OPTRT_OPENBODY | OPTRT_JUMPSUNKNOWN | OPTRT_CALLSUNKNOWN) )
    {
        return(1);
    }

    return(0);
}

// Color every routine that qualifies for one (bank, pool), each component's
// members taking a block of consecutive colors.  Tarjan's numbering is reverse
// topological, so ascending order finishes every callee before its callers.
// Chains follow the whole graph and only the counting is restricted: a bank 0
// to bank 2 to bank 0 call puts two bank 0 words on the stack at once.
// Returns the longest chain (the number of colors), 0 when nothing qualifies.
static int
colorSweep(OptTableP tableP, ShSweepP sweepP, int bank, int pool)
{
OptRoutineP routineP;
OptCallArcP arcP;
int scc;
int own;
int best;
int deepest;
int next;

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

            if( qualifies(routineP, bank, pool) )
            {
                ++own;
            }

            for( arcP = routineP->callsP; arcP; arcP = arcP->nextP )
            {
                // An arc inside the component is already counted in 'own'.
                if( arcP->toP->scc == scc )
                {
                    continue;
                }

                if( sweepP->depthP[arcP->toP->scc] > best )
                {
                    best = sweepP->depthP[arcP->toP->scc];
                }
            }
        }

        sweepP->bestP[scc] = best;
        sweepP->depthP[scc] = (own + best);

        if( sweepP->depthP[scc] > deepest )
        {
            deepest = sweepP->depthP[scc];
        }
    }

    // Assign the colors.  List order (bank, then entry address) keeps the
    // assignment stable from run to run.
    for( scc = 1; scc <= tableP->sccCount; ++scc )
    {
        next = (sweepP->bestP[scc] + 1);

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( (routineP->scc != scc) || !qualifies(routineP, bank, pool) )
            {
                continue;
            }

            sweepP->colorP[routineP->id] = next;
            ++next;
        }
    }

    return(deepest);
}

// Build the groups for one colored (bank, pool), per distinct return word.  A
// word whose namers disagree on color is split into a group of its own and
// counted, so no word gets two colors and no entry is dropped.
static void
buildBankPool(OptTableP tableP, ShSweepP sweepP, int bank, int pool, int depth)
{
OptRoutineP routineP;
OptShareGroupP groupP;
ShWordP wordsP;
int wordCount;
int color;
int used;
int freed;
int i;

    if( !(wordsP = (ShWordP)calloc((size_t)(tableP->routineCount + 1), sizeof(ShWord))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's sharing groups\n");
        exit(1);
    }

    wordCount = 0;

    // Collect the distinct words in routine list order.
    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !qualifies(routineP, bank, pool) )
        {
            continue;
        }

        for( i = 0; i < wordCount; ++i )
        {
            if( wordsP[i].addr == routineP->returnAddr )
            {
                break;
            }
        }

        if( i == wordCount )
        {
            wordsP[i].addr = routineP->returnAddr;
            wordsP[i].color = sweepP->colorP[routineP->id];
            wordsP[i].leaf = 1;
            ++wordCount;
        }
        else if( wordsP[i].color != sweepP->colorP[routineP->id] )
        {
            // Two entries into one body at different depths: not shareable.
            wordsP[i].split = 1;
        }

        if( shareUnknown(routineP) )
        {
            wordsP[i].unknown = 1;
        }

        if( !shareLeaf(routineP) )
        {
            wordsP[i].leaf = 0;
        }

        if( routineP->flags & OPTRT_ONCYCLE )
        {
            wordsP[i].cyclic = 1;
        }
    }

    // One group per color that has an unsplit word, then one per split word.
    for( color = 1; color <= depth; ++color )
    {
        used = 0;

        for( i = 0; i < wordCount; ++i )
        {
            if( !wordsP[i].split && (wordsP[i].color == color) )
            {
                used = 1;
                break;
            }
        }

        if( !used )
        {
            continue;
        }

        groupP = newGroup(tableP, bank, pool, color);
        fillGroup(tableP, groupP, sweepP, wordsP, wordCount, bank, pool);
        checkGroup(tableP, groupP);
    }

    for( i = 0; i < wordCount; ++i )
    {
        if( !wordsP[i].split )
        {
            continue;
        }

        ++tableP->shareSplitWords;

        groupP = newGroup(tableP, bank, pool, wordsP[i].color);
        groupP->splitOut = 1;
        groupP->keepAddr = wordsP[i].addr;
        groupP->wordCount = 1;
        groupP->freedWords = 0;
        groupP->unknownWords = wordsP[i].unknown;
        groupP->leafWords = wordsP[i].leaf;
        groupP->cyclicWords = wordsP[i].cyclic;

        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( qualifies(routineP, bank, pool) && (routineP->returnAddr == wordsP[i].addr) )
            {
                groupP->membersPP[groupP->memberCount] = routineP;
                ++groupP->memberCount;
            }
        }
    }

    // Add what this (bank, pool) freed; the caller compares the bank's total
    // with bankSaving once both pools are swept.
    freed = 0;

    for( groupP = tableP->shareGroupsP; groupP; groupP = groupP->nextP )
    {
        if( (groupP->bank == bank) && (groupP->pool == pool) )
        {
            freed += groupP->freedWords;
        }
    }

    tableP->shareBankFreed[bank] += freed;
    tableP->shareFreedWords += freed;

    free(wordsP);
}

// Make an empty group with room for every routine and append it to the list.
// Returns the new group, never NILP: out of memory exits.
static OptShareGroupP
newGroup(OptTableP tableP, int bank, int pool, int color)
{
OptShareGroupP groupP;

    if( !(groupP = (OptShareGroupP)calloc((size_t)1, sizeof(OptShareGroup))) ||
        !(groupP->membersPP = (OptRoutineP *)calloc((size_t)(tableP->routineCount + 1),
            sizeof(OptRoutineP))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer's sharing groups\n");
        exit(1);
    }

    groupP->bank = bank;
    groupP->pool = pool;
    groupP->color = color;
    groupP->keepAddr = -1;

    if( tableP->shareGroupsTailP )
    {
        tableP->shareGroupsTailP->nextP = groupP;
    }
    else
    {
        tableP->shareGroupsP = groupP;
    }

    tableP->shareGroupsTailP = groupP;
    ++tableP->shareGroupCount;
    return(groupP);
}

// Fill one color's group with its words, its members and the address to keep,
// which is the lowest so the report is stable from run to run.
static void
fillGroup(OptTableP tableP, OptShareGroupP groupP, ShSweepP sweepP, ShWordP wordsP,
    int wordCount, int bank, int pool)
{
OptRoutineP routineP;
int i;

    for( i = 0; i < wordCount; ++i )
    {
        if( wordsP[i].split || (wordsP[i].color != groupP->color) )
        {
            continue;
        }

        ++groupP->wordCount;

        if( wordsP[i].unknown )
        {
            ++groupP->unknownWords;
            ++tableP->shareUnknownWords;
        }

        if( wordsP[i].leaf )
        {
            ++groupP->leafWords;
        }

        if( wordsP[i].cyclic )
        {
            ++groupP->cyclicWords;
        }

        if( (groupP->keepAddr < 0) || (wordsP[i].addr < groupP->keepAddr) )
        {
            groupP->keepAddr = wordsP[i].addr;
        }
    }

    groupP->freedWords = (groupP->wordCount - 1);

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !qualifies(routineP, bank, pool) ||
            (sweepP->colorP[routineP->id] != groupP->color) )
        {
            continue;
        }

        // A split word's namers belong to the split word's own group.
        for( i = 0; i < wordCount; ++i )
        {
            if( (wordsP[i].addr == routineP->returnAddr) && wordsP[i].split )
            {
                break;
            }
        }

        if( i != wordCount )
        {
            continue;
        }

        groupP->membersPP[groupP->memberCount] = routineP;
        ++groupP->memberCount;
    }
}

// Check that no two members of a group can reach each other, both ways, and
// skip pairs that name the same word.  A failure goes to stderr and marks the
// group so the report withholds it.
static void
checkGroup(OptTableP tableP, OptShareGroupP groupP)
{
OptRoutineP oneP;
OptRoutineP twoP;
int i;
int j;

    for( i = 0; i < groupP->memberCount; ++i )
    {
        oneP = groupP->membersPP[i];

        for( j = (i + 1); j < groupP->memberCount; ++j )
        {
            twoP = groupP->membersPP[j];

            if( oneP->returnAddr == twoP->returnAddr )
            {
                continue;
            }

            if( optRoutineReaches(tableP, oneP, twoP) ||
                optRoutineReaches(tableP, twoP, oneP) )
            {
                fprintf(stderr,
                    "am1: optimizer sharing self-check failed: bank %d pool %d color %d "
                    "proposes one word for %04o (%s) and %04o (%s), and one reaches the other\n",
                    groupP->bank, groupP->pool, groupP->color,
                    oneP->returnAddr, routineName(oneP),
                    twoP->returnAddr, routineName(twoP));

                groupP->checkFailed = 1;
                ++tableP->shareCheckFailures;
            }
        }
    }
}

// Count the distinct jda return words excluded, in total and per bank, so
// candidates plus exclusions reconcile with returnWordCount.
static void
countJdaWords(OptTableP tableP)
{
OptRoutineP routineP;
OptRoutineP otherP;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( (routineP->returnAddr < 0) || (routineP->returnWord != OPTRW_JDA) )
        {
            continue;
        }

        for( otherP = tableP->routinesP; otherP && (otherP != routineP); otherP = otherP->nextP )
        {
            if( (otherP->returnAddr == routineP->returnAddr) &&
                (otherP->returnBank == routineP->returnBank) )
            {
                break;
            }
        }

        if( otherP == routineP )
        {
            ++tableP->shareJdaWords;

            if( (routineP->returnBank >= 0) && (routineP->returnBank <= MAXBANK) )
            {
                ++tableP->shareBankJda[routineP->returnBank];
            }
        }
    }
}

// Name the word at a bank and address, for the report.
// Returns the first label on it, or "-" when it has none or no word was
// emitted there.
static const char *
addrName(OptTableP tableP, int bank, int addr)
{
OptWordP entryP;

    if( (addr < 0) || !(entryP = wordAt(tableP, bank, addr)) )
    {
        return("-");
    }

    return( firstLabelName(entryP) );
}

// Name a routine by the first label on its entry word.
// Returns the label, or "-" when the entry carries none.
static const char *
routineName(OptRoutineP routineP)
{
    if( !routineP || !routineP->entryBlockP || !routineP->entryBlockP->firstP )
    {
        return("-");
    }

    return( firstLabelName(routineP->entryBlockP->firstP) );
}

// The address the leaf cut would keep in one (bank, pool): the lowest return
// word held by a leaf there.
// Returns the address, or -1 when the bank and pool hold no leaf word.
static int
leafKeepAddr(OptTableP tableP, int bank, int pool)
{
OptRoutineP routineP;
int keep;

    keep = -1;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !qualifies(routineP, bank, pool) || !shareLeaf(routineP) )
        {
            continue;
        }

        if( (keep < 0) || (routineP->returnAddr < keep) )
        {
            keep = routineP->returnAddr;
        }
    }

    return(keep);
}

// How many distinct return words the leaves of one (bank, pool) hold.
// Returns the count, 0 when there are none.
static int
leafWords(OptTableP tableP, int bank, int pool)
{
OptRoutineP routineP;
OptRoutineP otherP;
int count;

    count = 0;

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        if( !qualifies(routineP, bank, pool) || !shareLeaf(routineP) )
        {
            continue;
        }

        for( otherP = tableP->routinesP; otherP && (otherP != routineP); otherP = otherP->nextP )
        {
            if( qualifies(otherP, bank, pool) && shareLeaf(otherP) &&
                (otherP->returnAddr == routineP->returnAddr) )
            {
                break;
            }
        }

        if( otherP == routineP )
        {
            ++count;
        }
    }

    return(count);
}

// How many groups the assignment made in one bank, both pools together.
// Returns the count, 0 when the bank has none.
static int
groupsInBank(OptTableP tableP, int bank)
{
OptShareGroupP groupP;
int count;

    count = 0;

    for( groupP = tableP->shareGroupsP; groupP; groupP = groupP->nextP )
    {
        if( groupP->bank == bank )
        {
            ++count;
        }
    }

    return(count);
}

// Find the group a routine is in.  Returns the group's color, or 0 when it is
// in none; *groupNoP gets the group's 1-based position in the list, or 0.
static int
colorOfRoutine(OptTableP tableP, OptRoutineP routineP, int *groupNoP)
{
OptShareGroupP groupP;
int number;
int i;

    number = 0;
    *groupNoP = 0;

    for( groupP = tableP->shareGroupsP; groupP; groupP = groupP->nextP )
    {
        ++number;

        for( i = 0; i < groupP->memberCount; ++i )
        {
            if( groupP->membersPP[i] == routineP )
            {
                *groupNoP = number;
                return( groupP->color );
            }
        }
    }

    return(0);
}

// The entry points.

// Build the return-word sharing proposal.  It creates no finding and must
// never be reachable from any optimization level.
void
optBuildSharing(OptTableP tableP)
{
ShSweep sweep;
int bank;
int pool;
int depth;
int leaves;
int i;

    if( !tableP )
    {
        return;
    }

    tableP->shareGroupsP = NILP;
    tableP->shareGroupsTailP = NILP;
    tableP->shareGroupCount = 0;
    tableP->shareFreedWords = 0;
    tableP->shareLeafFreed = 0;
    tableP->shareUnknownWords = 0;
    tableP->shareSplitWords = 0;
    tableP->shareJdaWords = 0;
    tableP->shareCheckFailures = 0;
    tableP->shareDisagreeBanks = 0;

    for( i = 0; i <= MAXBANK; ++i )
    {
        tableP->shareBankFreed[i] = 0;
        tableP->shareBankJda[i] = 0;
    }

    if( !tableP->routineCount || !tableP->sccCount )
    {
        return;
    }

    countJdaWords(tableP);

    if( !(sweep.depthP = (int *)calloc((size_t)(tableP->sccCount + 1), sizeof(int))) ||
        !(sweep.bestP = (int *)calloc((size_t)(tableP->sccCount + 1), sizeof(int))) ||
        !(sweep.colorP = (int *)calloc((size_t)(tableP->routineCount + 1), sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory coloring the optimizer's return words\n");
        exit(1);
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->bankReturnWords[bank] )
        {
            continue;
        }

        for( pool = 0; pool <= 1; ++pool )
        {
            memset(sweep.colorP, 0, (size_t)(tableP->routineCount + 1) * sizeof(int));
            memset(sweep.depthP, 0, (size_t)(tableP->sccCount + 1) * sizeof(int));
            memset(sweep.bestP, 0, (size_t)(tableP->sccCount + 1) * sizeof(int));

            if( !(depth = colorSweep(tableP, &sweep, bank, pool)) )
            {
                continue;
            }

            buildBankPool(tableP, &sweep, bank, pool, depth);

            // The leaf cut: all leaves here can share one word.
            if( (leaves = leafWords(tableP, bank, pool)) > 1 )
            {
                tableP->shareLeafFreed += (leaves - 1);
            }
        }

        // The groups must free what the call-depth measurement predicted; the
        // report flags a bank where they disagree.
        if( tableP->shareBankFreed[bank] != tableP->bankSaving[bank] )
        {
            ++tableP->shareDisagreeBanks;
        }
    }

    free(sweep.depthP);
    free(sweep.bestP);
    free(sweep.colorP);
}

// Release every group and clear the list; safe when none was built.
void
freeShareGroups(OptTableP tableP)
{
OptShareGroupP groupP;
OptShareGroupP nextP;

    for( groupP = tableP->shareGroupsP; groupP; groupP = nextP )
    {
        nextP = groupP->nextP;

        if( groupP->membersPP )
        {
            free(groupP->membersPP);
        }

        free(groupP);
    }

    tableP->shareGroupsP = NILP;
    tableP->shareGroupsTailP = NILP;
    tableP->shareGroupCount = 0;
}

// Write the report's advisory return-word sharing section: the caveats, the
// leaf cut on its own (checkable by eye), the full per-bank assignment, then
// what collecting the words would take.
void
writeSharingReport(FILE *fP, OptTableP tableP)
{
OptShareGroupP groupP;
OptRoutineP routineP;
int bank;
int pool;
int any;
int keep;
int words;
int i;

    fprintf(fP, "\nReturn-word sharing: %d word%s could be freed, in %d group%s\n",
        tableP->shareFreedWords, (tableP->shareFreedWords == 1)?"":"s",
        tableP->shareGroupCount, (tableP->shareGroupCount == 1)?"":"s");

    if( !tableP->routineCount )
    {
        fprintf(fP, "  No routine was found in this program, so there is nothing to share.\n");
        return;
    }

    fprintf(fP, "  ADVISORY ONLY, and permanently so.  This is not a finding, it is not counted\n");
    fprintf(fP, "  in any finding total, and no flag makes am1 act on it.  Every other\n");
    fprintf(fP, "  suggestion in this report fails visibly when it is wrong.  This one does not:\n");
    fprintf(fP, "  a wrongly shared return word fails as a wild jump at run time, in a routine\n");
    fprintf(fP, "  that works until the one call ordering that overlaps, with no stack and no\n");
    fprintf(fP, "  trap to catch it -- and re-running the old source will not reproduce it,\n");
    fprintf(fP, "  because it depends on an interleaving and not on the code.  Read the routines\n");
    fprintf(fP, "  named below and satisfy yourself before changing anything.\n");
    fprintf(fP, "  Two routines can share one return word when neither can be on the call stack\n");
    fprintf(fP, "  while the other is.  A word is shared only inside its own bank -- it is\n");
    fprintf(fP, "  reached by an ordinary memory reference -- and only inside its own pool: a\n");
    fprintf(fP, "  sequence-break handler runs between any two words of what it interrupted, so\n");
    fprintf(fP, "  the routines a handler can reach are a second stack with their own supply.\n");

    if( tableP->shareJdaWords )
    {
        fprintf(fP, "  %d return word%s excluded because %s the jda form: there the word is the\n",
            tableP->shareJdaWords, (tableP->shareJdaWords == 1)?" is":"s are",
            (tableP->shareJdaWords == 1)?"it is":"they are");
        fprintf(fP, "  one below the routine's entry, at an address fixed by where the routine\n");
        fprintf(fP, "  sits, so there is nothing separable to share.\n");
    }

    if( tableP->shareCheckFailures )
    {
        fprintf(fP, "  WITHHELD: %d group%s failed this analysis's own pairwise check and %s not\n",
            tableP->shareCheckFailures, (tableP->shareCheckFailures == 1)?"":"s",
            (tableP->shareCheckFailures == 1)?"is":"are");
        fprintf(fP, "  printed.  That is a defect in the analysis and not advice about your\n");
        fprintf(fP, "  program; please report it.\n");
    }

    // The leaf cut.

    fprintf(fP, "\n  The leaf cut, which needs no call graph: %d word%s\n",
        tableP->shareLeafFreed, (tableP->shareLeafFreed == 1)?"":"s");
    fprintf(fP, "    A routine whose body contains no call and no indirect jump the analysis\n");
    fprintf(fP, "    could not follow cannot be running while another such routine is running.\n");
    fprintf(fP, "    So every leaf in a bank can share one return word.  That is the whole\n");
    fprintf(fP, "    proof, and every routine below can be checked against the source by eye.\n");

    any = 0;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        for( pool = 0; pool <= 1; ++pool )
        {
            if( (words = leafWords(tableP, bank, pool)) < 2 )
            {
                continue;
            }

            keep = leafKeepAddr(tableP, bank, pool);
            any = 1;

            fprintf(fP, "    bank %2d%s: %d word%s, keep %04o (%s), free the other %d\n",
                bank, (pool)?", sequence-break pool":"", words, (words == 1)?"":"s",
                keep, addrName(tableP, bank, keep), (words - 1));

            for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
            {
                if( !qualifies(routineP, bank, pool) || !shareLeaf(routineP) )
                {
                    continue;
                }

                fprintf(fP, "      %04o  %s (entry %04o)\n", routineP->returnAddr,
                    routineName(routineP), routineP->entryAddr);
            }
        }
    }

    if( !any )
    {
        fprintf(fP, "    No bank holds two leaf routines, so this cut recovers nothing here.\n");
    }

    // The full assignment.

    fprintf(fP, "\n  The full assignment, per bank\n");
    fprintf(fP, "    Every group below is one proposed word.  The routines named in a group were\n");
    fprintf(fP, "    each checked against every other in the group, both ways, through the call\n");
    fprintf(fP, "    graph's transitive closure.  A group marked NOT FULLY KNOWN holds a routine\n");
    fprintf(fP, "    whose body or exits the analysis could not read in full, and those are the\n");
    fprintf(fP, "    ones to look at hardest: they are where this could be wrong in the\n");
    fprintf(fP, "    direction that costs you a return address.\n");

    if( !tableP->shareGroupCount )
    {
        fprintf(fP, "    No routine here holds a return word that could be shared.\n");
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !groupsInBank(tableP, bank) )
        {
            continue;
        }

        fprintf(fP, "    bank %2d: %d return word%s, %d group%s, %d word%s freed\n",
            bank, tableP->bankReturnWords[bank],
            (tableP->bankReturnWords[bank] == 1)?"":"s",
            groupsInBank(tableP, bank), (groupsInBank(tableP, bank) == 1)?"":"s",
            tableP->shareBankFreed[bank], (tableP->shareBankFreed[bank] == 1)?"":"s");

        for( groupP = tableP->shareGroupsP; groupP; groupP = groupP->nextP )
        {
            if( (groupP->bank != bank) || groupP->checkFailed )
            {
                continue;
            }

            if( groupP->splitOut )
            {
                fprintf(fP, "      %04o (%s) shares with nothing: %d entries into one body sit at\n",
                    groupP->keepAddr, addrName(tableP, bank, groupP->keepAddr),
                    groupP->memberCount);
                fprintf(fP, "        different call depths, so neither depth describes the word\n");
            }
            else
            {
                fprintf(fP, "      keep %04o (%s)%s: %d word%s in %d routine%s, %d freed%s\n",
                    groupP->keepAddr, addrName(tableP, bank, groupP->keepAddr),
                    (groupP->pool)?", sequence-break pool":"",
                    groupP->wordCount, (groupP->wordCount == 1)?"":"s",
                    groupP->memberCount, (groupP->memberCount == 1)?"":"s",
                    groupP->freedWords,
                    (groupP->unknownWords)?"   NOT FULLY KNOWN":"");
            }

            for( i = 0; i < groupP->memberCount; ++i )
            {
                routineP = groupP->membersPP[i];

                fprintf(fP, "        %04o  %s (entry %04o)%s%s\n",
                    routineP->returnAddr, routineName(routineP), routineP->entryAddr,
                    (shareUnknown(routineP))?"  not fully known":"",
                    (routineP->flags & OPTRT_ONCYCLE)?"  on a cycle":"");
            }
        }
    }

    // What collecting the words would take.

    fprintf(fP, "\n  What this would cost to collect\n");
    fprintf(fP, "    A FREED WORD IS NOT A COLLECTED WORD.  Sharing turns N return words into\n");
    fprintf(fP, "    one and frees N-1 -- but those are scattered addresses inside routine\n");
    fprintf(fP, "    bodies, and on a hand-laid source the space only becomes usable when the\n");
    fprintf(fP, "    author repacks the bank.  The figure above is what a layout could collect,\n");
    fprintf(fP, "    not what it collects by itself.\n");
    fprintf(fP, "    THE EDIT IS NOT LOCAL.  A shared word cannot stay \"local\" to one routine.\n");
    fprintf(fP, "    Every \"dac rtn\" and every \"jmp i rtn\" in every routine of a group has to\n");
    fprintf(fP, "    name the one surviving address, which is why that address is named above.\n");

    if( tableP->shareUnknownWords )
    {
        fprintf(fP, "    %d of the words proposed here belong to a routine the call graph does not\n",
            tableP->shareUnknownWords);
        fprintf(fP, "    fully know.  Check those against the source first.\n");
    }

    if( tableP->shareSplitWords )
    {
        fprintf(fP, "    %d word%s named by two entries at different call depths, and %s excluded\n",
            tableP->shareSplitWords, (tableP->shareSplitWords == 1)?" is":"s are",
            (tableP->shareSplitWords == 1)?"is":"are");
        fprintf(fP, "    from sharing entirely.\n");
    }

    if( tableP->shareDisagreeBanks )
    {
        fprintf(fP, "    %d bank%s where this assignment frees a different number of words from\n",
            tableP->shareDisagreeBanks, (tableP->shareDisagreeBanks == 1)?"":"s");
        fprintf(fP, "    the call-depth measurement; \"-O=share\" prints both figures side by side.\n");

        if( tableP->shareJdaWords || tableP->shareSplitWords )
        {
            fprintf(fP, "    The measurement counts every return word.  This assignment excludes the\n");
            fprintf(fP, "    jda form and any word two entries name at different depths, neither of\n");
            fprintf(fP, "    which can be shared, which is what the difference is.\n");
        }
        else
        {
            fprintf(fP, "    Nothing here explains that difference, and one of the two is wrong.\n");
            fprintf(fP, "    Please report it.\n");
        }
    }
}

// Print the sharing dump (-O=share): one line per routine, per group and its
// members, then each bank's reconciliation against the call-depth measurement.
void
optDumpShare(FILE *fP, OptTableP tableP)
{
OptShareGroupP groupP;
OptRoutineP routineP;
int bank;
int number;
int group;
int color;
int i;

    fprintf(fP, "share: %d group%s, %d word%s freed, %d not fully known, %d split, %d jda excluded\n",
        tableP->shareGroupCount, (tableP->shareGroupCount == 1)?"":"s",
        tableP->shareFreedWords, (tableP->shareFreedWords == 1)?"":"s",
        tableP->shareUnknownWords, tableP->shareSplitWords, tableP->shareJdaWords);

    for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
    {
        color = colorOfRoutine(tableP, routineP, &group);

        fprintf(fP, "shareroutine %d: %d %04o (%s) return %d %04o color %d pool %d group %d%s%s%s\n",
            routineP->id, routineP->bank, routineP->entryAddr, routineName(routineP),
            (routineP->returnAddr < 0)?-1:routineP->returnBank,
            (routineP->returnAddr < 0)?0:routineP->returnAddr,
            color, (routineP->flags & OPTRT_SBSPOOL)?1:0, group,
            (shareLeaf(routineP))?" leaf":"",
            (shareUnknown(routineP))?" unknown":"",
            (routineP->returnWord == OPTRW_JDA)?" jda":"");
    }

    number = 0;

    for( groupP = tableP->shareGroupsP; groupP; groupP = groupP->nextP )
    {
        ++number;

        fprintf(fP, "sharegroup %d: bank %d pool %d color %d keep %04o (%s) words %d freed %d unknown %d leaf %d cyclic %d%s%s\n",
            number, groupP->bank, groupP->pool, groupP->color, groupP->keepAddr,
            addrName(tableP, groupP->bank, groupP->keepAddr),
            groupP->wordCount, groupP->freedWords, groupP->unknownWords,
            groupP->leafWords, groupP->cyclicWords,
            (groupP->splitOut)?" splitout":"",
            (groupP->checkFailed)?" CHECKFAILED":"");

        for( i = 0; i < groupP->memberCount; ++i )
        {
            fprintf(fP, "sharemember %d: %d %04o (%s) return %04o\n", number,
                groupP->membersPP[i]->bank, groupP->membersPP[i]->entryAddr,
                routineName(groupP->membersPP[i]),
                groupP->membersPP[i]->returnAddr);
        }
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->bankReturnWords[bank] )
        {
            continue;
        }

        fprintf(fP, "sharebank %d: %d words, %d jda excluded, %d groups, assignment frees %d, measurement says %d, %s\n",
            bank, tableP->bankReturnWords[bank], tableP->shareBankJda[bank],
            groupsInBank(tableP, bank), tableP->shareBankFreed[bank],
            tableP->bankSaving[bank],
            (tableP->shareBankFreed[bank] == tableP->bankSaving[bank])?"agree":"DISAGREE");
    }

    fprintf(fP, "shareleaf: %d leaf routines, leaf cut alone frees %d word%s, measurement says %d\n",
        tableP->leafRoutines, tableP->shareLeafFreed,
        (tableP->shareLeafFreed == 1)?"":"s", tableP->leafSaving);
}
