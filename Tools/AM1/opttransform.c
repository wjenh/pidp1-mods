/*
 * The am1 optimizer's transform pass: decides a fate for every live finding
 * and rewrites the words that fire.  T3 (jmp A where A holds jmp B becomes
 * jmp B, followed to the chain's far end) and T8a-d (lio [0] -> cli,
 * lac [0] -> cla, lac [777777] -> cla cma, lac [n] -> law n or law i n)
 * replace one word with one word in place.  -O1 fires them only inside an
 * optimize region; -O2 fires them wherever optguess.c's heuristics (H1-H5
 * outside a region, H1 alone inside) do not refuse.  A nooptimize span stops
 * every rewrite at every level.  The rewrites that change the program's
 * length -- S1 inline expansion, S4 fall-through placement, S2 loop unrolling
 * and space mode's deleting peepholes T1, T1b, T2, T6, T7 and T13 -- are
 * planned and given fates here, but relayout (optrelayout.c) makes them; this
 * file only builds the new trees they need.  H6 refuses each of them at every
 * level, and never T3 or T8.  The extend window's eem and lem deletions are
 * planned by optwindow.c and numbered for bisection here, after the space
 * deletions.  T14 is not carried: lia, lai and swp are PDP-1D instructions,
 * which a machine can have switched off.  Every fate is decided before any
 * word is rewritten, so no fate depends on another rewrite; bisection then
 * switches chosen rewrites off.
 *
 * This is the only optimizer file that writes a parse node.  A rewrite
 * replaces the emitting statement's rightP with a tree shaped exactly as the
 * parser builds the same source text, carrying the old expression's pc, bank
 * and line.  Every code generator re-reduces rightP, and optimize() runs
 * before all of them, so every output shows the rewrite.  The optimizer's
 * word table is not changed, so findings, dumps and the report's evidence
 * describe the program as written.  A pool word a T8 stops naming is still
 * emitted unless relayout reclaims it.
 *
 * Runs at most twice per optimize() call: once for the program and, under
 * -O=xform with -O1, again in select-only mode over the rewritten tree, which
 * must fire nothing.  Each rewrite is checked as it is made: the word's length
 * is unchanged, the new tree evaluates to the encoding the rule names, and the
 * word decoded afresh reads the same pool value or far target.  A failed check
 * is fatal through leave(), before any output is written.
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

// The encodings the rules name, from the handbook, so that a rewritten tree
// is checked against something other than the permanent symbols it was built
// from.
#define XF_OPERATE      0760000     // the operate group, code 76, no micro-ops
#define XF_JMP          0600000     // jmp, code 60
#define XF_LAW          0700000     // law, code 70
#define XF_LAWNEG       0010000     // law's bit 5: load the complement of the field
#define XF_GARBAGE      0525252     // what a register holds before a simulated word
#define XF_DAC          0240000     // dac, code 24: what an inlined jda leaves at its site

extern SymNodeP permSymP;           // the permanent symbols, from permsyms.def.c
extern bool spaceIsAdd;             // -a: a space in an expression adds
extern int evalExpr(PNodeP nodeP);
extern void leave(int signo);

// What happened to a live finding: the dump's word and the report's phrase,
// and -O2's own phrase where -O1's names the wrong level (NILP: the same).
// Indexed by OptXformFate, so the order here is that enum's order.
static const struct
{
    const char *tagP;
    const char *textP;
    const char *text2P;
} fateTable[OPTXF_COUNT] =
{
    { "fired",           "rewritten", NILP },
    { "noregions",       "no region is declared", NILP },
    { "outside",         "outside every declared region", NILP },
    { "partial",         "straddling a region boundary", NILP },
    { "notcarried",      "a rule -O1 does not carry, because it changes the program's length",
                         "a deletion, which -O2 makes only inside an optimize or speed region, and T14 at no level" },
    { "cycle",           "a chain of jmps that comes back on itself, so it has no far target", NILP },
    { "long",            "a chain of jmps longer than the optimizer follows", NILP },
    { "unrepresentable", "not the whole expression of one statement", NILP },
    { "handsoff",        "inside a nooptimize span", NILP },
    { "guessed",         "left alone on H6's guess", "left alone on -O2's guess" },
    { "off",             "switched off for bisection", NILP },
    { "timed",           "in a delay or device loop, whose timing a deletion would change", NILP },
    { "overlap",         "a word another rewrite makes, copies, moves or deletes", NILP }
};

static OptXformFate decideFate(OptTableP tableP, OptXformP recP);
static OptXformFate gateFate(OptTableP tableP, OptFindingP findingP, unsigned int *assumedP);
static int touchesHandsOff(OptTableP tableP, OptFindingP findingP);
static int ruleCarried(OptRuleId rule);
static int ruleIsT8(OptRuleId rule);
static int representable(OptWordP entryP);
static void keepChain(OptXformP recP, OptWordP *chainPP, int hops, OptWordP stopP);
static OptWordP throughWord(OptTableP tableP, OptFindingP findingP);
static int expectedValue(OptFindingP findingP, OptWordP throughP);
static void checkMeaning(OptFindingP findingP, OptWordP throughP, int value);
static PNodeP buildT3(OptWordP entryP, OptWordP viaP, int expected);
static PNodeP buildT8(OptFindingP findingP, OptWordP poolP);
static PNodeP newNode(PNodeP modelP, int type);
static PNodeP permNode(OptWordP entryP, const char *nameP);
static PNodeP binNode(PNodeP modelP, int op, PNodeP leftP, PNodeP rightP);
static PNodeP intNode(PNodeP modelP, int value);
static void installTree(OptWordP entryP, PNodeP treeP);
static PNodeP constantComment(PNodeP nodeP);
static void freeTree(PNodeP nodeP);
static void fireOne(OptTableP tableP, OptXformP recP, int apply);
static void selectForBisection(OptTableP tableP);
static void tallyFreed(OptTableP tableP);
static int poolReadersDeclared(OptTableP tableP, OptWordP poolP);
static int poolReadersInRegions(OptTableP tableP, OptWordP poolP);
static int firedT8From(OptTableP tableP, OptWordP fromP, OptWordP poolP);
static void addRecord(OptTableP tableP, OptXformP recP);
static void internalError(OptWordP entryP, const char *fmtP, ...);

static void planInlines(OptTableP tableP);
static OptInlineFate inlineGate(OptTableP tableP, OptInlineP inP);
static int bodyTouches(OptTableP tableP, OptInlineP inP, unsigned int flags);
static int inBodyOf(OptInlineP inP, OptWordP wordP);
static int nestsWith(OptTableP tableP, OptInlineP inP);
static void fireInline(OptTableP tableP, OptInlineP inP, int *freeP, int limit);
static int compareCheapest(const void *aP, const void *bP);
static PNodeP buildPrefix(OptWordP siteP);
static void buildRetargets(OptTableP tableP, OptInlineP inP);
static int holdsConstant(PNodeP nodeP);
static void addInline(OptTableP tableP, OptInlineP inP);
static void printInlineLine(FILE *fP, const char *labelP, OptInlineP inP, int declared);

static void planPlaces(OptTableP tableP);
static OptPlaceFate placeGate(OptTableP tableP, OptPlaceP pP);
static int placeHolds(OptPlaceP pP, int bank, int addr);
static int placeOverlaps(OptTableP tableP, OptPlaceP pP);
static void addPlace(OptTableP tableP, OptPlaceP pP);
static void printPlaceLine(FILE *fP, const char *labelP, OptPlaceP pP);

static void planUnrolls(OptTableP tableP);
static OptUnrollFate unrollGate(OptTableP tableP, OptUnrollP uP);
static int unrollHolds(OptUnrollP uP, int bank, int addr);
static int unrollOverlaps(OptTableP tableP, OptUnrollP uP);
static void addUnroll(OptTableP tableP, OptUnrollP uP);
static void printUnrollLine(FILE *fP, const char *labelP, OptUnrollP uP, int declared);
static void printBits(FILE *fP, const char *wordP, unsigned int bits, int bisect);

static void planSpace(OptTableP tableP);
static int spaceDeclared(OptFindingP findingP, int *allRegionP);
static int patternDeclared(OptFindingP findingP);
static OptXformFate spaceGate(OptTableP tableP, OptXformP recP, int allRegion);
static PNodeP buildSpace(OptXformP recP, int *expectedP);
static PNodeP buildT1(OptWordP firstP, OptWordP secondP, int *expectedP);
static PNodeP buildT1b(OptWordP firstP, OptWordP secondP, int *expectedP);
static PNodeP buildT2(OptWordP skipP, int *expectedP);
static int flattenWords(PNodeP nodeP, PNodeP *leavesPP, int count, int max);
static PNodeP joinWords(PNodeP modelP, PNodeP *leavesPP, int count);
static int spaceOverlaps(OptTableP tableP, OptXformP recP);
static int patternHolds(OptFindingP findingP, int bank, int addr);
static void printSpaceLine(FILE *fP, OptXformP recP);

// The pass.

// Decide the fate of every live finding and, when apply is non-zero, rewrite
// each one that fires.  All fates are decided before any word is rewritten, so
// no fate depends on an earlier rewrite.  An internal inconsistency is fatal.
void
optTransform(OptTableP tableP, int apply)
{
OptFindingP findingP;
OptXformP recP;
OptInlineP inP;
int i;
int in;

    if( tableP->xformRan )
    {
        return;     // one pass per table: a second would record every fate twice
    }

    tableP->xformRan = 1;
    tableP->xformApplied = apply;

    for( findingP = tableP->findingsP; findingP; findingP = findingP->nextP )
    {
        if( !(recP = (OptXformP)calloc(1, sizeof(OptXform))) )
        {
            fprintf(stderr, "am1: out of memory recording the -O1 transforms\n");
            exit(1);
        }

        recP->findingP = findingP;
        recP->before = findingP->wordsP[0]->value;
        recP->after = recP->before;
        recP->fate = decideFate(tableP, recP);
        addRecord(tableP, recP);
        ++tableP->xformFates[recP->fate];
    }

    // Plan the length-changing rewrites, fates first.  Only when edits are
    // made, since relayout makes them, and only under a level, since -O=xform
    // alone rewrites nothing.  Each plan avoids the words the earlier ones fired.
    if( apply && (optTransformLevel() >= 1) )
    {
        planInlines(tableP);
        planPlaces(tableP);
        planUnrolls(tableP);
        planSpace(tableP);
        optPlanWindow(tableP);
    }

    // Bisection comes after every fate and before any rewrite, so switching
    // one off cannot change another.  Only when edits are made: the
    // idempotence pass must see switched-off words as they are.
    if( apply && optBisectGiven() )
    {
        selectForBisection(tableP);
    }

    // A deleting rule's kept word is installed by relayout, with its delete.
    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate == OPTXF_FIRED) && !optRuleDeletes(recP->findingP->rule) )
        {
            fireOne(tableP, recP, apply);
        }
    }

    // The trees each inline copy needs; relayout makes the copy itself.
    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate == OPTIN_FIRED )
        {
            buildRetargets(tableP, inP);
        }
    }

    tallyFreed(tableP);         // after the selection: a switched-off T8 frees nothing

    // Count, for the report, the patterns the evidence refused inside a region.
    if( tableP->regionCount )
    {
        for( findingP = tableP->suppressedP; findingP; findingP = findingP->nextP )
        {
            in = 0;

            for( i = 0; i < findingP->wordCount; ++i )
            {
                if( findingP->wordsP[i]->flags & OPTF_INREGION )
                {
                    ++in;
                }
            }

            if( in == findingP->wordCount )
            {
                ++tableP->xformRefusedInside;
            }
        }
    }
}

// Decide one live finding's fate: nooptimize span, region (-O1) or guess
// (-O2), rule carried, representable, and for T3 a reachable far target.
// Returns OPTXF_FIRED or the refusing fate; records -O2's assumed heuristics.
static OptXformFate
decideFate(OptTableP tableP, OptXformP recP)
{
OptFindingP findingP;
OptWordP entryP;
OptWordP viaP;
OptWordP stopP;
OptWordP chainPP[OPT_MAXCHAIN];
OptXformFate fate;
OptChainEnd end;
int hops;

    findingP = recP->findingP;

    if( (fate = gateFate(tableP, findingP, &recP->assumed)) != OPTXF_FIRED )
    {
        return(fate);
    }

    if( !ruleCarried(findingP->rule) )
    {
        return(OPTXF_NOTCARRIED);
    }

    entryP = findingP->wordsP[0];

    // Every carried rule is one word for one word; if that ever breaks, stop
    // the assembly rather than shift every address after the word.
    if( (findingP->wordCount != 1) || (findingP->saveWords != 0) )
    {
        internalError(entryP, "%s at bank %d %04o would change the program's length (%d word%s, saving %d); only a one-word-for-one-word rule may reach the edit",
            optRuleName(findingP->rule), findingP->bank, findingP->addr, findingP->wordCount,
            (findingP->wordCount == 1)?"":"s", findingP->saveWords);
    }

    if( (optTransformLevel() < 2) && !(entryP->flags & OPTF_INREGION) )
    {
        internalError(entryP, "%s at bank %d %04o is placed inside a region, but its word is not marked as in one",
            optRuleName(findingP->rule), findingP->bank, findingP->addr);
    }

    if( !representable(entryP) )
    {
        return(OPTXF_UNREPRESENTABLE);
    }

    if( findingP->rule == OPTRULE_T3 )
    {
        // Follow the chain to its end, so the rewritten word is not a pattern
        // again.  Rewrite order does not matter: every fate comes from the
        // analysis, and a chain through another finding's word ends where that
        // finding's does.  The walk stops short at a nooptimize word or, under
        // -O2, one an applicable heuristic marks; the jmp then lands on that
        // word, as T3's single hop would.  The gate has already refused a first
        // intermediate so marked, so at least one hop is always allowed.
        viaP = wordAt(tableP, findingP->bank, entryP->decode.address);

        if( !viaP )
        {
            internalError(entryP, "T3 at bank %d %04o names an intermediate at %04o that is not in the table",
                findingP->bank, findingP->addr, entryP->decode.address);
        }

        chainPP[0] = viaP;
        end = optChainWalk(tableP, entryP, (optTransformLevel() >= 2)?optGuessApplies(findingP):0,
            chainPP, &hops, &stopP);

        if( end == OPTCHAIN_CYCLE )
        {
            return(OPTXF_CYCLE);
        }

        if( end == OPTCHAIN_LONG )
        {
            return(OPTXF_LONG);
        }

        keepChain(recP, chainPP, hops, stopP);
    }

    return(OPTXF_FIRED);
}

// Keep a T3 chain on its record, for the rewrite and the dump.  Allocation
// failure is fatal.
static void
keepChain(OptXformP recP, OptWordP *chainPP, int hops, OptWordP stopP)
{
    if( !(recP->chainPP = (OptWordP *)calloc((size_t)hops, sizeof(OptWordP))) )
    {
        fprintf(stderr, "am1: out of memory recording the -O1 transforms\n");
        exit(1);
    }

    memcpy(recP->chainPP, chainPP, ((size_t)hops * sizeof(OptWordP)));
    recP->hops = hops;
    recP->stopP = stopP;
}

// Decide whether any rewrite may happen here: HANDSOFF in a nooptimize span,
// then -O1's region or -O2's heuristics.  *assumedP gets -O2's relevant bits.
// Returns OPTXF_FIRED to continue, or the fate that stops the finding.
static OptXformFate
gateFate(OptTableP tableP, OptFindingP findingP, unsigned int *assumedP)
{
unsigned int applies;

    *assumedP = 0;

    if( touchesHandsOff(tableP, findingP) )
    {
        return(OPTXF_HANDSOFF);
    }

    // -O2 needs no region: a finding marked by an applicable heuristic (H1 to
    // H5 outside a region, H1 inside, and H6 on a deletion everywhere) is
    // GUESSED.  *assumedP is the refusing bits, or on a pass the bits the
    // rewrite rests on.
    if( optTransformLevel() >= 2 )
    {
        applies = optGuessApplies(findingP);

        if( findingP->guessBits & applies )
        {
            *assumedP = (findingP->guessBits & applies);
            return(OPTXF_GUESSED);
        }

        *assumedP = applies;
        return(OPTXF_FIRED);
    }

    switch( findingP->place )
    {
    case OPTREG_NOREGIONS:
        return(OPTXF_NOREGIONS);

    case OPTREG_OUTSIDE:
        return(OPTXF_OUTSIDE);

    case OPTREG_PARTIAL:
        return(OPTXF_PARTIAL);

    default:
        break;
    }

    return(OPTXF_FIRED);
}

// Test whether any word of a finding lies in a nooptimize span, including the
// word a T3 or T8 reads through: a fenced intermediate or pool word is the
// author's.  Returns 1 if one does, 0 if none does or no span is declared.
static int
touchesHandsOff(OptTableP tableP, OptFindingP findingP)
{
OptWordP throughP;
int i;

    if( !tableP->handsOffCount )
    {
        return(0);
    }

    for( i = 0; i < findingP->wordCount; ++i )
    {
        if( findingP->wordsP[i]->flags & OPTF_HANDSOFF )
        {
            return(1);
        }
    }

    if( ruleCarried(findingP->rule) && (throughP = throughWord(tableP, findingP)) &&
        (throughP->flags & OPTF_HANDSOFF) )
    {
        return(1);
    }

    return(0);
}

// Test whether -O1 and -O2 carry a rule in place: the five one-word-for-one-word
// rules, T3 and T8a-d.  The deleting rules are carried by planSpace() instead.
// Returns 1 if it is carried, 0 if not.
static int
ruleCarried(OptRuleId rule)
{
    return( (rule == OPTRULE_T3) || ruleIsT8(rule) );
}

// Test whether a rule is one of the four constant-load rules.
// Returns 1 if it is, 0 if not.
static int
ruleIsT8(OptRuleId rule)
{
    return( (rule == OPTRULE_T8A) || (rule == OPTRULE_T8B) || (rule == OPTRULE_T8C) || (rule == OPTRULE_T8D) );
}

// Test whether a word is exactly one statement's whole expression: OPTK_EXPR,
// emitted from rightP by one of the four statement types, at its own place.
// Returns 1 if the word can be rewritten in place, 0 if not.
static int
representable(OptWordP entryP)
{
PNodeP nodeP;

    if( (entryP->kind != OPTK_EXPR) || !entryP->nodeP || !entryP->exprP )
    {
        return(0);
    }

    if( entryP->flags & (OPTF_RESERVED | OPTF_DUPADDR | OPTF_PCMISMATCH) )
    {
        return(0);
    }

    nodeP = entryP->nodeP;

    if( nodeP->rightP != entryP->exprP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case EXPR:
    case LOCATION:
    case LCLLOCATION:
    case ORIGIN:
        break;

    default:
        return(0);
    }

    return( (nodeP->bank == entryP->bank) && (nodeP->pc == entryP->addr) );
}

// Test whether T3 would follow a word as an intermediate: in the graph, a
// direct jmp not to itself, never patched, written, taken or pointer-written.
// Shared with optspeed.c.  Returns 1 if it would, 0 if not or when wordP is NILP.
int
optChainFollowable(OptWordP wordP)
{
    if( !wordP || !inGraph(wordP) )
    {
        return(0);
    }

    if( (wordP->decode.group != OPTG_MEMREF) || (wordP->decode.opcode != 060) || wordP->decode.memIndirect )
    {
        return(0);
    }

    if( wordP->decode.address == wordP->addr )
    {
        return(0);
    }

    return( !(wordP->flags & (OPTF_PATCHED | OPTF_WRITTEN | OPTF_TAKEN | OPTF_MAYBE_WRITTEN)) );
}

// Follow a T3 chain from its first intermediate, already in chainPP[0],
// appending each next word while T3 would follow it.  Returns CYCLE when the
// next word is jmpP or already on the chain (no far end), END when it is not
// followable (the last word holds the far target), STOPPED when it is in a
// nooptimize span or has a guessMask bit (named in *stopPP), and LONG past
// OPT_MAXCHAIN words.  *hopsP is the number of words on the chain.  The cycle
// test comes first so a loop back to an unfollowable jmpP is not taken for an
// end, which would rewrite the jmp to jump to itself.
OptChainEnd
optChainWalk(OptTableP tableP, OptWordP jmpP, unsigned int guessMask, OptWordP *chainPP, int *hopsP,
    OptWordP *stopPP)
{
OptWordP curP;
OptWordP nextP;
int hops;
int i;

    hops = 1;
    *stopPP = NILP;

    for( ;; )
    {
        curP = chainPP[hops - 1];
        nextP = wordAt(tableP, curP->bank, curP->decode.address);

        if( nextP == jmpP )
        {
            *hopsP = hops;
            return(OPTCHAIN_CYCLE);
        }

        for( i = 0; i < hops; ++i )
        {
            if( chainPP[i] == nextP )
            {
                *hopsP = hops;
                return(OPTCHAIN_CYCLE);
            }
        }

        if( !optChainFollowable(nextP) )
        {
            *hopsP = hops;
            return(OPTCHAIN_END);
        }

        if( (nextP->flags & OPTF_HANDSOFF) || (guessMask && (optGuessWordBits(tableP, nextP) & guessMask)) )
        {
            *hopsP = hops;
            *stopPP = nextP;
            return(OPTCHAIN_STOPPED);
        }

        if( hops == OPT_MAXCHAIN )
        {
            *hopsP = hops;
            return(OPTCHAIN_LONG);
        }

        chainPP[hops++] = nextP;
    }
}

// Find the word a carried rule reads through, T8's pool word or T3's
// intermediate; the finding does not store it.
// Returns the word, or NILP if it is not in the table.
static OptWordP
throughWord(OptTableP tableP, OptFindingP findingP)
{
    return( wordAt(tableP, findingP->bank, findingP->wordsP[0]->decode.address) );
}

// One rewrite.

// Rewrite one FIRED word or, in a dry run, work out what it would become:
// build the tree, check it three ways, spell both words for the record, and
// when apply is set install it.  A failed check is fatal.
static void
fireOne(OptTableP tableP, OptXformP recP, int apply)
{
OptFindingP findingP;
OptWordP entryP;
OptWordP throughP;
OptWordP farP;
OptWord after;
PNodeP treeP;
int expected;
int value;
char spell[OPTMSG_SIZE];

    findingP = recP->findingP;
    entryP = findingP->wordsP[0];

    if( !(throughP = throughWord(tableP, findingP)) )
    {
        internalError(entryP, "%s at bank %d %04o reads through %04o, which is not in the table",
            optRuleName(findingP->rule), findingP->bank, findingP->addr, entryP->decode.address);
    }

    // T3 takes its target from the last jmp on its chain.
    farP = throughP;

    if( findingP->rule == OPTRULE_T3 )
    {
        if( !recP->chainPP || (recP->hops < 1) || (recP->chainPP[0] != throughP) )
        {
            internalError(entryP, "T3 at bank %d %04o reached the edit without a chain that starts at %04o",
                findingP->bank, findingP->addr, throughP->addr);
        }

        farP = recP->chainPP[recP->hops - 1];
    }

    expected = expectedValue(findingP, farP);

    if( findingP->rule == OPTRULE_T3 )
    {
        treeP = buildT3(entryP, farP, expected);
    }
    else
    {
        treeP = buildT8(findingP, throughP);
    }

    value = (evalExpr(treeP) & WRDMASK);

    if( value != expected )
    {
        internalError(entryP, "%s at bank %d %04o built a word that assembles to %06o, not the %06o the rule names",
            optRuleName(findingP->rule), findingP->bank, findingP->addr, value, expected);
    }

    checkMeaning(findingP, farP, value);

    recP->throughP = throughP;
    recP->regionP = optRegionOfWord(tableP, entryP);
    recP->after = value;

    spellWord(entryP, spell, sizeof(spell));
    recP->beforeP = allocPrintf("%s", spell);

    after = *entryP;
    after.exprP = treeP;
    spellWord(&after, spell, sizeof(spell));
    recP->afterP = allocPrintf("%s", spell);

    if( findingP->perHop )
    {
        // Every jmp the rewrite jumps past is a hop, each worth T3's saving.
        tableP->xformHops += recP->hops;
        tableP->xformHopTime += (recP->hops * findingP->saveTime);
    }
    else
    {
        tableP->xformTime += findingP->saveTime;
    }

    if( throughP->flags & OPTF_TAKEN )
    {
        ++tableP->xformThroughTaken;
    }

    if( throughP->flags & OPTF_MAYBE_WRITTEN )
    {
        ++tableP->xformThroughMaybe;
    }

    if( recP->regionP && recP->regionP->contradictions )
    {
        ++tableP->xformContradicted;
    }

    if( apply )
    {
        // A far target written as a number must be re-pointed by relayout if
        // the target word moves, so keep the number node and the word it names.
        if( (findingP->rule == OPTRULE_T3) && (treeP->type == BINOP) && treeP->rightP &&
            (treeP->rightP->type == INTEGER) )
        {
            recP->numberP = treeP->rightP;
            recP->targetP = wordAt(tableP, entryP->bank, farP->decode.address);
        }

        // T8d's number is the constant's value, and a constant such as
        // [label:0] holds an address relayout may move, so keep the number
        // node for relayout to reset.  A constant of literals cannot move and
        // is not kept, since a kept number refuses an inline copy of the word.
        if( (findingP->rule == OPTRULE_T8D) && optConstMayMove(entryP->exprP) &&
            (treeP->type == BINOP) && treeP->rightP )
        {
            if( treeP->rightP->type == INTEGER )
            {
                recP->numberP = treeP->rightP;
            }
            else if( (treeP->rightP->type == BINOP) && treeP->rightP->rightP &&
                (treeP->rightP->rightP->type == INTEGER) )
            {
                recP->numberP = treeP->rightP->rightP;
            }
        }

        installTree(entryP, treeP);
    }
    else
    {
        freeTree(treeP);
    }
}

// Compute the word a rule says the rewrite produces, from the handbook's
// encodings rather than the permanent symbols the tree is built from, so the
// two check each other.  Returns the 18-bit word.
static int
expectedValue(OptFindingP findingP, OptWordP throughP)
{
int value;

    switch( findingP->rule )
    {
    case OPTRULE_T3:
        return( XF_JMP | throughP->decode.address );

    case OPTRULE_T8A:
        return( XF_OPERATE | OPTM_CLI );

    case OPTRULE_T8B:
        return( XF_OPERATE | OPTM_CLA );

    case OPTRULE_T8C:
        return( XF_OPERATE | OPTM_CLA | OPTM_CMA );

    case OPTRULE_T8D:
        value = throughP->value;

        if( value <= ADDRMASK )
        {
            return( XF_LAW | value );
        }

        return( XF_LAW | XF_LAWNEG | ((~value) & WRDMASK) );

    default:
        internalError(findingP->wordsP[0], "%s at bank %d %04o reached the edit, and -O1 does not carry it",
            optRuleName(findingP->rule), findingP->bank, findingP->addr);
    }

    return(0);
}

// Check, by decoding the rewritten word afresh, that it does what the original
// did: for T3 a direct jmp to the far target; for T8 the loaded register holds
// the pool value and the other is untouched from any start.  Fatal if not.
static void
checkMeaning(OptFindingP findingP, OptWordP throughP, int value)
{
OptWordP entryP;
OptDecode decode;
int ac;
int io;
int wantIO;

    entryP = findingP->wordsP[0];
    memset(&decode, 0, sizeof(decode));
    optDecodeValue(value, &decode);

    if( findingP->rule == OPTRULE_T3 )
    {
        if( (decode.group != OPTG_MEMREF) || (decode.opcode != 060) || decode.memIndirect ||
            (decode.address != throughP->decode.address) )
        {
            internalError(entryP, "T3 at bank %d %04o rewrote to %06o, which is not a jmp to %04o",
                findingP->bank, findingP->addr, value, throughP->decode.address);
        }

        return;
    }

    wantIO = ((entryP->decode.group == OPTG_MEMREF) && (entryP->decode.opcode == 022));
    ac = XF_GARBAGE;
    io = XF_GARBAGE;

    if( decode.group == OPTG_OPERATE )
    {
        optSimulateOperate(decode.microBits, XF_GARBAGE, XF_GARBAGE, 0, 0, &ac, &io);
    }
    else if( decode.group == OPTG_LAW )
    {
        ac = (decode.indirect)?((~decode.address) & WRDMASK):decode.address;
    }
    else
    {
        internalError(entryP, "%s at bank %d %04o rewrote to %06o, which neither operates nor is a law",
            optRuleName(findingP->rule), findingP->bank, findingP->addr, value);
    }

    ac &= WRDMASK;
    io &= WRDMASK;

    if( wantIO )
    {
        if( (io != throughP->value) || (ac != XF_GARBAGE) )
        {
            internalError(entryP, "%s at bank %d %04o rewrote to %06o, which leaves IO %06o and AC %06o, not IO %06o and AC unchanged",
                optRuleName(findingP->rule), findingP->bank, findingP->addr, value, io, ac, throughP->value);
        }
    }
    else if( (ac != throughP->value) || (io != XF_GARBAGE) )
    {
        internalError(entryP, "%s at bank %d %04o rewrote to %06o, which leaves AC %06o and IO %06o, not AC %06o and IO unchanged",
            optRuleName(findingP->rule), findingP->bank, findingP->addr, value, ac, io, throughP->value);
    }
}

// Build T3's new expression: jmp to the target of the last jmp on the chain.
// The operand is the intermediate's own symbol when it was a plain "jmp name"
// with a resolved global, so the listing reads as an author would write it;
// anything else gets a number, since a local or '.' would name a different
// word from the rewritten line, and so does a symbol form that does not
// assemble to exactly the expected word.  Allocation failure is fatal.
static PNodeP
buildT3(OptWordP entryP, OptWordP viaP, int expected)
{
PNodeP modelP;
PNodeP viaExprP;
PNodeP treeP;
PNodeP operandP;

    modelP = entryP->exprP;
    viaExprP = viaP->exprP;

    if( viaExprP && (viaExprP->type == BINOP) && (viaExprP->value.ival == SEPARATOR) &&
        viaExprP->leftP && (viaExprP->leftP->type == OPADDR) &&
        viaExprP->rightP && (viaExprP->rightP->type == ADDR) &&
        viaExprP->rightP->value.symP && (viaExprP->rightP->value.symP->flags & SYMF_RESOLVED) )
    {
        operandP = newNode(modelP, ADDR);
        operandP->value.symP = viaExprP->rightP->value.symP;
        treeP = binNode(modelP, SEPARATOR, permNode(entryP, "jmp"), operandP);

        if( (evalExpr(treeP) & WRDMASK) == expected )
        {
            return(treeP);
        }

        freeTree(treeP);
    }

    return( binNode(modelP, SEPARATOR, permNode(entryP, "jmp"), intNode(modelP, viaP->decode.address)) );
}

// Build T8's new expression, in the shape the parser gives the same text:
//   cli, cla           one operate symbol
//   cla cma            joined by a space, or under -a by '|', since there a
//                      space adds and 760200 + 761000 is not 761200
//   law n              law, a space, the number
//   law i n            law, a space, then i and the number joined by a space
static PNodeP
buildT8(OptFindingP findingP, OptWordP poolP)
{
OptWordP entryP;
PNodeP modelP;
int value;

    entryP = findingP->wordsP[0];
    modelP = entryP->exprP;
    value = poolP->value;

    switch( findingP->rule )
    {
    case OPTRULE_T8A:
        return( permNode(entryP, "cli") );

    case OPTRULE_T8B:
        return( permNode(entryP, "cla") );

    case OPTRULE_T8C:
        return( binNode(modelP, (spaceIsAdd)?OR:SEPARATOR, permNode(entryP, "cla"), permNode(entryP, "cma")) );

    case OPTRULE_T8D:
        if( value <= ADDRMASK )
        {
            return( binNode(modelP, SEPARATOR, permNode(entryP, "law"), intNode(modelP, value)) );
        }

        return( binNode(modelP, SEPARATOR, permNode(entryP, "law"),
            binNode(modelP, SEPARATOR, permNode(entryP, "i"), intNode(modelP, ((~value) & WRDMASK)))) );

    default:
        internalError(entryP, "%s at bank %d %04o reached the T8 builder",
            optRuleName(findingP->rule), findingP->bank, findingP->addr);
    }

    return(NILP);
}

// Allocate one node carrying the pc, bank and line of the expression it helps
// replace.  Not parsefns.c's newnode(): that stamps the parser's current bank
// and adjusts the line for flex.  Allocation failure is fatal.
static PNodeP
newNode(PNodeP modelP, int type)
{
PNodeP nodeP;

    if( !(nodeP = (PNodeP)calloc(1, sizeof(PNode))) )
    {
        fprintf(stderr, "am1: out of memory building a -O1 rewrite\n");
        exit(1);
    }

    nodeP->type = type;
    nodeP->flags = 0;
    nodeP->pc = modelP->pc;
    nodeP->bank = modelP->bank;
    nodeP->lineNo = modelP->lineNo;
    return(nodeP);
}

// Make a node for a permanent symbol, typed as the parser types that name: LAW,
// IMOD for i, VALUESPEC, OPCODE, OPADDR or OPORABLE.  A missing symbol is fatal.
static PNodeP
permNode(OptWordP entryP, const char *nameP)
{
SymNodeP symP;
PNodeP nodeP;
int type;

    if( !(symP = sym_find((SymNodeP *)&permSymP, (char *)nameP)) )
    {
        internalError(entryP, "the permanent symbol %s is missing", nameP);
    }

    if( symP->flags & SYMF_LAW )
    {
        type = LAW;
    }
    else
    {
        switch( symP->flags & SYM_MASK )
        {
        case SYM_VALUE:
            type = (symP->flags & SYMF_INDIRECT)?IMOD:VALUESPEC;
            break;

        case SYM_OPCODE:
            type = OPCODE;
            break;

        case SYM_OPADDR:
            type = OPADDR;
            break;

        case SYM_OPORABLE:
            type = OPORABLE;
            break;

        default:
            internalError(entryP, "the permanent symbol %s has no node type", nameP);
            type = 0;
            break;
        }
    }

    nodeP = newNode(entryP->exprP, type);
    nodeP->value.symP = symP;
    return(nodeP);
}

// Make a binary operator node, as binop() in parsefns.c does.
static PNodeP
binNode(PNodeP modelP, int op, PNodeP leftP, PNodeP rightP)
{
PNodeP nodeP;

    nodeP = newNode(modelP, BINOP);
    nodeP->value.ival = op;
    nodeP->leftP = leftP;
    nodeP->rightP = rightP;
    return(nodeP);
}

// Make a number node.
static PNodeP
intNode(PNodeP modelP, int value)
{
PNodeP nodeP;

    nodeP = newNode(modelP, INTEGER);
    nodeP->value.ival = value;
    return(nodeP);
}

// Put a new expression into the word's statement.  The old tree stays in
// OptWord.exprP, so the report still spells the word as the source did.
static void
installTree(OptWordP entryP, PNodeP treeP)
{
PNodeP stmtP;
PNodeP commentP;
PNodeP noteP;

    stmtP = entryP->nodeP;
    commentP = constantComment(stmtP->rightP);
    stmtP->rightP = treeP;

    if( commentP )
    {
        // "lac [6  // six": the comment rode on the CONSTANT node and ended the
        // line.  Move it to a COMMENT statement after this one, the node the
        // parser makes for "law 6  // six".
        noteP = newNode(stmtP, COMMENT);
        noteP->value.strP = commentP->value.strP;
        noteP->leftP = stmtP->leftP;
        stmtP->leftP = noteP;
        return;
    }

    if( stmtP->leftP && (stmtP->leftP->type == EMPTYLINE) )
    {
        // "lac [5" with no comment: the closing newline is re-read as an empty
        // line, so the line end belonged to the CONSTANT node and went with the
        // expression.  Insert a TERMINATOR, the node the parser makes for
        // "law 5".  A constant closed by ']' or ';' already has its TERMINATOR
        // or SEMI.
        noteP = newNode(stmtP, TERMINATOR);
        noteP->leftP = stmtP->leftP;
        stmtP->leftP = noteP;
    }
}

// Find a line comment carried by a constant reference in an expression; only
// an unclosed CONSTANT node's rightP can hold one.
// Returns the COMMENT node, or NILP if the expression carries none.
static PNodeP
constantComment(PNodeP nodeP)
{
PNodeP foundP;

    if( !nodeP )
    {
        return(NILP);
    }

    if( nodeP->type == CONSTANT )
    {
        if( nodeP->rightP && (nodeP->rightP->type == COMMENT) )
        {
            return(nodeP->rightP);
        }

        return(NILP);
    }

    if( (foundP = constantComment(nodeP->leftP)) )
    {
        return(foundP);
    }

    return( constantComment(nodeP->rightP) );
}

// Release a tree this file built and did not install (the dry run's, or a T3
// symbol form that failed its check).  The symbols it points at are not freed.
static void
freeTree(PNodeP nodeP)
{
    if( !nodeP )
    {
        return;
    }

    freeTree(nodeP->leftP);
    freeTree(nodeP->rightP);
    free(nodeP);
}

// S1, inline expansion, inside the author's declarations.

// Decide the fate of every call site a declaration reaches (its word in a speed
// region, or its callee marked 'inline'); undeclared sites are not recorded.
// The fates, decided from the analysis alone before any word is rewritten:
//
//   refused by the judge, touched by a nooptimize span, marked by -O2's guess
//   where it applies, or not writable as an edit: does not fire;
//   a single-site callee: always inlined, and its original deleted;
//   a marked callee: inlined at every declared site, over the cap and budget;
//   any other site: needs a body within the cap, and is paid from its bank's
//   budget, cheapest first (microseconds saved per word spent), down to the
//   reserve.
//
// A bank's budget is its free words above its highest address, to its load
// ceiling.  Single-site and marked copies may spend it all (FULL only past the
// ceiling); budgeted ones leave the reserve.  A single-site copy is charged
// its whole copy, not the net after deleting its callee, since that delete may
// be refused.  One copy may not contain another: a site in a body a fired
// inline copies or deletes, or whose body holds a fired site, is NESTED,
// because copies are made from the text as written and their "jmp .+k"
// distances are the text's.  Taking single-site, then marked, then budget
// order fixes which of two nested sites wins.
static void
planInlines(OptTableP tableP)
{
OptBlockP *blocksPP;
OptInlineP inP;
OptInlineP *budgetPP;
OptWordP wordP;
OptInlineShape shape;
int freeWords[MAXBANK + 1];
int highest[MAXBANK + 1];
int bank;
int addr;
int i;
int n;
int cap;
int reserve;
int marked;

    if( !tableP->speedCount && !tableP->inlineMarkResolved )
    {
        return;     // nothing is declared: no site is recorded
    }

    blocksPP = optBlockIndex(tableP);

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            if( !wordP || !wordP->isCallSite || !wordP->blockP || !wordP->blockP->reached )
            {
                continue;
            }

            optInlineJudge(tableP, blocksPP, wordP, &shape);
            marked = (shape.routineP && optInlineMarkOfRoutine(tableP, shape.routineP));

            if( !(wordP->flags & OPTF_INSPEED) && !marked )
            {
                continue;
            }

            if( !(inP = (OptInlineP)calloc(1, sizeof(OptInline))) )
            {
                fprintf(stderr, "am1: out of memory recording the inline sites\n");
                exit(1);
            }

            inP->siteP = wordP;
            inP->shape = shape;
            inP->marked = marked;
            inP->inSpeed = ((wordP->flags & OPTF_INSPEED) != 0);
            inP->outcome = -1;
            inP->fate = inlineGate(tableP, inP);
            addInline(tableP, inP);
        }
    }

    free(blocksPP);

    // Each bank's budget: its free words above the highest address.
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

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        freeWords[bank] = (highest[bank] < 0)?0:(optBankCeiling(tableP, bank) - highest[bank]);
    }

    cap = optInlineCap();
    reserve = optInlineReserve();

    // Single-site callees, then marked ones: over the cap and budget, to the
    // ceiling.
    for( i = 0; i < 2; ++i )
    {
        for( inP = tableP->inlinesP; inP; inP = inP->nextP )
        {
            if( (inP->fate == OPTIN_FIRED) && !inP->ordinal &&
                ((i == 0)?inP->shape.single:(!inP->shape.single && inP->marked)) )
            {
                inP->ordinal = 1;       // taken; cleared below before bisection numbers them
                fireInline(tableP, inP, &freeWords[inP->siteP->bank], 0);
            }
        }
    }

    // Every other site: the cap, then the budget, cheapest first.
    n = 0;

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( (inP->fate == OPTIN_FIRED) && !inP->ordinal )
        {
            if( inP->shape.body > cap )
            {
                inP->fate = OPTIN_CAP;
                continue;
            }

            ++n;
        }
    }

    if( !(budgetPP = (OptInlineP *)calloc((size_t)(n + 1), sizeof(OptInlineP))) )
    {
        fprintf(stderr, "am1: out of memory recording the inline sites\n");
        exit(1);
    }

    for( n = 0, inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( (inP->fate == OPTIN_FIRED) && !inP->ordinal )
        {
            budgetPP[n++] = inP;
        }
    }

    qsort(budgetPP, (size_t)n, sizeof(OptInlineP), compareCheapest);

    for( i = 0; i < n; ++i )
    {
        budgetPP[i]->ordinal = 1;
        fireInline(tableP, budgetPP[i], &freeWords[budgetPP[i]->siteP->bank], reserve);
    }

    free(budgetPP);

    // The fates are final: count them, and charge what fired.
    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        inP->ordinal = 0;
        ++tableP->inlineFates[inP->fate];

        if( inP->fate == OPTIN_FIRED )
        {
            tableP->inlineSpent[inP->siteP->bank] += inP->shape.spent;
            tableP->inlineTime += inP->shape.best;
        }
    }
}

// Decide whether a site may be inlined at all: the judge's refusal, a
// nooptimize span, -O2's guess, a writable edit.  Returns the refusing fate,
// or OPTIN_FIRED provisionally; planInlines() decides the rest.
static OptInlineFate
inlineGate(OptTableP tableP, OptInlineP inP)
{
OptWordP siteP;
OptWordP wordP;
unsigned int applies;
int addr;

    siteP = inP->siteP;

    if( !inP->shape.eligible )
    {
        return(OPTIN_REFUSED);
    }

    if( tableP->handsOffCount && ((siteP->flags & OPTF_HANDSOFF) || bodyTouches(tableP, inP, OPTF_HANDSOFF)) )
    {
        return(OPTIN_HANDSOFF);
    }

    // The copy puts words in at the site, so H6 refuses at every level.
    if( inP->shape.guessBits & OPTGH_LENGTH )
    {
        inP->assumed = (inP->shape.guessBits & OPTGH_LENGTH);
        return(OPTIN_GUESSED);
    }

    // Under -O2 the declaration licenses the rewrite and the guess can only
    // refuse it.  Inside an optimize region H1 alone applies, as for T3 and T8.
    if( optTransformLevel() >= 2 )
    {
        applies = (siteP->flags & OPTF_INREGION)?(OPTGH_INREGION & OPTGH_AUTHORIZED):OPTGH_AUTHORIZED;

        if( inP->shape.guessBits & applies )
        {
            inP->assumed = (inP->shape.guessBits & applies);
            return(OPTIN_GUESSED);
        }

        inP->assumed = (applies | OPTGH_LENGTH);
    }

    // The site's statement is deleted or, for a jda, followed by "dac Y", whose
    // operand must be the jda's one name.  Each word the copy retargets gets a
    // new expression, which a word that holds a constant cannot be given
    // without a pool slot moving under it.
    if( !representable(siteP) )
    {
        return(OPTIN_UNREPRESENTABLE);
    }

    if( inP->shape.isJda && !(inP->prefixP = buildPrefix(siteP)) )
    {
        return(OPTIN_UNREPRESENTABLE);
    }

    for( addr = (inP->shape.entryP->addr + 1); addr < inP->shape.lastP->addr; ++addr )
    {
        wordP = wordAt(tableP, siteP->bank, addr);

        if( optInlineRetargets(&inP->shape, wordP) && holdsConstant(wordP->exprP) )
        {
            freeTree(inP->prefixP);
            inP->prefixP = NILP;
            return(OPTIN_UNREPRESENTABLE);
        }
    }

    return(OPTIN_FIRED);
}

// Test whether any word of the site's callee, entry to return, carries one of
// these flags.  Returns 1 if one does, 0 if none does.
static int
bodyTouches(OptTableP tableP, OptInlineP inP, unsigned int flags)
{
OptWordP wordP;
int addr;

    for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
    {
        if( (wordP = wordAt(tableP, inP->siteP->bank, addr)) && (wordP->flags & flags) )
        {
            return(1);
        }
    }

    return(0);
}

// Test whether a word is in the site's callee; the judge has made the body one
// run of addresses.  Returns 1 if it is, 0 if not.
static int
inBodyOf(OptInlineP inP, OptWordP wordP)
{
    return( (wordP->bank == inP->siteP->bank) && (wordP->addr >= inP->shape.entryP->addr) &&
        (wordP->addr <= inP->shape.lastP->addr) );
}

// Test whether firing this site would put one copy inside another: it lies in
// a taken site's callee, or a taken site lies in its callee.
// Returns 1 if it would, 0 if not.
static int
nestsWith(OptTableP tableP, OptInlineP inP)
{
OptInlineP otherP;

    for( otherP = tableP->inlinesP; otherP; otherP = otherP->nextP )
    {
        if( (otherP == inP) || (otherP->fate != OPTIN_FIRED) || !otherP->ordinal )
        {
            continue;       // only the ones already taken
        }

        if( inBodyOf(otherP, inP->siteP) || inBodyOf(inP, otherP->siteP) )
        {
            return(1);
        }
    }

    return(0);
}

// Take one site that has passed the gate: NESTED, refused for want of room
// (BUDGET when a reserve is kept, FULL at the ceiling), or FIRED with its
// words charged to the bank.  A copy that shrinks the program always fits.
static void
fireInline(OptTableP tableP, OptInlineP inP, int *freeP, int limit)
{
    if( nestsWith(tableP, inP) )
    {
        inP->fate = OPTIN_NESTED;
        return;
    }

    if( (inP->shape.spent > 0) && (inP->shape.spent > (*freeP - limit)) )
    {
        inP->fate = (limit)?OPTIN_BUDGET:OPTIN_FULL;
        return;
    }

    inP->deletes = inP->shape.single;
    *freeP -= inP->shape.spent;
}

// Order the budget: free copies first, then most microseconds saved per word
// spent, then bank and address, so the order is total and repeatable.
// Returns <0, 0 or >0, as qsort() wants.
static int
compareCheapest(const void *aP, const void *bP)
{
OptInlineP a;
OptInlineP b;
long lhs;
long rhs;

    a = *(const OptInlineP *)aP;
    b = *(const OptInlineP *)bP;

    if( (a->shape.spent <= 0) != (b->shape.spent <= 0) )
    {
        return( (a->shape.spent <= 0)?-1:1 );
    }

    if( (a->shape.spent > 0) && (b->shape.spent > 0) )
    {
        lhs = ((long)a->shape.best * b->shape.spent);
        rhs = ((long)b->shape.best * a->shape.spent);

        if( lhs != rhs )
        {
            return( (lhs > rhs)?-1:1 );
        }
    }

    if( a->siteP->bank != b->siteP->bank )
    {
        return( a->siteP->bank - b->siteP->bank );
    }

    return( a->siteP->addr - b->siteP->addr );
}

// Build the "dac Y" an inlined jda leaves at its site: jda deposits AC in Y and
// enters at Y+1.  Only a jda written "jda Y" is rebuilt, and the result must
// assemble to dac and the jda's address.  Returns NILP for any other form.
static PNodeP
buildPrefix(OptWordP siteP)
{
PNodeP exprP;
PNodeP operandP;
PNodeP treeP;

    exprP = siteP->exprP;

    if( !exprP || (exprP->type != BINOP) || (exprP->value.ival != SEPARATOR) || !exprP->rightP ||
        ((exprP->rightP->type != ADDR) && (exprP->rightP->type != LCLADDR)) ||
        exprP->rightP->leftP || exprP->rightP->rightP )
    {
        return(NILP);
    }

    operandP = newNode(exprP, exprP->rightP->type);
    operandP->value.symP = exprP->rightP->value.symP;
    treeP = binNode(exprP, SEPARATOR, permNode(siteP, "dac"), operandP);

    if( (evalExpr(treeP) & WRDMASK) != (XF_DAC | siteP->decode.address) )
    {
        freeTree(treeP);
        return(NILP);
    }

    return(treeP);
}

// Build the "jmp .+k" of every body word a fired copy points past the copy; k
// is the distance to the dropped return, which relayout resets from the final
// layout.  Each tree is checked against its jmp; a mismatch is fatal.
static void
buildRetargets(OptTableP tableP, OptInlineP inP)
{
OptWordP wordP;
OptWordP lastP;
PNodeP modelP;
PNodeP dotP;
PNodeP offsetP;
PNodeP treeP;
int addr;
int n;
int value;

    lastP = inP->shape.lastP;
    n = 0;

    for( addr = (inP->shape.entryP->addr + 1); addr < lastP->addr; ++addr )
    {
        n += optInlineRetargets(&inP->shape, wordAt(tableP, lastP->bank, addr));
    }

    inP->retargetCount = 0;

    if( !n )
    {
        return;
    }

    if( !(inP->retargetsPP = (OptWordP *)calloc((size_t)n, sizeof(OptWordP))) ||
        !(inP->retargetTreesPP = (PNodeP *)calloc((size_t)n, sizeof(PNodeP))) ||
        !(inP->offsetsPP = (PNodeP *)calloc((size_t)n, sizeof(PNodeP))) )
    {
        fprintf(stderr, "am1: out of memory building an inline copy\n");
        exit(1);
    }

    for( addr = (inP->shape.entryP->addr + 1); addr < lastP->addr; ++addr )
    {
        wordP = wordAt(tableP, lastP->bank, addr);

        if( !optInlineRetargets(&inP->shape, wordP) )
        {
            continue;
        }

        modelP = wordP->exprP;
        dotP = newNode(modelP, DOT);
        dotP->value.ival = wordP->addr;
        offsetP = intNode(modelP, (lastP->addr - wordP->addr));
        treeP = binNode(modelP, SEPARATOR, permNode(wordP, "jmp"), binNode(modelP, PLUS, dotP, offsetP));
        value = (evalExpr(treeP) & WRDMASK);

        if( value != (XF_JMP | lastP->addr) )
        {
            internalError(inP->siteP, "S1 at bank %d %04o built %06o for the word at %04o, not a jmp to %04o",
                inP->siteP->bank, inP->siteP->addr, value, wordP->addr, lastP->addr);
        }

        inP->retargetsPP[inP->retargetCount] = wordP;
        inP->retargetTreesPP[inP->retargetCount] = treeP;
        inP->offsetsPP[inP->retargetCount] = offsetP;
        ++inP->retargetCount;
    }
}

// Test whether an expression holds a constant reference.
// Returns 1 if it does, 0 if not.
static int
holdsConstant(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    if( nodeP->type == CONSTANT )
    {
        return(1);
    }

    return( holdsConstant(nodeP->leftP) || holdsConstant(nodeP->rightP) );
}

// Append an inline record; sites arrive in bank then address order.
static void
addInline(OptTableP tableP, OptInlineP inP)
{
    if( tableP->inlinesTailP )
    {
        tableP->inlinesTailP->nextP = inP;
    }
    else
    {
        tableP->inlinesP = inP;
    }

    tableP->inlinesTailP = inP;
    ++tableP->inlineCount;
}

// S4, fall-through placement.

// Decide the fate of every placement site: a jmp J in an optimize or speed
// region (either is the author's word that a length-changing rewrite may
// happen there) or, under -O2 with -O=undeclared, any site on the guess.
// Relayout moves the run to follow J and deletes J, so J's block falls into
// it; a label on J goes to the run's first word.  The run ends in a jmp, so
// nothing falls out of it before or after the move.  Every fate is decided
// here before any rewrite, and no fired site touches another, a fired inline,
// or a fired T3 or T8 word, so any subset of fired sites is a valid program.
static void
planPlaces(OptTableP tableP)
{
OptPlaceP pP;
OptWordP wordP;
OptPlaceShape shape;
int anywhere;
int declared;
int bank;
int addr;

    anywhere = optUndeclared();

    if( !tableP->regionCount && !tableP->speedCount && !anywhere )
    {
        return;     // nothing declares a site: none is recorded
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            if( !wordP || (optPlaceSiteKind(tableP, wordP) != 2) )
            {
                continue;
            }

            declared = ((wordP->flags & (OPTF_INREGION | OPTF_INSPEED)) != 0);

            if( !declared && !anywhere )
            {
                continue;
            }

            optPlaceJudge(tableP, wordP, &shape);

            if( !(pP = (OptPlaceP)calloc(1, sizeof(OptPlace))) )
            {
                fprintf(stderr, "am1: out of memory recording the placement sites\n");
                exit(1);
            }

            pP->shape = shape;
            pP->declared = declared;
            pP->outcome = -1;
            pP->deleteOutcome = -1;
            pP->fate = placeGate(tableP, pP);
            addPlace(tableP, pP);
        }
    }

    // In bank then address order: a site touching one taken before it, a fired
    // inline, or a fired T3 or T8 word stays as it is.
    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( (pP->fate == OPTPL_FIRED) && placeOverlaps(tableP, pP) )
        {
            pP->fate = OPTPL_OVERLAP;
        }

        ++tableP->placeFates[pP->fate];

        if( pP->fate == OPTPL_FIRED )
        {
            tableP->placeTime += pP->shape.save;
        }
    }
}

// Decide whether a site may be moved at all: the judge's refusal, the run's
// coverage by the declaration, a nooptimize span, -O2's guess.  Returns the
// refusing fate, or OPTPL_FIRED provisionally until placeOverlaps() is asked.
static OptPlaceFate
placeGate(OptTableP tableP, OptPlaceP pP)
{
OptPlaceShapeP sP;
OptWordP wordP;
unsigned int applies;
int addr;
int anywhere;

    sP = &pP->shape;

    if( !sP->eligible )
    {
        return(OPTPL_REFUSED);
    }

    anywhere = optUndeclared();

    // A declared J whose run the declaration does not cover is PARTIAL, unless
    // the switch covers every word.
    for( addr = sP->targetP->addr; !anywhere && (addr <= sP->endP->addr); ++addr )
    {
        wordP = wordAt(tableP, sP->jmpP->bank, addr);

        if( !(wordP->flags & (OPTF_INREGION | OPTF_INSPEED)) )
        {
            return(OPTPL_PARTIAL);
        }
    }

    for( addr = sP->targetP->addr; addr <= sP->endP->addr; ++addr )
    {
        wordP = wordAt(tableP, sP->jmpP->bank, addr);

        if( (sP->jmpP->flags | wordP->flags) & OPTF_HANDSOFF )
        {
            return(OPTPL_HANDSOFF);
        }
    }

    // Moving the run changes the length at J and where the run was, so H6
    // refuses at every level.
    if( sP->guessBits & OPTGH_LENGTH )
    {
        pP->assumed = (sP->guessBits & OPTGH_LENGTH);
        return(OPTPL_GUESSED);
    }

    // Under -O2 the guess can only refuse.  Inside an optimize region H1 alone
    // applies, as for T3, T8 and the inlines.
    if( optTransformLevel() >= 2 )
    {
        applies = (sP->jmpP->flags & OPTF_INREGION)?(OPTGH_INREGION & OPTGH_AUTHORIZED):OPTGH_AUTHORIZED;

        if( sP->guessBits & applies )
        {
            pP->assumed = (sP->guessBits & applies);
            return(OPTPL_GUESSED);
        }

        pP->assumed = (applies | OPTGH_LENGTH);
    }

    return(OPTPL_FIRED);
}

// Test whether a word is J or a word of the run.
// Returns 1 if it is, 0 if not.
static int
placeHolds(OptPlaceP pP, int bank, int addr)
{
OptPlaceShapeP sP;

    sP = &pP->shape;

    if( bank != sP->jmpP->bank )
    {
        return(0);
    }

    return( (addr == sP->jmpP->addr) || ((addr >= sP->targetP->addr) && (addr <= sP->endP->addr)) );
}

// Test whether this site touches a word already taken: an earlier fired site,
// a fired inline's call word, copied body or deleted return, or a fired T3 or
// T8 word.  Returns 1 if it would, 0 if not.
static int
placeOverlaps(OptTableP tableP, OptPlaceP pP)
{
OptPlaceP otherP;
OptInlineP inP;
OptXformP recP;
OptWordP wordP;
int addr;

    for( otherP = tableP->placesP; otherP != pP; otherP = otherP->nextP )
    {
        if( otherP->fate != OPTPL_FIRED )
        {
            continue;
        }

        if( placeHolds(pP, otherP->shape.jmpP->bank, otherP->shape.jmpP->addr) )
        {
            return(1);
        }

        for( addr = otherP->shape.targetP->addr; addr <= otherP->shape.endP->addr; ++addr )
        {
            if( placeHolds(pP, otherP->shape.jmpP->bank, addr) )
            {
                return(1);
            }
        }
    }

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        if( placeHolds(pP, inP->siteP->bank, inP->siteP->addr) )
        {
            return(1);
        }

        for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
        {
            if( placeHolds(pP, inP->siteP->bank, addr) )
            {
                return(1);
            }
        }

        if( inP->deletes && (wordP = inP->shape.rtnP) && placeHolds(pP, wordP->bank, wordP->addr) )
        {
            return(1);
        }
    }

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate == OPTXF_FIRED) &&
            placeHolds(pP, recP->findingP->wordsP[0]->bank, recP->findingP->wordsP[0]->addr) )
        {
            return(1);
        }
    }

    return(0);
}

// Append a placement record; sites arrive in bank then address order.
static void
addPlace(OptTableP tableP, OptPlaceP pP)
{
    if( tableP->placesTailP )
    {
        tableP->placesTailP->nextP = pP;
    }
    else
    {
        tableP->placesP = pP;
    }

    tableP->placesTailP = pP;
    ++tableP->placeCount;
}

// Print one placement site's -O=xform dump line, in the findings' shape:
//
//   xform fired S4 0 0431 f.am1:40 target 0500 p1 run 2 save 5 region | jmp p1
//          => move 0500-0501 after 0431, delete 0431 #4
//
// (one line).  "region" or "undeclared" says what reached the site.
static void
printPlaceLine(FILE *fP, const char *labelP, OptPlaceP pP)
{
OptPlaceShapeP sP;
OptWordP jmpP;
char spell[OPTMSG_SIZE];

    sP = &pP->shape;
    jmpP = sP->jmpP;
    fprintf(fP, "%s %s S4 %d %04o %s:%d target %04o %s", labelP, optPlaceFateName(pP->fate), jmpP->bank,
        jmpP->addr, (jmpP->fileP)?jmpP->fileP:"-", jmpP->lineNo, sP->targetP->addr, firstLabelName(sP->targetP));

    if( sP->endP )
    {
        fprintf(fP, " run %d save %d", sP->run, sP->save);
    }

    spellWord(jmpP, spell, sizeof(spell));
    fprintf(fP, " %s | %s", (pP->declared)?"region":"undeclared", spell);

    switch( pP->fate )
    {
    case OPTPL_FIRED:
    case OPTPL_OFF:
        fprintf(fP, " => move %04o-%04o after %04o, delete %04o", sP->targetP->addr, sP->endP->addr,
            jmpP->addr, jmpP->addr);
        break;

    case OPTPL_REFUSED:
        fprintf(fP, " %s", optPlaceWhyName(sP->why));
        break;

    default:
        break;
    }

    if( (pP->fate == OPTPL_FIRED) || (pP->fate == OPTPL_GUESSED) )
    {
        printBits(fP, (pP->fate == OPTPL_GUESSED)?"by":"assumes", pP->assumed, 0);
    }

    if( pP->fate == OPTPL_OFF )
    {
        printBits(fP, "by", pP->offBy, 1);
    }

    if( pP->ordinal )
    {
        fprintf(fP, " #%d", pP->ordinal);
    }

    fprintf(fP, "\n");
}

// Print " WORD NAME,NAME" for a set of heuristic bits, or of bisection bits
// when bisect is set; nothing for none.
static void
printBits(FILE *fP, const char *wordP, unsigned int bits, int bisect)
{
int h;
int first;
int count;

    if( !bits )
    {
        return;
    }

    fprintf(fP, " %s", wordP);
    count = (bisect)?OPTBIS_COUNT:OPTGH_COUNT;
    first = 1;

    for( h = 0; h < count; ++h )
    {
        if( bits & (1u << h) )
        {
            fprintf(fP, "%s%s", (first)?" ":",", (bisect)?optBisectName(1u << h):optGuessName((OptGuessId)h));
            first = 0;
        }
    }
}

// Name a placement fate, as the dump and the report print it.
// Returns a static string, never NILP.
const char *
optPlaceFateName(OptPlaceFate fate)
{
static const char *namesPP[OPTPL_COUNT] =
{
    "fired", "refused", "partial", "handsoff", "guessed", "overlap", "off"
};

    if( (fate < 0) || (fate >= OPTPL_COUNT) )
    {
        return("?");
    }

    return(namesPP[fate]);
}

// Write the report's placement section: each recorded site's fate and what
// relayout made of each move.  Written after relayout, only when a site was
// recorded.
void
writePlaceReport(FILE *fP, OptTableP tableP)
{
OptPlaceP pP;
OptPlaceShapeP sP;
int refused;
int moved;
int words;
int time;

    if( !tableP->placeCount )
    {
        return;
    }

    for( refused = 0, moved = 0, words = 0, time = 0, pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        if( pP->outcome > 0 )
        {
            ++refused;
            continue;
        }

        ++moved;

        if( !pP->deleteOutcome )
        {
            ++words;
            time += pP->shape.save;
        }
    }

    fprintf(fP, "\nFall-through placement (-O%d): %d site%s recorded, %d moved\n",
        (optTransformLevel() >= 2)?2:1, tableP->placeCount, (tableP->placeCount == 1)?"":"s", moved);
    if( optUndeclared() )
    {
        fprintf(fP, "  A site is recorded when its jmp is in an optimize or speed region, or anywhere,\n");
        fprintf(fP, "  since %s was given.  Each move\n", optUndeclaredSpelling());
    }
    else
    {
        fprintf(fP, "  A site is recorded when its jmp is in an optimize or speed region.  Each move\n");
    }
    fprintf(fP, "  deletes the jmp: %d word%s freed and %d microseconds saved per execution of every\n",
        words, (words == 1)?"":"s", time);
    fprintf(fP, "  moved site.  %d move%s refused by relayout.\n", refused, (refused == 1)?" was":"s were");

    if( optUndeclared() )
    {
        fprintf(fP, "  A move only the switch licensed is marked (undeclared).\n");
    }

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        sP = &pP->shape;
        fprintf(fP, "    S4   bank %2d %04o  %s:%d  %s  ", sP->jmpP->bank, sP->jmpP->addr,
            (sP->jmpP->fileP)?sP->jmpP->fileP:"-", sP->jmpP->lineNo, firstLabelName(sP->targetP));

        switch( pP->fate )
        {
        case OPTPL_FIRED:
            if( pP->outcome > 0 )
            {
                fprintf(fP, "NOT moved: relayout refused the move (%s), and the jmp stays\n", pP->outcomeP);
            }
            else if( pP->deleteOutcome )
            {
                fprintf(fP, "moved, %d words, but the jmp stays: relayout refused its delete (%s)\n",
                    sP->run, (pP->deleteOutcomeP)?pP->deleteOutcomeP:"-");
            }
            else
            {
                fprintf(fP, "moved, %d word%s, 1 word freed, saves %d us%s\n", sP->run, (sP->run == 1)?"":"s",
                    sP->save, (pP->declared)?"":" (undeclared)");
            }
            break;

        case OPTPL_REFUSED:
            fprintf(fP, "not moved: %s\n", optPlaceWhyName(sP->why));
            break;

        case OPTPL_PARTIAL:
            fprintf(fP, "not moved: the jmp is declared and a word of the run is not\n");
            break;

        case OPTPL_HANDSOFF:
            fprintf(fP, "not moved: the jmp or the run is in a nooptimize span\n");
            break;

        case OPTPL_GUESSED:
            fprintf(fP, "not moved: left alone on -O2's guess\n");
            break;

        case OPTPL_OVERLAP:
            fprintf(fP, "not moved: it touches another move, an inline copy or a rewritten word\n");
            break;

        default:
            fprintf(fP, "not moved: switched off for bisection, rewrite #%d\n", pP->ordinal);
            break;
        }
    }
}

// S2, loop unrolling.

// Decide the fate of every loop whose isp is in an optimize or speed region.
// Relayout copies the body n - 1 times after I, then deletes I and J, and D
// and S; H's label stays on the first copy, and a label on S moves with its
// delete to H.  Every fate is decided here before any rewrite, and no fired
// loop touches another, a fired inline or placement, or a fired T3 or T8
// word, so any subset of fired loops is a valid program.  The cap and budget
// are asked in record order, after the inlines have taken their words.
static void
planUnrolls(OptTableP tableP)
{
OptUnrollP uP;
OptWordP wordP;
int highest[MAXBANK + 1];
int room;
int bank;
int addr;
int i;

    if( !tableP->regionCount && !tableP->speedCount )
    {
        return;     // nothing declares a loop: none is recorded
    }

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( !tableP->banksP[bank] )
        {
            continue;
        }

        for( addr = 0; addr < BANKSIZE; ++addr )
        {
            wordP = tableP->banksP[bank]->wordsP[addr];

            if( !wordP || !(wordP->flags & (OPTF_INREGION | OPTF_INSPEED)) || !optUnrollSite(tableP, wordP) )
            {
                continue;
            }

            if( !(uP = (OptUnrollP)calloc(1, sizeof(OptUnroll))) )
            {
                fprintf(stderr, "am1: out of memory recording the loops to unroll\n");
                exit(1);
            }

            optUnrollJudge(tableP, wordP, &uP->shape);
            uP->outcome = -1;
            uP->fate = unrollGate(tableP, uP);
            addUnroll(tableP, uP);
        }
    }

    // Each bank's budget: free words above the highest address, less what the
    // inlines and the earlier loops took.
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

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        bank = uP->shape.ispP->bank;

        if( uP->fate == OPTUN_FIRED )
        {
            room = optBankCeiling(tableP, bank) - highest[bank] - tableP->inlineSpent[bank] - tableP->unrollSpent[bank] -
                optInlineReserve();

            if( unrollOverlaps(tableP, uP) )
            {
                uP->fate = OPTUN_OVERLAP;
            }
            else if( uP->shape.unrolled > optUnrollCap() )
            {
                uP->fate = OPTUN_CAP;
            }
            else if( (uP->shape.spent > 0) && (uP->shape.spent > room) )
            {
                uP->fate = OPTUN_BUDGET;
            }
        }

        ++tableP->unrollFates[uP->fate];

        if( uP->fate == OPTUN_FIRED )
        {
            tableP->unrollSpent[bank] += uP->shape.spent;
            tableP->unrollTime += uP->shape.save;
        }
    }
}

// Decide whether a loop may be unrolled at all: the judge's refusal, coverage
// by the declaration, a nooptimize span, the guess.  The guess refuses at every
// level, since unrolling H2's delay loop would shorten the delay.
// Returns the refusing fate, or OPTUN_FIRED provisionally.
static OptUnrollFate
unrollGate(OptTableP tableP, OptUnrollP uP)
{
OptUnrollShapeP sP;
OptWordP wordP;
int addr;

    sP = &uP->shape;

    if( !sP->eligible )
    {
        return(OPTUN_REFUSED);
    }

    for( addr = sP->setP->addr; addr <= sP->jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, sP->ispP->bank, addr);

        if( !(wordP->flags & (OPTF_INREGION | OPTF_INSPEED)) )
        {
            return(OPTUN_PARTIAL);
        }
    }

    for( addr = sP->setP->addr; addr <= sP->jmpP->addr; ++addr )
    {
        wordP = wordAt(tableP, sP->ispP->bank, addr);

        if( wordP->flags & OPTF_HANDSOFF )
        {
            return(OPTUN_HANDSOFF);
        }
    }

    if( sP->guessBits )
    {
        uP->assumed = sP->guessBits;
        return(OPTUN_GUESSED);
    }

    return(OPTUN_FIRED);
}

// Test whether a word is in the loop, S to J.
// Returns 1 if it is, 0 if not.
static int
unrollHolds(OptUnrollP uP, int bank, int addr)
{
    return( (bank == uP->shape.ispP->bank) && (addr >= uP->shape.setP->addr) && (addr <= uP->shape.jmpP->addr) );
}

// Test whether this loop touches a word already taken: an earlier fired loop,
// a fired inline's call word, copied body or deleted return, a fired
// placement's jmp or run, or a fired T3 or T8 word.  Returns 1 if so, 0 if not.
static int
unrollOverlaps(OptTableP tableP, OptUnrollP uP)
{
OptUnrollP otherP;
OptInlineP inP;
OptPlaceP pP;
OptXformP recP;
OptWordP wordP;
int addr;

    for( otherP = tableP->unrollsP; otherP != uP; otherP = otherP->nextP )
    {
        if( otherP->fate != OPTUN_FIRED )
        {
            continue;
        }

        for( addr = otherP->shape.setP->addr; addr <= otherP->shape.jmpP->addr; ++addr )
        {
            if( unrollHolds(uP, otherP->shape.ispP->bank, addr) )
            {
                return(1);
            }
        }
    }

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        if( unrollHolds(uP, inP->siteP->bank, inP->siteP->addr) )
        {
            return(1);
        }

        for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
        {
            if( unrollHolds(uP, inP->siteP->bank, addr) )
            {
                return(1);
            }
        }

        if( inP->deletes && (wordP = inP->shape.rtnP) && unrollHolds(uP, wordP->bank, wordP->addr) )
        {
            return(1);
        }
    }

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        if( unrollHolds(uP, pP->shape.jmpP->bank, pP->shape.jmpP->addr) )
        {
            return(1);
        }

        for( addr = pP->shape.targetP->addr; addr <= pP->shape.endP->addr; ++addr )
        {
            if( unrollHolds(uP, pP->shape.jmpP->bank, addr) )
            {
                return(1);
            }
        }
    }

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate == OPTXF_FIRED) &&
            unrollHolds(uP, recP->findingP->wordsP[0]->bank, recP->findingP->wordsP[0]->addr) )
        {
            return(1);
        }
    }

    return(0);
}

// Append a loop record; loops arrive in bank then address order.
static void
addUnroll(OptTableP tableP, OptUnrollP uP)
{
    if( tableP->unrollsTailP )
    {
        tableP->unrollsTailP->nextP = uP;
    }
    else
    {
        tableP->unrollsP = uP;
    }

    tableP->unrollsTailP = uP;
    ++tableP->unrollCount;
}

// Print one loop's -O=xform dump line, keyed by the isp, the word bisection
// names it by:
//
//   xform fired S2 0 0403 f.am1:40 head 0401 h1 trips 3 body 2 spent 2 save 60
//          region | isp c1 => unroll 0401-0402 x3, delete 0377-0400 0403-0404 #5
//
// (one line).  "region" or "speed" says which declaration reached the loop.
static void
printUnrollLine(FILE *fP, const char *labelP, OptUnrollP uP, int declared)
{
OptUnrollShapeP sP;
OptWordP ispP;
char spell[OPTMSG_SIZE];

    sP = &uP->shape;
    ispP = sP->ispP;
    fprintf(fP, "%s %s S2 %d %04o %s:%d head %04o %s trips %d", labelP, optUnrollFateName(uP->fate), ispP->bank,
        ispP->addr, (ispP->fileP)?ispP->fileP:"-", ispP->lineNo, sP->jmpP->decode.address,
        (sP->headP)?firstLabelName(sP->headP):"-", sP->trips);

    if( sP->eligible )
    {
        fprintf(fP, " body %d spent %d save %d", sP->body, sP->spent, sP->save);
    }

    spellWord(ispP, spell, sizeof(spell));
    fprintf(fP, " %s | %s", (ispP->flags & OPTF_INSPEED)?"speed":"region", spell);

    switch( uP->fate )
    {
    case OPTUN_FIRED:
    case OPTUN_OFF:
        fprintf(fP, " => unroll %04o-%04o x%d, delete %04o-%04o %04o-%04o", sP->headP->addr, (ispP->addr - 1),
            sP->trips, sP->setP->addr, sP->depP->addr, ispP->addr, sP->jmpP->addr);
        break;

    case OPTUN_REFUSED:
        fprintf(fP, " %s%s", optUnrollWhyName(sP->why), sP->detail);
        break;

    case OPTUN_CAP:
        fprintf(fP, " unrolled %d cap %d", sP->unrolled, optUnrollCap());
        break;

    case OPTUN_BUDGET:
        // Name the declared ceiling, as an inline's budget and full lines do.
        if( declared )
        {
            fprintf(fP, " declared ceiling %04o", declared);
        }
        break;

    default:
        break;
    }

    if( uP->fate == OPTUN_GUESSED )
    {
        printBits(fP, "by", uP->assumed, 0);
    }

    if( uP->fate == OPTUN_OFF )
    {
        printBits(fP, "by", uP->offBy, 1);
    }

    if( uP->ordinal )
    {
        fprintf(fP, " #%d", uP->ordinal);
    }

    fprintf(fP, "\n");
}

// Name an unroll fate, as the dump and the report print it.
// Returns a static string, never NILP.
const char *
optUnrollFateName(OptUnrollFate fate)
{
static const char *namesPP[OPTUN_COUNT] =
{
    "fired", "refused", "partial", "handsoff", "guessed", "overlap", "cap", "budget", "off"
};

    if( (fate < 0) || (fate >= OPTUN_COUNT) )
    {
        return("?");
    }

    return(namesPP[fate]);
}

// Write the report's unroll section: each recorded loop's fate and what
// relayout made of it.  Written after relayout, only when a loop was recorded.
void
writeUnrollReport(FILE *fP, OptTableP tableP)
{
OptUnrollP uP;
OptUnrollShapeP sP;
OptWordP ispP;
int refused;
int unrolled;
int words;
int time;

    if( !tableP->unrollCount )
    {
        return;
    }

    for( refused = 0, unrolled = 0, words = 0, time = 0, uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( uP->fate != OPTUN_FIRED )
        {
            continue;
        }

        if( uP->outcome > 0 )
        {
            ++refused;
            continue;
        }

        ++unrolled;
        words += (uP->shape.spent + uP->deleteRefused);

        if( !uP->deleteRefused )
        {
            time += uP->shape.save;
        }
    }

    fprintf(fP, "\nLoop unrolling (-O%d): %d loop%s recorded, %d unrolled\n",
        (optTransformLevel() >= 2)?2:1, tableP->unrollCount, (tableP->unrollCount == 1)?"":"s", unrolled);
    fprintf(fP, "  A loop is recorded when its isp is in an optimize or speed region.  Unrolling\n");
    fprintf(fP, "  changed the program's length by %d word%s, and saves %d microseconds over\n",
        words, ((words == 1) || (words == -1))?"":"s", time);
    fprintf(fP, "  one pass through each unrolled loop whose setup relayout deleted.  %d unroll%s\n",
        refused, (refused == 1)?"":"s");
    fprintf(fP, "  %s refused by relayout.\n", (refused == 1)?"was":"were");

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        sP = &uP->shape;
        ispP = sP->ispP;
        fprintf(fP, "    S2   bank %2d %04o  %s:%d  %s  ", ispP->bank, sP->jmpP->decode.address,
            (ispP->fileP)?ispP->fileP:"-", ispP->lineNo, (sP->headP)?firstLabelName(sP->headP):"-");

        switch( uP->fate )
        {
        case OPTUN_FIRED:
            if( uP->outcome > 0 )
            {
                fprintf(fP, "NOT unrolled: relayout refused it (%s), and the loop stays\n", uP->outcomeP);
            }
            else if( uP->deleteRefused )
            {
                fprintf(fP, "unrolled, %d trip%s of %d word%s, but relayout kept %d setup word%s\n", sP->trips,
                    (sP->trips == 1)?"":"s", sP->body, (sP->body == 1)?"":"s", uP->deleteRefused,
                    (uP->deleteRefused == 1)?"":"s");
            }
            else
            {
                fprintf(fP, "unrolled, %d trip%s of %d word%s, length %+d, saves %d us\n", sP->trips,
                    (sP->trips == 1)?"":"s", sP->body, (sP->body == 1)?"":"s", sP->spent, sP->save);
            }
            break;

        case OPTUN_REFUSED:
            fprintf(fP, "not unrolled: %s%s\n", optUnrollWhyName(sP->why), sP->detail);
            break;

        case OPTUN_PARTIAL:
            fprintf(fP, "not unrolled: the isp is declared and a word of the loop is not\n");
            break;

        case OPTUN_HANDSOFF:
            fprintf(fP, "not unrolled: a word of the loop is in a nooptimize span\n");
            break;

        case OPTUN_GUESSED:
            fprintf(fP, "not unrolled: a heuristic marks a word of the loop\n");
            break;

        case OPTUN_OVERLAP:
            fprintf(fP, "not unrolled: it touches another loop, an inline copy, a move or a rewritten word\n");
            break;

        case OPTUN_CAP:
            fprintf(fP, "not unrolled: %d words unrolled, over the cap of %d\n", sP->unrolled, optUnrollCap());
            break;

        case OPTUN_BUDGET:
            fprintf(fP, "not unrolled: bank %d has too few free words for %d more", ispP->bank, sP->spent);

            // Name the author's ceiling when that is what counted.
            if( optBankCeilingDeclared(tableP, ispP->bank) )
            {
                fprintf(fP, ", below its declared %%%%ceiling %04o", optBankCeilingDeclared(tableP, ispP->bank));
            }

            fprintf(fP, "\n");
            break;

        default:
            fprintf(fP, "not unrolled: switched off for bisection, rewrite #%d\n", uP->ordinal);
            break;
        }
    }
}

// Space mode, the deleting peepholes.

// Test whether space mode carries a rule: T1, T1b, T2, T6, T7 and T13.
// Returns 1 if it does, 0 if not.
int
optRuleDeletes(OptRuleId rule)
{
    switch( rule )
    {
    case OPTRULE_T1:
    case OPTRULE_T1B:
    case OPTRULE_T2:
    case OPTRULE_T6:
    case OPTRULE_T7:
    case OPTRULE_T13:
        return(1);

    default:
        return(0);
    }
}

// Write the report's pool reclaim section: the pool words declared rewrites
// freed and which relayout collected.  Written only where a declared rewrite
// freed a word.  Each word is named with the rewrite that freed it, since
// switching that rewrite off is how a reader keeps it; the reclaim has no
// bisection number of its own.
void
writeReclaimReport(FILE *fP, OptTableP tableP)
{
OptXformP recP;
OptWordP entryP;

    // Written whenever a level made its rewrites, so that a build which freed
    // nothing says 0 of 0 rather than leaving the reader to wonder whether the
    // section ran.
    if( !tableP->poolFreeable && !tableP->poolAdvice &&
        !(tableP->xformApplied && (optTransformLevel() >= 1)) )
    {
        return;
    }

    fprintf(fP, "\nPool reclaim (-O%d): %d of %d %s storage word%s collected\n",
        (optTransformLevel() >= 2)?2:1, tableP->poolReclaimed, tableP->poolFreeable,
        (optUndeclared())?"licensed":"declared", (tableP->poolFreeable == 1)?"":"s");
    fprintf(fP, "  A pool word no word names any more goes, and every word after it moves, where a\n");

    if( optUndeclared() )
    {
        fprintf(fP, "  declaration licensed every rewrite that stopped naming it, or anywhere, since\n");
        fprintf(fP, "  %s was given; a word only the switch licensed is marked (undeclared).\n",
            optUndeclaredSpelling());
        fprintf(fP, "  It is not a rewrite of its own and has no bisection ordinal: switching off the\n");
        fprintf(fP, "  rewrite named below keeps the word.\n");
    }
    else
    {
        fprintf(fP, "  declaration licensed every rewrite that stopped naming it.  It is not a rewrite\n");
        fprintf(fP, "  of its own and has no bisection ordinal: switching off the rewrite named below\n");
        fprintf(fP, "  keeps the word.\n");
    }

    // Outside a declaration the saving stays advice; name what it would take
    // to collect, the only thing the author can do about it.
    if( tableP->poolAdvice )
    {
        fprintf(fP, "  %d more word%s freed with no declaration behind every rewrite that stopped\n",
            tableP->poolAdvice, (tableP->poolAdvice == 1)?"":"s");
        fprintf(fP, "  naming %s, and %s kept.  An optimize region over every one of those rewrites\n",
            (tableP->poolAdvice == 1)?"it":"them", (tableP->poolAdvice == 1)?"is":"are");
        fprintf(fP, "  would collect %s%s.\n", (tableP->poolAdvice == 1)?"it":"them",
            (optTransformLevel() >= 2)?", and so would -O=undeclared":"");
    }

    // Relayout keys each pool slot by its value, so a slot two references
    // shared splits when the addresses they hold stop being equal, and two
    // slots merge when they become equal; neither is counted by the rewrites.
    if( tableP->poolGrown > 0 )
    {
        fprintf(fP, "  Relayout added %d pool word%s, not counted above: a constant shared by references\n",
            tableP->poolGrown, (tableP->poolGrown == 1)?"":"s");
        fprintf(fP, "  whose values differ once code has moved takes a slot for each value.\n");
    }
    else if( tableP->poolGrown < 0 )
    {
        fprintf(fP, "  Relayout dropped %d more pool word%s than counted above: constants whose values\n",
            -tableP->poolGrown, (tableP->poolGrown == -1)?"":"s");
        fprintf(fP, "  became equal once code had moved now share a slot.\n");
    }

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( !recP->freesPool || !recP->throughP )
        {
            continue;
        }

        entryP = recP->findingP->wordsP[0];
        fprintf(fP, "    %-4s bank %2d %04o  %s:%d  %s %04o", optRuleName(recP->findingP->rule),
            recP->findingP->bank, recP->findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo,
            (recP->poolCollected)?"collected":"freed but kept", recP->throughP->addr);

        if( !recP->licensedPool )
        {
            fprintf(fP, ": no declaration licensed every rewrite that stopped naming it");
        }
        else if( recP->poolCollected && !poolReadersInRegions(tableP, recP->throughP) )
        {
            fprintf(fP, " (undeclared)");
        }

        // No ordinal: this section is written whether a bisection modifier was
        // given or not, and must not differ between -O=upto=all and the plain
        // build.  The Transforms section carries every ordinal.
        fprintf(fP, "\n");
    }
}

// Write the report's space section: every declared deleting finding, its fate,
// and for each that fired whether relayout made the deletion.  Written only
// when planSpace() decided a record.
void
writeSpaceReport(FILE *fP, OptTableP tableP)
{
OptXformP recP;
OptFindingP findingP;
OptWordP entryP;
int fired;
int refused;
int words;
int time;
int allRegion;

    if( !tableP->spaceGated )
    {
        return;
    }

    for( fired = 0, refused = 0, words = 0, time = 0, recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate != OPTXF_FIRED) || !optRuleDeletes(recP->findingP->rule) )
        {
            continue;
        }

        ++fired;

        if( !recP->outcomeP || strcmp(recP->outcomeP, "accepted") )
        {
            ++refused;
            continue;
        }

        words += recP->findingP->saveWords;
        time += recP->findingP->saveTime;
    }

    fprintf(fP, "\nSpace mode (-O%d): %d deleting finding%s %s, %d deleted\n",
        (optTransformLevel() >= 2)?2:1, tableP->spaceGated, (tableP->spaceGated == 1)?"":"s",
        (optUndeclared())?"licensed":"declared", (fired - refused));
    fprintf(fP, "  T1, T1b, T2, T6, T7 and T13 delete a word where every word of the pattern is\n");

    if( optUndeclared() )
    {
        fprintf(fP, "  in an optimize or speed region, or anywhere, since %s was given,\n", optUndeclaredSpelling());
        fprintf(fP, "  never in a delay or device loop (H2, H3).  A deletion only the switch licensed\n");
        fprintf(fP, "  is marked (undeclared).\n");
    }
    else
    {
        fprintf(fP, "  in an optimize or speed region, never in a delay or device loop (H2, H3).\n");
    }
    fprintf(fP, "  The deletions made the program %d word%s shorter, and save %d microseconds over\n",
        words, (words == 1)?"":"s", time);
    fprintf(fP, "  one pass through each; every word after a deletion has moved.  %d deletion%s\n",
        refused, (refused == 1)?"":"s");
    fprintf(fP, "  %s refused by relayout, and %s the words the source wrote.\n", (refused == 1)?"was":"were",
        (refused == 1)?"keeps":"keep");

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        findingP = recP->findingP;

        if( !optRuleDeletes(findingP->rule) || !recP->delP )
        {
            if( optRuleDeletes(findingP->rule) && (recP->fate != OPTXF_FIRED) &&
                spaceDeclared(findingP, &allRegion) )
            {
                entryP = findingP->wordsP[0];
                fprintf(fP, "    %-4s bank %2d %04o  %s:%d  not deleted: %s\n", optRuleName(findingP->rule),
                    findingP->bank, findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo,
                    optXformFateText(recP->fate));
            }

            continue;
        }

        entryP = findingP->wordsP[0];
        fprintf(fP, "    %-4s bank %2d %04o  %s:%d  ", optRuleName(findingP->rule), findingP->bank, findingP->addr,
            (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);

        if( recP->fate != OPTXF_FIRED )
        {
            fprintf(fP, "not deleted: %s, rewrite #%d\n", optXformFateText(recP->fate), recP->ordinal);
        }
        else if( recP->outcomeP && !strcmp(recP->outcomeP, "accepted") )
        {
            fprintf(fP, "deleted %04o: %s => %s%s\n", recP->delP->addr, (recP->beforeP)?recP->beforeP:"-",
                (recP->afterP)?recP->afterP:"-", (patternDeclared(findingP))?"":" (undeclared)");
        }
        else
        {
            fprintf(fP, "NOT deleted: relayout refused it (%s), and the words stay as written\n",
                (recP->outcomeP)?recP->outcomeP:"not run");
        }
    }
}

// Decide the fate of every deleting rule's record whose whole pattern is in an
// optimize or speed region, or anywhere under -O2 with -O=undeclared, after
// the inlines, placements and unrolls have taken their words.  Other records
// keep decideFate()'s fate.  Record order, so an earlier finding wins a word
// two share.
static void
planSpace(OptTableP tableP)
{
OptXformP recP;
int allRegion;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( !optRuleDeletes(recP->findingP->rule) || !spaceDeclared(recP->findingP, &allRegion) )
        {
            continue;
        }

        // Declared, so counted and listed; a nooptimize span has already
        // decided it.
        ++tableP->spaceGated;

        if( recP->fate == OPTXF_HANDSOFF )
        {
            continue;
        }

        --tableP->xformFates[recP->fate];
        recP->fate = spaceGate(tableP, recP, allRegion);
        ++tableP->xformFates[recP->fate];

        if( recP->fate == OPTXF_FIRED )
        {
            tableP->spaceWords += recP->findingP->saveWords;
            tableP->spaceTime += recP->findingP->saveTime;

            // The rule's own conditions keep a written word out of its
            // pattern, but the report warns, as for T3 and T8.
            if( recP->regionP && recP->regionP->contradictions )
            {
                ++tableP->xformContradicted;
            }
        }
    }
}

// Test whether every word of a finding's pattern is in an optimize or speed
// region, or -O=undeclared licenses the finding wherever it is; *allRegionP is
// set when all are in optimize regions (-O2 asks H1 alone there, and all six
// heuristics elsewhere).  Returns 1 if the finding is licensed, 0 if not.
static int
spaceDeclared(OptFindingP findingP, int *allRegionP)
{
int i;

    *allRegionP = 1;

    for( i = 0; i < findingP->wordCount; ++i )
    {
        if( !(findingP->wordsP[i]->flags & (OPTF_INREGION | OPTF_INSPEED)) && !optUndeclared() )
        {
            return(0);
        }

        if( !(findingP->wordsP[i]->flags & OPTF_INREGION) )
        {
            *allRegionP = 0;
        }
    }

    return(1);
}

// Test whether every word of a finding's pattern is in an optimize or speed
// region, whatever the switch says; the report marks a rewrite that fails it
// as the switch's alone.  Returns 1 if every word is declared, 0 if not.
static int
patternDeclared(OptFindingP findingP)
{
int i;

    for( i = 0; i < findingP->wordCount; ++i )
    {
        if( !(findingP->wordsP[i]->flags & (OPTF_INREGION | OPTF_INSPEED)) )
        {
            return(0);
        }
    }

    return(1);
}

// Decide one licensed deleting record: timing loops (H2, H3) at every level,
// -O2's guess, a writable edit, overlap with another fired rewrite.  A FIRED
// record gets its words, the kept word's tree and both spellings.
static OptXformFate
spaceGate(OptTableP tableP, OptXformP recP, int allRegion)
{
OptFindingP findingP;
OptWordP keepP;
OptWordP delP;
OptWord after;
unsigned int applies;
unsigned int timing;
PNodeP treeP;
int expected;
char spell[OPTMSG_SIZE];
char spell2[OPTMSG_SIZE];

    findingP = recP->findingP;
    recP->assumed = 0;

    if( touchesHandsOff(tableP, findingP) )
    {
        return(OPTXF_HANDSOFF);
    }

    // A deletion shortens every pass through the words after it, and a delay
    // or device loop is timed by its length, so H2 and H3 refuse at every
    // level, as for S2.  It also moves every word after it, so H6 refuses at
    // every level too.  -O2 adds what it asks of T3 and T8.
    timing = ((1u << OPTGH_H2) | (1u << OPTGH_H3));
    applies = (timing | OPTGH_LENGTH);

    if( optTransformLevel() >= 2 )
    {
        applies |= (allRegion)?OPTGH_INREGION:OPTGH_AUTHORIZED;
    }

    applies &= (OPTGH_AUTHORIZED | OPTGH_LENGTH);

    if( findingP->guessBits & applies & timing )
    {
        recP->assumed = (findingP->guessBits & applies);
        return(OPTXF_TIMED);
    }

    if( findingP->guessBits & applies )
    {
        recP->assumed = (findingP->guessBits & applies);
        return(OPTXF_GUESSED);
    }

    // T1, T1b and T2 keep the first word and delete the second; T6, T7 and
    // T13 delete the first and keep nothing.
    switch( findingP->rule )
    {
    case OPTRULE_T1:
    case OPTRULE_T1B:
    case OPTRULE_T2:
        keepP = findingP->wordsP[0];
        delP = findingP->wordsP[1];
        break;

    default:
        keepP = NILP;
        delP = findingP->wordsP[0];
        break;
    }

    // Relayout deletes a whole statement of one word, and not an origin's.
    if( !representable(delP) || (delP->nodeP->type == ORIGIN) || (keepP && !representable(keepP)) )
    {
        return(OPTXF_UNREPRESENTABLE);
    }

    treeP = NILP;
    expected = delP->value;

    if( keepP && !(treeP = buildSpace(recP, &expected)) )
    {
        return(OPTXF_UNREPRESENTABLE);
    }

    recP->delP = delP;
    recP->keepP = keepP;

    if( spaceOverlaps(tableP, recP) )
    {
        recP->delP = NILP;
        recP->keepP = NILP;
        return(OPTXF_OVERLAP);
    }

    recP->treeP = treeP;
    recP->assumed = applies;
    recP->regionP = optRegionOfWord(tableP, (keepP)?keepP:delP);

    if( keepP )
    {
        recP->before = keepP->value;
        recP->after = expected;
        spellWord(keepP, spell, sizeof(spell));
        spellWord(delP, spell2, sizeof(spell2));
        recP->beforeP = allocPrintf("%s; %s", spell, spell2);
        after = *keepP;
        after.exprP = treeP;
        spellWord(&after, spell, sizeof(spell));
        recP->afterP = allocPrintf("%s", spell);
    }
    else
    {
        recP->before = delP->value;
        recP->after = delP->value;
        spellWord(delP, spell, sizeof(spell));
        recP->beforeP = allocPrintf("%s", spell);
        recP->afterP = allocPrintf("(deleted)");
    }

    return(OPTXF_FIRED);
}

// Build and check the kept word's new expression for T1, T1b or T2: it must
// assemble to the rule's word (*expectedP), and for T1 act as the two words
// did.  The tree shares the source's nodes -- relayout finds a constant's pool
// slot by its node -- so it is never freed.  Returns NILP when the source's
// shape cannot be rewritten.
static PNodeP
buildSpace(OptXformP recP, int *expectedP)
{
OptFindingP findingP;
PNodeP treeP;

    findingP = recP->findingP;

    switch( findingP->rule )
    {
    case OPTRULE_T1:
        treeP = buildT1(findingP->wordsP[0], findingP->wordsP[1], expectedP);
        break;

    case OPTRULE_T1B:
        treeP = buildT1b(findingP->wordsP[0], findingP->wordsP[1], expectedP);
        break;

    case OPTRULE_T2:
        treeP = buildT2(findingP->wordsP[0], expectedP);
        break;

    default:
        return(NILP);
    }

    if( treeP && ((evalExpr(treeP) & WRDMASK) != *expectedP) )
    {
        return(NILP);
    }

    return(treeP);
}

// Build T1: the two operate words side by side ("cla cma", or joined by '|'
// under -a, where a space adds); it must leave AC and IO as the pair did.
// Returns the tree, or NILP.
static PNodeP
buildT1(OptWordP firstP, OptWordP secondP, int *expectedP)
{
OptDecode decode;
int ac1;
int io1;
int ac2;
int io2;

    // lap ORs in the address after its own word, which is one lower once the
    // second word is merged into the first.
    if( secondP->decode.microBits & OPTM_LAP )
    {
        return(NILP);
    }

    *expectedP = ((firstP->value | secondP->value) & WRDMASK);
    memset(&decode, 0, sizeof(decode));
    optDecodeValue(*expectedP, &decode);

    if( decode.group != OPTG_OPERATE )
    {
        return(NILP);
    }

    optSimulateOperate(firstP->decode.microBits, XF_GARBAGE, (XF_GARBAGE ^ WRDMASK), 0, 0, &ac1, &io1);
    optSimulateOperate(secondP->decode.microBits, ac1, io1, 0, 0, &ac1, &io1);
    optSimulateOperate(decode.microBits, XF_GARBAGE, (XF_GARBAGE ^ WRDMASK), 0, 0, &ac2, &io2);

    if( (ac1 != ac2) || (io1 != io2) )
    {
        return(NILP);
    }

    return( binNode(firstP->exprP, (spaceIsAdd)?OR:SEPARATOR, firstP->exprP, secondP->exprP) );
}

// Build T1b: the opcode's name and the summed count, "ral 5s".
// Returns the tree, or NILP when either has no permanent symbol.
static PNodeP
buildT1b(OptWordP firstP, OptWordP secondP, int *expectedP)
{
int total;
char count[8];

    total = (firstP->decode.shiftCount + secondP->decode.shiftCount);

    if( (total < 1) || (total > 9) || !firstP->decode.mnemonicP )
    {
        return(NILP);
    }

    *expectedP = ((firstP->value & ~OPTSH_COUNT) | ((1 << total) - 1));
    sprintf(count, "%ds", total);

    if( !sym_find((SymNodeP *)&permSymP, (char *)firstP->decode.mnemonicP) ||
        !sym_find((SymNodeP *)&permSymP, count) )
    {
        return(NILP);
    }

    return( binNode(firstP->exprP, SEPARATOR, permNode(firstP, firstP->decode.mnemonicP), permNode(firstP, count)) );
}

// Build T2, the reversed skip: a skip group word gains or loses its "i"; sad
// and sas swap, the operand kept.  "sza i" becomes "sza", "sas i [x]" becomes
// "sad i [x]".  Returns the tree, or NILP when the source has another shape.
static PNodeP
buildT2(OptWordP skipP, int *expectedP)
{
PNodeP leavesPP[16];
PNodeP nodeP;
int count;
int i;
int n;
int found;
const char *otherP;

    count = flattenWords(skipP->exprP, leavesPP, 0, (int)(sizeof(leavesPP) / sizeof(leavesPP[0])) - 1);

    if( count <= 0 )
    {
        return(NILP);
    }

    if( skipP->decode.group == OPTG_SKIP )
    {
        *expectedP = (skipP->value ^ 0010000);

        for( i = 0, n = 0, found = 0; i < count; ++i )
        {
            if( leavesPP[i]->type == IMOD )
            {
                found = 1;
                continue;
            }

            leavesPP[n++] = leavesPP[i];
        }

        if( !found )
        {
            leavesPP[n++] = permNode(skipP, "i");
        }

        return( (n > 0)?joinWords(skipP->exprP, leavesPP, n):NILP );
    }

    if( (skipP->decode.group != OPTG_MEMREF) || ((skipP->decode.opcode != 050) && (skipP->decode.opcode != 052)) )
    {
        return(NILP);
    }

    *expectedP = (skipP->value ^ 0020000);
    otherP = (skipP->decode.opcode == 050)?"sas":"sad";

    for( i = 0, found = 0; i < count; ++i )
    {
        nodeP = leavesPP[i];

        if( ((nodeP->type == OPADDR) || (nodeP->type == OPCODE)) && nodeP->value.symP && nodeP->value.symP->name &&
            (!strcmp(nodeP->value.symP->name, "sad") || !strcmp(nodeP->value.symP->name, "sas")) )
        {
            leavesPP[i] = permNode(skipP, otherP);
            ++found;
        }
    }

    return( (found == 1)?joinWords(skipP->exprP, leavesPP, count):NILP );
}

// Collect, from count on, the leaves of a chain of SEPARATOR operators, left
// to right.  Returns the new count, or -1 when there are more than max.
static int
flattenWords(PNodeP nodeP, PNodeP *leavesPP, int count, int max)
{
    if( count < 0 )
    {
        return(-1);
    }

    if( (nodeP->type == BINOP) && (nodeP->value.ival == SEPARATOR) && nodeP->leftP && nodeP->rightP )
    {
        count = flattenWords(nodeP->leftP, leavesPP, count, max);
        return( flattenWords(nodeP->rightP, leavesPP, count, max) );
    }

    if( count >= max )
    {
        return(-1);
    }

    leavesPP[count] = nodeP;
    return(count + 1);
}

// Join words left to right with a space, or '|' under -a, where a space adds.
static PNodeP
joinWords(PNodeP modelP, PNodeP *leavesPP, int count)
{
PNodeP treeP;
int i;

    treeP = leavesPP[0];

    for( i = 1; i < count; ++i )
    {
        treeP = binNode(modelP, (spaceIsAdd)?OR:SEPARATOR, treeP, leavesPP[i]);
    }

    return(treeP);
}

// Test whether another fired rewrite involves a word of this pattern: an
// earlier deletion, a T3 or T8 word or the word a T3 lands on (whose number
// relayout re-points), an inline's site, callee or return, a placement's jmp
// or run, a loop.  Returns 1 if one does, 0 if none does.
static int
spaceOverlaps(OptTableP tableP, OptXformP recP)
{
OptFindingP findingP;
OptXformP otherP;
OptInlineP inP;
OptPlaceP pP;
OptUnrollP uP;
OptWordP wordP;
int addr;
int i;

    findingP = recP->findingP;

    for( otherP = tableP->xformsP; otherP; otherP = otherP->nextP )
    {
        if( (otherP == recP) || (otherP->fate != OPTXF_FIRED) )
        {
            continue;
        }

        if( optRuleDeletes(otherP->findingP->rule) )
        {
            for( i = 0; i < otherP->findingP->wordCount; ++i )
            {
                if( patternHolds(findingP, otherP->findingP->wordsP[i]->bank, otherP->findingP->wordsP[i]->addr) )
                {
                    return(1);
                }
            }

            continue;
        }

        if( patternHolds(findingP, otherP->findingP->bank, otherP->findingP->addr) )
        {
            return(1);
        }

        if( (otherP->findingP->rule == OPTRULE_T3) && otherP->chainPP && (otherP->hops > 0) &&
            (wordP = wordAt(tableP, otherP->findingP->bank, otherP->chainPP[otherP->hops - 1]->decode.address)) &&
            (wordP == recP->delP) )
        {
            return(1);
        }
    }

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        if( patternHolds(findingP, inP->siteP->bank, inP->siteP->addr) )
        {
            return(1);
        }

        for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
        {
            if( patternHolds(findingP, inP->siteP->bank, addr) )
            {
                return(1);
            }
        }

        if( (wordP = inP->shape.rtnP) && patternHolds(findingP, wordP->bank, wordP->addr) )
        {
            return(1);
        }
    }

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        if( patternHolds(findingP, pP->shape.jmpP->bank, pP->shape.jmpP->addr) )
        {
            return(1);
        }

        for( addr = pP->shape.targetP->addr; addr <= pP->shape.endP->addr; ++addr )
        {
            if( patternHolds(findingP, pP->shape.jmpP->bank, addr) )
            {
                return(1);
            }
        }
    }

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( uP->fate != OPTUN_FIRED )
        {
            continue;
        }

        for( addr = uP->shape.setP->addr; addr <= uP->shape.jmpP->addr; ++addr )
        {
            if( patternHolds(findingP, uP->shape.ispP->bank, addr) )
            {
                return(1);
            }
        }
    }

    return(0);
}

// Test whether a fired rewrite involves one word, for optPlanWindow(): the
// words spaceOverlaps() tests for a pattern, and also the word a deleting
// finding deletes when it is the one just before.  This word then follows the
// word before that one, and if that is a skip, deleting this one too would
// change what the skip passes over.  (T2's own third word, which comes to
// follow its skip, is in its pattern.)  Returns 1 if a fired rewrite does, 0
// if none does.
int
optWordTouched(OptTableP tableP, OptWordP wordP)
{
OptFinding one;
OptXformP otherP;
OptInlineP inP;
OptPlaceP pP;
OptUnrollP uP;
OptWordP landP;
int addr;
int i;

    memset(&one, 0, sizeof(one));
    one.wordCount = 1;
    one.wordsP[0] = wordP;

    for( otherP = tableP->xformsP; otherP; otherP = otherP->nextP )
    {
        if( otherP->fate != OPTXF_FIRED )
        {
            continue;
        }

        if( optRuleDeletes(otherP->findingP->rule) )
        {
            for( i = 0; i < otherP->findingP->wordCount; ++i )
            {
                if( otherP->findingP->wordsP[i] == wordP )
                {
                    return(1);
                }
            }

            if( otherP->delP && (otherP->delP->bank == wordP->bank) && (otherP->delP->addr == (wordP->addr - 1)) )
            {
                return(1);
            }

            continue;
        }

        if( patternHolds(&one, otherP->findingP->bank, otherP->findingP->addr) )
        {
            return(1);
        }

        if( (otherP->findingP->rule == OPTRULE_T3) && otherP->chainPP && (otherP->hops > 0) &&
            (landP = wordAt(tableP, otherP->findingP->bank, otherP->chainPP[otherP->hops - 1]->decode.address)) &&
            (landP == wordP) )
        {
            return(1);
        }
    }

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        if( patternHolds(&one, inP->siteP->bank, inP->siteP->addr) )
        {
            return(1);
        }

        for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
        {
            if( patternHolds(&one, inP->siteP->bank, addr) )
            {
                return(1);
            }
        }

        if( (landP = inP->shape.rtnP) && patternHolds(&one, landP->bank, landP->addr) )
        {
            return(1);
        }
    }

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        if( patternHolds(&one, pP->shape.jmpP->bank, pP->shape.jmpP->addr) )
        {
            return(1);
        }

        for( addr = pP->shape.targetP->addr; addr <= pP->shape.endP->addr; ++addr )
        {
            if( patternHolds(&one, pP->shape.jmpP->bank, addr) )
            {
                return(1);
            }
        }
    }

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( uP->fate != OPTUN_FIRED )
        {
            continue;
        }

        for( addr = uP->shape.setP->addr; addr <= uP->shape.jmpP->addr; ++addr )
        {
            if( patternHolds(&one, uP->shape.ispP->bank, addr) )
            {
                return(1);
            }
        }
    }

    return(0);
}

// representable(), for optwindow.c.
int
optWordRepresentable(OptWordP entryP)
{
    return( representable(entryP) );
}

// Test whether a word is one of a finding's pattern.
// Returns 1 if it is, 0 if not.
static int
patternHolds(OptFindingP findingP, int bank, int addr)
{
int i;

    for( i = 0; i < findingP->wordCount; ++i )
    {
        if( (findingP->wordsP[i]->bank == bank) && (findingP->wordsP[i]->addr == addr) )
        {
            return(1);
        }
    }

    return(0);
}

// Bisection.

// Number the records that would fire, in record order from 1, and switch off
// (fate OFF, recording the modifiers) every one a bisection modifier does not
// keep.  Record order is fixed by the analysis, so -O=upto=N names the same
// rewrites on every build.  Any subset is sound because a fate reads only the
// analysis, never an earlier rewrite.  A modifier names a rewrite by its
// word, so two firing records on one word is fatal (none can today).
static void
selectForBisection(OptTableP tableP)
{
OptXformP recP;
OptXformP earlierP;
OptInlineP inP;
OptPlaceP pP;
OptUnrollP uP;
OptWinDelP delP;
OptWordP entryP;
unsigned int offBy;
unsigned int bit;
int ordinal;
int b;

    ordinal = 0;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        // Deletions are numbered after the loops, so no T3 or T8 ordinal moves.
        if( (recP->fate != OPTXF_FIRED) || optRuleDeletes(recP->findingP->rule) )
        {
            continue;
        }

        entryP = recP->findingP->wordsP[0];

        for( earlierP = tableP->xformsP; earlierP != recP; earlierP = earlierP->nextP )
        {
            if( earlierP->ordinal && (earlierP->findingP->wordsP[0] == entryP) )
            {
                internalError(entryP, "%s and %s would both rewrite bank %d %04o, so a bisection list cannot name one of them",
                    optRuleName(earlierP->findingP->rule), optRuleName(recP->findingP->rule),
                    recP->findingP->bank, recP->findingP->addr);
            }
        }

        recP->ordinal = ++ordinal;
        offBy = optBisectSelect(recP->ordinal, recP->findingP->bank, recP->findingP->addr);

        if( !offBy )
        {
            continue;
        }

        recP->fate = OPTXF_OFF;
        recP->offBy = offBy;
        --tableP->xformFates[OPTXF_FIRED];
        ++tableP->xformFates[OPTXF_OFF];

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->xformOffBy[b];
            }
        }
    }

    tableP->xformBisect = 1;
    tableP->xformCandidates = ordinal;

    // Inline sites, numbered on from the last T3 or T8.  No fired copy holds
    // another, and a call word is never a T3 or T8 word, so any subset is sound.
    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        inP->ordinal = ++ordinal;
        ++tableP->inlineCandidates;

        if( !(offBy = optBisectSelect(inP->ordinal, inP->siteP->bank, inP->siteP->addr)) )
        {
            continue;
        }

        inP->fate = OPTIN_OFF;
        inP->offBy = offBy;
        inP->deletes = 0;
        --tableP->inlineFates[OPTIN_FIRED];
        ++tableP->inlineFates[OPTIN_OFF];
        tableP->inlineSpent[inP->siteP->bank] -= inP->shape.spent;
        tableP->inlineTime -= inP->shape.best;

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->inlineOffBy[b];
            }
        }
    }

    // Placements, numbered on and named by their jmp.  placeOverlaps() keeps
    // them apart from every other rewrite, so any subset is sound.
    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        pP->ordinal = ++ordinal;
        ++tableP->placeCandidates;

        if( !(offBy = optBisectSelect(pP->ordinal, pP->shape.jmpP->bank, pP->shape.jmpP->addr)) )
        {
            continue;
        }

        pP->fate = OPTPL_OFF;
        pP->offBy = offBy;
        --tableP->placeFates[OPTPL_FIRED];
        ++tableP->placeFates[OPTPL_OFF];
        tableP->placeTime -= pP->shape.save;

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->placeOffBy[b];
            }
        }
    }

    // Loops, numbered on and named by their isp.  unrollOverlaps() keeps them
    // apart from every other rewrite, so any subset is sound.
    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( uP->fate != OPTUN_FIRED )
        {
            continue;
        }

        uP->ordinal = ++ordinal;
        ++tableP->unrollCandidates;

        if( !(offBy = optBisectSelect(uP->ordinal, uP->shape.ispP->bank, uP->shape.ispP->addr)) )
        {
            continue;
        }

        uP->fate = OPTUN_OFF;
        uP->offBy = offBy;
        --tableP->unrollFates[OPTUN_FIRED];
        ++tableP->unrollFates[OPTUN_OFF];
        tableP->unrollSpent[uP->shape.ispP->bank] -= uP->shape.spent;
        tableP->unrollTime -= uP->shape.save;

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->unrollOffBy[b];
            }
        }
    }

    // Deletions, numbered on and named by the pattern's first word;
    // spaceOverlaps() keeps them apart.  Switched off, they are counted with
    // the T3s and T8s under Not rewritten.
    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate != OPTXF_FIRED) || !optRuleDeletes(recP->findingP->rule) )
        {
            continue;
        }

        recP->ordinal = ++ordinal;
        ++tableP->spaceCandidates;

        if( !(offBy = optBisectSelect(recP->ordinal, recP->findingP->bank, recP->findingP->addr)) )
        {
            continue;
        }

        recP->fate = OPTXF_OFF;
        recP->offBy = offBy;
        --tableP->xformFates[OPTXF_FIRED];
        ++tableP->xformFates[OPTXF_OFF];
        tableP->spaceWords -= recP->findingP->saveWords;
        tableP->spaceTime -= recP->findingP->saveTime;
        recP->treeP = NILP;         // not freed: it shares the source's nodes (buildSpace())

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->xformOffBy[b];
            }
        }
    }

    // The eem and lem deletions, numbered on in record order, so every lem a
    // freed eem depends on has a lower number than the eem: -O=upto keeps or
    // drops them together.  A range or a list can split them; the freed eem is
    // then switched off with its lem.
    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        if( delP->fate != OPTWD_FIRED )
        {
            continue;
        }

        delP->ordinal = ++ordinal;
        ++tableP->windelCandidates;

        if( !(offBy = optBisectSelect(delP->ordinal, delP->wordP->bank, delP->wordP->addr)) )
        {
            continue;
        }

        delP->fate = OPTWD_OFF;
        delP->offBy = offBy;
        --tableP->windelFates[OPTWD_FIRED];
        ++tableP->windelFates[OPTWD_OFF];

        for( b = 0, bit = 1u; b < OPTBIS_COUNT; ++b, bit <<= 1 )
        {
            if( offBy & bit )
            {
                ++tableP->windelOffBy[b];
            }
        }
    }

    optWindowSettleOff(tableP);
    optBisectWarnUnmatched();
}

// The totals.

// Count the pool words no instruction names once the fired T8 words stop
// naming them: every edge into the word comes from a fired T8 reading it, so a
// shared pool word counts once and one still read elsewhere not at all.  A T8
// switched off by bisection is not fired, so it frees nothing.
static void
tallyFreed(OptTableP tableP)
{
OptXformP recP;
OptXformP earlierP;
OptEdgeP edgeP;
int seen;
int others;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate != OPTXF_FIRED) || !ruleIsT8(recP->findingP->rule) )
        {
            continue;
        }

        seen = 0;

        for( earlierP = tableP->xformsP; earlierP != recP; earlierP = earlierP->nextP )
        {
            if( (earlierP->fate == OPTXF_FIRED) && ruleIsT8(earlierP->findingP->rule) &&
                (earlierP->throughP == recP->throughP) )
            {
                seen = 1;
                break;
            }
        }

        if( seen )
        {
            continue;
        }

        others = 0;

        for( edgeP = recP->throughP->inP; edgeP; edgeP = edgeP->nextInP )
        {
            if( !firedT8From(tableP, edgeP->fromP, recP->throughP) )
            {
                ++others;
            }
        }

        if( !others )
        {
            ++tableP->xformFreed;

            // Relayout may collect it only where a declaration, or
            // -O=undeclared, licensed every rewrite that stopped naming it; a
            // T8 fired on -O2's guess outside a region, without the switch,
            // frees it for the report only.  Relayout establishes the word's
            // liveness itself from the edited program.
            recP->freesPool = 1;
            recP->licensedPool = poolReadersDeclared(tableP, recP->throughP);

            if( tableP->xformApplied )
            {
                if( recP->licensedPool )
                {
                    ++tableP->poolFreeable;
                }
                else
                {
                    ++tableP->poolAdvice;
                }
            }
        }
    }
}

// Test whether the reclaim of this pool word is licensed: every fired T8
// reading it is in a declared region, or -O=undeclared was given.  A reader
// outside is -O2's guess, and without the switch reclaiming its word would turn
// a same-length rewrite into one that moves every later word.
// Returns 1 when it is licensed, 0 when it is not.
static int
poolReadersDeclared(OptTableP tableP, OptWordP poolP)
{
    return( optUndeclared() || poolReadersInRegions(tableP, poolP) );
}

// Test whether every fired T8 reading this pool word is in a declared region,
// whatever the switch says; the report marks a word that fails it as the
// switch's alone.  Returns 1 when they all are, 0 when any one is not.
static int
poolReadersInRegions(OptTableP tableP, OptWordP poolP)
{
OptXformP recP;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate == OPTXF_FIRED) && ruleIsT8(recP->findingP->rule) &&
            (recP->throughP == poolP) && !recP->regionP )
        {
            return(0);
        }
    }

    return(1);
}

// Test whether a word is a fired T8 word that reads this pool word.
// Returns 1 if it is, 0 if not.
static int
firedT8From(OptTableP tableP, OptWordP fromP, OptWordP poolP)
{
OptXformP recP;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate == OPTXF_FIRED) && ruleIsT8(recP->findingP->rule) &&
            (recP->findingP->wordsP[0] == fromP) && (recP->throughP == poolP) )
        {
            return(1);
        }
    }

    return(0);
}

// Append a record, keeping the list in finding order.
static void
addRecord(OptTableP tableP, OptXformP recP)
{
    if( tableP->xformsTailP )
    {
        tableP->xformsTailP->nextP = recP;
    }
    else
    {
        tableP->xformsP = recP;
    }

    tableP->xformsTailP = recP;
}

// Stop the assembly on an inconsistency this file should never meet, in the
// shape of am1's other line-numbered errors.  leave() removes the .opt file;
// no code generator has run yet.  Does not return.
static void
internalError(OptWordP entryP, const char *fmtP, ...)
{
va_list ap;

    fprintf(stderr, "am1: internal error in -O%d: ", (optTransformLevel() >= 2)?2:1);
    va_start(ap, fmtP);
    vfprintf(stderr, fmtP, ap);
    va_end(ap);
    fprintf(stderr, "\nat line %d, file %s\n", entryP->lineNo, (entryP->fileP)?entryP->fileP:"-");
    leave(0);
}

// Names, the dump, and release.

// Name a fate as one word, for the dump.
// Returns a static string, never NILP.
const char *
optXformFateName(OptXformFate fate)
{
    if( (fate < 0) || (fate >= OPTXF_COUNT) )
    {
        return("?");
    }

    return( fateTable[fate].tagP );
}

// Name a fate as a phrase, for the report, in the words of the level that ran.
// Returns a static string, never NILP.
const char *
optXformFateText(OptXformFate fate)
{
    if( (fate < 0) || (fate >= OPTXF_COUNT) )
    {
        return("?");
    }

    if( (optTransformLevel() >= 2) && fateTable[fate].text2P )
    {
        return( fateTable[fate].text2P );
    }

    return( fateTable[fate].textP );
}

// Print the transform dump (-O=xform): a summary line headed "labelP:", then
// one line per live finding headed labelP and the fate, so a check script can
// count them.  firedOnly omits unfired lines, for the idempotence pass.
//
//   xform: applied fired 3 noregions 0 outside 2 partial 1 notcarried 2
//          cycle 0 long 0 unrepresentable 0 freed 1 us 10 hops 1 hopus 5
//          throughtaken 0 throughmaybe 0 contradicted 0 refusedinside 1
//   xform fired T8d 0 0103 t8d.am1:12 200120 700005 through 0 0120 clean
//          region 1 | lac [5] => law 0o5
//   xform outside T2 0 0110 t2.am1:15
//
// (each on one line).  "dryrun" means -O=xform without a level.  Summary
// fields appear only where they can be non-zero, so a plain -O1 dump stays
// stable: "handsoff N guessed N" under -O2, with a nooptimize span, or when H6
// guessed below -O2; "off N" and each line's "#n" only with a bisection
// modifier; and the s1, s4, s2 and space fields only when such a site was
// recorded.
void
optDumpTransforms(FILE *fP, OptTableP tableP, const char *labelP, int firedOnly)
{
OptXformP recP;
OptInlineP inP;
OptPlaceP pP;
OptUnrollP uP;
OptFindingP findingP;
OptWordP entryP;
OptWordP throughP;
int fate;
int last;
int h;
int first;
int words;

    fprintf(fP, "%s: %s", labelP, (tableP->xformApplied)?"applied":"dryrun");
    last = ((optTransformLevel() >= 2) || tableP->handsOffCount || tableP->xformFates[OPTXF_GUESSED])?
        OPTXF_OFF:OPTXF_HANDSOFF;

    // "fired" and "off" count the inline, placement and loop sites too, since
    // am1bisect.sh reads the rewrites to bisect from these two.
    for( fate = 0; fate < last; ++fate )
    {
        fprintf(fP, " %s %d", optXformFateName((OptXformFate)fate), tableP->xformFates[fate] +
            ((fate == OPTXF_FIRED)?(tableP->inlineFates[OPTIN_FIRED] + tableP->placeFates[OPTPL_FIRED] +
            tableP->unrollFates[OPTUN_FIRED]):0));
    }

    // "off N" only when the selection ran, so no other dump changes.
    if( tableP->xformBisect )
    {
        fprintf(fP, " %s %d", optXformFateName(OPTXF_OFF),
            tableP->xformFates[OPTXF_OFF] + tableP->inlineFates[OPTIN_OFF] + tableP->placeFates[OPTPL_OFF] +
            tableP->unrollFates[OPTUN_OFF]);
    }

    fprintf(fP, " freed %d us %d hops %d hopus %d throughtaken %d throughmaybe %d contradicted %d refusedinside %d",
        tableP->xformFreed, tableP->xformTime, tableP->xformHops, tableP->xformHopTime,
        tableP->xformThroughTaken, tableP->xformThroughMaybe, tableP->xformContradicted,
        tableP->xformRefusedInside);

    // Each inline fate is "s1" plus the fate, so none reads as " fired " or
    // " off " above.
    if( tableP->inlineCount )
    {
        fprintf(fP, " s1sites %d", tableP->inlineCount);

        for( fate = 0; fate < OPTIN_COUNT; ++fate )
        {
            if( (fate != OPTIN_OFF) || tableP->xformBisect )
            {
                fprintf(fP, " s1%s %d", optInlineFateName((OptInlineFate)fate), tableP->inlineFates[fate]);
            }
        }

        for( words = 0, h = 0; h <= MAXBANK; ++h )
        {
            words += tableP->inlineSpent[h];
        }

        fprintf(fP, " s1words %d s1us %d", words, tableP->inlineTime);
    }

    if( tableP->placeCount )
    {
        fprintf(fP, " s4sites %d", tableP->placeCount);

        for( fate = 0; fate < OPTPL_COUNT; ++fate )
        {
            if( (fate != OPTPL_OFF) || tableP->xformBisect )
            {
                fprintf(fP, " s4%s %d", optPlaceFateName((OptPlaceFate)fate), tableP->placeFates[fate]);
            }
        }

        fprintf(fP, " s4us %d", tableP->placeTime);
    }

    if( tableP->unrollCount )
    {
        fprintf(fP, " s2sites %d", tableP->unrollCount);

        for( fate = 0; fate < OPTUN_COUNT; ++fate )
        {
            if( (fate != OPTUN_OFF) || tableP->xformBisect )
            {
                fprintf(fP, " s2%s %d", optUnrollFateName((OptUnrollFate)fate), tableP->unrollFates[fate]);
            }
        }

        for( words = 0, h = 0; h <= MAXBANK; ++h )
        {
            words += tableP->unrollSpent[h];
        }

        fprintf(fP, " s2words %d s2us %d", words, tableP->unrollTime);
    }

    // Space mode's own fates; its FIRED and OFF are counted in " fired " and
    // " off " above, with the T3s and T8s.
    if( tableP->spaceGated )
    {
        fprintf(fP, " %s %d %s %d spacewords %d spaceus %d", optXformFateName(OPTXF_TIMED),
            tableP->xformFates[OPTXF_TIMED], optXformFateName(OPTXF_OVERLAP), tableP->xformFates[OPTXF_OVERLAP],
            tableP->spaceWords, tableP->spaceTime);
    }

    fprintf(fP, "\n");

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( firedOnly && (recP->fate != OPTXF_FIRED) )
        {
            continue;
        }

        if( (recP->fate == OPTXF_FIRED) && optRuleDeletes(recP->findingP->rule) )
        {
            fprintf(fP, "%s ", labelP);
            printSpaceLine(fP, recP);
            continue;
        }

        findingP = recP->findingP;
        entryP = findingP->wordsP[0];
        fprintf(fP, "%s %s %s %d %04o %s:%d", labelP, optXformFateName(recP->fate),
            optRuleName(findingP->rule), findingP->bank, findingP->addr,
            (entryP->fileP)?entryP->fileP:"-", entryP->lineNo);

        if( recP->fate != OPTXF_FIRED )
        {
            if( ((recP->fate == OPTXF_GUESSED) || (recP->fate == OPTXF_TIMED)) && recP->assumed )
            {
                fprintf(fP, " by");
                first = 1;

                for( h = 0; h < OPTGH_COUNT; ++h )
                {
                    if( recP->assumed & (1u << h) )
                    {
                        fprintf(fP, "%s%s", (first)?" ":",", optGuessName((OptGuessId)h));
                        first = 0;
                    }
                }
            }

            // The modifiers that switched it off, and its ordinal.
            if( recP->fate == OPTXF_OFF )
            {
                fprintf(fP, " by");
                first = 1;

                for( h = 0; h < OPTBIS_COUNT; ++h )
                {
                    if( recP->offBy & (1u << h) )
                    {
                        fprintf(fP, "%s%s", (first)?" ":",", optBisectName(1u << h));
                        first = 0;
                    }
                }

                fprintf(fP, " #%d", recP->ordinal);
            }

            fprintf(fP, "\n");
            continue;
        }

        throughP = recP->throughP;
        fprintf(fP, " %06o %06o through %d %04o ", recP->before, recP->after, throughP->bank, throughP->addr);

        if( !(throughP->flags & (OPTF_TAKEN | OPTF_MAYBE_WRITTEN)) )
        {
            fprintf(fP, "clean");
        }
        else
        {
            fprintf(fP, "%s%s%s", (throughP->flags & OPTF_TAKEN)?"taken":"",
                ((throughP->flags & OPTF_TAKEN) && (throughP->flags & OPTF_MAYBE_WRITTEN))?",":"",
                (throughP->flags & OPTF_MAYBE_WRITTEN)?"maybe-written":"");
        }

        fprintf(fP, " region %d | %s => %s", (recP->regionP)?recP->regionP->id:0,
            (recP->beforeP)?recP->beforeP:"-", (recP->afterP)?recP->afterP:"-");

        // A chain names every jmp it jumps past and the word that stopped it
        // short; a single unstopped hop prints neither.
        if( recP->hops > 1 )
        {
            fprintf(fP, " via");

            for( h = 0; h < recP->hops; ++h )
            {
                fprintf(fP, " %04o", recP->chainPP[h]->addr);
            }
        }

        if( recP->stopP )
        {
            fprintf(fP, " stopped %04o %s", recP->stopP->addr,
                (recP->stopP->flags & OPTF_HANDSOFF)?"handsoff":"guessed");
        }

        if( recP->assumed )
        {
            fprintf(fP, " assumes");
            first = 1;

            for( h = 0; h < OPTGH_COUNT; ++h )
            {
                if( recP->assumed & (1u << h) )
                {
                    fprintf(fP, "%s%s", (first)?" ":",", optGuessName((OptGuessId)h));
                    first = 0;
                }
            }
        }

        // The ordinal goes last, so -O=upto=N can be read off the dump and
        // every earlier field keeps its place.
        if( recP->ordinal )
        {
            fprintf(fP, " #%d", recP->ordinal);
        }

        fprintf(fP, "\n");
    }

    // Inline, placement and loop lines follow, in the order bisection
    // numbers them.
    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( !firedOnly || (inP->fate == OPTIN_FIRED) )
        {
            printInlineLine(fP, labelP, inP, optBankCeilingDeclared(tableP, inP->siteP->bank));
        }
    }

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( !firedOnly || (pP->fate == OPTPL_FIRED) )
        {
            printPlaceLine(fP, labelP, pP);
        }
    }

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( !firedOnly || (uP->fate == OPTUN_FIRED) )
        {
            printUnrollLine(fP, labelP, uP, optBankCeilingDeclared(tableP, uP->shape.ispP->bank));
        }
    }
}

// Print one fired deletion's -O=xform dump line, after its label: the finding's
// fields, the word deleted, the word kept and what it becomes, then the tail a
// T3 or T8 line has.  The relayout dump says whether the deletion was made.
//
//   xform fired T2 0 0373 f.am1:589 deletes 0 0374 keeps 0 0373 640100 650100
//          region 3 | sza; jmp x => sza i assumes H2,H3 #7
static void
printSpaceLine(FILE *fP, OptXformP recP)
{
OptFindingP findingP;
OptWordP entryP;
int h;
int first;

    findingP = recP->findingP;
    entryP = findingP->wordsP[0];
    fprintf(fP, "%s %s %d %04o %s:%d deletes %d %04o", optXformFateName(recP->fate), optRuleName(findingP->rule),
        findingP->bank, findingP->addr, (entryP->fileP)?entryP->fileP:"-", entryP->lineNo, recP->delP->bank,
        recP->delP->addr);

    if( recP->keepP )
    {
        fprintf(fP, " keeps %d %04o %06o %06o", recP->keepP->bank, recP->keepP->addr, recP->before, recP->after);
    }

    fprintf(fP, " region %d | %s => %s", (recP->regionP)?recP->regionP->id:0,
        (recP->beforeP)?recP->beforeP:"-", (recP->afterP)?recP->afterP:"-");

    if( recP->assumed )
    {
        fprintf(fP, " assumes");
        first = 1;

        for( h = 0; h < OPTGH_COUNT; ++h )
        {
            if( recP->assumed & (1u << h) )
            {
                fprintf(fP, "%s%s", (first)?" ":",", optGuessName((OptGuessId)h));
                first = 0;
            }
        }
    }

    if( recP->ordinal )
    {
        fprintf(fP, " #%d", recP->ordinal);
    }

    fprintf(fP, "\n");
}

// Print one inline site's -O=xform dump line, in the findings' shape, then
// what the judge said of it:
//
//   xform fired S1 0 0203 f.am1:40 callee 0 0300 sub body 5 copy 3 spent 2
//          save 20..20 speed | jsp sub => copy 0301-0303 #3
//
// (one line).  "single net N" follows a single-site callee's save, "marked"
// and "speed" say which declaration reached it, and the tail after '|' says
// what the edit is or why there is none.
static void
printInlineLine(FILE *fP, const char *labelP, OptInlineP inP, int declared)
{
OptWordP siteP;
OptInlineShapeP sP;
unsigned int bits;
char spell[OPTMSG_SIZE];
int h;
int first;

    siteP = inP->siteP;
    sP = &inP->shape;
    fprintf(fP, "%s %s S1 %d %04o %s:%d", labelP, optInlineFateName(inP->fate), siteP->bank, siteP->addr,
        (siteP->fileP)?siteP->fileP:"-", siteP->lineNo);

    if( sP->entryP )
    {
        fprintf(fP, " callee %d %04o %s", sP->entryP->bank, sP->entryP->addr, firstLabelName(sP->entryP));
    }
    else
    {
        fprintf(fP, " callee -");
    }

    if( sP->eligible )
    {
        fprintf(fP, " body %d copy %d spent %d save %d..%d", sP->body, sP->copy, sP->spent, sP->best, sP->worst);

        if( sP->single )
        {
            fprintf(fP, " single net %d", sP->net);
        }
    }

    fprintf(fP, "%s%s", (inP->marked)?" marked":"", (inP->inSpeed)?" speed":"");
    spellWord(siteP, spell, sizeof(spell));
    fprintf(fP, " | %s", spell);

    switch( inP->fate )
    {
    case OPTIN_FIRED:
    case OPTIN_OFF:
        fprintf(fP, " => %scopy %04o-%04o", (sP->isJda)?"dac, ":"", sP->entryP->addr + 1, sP->lastP->addr - 1);

        if( inP->retargetCount )
        {
            fprintf(fP, " retarget");

            for( h = 0; h < inP->retargetCount; ++h )
            {
                fprintf(fP, " %04o", inP->retargetsPP[h]->addr);
            }
        }

        if( inP->deletes )
        {
            fprintf(fP, " delete %04o-%04o", sP->entryP->addr, sP->lastP->addr);

            // A dac form's return word outside the body is deleted too
            // (optrelayout.c, addInlineEdits()).
            if( !sP->dapForm && sP->rtnP &&
                ((sP->rtnP->addr < sP->entryP->addr) || (sP->rtnP->addr > sP->lastP->addr)) )
            {
                fprintf(fP, " and %04o", sP->rtnP->addr);
            }
        }
        break;

    case OPTIN_REFUSED:
        fprintf(fP, " %s%s", optInlineWhyName(sP->why), (sP->why == optInlineNotCopyable())?sP->detail:"");
        break;

    case OPTIN_CAP:
        fprintf(fP, " body over %d", optInlineCap());
        break;

    // Name the declared ceiling these two ran out of, if there is one.
    case OPTIN_BUDGET:
    case OPTIN_FULL:
        if( declared )
        {
            fprintf(fP, " declared ceiling %04o", declared);
        }
        break;

    default:
        break;
    }

    bits = inP->assumed;

    if( bits && ((inP->fate == OPTIN_FIRED) || (inP->fate == OPTIN_GUESSED)) )
    {
        fprintf(fP, " %s", (inP->fate == OPTIN_GUESSED)?"by":"assumes");
        first = 1;

        for( h = 0; h < OPTGH_COUNT; ++h )
        {
            if( bits & (1u << h) )
            {
                fprintf(fP, "%s%s", (first)?" ":",", optGuessName((OptGuessId)h));
                first = 0;
            }
        }
    }

    if( inP->fate == OPTIN_OFF )
    {
        fprintf(fP, " by");
        first = 1;

        for( h = 0; h < OPTBIS_COUNT; ++h )
        {
            if( inP->offBy & (1u << h) )
            {
                fprintf(fP, "%s%s", (first)?" ":",", optBisectName(1u << h));
                first = 0;
            }
        }
    }

    if( inP->ordinal )
    {
        fprintf(fP, " #%d", inP->ordinal);
    }

    fprintf(fP, "\n");
}

// Name an inline fate, as the dump and the report print it.
// Returns a static string, never NILP.
const char *
optInlineFateName(OptInlineFate fate)
{
static const char *namesPP[OPTIN_COUNT] =
{
    "fired", "refused", "handsoff", "guessed", "unrepresentable", "nested", "cap", "budget", "full", "off"
};

    if( (fate < 0) || (fate >= OPTIN_COUNT) )
    {
        return("?");
    }

    return(namesPP[fate]);
}

// Write the report's inline section: each declared site's fate, what relayout
// made of each copy, and each bank's budget.  Written after relayout, only
// when a declaration reached a site.
void
writeInlineReport(FILE *fP, OptTableP tableP)
{
OptInlineP inP;
OptInlineShapeP sP;
int bank;
int words;
int refused;
int deleted;

    if( !tableP->inlineCount )
    {
        return;
    }

    for( words = 0, bank = 0; bank <= MAXBANK; ++bank )
    {
        words += tableP->inlineSpent[bank];
    }

    for( refused = 0, deleted = 0, inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        refused += ((inP->fate == OPTIN_FIRED) && (inP->outcome > 0));
        deleted += inP->deleted;
    }

    fprintf(fP, "\nInline expansion (-O%d): %d call site%s declared, %d copied\n",
        (optTransformLevel() >= 2)?2:1, tableP->inlineCount, (tableP->inlineCount == 1)?"":"s",
        tableP->inlineFates[OPTIN_FIRED] - refused);
    fprintf(fP, "  A site is declared when its word is in a speed region or its callee is marked\n");
    fprintf(fP, "  '%%%%inline'.  Cap %d words, reserve %d words.  %d word%s budgeted, %d deleted with\n",
        optInlineCap(), optInlineReserve(), words, (words == 1)?"":"s", deleted);
    fprintf(fP, "  single-site callees; %d microseconds saved per execution of every copied site,\n",
        tableP->inlineTime);
    fprintf(fP, "  counted by each site's best return.  %d cop%s refused by relayout.\n",
        refused, (refused == 1)?"y was":"ies were");

    for( bank = 0; bank <= MAXBANK; ++bank )
    {
        if( tableP->inlineSpent[bank] )
        {
            fprintf(fP, "  bank %2d: %d words budgeted, ceiling %04o%s\n", bank, tableP->inlineSpent[bank],
                optBankCeiling(tableP, bank), (optBankCeilingDeclared(tableP, bank))?" (declared)":"");
        }
    }

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        sP = &inP->shape;
        fprintf(fP, "    S1   bank %2d %04o  %s:%d  %s  ", inP->siteP->bank, inP->siteP->addr,
            (inP->siteP->fileP)?inP->siteP->fileP:"-", inP->siteP->lineNo,
            (sP->entryP)?firstLabelName(sP->entryP):"-");

        switch( inP->fate )
        {
        case OPTIN_FIRED:
            if( inP->outcome > 0 )
            {
                fprintf(fP, "NOT copied: relayout refused the copy (%s), and the call stays\n", inP->outcomeP);
            }
            else
            {
                fprintf(fP, "copied, %d word%s, saves %d us", sP->spent, (sP->spent == 1)?"":"s", sP->best);

                if( inP->deletes )
                {
                    fprintf(fP, "; callee deleted, %d word%s%s", inP->deleted, (inP->deleted == 1)?"":"s",
                        (inP->deleteRefused)?", the rest refused by relayout":"");
                }

                fprintf(fP, "\n");
            }
            break;

        case OPTIN_REFUSED:
            fprintf(fP, "not copied: %s%s\n", optInlineWhyName(sP->why),
                (sP->why == optInlineNotCopyable())?sP->detail:"");
            break;

        case OPTIN_CAP:
            fprintf(fP, "not copied: body %d words, over the cap of %d\n", sP->body, optInlineCap());
            break;

        case OPTIN_OFF:
            fprintf(fP, "not copied: switched off for bisection, rewrite #%d\n", inP->ordinal);
            break;

        case OPTIN_HANDSOFF:
            fprintf(fP, "not copied: the call or the callee is in a nooptimize span\n");
            break;

        case OPTIN_GUESSED:
            fprintf(fP, "not copied: left alone on -O2's guess\n");
            break;

        case OPTIN_UNREPRESENTABLE:
            fprintf(fP, "not copied: the call, or a word the copy must retarget, is not one statement's\n");
            fprintf(fP, "      whole expression written without a constant, or the jda is not \"jda NAME\"\n");
            break;

        case OPTIN_NESTED:
            fprintf(fP, "not copied: another copy holds this call, or this copy would hold another\n");
            break;

        case OPTIN_BUDGET:
            fprintf(fP, "not copied: the bank's budget went to cheaper sites, down to the reserve\n");
            break;

        default:
            if( optBankCeilingDeclared(tableP, inP->siteP->bank) )
            {
                // Name the author's ceiling.
                fprintf(fP, "not copied: the copy would pass the bank's declared ceiling, %%%%ceiling %04o\n",
                    optBankCeilingDeclared(tableP, inP->siteP->bank));
            }
            else
            {
                fprintf(fP, "not copied: the copy would pass the bank's ceiling\n");
            }
            break;
        }
    }
}

// Release the transform record and clear the table's; safe on a table the
// pass never ran on.
void
freeXformList(OptTableP tableP)
{
OptXformP recP;
OptXformP nextP;
OptInlineP inP;
OptInlineP nextInP;
OptPlaceP pP;
OptPlaceP nextPP;
OptUnrollP uP;
OptUnrollP nextUP;

    for( recP = tableP->xformsP; recP; recP = nextP )
    {
        nextP = recP->nextP;
        free(recP->beforeP);
        free(recP->afterP);
        free(recP->chainPP);
        free(recP);
    }

    tableP->xformsP = NILP;
    tableP->xformsTailP = NILP;

    // The inline trees are not freed: a standing copy holds them, and a
    // refused one was left unspliced for the reason undoEdit() gives.
    for( inP = tableP->inlinesP; inP; inP = nextInP )
    {
        nextInP = inP->nextP;
        free(inP->retargetsPP);
        free(inP->retargetTreesPP);
        free(inP->offsetsPP);
        free(inP);
    }

    tableP->inlinesP = NILP;
    tableP->inlinesTailP = NILP;

    // Placement and loop records own no tree.
    for( pP = tableP->placesP; pP; pP = nextPP )
    {
        nextPP = pP->nextP;
        free(pP);
    }

    tableP->placesP = NILP;
    tableP->placesTailP = NILP;

    for( uP = tableP->unrollsP; uP; uP = nextUP )
    {
        nextUP = uP->nextP;
        free(uP);
    }

    tableP->unrollsP = NILP;
    tableP->unrollsTailP = NILP;
}
