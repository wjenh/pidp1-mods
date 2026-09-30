/* optreport.c - the am1 optimizer's .opt report and -O=rules dump: one finding,
 * the live and suppressed finding lists, the header notes, the statistics
 * (calling optrefs.c and optflow.c so a bank's evidence prints in one place),
 * and the Transforms section, the record of what -O1 or -O2 rewrote.
 *
 * Runs after yyparse() and before any code generator, reading the parse tree
 * and symbol tables without modifying them; what it derives lives in the
 * OptTable (optimizer.h).  Under -O1 the tree has already been rewritten, but
 * the findings are spelled from the expressions the word table kept, so they
 * still describe the program as written.  Single threaded, called from
 * writeReport() in optimizer.c and from optimize() for the dump.
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
static void writeSbsNote(FILE *fP, OptTableP tableP);

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
    { "tempused",   "the temporary is reached from outside the pattern" },
    { "through-taken", "the address of the word the rule reads through is used as a value, so a pointer can write it" },
    { "unreached",  "a word of the pattern is in unlabeled code no entry reaches, so nothing shows it is an instruction" },
    { "table",      "the pattern is in a table: a run reached only through its address that runs into data" }
};

// The short tag for a refusal reason.
// Returns a static string, "?" for a reason out of range.
const char *
optWhyName(OptWhy why)
{
    if( (why < 0) || (why >= OPTWHY_COUNT) )
    {
        return("?");
    }

    return( optWhyTable[why].tagP );
}

// The report and the dump.

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
        // A refused finding still shows what a naive rewrite would give, since
        // the reason is about it; "would become" so nobody types it in.
        fprintf(fP, "        %s %s\n", (suppressed)?"would become:":"becomes:", findingP->replaceP);
    }

    if( findingP->detailP )
    {
        fprintf(fP, "        %s %s\n", (suppressed)?"because":"note:", findingP->detailP);
    }

    // Where a live finding sits, only when the source declares a region: a
    // refused pattern's place decides nothing, and the region section names
    // its contradicting words.  Outside a region is not a refusal.
    if( !suppressed && (findingP->place != OPTREG_NOREGIONS) )
    {
        if( optTransformLevel() >= 2 )
        {
            fprintf(fP, "        region: %s%s\n", optRegionPlaceText(findingP->place),
                (findingP->place == OPTREG_INSIDE)?", so -O2 applies only H1 here" :
                ", so -O2 applies every heuristic here");
        }
        else
        {
            fprintf(fP, "        region: %s%s\n", optRegionPlaceText(findingP->place),
                (findingP->place == OPTREG_INSIDE)?"" : ", so a transform would not fire here");
        }
    }
}

// The header notes: what the reader is holding, the preconditions, and the one
// the analysis cannot check.  P5's sentence depends on the level and on whether
// the source declares a region; under -O2, P5 is a guess.
void
writeHeaderNotes(FILE *fP, OptTableP tableP)
{
    if( optTransformLevel() >= 2 )
    {
        fprintf(fP, "\nWARNING: -O2 rewrote this program on a guess.  Where it could not tell whether\n");
        fprintf(fP, "code is shared with a device, a sequence-break handler or a timing\n");
        fprintf(fP, "requirement, it guessed, and the guess can be wrong.  Test the program.  The\n");
        fprintf(fP, "Transforms section names the guess behind every rewrite.  A %%%%nooptimize /\n");
        fprintf(fP, "%%%%endnooptimize span around code keeps every optimization level out of it.\n");
        fprintf(fP, "\nThis is an advisory report, and the record of what -O2 changed: the Transforms\n");
        fprintf(fP, "section lists every word it rewrote.  Everything else describes the program as\n");
        fprintf(fP, "written, before any rewrite.\n");
    }
    else if( optTransformLevel() >= 1 )
    {
        fprintf(fP, "\nThis is an advisory report, and the record of what -O1 changed: the Transforms\n");
        fprintf(fP, "section lists every word it rewrote.  Everything else describes the program as\n");
        fprintf(fP, "written, before any rewrite.\n");
    }
    else
    {
        fprintf(fP, "\nThis is an advisory report.  Nothing in the assembled program was changed.\n");
    }

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
    if( optTransformLevel() >= 2 )
    {
        fprintf(fP, "\nP5 is guessed by -O2's heuristics: see What -O2 would assume below.  Inside an\n");
        fprintf(fP, "optimize region your declaration stands in for all but H1.\n");
    }
    else if( tableP->regionCount )
    {
        fprintf(fP, "\nP5 is your declaration and this source makes it: see the Optimize regions\n");
        fprintf(fP, "section below, where it is checked against the evidence.\n");
    }
    else
    {
        fprintf(fP, "\nP5 is not checked here and must be reviewed by hand.\n");
    }

    fprintf(fP, "The assembler cannot know when words are modified behind the program's\n");
    fprintf(fP, "back, for example, code loaded at runtime from the drum.\n");
    fprintf(fP, "Timing is similar; a delay loop for the typewriter, the punch or the display is\n");
    fprintf(fP, "meant to take the time it takes.\n");

    writeSbsNote(fP, tableP);
}

// The header's sequence-break paragraph: which frames optSbsChannels() in
// optflow.c assumed, and why.  An enable in a program whose own code covers
// channel 0's frame is only warned about here; it changes no analysis.
static void
writeSbsNote(FILE *fP, OptTableP tableP)
{
OptWordP firstP;
int count;
int channels;
int startBank;
int startAddr;

    count = optSbsEnableCount(tableP);

    if( !count )
    {
        fprintf(fP, "\nSequence breaks: the program holds no esm, asc or isb, so no break frame is\n");
        fprintf(fP, "assumed and bank 0's 0 to 077 is read as ordinary code and data.  If the\n");
        fprintf(fP, "system can already be on when the program starts, guarding against that is\n");
        fprintf(fP, "the author's part; an lsm among the first instructions does it.\n");
        return;
    }

    firstP = optSbsFirstEnable(tableP);
    channels = optSbsChannels(tableP);
    startBank = ((tableP->startAddr >> 12) & 017);
    startAddr = (tableP->startAddr & ADDRMASK);

    if( !channels )
    {
        fprintf(fP, "\nWarning: the program holds %d enable command%s, the first %s at bank %o %04o,\n",
            count, (count == 1)?"":"s", optSbsEnableName(firstP), firstP->bank, firstP->addr);
        fprintf(fP, "but it starts at 0%04o, so its own code covers channel 0's frame.  No\n", startAddr);
        fprintf(fP, "break frame is assumed.\n");
        return;
    }

    fprintf(fP, "\nSequence breaks: the program holds %d enable command%s, the first %s at\n",
        count, (count == 1)?"":"s", optSbsEnableName(firstP));
    fprintf(fP, "bank %o %04o, so break frames are assumed.  ", firstP->bank, firstP->addr);

    if( !tableP->hasStart )
    {
        fprintf(fP, "The program ends with stop, so\n");
        fprintf(fP, "nothing says how much of bank 0's low core it leaves them: all sixteen\n");
        fprintf(fP, "channels', 0 to 077.\n");
    }
    else if( startBank )
    {
        fprintf(fP, "The program starts in bank %o, so\n", startBank);
        fprintf(fP, "nothing says how much of bank 0's low core it leaves them: all sixteen\n");
        fprintf(fP, "channels', 0 to 077.\n");
    }
    else if( channels == OPTSBS_CHANNELS )
    {
        fprintf(fP, "The start address, 0%04o,\n", startAddr);
        fprintf(fP, "leaves 0 to 077 below it: all sixteen channels'.\n");
    }
    else if( channels == 1 )
    {
        fprintf(fP, "The start address, 0%04o,\n", startAddr);
        fprintf(fP, "leaves channel 0's below it, 0 to 3, and no other.\n");
    }
    else
    {
        fprintf(fP, "The start address, 0%04o,\n", startAddr);
        fprintf(fP, "leaves channels 0 to %d below it, 0 to 0%o.\n", (channels - 1),
            ((channels * OPTSBS_FRAMESIZE) - 1));
    }
}

// The findings, grouped by rule.  A rule with nothing to say is left out; a
// report with no findings still says so.
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

// The patterns that matched and were then refused, grouped by rule, with a tally
// by reason first; the self-modifying code, dispatch tables and shared scratch
// words show up here.
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

// Write the Transforms section, an audit of what -O1 or -O2 rewrote: every
// rewrite in octal and source, every live finding left alone and why, what was
// realized, and the region warning repeated where the reader is looking.
void
writeTransformReport(FILE *fP, OptTableP tableP)
{
OptXformP recP;
OptFindingP findingP;
OptWordP entryP;
OptRegionP regionP;
int fired;
int count;
int level2;
int h;
int first;
int off;

    fired = tableP->xformFates[OPTXF_FIRED];
    level2 = (optTransformLevel() >= 2);

    if( fired )
    {
        fprintf(fP, "\nTransforms (-O%d): %d word%s rewritten\n", (level2)?2:1, fired, (fired == 1)?"":"s");
    }
    else
    {
        fprintf(fP, "\nTransforms (-O%d): nothing rewritten\n", (level2)?2:1);
    }

    if( level2 )
    {
        fprintf(fP, "  -O2 rewrites a word only where a live finding above says it can, only where\n");
        fprintf(fP, "  no heuristic that applies marks a word the rewrite involves, never inside a\n");
        fprintf(fP, "  nooptimize span, and only for the rules that replace one word with one word at\n");
        fprintf(fP, "  the same address: T3, and T8a to T8d.  Outside an optimize region all five\n");
        fprintf(fP, "  heuristics apply, inside one only H1.  Nothing is inserted, nothing is deleted\n");
        fprintf(fP, "  and no label moves, so every other word of the program is where it would have\n");
        fprintf(fP, "  been without -O2.\n");
    }
    else
    {
        fprintf(fP, "  -O1 rewrites a word only where a live finding above says it can, only inside\n");
        fprintf(fP, "  a region you declared, and only for the rules that replace one word with one\n");
        fprintf(fP, "  word at the same address: T3, and T8a to T8d.  Nothing is inserted, nothing is\n");
        fprintf(fP, "  deleted and no label moves, so every other word of the program is where it\n");
        fprintf(fP, "  would have been without -O1.\n");
    }

    // Inline expansion, placement and unrolling change the program's length,
    // unlike the same-address rewrites the paragraph above describes.
    if( tableP->unrollFates[OPTUN_FIRED] )
    {
        fprintf(fP, "  That holds for these rewrites only.  Inline expansion, fall-through\n");
        fprintf(fP, "  placement and loop unrolling, reported last, change the program's length,\n");
        fprintf(fP, "  and every word after a change has moved.\n");
    }
    else if( tableP->inlineFates[OPTIN_FIRED] || tableP->placeFates[OPTPL_FIRED] )
    {
        fprintf(fP, "  That holds for these rewrites only.  Inline expansion and fall-through\n");
        fprintf(fP, "  placement, reported last, change the program's length, and every word\n");
        fprintf(fP, "  after a change has moved.\n");
    }

    // The deleting rules fire inside a declaration, or anywhere under
    // -O=undeclared; the paragraph above is not about them.
    if( tableP->spaceWords && optUndeclared() )
    {
        fprintf(fP, "  Inside an optimize or speed region, or anywhere, since %s was given,\n",
            optUndeclaredSpelling());
        fprintf(fP, "  T1, T1b, T2, T6, T7 and T13 delete a word as well (space mode).  Each is listed\n");
        fprintf(fP, "  with the word it deletes, and Space mode, last, says whether relayout made the\n");
        fprintf(fP, "  deletion; after one, every word moves.\n");
    }
    else if( tableP->spaceWords )
    {
        fprintf(fP, "  Inside an optimize or speed region, T1, T1b, T2, T6, T7 and T13 delete a word\n");
        fprintf(fP, "  as well (space mode).  Each is listed with the word it deletes, and Space mode,\n");
        fprintf(fP, "  last, says whether relayout made the deletion; after one, every word moves.\n");
    }

    // A bisection builds a subset, which the reader must know before reading
    // which words were rewritten.  Every kind of rewrite is numbered and counted.
    if( tableP->xformBisect )
    {
        off = tableP->xformFates[OPTXF_OFF] + tableP->inlineFates[OPTIN_OFF] + tableP->placeFates[OPTPL_OFF] +
            tableP->unrollFates[OPTUN_OFF];
        fprintf(fP, "  Bisection (%s): %d of the %d rewrites that would fire\n", optBisectSpec(),
            off, tableP->xformCandidates + tableP->inlineCandidates + tableP->placeCandidates +
            tableP->unrollCandidates + tableP->spaceCandidates);
        fprintf(fP, "  %s switched off: %d by -O=upto, %d by -O=range, %d by -O=off.  Each is listed\n",
            (off == 1)?"was":"were",
            tableP->xformOffBy[0] + tableP->inlineOffBy[0] + tableP->placeOffBy[0] + tableP->unrollOffBy[0],
            tableP->xformOffBy[1] + tableP->inlineOffBy[1] + tableP->placeOffBy[1] + tableP->unrollOffBy[1],
            tableP->xformOffBy[2] + tableP->inlineOffBy[2] + tableP->placeOffBy[2] + tableP->unrollOffBy[2]);

        // Name only the sections that hold switched-off rewrites.
        if( tableP->unrollCandidates )
        {
            fprintf(fP, "  under Not rewritten (%d), Inline expansion (%d), Fall-through placement (%d)\n",
                tableP->xformFates[OPTXF_OFF], tableP->inlineFates[OPTIN_OFF], tableP->placeFates[OPTPL_OFF]);
            fprintf(fP, "  or Loop unrolling (%d), and every word it names is the word the source wrote.\n",
                tableP->unrollFates[OPTUN_OFF]);
        }
        else if( tableP->placeCandidates )
        {
            fprintf(fP, "  under Not rewritten (%d), Inline expansion (%d) or Fall-through placement (%d),\n",
                tableP->xformFates[OPTXF_OFF], tableP->inlineFates[OPTIN_OFF], tableP->placeFates[OPTPL_OFF]);
            fprintf(fP, "  and every word it names is the word the source wrote.\n");
        }
        else if( tableP->inlineCandidates )
        {
            fprintf(fP, "  under Not rewritten (%d) or Inline expansion (%d), and every word it names is\n",
                tableP->xformFates[OPTXF_OFF], tableP->inlineFates[OPTIN_OFF]);
            fprintf(fP, "  the word the source wrote.\n");
        }
        else
        {
            fprintf(fP, "  under Not rewritten, and every word it names is the word the source wrote.\n");
        }
    }

    if( !level2 && !tableP->regionCount )
    {
        fprintf(fP, "  This source has no %%%%optimize/%%%%endoptimize region, so the program was\n");
        fprintf(fP, "  assembled exactly as it would have been without -O1.  There %s %d live\n",
            (tableP->findingCount == 1)?"is":"are", tableP->findingCount);
        fprintf(fP, "  finding%s above, and none was acted on.\n", (tableP->findingCount == 1)?"":"s");
        return;
    }

    // What fired.

    if( fired )
    {
        fprintf(fP, "\n  Rewritten: %d\n", fired);

        // Under -O2, what each heuristic named on an "assumes:" line means;
        // if the program misbehaves, rewrites whose guess failed are suspect.
        if( level2 )
        {
            fprintf(fP, "  Each rewrite rests on the guess that no heuristic it names found anything:\n");

            for( h = 0; h < OPTGH_COUNT; ++h )
            {
                fprintf(fP, "    %s  %s\n", optGuessName((OptGuessId)h), optGuessAssumption((OptGuessId)h));
            }
        }

        for( recP = tableP->xformsP; recP; recP = recP->nextP )
        {
            if( recP->fate != OPTXF_FIRED )
            {
                continue;
            }

            findingP = recP->findingP;
            entryP = findingP->wordsP[0];

            if( level2 && !recP->regionP )
            {
                fprintf(fP, "    %-4s bank %2d %04o  %s:%d  no region\n", optRuleName(findingP->rule),
                    findingP->bank, findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);
            }
            else
            {
                fprintf(fP, "    %-4s bank %2d %04o  %s:%d  region %d\n", optRuleName(findingP->rule),
                    findingP->bank, findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo,
                    (recP->regionP)?recP->regionP->id:0);
            }

            fprintf(fP, "        before %06o  %s\n", recP->before, (recP->beforeP)?recP->beforeP:"-");
            fprintf(fP, "        after  %06o  %s\n", recP->after, (recP->afterP)?recP->afterP:"-");

            // The word a deleting rule takes out.
            if( recP->delP )
            {
                fprintf(fP, "        deletes bank %2d %04o\n", recP->delP->bank, recP->delP->addr);
            }

            // The heuristics that applied and found nothing; the legend is above.
            if( recP->assumed )
            {
                fprintf(fP, "        assumes:");

                for( h = 0; h < OPTGH_COUNT; ++h )
                {
                    if( recP->assumed & (1u << h) )
                    {
                        fprintf(fP, " %s", optGuessName((OptGuessId)h));
                    }
                }

                fprintf(fP, "\n");
            }
        }
    }

    // What did not fire, and why.

    fprintf(fP, "\n  Not rewritten: %d\n", (tableP->findingCount - fired));

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( recP->fate == OPTXF_FIRED )
        {
            continue;
        }

        findingP = recP->findingP;
        entryP = findingP->wordsP[0];
        fprintf(fP, "    %-4s bank %2d %04o  %s:%d  %s", optRuleName(findingP->rule),
            findingP->bank, findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo,
            optXformFateText(recP->fate));

        // The heuristics that left it alone; What -O2 would assume explains each.
        if( (recP->fate == OPTXF_GUESSED) && recP->assumed )
        {
            fprintf(fP, ":");
            first = 1;

            for( h = 0; h < OPTGH_COUNT; ++h )
            {
                if( recP->assumed & (1u << h) )
                {
                    fprintf(fP, "%s%s", (first)?" ":", ", optGuessName((OptGuessId)h));
                    first = 0;
                }
            }
        }

        // The modifiers that switched it off, and its place in the order
        // -O=upto counts.
        if( recP->fate == OPTXF_OFF )
        {
            fprintf(fP, ":");
            first = 1;

            for( h = 0; h < OPTBIS_COUNT; ++h )
            {
                if( recP->offBy & (1u << h) )
                {
                    fprintf(fP, "%s-O=%s", (first)?" ":", ", optBisectName(1u << h));
                    first = 0;
                }
            }

            fprintf(fP, ", rewrite #%d", recP->ordinal);
        }

        fprintf(fP, "\n");
    }

    if( tableP->xformRefusedInside )
    {
        fprintf(fP, "  %d pattern%s inside a region %s refused by the evidence, and %s listed with\n",
            tableP->xformRefusedInside, (tableP->xformRefusedInside == 1)?"":"s",
            (tableP->xformRefusedInside == 1)?"was":"were", (tableP->xformRefusedInside == 1)?"is":"are");
        fprintf(fP, "  the reason under Suppressed findings above.  -O%d never acts on a refused pattern.\n",
            (level2)?2:1);
    }

    // What it realized.

    // Relayout makes the deletions after this section; Space mode says how many.
    if( tableP->spaceWords )
    {
        fprintf(fP, "\n  Realized: up to %d code word%s, by the deletions above; Space mode, last,\n",
            tableP->spaceWords, (tableP->spaceWords == 1)?"":"s");
        fprintf(fP, "  says how many relayout made.  No other rule -O%d carries removes a word.\n", (level2)?2:1);
    }
    else
    {
        fprintf(fP, "\n  Realized: 0 code words.  Only a deletion removes a word, and -O%d makes one\n", (level2)?2:1);

        if( level2 )
        {
            fprintf(fP, "  only inside an optimize or speed region, or anywhere under -O=undeclared;\n");
            fprintf(fP, "  none was made here.\n");
        }
        else
        {
            fprintf(fP, "  only inside an optimize or speed region; none was made here.\n");
        }
    }
    // A freed pool word is collected only where a declaration, or
    // -O=undeclared, licensed every rewrite that stopped naming it, and
    // relayout decides that after this section is written, so Pool reclaim
    // says what went.
    if( tableP->poolFreeable && optUndeclared() )
    {
        fprintf(fP, "  %d storage word%s freed, every one licensed by %s or a\n",
            tableP->xformFreed, (tableP->xformFreed == 1)?"":"s", optUndeclaredSpelling());
        fprintf(fP, "  declaration: a pool word no rewritten word names any more.  Pool reclaim,\n");
        fprintf(fP, "  last, says how many went.\n");
    }
    else if( tableP->poolFreeable )
    {
        fprintf(fP, "  %d storage word%s freed, %d of them inside a declaration: a pool word no\n",
            tableP->xformFreed, (tableP->xformFreed == 1)?"":"s", tableP->poolFreeable);
        fprintf(fP, "  rewritten word names any more.  Pool reclaim, last, says how many went; one\n");
        fprintf(fP, "  freed on the guess alone stays, because collecting it would make a same-length\n");
        fprintf(fP, "  rewrite one that moves every word after it.\n");
    }
    else
    {
        fprintf(fP, "  %d storage word%s freed and not reclaimed: a pool word no rewritten word names\n",
            tableP->xformFreed, (tableP->xformFreed == 1)?"":"s");
        fprintf(fP, "  any more is collected only where a declaration licensed every rewrite that\n");
        fprintf(fP, "  stopped naming it, and one freed on the guess alone stays where it is.\n");
    }
    fprintf(fP, "  %d microsecond%s from one pass through every rewritten word%s.\n", tableP->xformTime,
        (tableP->xformTime == 1)?"":"s", (tableP->xformHops)?" but the jmps":"");

    if( tableP->xformHops )
    {
        fprintf(fP, "  %d rewritten jmp%s, which save%s a cycle only on a hop actually taken: %d\n",
            tableP->xformHops, (tableP->xformHops == 1)?"":"s", (tableP->xformHops == 1)?"s":"",
            tableP->xformHopTime);
        fprintf(fP, "  microseconds for one hop through each.\n");
    }

    if( tableP->xformThroughTaken || tableP->xformThroughMaybe )
    {
        fprintf(fP, "  Of the rewritten words, %d read through a word whose address is used as a\n",
            tableP->xformThroughTaken);
        fprintf(fP, "  value, and %d through one that may be written through a pointer the analysis\n",
            tableP->xformThroughMaybe);
        fprintf(fP, "  could not follow.  No rule consults either mark and -O%d adds no condition to a\n",
            (level2)?2:1);
        fprintf(fP, "  rule; -O=xform names each such word.\n");
    }

    // The region warning, repeated.

    if( !tableP->regionContradictions )
    {
        return;
    }

    fprintf(fP, "\n  WARNING: the evidence contradicts your region declaration at %d word%s; the\n",
        tableP->regionContradictions, (tableP->regionContradictions == 1)?"":"s");
    fprintf(fP, "  Optimize regions section above names each one.\n");

    if( !tableP->xformContradicted )
    {
        fprintf(fP, "  No word rewritten above is inside a region the evidence disputes, but the\n");
        fprintf(fP, "  declaration is still disputed and is worth settling before relying on it.\n");
        return;
    }

    fprintf(fP, "  %d of the words rewritten above %s inside a region the evidence disputes:\n",
        tableP->xformContradicted, (tableP->xformContradicted == 1)?"is":"are");

    for( regionP = tableP->regionsP; regionP; regionP = regionP->nextP )
    {
        if( !regionP->contradictions )
        {
            continue;
        }

        count = 0;

        for( recP = tableP->xformsP; recP; recP = recP->nextP )
        {
            if( (recP->fate == OPTXF_FIRED) && (recP->regionP == regionP) )
            {
                ++count;
            }
        }

        if( count )
        {
            fprintf(fP, "    region %d  %s lines %d to %d: %d rewritten, %d word%s contradicted\n", regionP->id,
                (regionP->fileP)?regionP->fileP:"-", regionP->openLine, regionP->endLine, count,
                regionP->contradictions, (regionP->contradictions == 1)?"":"s");
        }
    }

    fprintf(fP, "  Each of those words passed P1 to P4 on its own evidence, which is why it was\n");
    fprintf(fP, "  rewritten.  But the region it is in is one you declared free of modified,\n");
    fprintf(fP, "  address-taken and timing code, and the analysis says otherwise.  This is the\n");
    fprintf(fP, "  case the region section warns about; settle it before trusting this program.\n");
}

// The statistics: what the program is made of, per bank.
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

            // The decoded view; reserved words have none.
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

            // The instruction mix and static time count the words the flow
            // graph holds, which the program is believed to execute, not every
            // word whose bits decode.
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

        // The references and their classification.
        writeReferenceReport(fP, tableP, bank);

        // The control-flow overlay.
        writeFlowReport(fP, tableP, bank);

        // The instruction mix and the static time.
        fprintf(fP, "    instructions: %d, %d memory reference (%d indirect), %d non-memory; static time %d us\n",
            instructions, memrefs, indirects, (instructions - memrefs), staticTime);

        if( xcts || waits )
        {
            fprintf(fP,
                "      the static time understates this bank: %d xct%s don't include the word they run,\n"
                "      and %d iot%s wait for a device\n",
                xcts, (xcts == 1)?"":"s", waits, (waits == 1)?"":"s");
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

// Print the rule dump (-O=rules): one line per finding, live then suppressed,
// in a form a test can compare, and a totals line last.  A word is its address,
// value and source spelling:
//   live RULE bBANK ADDR nWORDS wWORDS tTEMPS uTIME | word ; word => replace | note
//   supp RULE bBANK ADDR REASON | word ; word => replace | evidence
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
