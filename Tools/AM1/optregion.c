/* optregion.c - the am1 optimizer's source declarations: the %%optimize,
 * %%nooptimize and %%speed spans, the 'inline NAME' markings and the %%ceiling
 * directives.  lexer.l and parser.y recognize them; the four code generators
 * step over their nodes and emit nothing.
 *
 * A region is an author's permission to rewrite, not a condition on being told:
 * the report advises everywhere, and a finding outside every region is still
 * live, only marked not declared.  So where a pattern sits is a property of a
 * finding (OptRegionPlace), never an OptWhy reason for suppressing it.
 *
 * Membership is a span of emission order, not an address range.  During
 * optBuildTable()'s walk, optRegionStatement() takes the directive nodes and
 * optRegionNoteWord() marks each word as it is created, so a bank change, an
 * overlay or a macro expansion inside a span needs no range arithmetic.  After
 * the rules, optCheckRegions() checks each region against the reference flags
 * and classifies the findings, writing only its own fields; opttransform.c
 * reads OptFinding.place and the OPTF_ flags set here.  Single threaded, one
 * run per optimize(); freeRegionList() releases everything from optFreeTable().
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

// The region the builder's walk is inside, NILP between regions.  The parser
// refuses nesting, so one pointer is the whole state.
static OptRegionP openRegionP;

// The nooptimize span the walk is inside, NILP between spans.
static OptRegionP openHandsOffP;

// The speed region the walk is inside, NILP between them.  The three kinds are
// independent: a word may be in any combination of them.
static OptRegionP openSpeedP;

// Where a finding's pattern sits, for the report and the dump.  Indexed by
// OptRegionPlace, so the order here is that enum's order.
static const struct
{
    const char *tagP;       // the dump's word
    const char *textP;      // the report's phrase
} placeTable[OPTREG_COUNT] =
{
    { "noregions", "no region declared" },
    { "inside",    "inside a declared region" },
    { "outside",   "outside every declared region" },
    { "partial",   "straddling a region boundary" }
};

static OptRegionP newRegion(OptTableP tableP, PNodeP nodeP, int bank, int pc);
static OptRegionP newHandsOff(OptTableP tableP, PNodeP nodeP, int bank, int pc);
static OptRegionP newSpeedRegion(OptTableP tableP, PNodeP nodeP, int bank, int pc);
static void newInlineMark(OptTableP tableP, PNodeP nodeP, int bank, int pc);
static void newCeiling(OptTableP tableP, PNodeP nodeP);
static void checkCeilings(OptTableP tableP);
static void bankHighest(OptTableP tableP, int *highest);
static void resolveInlineMarks(OptTableP tableP);
static int routineHasLabel(OptTableP tableP, OptRoutineP routineP, const char *nameP);
static void noteSpanWord(OptRegionP spanP, OptWordP entryP);
static OptRegionP spanOfIndex(OptRegionP listP, int index);
static OptRegionP regionOfIndex(OptTableP tableP, int index);
static OptRegionPlace placeOfFinding(OptTableP tableP, OptFindingP findingP);
static void spellEvidence(OptWordP entryP, char *bufP, int size);

// Building: called from the word table walk.

// Clear the region state at the start of a run.
void
optRegionReset(void)
{
    openRegionP = NILP;
    openHandsOffP = NILP;
    openSpeedP = NILP;
}

// Take one directive node as the builder's walk reaches it; bank and pc are
// where the next word would go.  The parser has already refused every bad
// nesting fatally, so an unexpected node is ignored, not diagnosed again.
void
optRegionStatement(OptTableP tableP, PNodeP nodeP, int bank, int pc)
{
    // An 'inline NAME' node is a point, not a span: it opens and closes nothing.
    if( nodeP->flags & PN_INLINE )
    {
        newInlineMark(tableP, nodeP, bank, pc);
        return;
    }

    // So is a '%%ceiling' node.
    if( nodeP->flags & PN_CEILING )
    {
        newCeiling(tableP, nodeP);
        return;
    }

    // A speed or endspeed node opens or closes a speed region.
    if( nodeP->flags & PN_SPEED )
    {
        if( nodeP->type == OPTIMIZE )
        {
            if( !openSpeedP )
            {
                openSpeedP = newSpeedRegion(tableP, nodeP, bank, pc);
            }
        }
        else if( openSpeedP )
        {
            openSpeedP->endLine = nodeP->lineNo;
            openSpeedP = NILP;
        }

        return;
    }

    // A nooptimize or endnooptimize node opens or closes a hands-off span.
    if( nodeP->flags & PN_HANDSOFF )
    {
        if( nodeP->type == OPTIMIZE )
        {
            if( !openHandsOffP )
            {
                openHandsOffP = newHandsOff(tableP, nodeP, bank, pc);
            }
        }
        else if( openHandsOffP )
        {
            openHandsOffP->endLine = nodeP->lineNo;
            openHandsOffP = NILP;
        }

        return;
    }

    if( nodeP->type == OPTIMIZE )
    {
        if( !openRegionP )
        {
            openRegionP = newRegion(tableP, nodeP, bank, pc);
        }

        return;
    }

    if( openRegionP )
    {
        openRegionP->endLine = nodeP->lineNo;
        openRegionP = NILP;
    }
}

// The flags a word created now carries from the open spans: any combination of
// OPTF_INREGION, OPTF_HANDSOFF and OPTF_INSPEED, or 0.
unsigned int
optRegionWordFlags(void)
{
    return( ((openRegionP)?OPTF_INREGION:0) | ((openHandsOffP)?OPTF_HANDSOFF:0)
        | ((openSpeedP)?OPTF_INSPEED:0) );
}

// Record a word just added to the table against the spans open now.  Entries
// are indexed in emission order and never reordered, so extending a span by
// index stays exact even when a bank change inside it moves the address.
void
optRegionNoteWord(OptTableP tableP, OptWordP entryP)
{
    if( openHandsOffP )
    {
        noteSpanWord(openHandsOffP, entryP);
        ++tableP->handsOffWords;
    }

    // Counted per bank too: the speed budget is a per-bank one.
    if( openSpeedP )
    {
        noteSpanWord(openSpeedP, entryP);
        ++tableP->speedWords;

        if( (entryP->bank >= 0) && (entryP->bank <= MAXBANK) )
        {
            ++tableP->speedBankWords[entryP->bank];
        }
    }

    if( !openRegionP )
    {
        return;
    }

    noteSpanWord(openRegionP, entryP);
    ++tableP->regionWords;

    if( (entryP->bank >= 0) && (entryP->bank <= MAXBANK) )
    {
        ++tableP->regionBankWords[entryP->bank];
    }
}

// Make a region and append it to the table's list, which stays in source order.
// Returns the new region; out of memory is fatal.
static OptRegionP
newRegion(OptTableP tableP, PNodeP nodeP, int bank, int pc)
{
OptRegionP regionP;

    if( !(regionP = (OptRegionP)calloc(1, sizeof(OptRegion))) )
    {
        fprintf(stderr, "am1: out of memory recording an optimize region\n");
        exit(1);
    }

    regionP->id = (tableP->regionCount + 1);
    regionP->openLine = nodeP->lineNo;
    regionP->endLine = -1;
    regionP->fileP = nodeP->value.strP;
    regionP->openBank = bank;
    regionP->openAddr = pc;
    regionP->firstIndex = -1;
    regionP->lastIndex = -1;

    if( tableP->regionsTailP )
    {
        tableP->regionsTailP->nextP = regionP;
    }
    else
    {
        tableP->regionsP = regionP;
    }

    tableP->regionsTailP = regionP;
    ++tableP->regionCount;
    return(regionP);
}

// Make a nooptimize span: an OptRegion, on the span list.
// Returns the new span; out of memory is fatal.
static OptRegionP
newHandsOff(OptTableP tableP, PNodeP nodeP, int bank, int pc)
{
OptRegionP spanP;

    if( !(spanP = (OptRegionP)calloc(1, sizeof(OptRegion))) )
    {
        fprintf(stderr, "am1: out of memory recording a nooptimize span\n");
        exit(1);
    }

    spanP->id = (tableP->handsOffCount + 1);
    spanP->openLine = nodeP->lineNo;
    spanP->endLine = -1;
    spanP->fileP = nodeP->value.strP;
    spanP->openBank = bank;
    spanP->openAddr = pc;
    spanP->firstIndex = -1;
    spanP->lastIndex = -1;

    if( tableP->handsOffTailP )
    {
        tableP->handsOffTailP->nextP = spanP;
    }
    else
    {
        tableP->handsOffP = spanP;
    }

    tableP->handsOffTailP = spanP;
    ++tableP->handsOffCount;
    return(spanP);
}

// Make a speed region: an OptRegion, on the speed list.
// Returns the new region; out of memory is fatal.
static OptRegionP
newSpeedRegion(OptTableP tableP, PNodeP nodeP, int bank, int pc)
{
OptRegionP speedRegionP;

    if( !(speedRegionP = (OptRegionP)calloc(1, sizeof(OptRegion))) )
    {
        fprintf(stderr, "am1: out of memory recording a speed region\n");
        exit(1);
    }

    speedRegionP->id = (tableP->speedCount + 1);
    speedRegionP->openLine = nodeP->lineNo;
    speedRegionP->endLine = -1;
    speedRegionP->fileP = nodeP->value.strP;
    speedRegionP->openBank = bank;
    speedRegionP->openAddr = pc;
    speedRegionP->firstIndex = -1;
    speedRegionP->lastIndex = -1;

    if( tableP->speedTailP )
    {
        tableP->speedTailP->nextP = speedRegionP;
    }
    else
    {
        tableP->speedP = speedRegionP;
    }

    tableP->speedTailP = speedRegionP;
    ++tableP->speedCount;
    return(speedRegionP);
}

// Record an 'inline NAME' marking.  The name is resolved by resolveInlineMarks()
// once the call graph exists; bank and pc let the report place a marking that
// names nothing.
static void
newInlineMark(OptTableP tableP, PNodeP nodeP, int bank, int pc)
{
OptInlineMarkP markP;

    if( !(markP = (OptInlineMarkP)calloc(1, sizeof(OptInlineMark))) )
    {
        fprintf(stderr, "am1: out of memory recording an inline marking\n");
        exit(1);
    }

    markP->id = (tableP->inlineMarkCount + 1);
    markP->nameP = nodeP->value2.strP;
    markP->fileP = nodeP->value.strP;
    markP->line = nodeP->lineNo;
    markP->declBank = bank;
    markP->declAddr = pc;
    markP->routineP = NILP;
    markP->dupOf = 0;

    if( tableP->inlineMarksTailP )
    {
        tableP->inlineMarksTailP->nextP = markP;
    }
    else
    {
        tableP->inlineMarksP = markP;
    }

    tableP->inlineMarksTailP = markP;
    ++tableP->inlineMarkCount;
}

// Record a '%%ceiling' directive, making it its bank's ceiling if it is the
// lowest the bank has declared.  The parser has already evaluated it and
// refused 0 and any value above the default.
static void
newCeiling(OptTableP tableP, PNodeP nodeP)
{
OptCeilingP ceilP;

    if( !(ceilP = (OptCeilingP)calloc(1, sizeof(OptCeiling))) )
    {
        fprintf(stderr, "am1: out of memory recording a ceiling\n");
        exit(1);
    }

    ceilP->id = (tableP->ceilingCount + 1);
    ceilP->bank = nodeP->bank;
    ceilP->value = nodeP->value2.ival;
    ceilP->fileP = nodeP->value.strP;
    ceilP->line = nodeP->lineNo;

    if( tableP->ceilingsTailP )
    {
        tableP->ceilingsTailP->nextP = ceilP;
    }
    else
    {
        tableP->ceilingsP = ceilP;
    }

    tableP->ceilingsTailP = ceilP;
    ++tableP->ceilingCount;

    // The lowest wins; at a tie the first stays the winner.
    if( !tableP->bankCeiling[ceilP->bank] || (ceilP->value < tableP->bankCeiling[ceilP->bank]) )
    {
        tableP->bankCeiling[ceilP->bank] = ceilP->value;
        tableP->bankCeilingId[ceilP->bank] = ceilP->id;
    }
}

// Fill highest[0..MAXBANK] with each bank's highest assembled word, -1 for a
// bank with none; the budgets in optspeed.c and opttransform.c count from it.
static void
bankHighest(OptTableP tableP, int *highest)
{
OptWordP wordP;
int bank;
int i;

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
}

// Refuse a ceiling the assembled program already reaches, whose budget would be
// negative.  Every directive is checked, not only each bank's winner, so the
// message names the line to fix.  Exits when a ceiling is crossed.
static void
checkCeilings(OptTableP tableP)
{
OptCeilingP ceilP;
int highest[MAXBANK + 1];

    if( !tableP->ceilingCount )
    {
        return;
    }

    bankHighest(tableP, highest);

    for( ceilP = tableP->ceilingsP; ceilP; ceilP = ceilP->nextP )
    {
        if( highest[ceilP->bank] >= ceilP->value )
        {
            fprintf(stderr, "am1: %%%%ceiling %04o at line %d, file %s: bank %d's highest assembled word is %04o, at or above it; "
                "the program already crosses the ceiling\n", ceilP->value, ceilP->line,
                (ceilP->fileP)?ceilP->fileP:"-", ceilP->bank, highest[ceilP->bank]);
            exit(1);
        }
    }
}

// Report whether a routine's entry word carries the label nameP: 1 if so, else
// 0.  Every label is compared, not just firstLabelName()'s, so a marking on an
// entry's second label still resolves.
static int
routineHasLabel(OptTableP tableP, OptRoutineP routineP, const char *nameP)
{
OptWordP entryP;
OptLabelP labelP;

    if( !nameP || !(entryP = wordAt(tableP, routineP->bank, routineP->entryAddr)) )
    {
        return(0);
    }

    for( labelP = entryP->labelsP; labelP; labelP = labelP->nextP )
    {
        if( labelP->symP && labelP->symP->name && !strcmp(labelP->symP->name, nameP) )
        {
            return(1);
        }
    }

    return(0);
}

// Match every 'inline' marking against the call graph's routines by the labels
// on their entry words, since a routine has no name of its own.  An unmatched
// marking stays unresolved for the report; a repeat marking gets dupOf.
static void
resolveInlineMarks(OptTableP tableP)
{
OptInlineMarkP markP;
OptInlineMarkP earlierP;
OptRoutineP routineP;

    for( markP = tableP->inlineMarksP; markP; markP = markP->nextP )
    {
        for( routineP = tableP->routinesP; routineP; routineP = routineP->nextP )
        {
            if( routineHasLabel(tableP, routineP, markP->nameP) )
            {
                markP->routineP = routineP;
                ++tableP->inlineMarkResolved;
                break;
            }
        }

        if( !markP->routineP )
        {
            continue;
        }

        for( earlierP = tableP->inlineMarksP; earlierP != markP; earlierP = earlierP->nextP )
        {
            if( earlierP->routineP == markP->routineP )
            {
                markP->dupOf = earlierP->id;
                ++tableP->inlineMarkDuplicates;
                break;
            }
        }
    }
}

// Extend a region's or span's index range with a word just added.
static void
noteSpanWord(OptRegionP spanP, OptWordP entryP)
{
    if( spanP->firstIndex < 0 )
    {
        spanP->firstIndex = entryP->index;
    }

    spanP->lastIndex = entryP->index;
    ++spanP->wordCount;
}

// Checking: the declaration against the evidence, and the findings.

// Check every region's declaration against the reference flags, classify every
// finding by where its pattern sits, and resolve the inline markings.
void
optCheckRegions(OptTableP tableP)
{
OptRegionP regionP;
OptFindingP findingP;
OptWordP entryP;
int i;

    checkCeilings(tableP);

    // A region declares its code unmodified at run time and no address in it
    // used as a value; a word in it written, patched or taken contradicts that.

    for( regionP = tableP->regionsP; regionP; regionP = regionP->nextP )
    {
        if( regionP->firstIndex < 0 )
        {
            continue;
        }

        for( i = regionP->firstIndex; i <= regionP->lastIndex; ++i )
        {
            entryP = tableP->entriesPP[i];

            if( !(entryP->flags & OPTF_INREGION) )
            {
                continue;
            }

            if( entryP->flags & (OPTF_WRITTEN | OPTF_TAKEN | OPTF_PATCHED) )
            {
                ++regionP->contradictions;
                ++tableP->regionContradictions;
            }
        }
    }

    // Both lists are classified so the field is never stale, but only the live
    // findings are counted and reported.
    for( findingP = tableP->suppressedP; findingP; findingP = findingP->nextP )
    {
        findingP->place = placeOfFinding(tableP, findingP);
    }

    for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        findingP->place = placeOfFinding(tableP, findingP);
        ++tableP->regionFindings[findingP->place];
    }

    // Markings are resolved here, not in the walk: the routines come from the
    // call graph, built after it.
    resolveInlineMarks(tableP);
}

// Decide where a finding's pattern sits.  It is inside only when every word is:
// a transform rewrites the whole pattern or none of it.
static OptRegionPlace
placeOfFinding(OptTableP tableP, OptFindingP findingP)
{
int i;
int in;
int out;

    if( !tableP->regionCount )
    {
        return(OPTREG_NOREGIONS);
    }

    in = 0;
    out = 0;

    for( i = 0; i < findingP->wordCount; ++i )
    {
        if( findingP->wordsP[i]->flags & OPTF_INREGION )
        {
            ++in;
        }
        else
        {
            ++out;
        }
    }

    if( in && out )
    {
        return(OPTREG_PARTIAL);
    }

    return( (in)?OPTREG_INSIDE:OPTREG_OUTSIDE );
}

// Find the region holding an entry index.
// Returns the region, or NILP when the index is in none.
static OptRegionP
regionOfIndex(OptTableP tableP, int index)
{
    return( spanOfIndex(tableP->regionsP, index) );
}

// Find the member of a region or span list holding an entry index.  Members of
// one list never nest, so the first match is the only one.
// Returns the span, or NILP when the index is in none.
static OptRegionP
spanOfIndex(OptRegionP listP, int index)
{
OptRegionP regionP;

    for( regionP = listP; regionP; regionP = regionP->nextP )
    {
        if( regionP->firstIndex < 0 )
        {
            continue;
        }

        if( (index >= regionP->firstIndex) && (index <= regionP->lastIndex) )
        {
            return(regionP);
        }
    }

    return(NILP);
}

// Find the nooptimize span a word is in.
// Returns the span, or NILP when the word is in none.
OptRegionP
optHandsOffOfWord(OptTableP tableP, OptWordP entryP)
{
    if( !(entryP->flags & OPTF_HANDSOFF) )
    {
        return(NILP);
    }

    return( spanOfIndex(tableP->handsOffP, entryP->index) );
}

// Find the region a word is in, for -O1's record of what it rewrote.
// Returns the region, or NILP when the word is in none.
OptRegionP
optRegionOfWord(OptTableP tableP, OptWordP entryP)
{
    if( !(entryP->flags & OPTF_INREGION) )
    {
        return(NILP);
    }

    return( regionOfIndex(tableP, entryP->index) );
}

// Find the speed region a word is in.
// Returns the region, or NILP when the word is in none.
OptRegionP
optSpeedOfWord(OptTableP tableP, OptWordP entryP)
{
    if( !(entryP->flags & OPTF_INSPEED) )
    {
        return(NILP);
    }

    return( spanOfIndex(tableP->speedP, entryP->index) );
}

// Find a routine's 'inline' marking, which licenses inlining its calls outside
// any speed region.  Duplicates are skipped, so the answer is always the first.
// Returns the marking, or NILP when the routine was not marked.
OptInlineMarkP
optInlineMarkOfRoutine(OptTableP tableP, OptRoutineP routineP)
{
OptInlineMarkP markP;

    if( !routineP )
    {
        return(NILP);
    }

    for( markP = tableP->inlineMarksP; markP; markP = markP->nextP )
    {
        if( (markP->routineP == routineP) && !markP->dupOf )
        {
            return(markP);
        }
    }

    return(NILP);
}

// Spell out a word's reference flags for the contradiction lines: every one set,
// in the order written, patched, taken.  bufP is always left null terminated.
static void
spellEvidence(OptWordP entryP, char *bufP, int size)
{
int used;

    used = 0;
    *bufP = '\0';

    if( entryP->flags & OPTF_WRITTEN )
    {
        used += snprintf((bufP + used), (size - used), "written");
    }

    if( (entryP->flags & OPTF_PATCHED) && (used < size) )
    {
        used += snprintf((bufP + used), (size - used), "%spatched", (used)?" ":"");
    }

    if( (entryP->flags & OPTF_TAKEN) && (used < size) )
    {
        snprintf((bufP + used), (size - used), "%staken", (used)?" ":"");
    }
}

// Name where a finding's pattern sits, as one word for the dump.
// Returns a static string, "?" for a place out of range.
const char *
optRegionPlaceName(OptRegionPlace place)
{
    if( (place < 0) || (place >= OPTREG_COUNT) )
    {
        return("?");
    }

    return( placeTable[place].tagP );
}

// The same thing as a phrase, for the finding lines in the report.
// Returns a static string, "?" for a place out of range.
const char *
optRegionPlaceText(OptRegionPlace place)
{
    if( (place < 0) || (place >= OPTREG_COUNT) )
    {
        return("?");
    }

    return( placeTable[place].textP );
}

// The report section and the dump.

// Write the report's region section: the regions, how the live findings fall
// against them, then the contradictions, last because they are what a reader
// acts on.  A source with no region gets a short paragraph saying so.
void
writeRegionReport(FILE *fP, OptTableP tableP)
{
OptRegionP regionP;
OptWordP entryP;
int bank;
int i;
char evidence[64];

    if( !tableP->regionCount )
    {
        fprintf(fP, "\nOptimize regions: none declared\n");
        fprintf(fP, "  This source has no %%%%optimize/%%%%endoptimize region, so precondition P5 is\n");
        fprintf(fP, "  not checked and nothing above has been withheld for want of one.  A region\n");

        // What a region licenses depends on the level.
        if( optTransformLevel() >= 2 )
        {
            fprintf(fP, "  is how an author tells -O1 which code it may rewrite.  -O2 needs none: under\n");
            fprintf(fP, "  -O2 a region only switches off heuristics H2 to H5 inside it.  A region\n");
            fprintf(fP, "  removes no advice.\n");
        }
        else if( optTransformLevel() >= 1 )
        {
            fprintf(fP, "  is how an author tells -O1 which code it may rewrite, and without one -O1\n");
            fprintf(fP, "  rewrites nothing.  A region removes no advice.\n");
        }
        else
        {
            fprintf(fP, "  is how an author tells -O1 which code it may rewrite; under -O alone it\n");
            fprintf(fP, "  removes no advice and it changes no output byte.\n");
        }
        return;
    }

    fprintf(fP, "\nOptimize regions: %d, holding %d word%s\n", tableP->regionCount,
        tableP->regionWords, (tableP->regionWords == 1)?"":"s");
    fprintf(fP, "  A region is your declaration about the code inside it: that no word of it is\n");
    fprintf(fP, "  modified while the program runs, that no word's address is used as a value,\n");
    fprintf(fP, "  and that none of it is there to take the time it takes.  That is precondition\n");
    fprintf(fP, "  P5, which the assembler cannot derive and which you can.\n");
    if( optTransformLevel() >= 2 )
    {
        // Under -O2 a region is not a permission, only a switch for H2 to H5.
        fprintf(fP, "  THIS REPORT ADVISES EVERYWHERE.  Under -O2 a region is not a permission:\n");
        fprintf(fP, "  -O2 may rewrite outside every region, on its guess.  Inside one it trusts\n");
        fprintf(fP, "  your declaration in place of heuristics H2 to H5, and still applies H1,\n");
        fprintf(fP, "  P1 to P4 and every nooptimize span.  The Transforms section below says what\n");
        fprintf(fP, "  it rewrote.\n");
    }
    else
    {
        if( optTransformLevel() >= 1 )
        {
            fprintf(fP, "  THIS REPORT ADVISES EVERYWHERE.  A region says where -O1 is ALLOWED to\n");
            fprintf(fP, "  rewrite, and the Transforms section below says what it rewrote.  A finding\n");
        }
        else
        {
            fprintf(fP, "  THIS REPORT ADVISES EVERYWHERE.  A region says where -O1 would be ALLOWED\n");
            fprintf(fP, "  to rewrite, and -O alone rewrites nothing.  A finding\n");
        }

        fprintf(fP, "  outside every region is not refused and has not been moved or hidden; it is\n");
        fprintf(fP, "  simply not opted in, and a transform would not fire on it.\n");
    }

    for( regionP = tableP->regionsP; regionP; regionP = regionP->nextP )
    {
        fprintf(fP, "\n  region %d  %s lines %d to %d\n", regionP->id,
            (regionP->fileP)?regionP->fileP:"-", regionP->openLine, regionP->endLine);

        if( regionP->firstIndex < 0 )
        {
            fprintf(fP, "    holds no emitted word.  It opened at bank %d %04o, where the next word\n",
                regionP->openBank, regionP->openAddr);
            fprintf(fP, "    would have gone.\n");
            continue;
        }

        fprintf(fP, "    bank %d %04o to bank %d %04o, %d word%s\n",
            tableP->entriesPP[regionP->firstIndex]->bank,
            tableP->entriesPP[regionP->firstIndex]->addr,
            tableP->entriesPP[regionP->lastIndex]->bank,
            tableP->entriesPP[regionP->lastIndex]->addr,
            regionP->wordCount, (regionP->wordCount == 1)?"":"s");

        if( tableP->entriesPP[regionP->firstIndex]->bank != tableP->entriesPP[regionP->lastIndex]->bank )
        {
            fprintf(fP, "    The region spans a bank change.  Membership is per emitted word and\n");
            fprintf(fP, "    not an address range, so every word between the two directives is in\n");
            fprintf(fP, "    it whichever bank it landed in.\n");
        }
    }

    fprintf(fP, "\n  Words inside a region, per bank:");

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( tableP->regionBankWords[bank] )
        {
            fprintf(fP, " bank %d %d", bank, tableP->regionBankWords[bank]);
        }
    }

    fprintf(fP, "\n");

    fprintf(fP, "  Live findings inside a region: %d.  Outside every region: %d.",
        tableP->regionFindings[OPTREG_INSIDE], tableP->regionFindings[OPTREG_OUTSIDE]);

    if( tableP->regionFindings[OPTREG_PARTIAL] )
    {
        fprintf(fP, "  Straddling one: %d.", tableP->regionFindings[OPTREG_PARTIAL]);
    }

    fprintf(fP, "\n");

    if( tableP->regionFindings[OPTREG_PARTIAL] && (optTransformLevel() >= 2) )
    {
        // Under -O2 a straddling pattern is simply not inside, so every
        // heuristic applies to it.
        fprintf(fP, "  A pattern that straddles a boundary is not inside the region: a\n");
        fprintf(fP, "  declaration covers the whole pattern or none of it, so -O2 applies every\n");
        fprintf(fP, "  heuristic to it, as it does outside every region.\n");
    }
    else if( tableP->regionFindings[OPTREG_PARTIAL] )
    {
        fprintf(fP, "  A pattern that straddles a boundary is not opted in: a rewrite takes the\n");
        fprintf(fP, "  whole pattern or none of it, so half a permission is none.  Moving the\n");
        fprintf(fP, "  directive by one line is usually all it needs.\n");
    }

    // The declaration against the evidence.

    fprintf(fP, "\n  Declaration contradicted by the evidence: %d word%s\n",
        tableP->regionContradictions, (tableP->regionContradictions == 1)?"":"s");

    if( !tableP->regionContradictions )
    {
        fprintf(fP, "    Nothing inside a region is written, patched or has its address taken, so\n");
        fprintf(fP, "    as far as this analysis can see your declaration holds.  It cannot see\n");
        fprintf(fP, "    timing, or a word the drum overwrites, and neither of those is checked.\n");
        return;
    }

    fprintf(fP, "    These words are inside a region you declared, and the reference analysis\n");
    fprintf(fP, "    saw them written, patched, or their address used as a value.  The\n");
    fprintf(fP, "    declaration and the evidence disagree, and this is the case where a\n");
    fprintf(fP, "    transform would one day corrupt a working program on your own say-so.\n");
    fprintf(fP, "    Either the region is drawn too wide, or the evidence is a reference you\n");
    fprintf(fP, "    know to be harmless -- but it is worth being sure which.\n");

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !(entryP->flags & OPTF_INREGION) )
        {
            continue;
        }

        if( !(entryP->flags & (OPTF_WRITTEN | OPTF_TAKEN | OPTF_PATCHED)) )
        {
            continue;
        }

        regionP = regionOfIndex(tableP, i);
        spellEvidence(entryP, evidence, sizeof(evidence));
        fprintf(fP, "    region %d  bank %d %04o %06o  %s  %s:%d\n",
            (regionP)?regionP->id:0, entryP->bank, entryP->addr, entryP->value,
            evidence, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);
    }
}

// Print the region dump (-O=regions): a summary line, one line per region and
// one per contradicting word, one fact per line for the check scripts.
void
optDumpRegions(FILE *fP, OptTableP tableP)
{
OptRegionP regionP;
OptWordP entryP;
int i;
char evidence[64];

    fprintf(fP, "regions: %d declared, %d word%s, %d contradiction%s, findings in %d out %d partial %d\n",
        tableP->regionCount, tableP->regionWords, (tableP->regionWords == 1)?"":"s",
        tableP->regionContradictions, (tableP->regionContradictions == 1)?"":"s",
        tableP->regionFindings[OPTREG_INSIDE], tableP->regionFindings[OPTREG_OUTSIDE],
        tableP->regionFindings[OPTREG_PARTIAL]);

    for( regionP = tableP->regionsP; regionP; regionP = regionP->nextP )
    {
        if( regionP->firstIndex < 0 )
        {
            fprintf(fP, "region %d: empty at %d %04o words 0 lines %d to %d file %s\n",
                regionP->id, regionP->openBank, regionP->openAddr,
                regionP->openLine, regionP->endLine,
                (regionP->fileP)?regionP->fileP:"-");
            continue;
        }

        fprintf(fP, "region %d: %d %04o to %d %04o words %d lines %d to %d file %s\n",
            regionP->id,
            tableP->entriesPP[regionP->firstIndex]->bank,
            tableP->entriesPP[regionP->firstIndex]->addr,
            tableP->entriesPP[regionP->lastIndex]->bank,
            tableP->entriesPP[regionP->lastIndex]->addr,
            regionP->wordCount, regionP->openLine, regionP->endLine,
            (regionP->fileP)?regionP->fileP:"-");
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( !(entryP->flags & OPTF_INREGION) )
        {
            continue;
        }

        if( !(entryP->flags & (OPTF_WRITTEN | OPTF_TAKEN | OPTF_PATCHED)) )
        {
            continue;
        }

        regionP = regionOfIndex(tableP, i);
        spellEvidence(entryP, evidence, sizeof(evidence));
        fprintf(fP, "regionbad %d: %d %04o %06o %s %s:%d\n",
            (regionP)?regionP->id:0, entryP->bank, entryP->addr, entryP->value,
            evidence, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);
    }
}

// The nooptimize spans' report section and dump lines.

// Write the report's hands-off section: each span, its extent, its word count
// and the live findings in it.  Nothing for a source without a span.
void
writeHandsOffReport(FILE *fP, OptTableP tableP)
{
OptRegionP spanP;
OptFindingP findingP;
int i;
int count;

    if( !tableP->handsOffCount )
    {
        return;
    }

    fprintf(fP, "\nHands-off spans: %d, holding %d word%s\n", tableP->handsOffCount,
        tableP->handsOffWords, (tableP->handsOffWords == 1)?"":"s");
    fprintf(fP, "  A %%%%nooptimize/%%%%endnooptimize span is your instruction that no\n");
    fprintf(fP, "  optimization level may rewrite the code inside it, whatever the analysis or\n");
    fprintf(fP, "  -O2's guess says.  A finding with any word in a span is still advice, and is\n");
    fprintf(fP, "  printed above; -O1 and -O2 leave it alone.  A span is checked against\n");
    fprintf(fP, "  nothing: it only ever withholds a rewrite, so it cannot be wrong in the\n");
    fprintf(fP, "  dangerous direction.\n");

    for( spanP = tableP->handsOffP; spanP; spanP = spanP->nextP )
    {
        fprintf(fP, "\n  span %d  %s lines %d to %d\n", spanP->id,
            (spanP->fileP)?spanP->fileP:"-", spanP->openLine, spanP->endLine);

        if( spanP->firstIndex < 0 )
        {
            fprintf(fP, "    holds no emitted word.  It opened at bank %d %04o, where the next word\n",
                spanP->openBank, spanP->openAddr);
            fprintf(fP, "    would have gone.\n");
            continue;
        }

        count = 0;

        for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
        {
            for( i = 0; i < findingP->wordCount; ++i )
            {
                if( optHandsOffOfWord(tableP, findingP->wordsP[i]) == spanP )
                {
                    ++count;
                    break;
                }
            }
        }

        fprintf(fP, "    bank %d %04o to bank %d %04o, %d word%s, %d live finding%s in it\n",
            tableP->entriesPP[spanP->firstIndex]->bank,
            tableP->entriesPP[spanP->firstIndex]->addr,
            tableP->entriesPP[spanP->lastIndex]->bank,
            tableP->entriesPP[spanP->lastIndex]->addr,
            spanP->wordCount, (spanP->wordCount == 1)?"":"s",
            count, (count == 1)?"":"s");
    }
}

// Print the hands-off lines of the region dump (-O=regions): a summary line and
// one per span.  Nothing for a source without a span.
void
optDumpHandsOff(FILE *fP, OptTableP tableP)
{
OptRegionP spanP;

    if( !tableP->handsOffCount )
    {
        return;
    }

    fprintf(fP, "handsoff: %d declared, %d word%s\n", tableP->handsOffCount, tableP->handsOffWords,
        (tableP->handsOffWords == 1)?"":"s");

    for( spanP = tableP->handsOffP; spanP; spanP = spanP->nextP )
    {
        if( spanP->firstIndex < 0 )
        {
            fprintf(fP, "handsoff %d: empty at %d %04o words 0 lines %d to %d file %s\n",
                spanP->id, spanP->openBank, spanP->openAddr, spanP->openLine, spanP->endLine,
                (spanP->fileP)?spanP->fileP:"-");
            continue;
        }

        fprintf(fP, "handsoff %d: %d %04o to %d %04o words %d lines %d to %d file %s\n",
            spanP->id,
            tableP->entriesPP[spanP->firstIndex]->bank,
            tableP->entriesPP[spanP->firstIndex]->addr,
            tableP->entriesPP[spanP->lastIndex]->bank,
            tableP->entriesPP[spanP->lastIndex]->addr,
            spanP->wordCount, spanP->openLine, spanP->endLine,
            (spanP->fileP)?spanP->fileP:"-");
    }
}

// The speed declarations, reported and dumped.

// Write the report's speed-declaration section: each speed region and marking
// and what it covers; nothing for a source declaring neither.  It says what was
// declared, not what it would gain: that is -O=speed's report.
void
writeSpeedDeclReport(FILE *fP, OptTableP tableP)
{
OptRegionP speedRegionP;
OptInlineMarkP markP;
int bank;

    if( !tableP->speedCount && !tableP->inlineMarkCount )
    {
        return;
    }

    fprintf(fP, "\nSpeed declarations: %d region%s, %d inline marking%s\n",
        tableP->speedCount, (tableP->speedCount == 1)?"":"s",
        tableP->inlineMarkCount, (tableP->inlineMarkCount == 1)?"":"s");
    fprintf(fP, "  A %%%%speed/%%%%endspeed region and a '%%%%inline NAME' marking declare where a\n");
    fprintf(fP, "  LENGTH-CHANGING rewrite may happen: one that makes the code longer or shorter\n");
    fprintf(fP, "  and moves every address after it.  They are the only way to license inline\n");
    fprintf(fP, "  expansion.  An optimize region licenses the other length-changing rewrites\n");
    fprintf(fP, "  too, and -O2's guess licenses them, but never inline expansion or loop\n");
    fprintf(fP, "  unrolling, only under -O=undeclared.\n");
    fprintf(fP, "  A speed region is not an optimize region and does not imply one: '%%%%optimize'\n");
    fprintf(fP, "  says P5 holds here, and '%%%%speed' says spend words here.  A word may be in\n");
    fprintf(fP, "  either, both or neither.\n");
    fprintf(fP, "  Under -O1 and -O2 a declared call may be copied (S1) and a declared loop\n");
    fprintf(fP, "  unrolled (S2), and a declared jmp's target run moved to follow it (S4); the\n");
    fprintf(fP, "  sections after this one say what was done, and -O=speed what each site would\n");
    fprintf(fP, "  cost and save.\n");

    for( speedRegionP = tableP->speedP; speedRegionP; speedRegionP = speedRegionP->nextP )
    {
        fprintf(fP, "\n  speed region %d  %s lines %d to %d\n", speedRegionP->id,
            (speedRegionP->fileP)?speedRegionP->fileP:"-", speedRegionP->openLine,
            speedRegionP->endLine);

        if( speedRegionP->firstIndex < 0 )
        {
            fprintf(fP, "    holds no emitted word.  It opened at bank %d %04o, where the next word\n",
                speedRegionP->openBank, speedRegionP->openAddr);
            fprintf(fP, "    would have gone.\n");
            continue;
        }

        fprintf(fP, "    bank %d %04o to bank %d %04o, %d word%s\n",
            tableP->entriesPP[speedRegionP->firstIndex]->bank,
            tableP->entriesPP[speedRegionP->firstIndex]->addr,
            tableP->entriesPP[speedRegionP->lastIndex]->bank,
            tableP->entriesPP[speedRegionP->lastIndex]->addr,
            speedRegionP->wordCount, (speedRegionP->wordCount == 1)?"":"s");
    }

    if( tableP->speedWords )
    {
        fprintf(fP, "\n  Words declared, per bank:");

        for( bank = 0; bank <= MAXBANK; ++bank )
        {
            if( tableP->speedBankWords[bank] )
            {
                fprintf(fP, "  b%d %d", bank, tableP->speedBankWords[bank]);
            }
        }

        fprintf(fP, "\n");
    }

    for( markP = tableP->inlineMarksP; markP; markP = markP->nextP )
    {
        fprintf(fP, "\n  inline %d  %s  %s line %d\n", markP->id, markP->nameP,
            (markP->fileP)?markP->fileP:"-", markP->line);

        if( !markP->routineP )
        {
            fprintf(fP, "    NAMES NO ROUTINE.  Nothing is marked.  The name has to be the label on a\n");
            fprintf(fP, "    routine's entry word: a label that is not one, or one that has been\n");
            fprintf(fP, "    renamed since the marking was written, reaches nothing.\n");
            continue;
        }

        fprintf(fP, "    routine %d at bank %d %04o, %d word%s, %d call site%s\n",
            markP->routineP->id, markP->routineP->bank, markP->routineP->entryAddr,
            markP->routineP->wordCount, (markP->routineP->wordCount == 1)?"":"s",
            (markP->routineP->jspSites + markP->routineP->jdaSites),
            ((markP->routineP->jspSites + markP->routineP->jdaSites) == 1)?"":"s");

        if( markP->dupOf )
        {
            fprintf(fP, "    marking %d named this routine already; this one adds nothing.\n",
                markP->dupOf);
        }
    }

    if( tableP->inlineMarkCount )
    {
        fprintf(fP, "\n  %d of %d marking%s named a routine",
            tableP->inlineMarkResolved, tableP->inlineMarkCount,
            (tableP->inlineMarkCount == 1)?"":"s");

        if( tableP->inlineMarkDuplicates )
        {
            fprintf(fP, ", %d of them one already marked", tableP->inlineMarkDuplicates);
        }

        fprintf(fP, ".\n");
    }
}

// Print the speed lines of the region dump (-O=regions): a summary, one line per
// speed region and one per marking, whose routine reads "none" when unresolved.
// Nothing for a source declaring neither.
void
optDumpSpeedDecls(FILE *fP, OptTableP tableP)
{
OptRegionP speedRegionP;
OptInlineMarkP markP;

    if( !tableP->speedCount && !tableP->inlineMarkCount )
    {
        return;
    }

    fprintf(fP, "speed: %d region%s, %d word%s, %d marking%s, %d resolved, %d duplicate%s\n",
        tableP->speedCount, (tableP->speedCount == 1)?"":"s",
        tableP->speedWords, (tableP->speedWords == 1)?"":"s",
        tableP->inlineMarkCount, (tableP->inlineMarkCount == 1)?"":"s",
        tableP->inlineMarkResolved, tableP->inlineMarkDuplicates,
        (tableP->inlineMarkDuplicates == 1)?"":"s");

    for( speedRegionP = tableP->speedP; speedRegionP; speedRegionP = speedRegionP->nextP )
    {
        if( speedRegionP->firstIndex < 0 )
        {
            fprintf(fP, "speed %d: empty at %d %04o words 0 lines %d to %d file %s\n",
                speedRegionP->id, speedRegionP->openBank, speedRegionP->openAddr,
                speedRegionP->openLine, speedRegionP->endLine,
                (speedRegionP->fileP)?speedRegionP->fileP:"-");
            continue;
        }

        fprintf(fP, "speed %d: %d %04o to %d %04o words %d lines %d to %d file %s\n",
            speedRegionP->id,
            tableP->entriesPP[speedRegionP->firstIndex]->bank,
            tableP->entriesPP[speedRegionP->firstIndex]->addr,
            tableP->entriesPP[speedRegionP->lastIndex]->bank,
            tableP->entriesPP[speedRegionP->lastIndex]->addr,
            speedRegionP->wordCount, speedRegionP->openLine, speedRegionP->endLine,
            (speedRegionP->fileP)?speedRegionP->fileP:"-");
    }

    for( markP = tableP->inlineMarksP; markP; markP = markP->nextP )
    {
        if( !markP->routineP )
        {
            fprintf(fP, "inline %d: %s none dup %d line %d file %s\n",
                markP->id, markP->nameP, markP->dupOf, markP->line,
                (markP->fileP)?markP->fileP:"-");
            continue;
        }

        fprintf(fP, "inline %d: %s %d %04o words %d sites %d dup %d line %d file %s\n",
            markP->id, markP->nameP, markP->routineP->bank, markP->routineP->entryAddr,
            markP->routineP->wordCount,
            (markP->routineP->jspSites + markP->routineP->jdaSites),
            markP->dupOf, markP->line, (markP->fileP)?markP->fileP:"-");
    }
}

// The declared ceilings, reported and dumped.

// Write the report's ceiling section: every '%%ceiling' directive, which one
// each bank keeps, and the room that leaves.  Nothing for a source without one.
void
writeCeilingReport(FILE *fP, OptTableP tableP)
{
OptCeilingP ceilP;
int highest[MAXBANK + 1];
int bank;
int last;

    if( !tableP->ceilingCount )
    {
        return;
    }

    bankHighest(tableP, highest);

    fprintf(fP, "\nBank ceilings: %d '%%%%ceiling' directive%s\n", tableP->ceilingCount,
        (tableP->ceilingCount == 1)?"":"s");
    fprintf(fP, "  A '%%%%ceiling EXPR' is the first word of its bank that a length-changing\n");
    fprintf(fP, "  rewrite -- an inline copy, an unrolled loop -- may not grow into: memory the\n");
    fprintf(fP, "  program fills only at run time, which no assembled word shows.  It can only\n");
    fprintf(fP, "  lower the default (07751 in bank 0, the end of the bank elsewhere), and a bank\n");
    fprintf(fP, "  keeps the lowest it declares.\n\n");

    for( ceilP = tableP->ceilingsP; ceilP; ceilP = ceilP->nextP )
    {
        fprintf(fP, "  ceiling %d  bank %d %04o  %s line %d  %s\n", ceilP->id, ceilP->bank, ceilP->value,
            (ceilP->fileP)?ceilP->fileP:"-", ceilP->line,
            (tableP->bankCeilingId[ceilP->bank] == ceilP->id)?"kept":"not kept, a lower one is");
    }

    fprintf(fP, "\n");

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->bankCeiling[bank] )
        {
            continue;
        }

        last = tableP->bankCeiling[bank] - 1;

        if( highest[bank] < 0 )
        {
            fprintf(fP, "  bank %d: no assembled word; a rewrite may use up to %04o\n", bank, last);
            continue;
        }

        fprintf(fP, "  bank %d: highest assembled word %04o, a rewrite may use up to %04o, %d free\n",
            bank, highest[bank], last, last - highest[bank]);
    }
}

// Print the ceiling lines of the region dump (-O=regions): one per directive,
// saying whether its bank kept it.
void
optDumpCeilings(FILE *fP, OptTableP tableP)
{
OptCeilingP ceilP;

    for( ceilP = tableP->ceilingsP; ceilP; ceilP = ceilP->nextP )
    {
        fprintf(fP, "ceiling %d: bank %d %04o %s line %d file %s\n", ceilP->id, ceilP->bank, ceilP->value,
            (tableP->bankCeilingId[ceilP->bank] == ceilP->id)?"kept":"lower",
            ceilP->line, (ceilP->fileP)?ceilP->fileP:"-");
    }
}

// Release the region, span, speed, marking and ceiling lists, and clear the
// table's pointers and the walk state.  Safe on a table that holds none.
void
freeRegionList(OptTableP tableP)
{
OptRegionP regionP;
OptRegionP nextP;
OptInlineMarkP markP;
OptInlineMarkP nextMarkP;
OptCeilingP ceilP;
OptCeilingP nextCeilP;

    for( regionP = tableP->regionsP; regionP; regionP = nextP )
    {
        nextP = regionP->nextP;
        free(regionP);
    }

    for( regionP = tableP->handsOffP; regionP; regionP = nextP )
    {
        nextP = regionP->nextP;
        free(regionP);
    }

    // Neither list owns its strings: nameP and fileP belong to the parse tree,
    // which outlives the table.
    for( regionP = tableP->speedP; regionP; regionP = nextP )
    {
        nextP = regionP->nextP;
        free(regionP);
    }

    for( markP = tableP->inlineMarksP; markP; markP = nextMarkP )
    {
        nextMarkP = markP->nextP;
        free(markP);
    }

    tableP->regionsP = NILP;
    tableP->regionsTailP = NILP;
    tableP->handsOffP = NILP;
    tableP->handsOffTailP = NILP;
    tableP->speedP = NILP;
    tableP->speedTailP = NILP;
    tableP->inlineMarksP = NILP;
    tableP->inlineMarksTailP = NILP;

    // The per-bank winners are left as they are; nothing reads them after this.
    for( ceilP = tableP->ceilingsP; ceilP; ceilP = nextCeilP )
    {
        nextCeilP = ceilP->nextP;
        free(ceilP);
    }

    tableP->ceilingsP = NILP;
    tableP->ceilingsTailP = NILP;
    openRegionP = NILP;
    openHandsOffP = NILP;
    openSpeedP = NILP;
}
