/* optimizer.c - the am1 optimizer's word table, option parsing and top-level sequence
 *
 * The word table holds one entry per word the program emits, in emission
 * order: bank, address, value, the node or symbol that produced it, its kind,
 * its labels and its source line.  Every analysis in the other opt*.c files is
 * built on it; optimize() runs them in order and writeReport() sets the
 * report's section order.  optimizer.h is the header they share.
 *
 * Called once from main() in am1.c when -O is given, after yyparse() and before
 * any code generator.  The table builder copies the walk of bincodegen.c and
 * testcodegen.c rather than calling into them (their generators are static and
 * write as they go), so the table holds the same words at the same addresses
 * as the tape; -O=dump prints it in -T format for wordtable_check.sh to diff.
 * Depends on am1.h and y.tab.h, evalExpr() from eval.c, banksP from parser.y
 * and the report FILE outfP that am1.c opens on <basename>.opt.  Single
 * threaded; an allocation failure is fatal, as everywhere in am1.
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

// The builder's running emission state.  It mirrors cur_bank/cur_pc in
// bincodegen.c so the address the binary generator WOULD use can be checked
// against the one the parser stamped on the node; they have disagreed before
// (rg33 in the regression suite pins a case).
typedef struct
{
    int bank;               // the bank the binary generator would be writing
    int pc;                 // and the address within it
    const char *fileP;      // the words' source file: from the last FILENAME passed
    bool failed;            // set when the walk met something it could not place
    bool inOverlay;         // between the view's %%overlay and its %%endoverlay
    int rootBank;           // where the root resumes at that %%endoverlay
    int rootPC;
} Builder, *BuilderP;

#define INITIAL_CAPACITY    256     // entries allocated for a new table, doubled as needed
#define OPT_OVNAMES         8       // root symbols of one drum overlay word that set its
                                    // pinned span; any past them are pinned alone

extern BankContextP banksP;         // every bank the program used, most recent first
extern FILE *outfP;                 // the report file, opened by am1.c
extern char *origFilenameP;         // the source file named on the command line

extern int evalExpr(PNodeP);
extern OverlayContextP overlaysP;   // every drum overlay, from the parser
extern SymNodeP ovTableSymP;        // the overlay descriptor table's label
extern void refreshOverlayAliases(void);

// Set by -O=dump: print the word table on stdout in -T format.
static bool optDumpWanted;

// Set by -O=decode: print the decoded dump on stdout.
static bool optDecodeDumpWanted;

// Set by -O=check: run the decoder's self-check first and print it on stdout.
static bool optCheckWanted;

// Set by -O=refs: print the reference dump on stdout.
static bool optRefsDumpWanted;

// Set by -O=flow: print the control-flow dump on stdout.
static bool optFlowDumpWanted;

// Set by -O=calls: print the routines and the call graph on stdout.
static bool optCallsDumpWanted;

// Set by -O=regions: print the optimize/endoptimize regions on stdout.
static bool optRegionsDumpWanted;

// Set by -O=rules: print the findings on stdout.
static bool optRulesDumpWanted;

// Set by -O=share: print the return-word sharing assignment on stdout.
static bool optShareDumpWanted;

// Set by -O=xform: print every live finding's fate on stdout.  Without a level
// it is a dry run: the fates and rewritten values are worked out, nothing is
// rewritten.
static bool optXformDumpWanted;

// Set by -O=values: print the AC/IO value measurement on stdout.
static bool optValuesDumpWanted;

// Set by -O=carry: print the carried value measurement (task A52) on stdout.
static bool optCarryDumpWanted;

// Set by -O=scratch: print the scratch-pool measurement on stdout.
static bool optScratchDumpWanted;

// Set by -O=guess: print the -O2 heuristics and what each would refuse; the
// report carries the same measurement either way.
static bool optGuessDumpWanted;

// Set by -O=speed: print the speed-mode candidates on stdout.
static bool optSpeedDumpWanted;

// Set by -O=window: print the extend-window analysis on stdout.
static bool optWindowDumpWanted;

// Set by -O=relayout: print the relayout dump.  Without -O=edits relayout runs
// with no edit, which must change nothing.
static bool optRelayoutDumpWanted;

// The optimization level, 1 or 2 from -O1 or -O2; 0 is the advisor alone.
static int transformLevel;

// The bisection modifiers.  An -O=off line keeps where it was read from, so a
// line that matches nothing can be named.
typedef struct
{
    int bank;
    int lo;
    int hi;
    char *ovNameP;          // the drum overlay its "@name" names, NILP for the root
    const char *specP;      // as given, for a warning
} BisectRange;

typedef struct
{
    int bank;
    int addr;
    char *ovNameP;          // the drum overlay its "@name" names, NILP for the root
    const char *fileP;      // the -O=off file, as given
    int lineNo;
    int matched;            // non-zero once a rewrite that would fire names it
} BisectListed;

#define BISECT_LINE     256         // the longest -O=off line read whole
#define BISECT_SPEC     512         // the longest spelling of the modifiers kept

static int bisectGiven;             // any bisection modifier was given
static long bisectUpto = -1;        // -O=upto=N, -1 when not given
static BisectRange *bisectRangesP;  // every -O=range, which add
static int bisectRangeCount;
static BisectListed *bisectListP;   // every line of every -O=off file
static int bisectListCount;
static char bisectSpec[BISECT_SPEC];    // the modifiers as given, for the report
static int bisectLastOrdinal;       // the last ordinal the views so far gave
static int bisectOffTotal;          // every view's rewrites switched off, and of those
static int bisectCandidateTotal;    // that would fire, for -O2's one warning
static int lastView;                // the view being analyzed is the last

// -O=inline=cap:N and -O=inline=reserve:N.
static int inlineCap = 8;           // the largest body copied at a declared site unless marked
static int inlineReserve = 64;      // the words each bank's budget leaves free

// -O=undeclared: under -O2, the length-changing rewrites an optimize region
// licenses (the deletions, the pool reclaim, the extend-window deletions and
// fall-through placement) fire where no region declares them, on the guess
// alone.  -O=place=undeclared, an alias, sets it too; placeSpelling
// says that one was given, so the messages name what the author typed.
static int undeclared;
static int placeSpelling;

// -O=unroll=cap:N, the most words a loop's body may become.
static int unrollCap = 64;

// -O=reclaim=off, testing build only: keeps the pool words a declared T8 frees,
// so a check whose oracle is one word for one word can still judge same-length
// rewrites.  The reclaim has no bisection number, hence a switch of its own.
static int reclaimOff;

// -O=source: am1.c writes the program back out as am1 source after optimize().
static bool sourceWanted;

// How -O=source writes it: SRC_MODE_TEXT copies each unchanged line from the
// source; the testing build's -O=source=render renders every line through the
// same line map, and -O=source=tree renders every line with no map at all.
static int sourceMode = SRC_MODE_TEXT;

// The builder's running state for the current optimize() call.
static Builder builder;

// The view optimize() is analyzing: NILP for the root alone, else the drum
// overlay whose words join the root's.  Relayout rebuilds tables of it too.
static OverlayContextP viewOvP;

static OptTableP newTable(void);
static OptWordP addEntry(OptTableP tableP, int bank, int addr, int value, OptKind kind,
    PNodeP nodeP, PNodeP exprP, SymNodeP symP, int lineNo, unsigned int flags);
static void indexEntry(OptTableP tableP, OptWordP entryP);
static void addLabelDef(OptTableP tableP, SymNodeP symP, int bank, int addr);
static void attachLabels(OptTableP tableP);
static void freeLabelList(OptLabelP labelP);

static void walkStatements(OptTableP tableP, PNodeP nodeP);
static void addExprWord(OptTableP tableP, PNodeP nodeP, PNodeP exprP);
static void addTextWords(OptTableP tableP, PNodeP nodeP, PNodeP textNodeP, OptKind kind);
static void addAsciiWords(OptTableP tableP, PNodeP nodeP, PNodeP asciiNodeP);
static void addTableWords(OptTableP tableP, PNodeP nodeP);
static void addVarWords(OptTableP tableP, PNodeListP listP, int bank);
static void addConstWords(OptTableP tableP, SymNodeP symP, int bank);
static void addOvCallWords(OptTableP tableP, PNodeP nodeP);
static void addOvTableWords(OptTableP tableP, PNodeP nodeP);
static void pinOverlayReferences(OptTableP tableP, PNodeP nodeP);
static void pinWordExpr(OptTableP tableP, OverlayContextP ovP, PNodeP exprP);
static void rootNames(OptTableP tableP, OverlayContextP ovP, PNodeP exprP, SymNodeP *namesPP, int *countP);
static void pinRange(OptTableP tableP, int bank, int first, int last);

static int canReduce(PNodeP nodeP);
static int reduceOperand(PNodeP nodeP);
static void advancePC(int incr);
static void checkPlacement(OptTableP tableP, OptWordP entryP);

static int optimizeView(PNodeP rootP, char *basenameP);
static OverlayContextP overlayNumbered(int number);
static void enterOverlayView(PNodeP nodeP);
static void leaveOverlayView(OptTableP tableP);
static void writeReport(FILE *fP, OptTableP tableP, char *basenameP);
static void writeOverlayReport(FILE *fP, OptTableP tableP);
#ifdef AM1_TEST_SWITCHES
static void secondPass(PNodeP rootP);
#endif

static int bisectSetUpto(const char *argP);
static int bisectAddRange(const char *argP);
static int bisectReadList(const char *pathP);
static int bisectNumber(const char *textP, int base, long max, long *valueP, const char **endPP);
static char *bisectOverlayName(const char *textP, const char **endPP);
static int bisectSameOverlay(const char *ovNameP, int ovWord);
static int bisectIsOverlay(const char *nameP);
static void bisectNoteSpec(const char *argP);
static int inlineSetting(const char *argP);
static int unrollSetting(const char *argP);

// Accept one -O=modifier: a dump or check switch, or a setting that takes a
// value.  The test-only modifiers exist only when AM1_TEST_SWITCHES is defined.
// Returns 1 if recognized and applied, 0 if not (the command line is invalid).
int
optimizeSetOption(char *nameP)
{
    if( !strcmp(nameP, "xform") )
    {
        optXformDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "source") )
    {
        sourceWanted = true;
        return(1);
    }

    // The test-only modifiers (make am1test).
#ifdef AM1_TEST_SWITCHES
    if( !strcmp(nameP, "source=render") )
    {
        sourceWanted = true;
        sourceMode = SRC_MODE_RENDER;
        return(1);
    }

    if( !strcmp(nameP, "source=tree") )
    {
        sourceWanted = true;
        sourceMode = SRC_MODE_TREE;
        return(1);
    }

    if( !strcmp(nameP, "dump") )
    {
        optDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "decode") )
    {
        optDecodeDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "refs") )
    {
        optRefsDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "flow") )
    {
        optFlowDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "calls") )
    {
        optCallsDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "share") )
    {
        optShareDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "regions") )
    {
        optRegionsDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "rules") )
    {
        optRulesDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "values") )
    {
        optValuesDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "carry") )
    {
        optCarryDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "scratch") )
    {
        optScratchDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "guess") )
    {
        optGuessDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "speed") )
    {
        optSpeedDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "relayout") )
    {
        optRelayoutDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "window") )
    {
        optWindowDumpWanted = true;
        return(1);
    }

    if( !strcmp(nameP, "check") )
    {
        optCheckWanted = true;
        return(1);
    }
#endif

    // Bisection, the modifiers that take a value.  strncmp, so that a scan of
    // this function for the plain modifiers by their strcmp finds only those.
    if( !strncmp(nameP, "upto=", 5) )
    {
        return( bisectSetUpto(nameP + 5) );
    }

    if( !strncmp(nameP, "range=", 6) )
    {
        return( bisectAddRange(nameP + 6) );
    }

    if( !strncmp(nameP, "off=", 4) )
    {
        return( bisectReadList(nameP + 4) );
    }

    // The relayout test instrument, testing build only.
#ifdef AM1_TEST_SWITCHES
    if( !strncmp(nameP, "edits=", 6) )
    {
        return( optRelayoutReadEdits(nameP + 6) );
    }
#endif

    // The inline cap and reserve.
    if( !strncmp(nameP, "inline=", 7) )
    {
        return( inlineSetting(nameP + 7) );
    }

    // -O=undeclared and its alias; am1.c refuses both without -O2.  strncmp
    // through the terminator, an exact match, so that a scan for the plain
    // modifiers by their strcmp does not count it among the dumps.
    if( !strncmp(nameP, "undeclared", sizeof("undeclared")) )
    {
        undeclared = 1;
        return(1);
    }

    if( !strncmp(nameP, "place=", 6) )
    {
        if( strcmp(nameP + 6, "undeclared") )
        {
            fprintf(stderr, "am1: -O=place=%s: expected -O=place=undeclared, an alias of -O=undeclared\n", nameP + 6);
            return(0);
        }

        undeclared = 1;
        placeSpelling = 1;
        return(1);
    }

    // S2's one setting, "cap:N".
    if( !strncmp(nameP, "unroll=", 7) )
    {
        return( unrollSetting(nameP + 7) );
    }

    // The pool reclaim's one setting, testing build only.
#ifdef AM1_TEST_SWITCHES
    if( !strncmp(nameP, "reclaim=", 8) )
    {
        if( strcmp(nameP + 8, "off") )
        {
            fprintf(stderr, "am1: -O=reclaim=%s: expected -O=reclaim=off\n", nameP + 8);
            return(0);
        }

        reclaimOff = 1;
        return(1);
    }
#endif

    return(0);
}

// Accept -O=unroll=cap:N, N a decimal count from 0 to 4096; the last one given
// holds.  Returns 1 if accepted, 0 if not (the caller prints the usage).
static int
unrollSetting(const char *argP)
{
long value;
const char *endP;

    if( strncmp(argP, "cap:", 4) )
    {
        fprintf(stderr, "am1: -O=unroll=%s: expected cap:N, N a decimal count of words\n", argP);
        return(0);
    }

    argP += 4;

    if( !bisectNumber(argP, 10, BANKSIZE, &value, &endP) || *endP )
    {
        fprintf(stderr, "am1: -O=unroll=cap:%s: expected a decimal count of words, 0 to %d\n", argP, BANKSIZE);
        return(0);
    }

    unrollCap = (int)value;
    return(1);
}

// The most words a loop's body may become when unrolled.
int
optUnrollCap(void)
{
    return(unrollCap);
}

// Returns 1 if -O=undeclared or -O=place=undeclared was given, 0 if not.
int
optUndeclaredGiven(void)
{
    return(undeclared);
}

// The switch as the author spelled it, for a message.  Returns a static string.
const char *
optUndeclaredSpelling(void)
{
    return( (placeSpelling)?"-O=place=undeclared":"-O=undeclared" );
}

// Returns 1 when the switch licenses the length-changing rewrites outside every
// region: given, and at -O2.  0 if not.
int
optUndeclared(void)
{
    return( undeclared && (transformLevel >= 2) );
}

// Returns 1 if -O=reclaim=off was given, 0 if not; always 0 in the production
// build, where the reclaim always happens.
int
optReclaimOff(void)
{
    return(reclaimOff);
}

// Returns 1 if -O=source was given, 0 if not.
int
optSourceWanted(void)
{
    return( sourceWanted?1:0 );
}

// Returns how -O=source writes the program: SRC_MODE_TEXT unless the testing
// build was given -O=source=render or -O=source=tree.
int
optSourceMode(void)
{
    return(sourceMode);
}

// Accept -O=inline=cap:N (the largest body copied without a marking, 8 unless
// given) or -O=inline=reserve:N (words each bank's budget leaves free, 64), N
// from 0 to 4096.  Returns 1 if accepted, 0 if not (the caller prints the usage).
static int
inlineSetting(const char *argP)
{
long value;
const char *endP;
int *settingP;

    if( !strncmp(argP, "cap:", 4) )
    {
        settingP = &inlineCap;
        argP += 4;
    }
    else if( !strncmp(argP, "reserve:", 8) )
    {
        settingP = &inlineReserve;
        argP += 8;
    }
    else
    {
        fprintf(stderr, "am1: -O=inline=%s: expected cap:N or reserve:N, N a decimal count of words\n", argP);
        return(0);
    }

    if( !bisectNumber(argP, 10, BANKSIZE, &value, &endP) || *endP )
    {
        fprintf(stderr, "am1: -O=inline=...:%s: expected a decimal count of words, 0 to %d\n", argP, BANKSIZE);
        return(0);
    }

    *settingP = (int)value;
    return(1);
}

// The largest body a declared site may copy unless its callee is marked or has
// one site.
int
optInlineCap(void)
{
    return(inlineCap);
}

// The words each bank's inline budget leaves free.
int
optInlineReserve(void)
{
    return(inlineReserve);
}

// Bisection

// Accept -O=upto=N: keep the first N rewrites that would fire (a decimal count;
// 0 keeps none).  Given twice it is refused, since two counts cannot both hold.
// Returns 1 if accepted, 0 if not (the caller prints the usage).
static int
bisectSetUpto(const char *argP)
{
long value;
const char *endP;

    if( bisectUpto >= 0 )
    {
        fprintf(stderr, "am1: -O=upto may be given only once\n");
        return(0);
    }

    if( !bisectNumber(argP, 10, 0x7fffffffL, &value, &endP) || *endP )
    {
        fprintf(stderr, "am1: -O=upto=%s: expected a decimal count of rewrites\n", argP);
        return(0);
    }

    bisectUpto = value;
    bisectGiven = 1;
    bisectNoteSpec(argP - 5);
    return(1);
}

// Accept -O=range=B:LO-HI: keep the rewrites in bank B (decimal) at LO to HI
// (octal); with "@name" after HI, those of drum overlay name, without it the
// root's.  Each -O=range adds a range.
// Returns 1 if accepted, 0 if not (the caller prints the usage).
static int
bisectAddRange(const char *argP)
{
BisectRange range;
long value;
const char *cP;

    cP = argP;

    if( !bisectNumber(cP, 10, MAXBANK, &value, &cP) || (*cP++ != ':') )
    {
        fprintf(stderr, "am1: -O=range=%s: expected B:LO-HI, a bank 0 to %d and octal addresses\n", argP, MAXBANK);
        return(0);
    }

    range.bank = (int)value;

    if( !bisectNumber(cP, 8, ADDRMASK, &value, &cP) || (*cP++ != '-') )
    {
        fprintf(stderr, "am1: -O=range=%s: expected B:LO-HI, a bank 0 to %d and octal addresses\n", argP, MAXBANK);
        return(0);
    }

    range.lo = (int)value;

    if( !bisectNumber(cP, 8, ADDRMASK, &value, &cP) || (*cP && (*cP != '@')) || (value < range.lo) )
    {
        fprintf(stderr, "am1: -O=range=%s: expected B:LO-HI, a bank 0 to %d and octal addresses, LO not above HI\n",
            argP, MAXBANK);
        return(0);
    }

    range.hi = (int)value;
    range.ovNameP = NILP;
    range.specP = argP;

    if( (*cP == '@') && (!(range.ovNameP = bisectOverlayName(cP + 1, &cP)) || *cP) )
    {
        fprintf(stderr, "am1: -O=range=%s: expected B:LO-HI@name, name a drum overlay's\n", argP);
        return(0);
    }

    if( !(bisectRangesP = (BisectRange *)realloc(bisectRangesP, ((size_t)(bisectRangeCount + 1) * sizeof(BisectRange)))) )
    {
        fprintf(stderr, "am1: out of memory reading -O=range\n");
        exit(1);
    }

    bisectRangesP[bisectRangeCount++] = range;
    bisectGiven = 1;
    bisectNoteSpec(argP - 6);
    return(1);
}

// Accept -O=off=FILE: switch off each rewrite listed as "bank addr", or
// "bank addr@name" for a drum overlay's word, as in an -O=xform line.  Returns 1
// if accepted, 0 for an empty name; an unreadable file or malformed line stops
// am1, since a wrong list is worse than no build.
static int
bisectReadList(const char *pathP)
{
FILE *fP;
char line[BISECT_LINE];
char *cP;
const char *endP;
long bank;
long addr;
int lineNo;
char *ovNameP;
BisectListed *entryP;

    if( !*pathP )
    {
        fprintf(stderr, "am1: -O=off= needs a file name\n");
        return(0);
    }

    if( !(fP = fopen(pathP, "r")) )
    {
        fprintf(stderr, "am1: -O=off=%s: cannot open the file\n", pathP);
        exit(1);
    }

    lineNo = 0;

    while( fgets(line, sizeof(line), fP) )
    {
        ++lineNo;

        if( !strchr(line, '\n') && !feof(fP) )
        {
            fprintf(stderr, "am1: -O=off=%s, line %d: the line is longer than %d characters\n",
                pathP, lineNo, (BISECT_LINE - 2));
            exit(1);
        }

        if( (cP = strchr(line, '#')) )
        {
            *cP = '\0';
        }

        for( cP = line; (*cP == ' ') || (*cP == '\t'); ++cP )
        {
        }

        if( (*cP == '\0') || (*cP == '\n') || (*cP == '\r') )
        {
            continue;
        }

        if( !bisectNumber(cP, 10, MAXBANK, &bank, &endP) || ((*endP != ' ') && (*endP != '\t')) )
        {
            fprintf(stderr, "am1: -O=off=%s, line %d: expected \"bank addr\", a bank 0 to %d and an octal address\n",
                pathP, lineNo, MAXBANK);
            exit(1);
        }

        for( ; (*endP == ' ') || (*endP == '\t'); ++endP )
        {
        }

        if( !bisectNumber(endP, 8, ADDRMASK, &addr, &endP) )
        {
            fprintf(stderr, "am1: -O=off=%s, line %d: expected \"bank addr\", a bank 0 to %d and an octal address\n",
                pathP, lineNo, MAXBANK);
            exit(1);
        }

        ovNameP = NILP;

        if( (*endP == '@') && !(ovNameP = bisectOverlayName(endP + 1, &endP)) )
        {
            fprintf(stderr, "am1: -O=off=%s, line %d: expected \"bank addr@name\", name a drum overlay's\n",
                pathP, lineNo);
            exit(1);
        }

        for( ; (*endP == ' ') || (*endP == '\t') || (*endP == '\r') || (*endP == '\n'); ++endP )
        {
        }

        if( *endP )
        {
            fprintf(stderr, "am1: -O=off=%s, line %d: expected \"bank addr\" and nothing more but a comment\n",
                pathP, lineNo);
            exit(1);
        }

        if( !(bisectListP = (BisectListed *)realloc(bisectListP, ((size_t)(bisectListCount + 1) * sizeof(BisectListed)))) )
        {
            fprintf(stderr, "am1: out of memory reading -O=off\n");
            exit(1);
        }

        entryP = &bisectListP[bisectListCount++];
        entryP->bank = (int)bank;
        entryP->addr = (int)addr;
        entryP->ovNameP = ovNameP;
        entryP->fileP = pathP;
        entryP->lineNo = lineNo;
        entryP->matched = 0;
    }

    fclose(fP);
    bisectGiven = 1;
    bisectNoteSpec(pathP - 4);
    return(1);
}

// Read one unsigned number in base: at least one digit, no sign, at most max.
// Returns 1 with *valueP set and *endPP after the digits, 0 if there is no
// number there or it is too large.
static int
bisectNumber(const char *textP, int base, long max, long *valueP, const char **endPP)
{
const char *cP;
long value;
int digit;

    value = 0;

    for( cP = textP; (*cP >= '0') && (*cP <= '9'); ++cP )
    {
        digit = (*cP - '0');

        if( digit >= base )
        {
            return(0);
        }

        value = ((value * base) + digit);

        if( value > max )
        {
            return(0);
        }
    }

    if( cP == textP )
    {
        return(0);
    }

    *valueP = value;
    *endPP = cP;
    return(1);
}

// Read the name after a qualifier's '@': every character up to a space, a tab,
// a line end or the string's end.  Whether a drum overlay has the name is known
// only after the parse, so optBisectWarnUnmatched() checks it.
// Returns the name, allocated, with *endPP after it; NILP for an empty name.
static char *
bisectOverlayName(const char *textP, const char **endPP)
{
const char *cP;
char *nameP;

    for( cP = textP; *cP && (*cP != ' ') && (*cP != '\t') && (*cP != '\r') && (*cP != '\n') && (*cP != '@'); ++cP )
    {
    }

    if( cP == textP )
    {
        return(NILP);
    }

    if( !(nameP = (char *)malloc((size_t)(cP - textP) + 1)) )
    {
        fprintf(stderr, "am1: out of memory reading a bisection modifier\n");
        exit(1);
    }

    memcpy(nameP, textP, (size_t)(cP - textP));
    nameP[cP - textP] = '\0';
    *endPP = cP;
    return(nameP);
}

// Does a modifier qualified by ovNameP (NILP for none) name a word of the view's
// drum overlay (ovWord non-zero) or a root word (zero)?  An unqualified one
// names root words only, since addresses repeat across overlays.
// Returns 1 if it does, 0 if not.
static int
bisectSameOverlay(const char *ovNameP, int ovWord)
{
    if( !ovWord )
    {
        return( (!ovNameP)?1:0 );
    }

    return( (ovNameP && viewOvP && !strcmp(ovNameP, viewOvP->nameP))?1:0 );
}

// Returns 1 when a drum overlay is named nameP, 0 if none is.
static int
bisectIsOverlay(const char *nameP)
{
OverlayContextP ovP;

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( !strcmp(ovP->nameP, nameP) )
        {
            return(1);
        }
    }

    return(0);
}

// Append "-O=" and argP to the modifiers' spelling for the report and the -O2
// warning; a spelling too long for the buffer is cut off with "...".
static void
bisectNoteSpec(const char *argP)
{
size_t used;

    used = strlen(bisectSpec);

    if( (used + strlen(argP) + 5) >= sizeof(bisectSpec) )
    {
        if( (used + 5) < sizeof(bisectSpec) )
        {
            strcat(bisectSpec, (used)?" ...":"...");
        }

        return;
    }

    snprintf((bisectSpec + used), (sizeof(bisectSpec) - used), "%s-O=%s", (used)?" ":"", argP);
}

// Returns 1 when any bisection modifier was given, 0 if not.
int
optBisectGiven(void)
{
    return(bisectGiven);
}

// The first ordinal a view's selection gives: one past the last that the views
// before it gave, so that -O=upto counts the root's rewrites first and then
// each drum overlay's.
int
optBisectFirstOrdinal(void)
{
    return(bisectLastOrdinal + 1);
}

// Record the last ordinal a view's selection gave; the next view numbers on.
void
optBisectSetLastOrdinal(int ordinal)
{
    bisectLastOrdinal = ordinal;
}

// The OPTBIS_ bits of the modifiers that switch off the ordinal-th rewrite that
// would fire, at bank and addr, a word of the view's drum overlay when ovWord is
// non-zero; 0 keeps it.  Every -O=off line naming the word is marked matched,
// whatever the other modifiers decide.
unsigned int
optBisectSelect(int ordinal, int bank, int addr, int ovWord)
{
unsigned int offBy;
int i;
int inRange;

    offBy = 0;

    if( (bisectUpto >= 0) && (ordinal > bisectUpto) )
    {
        offBy |= OPTBIS_UPTO;
    }

    if( bisectRangeCount )
    {
        inRange = 0;

        for( i = 0; i < bisectRangeCount; ++i )
        {
            if( (bisectRangesP[i].bank == bank) && (addr >= bisectRangesP[i].lo) && (addr <= bisectRangesP[i].hi) &&
                bisectSameOverlay(bisectRangesP[i].ovNameP, ovWord) )
            {
                inRange = 1;
                break;
            }
        }

        if( !inRange )
        {
            offBy |= OPTBIS_RANGE;
        }
    }

    for( i = 0; i < bisectListCount; ++i )
    {
        if( (bisectListP[i].bank == bank) && (bisectListP[i].addr == addr) &&
            bisectSameOverlay(bisectListP[i].ovNameP, ovWord) )
        {
            bisectListP[i].matched = 1;
            offBy |= OPTBIS_OFF;
        }
    }

    return(offBy);
}

// Warn on stderr, whatever -W says, about each -O=off line that named no
// rewrite: a list goes stale when an edit moves an address, which must be
// visible but must not stop the build.  A qualifier naming no drum overlay is
// warned of the same way, a range's too.  Once, after the last view.
void
optBisectWarnUnmatched(void)
{
int i;

    for( i = 0; i < bisectRangeCount; ++i )
    {
        if( bisectRangesP[i].ovNameP && !bisectIsOverlay(bisectRangesP[i].ovNameP) )
        {
            fprintf(stderr, "am1: warning: -O=range=%s: there is no overlay %s\n", bisectRangesP[i].specP,
                bisectRangesP[i].ovNameP);
        }
    }

    for( i = 0; i < bisectListCount; ++i )
    {
        if( bisectListP[i].ovNameP && !bisectIsOverlay(bisectListP[i].ovNameP) )
        {
            fprintf(stderr, "am1: warning: -O=off=%s, line %d: there is no overlay %s\n",
                bisectListP[i].fileP, bisectListP[i].lineNo, bisectListP[i].ovNameP);
        }
        else if( !bisectListP[i].matched )
        {
            fprintf(stderr, "am1: warning: -O=off=%s, line %d: bank %d %04o%s%s names no rewrite that would fire; is the list stale?\n",
                bisectListP[i].fileP, bisectListP[i].lineNo, bisectListP[i].bank, bisectListP[i].addr,
                (bisectListP[i].ovNameP)?"@":"", (bisectListP[i].ovNameP)?bisectListP[i].ovNameP:"");
        }
    }
}

// Returns 1 while the last view is analyzed, 0 before it.
int
optLastView(void)
{
    return(lastView);
}

// The tag a word of the view's drum overlay carries after its "bank addr" in
// -O=xform and the report, which an -O=off line or an -O=range copies; "" for
// a root word, so that a program without overlays prints as it always has.
const char *
optOverlayTag(OptWordP entryP)
{
static char *tagP;
static OverlayContextP taggedP;

    if( !viewOvP || !entryP || !(entryP->flags & OPTF_OVWORD) )
    {
        return("");
    }

    if( taggedP != viewOvP )
    {
        free(tagP);

        if( !(tagP = (char *)malloc(strlen(viewOvP->nameP) + 2)) )
        {
            fprintf(stderr, "am1: out of memory naming an overlay's word\n");
            exit(1);
        }

        sprintf(tagP, "@%s", viewOvP->nameP);
        taggedP = viewOvP;
    }

    return(tagP);
}

// The bisection modifiers as given; empty when none was.
const char *
optBisectSpec(void)
{
    return(bisectSpec);
}

// The name of one OPTBIS_ bit, "?" for anything else.
const char *
optBisectName(unsigned int bit)
{
    switch( bit )
    {
    case OPTBIS_UPTO:
        return("upto");

    case OPTBIS_RANGE:
        return("range");

    case OPTBIS_OFF:
        return("off");

    default:
        break;
    }

    return("?");
}

// Accept -O1 or -O2; am1.c has already refused every other digit.
void
optimizeSetLevel(int level)
{
    transformLevel = level;
}

// The optimization level: 1 under -O1, 2 under -O2, 0 otherwise.
int
optTransformLevel(void)
{
    return(transformLevel);
}

// Run the optimizer: build and analyze the word table, print the dumps asked
// for, write the report to outfP, and relay the program out when needed.
// Returns 1 on success, 0 if the self-check failed or the table was not built.
int
optimize(PNodeP rootP, char *basenameP)
{
OverlayContextP ovP;
int number;

    // Every -O2 assembly first warns that it rewrites on a guess: on stderr and
    // regardless of -W, which silences only warnings about the source.
    if( transformLevel >= 2 )
    {
        fprintf(stderr, "am1: warning: -O2 guesses where code is shared with a device, a sequence-break\n");
        fprintf(stderr, "am1: warning: handler or a timing requirement, and the guess can be wrong.  Test\n");
        fprintf(stderr, "am1: warning: the program.  %s.opt names the guess behind every rewrite, and\n", basenameP);
        fprintf(stderr, "am1: warning: %%%%nooptimize/%%%%endnooptimize keeps -O2 out of code it must not touch.\n");

        // The switch deletes and moves code no region allows, and every later
        // address with it.  H6 sees an address changed in the block that loads
        // it, and a stored pointer an idx or isp walks; the warning names the
        // two cases it does not see.
        if( undeclared )
        {
            fprintf(stderr, "am1: warning: %s deletes and moves code outside every region, on the guess.\n",
                optUndeclaredSpelling());
            fprintf(stderr, "am1: warning: It does not see arithmetic on an address outside the block that loads it,\n");
            fprintf(stderr, "am1: warning: nor an entry past the first label of the run an address indexes.\n");
        }
    }

    if( optCheckWanted )
    {
        if( !optDecoderSelfCheck(stdout) )
        {
            fflush(stdout);
            return(0);
        }

        fflush(stdout);
    }

    // The root alone first, which is the whole program when it has no drum
    // overlay; then each overlay in source order, the root its context.  Two
    // overlays of a window never share a view, so no analysis meets two words
    // at one address that -M did not put there.
    // An -O=edits line's "@name" can be checked only now the parse has made the
    // overlays.
    optRelayoutCheckEdits();

    viewOvP = NILP;
    lastView = (overlayNumbered(1))?0:1;

    if( !optimizeView(rootP, basenameP) )
    {
        return(0);
    }

    for( number = 1; (ovP = overlayNumbered(number)); ++number )
    {
        viewOvP = ovP;
        lastView = (overlayNumbered(number + 1))?0:1;

        if( !optimizeView(rootP, basenameP) )
        {
            viewOvP = NILP;
            return(0);
        }
    }

    viewOvP = NILP;
    return(1);
}

// Analyze, rewrite and report one view, viewOvP's: the root alone, or a drum
// overlay with the root as its context.  In an overlay's view only the
// overlay's words are rewritten and relaid, and its report is a section of its
// own.
// Returns 1 on success, 0 on failure.
static int
optimizeView(PNodeP rootP, char *basenameP)
{
OptTableP tableP;

    if( !(tableP = optBuildTable(rootP)) )
    {
        return(0);
    }

    optDecodeTable(tableP);
    optBuildReferences(tableP);
    optBuildFlow(tableP);

    // After the flow, since a routine is a set of blocks; before the rules.
    optBuildCallGraph(tableP);

    // Advisory: it reads the closure and makes no finding, and runs before the
    // rules so nothing it does can reach the finding list.
    optBuildSharing(tableP);

    optRunRules(tableP);

    // After the rules, whose findings it classifies by region; no rule sees
    // anything it does.
    optCheckRegions(tableP);

    // After the rules and regions, whose findings it marks, and before the
    // transform, so the guess describes the program as written.
    optBuildGuess(tableP);

    // Advisory, after the guess, whose H2 and H3 marks refuse a deletion; it
    // warns about proved hazards on every -O run.
    optBuildWindow(tableP);

    // -O=source ties each statement to the source line it came from while the
    // statement list still stands as parsed, before any rewrite moves it: in
    // the root's view, the first.
    if( sourceWanted && !viewOvP )
    {
        srcMapLines(rootP);
    }

    // The one place the program changes: last, so every analysis above saw the
    // tree as written, and before the report, which says what was rewritten.
    // Without a level this is -O=xform's dry run, which touches nothing.
    if( (transformLevel >= 1) || optXformDumpWanted )
    {
        // -O=source must know which reference created each pool slot, which
        // a rewrite can take away: before the root view's first rewrite,
        // the overlays' pools included.
        if( sourceWanted && (transformLevel >= 1) && !viewOvP )
        {
            srcSnapshotPools();
        }

        optTransform(tableP, (transformLevel >= 1));
    }

    // Tell the developer when bisection switched rewrites off, since this is
    // then not the full -O2 build: once, in the last view, for every view.
    if( (transformLevel >= 2) && tableP->xformBisect )
    {
        bisectOffTotal += tableP->xformFates[OPTXF_OFF] + tableP->inlineFates[OPTIN_OFF] +
            tableP->placeFates[OPTPL_OFF] + tableP->unrollFates[OPTUN_OFF] + tableP->windelFates[OPTWD_OFF];
        bisectCandidateTotal += tableP->xformCandidates + tableP->inlineCandidates + tableP->placeCandidates +
            tableP->unrollCandidates + tableP->spaceCandidates + tableP->windelCandidates;

        if( lastView )
        {
            fprintf(stderr, "am1: warning: bisection: %d of the %d rewrites -O2 would make are switched off (%s).\n",
                bisectOffTotal, bisectCandidateTotal, optBisectSpec());
        }
    }

    // Each overlay's view prints its dumps whole, root words included, under a
    // heading of its own; the root's view prints as it always has.
    if( viewOvP && (optDumpWanted || optDecodeDumpWanted || optRefsDumpWanted || optFlowDumpWanted ||
        optCallsDumpWanted || optShareDumpWanted || optRegionsDumpWanted || optRulesDumpWanted ||
        optValuesDumpWanted || optCarryDumpWanted || optScratchDumpWanted || optGuessDumpWanted || optSpeedDumpWanted ||
        optWindowDumpWanted || optXformDumpWanted || optRelayoutDumpWanted) )
    {
        fprintf(stdout, "\n");
        writeOverlayHeading(stdout, tableP);
    }

    if( optDumpWanted )
    {
        optDumpTable(stdout, tableP);
        fflush(stdout);
    }

    if( optDecodeDumpWanted )
    {
        optDumpDecoded(stdout, tableP);
        fflush(stdout);
    }

    if( optRefsDumpWanted )
    {
        optDumpReferences(stdout, tableP);
        fflush(stdout);
    }

    if( optFlowDumpWanted )
    {
        optDumpFlow(stdout, tableP);
        fflush(stdout);
    }

    if( optCallsDumpWanted )
    {
        optDumpCalls(stdout, tableP);
        fflush(stdout);
    }

    if( optShareDumpWanted )
    {
        optDumpShare(stdout, tableP);
        fflush(stdout);
    }

    if( optRegionsDumpWanted )
    {
        optDumpRegions(stdout, tableP);
        optDumpHandsOff(stdout, tableP);        // nothing without a span
        optDumpSpeedDecls(stdout, tableP);      // nothing without a declaration
        optDumpCeilings(stdout, tableP);        // nothing without a %%ceiling
        fflush(stdout);
    }

    if( optRulesDumpWanted )
    {
        optDumpRules(stdout, tableP);
        fflush(stdout);
    }

    // The measurement dumps read the finished table, which holds the program as
    // written even under -O1.
    if( optValuesDumpWanted )
    {
        optDumpValues(stdout, tableP);
        fflush(stdout);
    }

    if( optCarryDumpWanted )
    {
        optDumpCarry(stdout, tableP);
        fflush(stdout);
    }

    if( optScratchDumpWanted )
    {
        optDumpScratch(stdout, tableP);
        fflush(stdout);
    }

    if( optGuessDumpWanted )
    {
        optDumpGuess(stdout, tableP);
        fflush(stdout);
    }

    if( optSpeedDumpWanted )
    {
        optDumpSpeed(stdout, tableP);
        fflush(stdout);
    }

    if( optWindowDumpWanted )
    {
        optDumpWindow(stdout, tableP);
        fflush(stdout);
    }

    if( optXformDumpWanted )
    {
        optDumpTransforms(stdout, tableP, "xform", 0);
        fflush(stdout);
    }

    if( viewOvP )
    {
        writeOverlayReport(outfP, tableP);
    }
    else
    {
        writeReport(outfP, tableP, basenameP);
    }

    // After the report, which describes the program as assembled, and before
    // the table is freed, since relayout checks every edit against it.  It runs
    // for the edits file or dump, any fired inline, placement, unroll or
    // deletion (an eem or lem included), or a declared T8's freed pool word: a
    // length change no edit announces.  An -O=edits line is tried in the view
    // of the words it names: the root's, or the overlay its "@name" names; an
    // overlay's view relays only that overlay, in its window.
    if( (optRelayoutGiven(viewOvP) || optRelayoutDumpWanted || tableP->inlineFates[OPTIN_FIRED] ||
        tableP->placeFates[OPTPL_FIRED] || tableP->unrollFates[OPTUN_FIRED] || tableP->spaceWords ||
        tableP->windelFates[OPTWD_FIRED] || (tableP->poolFreeable && !optReclaimOff())) )
    {
        optRelayout(tableP, rootP, (optRelayoutDumpWanted)?stdout:NILP);
        fflush(stdout);
    }

    // The sections that say what relayout made of each rewrite, so after it;
    // each prints nothing when nothing was declared.  An overlay's go in its
    // own section of the report.
    writeInlineReport(outfP, tableP);
    writePlaceReport(outfP, tableP);
    writeUnrollReport(outfP, tableP);
    writeSpaceReport(outfP, tableP);
    writeWindowDeleteReport(outfP, tableP);
    writeReclaimReport(outfP, tableP);

    // -O=source annotates each rewrite from the table, which still holds the
    // program as assembled; after relayout, whose edits it describes too.
    if( sourceWanted )
    {
        srcCollect(tableP);
    }

    optFreeTable(tableP);

    // The idempotence check, testing build only, after the report so a second
    // build of the table cannot disturb anything the report reads.
#ifdef AM1_TEST_SWITCHES
    if( optXformDumpWanted && (transformLevel >= 1) )
    {
        secondPass(rootP);
        fflush(stdout);
    }
#endif

    return(1);
}

// The drum overlay numbered number, from 1 in source order.
// Returns it, or NILP past the last.
static OverlayContextP
overlayNumbered(int number)
{
OverlayContextP ovP;

    for( ovP = overlaysP; ovP && (ovP->number != number); ovP = ovP->nextP )
    {
        ;
    }

    return(ovP);
}

// Analyze the rewritten program again and decide the fates without editing; it
// must fire nothing but the rewrites bisection switched off, which are still as
// the source wrote them.  Prints lines headed "second" and writes no report.
#ifdef AM1_TEST_SWITCHES
static void
secondPass(PNodeP rootP)
{
OptTableP tableP;

    if( !(tableP = optBuildTable(rootP)) )
    {
        fprintf(stdout, "second: the word table of the rewritten program could not be built\n");
        return;
    }

    optDecodeTable(tableP);
    optBuildReferences(tableP);
    optBuildFlow(tableP);
    optBuildCallGraph(tableP);
    optBuildSharing(tableP);
    optRunRules(tableP);
    optCheckRegions(tableP);
    optBuildGuess(tableP);      // -O2's fates read it; -O1's do not
    optTransform(tableP, 0);
    optDumpTransforms(stdout, tableP, "second", 1);
    optFreeTable(tableP);
}
#endif

// Build the word table by the code generators' walk: the statement list off
// rootP->leftP, then each bank's constants and variables not placed explicitly.
// Returns the table, or NILP if something could not be placed (said on stderr).
OptTableP
optBuildTable(PNodeP rootP)
{
OptTableP tableP;
BankContextP bankP;

    tableP = newTable();
    tableP->viewOvP = viewOvP;

    // The binary generator starts where macro1 did, bank 0 address 4.
    builder.bank = 0;
    builder.pc = 4;
    builder.failed = false;
    builder.inOverlay = false;

    // The command-line source until a FILENAME statement says otherwise.  cpp's
    // first line marker never becomes a statement, so without includes this is
    // the only name the words get.
    builder.fileP = origFilenameP;

    // No region is open before the walk, and the parser refuses one left open,
    // so nothing has to be closed after it.
    optRegionReset();

    if( rootP->rightP && (rootP->rightP->type == START) )
    {
        tableP->hasStart = 1;
        tableP->startAddr = rootP->rightP->value.ival;
    }

    walkStatements(tableP, rootP->leftP);

    // Now the constants and variables that had no explicit 'constants' or
    // 'variables' directive.  The parser placed them at the end of their own
    // bank, constants first, then variables, and recorded where each block
    // starts; every generator emits them in that order.
    for( bankP = banksP; bankP && !builder.failed; bankP = bankP->nextP )
    {
        if( !bankP->constSymP && !bankP->varNodesP )
        {
            continue;
        }

        builder.bank = bankP->bank;
        builder.pc = (bankP->constSymP)?bankP->constPC:bankP->varPC;

        if( bankP->constSymP )
        {
            addConstWords(tableP, bankP->constSymP, bankP->bank);
        }

        if( bankP->varNodesP )
        {
            addVarWords(tableP, bankP->varNodesP, bankP->bank);
        }
    }

    if( builder.failed )
    {
        optFreeTable(tableP);
        return(NILP);
    }

    pinOverlayReferences(tableP, rootP->leftP);
    attachLabels(tableP);
    return(tableP);
}

// Print the table in -T format: bank and address as one six-digit octal number,
// then the value.  Reserved table words print nothing, as in -T.
void
optDumpTable(FILE *fP, OptTableP tableP)
{
int i;
OptWordP entryP;

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];

        if( entryP->flags & OPTF_RESERVED )
        {
            continue;
        }

        fprintf(fP, "%02o%04o %06o\n", entryP->bank, entryP->addr, entryP->value);
    }
}

// Release a table and everything it owns; the parse tree and symbol tables,
// which it only points into, are untouched.  Safe with NILP.
void
optFreeTable(OptTableP tableP)
{
int i;
OptLabelDefP defP;
OptLabelDefP nextDefP;

    if( !tableP )
    {
        return;
    }

    for( i = 0; i < tableP->count; ++i )
    {
        freeLabelList(tableP->entriesPP[i]->labelsP);
        freeEdgeList(tableP->entriesPP[i]->outP);
        free(tableP->entriesPP[i]);
    }

    free(tableP->entriesPP);

    for( i = 0; i <= MAXBANK; ++i )
    {
        if( tableP->banksP[i] )
        {
            free(tableP->banksP[i]);
        }
    }

    for( defP = tableP->labelsP; defP; defP = nextDefP )
    {
        nextDefP = defP->nextP;
        free(defP);
    }

    // A block owns only its own edge list, so freeing the entries first is safe.
    freeBlockList(tableP);

    // A region names its words by index and owns nothing else.
    freeRegionList(tableP);

    // Sharing groups before the routines they name, though a group never
    // dereferences a routine while being freed.
    freeShareGroups(tableP);

    // Routines own no block or word, so this may run either side of
    // freeBlockList().
    freeRoutineList(tableP);

    // The transform record before the findings it points at; the trees a fired
    // rewrite installed belong to the parse tree and stay.
    freeXformList(tableP);

    // The guess owns none of the blocks, routines and words it points at.
    freeGuess(tableP);

    // Nor does the window analysis.
    freeWindow(tableP);

    // A finding owns its two strings; its words went with the table.
    freeFindingList(tableP->findingsP);
    freeFindingList(tableP->suppressedP);

    free(tableP);
}

// Allocate an empty table with room for INITIAL_CAPACITY entries; an allocation
// failure is fatal.
static OptTableP
newTable(void)
{
OptTableP tableP;

    if( !(tableP = (OptTableP)calloc(1, sizeof(OptTable))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    tableP->capacity = INITIAL_CAPACITY;

    if( !(tableP->entriesPP = (OptWordP *)calloc(tableP->capacity, sizeof(OptWordP))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    return(tableP);
}

// Append one entry and index it.  This is the only range check, so nothing
// downstream repeats it; an entry outside memory fails the build.
// Returns the new entry, or NILP if it was out of range.
static OptWordP
addEntry(OptTableP tableP, int bank, int addr, int value, OptKind kind,
    PNodeP nodeP, PNodeP exprP, SymNodeP symP, int lineNo, unsigned int flags)
{
OptWordP entryP;
OptWordP *newPP;

    if( (bank < 0) || (bank > MAXBANK) || (addr < 0) || (addr >= BANKSIZE) )
    {
        fprintf(stderr, "am1: optimizer: word at bank %d address 0%o is outside memory (line %d)\n",
            bank, addr, lineNo);
        builder.failed = true;
        return(NILP);
    }

    if( tableP->count >= tableP->capacity )
    {
        // Double the entry array; the entries themselves do not move.
        tableP->capacity = (tableP->capacity * 2);

        if( !(newPP = (OptWordP *)realloc(tableP->entriesPP, (tableP->capacity * sizeof(OptWordP)))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer word table\n");
            exit(1);
        }

        tableP->entriesPP = newPP;
    }

    if( !(entryP = (OptWordP)calloc(1, sizeof(OptWord))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    entryP->bank = bank;
    entryP->addr = addr;
    entryP->value = (value & WRDMASK);
    entryP->kind = kind;
    entryP->nodeP = nodeP;
    entryP->exprP = exprP;
    entryP->symP = symP;
    entryP->fileP = builder.fileP;
    entryP->lineNo = lineNo;
    // Region flags are applied as the word is created: only here is being
    // between an optimize and its endoptimize a fact, not a reconstruction.
    // So is being a word of the view's drum overlay.
    entryP->flags = (flags | optRegionWordFlags());

    if( builder.inOverlay )
    {
        entryP->flags |= OPTF_OVWORD;
        ++tableP->ovWordCount;
    }
    entryP->index = tableP->count;
    entryP->labelsP = NILP;
    entryP->sameAddrP = NILP;

    tableP->entriesPP[tableP->count++] = entryP;
    optRegionNoteWord(tableP, entryP);

    if( flags & OPTF_RESERVED )
    {
        ++tableP->reservedCount;
    }

    indexEntry(tableP, entryP);
    checkPlacement(tableP, entryP);
    return(entryP);
}

// Index an entry by bank and address.  A later entry at the same address (an
// overlay, only under -M) chains behind the first through sameAddrP, and the
// whole chain is flagged OPTF_DUPADDR.
static void
indexEntry(OptTableP tableP, OptWordP entryP)
{
OptBankP bankP;
OptWordP chainP;

    if( !(bankP = tableP->banksP[entryP->bank]) )
    {
        if( !(bankP = (OptBankP)calloc(1, sizeof(OptBank))) )
        {
            fprintf(stderr, "am1: out of memory building the optimizer word table\n");
            exit(1);
        }

        bankP->bank = entryP->bank;
        tableP->banksP[entryP->bank] = bankP;
    }

    ++bankP->count;

    if( !bankP->wordsP[entryP->addr] )
    {
        bankP->wordsP[entryP->addr] = entryP;
        return;
    }

    // Walk to the end of the chain, then flag the whole chain.  The head was
    // not a duplicate until now, so it may not be flagged yet.
    for( chainP = bankP->wordsP[entryP->addr]; chainP->sameAddrP; chainP = chainP->sameAddrP )
    {
        ;
    }

    chainP->sameAddrP = entryP;

    for( chainP = bankP->wordsP[entryP->addr]; chainP; chainP = chainP->sameAddrP )
    {
        if( !(chainP->flags & OPTF_DUPADDR) )
        {
            chainP->flags |= OPTF_DUPADDR;
            ++tableP->dupCount;
        }
    }
}

// Record a label for attachment after the walk: a label on a line of its own
// names whatever is emitted at its address, possibly by a later statement or
// a pool.
static void
addLabelDef(OptTableP tableP, SymNodeP symP, int bank, int addr)
{
OptLabelDefP defP;

    if( !(defP = (OptLabelDefP)calloc(1, sizeof(OptLabelDef))) )
    {
        fprintf(stderr, "am1: out of memory building the optimizer word table\n");
        exit(1);
    }

    defP->symP = symP;
    defP->bank = bank;
    defP->addr = addr;
    defP->attached = 0;
    defP->nextP = tableP->labelsP;
    tableP->labelsP = defP;
    ++tableP->labelCount;
}

// Attach each label to every entry at its address (each entry of an overlay).
// A label with no entry there stays on the definition list with attached 0.
static void
attachLabels(OptTableP tableP)
{
OptLabelDefP defP;
OptBankP bankP;
OptWordP entryP;
OptLabelP labelP;

    for( defP = tableP->labelsP; defP; defP = defP->nextP )
    {
        if( (defP->bank < 0) || (defP->bank > MAXBANK) || (defP->addr < 0) || (defP->addr >= BANKSIZE) )
        {
            continue;
        }

        if( !(bankP = tableP->banksP[defP->bank]) )
        {
            continue;
        }

        for( entryP = bankP->wordsP[defP->addr]; entryP; entryP = entryP->sameAddrP )
        {
            if( !(labelP = (OptLabelP)calloc(1, sizeof(OptLabel))) )
            {
                fprintf(stderr, "am1: out of memory building the optimizer word table\n");
                exit(1);
            }

            labelP->symP = defP->symP;
            labelP->nextP = entryP->labelsP;
            entryP->labelsP = labelP;
            defP->attached = 1;
        }
    }
}

// Free one entry's label list.
static void
freeLabelList(OptLabelP labelP)
{
OptLabelP nextP;

    while( labelP )
    {
        nextP = labelP->nextP;
        free(labelP);
        labelP = nextP;
    }
}

// Walk the statement list and add every word it emits: writeStatements() from
// bincodegen.c without the overwrite checks, since here an overwrite simply
// becomes an overlay chain.
static void
walkStatements(OptTableP tableP, PNodeP nodeP)
{
    while( nodeP && !builder.failed )
    {
        // A drum overlay's words are not the root's: they go to the drum, and
        // its pc is its window's.  Only the view's own overlay is in its
        // table, and a region declared inside an overlay declares only there.
        if( nodeP->overlay && (!viewOvP || (nodeP->overlay != viewOvP->number)) )
        {
            nodeP = nodeP->leftP;
            continue;
        }

        // In an overlay's view the root is context: its words are analyzed
        // but its regions, speed declarations and ceilings were the root
        // view's to act on.  So is a %%ceiling inside the overlay, since the
        // window's last address bounds an overlay, not a bank's ceiling.
        if( viewOvP && ((nodeP->type == OPTIMIZE) || (nodeP->type == ENDOPTIMIZE)) &&
            (!nodeP->overlay || (nodeP->flags & PN_CEILING)) )
        {
            nodeP = nodeP->leftP;
            continue;
        }

        switch( nodeP->type )
        {
        case COMMENT:           // none of these emit anything or change state
        case TERMINATOR:
            break;

        case ORIGIN:
            // "expr/" moves the pc.  The node never carries a word (rightP is
            // empty), but the generators check anyway, so this does too.
            builder.pc = (nodeP->value.ival & ADDRMASK);

            if( canReduce(nodeP->rightP) )
            {
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            break;

        case EXPR:
            if( canReduce(nodeP->rightP) )
            {
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            // The label itself, at the node's pc, whatever follows it.
            addLabelDef(tableP, nodeP->value.symP, nodeP->bank, nodeP->pc);

            if( canReduce(nodeP->rightP) )
            {
                // A single word on the same line as the label ("foo, jmp bar").
                addExprWord(tableP, nodeP, nodeP->rightP);
            }
            else if( nodeP->rightP )
            {
                // A text directive on the same line as the label
                // ("msg, text \"hello\""), which emits several words.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                    addTextWords(tableP, nodeP, nodeP->rightP, OPTK_TEXT);
                    break;

                case TYPE340:
                    addTextWords(tableP, nodeP, nodeP->rightP, OPTK_TYPE340);
                    break;

                case ASCII:
                    addAsciiWords(tableP, nodeP, nodeP->rightP);
                    break;

                default:
                    // A directive that emits nothing (local, endloc and the like).
                    break;
                }
            }
            break;

        case VARS:
            // An explicit 'variables' directive: the list of every variable
            // declared in this bank so far, addresses already assigned.
            addVarWords(tableP, (PNodeListP)(nodeP->value.ptr), nodeP->bank);
            break;

        case CONSTANTS:
            // An explicit 'constants' directive: the pool gathered so far in
            // this bank.  The parser leaves value.symP empty when there was
            // nothing to pool, and addConstWords() accepts that.
            addConstWords(tableP, nodeP->value.symP, nodeP->bank);
            break;

        case TEXT:
            addTextWords(tableP, nodeP, nodeP, OPTK_TEXT);
            break;

        case TYPE340:
            addTextWords(tableP, nodeP, nodeP, OPTK_TYPE340);
            break;

        case ASCII:
            addAsciiWords(tableP, nodeP, nodeP);
            break;

        case BANK:
            // Switch banks; value2 holds the pc the new bank continues at.
            builder.bank = nodeP->value.ival;
            builder.pc = (nodeP->value2.ival & ADDRMASK);
            break;

        case OPTIMIZE:
        case ENDOPTIMIZE:
            // Nothing is emitted; the markers raise and lower the counter
            // addEntry() reads, so region membership follows emission order.
            optRegionStatement(tableP, nodeP, builder.bank, builder.pc);
            break;

        case FILENAME:
            // A cpp line marker.  The words that follow come from this file,
            // with its line numbers, so a finding can name the file to look in.
            builder.fileP = nodeP->value.strP;
            break;

        case TABLE:
            addTableWords(tableP, nodeP);
            break;

        case OVCALL:
            addOvCallWords(tableP, nodeP);
            break;

        case OVTABLE:
            addOvTableWords(tableP, nodeP);
            break;

        case OVERLAYDECL:
            // Only the view's own overlay reaches here.
            enterOverlayView(nodeP);
            break;

        case ENDOVERLAY:
            leaveOverlayView(tableP);
            break;

        default:
            // Everything else (var declarations, export, import, comments of
            // every flavor, file markers) emits nothing.
            break;
        }

        nodeP = nodeP->leftP;
    }
}

// Enter the view's drum overlay at its %%overlay, nodeP: its words are laid in
// its window's bank from the window's first address past the header words,
// as the parser laid them, and the root resumes at %%endoverlay.
static void
enterOverlayView(PNodeP nodeP)
{
OverlayContextP ovP;

    ovP = (OverlayContextP)(nodeP->value.ptr);
    builder.rootBank = builder.bank;
    builder.rootPC = builder.pc;
    builder.bank = ovP->windowP->bank;
    builder.pc = (ovP->windowP->first + OVHEADERWORDS);
    builder.inOverlay = true;
}

// Leave the view's drum overlay at its %%endoverlay: add the constants and
// variables the parser placed at its end, constants first, as the overlay's
// code generator writes them, then resume the root where it was.
static void
leaveOverlayView(OptTableP tableP)
{
OverlayContextP ovP;

    ovP = viewOvP;

    if( ovP->ctx.constSymP || ovP->ctx.varNodesP )
    {
        builder.pc = (ovP->ctx.constSymP)?ovP->ctx.constPC:ovP->ctx.varPC;

        if( ovP->ctx.constSymP )
        {
            addConstWords(tableP, ovP->ctx.constSymP, ovP->ctx.bank);
        }

        if( ovP->ctx.varNodesP )
        {
            addVarWords(tableP, ovP->ctx.varNodesP, ovP->ctx.bank);
        }
    }

    builder.inOverlay = false;
    builder.bank = builder.rootBank;
    builder.pc = builder.rootPC;
}

// Is this word the subject of its table's view?  Every word is in the root's
// view; in a drum overlay's view only the overlay's are, and the root's are
// context, already analyzed and rewritten in the root's view.
// Returns 1 if it is, 0 if not.
int
optInView(OptTableP tableP, OptWordP entryP)
{
    return( (!tableP->viewOvP || (entryP->flags & OPTF_OVWORD))?1:0 );
}

// Add the one word of an expression statement (EXPR, or a label with an
// expression on its line) at the node's pc, reduced as the binary generator
// reduces it.
static void
addExprWord(OptTableP tableP, PNodeP nodeP, PNodeP exprP)
{
int value;

    value = reduceOperand(exprP);
    addEntry(tableP, nodeP->bank, nodeP->pc, value, OPTK_EXPR, nodeP, exprP, NILP, nodeP->lineNo, 0);
    advancePC(1);
}

// Add the words of a packed 'text' or 'type340' string, credited to nodeP;
// textNodeP carries the FlexText.  The packing is writeText()'s in bincodegen.c.
static void
addTextWords(OptTableP tableP, PNodeP nodeP, PNodeP textNodeP, OptKind kind)
{
int i;
int val;
int addr;
char *bufP;
FlexText flexText;

    flexText = textNodeP->value.flexText;
    bufP = flexText.bufP;
    addr = textNodeP->pc;

    // Three six-bit characters per word, high first; a short last word is
    // padded with zeros, and an empty string still emits one zero word.
    // writeText() never clears its accumulator, so later words carry earlier
    // characters above bit 17; the tape writer drops them, and so does the mask.
    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
            advancePC(1);
        }

        val = (((val << 6) | *bufP++) & WRDMASK);
    }

    if( i % 3 )     // a partial last word, pad it out
    {
        while( i++ % 3 )
        {
            val = ((val << 6) & WRDMASK);
        }

        addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
        advancePC(1);
    }
    else if( (i >= flexText.nchars) && !(i % 3) )
    {
        // The last full word, or the single zero word of an empty string.
        addEntry(tableP, textNodeP->bank, addr++, val, kind, nodeP, NILP, NILP, textNodeP->lineNo, 0);
        advancePC(1);
    }
}

// Add the words of a packed 'ascii' string, credited to nodeP, as writeAscii()
// in bincodegen.c packs them: two nine-bit characters per word, high first, the
// NUL included, a lone last character padded with a zero low byte.
static void
addAsciiWords(OptTableP tableP, PNodeP nodeP, PNodeP asciiNodeP)
{
int i;
int word;
int addr;
char *strP;

    strP = asciiNodeP->value.strP;
    addr = asciiNodeP->pc;
    word = 0;
    i = 0;      // 0 is doing the high byte, 1 the low byte

    do
    {
        if( !i )
        {
            word = *strP;
        }
        else
        {
            word = ((word << 9) | *strP);
            addEntry(tableP, asciiNodeP->bank, addr++, word, OPTK_ASCII, nodeP, NILP, NILP, asciiNodeP->lineNo, 0);
            advancePC(1);
        }

        i ^= 1;
    }
    while( *strP++ );

    if( i )         // a high byte with no low byte to pair it with
    {
        addEntry(tableP, asciiNodeP->bank, addr++, (word << 9), OPTK_ASCII, nodeP, NILP, NILP, asciiNodeP->lineNo, 0);
        advancePC(1);
    }
}

// Add the words of a 'table' directive.  An initializer is evaluated once, as
// the binary generator does; without one the words are OPTF_RESERVED, occupying
// memory with nothing emitted.
static void
addTableWords(OptTableP tableP, PNodeP nodeP)
{
int i;
int value;
unsigned int flags;

    if( nodeP->rightP )
    {
        value = evalExpr(nodeP->rightP);
        flags = 0;
    }
    else
    {
        value = 0;
        flags = OPTF_RESERVED;
    }

    for( i = 0; (i < nodeP->value.ival) && !builder.failed; ++i )
    {
        addEntry(tableP, nodeP->bank, (nodeP->pc + i), value, OPTK_TABLE, nodeP, nodeP->rightP, NILP,
            nodeP->lineNo, flags);
        advancePC(1);
    }
}

// Add one word per variable, most recent declaration first, the generators'
// order.  Each item is the variable's ADDR node (leftP the initializer), and
// the variable's name is also a label at its word.
static void
addVarWords(OptTableP tableP, PNodeListP listP, int bank)
{
int value;
PNodeP nodeP;
SymNodeP symP;

    while( listP && !builder.failed )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        value = (nodeP->leftP)?reduceOperand(nodeP->leftP):0;
        addEntry(tableP, bank, nodeP->pc, value, OPTK_VAR, nodeP, nodeP->leftP, symP, nodeP->lineNo, 0);
        addLabelDef(tableP, symP, bank, nodeP->pc);
        advancePC(1);

        listP = listP->nextP;
    }
}

// Add a constant pool's words in the parser's pre-order (node, left, right),
// the generators' order.  A slot's value is its address, value2 the constant,
// ptr the expression that first interned it.
static void
addConstWords(OptTableP tableP, SymNodeP symP, int bank)
{
PNodeP exprP;
int lineNo;

    if( !symP || builder.failed )
    {
        return;
    }

    exprP = (PNodeP)(symP->ptr);
    lineNo = (exprP)?exprP->lineNo:-1;

    addEntry(tableP, bank, symP->value, symP->value2, OPTK_CONST, NILP, exprP, symP, lineNo, 0);
    // The pool symbol's name is a fingerprint, not source text, but it is the
    // only handle a CONSTANT reference node carries.
    addLabelDef(tableP, symP, bank, symP->value);
    advancePC(1);

    addConstWords(tableP, symP->leftP, bank);
    addConstWords(tableP, symP->rightP, bank);
}

// Add a %%ovcall's words: jsp ovEnter, or eem and jsp i through a pool word
// when the manager is in another bank, then the two data words.  Each is an
// EXPR node of its own, with its own pc, chained from the OVCALL node's
// rightP; the words are a hands-off span, since the manager reads the data
// words through the call's return address.
static void
addOvCallWords(OptTableP tableP, PNodeP nodeP)
{
PNodeP wordP;

    // In an overlay's view the call is the root's, context only, and its span
    // was the root view's to report.
    if( !viewOvP )
    {
        optRegionOvcall(tableP, nodeP, builder.bank, builder.pc, 1);
    }

    for( wordP = nodeP->rightP; wordP && !builder.failed; wordP = wordP->leftP )
    {
        addEntry(tableP, wordP->bank, wordP->pc, reduceOperand(wordP->rightP), OPTK_EXPR, wordP, wordP->rightP,
            NILP, wordP->lineNo, OPTF_OVCALL);
        advancePC(1);
    }

    if( !viewOvP )
    {
        optRegionOvcall(tableP, nodeP, builder.bank, builder.pc, 0);
    }
}

// Add the drum overlay descriptor table's words, labeled ovTable.  Their
// values are made after the optimizer, so they are reserved here; the manager
// reads and writes them through pointers no edge records, so they are pinned.
static void
addOvTableWords(OptTableP tableP, PNodeP nodeP)
{
int i;

    if( ovTableSymP )
    {
        addLabelDef(tableP, ovTableSymP, nodeP->bank, nodeP->pc);
    }

    for( i = 0; (i < nodeP->value.ival) && !builder.failed; ++i )
    {
        addEntry(tableP, nodeP->bank, (nodeP->pc + i), 0, OPTK_TABLE, nodeP, NILP, NILP, nodeP->lineNo,
            (OPTF_RESERVED | OPTF_OVREF));
        advancePC(1);
    }
}

// Flag every root word a drum overlay names OPTF_OVREF, as referenced from
// outside the root: the overlay's words are not in the table, so no edge
// records the reference.  An overlay's view holds that overlay's words, whose
// references are edges, and pins for every other overlay, so each sees what
// the others do to the root.  nodeP is the statement list.
static void
pinOverlayReferences(OptTableP tableP, PNodeP nodeP)
{
int i;
PNodeP subP;
OverlayContextP ovP;

    if( !overlaysP )
    {
        return;
    }

    // An alias holds a copy of its root symbol's address, which a relayout
    // may have moved since.
    refreshOverlayAliases();

    for( ; nodeP; nodeP = nodeP->leftP )
    {
        if( !nodeP->overlay || (viewOvP && (nodeP->overlay == viewOvP->number)) )
        {
            continue;
        }

        for( ovP = overlaysP; ovP && (ovP->number != nodeP->overlay); ovP = ovP->nextP )
        {
            ;
        }

        if( !ovP )
        {
            continue;
        }

        switch( nodeP->type )
        {
        case EXPR:
        case LOCATION:
        case LCLLOCATION:
            if( canReduce(nodeP->rightP) )
            {
                pinWordExpr(tableP, ovP, nodeP->rightP);
            }
            break;

        case TABLE:
            pinWordExpr(tableP, ovP, nodeP->rightP);
            break;

        case VAR:
            // The vars of one statement, linked through rightP, each with its
            // initial value in leftP.
            for( subP = nodeP->rightP; subP; subP = subP->rightP )
            {
                pinWordExpr(tableP, ovP, subP->leftP);
            }
            break;

        default:
            break;
        }
    }

    for( i = 0; i < tableP->count; ++i )
    {
        if( tableP->entriesPP[i]->flags & OPTF_OVREF )
        {
            ++tableP->ovRefCount;
        }
    }
}

// Pin the root words one word of overlay ovP names in exprP: each named word,
// and, when the word's address field lands elsewhere in the same bank
// (rootTab+2), every word from the named one to there, since a deletion
// between them would change what the overlay's word holds.  A literal's own
// expression is a word of its own, in the overlay's pool.
static void
pinWordExpr(OptTableP tableP, OverlayContextP ovP, PNodeP exprP)
{
int i;
int count;
int bank;
int first;
int last;
int target;
SymNodeP namesP[OPT_OVNAMES];

    if( !exprP )
    {
        return;
    }

    count = 0;
    rootNames(tableP, ovP, exprP, namesP, &count);

    if( !count )
    {
        return;
    }

    target = (evalExpr(exprP) & ADDRMASK);
    bank = namesP[0]->bank;
    first = target;
    last = target;

    for( i = 0; i < count; ++i )
    {
        if( namesP[i]->bank != bank )
        {
            // Names in two banks: no span between them means anything.
            pinRange(tableP, namesP[i]->bank, namesP[i]->value, namesP[i]->value);
            continue;
        }

        first = (namesP[i]->value < first)?namesP[i]->value:first;
        last = (namesP[i]->value > last)?namesP[i]->value:last;
    }

    pinRange(tableP, bank, first, last);
}

// Gather the root symbols an expression names outside its literals into
// namesPP, at most OPT_OVNAMES, *countP of them; a literal's expression is
// pinned as a word of its own as it is met.  An overlay's own symbols and its
// locals are not the root's; an alias is its root symbol.
static void
rootNames(OptTableP tableP, OverlayContextP ovP, PNodeP exprP, SymNodeP *namesPP, int *countP)
{
SymNodeP symP;

    for( ; exprP; exprP = exprP->rightP )
    {
        switch( exprP->type )
        {
        case CONSTANT:
            pinWordExpr(tableP, ovP, (PNodeP)exprP->value2.ptr);
            break;

        case ADDR:
        case BREF:
            symP = exprP->value.symP;

            if( symP->flags & SYMF_OVALIAS )
            {
                symP = symP->symP;
            }
            else if( (exprP->type == ADDR) && (sym_find(&(ovP->ctx.globalSymP), symP->name) == symP) )
            {
                symP = NILP;
            }

            if( symP && (*countP < OPT_OVNAMES) )
            {
                namesPP[(*countP)++] = symP;
            }
            else if( symP )
            {
                pinRange(tableP, symP->bank, symP->value, symP->value);
            }
            break;

        default:
            break;
        }

        rootNames(tableP, ovP, exprP->leftP, namesPP, countP);
    }
}

// Flag the root words of bank from first to last OPTF_OVREF, each entry at an
// address included.  In an overlay's view its own words are never pinned: an
// overlay's local names its own window's words.
static void
pinRange(OptTableP tableP, int bank, int first, int last)
{
int addr;
OptWordP entryP;

    if( (bank < 0) || (bank > MAXBANK) || !tableP->banksP[bank] )
    {
        return;
    }

    for( addr = first; (addr <= last) && (addr < BANKSIZE); ++addr )
    {
        for( entryP = tableP->banksP[bank]->wordsP[addr]; entryP; entryP = entryP->sameAddrP )
        {
            if( !(entryP->flags & OPTF_OVWORD) )
            {
                entryP->flags |= OPTF_OVREF;
            }
        }
    }
}

// Does an expression statement emit a word?  Decided as the generators decide:
// directives that parse as expressions, and the text directives, do not.
// Returns 1 if the expression emits a word, 0 if it does not.
static int
canReduce(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    switch( nodeP->type )
    {
    case ORIGIN:
    case LOCAL:
    case PRIVATE:
    case ADDLOCAL:
    case ENDLOC:
    case FORCELOC:
    case TERMINATOR:
        return(0);

    case TEXT:
    case ASCII:
    case TYPE340:
        return(0);

    case EXPR:
    case SEPARATOR:
        return( canReduce(nodeP->rightP) );

    default:
        return(1);
    }
}

// Reduce a statement's expression to its word as reduceOperand() in
// bincodegen.c does: a bare top-level '.' is the address being written; a
// nested '.' was snapshotted by the parser, so evalExpr() resolves it.
static int
reduceOperand(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(0);
    }

    if( nodeP->type == DOT )
    {
        return( builder.pc );
    }

    return( evalExpr(nodeP) );
}

// Advance the running pc as adjustPC() in bincodegen.c does, wrapping to 0
// when it runs off the end of the bank.
static void
advancePC(int incr)
{
    builder.pc = (builder.pc + incr);

    if( builder.pc > ADDRMASK )
    {
        builder.pc = 0;
    }
}

// Flag an entry whose parser-assigned bank and address differ from where the
// binary generator's running pc would put it: a parser defect the report names.
static void
checkPlacement(OptTableP tableP, OptWordP entryP)
{
    if( (entryP->bank != builder.bank) || (entryP->addr != builder.pc) )
    {
        entryP->flags |= OPTF_PCMISMATCH;
        ++tableP->mismatchCount;
    }
}

// Write the report: the notes, findings and refused patterns first, as the
// actionable part; then the regions and guess that answer P5, the transforms
// under a level, the evidence, the advisory sharing sections and statistics.
static void
writeReport(FILE *fP, OptTableP tableP, char *basenameP)
{
    fprintf(fP, "am1 optimizer report for %s\n", basenameP);
    fprintf(fP, "%s\n", AM1VERSION);
    fprintf(fP, "source file %s\n", (origFilenameP)?origFilenameP:basenameP);

    writeHeaderNotes(fP, tableP);
    writeFindingList(fP, tableP);
    writeSuppressedList(fP, tableP);
    writeWindowReport(fP, tableP);          // nothing without a reached eem or lem, or an unexamined reference
    writeRegionReport(fP, tableP);
    writeHandsOffReport(fP, tableP);
    writeSpeedDeclReport(fP, tableP);       // nothing without a declaration
    writeCeilingReport(fP, tableP);         // nothing without a %%ceiling
    writeGuessReport(fP, tableP);

    if( transformLevel >= 1 )
    {
        writeTransformReport(fP, tableP);
    }

    writeConflictList(fP, tableP);
    writeUnreachedList(fP, tableP);
    writeCycleList(fP, tableP);
    writeSharingReport(fP, tableP);
    writeScratchSharingReport(fP, tableP);
    writeStatistics(fP, tableP);
}

// A drum overlay's section of the report, after the root's: the sections that
// are the overlay's alone.  Its findings are its own words' (the rules file
// none on the root's in this view), and so are its regions.
static void
writeOverlayReport(FILE *fP, OptTableP tableP)
{
    fprintf(fP, "\n\n");
    writeOverlayHeading(fP, tableP);
    writeFindingList(fP, tableP);
    writeSuppressedList(fP, tableP);
    writeRegionReport(fP, tableP);
    writeHandsOffReport(fP, tableP);
    writeSpeedDeclReport(fP, tableP);       // nothing without a declaration
    writeGuessReport(fP, tableP);

    if( transformLevel >= 1 )
    {
        writeTransformReport(fP, tableP);
    }

    writeUnreachedList(fP, tableP);
    writeOverlayStatistics(fP, tableP);
}

// The report's name for a word kind.
const char *
kindName(OptKind kind)
{
    switch( kind )
    {
    case OPTK_EXPR:
        return("expr");

    case OPTK_TEXT:
        return("text");

    case OPTK_ASCII:
        return("ascii");

    case OPTK_TYPE340:
        return("type340");

    case OPTK_TABLE:
        return("table");

    case OPTK_VAR:
        return("var");

    case OPTK_CONST:
        return("const");

    default:
        return("unknown");
    }
}
