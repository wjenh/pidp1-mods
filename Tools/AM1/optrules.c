/*
 * The am1 optimizer's rules: the F-15D instruction timing table, the shared
 * preconditions P1 to P4, and the twelve rules (T1, T1b, T2, T3, T6, T7,
 * T8a-d, T13, T14).  A rule walks one bank's words in address order and,
 * where its pattern matches, builds a finding: the words, the source it
 * suggests instead, and the saving.  A match that is then refused becomes a
 * suppressed finding carrying its reason.  Nothing here changes a word: the
 * tree, the symbol tables and the tape are what they would be without -O.
 *
 * Called single threaded from optimize() in optimizer.c, after the flow graph
 * is built; findings live in the OptTable of optimizer.h and are freed by
 * optFreeTable().  Allocation failure is fatal, as elsewhere in am1.
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

// Instruction times in microseconds, from the F-15D handbook's "Operating
// Speeds": a memory cycle is 5; a two-cycle memory reference takes 10; jumps,
// augmented instructions and a non-waiting iot take 5; each indirect step adds
// 5.  mul and div use the appendix maxima, 25 and 40, since a static sum
// cannot know whether a divide overflows early.
#define OPTTIME_CYCLE       5       // one memory cycle, and every augmented instruction
#define OPTTIME_MEMREF      10      // two cycles: fetch the instruction, reach the operand
#define OPTTIME_MUL         25      // handbook appendix, "25 max"
#define OPTTIME_DIV         40      // handbook appendix, "40 max"

// Which shared preconditions a rule asks for.  A rule that removes words needs
// all four; one that rewrites a word in place needs only those that say the
// word is what it appears to be.
#define OPTPC_P1            0x01    // no side entry into an interior word
#define OPTPC_P2            0x02    // no word of the pattern written or taken
#define OPTPC_P3            0x04    // no word of the pattern executed by an xct
#define OPTPC_P4            0x08    // no skip-class word immediately before it
#define OPTPC_ALL           0x0F

// Micro-ops that cannot be merged into one copy: two halts are two stops, and
// two complements cancel.
static const OptMicroId optNotIdempotent[3] = { OPTMO_HLT, OPTMO_CMA, OPTMO_CMI };

// The rules, their short names and summaries, in the order the report groups
// findings.
static const struct
{
    const char *nameP;
    const char *summaryP;
} optRuleTable[OPTRULE_COUNT] =
{
    { "T1",  "two consecutive operate words merge into one" },
    { "T1b", "two consecutive shifts of the same opcode merge into one" },
    { "T2",  "skip; jmp .+2; W is the inverted skip and W" },
    { "T3",  "a jmp to a word that is itself a jmp can go straight to the far target" },
    { "T6",  "a cla before an instruction that loads AC outright" },
    { "T7",  "a cli before a lio" },
    { "T8a", "lio of a pool word holding zero is cli" },
    { "T8b", "lac of a pool word holding zero is cla" },
    { "T8c", "lac of a pool word holding all ones is cla cma" },
    { "T8d", "lac of a pool word holding a 12 bit value is a law immediate" },
    { "T13", "a jmp to the next word does nothing" },
    { "T14", "a copy between AC and IO through a temporary is a PDP-1D transfer" }
};


static OptWordP patternWord(OptTableP tableP, int bank, int addr);
static OptFindingP startFinding(OptRuleId rule, OptWordP *wordsPP, int count);
static void emitFinding(OptTableP tableP, OptFindingP findingP, OptWhy why);
static void describeReferrer(OptWordP entryP, OptRole role, int patchOnly, char *bufP, int size);
static int unreachedRunLabeled(OptTableP tableP, OptWordP entryP);
static int reachedPredecessor(OptTableP tableP, OptBlockP blockP);
static OptWhy checkLate(OptTableP tableP, OptFindingP findingP);

static OptWhy checkP1(OptFindingP findingP);
static OptWhy checkP2(OptFindingP findingP);
static OptWhy checkP3(OptFindingP findingP);
static OptWhy checkP4(OptTableP tableP, OptFindingP findingP);
static OptWhy checkShared(OptTableP tableP, OptFindingP findingP, unsigned int which);
static int isMemrefOp(OptWordP entryP, int opcode);

static int t6Claims(OptWordP w1P, OptWordP w2P);
static int t7Claims(OptWordP w1P, OptWordP w2P);
static OptWhy t1Refusal(OptWordP w1P, OptWordP w2P, OptFindingP findingP);
static int invertSkip(OptWordP entryP, int *valueP);

static void ruleT1(OptTableP tableP, int bank, int addr);
static void ruleT1b(OptTableP tableP, int bank, int addr);
static void ruleT2(OptTableP tableP, int bank, int addr);
static void ruleT3(OptTableP tableP, int bank, int addr);
static void ruleT6(OptTableP tableP, int bank, int addr);
static void ruleT7(OptTableP tableP, int bank, int addr);
static void ruleT8(OptTableP tableP, int bank, int addr);
static void ruleT13(OptTableP tableP, int bank, int addr);
static void ruleT14(OptTableP tableP, int bank, int addr, int *claimedToP);
static int tempIsPrivate(OptTableP tableP, OptFindingP findingP, OptWordP tempP);


// Run every rule over a table whose control flow has been overlaid.  The scan
// runs by bank then address and findings are appended as met, so both lists
// come out sorted, diffable between runs.  T6 and T7 go first at each address
// because T1 would merge the same pairs to the same word, doubling the saving.
void
optRunRules(OptTableP tableP)
{
int bank;
int addr;
int t14ClaimedTo;

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        // The highest address a T14 exchange has taken, so the two-word forms
        // do not report a fragment of it.  Addresses only rise in a bank.
        t14ClaimedTo = -1;

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            ruleT6(tableP, bank, addr);
            ruleT7(tableP, bank, addr);
            ruleT1(tableP, bank, addr);
            ruleT1b(tableP, bank, addr);
            ruleT2(tableP, bank, addr);
            ruleT3(tableP, bank, addr);
            ruleT8(tableP, bank, addr);
            ruleT13(tableP, bank, addr);
            ruleT14(tableP, bank, addr, &t14ClaimedTo);
        }
    }
}

// The static execution time of one word in microseconds.  It cannot include
// the instruction an xct runs, an iot's wait, or any indirect step past the
// first.
// Returns the time, or 0 for a reserved word or a spare instruction code.
int
optWordTime(OptWordP entryP)
{
int time;

    if( !entryP || (entryP->flags & OPTF_RESERVED) )
    {
        return(0);
    }

    switch( entryP->decode.group )
    {
    case OPTG_LAW:
    case OPTG_SKIP:
    case OPTG_SHIFT:
    case OPTG_OPERATE:
    case OPTG_1D:
    case OPTG_IOT:
        // One call on memory, so one cycle.
        return(OPTTIME_CYCLE);

    case OPTG_MEMREF:
        break;

    case OPTG_UNKNOWN:
    default:
        // A spare instruction code has no defined behavior, so no time.
        return(0);
    }

    switch( entryP->decode.opcode )
    {
    case 010:       // xct: one cycle; the word it runs is counted separately
    case 060:       // jmp
    case 062:       // jsp
        time = OPTTIME_CYCLE;
        break;

    case 054:       // mul
        time = OPTTIME_MUL;
        break;

    case 056:       // div
        time = OPTTIME_DIV;
        break;

    default:
        // Every other memory reference.
        time = OPTTIME_MEMREF;
        break;
    }

    if( entryP->decode.memIndirect )
    {
        // One indirect step; a pointer that itself indirects costs another
        // cycle, not visible here.
        time += OPTTIME_CYCLE;
    }

    return(time);
}

// Name a rule for the report and the dump.
// Returns a static string, never NILP.
const char *
optRuleName(OptRuleId rule)
{
    if( (rule < 0) || (rule >= OPTRULE_COUNT) )
    {
        return("?");
    }

    return( optRuleTable[rule].nameP );
}

// The one line description of a rule, for the report's group headings.
// Returns a static string, never NILP.
const char *
optRuleSummary(OptRuleId rule)
{
    if( (rule < 0) || (rule >= OPTRULE_COUNT) )
    {
        return("unknown rule");
    }

    return( optRuleTable[rule].summaryP );
}

// Findings and the shared preconditions

// The single word at a bank and address a rule may reason about: in the graph
// by inGraph(), the same test the flow graph uses, so the two never disagree
// about what is an instruction.
// Returns the word, or NILP when there is nothing there a rule may touch.
static OptWordP
patternWord(OptTableP tableP, int bank, int addr)
{
OptWordP entryP;

    if( !(entryP = wordAt(tableP, bank, addr)) )
    {
        return(NILP);
    }

    if( !inGraph(entryP) )
    {
        return(NILP);
    }

    return(entryP);
}

// Begin a finding for a matched pattern.  The words are in address order and
// the first places the finding in the report.
// Returns the finding; out of memory is fatal.
static OptFindingP
startFinding(OptRuleId rule, OptWordP *wordsPP, int count)
{
OptFindingP findingP;
int i;

    if( !(findingP = (OptFindingP)calloc(1, sizeof(OptFinding))) )
    {
        fprintf(stderr, "am1: out of memory building an optimizer finding\n");
        exit(1);
    }

    findingP->rule = rule;
    findingP->why = OPTWHY_NONE;
    findingP->bank = wordsPP[0]->bank;
    findingP->addr = wordsPP[0]->addr;
    findingP->wordCount = count;

    for( i = 0; (i < count) && (i < OPTFIND_MAXWORDS); ++i )
    {
        findingP->wordsP[i] = wordsPP[i];
    }

    return(findingP);
}

// Does the run of unreached code around a word carry a label anywhere?  The
// run is the in-graph unreached words at consecutive addresses either side.
// Returns 1 when some word of the run carries a label, 0 otherwise.
static int
unreachedRunLabeled(OptTableP tableP, OptWordP entryP)
{
int addr;
OptWordP runP;

    for( addr = entryP->addr; addr >= 0; --addr )
    {
        runP = wordAt(tableP, entryP->bank, addr);

        if( !runP || !inGraph(runP) || !(runP->flags & OPTF_UNREACHED) )
        {
            break;
        }

        if( runP->flags & OPTF_HASLABEL )
        {
            return(1);
        }
    }

    for( addr = (entryP->addr + 1); addr < BANKSIZE; ++addr )
    {
        runP = wordAt(tableP, entryP->bank, addr);

        if( !runP || !inGraph(runP) || !(runP->flags & OPTF_UNREACHED) )
        {
            break;
        }

        if( runP->flags & OPTF_HASLABEL )
        {
            return(1);
        }
    }

    return(0);
}

// Does any block the reachability walk reached have an edge into this one?
// Blocks list only successors, so all are searched; this is asked rarely.
// Returns 1 when a reached block leads here, 0 otherwise.
static int
reachedPredecessor(OptTableP tableP, OptBlockP blockP)
{
OptBlockP fromP;
OptFlowEdgeP edgeP;

    if( !blockP->predCount )
    {
        return(0);
    }

    for( fromP = tableP->blocksP; fromP; fromP = fromP->nextP )
    {
        if( !fromP->reached )
        {
            continue;
        }

        for( edgeP = fromP->succP; edgeP; edgeP = edgeP->nextP )
        {
            if( edgeP->toP == blockP )
            {
                return(1);
            }
        }
    }

    return(0);
}

// The two refusals every rule gets last, only on a finding nothing else
// refused, since both question whether the words are instructions at all:
//   unreached  a word is in a block no entry reaches and no word of the
//              unreached run around it has a label: likely unlabeled data that
//              decodes as instructions, like a patch table.  The label test
//              keeps real code the flow analysis loses, such as code after a
//              transfer spelled as a number; that code is labeled.
//   table      the block is entered by no reached block, its first word is an
//              entry only because its address is taken, and it runs into data
//              or nothing: a table used through its address, e.g. run by an xct
//              with a computed target, which P3 cannot see.  Only a reached
//              predecessor counts, since the fall into a table from an xct's
//              target is never taken: xct runs its word in place.
// A block that ends in a transfer of its own is code, however entered.
// Returns OPTWHY_NONE when neither applies; otherwise the reason, with the
// finding's detail replaced by the evidence.
static OptWhy
checkLate(OptTableP tableP, OptFindingP findingP)
{
int i;
OptWordP entryP;
OptBlockP blockP;

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( (entryP->flags & OPTF_UNREACHED) && !unreachedRunLabeled(tableP, entryP) )
        {
            if( findingP->detailP )
            {
                free(findingP->detailP);
            }

            findingP->detailP = allocPrintf("%04o is in unlabeled code no entry reaches, so it may be data that only looks like an instruction",
                entryP->addr);
            return(OPTWHY_UNREACHED);
        }
    }

    for( i = 0; i < findingP->wordCount; ++i )
    {
        blockP = findingP->wordsP[i]->blockP;

        if( !blockP || !blockP->firstP || reachedPredecessor(tableP, blockP) )
        {
            continue;
        }

        if( !(blockP->firstP->flags & OPTF_TAKEN) || (blockP->firstP->flags & OPTF_START) )
        {
            continue;
        }

        if( (blockP->endKind != OPTBE_NOTCODE) && (blockP->endKind != OPTBE_GAP) )
        {
            continue;
        }

        if( findingP->detailP )
        {
            free(findingP->detailP);
        }

        findingP->detailP = allocPrintf("%04o to %04o are reached only through the address %04o and run into data, so they are a table",
            blockP->startAddr, blockP->endAddr, blockP->startAddr);
        return(OPTWHY_TABLE);
    }

    return(OPTWHY_NONE);
}

// File a finding: live when why is OPTWHY_NONE, after checkLate() passes it,
// and suppressed otherwise.  Both lists are appended to in scan order.  A
// per-hop saving (T3) is left out of the time total, which is what one pass
// through every finding would save.
static void
emitFinding(OptTableP tableP, OptFindingP findingP, OptWhy why)
{
    if( why == OPTWHY_NONE )
    {
        why = checkLate(tableP, findingP);
    }

    findingP->why = why;

    if( why == OPTWHY_NONE )
    {
        if( tableP->findingsTailP )
        {
            tableP->findingsTailP->nextP = findingP;
        }
        else
        {
            tableP->findingsP = findingP;
        }

        tableP->findingsTailP = findingP;
        ++tableP->findingCount;
        ++tableP->findingCounts[findingP->rule];
        tableP->savedWords += findingP->saveWords;
        tableP->savedTemps += findingP->saveTemps;

        if( !findingP->perHop )
        {
            tableP->savedTime += findingP->saveTime;
        }

        return;
    }

    if( tableP->suppressedTailP )
    {
        tableP->suppressedTailP->nextP = findingP;
    }
    else
    {
        tableP->suppressedP = findingP;
    }

    tableP->suppressedTailP = findingP;
    ++tableP->suppressedCount;
    ++tableP->suppressedCounts[findingP->rule];
    ++tableP->whyCounts[why];
}

// Release a finding list and the two strings each finding owns.
void
freeFindingList(OptFindingP findingP)
{
OptFindingP nextP;

    while( findingP )
    {
        nextP = findingP->nextP;

        if( findingP->replaceP )
        {
            free(findingP->replaceP);
        }

        if( findingP->detailP )
        {
            free(findingP->detailP);
        }

        free(findingP);
        findingP = nextP;
    }
}

// Name the first word that reaches an entry in a given role, for the evidence
// line of a refused finding.  patchOnly asks for a dap or dip specifically,
// which is what makes the difference between "written" and "patched".
// The buffer always gets a sentence: a flag can also come from conservative
// marking, where no edge records its source.
static void
describeReferrer(OptWordP entryP, OptRole role, int patchOnly, char *bufP, int size)
{
OptEdgeP edgeP;
// The rest of the sentence ("by the ", " at bank ", ", line " and three ints)
// needs at most 58 bytes, so this leaves room for it; spellWord() clips a
// long spelling.
char spell[OPTMSG_SIZE - 64];

    for( edgeP = entryP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->role != role )
        {
            continue;
        }

        if( patchOnly && !(edgeP->flags & OPTEF_PATCH) )
        {
            continue;
        }

        if( !edgeP->fromP )
        {
            continue;
        }

        spellWord(edgeP->fromP, spell, sizeof(spell));
        snprintf(bufP, size, "by the %s at bank %d %04o, line %d", spell,
            edgeP->fromP->bank, edgeP->fromP->addr, edgeP->fromP->lineNo);
        return;
    }

    snprintf(bufP, size, "through a pointer this analysis could not follow");
}

// Precondition P1: nothing may enter the pattern except at its first word,
// by a label, a jump edge or a program entry.  Block starts are NOT the test:
// T2's skip makes block starts of its own following words.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP1(OptFindingP findingP)
{
int i;
OptWordP entryP;

    for( i = 1; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_HASLABEL )
        {
            findingP->detailP = allocPrintf("%04o carries the label %s, so control can arrive inside the pattern",
                entryP->addr, firstLabelName(entryP));
            return(OPTWHY_P1_LABEL);
        }

        if( entryP->flags & OPTF_JUMPTARGET )
        {
            findingP->detailP = allocPrintf("%04o is jumped to from elsewhere, so control can arrive inside the pattern",
                entryP->addr);
            return(OPTWHY_P1_ENTRY);
        }

        if( entryP->flags & OPTF_ENTRY )
        {
            findingP->detailP = allocPrintf("%04o is an entry point of the program, so control can arrive inside the pattern",
                entryP->addr);
            return(OPTWHY_P1_ENTRY);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P2: no word of the pattern, the first included, may be written
// at run time or have its address used as a value; either way it may not be
// the instruction the source spelled, or may be reached unseen.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP2(OptFindingP findingP)
{
int i;
OptWordP entryP;
char evidence[OPTMSG_SIZE];

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_PATCHED )
        {
            describeReferrer(entryP, OPTR_WRITE, 1, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("the address field of %04o is patched %s", entryP->addr, evidence);
            return(OPTWHY_P2_WRITTEN);
        }

        if( entryP->flags & OPTF_WRITTEN )
        {
            describeReferrer(entryP, OPTR_WRITE, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("%04o is written %s", entryP->addr, evidence);
            return(OPTWHY_P2_WRITTEN);
        }

        if( entryP->flags & OPTF_TAKEN )
        {
            describeReferrer(entryP, OPTR_TAKEN, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("the address %04o is used as a value %s", entryP->addr, evidence);
            return(OPTWHY_P2_TAKEN);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P3: no word of the pattern may be the target of an xct, which
// runs one word in place.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP3(OptFindingP findingP)
{
int i;
OptWordP entryP;
char evidence[OPTMSG_SIZE];

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];

        if( entryP->flags & OPTF_XCTTARGET )
        {
            describeReferrer(entryP, OPTR_EXECUTE, 0, evidence, sizeof(evidence));
            findingP->detailP = allocPrintf("%04o is executed in place %s", entryP->addr, evidence);
            return(OPTWHY_P3_XCT);
        }
    }

    return(OPTWHY_NONE);
}

// Precondition P4: no skip-class word immediately before the pattern.  Its
// first word might not execute, and a rewrite that changes the pattern's size
// changes where the skip lands.  OPTF_AFTERSKIP, from optSkipWord(), is the
// only test: it includes div.
// Returns OPTWHY_NONE when the precondition holds, the reason otherwise.
static OptWhy
checkP4(OptTableP tableP, OptFindingP findingP)
{
OptWordP firstP;
OptWordP prevP;
char spell[OPTMSG_SIZE];

    firstP = findingP->wordsP[0];
    prevP = wordAt(tableP, firstP->bank, (firstP->addr - 1));

    if( !(firstP->flags & OPTF_AFTERSKIP) )
    {
        return(OPTWHY_NONE);
    }

    if( prevP )
    {
        spellWord(prevP, spell, sizeof(spell));
        findingP->detailP = allocPrintf("%04o is the skip-class word %s, so %04o may not execute at all",
            prevP->addr, spell, firstP->addr);
    }
    else
    {
        findingP->detailP = allocPrintf("the word before %04o is a skip, so %04o may not execute at all",
            firstP->addr, firstP->addr);
    }

    return(OPTWHY_P4_SKIP);
}

// Run the shared preconditions a rule asks for, in their numbered order, and
// stop at the first that fails.
// Returns OPTWHY_NONE when they all hold, the first failure's reason
// otherwise; on a failure the finding's detail says which word and why.
static OptWhy
checkShared(OptTableP tableP, OptFindingP findingP, unsigned int which)
{
OptWhy why;

    if( which & OPTPC_P1 )
    {
        if( (why = checkP1(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P2 )
    {
        if( (why = checkP2(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P3 )
    {
        if( (why = checkP3(findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    if( which & OPTPC_P4 )
    {
        if( (why = checkP4(tableP, findingP)) != OPTWHY_NONE )
        {
            return(why);
        }
    }

    return(OPTWHY_NONE);
}

// Is a word a particular memory reference instruction?  The indirect bit is
// not tested: callers that care test decode.memIndirect, and jda shares cal's
// opcode with that bit set.
// Returns 1 when it matches, 0 otherwise or for NILP.
static int
isMemrefOp(OptWordP entryP, int opcode)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->decode.group != OPTG_MEMREF )
    {
        return(0);
    }

    return( entryP->decode.opcode == opcode );
}

// The rules

// Does T6 own this pair?  T6 and T1 would both fire on a bare cla before a
// word that clears AC itself, suggesting the same word; T6's message is the
// useful one.  The first word must be a bare cla.  lat and lap OR into AC, so
// they qualify only carrying the cla bit, as permsyms.def spells them.  A lap
// pair is owned here so that ruleT6() can report its refusal.
// Returns 1 when T6 owns the pair, 0 otherwise.
static int
t6Claims(OptWordP w1P, OptWordP w2P)
{
    if( !w1P || !w2P )
    {
        return(0);
    }

    if( (w1P->decode.group != OPTG_OPERATE) || (w1P->decode.microBits != OPTM_CLA) )
    {
        return(0);
    }

    if( w2P->decode.group == OPTG_LAW )
    {
        return(1);
    }

    if( isMemrefOp(w2P, 020) )      // lac, indirect or not: it fills AC either way
    {
        return(1);
    }

    if( (w2P->decode.group == OPTG_OPERATE)
        && (w2P->decode.microBits & OPTM_CLA)
        && (w2P->decode.microBits & (OPTM_LAT | OPTM_LAP)) )
    {
        return(1);
    }

    return(0);
}

// Does T7 own this pair?  A bare cli in front of a lio, which fills IO
// outright.  lio is a memory reference, so this never collides with T1.
// Returns 1 when T7 owns the pair, 0 otherwise.
static int
t7Claims(OptWordP w1P, OptWordP w2P)
{
    if( !w1P || !w2P )
    {
        return(0);
    }

    if( (w1P->decode.group != OPTG_OPERATE) || (w1P->decode.microBits != OPTM_CLI) )
    {
        return(0);
    }

    return( isMemrefOp(w2P, 022) );
}

// May two consecutive operate words be merged?  Only their contents are
// judged here.  One word applies all its micro-ops in phase order (handbook
// page 21); two words apply the first's completely before the second's.  They
// agree only when nothing in the first acts later than anything in the second,
// after four cases the phase order alone does not catch.
// Returns OPTWHY_NONE when the merge is sound, the reason otherwise.
static OptWhy
t1Refusal(OptWordP w1P, OptWordP w2P, OptFindingP findingP)
{
int i;
int j;
unsigned int m1;
unsigned int m2;
const OptMicroOp *aP;
const OptMicroOp *bP;

    m1 = w1P->decode.microBits;
    m2 = w2P->decode.microBits;

    // lap ORs in its own address plus one, and the merge moves the second word
    // one lower; a lap in the first word does not move.
    if( optMicroOpPresent(m2, OPTMO_LAP) )
    {
        findingP->detailP = allocPrintf("the second word carries lap, whose value is its own address plus one; merging moves it one word lower");
        return(OPTWHY_LAP);
    }

    // lia and lai are armed at TP8 and complete during the next instruction's
    // fetch, so in separate words they are two copies, the first finished
    // before the second is armed.  In one word they are a single exchange.
    if( (optMicroOpPresent(m1, OPTMO_LIA) && optMicroOpPresent(m2, OPTMO_LAI))
        || (optMicroOpPresent(m1, OPTMO_LAI) && optMicroOpPresent(m2, OPTMO_LIA)) )
    {
        findingP->detailP = allocPrintf("lia in one word and lai in the other are two copies; in a single word they are one exchange");
        return(OPTWHY_EXCHANGE);
    }

    // A micro-op that is not idempotent cannot fold into one copy.
    for( i = 0; i < (int)(sizeof(optNotIdempotent) / sizeof(optNotIdempotent[0])); ++i )
    {
        if( optMicroOpPresent(m1, optNotIdempotent[i]) && optMicroOpPresent(m2, optNotIdempotent[i]) )
        {
            findingP->detailP = allocPrintf("both words carry %s, which done twice is not the same as done once",
                optMicroOps[optNotIdempotent[i]].nameP);
            return(OPTWHY_REPEAT);
        }
    }

    // The flag field is one four bit slot: a word cannot clear one flag and
    // set another, nor name two flag numbers.
    if( (w1P->decode.flagOp != OPTFLAG_NONE) && (w2P->decode.flagOp != OPTFLAG_NONE) )
    {
        if( (w1P->decode.flagOp != w2P->decode.flagOp) || (w1P->decode.flagNum != w2P->decode.flagNum) )
        {
            findingP->detailP = allocPrintf("the words carry %s %o and %s %o, and one word has only one flag field",
                (w1P->decode.flagOp == OPTFLAG_CLF)?"clf":"stf", w1P->decode.flagNum,
                (w2P->decode.flagOp == OPTFLAG_CLF)?"clf":"stf", w2P->decode.flagNum);
            return(OPTWHY_FLAGS);
        }
    }

    // Now the phase order itself.
    for( i = 0; i < OPTMO_COUNT; ++i )
    {
        if( !optMicroOpPresent(m1, (OptMicroId)i) )
        {
            continue;
        }

        aP = &optMicroOps[i];

        for( j = 0; j < OPTMO_COUNT; ++j )
        {
            if( i == j )
            {
                continue;
            }

            if( !optMicroOpPresent(m2, (OptMicroId)j) )
            {
                continue;
            }

            bP = &optMicroOps[j];

            if( (aP->phase > bP->phase) || ((aP->phase == bP->phase) && (aP->order > bP->order)) )
            {
                findingP->detailP = allocPrintf("%s in the first word acts after %s in the second, and one word would run them the other way round",
                    aP->nameP, bP->nameP);
                return(OPTWHY_PHASE);
            }
        }
    }

    return(OPTWHY_NONE);
}

// T1: two consecutive operate words become one with the union of their
// micro-ops.  Saves the word and its cycle.
static void
ruleT1(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
unsigned int merged;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.group != OPTG_OPERATE )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[1]->decode.group != OPTG_OPERATE )
    {
        return;
    }

    if( t6Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T1, wordsP, 2);
    merged = (wordsP[0]->decode.microBits | wordsP[1]->decode.microBits);
    spellOperate(merged, spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    if( (why = t1Refusal(wordsP[0], wordsP[1], findingP)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("the merged word is %06o", (0760000 | merged));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T1b: two consecutive shifts of the same opcode become one shift of the
// summed count, if it is at most nine.  The count is the number of one bits
// in bits 9 to 17, spelled 1s to 9s.
static void
ruleT1b(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
int total;

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.group != OPTG_SHIFT )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[1]->decode.group != OPTG_SHIFT )
    {
        return;
    }

    // Bits 0 to 8 are the whole opcode: group, direction, shift or rotate, and
    // registers.
    if( (wordsP[0]->value & ~OPTSH_COUNT) != (wordsP[1]->value & ~OPTSH_COUNT) )
    {
        return;
    }

    // A shift naming no register does nothing; nothing to suggest.
    if( !wordsP[0]->decode.shiftRegs )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T1B, wordsP, 2);
    total = (wordsP[0]->decode.shiftCount + wordsP[1]->decode.shiftCount);

    if( total > 9 )
    {
        // No replacement: there is no 10s symbol.
        findingP->detailP = allocPrintf("%d steps and %d steps make %d, and bits 9 to 17 hold at most nine",
            wordsP[0]->decode.shiftCount, wordsP[1]->decode.shiftCount, total);
        emitFinding(tableP, findingP, OPTWHY_SHIFTCOUNT);
        return;
    }

    findingP->replaceP = allocPrintf("%s %ds", wordsP[0]->decode.mnemonicP, total);

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("the merged word is %06o", ((wordsP[0]->value & ~OPTSH_COUNT) | ((1 << total) - 1)));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// Can a skip-class word be inverted, and to what?  The skip group's bit 5
// reverses the whole combined condition (handbook page 20); sad and sas are
// each other's inverse, one bit apart.  isp and div have no inverse.
// Returns 1 and the inverted word in *valueP, or 0 when there is no inverse.
static int
invertSkip(OptWordP entryP, int *valueP)
{
    if( !entryP )
    {
        return(0);
    }

    if( entryP->decode.group == OPTG_SKIP )
    {
        *valueP = (entryP->value ^ 0010000);
        return(1);
    }

    if( entryP->decode.group == OPTG_MEMREF )
    {
        switch( entryP->decode.opcode )
        {
        case 050:       // sad, skip if AC and Y differ
        case 052:       // sas, skip if AC and Y are the same
            *valueP = (entryP->value ^ 0020000);
            return(1);

        default:
            break;
        }
    }

    return(0);
}

// T2: "skip; jmp .+2; W" is the inverted skip followed by W.  The jmp's target
// is the word after W, so both forms take the same path either way.  Saves the
// jmp word and its cycle on the path that took it.
static void
ruleT2(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[3];
OptFindingP findingP;
OptWhy why;
int inverted;
char spell[OPTMSG_SIZE];
char operand[OPTMSG_SIZE];

    if( (addr + 3) >= BANKSIZE )
    {
        return;
    }

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !optSkipWord(wordsP[0]) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[1], 060) || wordsP[1]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[1]->decode.address != (addr + 3) )
    {
        return;
    }

    if( !(wordsP[2] = patternWord(tableP, bank, (addr + 2))) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T2, wordsP, 3);

    if( !invertSkip(wordsP[0], &inverted) )
    {
        spellWord(wordsP[0], spell, sizeof(spell));
        findingP->detailP = allocPrintf("%s cannot be written the other way round: it is not the skip group and it is not sad or sas",
            spell);
        emitFinding(tableP, findingP, OPTWHY_NOINVERSE);
        return;
    }

    if( wordsP[0]->decode.group == OPTG_SKIP )
    {
        spellSkipValue(inverted, spell, sizeof(spell));
        findingP->replaceP = allocPrintf("%s", spell);
    }
    else
    {
        spellOperand(wordsP[0], operand, sizeof(operand));
        findingP->replaceP = allocPrintf("%s%s %s", (wordsP[0]->decode.opcode == 050)?"sas":"sad",
            (wordsP[0]->decode.memIndirect)?" i":"", operand);
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("drop the jmp at %04o and reverse the skip; the word at %04o stays where it is in the source",
        wordsP[1]->addr, wordsP[2]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T3: a jmp to a jmp can name the far target directly.  It removes no word,
// only the hop's cycle on the executions that take it.  The intermediate must
// not be patched, or the rewritten jump would bypass the patch.
static void
ruleT3(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptWordP viaP;
OptFindingP findingP;
OptWhy why;
char evidence[OPTMSG_SIZE];
char operand[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 060) || wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address == addr )
    {
        return;     // a jmp to itself: a deliberate spin, not a hop
    }

    if( !(viaP = patternWord(tableP, bank, wordsP[0]->decode.address)) )
    {
        return;
    }

    if( !isMemrefOp(viaP, 060) || viaP->decode.memIndirect )
    {
        return;
    }

    if( viaP->decode.address == viaP->addr )
    {
        return;     // the intermediate spins on itself: following it is not a hop
    }

    if( viaP->decode.address == wordsP[0]->decode.address )
    {
        return;     // nothing would change
    }

    findingP = startFinding(OPTRULE_T3, wordsP, 1);

    // These refusals, and the taken intermediate below, offer no suggested
    // line: the far target is whatever was last written to the intermediate,
    // so its assembled address (often a "jmp 0" placeholder) would print as a
    // destination the rule never meant.
    if( viaP->flags & OPTF_PATCHED )
    {
        describeReferrer(viaP, OPTR_WRITE, 1, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the intermediate jmp at %04o is patched %s, so where it goes is not fixed",
            viaP->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_PATCHED);
        return;
    }

    if( viaP->flags & OPTF_WRITTEN )
    {
        describeReferrer(viaP, OPTR_WRITE, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the intermediate jmp at %04o is written %s, so it is not always a jmp",
            viaP->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_P2_WRITTEN);
        return;
    }

    // Past those refusals the intermediate's address field is fixed.
    spellOperand(viaP, operand, sizeof(operand));
    findingP->replaceP = allocPrintf("jmp %s", operand);

    // The rewritten word stays one word in the same place, so only P2 applies.
    if( (why = checkShared(tableP, findingP, OPTPC_P2)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    // An intermediate whose address is taken can be written through a pointer
    // this analysis cannot follow; a patch table may write such jmps at boot.
    // Tested after P2 so an earlier reason stands, and with no suggestion.
    if( viaP->flags & (OPTF_TAKEN | OPTF_MAYBE_WRITTEN) )
    {
        describeReferrer(viaP, OPTR_TAKEN, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the address of the intermediate jmp at %04o is used as a value %s, so a pointer can write it",
            viaP->addr, evidence);
        free(findingP->replaceP);
        findingP->replaceP = NILP;
        emitFinding(tableP, findingP, OPTWHY_THROUGH);
        return;
    }

    findingP->saveWords = 0;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->perHop = 1;
    findingP->detailP = allocPrintf("%04o holds a jmp to %04o; the word at %04o is unchanged and any other route through it still works",
        viaP->addr, viaP->decode.address, viaP->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T6: a bare cla before an instruction that fills AC outright.  The cla goes
// and the loading instruction moves up one word.
static void
ruleT6(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !t6Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T6, wordsP, 2);
    spellWord(wordsP[1], spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    // A cla before lap is redundant for AC (lap carries the cla bit), but lap
    // ORs in its own address plus one and dropping the cla moves it one word
    // lower, changing the value.  Reported refused rather than not looked for.
    if( (wordsP[1]->decode.group == OPTG_OPERATE) && (wordsP[1]->decode.microBits & OPTM_LAP) )
    {
        findingP->detailP = allocPrintf("lap loads its own address plus one, and dropping the cla at %04o would move it one word lower",
            wordsP[0]->addr);
        emitFinding(tableP, findingP, OPTWHY_LAP);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%s fills AC on its own, so the cla at %04o changes nothing",
        spell, wordsP[0]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T7: a bare cli in front of a lio, which fills IO outright.
static void
ruleT7(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[2];
OptFindingP findingP;
OptWhy why;
char spell[OPTMSG_SIZE];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( !t7Claims(wordsP[0], wordsP[1]) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T7, wordsP, 2);
    spellWord(wordsP[1], spell, sizeof(spell));
    findingP->replaceP = allocPrintf("%s", spell);

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%s fills IO on its own, so the cli at %04o changes nothing",
        spell, wordsP[0]->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T8: a lac or lio of a constant pool word whose value an augmented
// instruction can produce.  The word stays put but drops a cycle, and the
// pool loses a word if nothing else names it.  Only pool words qualify: a
// variable that holds zero now can be written later.
static void
ruleT8(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptWordP poolP;
OptFindingP findingP;
OptRuleId rule;
OptWhy why;
OptEdgeP edgeP;
int value;
int others;
char evidence[OPTMSG_SIZE];
char replace[OPTMSG_SIZE];
char symbol[OPTMSG_SIZE - 8];

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 020) && !isMemrefOp(wordsP[0], 022) )
    {
        return;
    }

    if( !(poolP = wordAt(tableP, bank, wordsP[0]->decode.address)) )
    {
        return;
    }

    if( (poolP->kind != OPTK_CONST) || (poolP->flags & (OPTF_RESERVED | OPTF_DUPADDR)) )
    {
        return;
    }

    value = poolP->value;
    rule = OPTRULE_COUNT;

    if( isMemrefOp(wordsP[0], 022) )
    {
        // lio: the only augmented instruction that fills IO is cli.
        if( value != 0 )
        {
            return;
        }

        rule = OPTRULE_T8A;
        strcpy(replace, "cli");
    }
    else if( !value )
    {
        rule = OPTRULE_T8B;
        strcpy(replace, "cla");
    }
    else if( value == WRDMASK )
    {
        rule = OPTRULE_T8C;
        strcpy(replace, "cla cma");
    }
    else if( value <= ADDRMASK )
    {
        rule = OPTRULE_T8D;

        // Keep the symbol the source used for the value; a number applied by
        // hand pins an address that moves with the build.
        if( spellConstSymbol(wordsP[0], symbol, sizeof(symbol)) )
        {
            snprintf(replace, sizeof(replace), "law %s", symbol);
        }
        else
        {
            sprintf(replace, "law 0o%o", value);
        }
    }
    else if( ((~value) & WRDMASK) <= ADDRMASK )
    {
        rule = OPTRULE_T8D;
        sprintf(replace, "law i 0o%o", ((~value) & WRDMASK));
    }
    else
    {
        return;
    }

    findingP = startFinding(rule, wordsP, 1);
    findingP->replaceP = allocPrintf("%s", replace);

    if( poolP->flags & OPTF_WRITTEN )
    {
        describeReferrer(poolP, OPTR_WRITE, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the pool word at %04o is written %s, so it does not always hold %06o",
            poolP->addr, evidence, value);
        emitFinding(tableP, findingP, OPTWHY_P2_WRITTEN);
        return;
    }

    if( wordsP[0]->flags & OPTF_PATCHED )
    {
        describeReferrer(wordsP[0], OPTR_WRITE, 1, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the address field of %04o is patched %s, so it does not always name the pool word",
            wordsP[0]->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_PATCHED);
        return;
    }

    // The word is rewritten where it stands, so only P2 applies.
    if( (why = checkShared(tableP, findingP, OPTPC_P2)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    // A pool word whose address is a value can be written through it.
    if( poolP->flags & (OPTF_TAKEN | OPTF_MAYBE_WRITTEN) )
    {
        describeReferrer(poolP, OPTR_TAKEN, 0, evidence, sizeof(evidence));
        findingP->detailP = allocPrintf("the address of the pool word at %04o is used as a value %s, so a pointer can write it",
            poolP->addr, evidence);
        emitFinding(tableP, findingP, OPTWHY_THROUGH);
        return;
    }

    others = 0;

    for( edgeP = poolP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        if( edgeP->fromP != wordsP[0] )
        {
            ++others;
        }
    }

    findingP->saveTime = (OPTTIME_MEMREF - OPTTIME_CYCLE);

    if( !others )
    {
        findingP->saveTemps = 1;
        findingP->detailP = allocPrintf("the pool word at %04o holds %06o and nothing else names it, so the pool loses a word too",
            poolP->addr, value);
    }
    else
    {
        findingP->detailP = allocPrintf("the pool word at %04o holds %06o and %d other reference%s name%s it, so it stays",
            poolP->addr, value, others, (others == 1)?"":"s", (others == 1)?"s":"");
    }

    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// T13: a jmp to the very next word.  Control goes there anyway.
static void
ruleT13(OptTableP tableP, int bank, int addr)
{
OptWordP wordsP[1];
OptFindingP findingP;
OptWhy why;

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !isMemrefOp(wordsP[0], 060) || wordsP[0]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address != (addr + 1) )
    {
        return;
    }

    findingP = startFinding(OPTRULE_T13, wordsP, 1);
    findingP->replaceP = allocPrintf("(delete the word)");

    // P1 means nothing for one word, but the rest matter: a patched word is
    // not this jmp when it runs, an xct of it goes to the word after the jmp,
    // and a skip before it would reach past the next word once it is gone.
    if( (why = checkShared(tableP, findingP, (OPTPC_P2 | OPTPC_P3 | OPTPC_P4))) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTime = OPTTIME_CYCLE;
    findingP->detailP = allocPrintf("%04o is the next word, which the machine would reach without the jmp", (addr + 1));
    emitFinding(tableP, findingP, OPTWHY_NONE);
}

// Is a temporary private to a pattern?  T14's rewrite stops writing it, so
// anything else that reads, writes or takes its address would change.
// Returns 1 when nothing outside the pattern reaches it; 0 otherwise, with
// the finding's detail saying what does.
static int
tempIsPrivate(OptTableP tableP, OptFindingP findingP, OptWordP tempP)
{
OptEdgeP edgeP;
int i;
int mine;
char spell[OPTMSG_SIZE];

    (void)tableP;

    if( tempP->flags & (OPTF_TAKEN | OPTF_XCTTARGET | OPTF_CODE) )
    {
        findingP->detailP = allocPrintf("the temporary at %04o is %s, so it is more than a scratch word",
            tempP->addr,
            (tempP->flags & OPTF_TAKEN)?"an address something uses as a value":
            ((tempP->flags & OPTF_XCTTARGET)?"executed by an xct":"classified code"));
        return(0);
    }

    for( edgeP = tempP->inP; edgeP; edgeP = edgeP->nextInP )
    {
        mine = 0;

        for( i = 0; i < findingP->wordCount; ++i )
        {
            if( edgeP->fromP == findingP->wordsP[i] )
            {
                mine = 1;
                break;
            }
        }

        if( mine )
        {
            continue;
        }

        if( edgeP->fromP )
        {
            spellWord(edgeP->fromP, spell, sizeof(spell));
            findingP->detailP = allocPrintf("the temporary at %04o is also reached by the %s at bank %d %04o, line %d",
                tempP->addr, spell, edgeP->fromP->bank, edgeP->fromP->addr, edgeP->fromP->lineNo);
        }
        else
        {
            findingP->detailP = allocPrintf("the temporary at %04o is reached from outside the pattern", tempP->addr);
        }

        return(0);
    }

    return(1);
}

// T14: a copy between AC and IO through a temporary, which the PDP-1D does in
// one word: "dac t; lio t" is lia, "dio t; lac t" is lai, and the four-word
// crosswise save and reload is swp (Docs/UsingPDP-1DInstructions.md).  The
// exchange is checked first and claims its addresses so the two-word forms
// skip them.  Both deposits must precede both loads, as each load overwrites
// a register the other deposit reads.
static void
ruleT14(OptTableP tableP, int bank, int addr, int *claimedToP)
{
OptWordP wordsP[OPTFIND_MAXWORDS];
OptWordP depositP[2];
OptWordP loadP[2];
OptWordP tempAcP;
OptWordP tempIoP;
OptFindingP findingP;
OptWhy why;
int i;
int acAddr;
int ioAddr;

    if( addr <= *claimedToP )
    {
        return;
    }

    // The four-word exchange

    if( (addr + 3) < BANKSIZE )
    {
        for( i = 0; i < 4; ++i )
        {
            wordsP[i] = patternWord(tableP, bank, (addr + i));
        }

        if( wordsP[0] && wordsP[1] && wordsP[2] && wordsP[3]
            && !wordsP[0]->decode.memIndirect && !wordsP[1]->decode.memIndirect
            && !wordsP[2]->decode.memIndirect && !wordsP[3]->decode.memIndirect )
        {
            depositP[0] = NILP;
            depositP[1] = NILP;
            loadP[0] = NILP;
            loadP[1] = NILP;

            for( i = 0; i < 2; ++i )
            {
                if( isMemrefOp(wordsP[i], 024) )        // dac: AC to the temporary
                {
                    depositP[0] = wordsP[i];
                }
                else if( isMemrefOp(wordsP[i], 032) )   // dio: IO to the temporary
                {
                    depositP[1] = wordsP[i];
                }
            }

            for( i = 2; i < 4; ++i )
            {
                if( isMemrefOp(wordsP[i], 020) )        // lac: AC from a temporary
                {
                    loadP[0] = wordsP[i];
                }
                else if( isMemrefOp(wordsP[i], 022) )   // lio: IO from a temporary
                {
                    loadP[1] = wordsP[i];
                }
            }

            if( depositP[0] && depositP[1] && loadP[0] && loadP[1] )
            {
                acAddr = depositP[0]->decode.address;   // where AC was put
                ioAddr = depositP[1]->decode.address;   // where IO was put

                // AC comes back from where IO went and IO from where AC went,
                // through two different words.
                if( (acAddr != ioAddr) && (loadP[0]->decode.address == ioAddr)
                    && (loadP[1]->decode.address == acAddr) )
                {
                    // Claimed whether this finding ends up live or refused.
                    *claimedToP = (addr + 3);
                    findingP = startFinding(OPTRULE_T14, wordsP, 4);
                    findingP->replaceP = allocPrintf("swp");
                    tempAcP = wordAt(tableP, bank, acAddr);
                    tempIoP = wordAt(tableP, bank, ioAddr);

                    if( !tempAcP || !tempIoP )
                    {
                        findingP->detailP = allocPrintf("one of the temporaries at %04o and %04o has no emitted word",
                            acAddr, ioAddr);
                        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
                        return;
                    }

                    if( !tempIsPrivate(tableP, findingP, tempAcP) || !tempIsPrivate(tableP, findingP, tempIoP) )
                    {
                        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
                        return;
                    }

                    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
                    {
                        emitFinding(tableP, findingP, why);
                        return;
                    }

                    findingP->saveWords = 3;
                    findingP->saveTemps = 2;
                    findingP->saveTime = ((4 * OPTTIME_MEMREF) - OPTTIME_CYCLE);
                    findingP->detailP = allocPrintf("swp exchanges AC and IO in one word; the temporaries at %04o and %04o become free",
                        acAddr, ioAddr);
                    emitFinding(tableP, findingP, OPTWHY_NONE);
                    return;
                }
            }
        }
    }

    // The two-word copies

    if( !(wordsP[0] = patternWord(tableP, bank, addr)) )
    {
        return;
    }

    if( !(wordsP[1] = patternWord(tableP, bank, (addr + 1))) )
    {
        return;
    }

    if( wordsP[0]->decode.memIndirect || wordsP[1]->decode.memIndirect )
    {
        return;
    }

    if( wordsP[0]->decode.address != wordsP[1]->decode.address )
    {
        return;
    }

    if( isMemrefOp(wordsP[0], 024) && isMemrefOp(wordsP[1], 022) )
    {
        // dac t; lio t: IO ends up holding AC, and AC is untouched.
        findingP = startFinding(OPTRULE_T14, wordsP, 2);
        findingP->replaceP = allocPrintf("lia");
    }
    else if( isMemrefOp(wordsP[0], 032) && isMemrefOp(wordsP[1], 020) )
    {
        // dio t; lac t: AC ends up holding IO, and IO is untouched.
        findingP = startFinding(OPTRULE_T14, wordsP, 2);
        findingP->replaceP = allocPrintf("lai");
    }
    else
    {
        return;
    }

    if( !(tempAcP = wordAt(tableP, bank, wordsP[0]->decode.address)) )
    {
        findingP->detailP = allocPrintf("the temporary at %04o has no emitted word", wordsP[0]->decode.address);
        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
        return;
    }

    if( !tempIsPrivate(tableP, findingP, tempAcP) )
    {
        emitFinding(tableP, findingP, OPTWHY_TEMPUSED);
        return;
    }

    if( (why = checkShared(tableP, findingP, OPTPC_ALL)) != OPTWHY_NONE )
    {
        emitFinding(tableP, findingP, why);
        return;
    }

    findingP->saveWords = 1;
    findingP->saveTemps = 1;
    findingP->saveTime = ((2 * OPTTIME_MEMREF) - OPTTIME_CYCLE);
    findingP->detailP = allocPrintf("the temporary at %04o is written and read by nothing else, so it becomes free too",
        tempAcP->addr);
    emitFinding(tableP, findingP, OPTWHY_NONE);
}
