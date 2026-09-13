/* optreport.c - the am1 optimizer: the .opt report and the -O=rules dump
 *
 * Purpose:
 *   Task A5's output half.  Prints one finding, the live and the suppressed
 *   finding lists, the header notes that say what the reader is looking at,
 *   the statistics, and the one-line-per-finding dump (-O=rules).  It calls
 *   into optrefs.c and optflow.c for the per-bank reference and control-flow
 *   sections, so that the whole of a bank's evidence is printed in one place.
 *
 * Architectural scope:
 *   Part of the am1 optimizer, the analysis-only advisor of
 *   Optimizer/FeasibilityStudy.md sections 5 to 7.  It runs after yyparse()
 *   has returned and before any code generator, reads the parse tree and the
 *   resolved symbol tables and never modifies either; everything it derives
 *   lives in the OptTable declared in optimizer.h.
 *
 * Dependencies:
 *   am1.h and y.tab.h for the tree, the symbol table and the node type
 *   tokens; optimizer.h for the word table, the shared types and the entry
 *   points the other optimizer files publish.
 *
 * Execution model:
 *   Single threaded, called from writeReport() in optimizer.c, and from
 *   optimize() for the -O=rules dump.  Memory is allocated with
 *   malloc/calloc and released by optFreeTable(); a failure to allocate is
 *   fatal, as it is everywhere else in am1.
 *
 * Verification:
 *   Tests/Optimizer/run_optimizer.sh compares whole reports against stored
 *   expectations; rules_check.sh checks the dump's totals against the
 *   report's own counts.
 *
 * Revision history:
 *
 * 09-Sep-2026 claude - task A7a: split out of optimizer.c.  The code is the
 *                      code that was there, moved and not rewritten; only the
 *                      static storage class of what now crosses a file
 *                      boundary changed
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

static void printFinding(FILE *fP, OptFindingP findingP, int suppressed);

// Why a matched pattern was refused: the tag the dump prints and the sentence
// the report prints.
static const struct
{
    const char *tagP;
    const char *textP;
} optWhyTable[OPTWHY_COUNT] =
{
    { "-",          "not refused" },
    { "P1-label",   "P1: a word inside the pattern carries a label" },
    { "P1-entry",   "P1: a word inside the pattern is a jump target or a program entry" },
    { "P2-written", "P2: a word of the pattern is written while the program runs" },
    { "P2-taken",   "P2: the address of a word of the pattern is used as a value" },
    { "P3-xct",     "P3: a word of the pattern is executed by an xct" },
    { "P4-skip",    "P4: a skip-class word sits immediately before the pattern" },
    { "phase",      "the hardware would apply the merged micro-ops in the other order" },
    { "flags",      "both words operate on a program flag, and one word has only one flag field" },
    { "repeat",     "a micro-op that is not idempotent appears in both words" },
    { "lap",        "the second word carries lap, whose value is its own address plus one" },
    { "exchange",   "lia and lai in separate words are two copies, not one exchange" },
    { "count",      "the summed shift count is more than the nine a shift word can hold" },
    { "noinverse",  "the skip-class word has no inverted form" },
    { "patched",    "the word the rule reads through has its address field written at run time" },
    { "tempused",   "the temporary is reached from outside the pattern" }
};

// The short tag for a refusal reason.
// Returns a static string, never NILP.
const char *
optWhyName(OptWhy why)
{
    if( (why < 0) || (why >= OPTWHY_COUNT) )
    {
        return("?");
    }

    return( optWhyTable[why].tagP );
}

// ---------------------------------------------------------------------------
// The report and the dump.
// ---------------------------------------------------------------------------

// Print one finding: where it is, what it would save or why it was refused,
// the words it names in octal and as the source spelled them, the suggested
// replacement, and the rule's note or the evidence for the refusal.
static void
printFinding(FILE *fP, OptFindingP findingP, int suppressed)
{
int i;
OptWordP entryP;
char spell[OPTMSG_SIZE];

    entryP = findingP->wordsP[0];

    fprintf(fP, "    bank %2d %04o  %s:%d", findingP->bank, findingP->addr,
        (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);

    if( suppressed )
    {
        fprintf(fP, "  %s\n", optWhyTable[findingP->why].textP);
    }
    else
    {
        fprintf(fP, "  saves %d word%s", findingP->saveWords, (findingP->saveWords == 1)?"":"s");

        if( findingP->saveTemps )
        {
            fprintf(fP, ", %d storage word%s", findingP->saveTemps, (findingP->saveTemps == 1)?"":"s");
        }

        fprintf(fP, ", %d us%s\n", findingP->saveTime, (findingP->perHop)?" per hop taken":"");
    }

    for( i = 0; i < findingP->wordCount; ++i )
    {
        entryP = findingP->wordsP[i];
        spellWord(entryP, spell, sizeof(spell));
        fprintf(fP, "        %04o %06o  %s\n", entryP->addr, entryP->value, spell);
    }

    if( findingP->replaceP )
    {
        // A refused finding still shows the word a naive rewrite would give:
        // it is what the reason line is about.  "would become" rather than
        // "becomes" so that nobody types it in.
        fprintf(fP, "        %s %s\n", (suppressed)?"would become:":"becomes:", findingP->replaceP);
    }

    if( findingP->detailP )
    {
        fprintf(fP, "        %s %s\n", (suppressed)?"because":"note:", findingP->detailP);
    }
}

// The header notes: what the reader is holding, what the preconditions mean,
// and the one precondition the analysis cannot check for them.
void
writeHeaderNotes(FILE *fP)
{
    fprintf(fP, "\nThis is an advisory report.  Nothing in the assembled program was changed.\n");
    fprintf(fP, "Addresses and word values are octal.\n");
    fprintf(fP, "A number inside a suggested source line carries an explicit 0o prefix.\n");
    fprintf(fP, "This avoids any conflict with the current radix setting if it is pasted into the code.\n");
    fprintf(fP, "\nA suggestion is made only when all of these hold:\n");
    fprintf(fP, "  P1 - no label, jump target or program entry inside the pattern, so nothing\n");
    fprintf(fP, "      can arrive anywhere but at its first word.\n");
    fprintf(fP, "  P2 - no word of the pattern is written or patched while the program runs,\n");
    fprintf(fP, "      and no word's address is used as a value.\n");
    fprintf(fP, "  P3 - no word of the pattern is executed in place by an xct.\n");
    fprintf(fP, "  P4 - no skip-class word immediately before the pattern.\n");
    fprintf(fP, "  P5 - no location involved is shared with a device or a sequence break handler.\n");
    fprintf(fP, "\nP5 is not checked here and must be reviewed by hand.\n");
    fprintf(fP, "The assembler cannot know when words are modified behind the program's\n");
    fprintf(fP, "back, for example, code loaded at runtime from the drum.\n");
    fprintf(fP, "Timing is similar; a delay loop for the typewriter, the punch or the display is\n");
    fprintf(fP, "meant to take the time it takes.\n");
}

// The findings, grouped by rule.  A rule with nothing to say is left out
// entirely; a report with no findings at all still says so, and the sections
// after it still carry the statistics.
void
writeFindingList(FILE *fP, OptTableP tableP)
{
int rule;
int any;
OptFindingP findingP;

    fprintf(fP, "\nFindings: %d\n", tableP->findingCount);

    if( !tableP->findingCount )
    {
        fprintf(fP, "  No rule matched anything it could suggest a change for.\n");
        return;
    }

    fprintf(fP, "  Taken together they would remove %d word%s", tableP->savedWords,
        (tableP->savedWords == 1)?"":"s");

    if( tableP->savedTemps )
    {
        fprintf(fP, " and %d storage word%s", tableP->savedTemps, (tableP->savedTemps == 1)?"":"s");
    }

    fprintf(fP, ", and %d microsecond%s from one pass through every one of them.\n",
        tableP->savedTime, (tableP->savedTime == 1)?"":"s");
    fprintf(fP, "  Per rule:");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( tableP->findingCounts[rule] )
        {
            fprintf(fP, " %s %d", optRuleName((OptRuleId)rule), tableP->findingCounts[rule]);
        }
    }

    fprintf(fP, "\n");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( !tableP->findingCounts[rule] )
        {
            continue;
        }

        fprintf(fP, "\n  %-4s %s (%d)\n", optRuleName((OptRuleId)rule), optRuleSummary((OptRuleId)rule),
            tableP->findingCounts[rule]);
        any = 0;

        for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
        {
            if( findingP->rule != (OptRuleId)rule )
            {
                continue;
            }

            printFinding(fP, findingP, 0);
            any = 1;
        }

        if( !any )
        {
            fprintf(fP, "    (none)\n");
        }
    }
}

// The patterns that matched and were then refused, grouped by rule, with a tally by reason first.
// This is the more informative half, it is where the self-modifying code, the dispatch tables and the
// shared scratch words show up.
void
writeSuppressedList(FILE *fP, OptTableP tableP)
{
int rule;
int why;
OptFindingP findingP;

    fprintf(fP, "\nSuppressed findings: %d\n", tableP->suppressedCount);

    if( !tableP->suppressedCount )
    {
        return;
    }

    fprintf(fP, "  These patterns matched and were refused.  Each line says which\n");
    fprintf(fP, "  precondition, or which fact about the words themselves, blocked it.\n");
    fprintf(fP, "  Per reason:");

    for( why = 0; why < OPTWHY_COUNT; ++why )
    {
        if( tableP->whyCounts[why] )
        {
            fprintf(fP, " %s %d", optWhyName((OptWhy)why), tableP->whyCounts[why]);
        }
    }

    fprintf(fP, "\n");

    for( rule = 0; rule < OPTRULE_COUNT; ++rule )
    {
        if( !tableP->suppressedCounts[rule] )
        {
            continue;
        }

        fprintf(fP, "\n  %-4s %s (%d)\n", optRuleName((OptRuleId)rule), optRuleSummary((OptRuleId)rule),
            tableP->suppressedCounts[rule]);

        for( findingP = tableP->suppressedP; findingP; findingP = findingP->nextP )
        {
            if( findingP->rule != (OptRuleId)rule )
            {
                continue;
            }

            printFinding(fP, findingP, 1);
        }
    }
}

// The statistics, what the program is made of, per bank.
void
writeStatistics(FILE *fP, OptTableP tableP)
{
int i;
int bank;
int kindCounts[OPTK_CONST + 1];
int groupCounts[OPTG_1D + 1];
int spellingCounts[OPTS_OTHER + 1];
int labelCount;
int spelled1D;
int unspelled1D;
int instructions;
int memrefs;
int indirects;
int staticTime;
int waits;
int xcts;
OptWordP entryP;
OptBankP bankP;
OptLabelP labelP;
OptLabelDefP defP;
int unattached;

    fprintf(fP, "\nStatistics\n");
    fprintf(fP, "  Word table: %d words", tableP->count);

    if( tableP->reservedCount )
    {
        fprintf(fP, ", %d reserved by table with no initializer", tableP->reservedCount);
    }

    if( tableP->dupCount )
    {
        fprintf(fP, ", %d at overlaid addresses", tableP->dupCount);
    }

    fprintf(fP, "\n");

    if( tableP->mismatchCount )
    {
        fprintf(fP, "WARNING: %d words whose parser-assigned address differs from the binary generator's\n",
            tableP->mismatchCount);
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !(bankP = tableP->banksP[bank]) )
        {
            continue;
        }

        for( i = 0; i <= OPTK_CONST; ++i )
        {
            kindCounts[i] = 0;
        }

        for( i = 0; i <= OPTG_1D; ++i )
        {
            groupCounts[i] = 0;
        }

        for( i = 0; i <= OPTS_OTHER; ++i )
        {
            spellingCounts[i] = 0;
        }

        labelCount = 0;
        spelled1D = 0;
        unspelled1D = 0;
        instructions = 0;
        memrefs = 0;
        indirects = 0;
        staticTime = 0;
        waits = 0;
        xcts = 0;

        for( i = 0; i < tableP->count; ++i )
        {
            entryP = tableP->entriesPP[i];

            if( entryP->bank != bank )
            {
                continue;
            }

            ++kindCounts[entryP->kind];

            for( labelP = entryP->labelsP; labelP; labelP = labelP->nextP )
            {
                ++labelCount;
            }

            // The decoded view (task A2): reserved words have none.
            if( entryP->flags & OPTF_RESERVED )
            {
                continue;
            }

            ++groupCounts[entryP->decode.group];
            ++spellingCounts[entryP->decode.spelling];

            if( entryP->decode.spelled1D )
            {
                ++spelled1D;
            }
            else if( entryP->decode.uses1D )
            {
                ++unspelled1D;
            }

            // The instruction mix and the static time count the words the
            // program is believed to execute -- the ones the control-flow
            // graph holds -- and not the words whose bits merely decode.
            if( !inGraph(entryP) )
            {
                continue;
            }

            ++instructions;
            staticTime += optWordTime(entryP);

            if( entryP->decode.group == OPTG_MEMREF )
            {
                ++memrefs;

                if( entryP->decode.memIndirect )
                {
                    ++indirects;
                }

                if( entryP->decode.opcode == 010 )
                {
                    ++xcts;
                }
            }
            else if( (entryP->decode.group == OPTG_IOT) && entryP->decode.iotWait )
            {
                ++waits;
            }
        }

        fprintf(fP, "  bank %2d: %5d words, %d labels;", bank, bankP->count, labelCount);

        for( i = 0; i <= OPTK_CONST; ++i )
        {
            if( kindCounts[i] )
            {
                fprintf(fP, " %s %d", kindName((OptKind)i), kindCounts[i]);
            }
        }

        fprintf(fP, "\n");

        // What the bits decode to, whatever the words are for.
        fprintf(fP, "    decoded:");

        for( i = 0; i <= OPTG_1D; ++i )
        {
            if( groupCounts[i] )
            {
                fprintf(fP, " %s %d", optGroupName((OptGroup)i), groupCounts[i]);
            }
        }

        fprintf(fP, "\n    spelled:");

        for( i = 0; i <= OPTS_OTHER; ++i )
        {
            if( spellingCounts[i] )
            {
                fprintf(fP, " %s %d", optSpellingName((OptSpelling)i), spellingCounts[i]);
            }
        }

        fprintf(fP, "\n");

        if( spelled1D || unspelled1D )
        {
            fprintf(fP,
                "    PDP-1D: %d words spelled with a PDP-1D mnemonic, %d words carry PDP-1D-only bits without one\n",
                spelled1D, unspelled1D);
        }

        // The references and the classification (task A3).
        writeReferenceReport(fP, tableP, bank);

        // The control-flow overlay (task A4).
        writeFlowReport(fP, tableP, bank);

        // The instruction mix and the static time (task A5).
        fprintf(fP, "    instructions: %d, %d memory reference (%d indirect), %d non-memory; static time %d us\n",
            instructions, memrefs, indirects, (instructions - memrefs), staticTime);

        if( xcts || waits )
        {
            fprintf(fP,
                "      the static time understates this bank: %d %s include the word they run,\n"
                "      and %d %s for a device\n",
                xcts, (xcts == 1)?"xct doesn't include the word it":"xcts don't include the words they",
                waits, (waits == 1)?"iot waits":"iots wait");
        }
    }

    fprintf(fP, "  Times are 5 us a memory cycle, 10 us for a two-cycle\n");
    fprintf(fP, "  memory reference, 5 us the jump and augmented instructions,\n");
    fprintf(fP, "  5 us more for one indirect step, mul 25 and div 40.\n");

    // Labels that name an address nothing was emitted at.
    unattached = 0;

    for( defP = tableP->labelsP; defP; defP = defP->nextP )
    {
        if( !defP->attached )
        {
            ++unattached;
        }
    }

    if( unattached )
    {
        fprintf(fP, "  %d labels name an address with no emitted word\n", unattached);
    }

    if( tableP->unresolvedCount )
    {
        fprintf(fP, "  %d references to symbols that never resolved were skipped, see stderr\n",
            tableP->unresolvedCount);
    }

    if( tableP->noWordCount )
    {
        fprintf(fP, "  %d references name an address at which nothing was emitted\n", tableP->noWordCount);
    }

    if( tableP->hasStart )
    {
        fprintf(fP, "  Program starts at 0%04o\n", tableP->startAddr);
    }
    else
    {
        fprintf(fP, "  Program ends with stop, no start address\n");
    }
}

// Print the rule dump (-O=rules): one line per finding, the live ones first
// and then the suppressed ones, each carrying everything the report says
// about it in a form a test can compare.  The format is
//
//   live RULE bBANK ADDR nWORDS wWORDS tTEMPS uTIME | word ; word => replace | note
//   supp RULE bBANK ADDR REASON | word ; word => replace | evidence
//
// where a word is its address, its value and its source spelling.  The last
// line is a summary, so a test can check the counts without parsing the rest.
void
optDumpRules(FILE *fP, OptTableP tableP)
{
int i;
int pass;
OptFindingP findingP;
OptWordP entryP;
char spell[OPTMSG_SIZE];

    for( pass = 0; pass < 2; ++pass )
    {
        findingP = (pass)?tableP->suppressedP:tableP->findingsP;

        for( ; findingP; findingP = findingP->nextP )
        {
            fprintf(fP, "%s %-4s b%d %04o", (pass)?"supp":"live", optRuleName(findingP->rule),
                findingP->bank, findingP->addr);

            if( pass )
            {
                fprintf(fP, " %s", optWhyName(findingP->why));
            }
            else
            {
                fprintf(fP, " n%d w%d t%d u%d%s", findingP->wordCount, findingP->saveWords,
                    findingP->saveTemps, findingP->saveTime, (findingP->perHop)?"/hop":"");
            }

            fprintf(fP, " |");

            for( i = 0; i < findingP->wordCount; ++i )
            {
                entryP = findingP->wordsP[i];
                spellWord(entryP, spell, sizeof(spell));
                fprintf(fP, "%s %04o %06o %s", (i)?" ;":"", entryP->addr, entryP->value, spell);
            }

            fprintf(fP, " => %s | %s\n", (findingP->replaceP)?findingP->replaceP:"-",
                (findingP->detailP)?findingP->detailP:"-");
        }
    }

    fprintf(fP, "totals live %d supp %d words %d temps %d time %d\n",
        tableP->findingCount, tableP->suppressedCount, tableP->savedWords,
        tableP->savedTemps, tableP->savedTime);
}
