/*
 * srccodegen.c - write the program back out as am1 source, for -O=source.
 *
 * Writes the program, after the optimizer and relayout have run, to
 * <basename>.opt.am1 beside the source.  A line the optimizer did not touch is
 * copied from the file it came from, keeping the source's #defines, macro
 * calls, #ifs, includes and comments.  A line it touched is rendered from the
 * tree, after a comment quoting what it was.  Under plain -O the output is the
 * source again.
 *
 * The copying rests on a line map (srcMapLines()), made before any rewrite:
 * the statement list's lines are paired, in order, with the lines of cpp's
 * output, whose markers name the file, line and #include each came from.  The
 * lexer's own line numbers drift around a lone label, an unclosed constant and
 * a C comment, so they cannot serve.  If the map does not hold, or the text
 * cannot give the order of lines needed, every line is rendered from the tree
 * instead (-O=source=tree), to be assembled with -n; stderr says so.  An
 * include file holding a change is written out in place of its #include line.
 *
 * Every statement is written so the lexer reads the same tokens back: a binary
 * operator has a blank on each side, so "a / b" is never read as an origin, and
 * operands are joined by a tab, so a separator stays a separator before a unary
 * minus.  Numbers are written in octal.
 *
 * Under -O1 and -O2 each change is annotated with a comment beginning
 * "am1 -O1:" (the level as run): a rewritten line says what it was, a deleted
 * word's line what it held, and a moved run or a copy is bracketed by a comment
 * line at each end.  The notes are collected from the word table before
 * optimize() frees it (srcCollect()), and from relayout (srcNoteCopy()).
 *
 * The constant pools: a plain assembly files each pool key in an unbalanced
 * tree at its first reference and places the words in pre-order, so a pool's
 * order follows where its constants are first named.  A rewrite can stop naming
 * a constant or move where it is first named.  A planning walk, in output
 * order, finds every key that would be filed before one of its ancestors and
 * every pool word nothing names any more, and writes a "table 0, [expr]" line
 * (names the constant, emits no word) ahead of the line that needs it, so the
 * pool comes out the same.
 *
 * A key read from a symbol not yet defined is its creation serial number, and
 * relayout's model counts every creation T3 and T8 took away, so the output
 * makes each one again after the new word: "table 0, name" for a symbol,
 * "table 0, [expr]" for a constant still pooled, and "local am1gone; endloc"
 * for a pool word since reclaimed.  A pool word relayout kept under the key of
 * an address it moved cannot be keyed by any source line; when that changes the
 * pool order, the header and stderr say so.
 *
 * A number relayout re-points (a T3's unnamed far target, a T8d constant
 * holding an address) is written from the nearest label below it, or as the
 * constant's own expression, or else annotated.  A copy stands outside its
 * routine's scope, so a local of that routine is written as '. + N'.
 *
 * Called from main() in am1.c after optimize(); it does not change the tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <ctype.h>

#include "am1.h"
#include "y.tab.h"
#include "type340chars.h"
#include "optimizer.h"

#define SRC_FMT         256     // the longest piece written through putf()
#define SRC_DEFINES     2048    // the -D set, as given, for the header
#define SRC_STRING      4096    // the longest text, ascii or type340 string rendered
#define SRC_NOTE        160     // the longest note text, and an expression quoted in one
#define SRC_DEPTH       4096    // the deepest pool tree the keeper plan follows
#define SRC_ITEMS       24      // the symbols and constants one old expression may name
#define SRC_REACH       0100    // the farthest a label may stand below the address it names
#define SRC_NEST        256     // the deepest #include nesting the line map follows

// A node flag: a note or a keeper belongs to this node, so the list is searched
// only for these.  Below relayout's flags in optimizer.h, above every PN_ flag.
#define SRC_NOTED       0x10000000

typedef enum
{
    SN_REWRITE,         // a word rewritten in place: oldP is what the source had
    SN_COPY,            // a copy: nodeP is its first node and lastP its last
    SN_KEEPER           // a pool key filed ahead of the line starting at nodeP
} SrcNoteKind;

typedef enum
{
    SI_MENTION,         // a symbol the old expression created, named again
    SI_CONSTANT,        // a constant whose pool word it created, named again
    SI_PHANTOM          // the same for a pool word reclaimed since: a symbol made and dropped
} SrcItemKind;

typedef struct
{
    SrcItemKind kind;
    PNodeP exprP;       // the symbol's node, or the constant's own expression
} SrcItem;

typedef struct srcnote
{
    struct srcnote *nextP;
    SrcNoteKind kind;
    PNodeP nodeP;
    PNodeP lastP;
    PNodeP oldP;        // SN_REWRITE: the old expression; SN_KEEPER: the constant's
    bool orphan;        // SN_KEEPER: nothing in the program names the word any more
    bool early;         // SN_REWRITE: a T3 or T8, made before relayout and its model
    bool asWord;        // SN_REWRITE: the new expression is written as its word
    int itemCount;      // SN_REWRITE: what the old expression created, named after it
    SrcItem items[SRC_ITEMS];
    // SN_REWRITE, a T3 or T8d whose new word holds an address as a number:
    // the number's node, and how it is written instead -- T8d's constant as
    // its source spelled it, or a T3's label and the offset from it.
    PNodeP numberP;
    PNodeP spellP;
    SymNodeP labelP;
    int offset;
    int bank;
    bool farNumber;     // a T3's: the plan looks for a label that reaches it
    char text[SRC_NOTE];
} SrcNote, *SrcNoteP;

// One pool slot of the optimized program, for the keeper plan.
typedef struct
{
    SymNodeP symP;
    SymNodeP parentP;   // its parent in the pool's tree, NILP for the root
    int refs;           // references the written program makes to it
    bool filed;         // the plan has passed a reference or a keeper for it
    bool early;         // and that one is on an earlier line, or is a keeper
    int line;           // the line of the reference that filed it
} SrcSlot, *SrcSlotP;

// The text layer: a file cpp read, as its lines.
typedef struct
{
    char *nameP;        // as cpp's line markers name it, from the working directory
    char *bufP;
    char **linesPP;     // line N is linesPP[N - 1], its newline cut off
    int count;
} SrcText, *SrcTextP;

// One reading of a file: the source itself, or one #include of a file.
typedef struct
{
    char *nameP;
    SrcTextP textP;     // read when the map first needs it
    int parent;         // the reading whose #include made this one; -1 for none
    int includeLine;    // that #include's line in the parent
    bool changed;       // written out in full, rather than kept as its #include
    bool kept;          // its #include line has been written
    bool opened;        // it has been written out, or is being
    bool otherDir;      // its directory is not the source's: a quoted #include in it
                        // is written with the path it named
    char *skipP;        // skipP[N]: line N is not copied (rendered, moved or gone)
    int *childAtP;      // childAtP[N]: the reading line N's #include made, or -1
    int cursor;         // the next line not yet copied or passed over
} SrcInst, *SrcInstP;

// One line of the statement list as parsed, and the source lines it came from.
typedef struct
{
    int inst;
    int first;
    int last;
    int nodes;          // the statement-list nodes it was parsed as
    bool seen;          // the optimized list still holds it, in place or moved
} SrcGroup, *SrcGroupP;

// A statement-list node's line, in the map's hash.
typedef struct
{
    PNodeP nodeP;
    int group;
} SrcMapItem;

// A stretch of the optimized statement list, as the text layer writes it.
typedef enum
{
    SU_PASS,            // a cpp marker or a blank line: the copied text gives it
    SU_LINE,            // a line the map knows, where it was: copied or rendered
    SU_OTHER            // rendered where it now stands: a moved run, a copy, a marker
} SrcUnitKind;

typedef struct
{
    SrcUnitKind kind;
    PNodeP firstP;
    PNodeP lastP;
    int group;          // SU_LINE: its line
    bool copyable;      // SU_LINE: nothing in it changed, so its text is copied
} SrcUnit;

extern PNodeListP wildcardsP;       // every sym:* reference, from the parser
extern SymListP constsListP;        // every constant pool, from the parser
extern BankContextP banksP;         // every bank, with its global symbols
extern char pfilename[];            // cpp's output file, from am1.c
extern bool doCpp;                  // cpp was run: false under -n
extern char *origFilenameP;         // the source named on the command line

extern char asciiToFlexo(char ac, int *shiftP);
extern int flexoToAscii(char fc, int *shiftP);
extern char asciiToType340(char ch);
extern int type340Shift(char ch);
extern int evalExpr(PNodeP nodeP);
extern long int hashExpr(PNodeP nodeP);

static FILE *srcfP;
static bool atBol;                  // the last character written ended a line
static bool failed;                 // a node the renderer cannot write was met
static const char *lastFileP;       // the file the last FILENAME named, for the notes
static char defines[SRC_DEFINES];   // " -DX -DY=1", as given on the command line
static int level;                   // the level as run, 0 for -O

static SrcNoteP notesP;             // every note, in the order made, so keepers
static SrcNoteP notesTailP;         // are written in the plan's order
static char pending[SRC_STRING];    // annotations for the line being written
static char *captureP;              // put() appends here instead of writing, when set
static PNodeP wrapP;                // the name putName() writes in parentheses
static size_t captureSize;
static bool inNote;                 // an expression is being quoted in a note
static int copyDepth;               // the copies the node being written is inside
static bool dotAsValue;             // '.' is written as the location it had when parsed

static SrcSlotP slotsP;             // the keeper plan's slots, hashed by symbol
static int slotSize;
static int planLine;                // the lines the plan walk has begun
static bool planAtBol;              // the next node the plan walk meets begins a line
static PNodeP planHeadP;            // the first node of the plan walk's current line
static int planPass;                // 1 counts references, 2 plans the keepers
static int planSerial;              // the highest creation serial the plan walk has met
static int unkeyed;                 // pool words whose kept key no source line can give

// Each pool slot's first reference as parsed, taken before any rewrite: the
// reference whose expression the slot holds is the one that created it.
typedef struct
{
    SymNodeP symP;
    void *firstP;
} SrcFirst;

static SrcFirst *firstsP;
static int firstSize;

static char includes[SRC_DEFINES];  // " -Ipath", as given on the command line
static int srcMode;                 // SRC_MODE_TEXT, _RENDER or _TREE, as run
static bool textLayer;              // the line map holds: the text layer writes
static bool decimalSeen;            // cpp's output switches the lexer to decimal
static bool radixPrefix;            // so a rendered number carries 0o
static bool dryRun;                 // the text layer's walk writes nothing
static bool walkFailed;             // and it found an order the text cannot give
static const char *whyTree;         // why the map did not hold, for stderr

static SrcTextP *textsPP;           // every file the map has read
static int textCount;
static SrcInstP instsP;             // every reading, in the order cpp made them
static int instCount;
static int instSize;
static int mainInst;                // the source's own reading
static int curInst;                 // the reading the walk is writing
static SrcGroupP groupsP;           // every line of the list as parsed
static int groupCount;
static int groupSize;
static SrcMapItem *mapP;            // node -> group, open-addressed
static int mapSize;
static int mapCount;

static void srcNode(PNodeP nodeP);
static void srcMarker(PNodeP nodeP);
static int runLine(PNodeP firstP, PNodeP markerP);
static void srcNotesBefore(PNodeP nodeP);
static void srcNotesAfter(PNodeP nodeP);
static SrcNoteP rewriteOf(PNodeP nodeP);
static void srcWordOrOperand(PNodeP nodeP, PNodeP exprP);
static void endLine(void);
static void addPending(const char *textP);
static void quote(PNodeP nodeP, char *outP, size_t size);
static SrcNoteP addNote(SrcNoteKind kind, PNodeP nodeP);
static void planKeepers(PNodeP rootP);
static void planStatements(PNodeP nodeP);
static void planVars(PNodeP nodeP);
static void planRefs(PNodeP nodeP);
static void planRemoved(PNodeP nodeP);
static void planClose(SymNodeP rootP);
static void keeper(SrcSlotP slotP, PNodeP exprP, bool orphan);
static void planCreation(SrcNoteP noteP, PNodeP nodeP);
static bool createsEarly(PNodeP nodeP);
static SrcNoteP numberNoteOf(PNodeP nodeP);
static bool plainSpelling(PNodeP nodeP);
static void chooseLabel(SrcNoteP noteP);
static void nearestLabel(SymNodeP symP, SrcNoteP noteP, int address);
static void srcNumber(PNodeP nodeP);
static void planMention(SymNodeP symP);
static void addItem(SrcNoteP noteP, SrcItemKind kind, PNodeP exprP);
static void snapFirsts(SymNodeP symP);
static void *firstRefOf(SymNodeP symP);
static void addSlots(SymNodeP symP, SymNodeP parentP);
static void keyNow(SrcSlotP slotP, char *keyP, size_t size);
static bool keyHolds(SrcSlotP slotP);
static bool subtreeHolds(SymNodeP symP, SrcSlotP topP, const char *nowP);
static bool sameSide(int a, int b);
static SrcSlotP findSlot(SymNodeP symP);
static void srcStatements(PNodeP nodeP);
static void srcOperand(PNodeP nodeP);
static void srcTrailer(PNodeP nodeP);
static void srcVarList(PNodeP nodeP);
static void srcNameList(PNodeP nodeP);
static void srcAsciiString(const char *strP);
static void srcTextStmt(PNodeP nodeP);
static void srcType340Stmt(PNodeP nodeP);
static void srcTextWords(FlexText text, const char *whatP);
static bool decodeText(FlexText text, char *outP, int size);
static bool sameText(FlexText text, const char *strP);
static bool decodeType340(FlexText text, bool escapes, char *outP, int size);
static bool sameType340(FlexText text, const char *strP);
static void srcDirective(PNodeP nodeP);
static void markWildcards(void);
static void cannot(PNodeP nodeP, const char *whereP);
static void put(const char *strP);
static void putf(const char *fmtP, ...);
static void putNum(int value);
static bool startsParen(PNodeP nodeP);
static PNodeP endName(PNodeP nodeP);
static void putName(PNodeP nodeP);
static void startLine(void);
static void srcHeader(const char *sourceP);
static void srcRange(PNodeP firstP, PNodeP lastP);
static bool mapFail(const char *whyP);
static SrcTextP readText(const char *nameP);
static int newInst(const char *nameP, int parent, int includeLine);
static bool loadInst(int inst);
static void dirOf(const char *pathP, char *dirP, size_t size);
static bool isLineEnd(PNodeP nodeP);
static bool isMarkerLine(const char *lineP, int *lineNoP, char *nameP, size_t size, bool *enterP, bool *leaveP);
static bool isBlankLine(const char *lineP);
static bool continues(const char *lineP);
static bool isIncludeLine(const char *lineP, char *nameP, size_t size);
static bool originOnly(const char *lineP);
static bool labelOn(const char *lineP, const char *nameP);
static bool noteDecimal(const char *lineP);
static void mapNode(PNodeP nodeP, int group);
static int groupOf(PNodeP nodeP);
static SrcNoteP copyOf(PNodeP nodeP);
static PNodeP rangeEnd(PNodeP nodeP);
static PNodeP unitEnd(PNodeP nodeP);
static PNodeP nextUnit(PNodeP nodeP, SrcUnit *unitP);
static void markChanged(int inst);
static void markGone(int group);
static void markInside(PNodeP firstP, PNodeP lastP, int group);
static int commonReading(int a, int b);
static bool isWithin(int inst, int outerInst);
static void textAnalyze(PNodeP firstP);
static bool textWalk(PNodeP firstP);
static void moveTo(int inst);
static void openInst(int inst);
static void closeInst(int inst);
static void flushTo(int inst, int line);
static void copyLine(int inst, int line);
static void wasLine(const char *lineP);

// Record one -D given on the command line, for the header comment.
void
srcNoteDefine(const char *defineP)
{
size_t used;

    used = strlen(defines);

    if( (used + strlen(defineP) + 4) < sizeof(defines) )
    {
        snprintf(defines + used, sizeof(defines) - used, " -D%s", defineP);
    }
}

// Record one -I given on the command line, for the header comment.
void
srcNoteInclude(const char *pathP)
{
size_t used;

    used = strlen(includes);

    if( (used + strlen(pathP) + 4) < sizeof(includes) )
    {
        snprintf(includes + used, sizeof(includes) - used, " -I%s", pathP);
    }
}

// Write the program in the tree as am1 source on outfP; sourceP is the source
// file named on the command line, for the header comment.
// Returns 1 when every node was written, 0 when one could not be (each is named
// on stderr, and the output must not be used).
int
srcCodegen(FILE *outfP, PNodeP rootP, const char *sourceP)
{
    srcfP = outfP;
    failed = false;
    lastFileP = NILP;
    pending[0] = '\0';
    level = optTransformLevel();
    markWildcards();

    // Before a word is written: a keeper goes ahead of the line that needs it.
    if( level )
    {
        planKeepers(rootP);
    }

    // The text layer, where its map holds and the copied text can give the
    // lines in the optimized order: a dry walk proves that before anything
    // is written, so a file is never left half copied.
    if( textLayer )
    {
        textAnalyze(rootP->leftP);
        dryRun = true;
        textLayer = textWalk(rootP->leftP);
        dryRun = false;

        if( !textLayer )
        {
            whyTree = "the optimized order of the lines is one the copied text cannot give";
        }
    }

    if( !textLayer && (srcMode != SRC_MODE_TREE) )
    {
        fprintf(stderr, "am1: -O=source: %s; every line is written from the tree instead, "
            "to be assembled with -n\n", (whyTree)?whyTree:"the line map was not made");
    }

    radixPrefix = (textLayer && decimalSeen);

    if( textLayer )
    {
        // The source's title line, as written; the map proved it is line 1.
        fprintf(srcfP, "%s\n", instsP[mainInst].textP->linesPP[0]);
        atBol = true;
        srcHeader(sourceP);
        textWalk(rootP->leftP);
        return( failed?0:1 );
    }

    // The title line is the tape's, so it must come first and unchanged.
    fprintf(srcfP, "%s\n", rootP->value.strP);
    atBol = true;
    srcHeader(sourceP);

    srcStatements(rootP->leftP);

    endLine();
    if( rootP->rightP->type == START )
    {
        put("start ");
        if( rootP->rightP->exprP )
        {
            srcOperand(rootP->rightP->exprP);
        }
        else
        {
            putf("%o", rootP->rightP->value.ival & WRDMASK);
        }
        put("\n");
    }
    else
    {
        put("stop\n");
    }

    return( failed?0:1 );
}

// Write the header comment, after the title line: what wrote the file, and how
// it is to be assembled.
static void
srcHeader(const char *sourceP)
{
time_t now;
char date[32];

    now = time(NILP);
    strftime(date, sizeof(date), "%d-%b-%Y", localtime(&now));

    put("// Written by ");
    put(AM1SHORTVERSION);
    put(" -O=source from ");
    put(sourceP);
    putf(", %s, at ", date);
    put((level == 0)?"-O (nothing rewritten)":((level == 1)?"-O1":"-O2"));
    put(".\n");

    if( textLayer && !doCpp )
    {
        put("// A line the optimizer did not change is copied from the source: assemble this\n");
        put("// file with am1 -n, as the source was.\n");
    }
    else if( textLayer )
    {
        put("// A line the optimizer did not change is copied from the source or its include file,\n");
        put("// so cpp runs on this file again: assemble it with");
        put((defines[0] || includes[0])?"":" no -D or -I");
        put(defines);
        put(includes);
        put(".\n");
    }
    else
    {
        put("// cpp has been applied, with");
        put(defines[0]?defines:" no -D");
        put(": assemble this file with am1 -n.\n");
    }

    if( textLayer && (srcMode == SRC_MODE_RENDER) )
    {
        put("// am1 -O=source=render: every statement is written from the tree; only a line\n");
        put("// that holds none, a #define or a blank, is copied.\n");
    }

    if( radixPrefix )
    {
        put("// A number written from the tree carries 0o: the source switches the lexer to decimal.\n");
    }

    if( level )
    {
        putf("// What the optimizer changed is marked by comments beginning \"am1 -O%d:\".  A\n", level);
        if( textLayer )
        {
            put("// changed line is written from the program as assembled, numbers standing for\n");
            put("// the source's names, after a \"was:\" line quoting it; an include file with a\n");
            put("// change is written out in place of its #include line.  A\n");
        }
        put("// 'table 0, [...]' line emits no word: it names a constant, so the pool\n");
        put("// holds its word, or files it, where the optimized program's pool has it.\n");
        put("// Built with the optimizer's %%-directives in place, this file is optimized\n");
        put("// again: assemble it without -O to get the program it was written from.\n");
    }

    if( unkeyed )
    {
        putf("// am1 -O%d: NOT the optimized tape: %d pool word%s that nothing names kept the key of\n", level,
            unkeyed, (unkeyed == 1)?"":"s");
        put("// an address relayout moved, which no source line can give; the pool's order differs.\n");
    }
}

// A BREF that was written sym:* must be written that way again: the constant
// pools key a wildcard by its name and a bank reference by its symbol, so the
// other spelling can change which constants share a word.  relayout marks the
// same nodes, and its copies carry the mark; this covers a run without it.
static void
markWildcards(void)
{
PNodeListP itemP;

    for( itemP = wildcardsP; itemP; itemP = itemP->nextP )
    {
        if( itemP->nodeP && (itemP->nodeP->type == BREF) )
        {
            itemP->nodeP->flags |= RL_WILD;
        }
    }
}

// Write the statements along the list from nodeP.  Each terminator node ends
// its line, as it did in the source, so that the keywords am1 knows only at the
// start of a line (start, constants, variables, import, export, a C comment)
// start one again.
static void
srcStatements(PNodeP nodeP)
{
    for( ; nodeP; nodeP = nodeP->leftP )
    {
#ifdef SRCCODEGEN_UNMOVE
        // A test build only: each moved run is written where the source had
        // it, so a test can confirm it sees the tape change.
        PNodeP runP;

        if( (nodeP->type == RELAYOUT) && (nodeP->value.ival == RL_BEGIN) )
        {
            while( nodeP->leftP && !((nodeP->type == RELAYOUT) && (nodeP->value.ival == RL_END)) )
            {
                nodeP = nodeP->leftP;
            }
            continue;
        }

        if( (nodeP->type == RELAYOUT) && (nodeP->value.ival == RL_MOVEDFROM) && nodeP->value2.ptr )
        {
            for( runP = ((PNodeP)(nodeP->value2.ptr))->leftP;
                 runP && !((runP->type == RELAYOUT) && (runP->value.ival == RL_END)); runP = runP->leftP )
            {
                srcNode(runP);
            }
        }
#endif
        if( nodeP->flags & SRC_NOTED )
        {
            srcNotesBefore(nodeP);
        }

        srcNode(nodeP);

        if( nodeP->flags & SRC_NOTED )
        {
            srcNotesAfter(nodeP);
        }
    }
}

// Write the statements from firstP to lastP, both included, as srcStatements()
// writes each: the text layer's rendered stretches.
static void
srcRange(PNodeP firstP, PNodeP lastP)
{
PNodeP nodeP;

    for( nodeP = firstP; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP->flags & SRC_NOTED )
        {
            srcNotesBefore(nodeP);
        }

        srcNode(nodeP);

        if( nodeP->flags & SRC_NOTED )
        {
            srcNotesAfter(nodeP);
        }

        if( nodeP == lastP )
        {
            break;
        }
    }
}

// Write one node of the statement list.
static void
srcNode(PNodeP nodeP)
{
    switch( nodeP->type )
    {
    case TERMINATOR:
    case EMPTYLINE:
        endLine();
        break;

    case SEMI:
        put("; ");
        break;

    case COMMENT:
        // The token ends its line, whether it follows a statement or not.
        // An annotation follows the source's own comment.
        if( !atBol )
        {
            put("  ");
        }
        put("//");
        put(nodeP->value.strP);
        endLine();
        break;

    case CSCOMMENT:
        // The text holds everything from the start of its line to the "*/".
        startLine();
        put(nodeP->value.strP);
        put("*/");
        break;

    case FILENAME:
        // cpp's line marker: nothing to assemble, but the reader is told
        // where an include's text begins and ends.  The text layer says that
        // itself, and a marker inside a moved run or a copy is noise.
        if( textLayer )
        {
            break;
        }

        if( !lastFileP || strcmp(lastFileP, nodeP->value.strP) )
        {
            startLine();
            putf("// am1 -O=source: from %s\n", nodeP->value.strP);
            lastFileP = nodeP->value.strP;
        }
        break;

    case ORIGIN:
        if( nodeP->exprP )
        {
            srcOperand(nodeP->exprP);
        }
        else
        {
            putNum(nodeP->value.ival & WRDMASK);
        }
        put("/");
        break;

    case EXPR:
#ifdef SRCCODEGEN_DROP_WORD
        // A test build only: the first word written is left out, so a test
        // can confirm it sees the tape change.
        {
            static bool dropped;

            if( !dropped )
            {
                dropped = true;
                break;
            }
        }
#endif
        if( atBol )
        {
            put("\t");
        }
        srcWordOrOperand(nodeP, nodeP->rightP);
        break;

    case LOCATION:
    case LCLLOCATION:
        put(nodeP->value.symP->name);
        put(",");
        if( nodeP->rightP )
        {
            put("\t");
            srcWordOrOperand(nodeP, nodeP->rightP);
        }
        break;

    case VAR:
        put("\tvar ");
        srcVarList(nodeP->rightP);
        break;

    case VARS:
        put("\tvariables");
        break;

    case CONSTANTS:
        put("\tconstants");
        break;

    case BANK:
        put("\tbank ");
        putNum(nodeP->value.ival);
        break;

    case TABLE:
        put("\ttable ");
        if( nodeP->exprP )
        {
            srcOperand(nodeP->exprP);
        }
        else
        {
            putNum(nodeP->value.ival);
        }

        if( nodeP->rightP )
        {
            put(", ");
            srcOperand(nodeP->rightP);
        }
        break;

    case TEXT:
    case ASCII:
    case TYPE340:
        put("\t");
        srcTrailer(nodeP);
        break;

    case IMPORT:
        if( *(nodeP->value.strP) == '<' )
        {
            putf("\timport %s", nodeP->value.strP);
        }
        else
        {
            put("\timport \"");
            srcAsciiString(nodeP->value.strP);
            put("\"");
        }
        break;

    case EXPORT:
        put("\texport ");
        srcNameList(nodeP->rightP);
        break;

    case OPTIMIZE:
    case ENDOPTIMIZE:
        srcDirective(nodeP);
        break;

    case RELAYOUT:
        // A deleted word is gone, and a moved run is already where the
        // statement list now has it; the markers write only notes.
        srcMarker(nodeP);
        break;

    default:
        cannot(nodeP, "a statement");
        break;
    }
}

// Note what one of relayout's markers stands for.  A deleted word's note goes
// on its line, which keeps any label and comment it had; a moved run's markers
// are lines of their own at each end of the run and where it stood.
static void
srcMarker(PNodeP nodeP)
{
char buf[SRC_NOTE];
char text[SRC_NOTE + 64];
PNodeP firstP;

    switch( nodeP->value.ival )
    {
    case RL_DELETED:
        quote(nodeP->exprP, buf, sizeof(buf));
        snprintf(text, sizeof(text), "// am1 -O%d: deleted %s", level, buf);
        addPending(text);
        break;

    case RL_MOVEDFROM:
        // value2.ptr is the run's RL_BEGIN, and the run follows that marker.
        firstP = (nodeP->value2.ptr)?((PNodeP)(nodeP->value2.ptr))->leftP:NILP;
        startLine();
        putf("// am1 -O%d: the run from line %d stood here; it was moved\n", level, runLine(firstP, nodeP));
        break;

    case RL_BEGIN:
        firstP = nodeP->leftP;
        startLine();
        putf("// am1 -O%d: moved here: the run from line %d\n", level, runLine(firstP, nodeP));
        break;

    case RL_END:
        startLine();
        putf("// am1 -O%d: the moved run ends\n", level);
        break;

    default:
        break;
    }
}

// The source line a moved run began on: the line map's where it holds, which
// is exact, or else the lexer's.
static int
runLine(PNodeP firstP, PNodeP markerP)
{
int group;

    if( !firstP )
    {
        return(markerP->lineNo);
    }

    if( textLayer && ((group = groupOf(firstP)) >= 0) )
    {
        return(groupsP[group].first);
    }

    return(firstP->lineNo);
}

// Write what goes before a noted node: the start of a copy, and the keepers
// its line needs, each a line of its own.
static void
srcNotesBefore(PNodeP nodeP)
{
SrcNoteP noteP;

    for( noteP = notesP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->nodeP == nodeP) && (noteP->kind == SN_COPY) )
        {
            startLine();
            putf("// am1 -O%d: %s begins\n", level, noteP->text);
            ++copyDepth;
        }
    }

    for( noteP = notesP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->nodeP == nodeP) && (noteP->kind == SN_KEEPER) )
        {
            startLine();
            put("\ttable 0, [");
            dotAsValue = true;
            srcOperand(noteP->oldP);
            dotAsValue = false;
            put("]");
            addPending((noteP->orphan)?"// am1 -O%L: keeps the pool word the rewritten code no longer names"
                                     :"// am1 -O%L: files the constant here, as the optimized pool has it");
            endLine();
        }
    }

    for( noteP = notesP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->nodeP == nodeP) && (noteP->kind == SN_REWRITE) )
        {
            addPending(noteP->text);
            if( noteP->asWord )
            {
                addPending("// am1 -O%L: written as its word: the source names the target later");
            }
        }
    }
}

// The rewrite note of a statement, if it has one.
static SrcNoteP
rewriteOf(PNodeP nodeP)
{
SrcNoteP noteP;

    for( noteP = (nodeP->flags & SRC_NOTED)?notesP:NILP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->nodeP == nodeP) && (noteP->kind == SN_REWRITE) )
        {
            return(noteP);
        }
    }

    return(NILP);
}

// Write a statement's expression, or its word when naming its symbols there
// would make them sooner than the source did.
static void
srcWordOrOperand(PNodeP nodeP, PNodeP exprP)
{
SrcNoteP noteP;

    if( (noteP = rewriteOf(nodeP)) && noteP->asWord )
    {
        putNum(evalExpr(exprP) & WRDMASK);
    }
    else if( nodeP->type == EXPR )
    {
        srcOperand(exprP);
    }
    else
    {
        srcTrailer(exprP);
    }
}

// Write what goes after a noted node: what a rewrite's old expression created,
// made again on its line, and the end of a copy it closes.
static void
srcNotesAfter(PNodeP nodeP)
{
SrcNoteP noteP;
SrcItem *itemP;
int i;

    if( (noteP = rewriteOf(nodeP)) && noteP->itemCount )
    {
        inNote = true;      // a constant's comment would end the line
        dotAsValue = true;

        for( i = 0; i < noteP->itemCount; ++i )
        {
            itemP = &noteP->items[i];

            switch( itemP->kind )
            {
            case SI_MENTION:
                put("; table 0, ");
                srcOperand(itemP->exprP);
                break;

            case SI_CONSTANT:
                put("; table 0, [");
                srcOperand(itemP->exprP);
                put("]");
                break;

            case SI_PHANTOM:
                put("; local am1gone; endloc");
                break;
            }
        }

        inNote = false;
        dotAsValue = false;
        addPending("// am1 -O%L: after the word, what the old one named first, so the pool keys hold");
    }

    for( noteP = notesP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->lastP == nodeP) && (noteP->kind == SN_COPY) )
        {
            startLine();
            putf("// am1 -O%d: %s ends\n", level, noteP->text);
            --copyDepth;
        }
    }
}

// Collect what the rewrites did to each statement, from the word table as
// assembled: a statement whose expression is no longer the one its word was
// assembled from was rewritten in place (T3, T8, and the word a deleting rule
// keeps), and the table still holds the old one.  Called by optimize() after
// relayout and before the table is freed, only for -O=source.
void
srcCollect(OptTableP tableP)
{
int i;
OptWordP entryP;
PNodeP stmtP;
OptXformP recP;
SrcNoteP noteP;
const char *ruleP;
char buf[SRC_NOTE];
bool early;

    if( !tableP->xformApplied )
    {
        return;
    }

    level = optTransformLevel();

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        stmtP = entryP->nodeP;

        if( (entryP->kind != OPTK_EXPR) || !stmtP || !stmtP->rightP || !entryP->exprP ||
            (stmtP->rightP == entryP->exprP) ||
            ((stmtP->type != EXPR) && (stmtP->type != LOCATION) && (stmtP->type != LCLLOCATION)) )
        {
            continue;
        }

        ruleP = "rewritten";
        early = false;

        for( recP = tableP->xformsP; recP; recP = recP->nextP )
        {
            if( (recP->fate == OPTXF_FIRED) && recP->findingP &&
                ((recP->keepP == entryP) ||
                 (!recP->delP && (recP->findingP->wordsP[0] == entryP))) )
            {
                ruleP = optRuleName(recP->findingP->rule);

                // T3 and T8 are installed before relayout, so its model of the
                // pools counts what their old expressions created as still made.
                early = ((recP->findingP->rule == OPTRULE_T3) ||
                         ((recP->findingP->rule >= OPTRULE_T8A) && (recP->findingP->rule <= OPTRULE_T8D)));
                break;
            }
        }

        noteP = addNote(SN_REWRITE, stmtP);
        noteP->early = early;
        noteP->oldP = entryP->exprP;

        // A number relayout re-points holds only while nothing moves: T8d's
        // is written as the constant it loads, where that spelling gives the
        // same word, and a T3's from a label, which the plan chooses.
        if( recP && (recP->fate == OPTXF_FIRED) && recP->numberP && early )
        {
            noteP->numberP = recP->numberP;
            noteP->bank = entryP->bank;
            recP->numberP->flags |= SRC_NOTED;

            if( recP->findingP->rule == OPTRULE_T8D )
            {
                noteP->spellP = optFirstConstInner(entryP->exprP);

                if( noteP->spellP && ((recP->after & 010000) || !plainSpelling(noteP->spellP) ||
                                      ((evalExpr(noteP->spellP) & WRDMASK) != recP->numberP->value.ival)) )
                {
                    noteP->spellP = NILP;
                }
            }
            else
            {
                noteP->farNumber = true;
            }
        }
        quote(entryP->exprP, buf, sizeof(buf));
        snprintf(noteP->text, sizeof(noteP->text), "// am1 -O%d: %s, was %s", level, ruleP, buf);
    }
}

// Take each pool slot's first reference as parsed, before the transforms
// rewrite anything; called by optimize() only for -O=source.
void
srcSnapshotPools(void)
{
SymListP listP;
int count;

    for( count = 0, listP = constsListP; listP; listP = listP->nextP )
    {
        slotSize = 0;
        addSlots(listP->symP, NILP);    // counts only, while slotsP is NILP
        count += slotSize;
    }

    slotSize = 0;

    for( firstSize = 64; firstSize < (4 * count); firstSize *= 2 )
    {
    }

    if( !(firstsP = (SrcFirst *)calloc((size_t)firstSize, sizeof(SrcFirst))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    for( listP = constsListP; listP; listP = listP->nextP )
    {
        snapFirsts(listP->symP);
    }
}

// Enter one pool tree's slots and their first references in the snapshot.
static void
snapFirsts(SymNodeP symP)
{
unsigned int h;

    if( !symP )
    {
        return;
    }

    for( h = (unsigned int)(((uintptr_t)symP >> 4) & (unsigned int)(firstSize - 1)); firstsP[h].symP;
         h = (h + 1) & (unsigned int)(firstSize - 1) )
    {
    }

    firstsP[h].symP = symP;
    firstsP[h].firstP = symP->ptr;
    snapFirsts(symP->leftP);
    snapFirsts(symP->rightP);
}

// The expression of a slot's first reference as parsed.
// Returns it, or NILP for a slot the snapshot does not hold.
static void *
firstRefOf(SymNodeP symP)
{
unsigned int h;

    if( !firstsP || !symP )
    {
        return(NILP);
    }

    for( h = (unsigned int)(((uintptr_t)symP >> 4) & (unsigned int)(firstSize - 1)); firstsP[h].symP;
         h = (h + 1) & (unsigned int)(firstSize - 1) )
    {
        if( firstsP[h].symP == symP )
        {
            return(firstsP[h].firstP);
        }
    }

    return(NILP);
}

// Record a copy relayout made: its first and last nodes, and what it is, for
// the comment lines bracketing it.  Called only for -O=source.
void
srcNoteCopy(PNodeP firstP, PNodeP lastP, const char *whatP)
{
SrcNoteP noteP;

    if( !firstP || !lastP )
    {
        return;
    }

    noteP = addNote(SN_COPY, firstP);
    noteP->lastP = lastP;
    lastP->flags |= SRC_NOTED;
    snprintf(noteP->text, sizeof(noteP->text), "%s", whatP);
}

// Add a note for nodeP, after every note made before it, and flag the node.
// Returns the note; out of memory is fatal.
static SrcNoteP
addNote(SrcNoteKind kind, PNodeP nodeP)
{
SrcNoteP noteP;

    if( !(noteP = (SrcNoteP)calloc(1, sizeof(SrcNote))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    noteP->kind = kind;
    noteP->nodeP = nodeP;
    nodeP->flags |= SRC_NOTED;

    if( notesTailP )
    {
        notesTailP->nextP = noteP;
    }
    else
    {
        notesP = noteP;
    }

    notesTailP = noteP;
    return(noteP);
}

// Quote an expression for a note, on one line: tabs become blanks, and a
// comment a constant carries is left out.
static void
quote(PNodeP nodeP, char *outP, size_t size)
{
char *cP;

    outP[0] = '\0';
    captureP = outP;
    captureSize = size;
    inNote = true;
    srcOperand(nodeP);
    inNote = false;
    captureP = NILP;

    for( cP = outP; *cP; ++cP )
    {
        if( *cP == '\t' )
        {
            *cP = ' ';
        }
    }
}

// Add an annotation to those for the line being written; "%L" in it is the
// level.  They are written at the line's end, after any comment of its own.
static void
addPending(const char *textP)
{
char buf[SRC_NOTE + 32];
const char *atP;
size_t used;

    if( (atP = strstr(textP, "%L")) )
    {
        snprintf(buf, sizeof(buf), "%.*s%d%s", (int)(atP - textP), textP, level, atP + 2);
        textP = buf;
    }

    used = strlen(pending);

    if( (used + strlen(textP) + 4) < sizeof(pending) )
    {
        snprintf(pending + used, sizeof(pending) - used, "%s%s", (used)?"  ":"", textP);
    }
}

// The text layer.

// Tie each statement to the source line it came from.  Called by optimize()
// for -O=source before any rewrite, while the statement list stands as parsed.
// What the lexer read -- cpp's output, or the source itself under -n -- is read
// again; its markers give each line's file, line and #include, and the list's
// lines are paired with its lines in order.  A blank line is not paired: the
// lexer makes a node for some and not others, and copying gives them anyway.
// Sets textLayer when the map holds; otherwise whyTree says why it did not.
void
srcMapLines(PNodeP rootP)
{
SrcTextP physP;
SrcTextP textP;
PNodeP nodeP;
PNodeP lastP;
PNodeP scanP;
SrcGroupP groupP;
int *instAtP;           // per line read: its reading, and its line there
int *lineAtP;
char *kindP;            // 'x' before the title or the title, 'm' a '#' line,
                        // 'b' blank to the lexer, 'c' anything else
int stack[SRC_NEST];
int lineOf[SRC_NEST];   // the next line of each open reading
int depth;
int n;
int i;
int pi;
int first;
int count;
int lineNo;
int inst;
bool blank;
bool found;
bool enter;
bool leave;
bool titleSeen;
char name[PATH_MAX];
char needle[SRC_STRING];
static char why[SRC_NOTE + PATH_MAX];

    srcMode = optSourceMode();
    textLayer = false;
    whyTree = NILP;

    if( srcMode == SRC_MODE_TREE )
    {
        return;
    }

    if( !(physP = readText((doCpp)?pfilename:origFilenameP)) )
    {
        mapFail("what the lexer read could not be read again");
        return;
    }

    n = physP->count;
    instAtP = (int *)calloc((size_t)n + 1, sizeof(int));
    lineAtP = (int *)calloc((size_t)n + 1, sizeof(int));
    kindP = (char *)calloc((size_t)n + 1, 1);

    if( !instAtP || !lineAtP || !kindP )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    depth = 0;
    titleSeen = false;

    // Under -n there are no markers: every line is the source's.
    if( !doCpp )
    {
        stack[0] = newInst(origFilenameP, -1, 0);
        lineOf[0] = 1;
        depth = 1;
    }

    for( i = 0; i < n; ++i )
    {
        instAtP[i] = (depth)?stack[depth - 1]:-1;

        if( physP->linesPP[i][0] == '#' )
        {
            kindP[i] = (titleSeen)?'m':'x';

            if( !isMarkerLine(physP->linesPP[i], &lineNo, name, sizeof(name), &enter, &leave) )
            {
                // A '#' line cpp passed through, a line of its file all the
                // same; the lexer makes a marker node of it too.
                if( depth )
                {
                    lineAtP[i] = lineOf[depth - 1]++;
                }
                continue;
            }

            if( enter )
            {
                if( !depth || (depth >= SRC_NEST) )
                {
                    mapFail("an #include nested deeper than the map follows");
                    goto done;
                }
                stack[depth] = newInst(name, stack[depth - 1], lineOf[depth - 1]);
                lineOf[depth++] = lineNo;
            }
            else if( leave )
            {
                // The marker names the file returned to.
                while( (depth > 1) && strcmp(instsP[stack[depth - 1]].nameP, name) )
                {
                    --depth;
                }

                if( !depth || strcmp(instsP[stack[depth - 1]].nameP, name) )
                {
                    mapFail("a return from an #include the map cannot follow");
                    goto done;
                }
                lineOf[depth - 1] = lineNo;
            }
            else if( depth && !strcmp(instsP[stack[depth - 1]].nameP, name) )
            {
                lineOf[depth - 1] = lineNo;
            }
            else if( depth <= 1 )
            {
                // cpp's opening markers name the source, <built-in> and
                // <command-line> in turn, each a reading of its own.
                stack[0] = newInst(name, -1, 0);
                lineOf[0] = lineNo;
                depth = 1;
            }
            else
            {
                mapFail("a line marker the map cannot follow");
                goto done;
            }
            continue;
        }

        if( !depth )
        {
            mapFail("a line before cpp's first marker");
            goto done;
        }

        instAtP[i] = stack[depth - 1];
        lineAtP[i] = lineOf[depth - 1]++;

        if( !titleSeen )
        {
            // The first line that is not a marker is the title (the lexer's
            // FIRST state): it must be the source's own first line.
            titleSeen = true;
            kindP[i] = 'x';
            mainInst = instAtP[i];

            if( (lineAtP[i] != 1) || (instsP[mainInst].parent != -1) )
            {
                mapFail("the title is not the source's first line");
                goto done;
            }
            continue;
        }

        if( noteDecimal(physP->linesPP[i]) )
        {
            decimalSeen = true;
        }

        kindP[i] = (isBlankLine(physP->linesPP[i]))?'b':'c';
    }

    // Pair the list's lines with the lines read, in order.
    pi = 0;

    for( nodeP = rootP->leftP; nodeP; nodeP = lastP->leftP )
    {
        lastP = nodeP;

        if( nodeP->type == FILENAME )
        {
            while( (pi < n) && ((kindP[pi] == 'b') || (kindP[pi] == 'x')) )
            {
                ++pi;
            }

            if( (pi >= n) || (kindP[pi] != 'm') )
            {
                mapFail("a line marker in the statement list has no '#' line to pair with");
                goto done;
            }

            ++pi;
            continue;
        }

        // The line runs to its end, or to a marker cpp put inside it.
        count = 1;
        blank = ((nodeP->type == EMPTYLINE) || (nodeP->type == TERMINATOR));

        while( !isLineEnd(lastP) && lastP->leftP && (lastP->leftP->type != FILENAME) )
        {
            lastP = lastP->leftP;
            ++count;
            blank = (blank && ((lastP->type == EMPTYLINE) || (lastP->type == TERMINATOR)));
        }

        if( blank )
        {
            continue;
        }

        // A '#' line with no marker node of its own is passed over too.
        while( (pi < n) && (kindP[pi] != 'c') )
        {
            ++pi;
        }

        if( pi >= n )
        {
            mapFail("the statement list has more lines than the text");
            goto done;
        }

        first = pi;

        if( nodeP->type == CSCOMMENT )
        {
            while( (pi < n) && !strstr(physP->linesPP[pi], "*/") )
            {
                ++pi;
            }
        }
        else if( (nodeP->type == ORIGIN) && (count > 1) && !isLineEnd(nodeP->leftP) &&
                 originOnly(physP->linesPP[pi]) )
        {
            // An origin alone on its line: the lexer takes the next line's
            // statement into the same line of the list.
            for( ++pi; (pi < n) && (kindP[pi] != 'c'); ++pi )
            {
            }
        }

        if( (pi >= n) || (instAtP[pi] != instAtP[first]) )
        {
            mapFail("a line of the statement list spans two files");
            goto done;
        }

        // Each comment and label must be on the lines paired with it.
        for( scanP = nodeP; ; scanP = scanP->leftP )
        {
            found = true;

            if( (scanP->type == COMMENT) && (nodeP->type != CSCOMMENT) )
            {
                snprintf(needle, sizeof(needle), "//%s", scanP->value.strP);
                for( found = false, i = first; !found && (i <= pi); ++i )
                {
                    found = (strstr(physP->linesPP[i], needle) != NILP);
                }
            }
            else if( (scanP->type == LOCATION) || (scanP->type == LCLLOCATION) )
            {
                for( found = false, i = first; !found && (i <= pi); ++i )
                {
                    found = labelOn(physP->linesPP[i], scanP->value.symP->name);
                }
            }

            if( !found )
            {
                inst = instAtP[first];
                snprintf(why, sizeof(why), "the line map lost its place at line %d of %s",
                    lineAtP[first], (inst >= 0)?instsP[inst].nameP:"?");
                mapFail(why);
                goto done;
            }

            if( scanP == lastP )
            {
                break;
            }
        }

        if( groupCount >= groupSize )
        {
            groupSize = (groupSize)?(groupSize * 2):1024;
            if( !(groupsP = (SrcGroupP)realloc(groupsP, (size_t)groupSize * sizeof(SrcGroup))) )
            {
                fprintf(stderr, "am1: out of memory writing -O=source\n");
                exit(1);
            }
        }

        groupP = &groupsP[groupCount];
        memset(groupP, 0, sizeof(SrcGroup));
        groupP->inst = instAtP[first];
        groupP->first = lineAtP[first];
        groupP->last = lineAtP[pi];
        groupP->nodes = count;

        for( scanP = nodeP; ; scanP = scanP->leftP )
        {
            mapNode(scanP, groupCount);
            if( scanP == lastP )
            {
                break;
            }
        }

        ++groupCount;
        ++pi;
    }

    // Every reading a line came from, and its parents, must be read, and
    // each #include line must be where cpp's counting put it.
    for( i = 0; i < groupCount; ++i )
    {
        for( inst = groupsP[i].inst; inst >= 0; inst = instsP[inst].parent )
        {
            if( !loadInst(inst) )
            {
                goto done;
            }
        }

        // cpp joins a line that ends in a backslash to the next, and counts
        // the joined line as the first: the line the list holds runs on over
        // the rest, which a rewrite must replace along with it.
        if( doCpp )
        {
            textP = instsP[groupsP[i].inst].textP;
            while( (groupsP[i].last < textP->count) && continues(textP->linesPP[groupsP[i].last - 1]) )
            {
                ++groupsP[i].last;
            }
        }
    }

    textLayer = true;

done:
    free(instAtP);
    free(lineAtP);
    free(kindP);
}

// Give up the map: every line is written from the tree.
// Returns false, for the caller to return.
static bool
mapFail(const char *whyP)
{
    textLayer = false;

    if( !whyTree )
    {
        whyTree = whyP;
    }

    return(false);
}

// Read a file whole, once, as its lines.
// Returns it, or NILP when it cannot be read.
static SrcTextP
readText(const char *nameP)
{
SrcTextP textP;
FILE *fP;
long size;
char *cP;
int i;

    for( i = 0; i < textCount; ++i )
    {
        if( !strcmp(textsPP[i]->nameP, nameP) )
        {
            return(textsPP[i]);
        }
    }

    if( !(fP = fopen(nameP, "r")) )
    {
        return(NILP);
    }

    fseek(fP, 0L, SEEK_END);
    size = ftell(fP);
    fseek(fP, 0L, SEEK_SET);

    if( !(textP = (SrcTextP)calloc(1, sizeof(SrcText))) || (size < 0) ||
        !(textP->bufP = (char *)malloc((size_t)size + 1)) || !(textP->nameP = strdup(nameP)) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    size = (long)fread(textP->bufP, 1, (size_t)size, fP);
    textP->bufP[size] = '\0';
    fclose(fP);

    // A line per newline, and one more for a last line with none.
    for( textP->count = 0, cP = textP->bufP; *cP; ++cP )
    {
        if( *cP == '\n' )
        {
            ++textP->count;
        }
    }

    if( (size > 0) && (textP->bufP[size - 1] != '\n') )
    {
        ++textP->count;
    }

    if( !(textP->linesPP = (char **)calloc((size_t)textP->count + 1, sizeof(char *))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    for( i = 0, cP = textP->bufP; i < textP->count; ++i )
    {
        textP->linesPP[i] = cP;

        if( (cP = strchr(cP, '\n')) )
        {
            *cP++ = '\0';
        }
        else
        {
            cP = textP->bufP + size;
        }
    }

    if( !(textsPP = (SrcTextP *)realloc(textsPP, (size_t)(textCount + 1) * sizeof(SrcTextP))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    textsPP[textCount++] = textP;
    return(textP);
}

// Add a reading of a file, made by the #include on includeLine of parent.
// Returns its index.
static int
newInst(const char *nameP, int parent, int includeLine)
{
SrcInstP instP;

    if( instCount >= instSize )
    {
        instSize = (instSize)?(instSize * 2):64;
        if( !(instsP = (SrcInstP)realloc(instsP, (size_t)instSize * sizeof(SrcInst))) )
        {
            fprintf(stderr, "am1: out of memory writing -O=source\n");
            exit(1);
        }
    }

    instP = &instsP[instCount];
    memset(instP, 0, sizeof(SrcInst));

    if( !(instP->nameP = strdup(nameP)) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    instP->parent = parent;
    instP->includeLine = includeLine;
    return(instCount++);
}

// Read a reading's file, if not yet read, and check its #include line.
// Returns false, the map given up, when either cannot be done.
static bool
loadInst(int inst)
{
SrcInstP instP;
SrcInstP parentP;
char name[PATH_MAX];
char mainDir[PATH_MAX];
char dir[PATH_MAX];

    instP = &instsP[inst];

    if( instP->textP )
    {
        return(true);
    }

    if( (instP->nameP[0] == '<') || !(instP->textP = readText(instP->nameP)) )
    {
        return( mapFail("a file cpp read could not be read again") );
    }

    if( !(instP->skipP = (char *)calloc((size_t)instP->textP->count + 2, 1)) ||
        !(instP->childAtP = (int *)malloc(((size_t)instP->textP->count + 2) * sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    memset(instP->childAtP, 0xff, ((size_t)instP->textP->count + 2) * sizeof(int));

    if( instP->parent < 0 )
    {
        return(true);
    }

    if( !loadInst(instP->parent) )
    {
        return(false);
    }

    parentP = &instsP[instP->parent];

    if( (instP->includeLine < 1) || (instP->includeLine > parentP->textP->count) ||
        !isIncludeLine(parentP->textP->linesPP[instP->includeLine - 1], name, sizeof(name)) )
    {
        return( mapFail("an #include is not on the line cpp's markers put it") );
    }

    parentP->childAtP[instP->includeLine] = inst;

    // A quoted #include in a file from another directory than the source's
    // named its file from that directory, which the output does not share.
    dirOf(instsP[mainInst].nameP, mainDir, sizeof(mainDir));
    dirOf(instP->nameP, dir, sizeof(dir));
    instP->otherDir = (strcmp(mainDir, dir) != 0);
    return(true);
}

// The directory a file is in, as a real path where one can be made.
static void
dirOf(const char *pathP, char *dirP, size_t size)
{
char real[PATH_MAX];
char *cP;

    snprintf(dirP, size, "%s", pathP);

    if( (cP = strrchr(dirP, '/')) )
    {
        cP[(cP == dirP)?1:0] = '\0';
    }
    else
    {
        snprintf(dirP, size, ".");
    }

    if( realpath(dirP, real) )
    {
        snprintf(dirP, size, "%s", real);
    }
}

// Whether a node ends a line of the statement list.
static bool
isLineEnd(PNodeP nodeP)
{
    return( nodeP && ((nodeP->type == TERMINATOR) || (nodeP->type == EMPTYLINE) || (nodeP->type == COMMENT)) );
}

// Read a cpp line marker, '# N "file" flags': the line the next line is, the
// file, and whether it enters an #include (flag 1) or returns from one (2).
// Returns false for a '#' line that is not a marker.
static bool
isMarkerLine(const char *lineP, int *lineNoP, char *nameP, size_t size, bool *enterP, bool *leaveP)
{
const char *cP;
const char *endP;
size_t len;

    *enterP = *leaveP = false;

    for( cP = lineP + 1; (*cP == ' ') || (*cP == '\t'); ++cP )
    {
    }

    if( (*cP < '0') || (*cP > '9') )
    {
        return(false);
    }

    *lineNoP = atoi(cP);

    while( (*cP >= '0') && (*cP <= '9') )
    {
        ++cP;
    }

    while( (*cP == ' ') || (*cP == '\t') )
    {
        ++cP;
    }

    if( (*cP != '"') || !(endP = strchr(cP + 1, '"')) )
    {
        return(false);
    }

    len = (size_t)(endP - (cP + 1));
    if( len >= size )
    {
        len = size - 1;
    }

    memcpy(nameP, cP + 1, len);
    nameP[len] = '\0';

    for( cP = endP + 1; *cP; ++cP )
    {
        if( (*cP == '1') && ((cP[-1] == ' ') || (cP[-1] == '\t')) )
        {
            *enterP = true;
        }
        else if( (*cP == '2') && ((cP[-1] == ' ') || (cP[-1] == '\t')) )
        {
            *leaveP = true;
        }
    }

    return(true);
}

// Whether the lexer makes no statement of a line: blank, or a radix switch,
// which the lexer takes to itself.
static bool
isBlankLine(const char *lineP)
{
const char *cP;
size_t len;

    for( cP = lineP; (*cP == ' ') || (*cP == '\t') || (*cP == '\r'); ++cP )
    {
    }

    for( len = strlen(cP); (len > 0) && ((cP[len - 1] == ' ') || (cP[len - 1] == '\t') || (cP[len - 1] == '\r')); --len )
    {
    }

    return( (len == 0) || ((len == 5) && !strncmp(cP, "octal", 5)) || ((len == 7) && !strncmp(cP, "decimal", 7)) );
}

// Whether cpp joins a line to the next: it ends in a backslash, which gcc's
// cpp honors with blanks after it too.
static bool
continues(const char *lineP)
{
size_t len;

    for( len = strlen(lineP); (len > 0) && ((lineP[len - 1] == ' ') || (lineP[len - 1] == '\t') || (lineP[len - 1] == '\r')); --len )
    {
    }

    return( (len > 0) && (lineP[len - 1] == '\\') );
}

// Whether a line is an #include, and the name it gives, as written.
static bool
isIncludeLine(const char *lineP, char *nameP, size_t size)
{
const char *cP;
const char *endP;
size_t len;

    for( cP = lineP; (*cP == ' ') || (*cP == '\t'); ++cP )
    {
    }

    if( *cP++ != '#' )
    {
        return(false);
    }

    while( (*cP == ' ') || (*cP == '\t') )
    {
        ++cP;
    }

    if( strncmp(cP, "include", 7) )
    {
        return(false);
    }

    for( cP += 7; (*cP == ' ') || (*cP == '\t'); ++cP )
    {
    }

    nameP[0] = '\0';

    if( ((*cP == '"') && (endP = strchr(cP + 1, '"'))) || ((*cP == '<') && (endP = strchr(cP + 1, '>'))) )
    {
        len = (size_t)(endP - cP) + 1;
        if( len >= size )
        {
            len = size - 1;
        }
        memcpy(nameP, cP, len);
        nameP[len] = '\0';
    }

    return(true);
}

// Whether a line holds an origin and nothing more, 'x/' with blanks around it.
static bool
originOnly(const char *lineP)
{
const char *cP;
const char *slashP;

    for( cP = lineP; (*cP == ' ') || (*cP == '\t'); ++cP )
    {
    }

    if( !*cP || (*cP == '/') || !(slashP = strchr(cP, '/')) )
    {
        return(false);
    }

    for( cP = slashP + 1; (*cP == ' ') || (*cP == '\t') || (*cP == '\r'); ++cP )
    {
    }

    return( *cP == '\0' );
}

// Whether a line defines a label: its name, not inside a longer name, and a
// comma after it.
static bool
labelOn(const char *lineP, const char *nameP)
{
const char *cP;
const char *afterP;
size_t len;

    len = strlen(nameP);

    for( cP = strstr(lineP, nameP); cP; cP = strstr(cP + 1, nameP) )
    {
        if( (cP > lineP) && (isalnum((unsigned char)cP[-1]) || (cP[-1] == '_')) )
        {
            continue;
        }

        for( afterP = cP + len; (*afterP == ' ') || (*afterP == '\t'); ++afterP )
        {
        }

        if( *afterP == ',' )
        {
            return(true);
        }
    }

    return(false);
}

// Whether a line switches the lexer to decimal: the word, outside a comment.
static bool
noteDecimal(const char *lineP)
{
const char *cP;
const char *endP;

    endP = strstr(lineP, "//");

    for( cP = strstr(lineP, "decimal"); cP && (!endP || (cP < endP)); cP = strstr(cP + 1, "decimal") )
    {
        if( ((cP == lineP) || !(isalnum((unsigned char)cP[-1]) || (cP[-1] == '_'))) &&
            !(isalnum((unsigned char)cP[7]) || (cP[7] == '_')) )
        {
            return(true);
        }
    }

    return(false);
}

// Enter a node's line in the map.
static void
mapNode(PNodeP nodeP, int group)
{
SrcMapItem *oldP;
int oldSize;
int i;
unsigned int h;

    if( (mapCount + 1) * 2 >= mapSize )
    {
        // Rehash into four times the room, keeping it at most half full.
        oldP = mapP;
        oldSize = mapSize;
        mapSize = (mapSize)?(mapSize * 4):4096;

        if( !(mapP = (SrcMapItem *)calloc((size_t)mapSize, sizeof(SrcMapItem))) )
        {
            fprintf(stderr, "am1: out of memory writing -O=source\n");
            exit(1);
        }

        for( i = 0; i < oldSize; ++i )
        {
            if( oldP[i].nodeP )
            {
                for( h = (unsigned int)(((uintptr_t)oldP[i].nodeP >> 4) & (unsigned int)(mapSize - 1)); mapP[h].nodeP;
                     h = (h + 1) & (unsigned int)(mapSize - 1) )
                {
                }
                mapP[h] = oldP[i];
            }
        }

        free(oldP);
    }

    for( h = (unsigned int)(((uintptr_t)nodeP >> 4) & (unsigned int)(mapSize - 1)); mapP[h].nodeP;
         h = (h + 1) & (unsigned int)(mapSize - 1) )
    {
    }

    mapP[h].nodeP = nodeP;
    mapP[h].group = group;
    ++mapCount;
}

// The line of the list as parsed that a node was in.
// Returns its index, or -1 for a node the map does not hold.
static int
groupOf(PNodeP nodeP)
{
unsigned int h;

    if( !mapP )
    {
        return(-1);
    }

    for( h = (unsigned int)(((uintptr_t)nodeP >> 4) & (unsigned int)(mapSize - 1)); mapP[h].nodeP;
         h = (h + 1) & (unsigned int)(mapSize - 1) )
    {
        if( mapP[h].nodeP == nodeP )
        {
            return(mapP[h].group);
        }
    }

    return(-1);
}

// The copy a node begins, if it begins one.
static SrcNoteP
copyOf(PNodeP nodeP)
{
SrcNoteP noteP;

    for( noteP = (nodeP->flags & SRC_NOTED)?notesP:NILP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->nodeP == nodeP) && (noteP->kind == SN_COPY) )
        {
            return(noteP);
        }
    }

    return(NILP);
}

// Where a stretch written where it now stands ends: a moved run's end marker,
// or a copy's last node.
// Returns it, or NILP when nodeP begins no such stretch.
static PNodeP
rangeEnd(PNodeP nodeP)
{
SrcNoteP noteP;
PNodeP endP;

    if( (nodeP->type == RELAYOUT) && (nodeP->value.ival == RL_BEGIN) )
    {
        for( endP = nodeP->leftP; endP; endP = endP->leftP )
        {
            if( (endP->type == RELAYOUT) && (endP->value.ival == RL_END) )
            {
                return(endP);
            }
        }
        return(nodeP);
    }

    if( (noteP = copyOf(nodeP)) )
    {
        return(noteP->lastP);
    }

    return(NILP);
}

// The last node of the line starting at nodeP: its end, or the node before a
// marker; a moved run or a copy inside the line is passed over whole.
static PNodeP
unitEnd(PNodeP nodeP)
{
PNodeP endP;

    for( ; ; nodeP = nodeP->leftP )
    {
        if( (endP = rangeEnd(nodeP)) )
        {
            nodeP = endP;
        }

        if( isLineEnd(nodeP) || !nodeP->leftP || (nodeP->leftP->type == FILENAME) )
        {
            return(nodeP);
        }
    }
}

// Take the next stretch of the optimized list, from nodeP, into unitP.
// Returns the node after it.
static PNodeP
nextUnit(PNodeP nodeP, SrcUnit *unitP)
{
PNodeP scanP;
PNodeP endP;
bool blank;
int group;
int count;

    memset(unitP, 0, sizeof(SrcUnit));
    unitP->firstP = nodeP;
    unitP->group = -1;

    // A copy may begin with a marker of its own, so this comes first.
    if( (endP = rangeEnd(nodeP)) || ((nodeP->type == RELAYOUT) && (nodeP->value.ival != RL_DELETED)) )
    {
        unitP->kind = SU_OTHER;
        unitP->lastP = (endP)?endP:nodeP;
        return(unitP->lastP->leftP);
    }

    if( nodeP->type == FILENAME )
    {
        unitP->kind = SU_PASS;
        unitP->lastP = nodeP;
        return(nodeP->leftP);
    }

    unitP->lastP = unitEnd(nodeP);

    // Its line is the first line the map knows among its own nodes; it is
    // copied only when it is all of that line, unchanged.
    blank = true;
    count = 0;
    unitP->copyable = (srcMode == SRC_MODE_TEXT);

    for( scanP = nodeP; ; scanP = scanP->leftP )
    {
        if( (endP = rangeEnd(scanP)) )
        {
            unitP->copyable = false;
            blank = false;
            scanP = endP;
        }
        else
        {
            group = groupOf(scanP);
            ++count;
            blank = (blank && ((scanP->type == EMPTYLINE) || (scanP->type == TERMINATOR)));

            if( (unitP->group < 0) && (group >= 0) )
            {
                unitP->group = group;
            }

            if( (group < 0) || (group != unitP->group) || (scanP->type == RELAYOUT) || rewriteOf(scanP) )
            {
                unitP->copyable = false;
            }
        }

        if( scanP == unitP->lastP )
        {
            break;
        }
    }

    // Nothing but line ends is never a line of the map, which passes such
    // lines over; one that maps is the tail of a line a rewrite split, where
    // the new word's comment ended the line first.  The rest of that line is
    // not all of it, so it is written from the tree, and this adds nothing.
    if( blank )
    {
        unitP->kind = SU_PASS;
        return(unitP->lastP->leftP);
    }

    if( unitP->group < 0 )
    {
        unitP->kind = SU_OTHER;
        return(unitP->lastP->leftP);
    }

    unitP->kind = SU_LINE;

    if( count != groupsP[unitP->group].nodes )
    {
        unitP->copyable = false;
    }

    return(unitP->lastP->leftP);
}

// A reading, and every reading around it, is written out in full.
static void
markChanged(int inst)
{
    for( ; inst >= 0; inst = instsP[inst].parent )
    {
        instsP[inst].changed = true;
    }
}

// A line of the list as parsed is not copied where it was.
static void
markGone(int group)
{
int line;

    for( line = groupsP[group].first; line <= groupsP[group].last; ++line )
    {
        instsP[groupsP[group].inst].skipP[line] = 1;
    }

    groupsP[group].seen = true;
    markChanged(groupsP[group].inst);
}

// Every line the map knows among firstP to lastP, other than group, is written
// elsewhere, so none is copied where it was.
static void
markInside(PNodeP firstP, PNodeP lastP, int group)
{
PNodeP nodeP;
int other;

    for( nodeP = firstP; nodeP; nodeP = nodeP->leftP )
    {
        if( ((other = groupOf(nodeP)) >= 0) && (other != group) )
        {
            markGone(other);
        }

        if( nodeP == lastP )
        {
            break;
        }
    }
}

// The innermost reading holding both a and b.
static int
commonReading(int a, int b)
{
int inst;

    for( ; a >= 0; a = instsP[a].parent )
    {
        for( inst = b; inst >= 0; inst = instsP[inst].parent )
        {
            if( inst == a )
            {
                return(a);
            }
        }
    }

    return(mainInst);
}

// Whether inst is outerInst or read from inside it.
static bool
isWithin(int inst, int outerInst)
{
    for( ; inst >= 0; inst = instsP[inst].parent )
    {
        if( inst == outerInst )
        {
            return(true);
        }
    }

    return(false);
}

// Decide what the walk copies and what it writes out: a line rendered, moved
// or gone is not copied, and its reading is written out in full, as is any
// reading something is written into, between two of its own lines.
static void
textAnalyze(PNodeP firstP)
{
PNodeP nodeP;
SrcUnit unit;
SrcNoteP noteP;
int prev;
int i;
bool inserted;

    prev = -1;
    inserted = false;

    for( nodeP = firstP; nodeP; )
    {
        nodeP = nextUnit(nodeP, &unit);

        switch( unit.kind )
        {
        case SU_PASS:
            // The keeper plan starts a line at a marker, so keepers can hang
            // on one: they are written where it stands.
            if( unit.firstP->flags & SRC_NOTED )
            {
                inserted = true;
            }
            break;

        case SU_OTHER:
            markInside(unit.firstP, unit.lastP, -1);
            inserted = true;
            break;

        case SU_LINE:
            groupsP[unit.group].seen = true;
            markInside(unit.firstP, unit.lastP, unit.group);

            if( !unit.copyable )
            {
                markGone(unit.group);
            }

            // A keeper is written just before the line it serves.
            for( noteP = (unit.firstP->flags & SRC_NOTED)?notesP:NILP; noteP; noteP = noteP->nextP )
            {
                if( (noteP->nodeP == unit.firstP) && (noteP->kind == SN_KEEPER) )
                {
                    markChanged(groupsP[unit.group].inst);
                }
            }

            if( inserted )
            {
                markChanged((prev >= 0)?commonReading(prev, groupsP[unit.group].inst):groupsP[unit.group].inst);
                inserted = false;
            }

            prev = groupsP[unit.group].inst;
            break;
        }
    }

    // A line the optimized list holds nowhere is gone.
    for( i = 0; i < groupCount; ++i )
    {
        if( !groupsP[i].seen )
        {
            markGone(i);
        }
    }

    markChanged(mainInst);
}

// Walk the optimized list and write it: each line in place copied from its
// file, or rendered, with the text between copied as it stands; each stretch
// written where it now stands rendered there.  An include with no change is
// its #include line; one with a change is written out in place.  With dryRun
// set nothing is written, and the walk proves the text can give this order.
// Returns false when it cannot: a line whose place the text has passed.
static bool
textWalk(PNodeP firstP)
{
PNodeP nodeP;
SrcUnit unit;
SrcGroupP groupP;
SrcInstP instP;
int inst;
int kept;
int line;
int i;

    walkFailed = false;

    for( i = 0; i < instCount; ++i )
    {
        instsP[i].kept = false;
        instsP[i].opened = false;
        instsP[i].cursor = 1;
    }

    // The title is written already.
    curInst = mainInst;
    instsP[mainInst].opened = true;
    instsP[mainInst].cursor = 2;

    for( nodeP = firstP; nodeP && !walkFailed; )
    {
        nodeP = nextUnit(nodeP, &unit);

        if( unit.kind == SU_PASS )
        {
            if( !dryRun && (unit.firstP->flags & SRC_NOTED) )
            {
                startLine();
                srcNotesBefore(unit.firstP);
            }
            continue;
        }

        if( unit.kind == SU_OTHER )
        {
#ifdef SRCCODEGEN_UNMOVE
            // A test build only: each moved run is written where the source
            // had it, so a test can confirm it sees the tape change.
            if( (unit.firstP->type == RELAYOUT) && (unit.firstP->value.ival == RL_BEGIN) )
            {
                continue;
            }

            if( !dryRun && (unit.firstP->type == RELAYOUT) && (unit.firstP->value.ival == RL_MOVEDFROM) &&
                unit.firstP->value2.ptr )
            {
                startLine();
                srcRange(((PNodeP)(unit.firstP->value2.ptr))->leftP, rangeEnd((PNodeP)(unit.firstP->value2.ptr)));
                continue;
            }
#endif
            if( !dryRun )
            {
                startLine();
                srcRange(unit.firstP, unit.lastP);
            }
            continue;
        }

        groupP = &groupsP[unit.group];

        // Inside an include with no change: its #include line stands for it.
        for( kept = -1, inst = groupP->inst; (inst >= 0) && !instsP[inst].changed; inst = instsP[inst].parent )
        {
            kept = inst;
        }

        if( kept >= 0 )
        {
            if( !unit.copyable )
            {
                walkFailed = true;
                break;
            }

            if( !instsP[kept].kept )
            {
                moveTo(instsP[kept].parent);
                flushTo(instsP[kept].parent, instsP[kept].includeLine + 1);

                if( !instsP[kept].kept )
                {
                    walkFailed = true;
                }
            }
            continue;
        }

        moveTo(groupP->inst);
        instP = &instsP[groupP->inst];

        if( walkFailed || (groupP->first < instP->cursor) )
        {
            walkFailed = true;
            break;
        }

        flushTo(groupP->inst, groupP->first);

        if( !dryRun )
        {
            startLine();

            if( unit.copyable )
            {
                // Its keepers go ahead of it; nothing else is noted on it.
                if( unit.firstP->flags & SRC_NOTED )
                {
                    srcNotesBefore(unit.firstP);
                }

                for( line = groupP->first; line <= groupP->last; ++line )
                {
                    put(instP->textP->linesPP[line - 1]);
                    put("\n");
                }
            }
            else
            {
                if( srcMode == SRC_MODE_TEXT )
                {
                    for( line = groupP->first; line <= groupP->last; ++line )
                    {
                        wasLine(instP->textP->linesPP[line - 1]);
                    }
                }

                srcRange(unit.firstP, unit.lastP);
                startLine();
            }
        }

        instP->cursor = groupP->last + 1;
    }

    if( !walkFailed )
    {
        moveTo(mainInst);
        flushTo(mainInst, instsP[mainInst].textP->count + 1);
    }

    return(!walkFailed);
}

// Make inst the reading being written: close each reading the walk is in that
// does not hold it, then open each one down to it.
static void
moveTo(int inst)
{
int chain[SRC_NEST];
int depth;
int i;

    while( !walkFailed && !isWithin(inst, curInst) )
    {
        closeInst(curInst);
    }

    for( depth = 0, i = inst; (i >= 0) && (i != curInst) && (depth < SRC_NEST); i = instsP[i].parent )
    {
        chain[depth++] = i;
    }

    while( !walkFailed && (depth-- > 0) )
    {
        // Written out once only, and only where its #include line still is.
        if( instsP[chain[depth]].opened ||
            (instsP[instsP[chain[depth]].parent].cursor > instsP[chain[depth]].includeLine) )
        {
            walkFailed = true;
            return;
        }

        flushTo(instsP[chain[depth]].parent, instsP[chain[depth]].includeLine);
        openInst(chain[depth]);
    }
}

// Begin writing an include out in place of its #include line.
static void
openInst(int inst)
{
SrcInstP instP;
SrcInstP parentP;

    instP = &instsP[inst];
    parentP = &instsP[instP->parent];

    if( !dryRun )
    {
        startLine();
        put("// am1 -O=source: ");
        put(instP->nameP);
        put(" follows, written out in place of:  ");
        put(parentP->textP->linesPP[instP->includeLine - 1]);
        put("\n");
    }

    parentP->cursor = instP->includeLine + 1;
    instP->opened = true;
    instP->cursor = 1;
    curInst = inst;
}

// End an include written out in place: the rest of its text, then back to
// the reading that included it.
static void
closeInst(int inst)
{
    flushTo(inst, instsP[inst].textP->count + 1);

    if( !dryRun )
    {
        startLine();
        put("// am1 -O=source: the end of ");
        put(instsP[inst].nameP);
        put("\n");
    }

    curInst = instsP[inst].parent;
}

// Copy a reading's lines from its cursor up to line, less the lines not
// copied.  An #include line on the way is copied when its file has no change,
// or its file written out in its place when it has one.
static void
flushTo(int inst, int line)
{
SrcInstP instP;
int child;
int saved;
int at;

    instP = &instsP[inst];

    while( !walkFailed && (instP->cursor < line) )
    {
        at = instP->cursor++;

        if( instP->skipP[at] )
        {
            continue;
        }

        child = instP->childAtP[at];

        if( (child >= 0) && instsP[child].changed )
        {
            // A changed include none of whose lines stands in place.
            if( instsP[child].opened )
            {
                continue;
            }

            saved = curInst;
            curInst = inst;
            openInst(child);
            closeInst(child);
            curInst = saved;
            instP->cursor = at + 1;
            continue;
        }

        if( child >= 0 )
        {
            instsP[child].kept = true;
        }

        copyLine(inst, at);
    }
}

// Copy one line of a reading.  A quoted #include in a file written out from
// another directory is written with the path its file was found by.
static void
copyLine(int inst, int line)
{
SrcInstP instP;
const char *lineP;
char name[PATH_MAX];
char path[2 * PATH_MAX];
char real[PATH_MAX];
char *cP;

    if( dryRun )
    {
        return;
    }

    instP = &instsP[inst];
    lineP = instP->textP->linesPP[line - 1];

    if( (inst != mainInst) && instP->otherDir && isIncludeLine(lineP, name, sizeof(name)) &&
        (name[0] == '"') && (name[1] != '/') )
    {
        snprintf(path, sizeof(path), "%s", instP->nameP);
        cP = strrchr(path, '/');
        snprintf((cP)?(cP + 1):path, sizeof(path) - (size_t)((cP)?(cP + 1 - path):0), "%.*s",
            (int)(strlen(name) - 2), name + 1);

        if( realpath(path, real) )
        {
            startLine();
            putf("#include \"%s\"", real);
            put("  // am1 -O=source: was ");
            put(name);
            put(", found beside ");
            put(instP->nameP);
            put("\n");
            return;
        }
    }

    startLine();
    put(lineP);
    put("\n");
}

// Write a comment quoting a source line a rendered line replaces.  Blanks at
// its end go, and a backslash there, which would join the next line to it.
static void
wasLine(const char *lineP)
{
size_t len;

    for( len = strlen(lineP); (len > 0) && ((lineP[len - 1] == ' ') || (lineP[len - 1] == '\t') ||
                                           (lineP[len - 1] == '\r') || (lineP[len - 1] == '\\')); --len )
    {
    }

    startLine();
    putf("// am1 -O%d: was: ", level);
    fwrite(lineP, 1, len, srcfP);
    put("\n");
}

// The keeper plan.

// Find the lines that need a keeper, before anything is written: walk the
// statements as they will be written, first counting every constant reference,
// then filing each key as a plain assembly would, and adding a keeper where a
// key would be filed before one of its ancestors in the optimized pool, or
// where a rewrite stopped naming a word the pool still holds.
static void
planKeepers(PNodeP rootP)
{
SymListP listP;
int count;
int i;
bool lost;

    for( count = 0, listP = constsListP; listP; listP = listP->nextP )
    {
        slotSize = 0;
        addSlots(listP->symP, NILP);    // counts only, while slotsP is NILP
        count += slotSize;
    }

    for( slotSize = 64; slotSize < (4 * count); slotSize *= 2 )
    {
    }

    if( !(slotsP = (SrcSlotP)calloc((size_t)slotSize, sizeof(SrcSlot))) )
    {
        fprintf(stderr, "am1: out of memory writing -O=source\n");
        exit(1);
    }

    for( listP = constsListP; listP; listP = listP->nextP )
    {
        addSlots(listP->symP, NILP);
    }

    for( planPass = 1; planPass <= 2; ++planPass )
    {
        planLine = 0;
        planAtBol = true;
        planHeadP = NILP;
        planSerial = 0;
        planStatements(rootP->leftP);
    }

    // A word of a pool no 'constants' directive closed, which nothing names and
    // no rewrite stood where it was named.  Not expected: it would be lost.
    for( lost = false, i = 0; i < slotSize; ++i )
    {
        if( slotsP[i].symP && !slotsP[i].filed )
        {
            fprintf(stderr, "am1: -O=source cannot place the pool word keyed %s\n", slotsP[i].symP->name);
            lost = true;
        }
    }

    failed |= lost;

    for( i = 0; i < slotSize; ++i )
    {
        if( slotsP[i].symP && !slotsP[i].refs && !keyHolds(&slotsP[i]) )
        {
            ++unkeyed;
        }
    }

    if( unkeyed )
    {
        fprintf(stderr, "am1: -O=source: %d pool word%s nothing names keep%s a key the source cannot give "
            "again; this file assembles to another pool order\n", unkeyed, (unkeyed == 1)?"":"s",
            (unkeyed == 1)?"s":"");
    }
}

// The key a plain assembly gives a slot's expression now, where relayout kept
// the parser's key for a word nothing names: an address its edits moved.
// Returns the key's text in keyP; a key read from a serial number is the
// parser's own.
static void
keyNow(SrcSlotP slotP, char *keyP, size_t size)
{
long key;

    key = strtol(slotP->symP->name, NILP, 10);

    if( !slotP->refs && (key >= 0) && (key < (1L << 22)) && slotP->symP->ptr )
    {
        snprintf(keyP, size, "%ld", hashExpr((PNodeP)(slotP->symP->ptr)));
    }
    else
    {
        snprintf(keyP, size, "%s", slotP->symP->name);
    }
}

// Whether an orphan slot's key, as a plain assembly makes it now, still sorts
// against every ancestor and descendant as the kept key does: then filed after
// its ancestors it lands where the optimized pool has it.
static bool
keyHolds(SrcSlotP slotP)
{
char now[48];
char other[48];
SymNodeP symP;
SrcSlotP upP;
int depth;

    keyNow(slotP, now, sizeof(now));

    if( !strcmp(now, slotP->symP->name) )
    {
        return(true);
    }

    for( depth = 0, symP = slotP->parentP; symP && (depth < SRC_DEPTH); symP = upP->parentP, ++depth )
    {
        upP = findSlot(symP);
        keyNow(upP, other, sizeof(other));

        if( !sameSide(strcmp(slotP->symP->name, symP->name), strcmp(now, other)) )
        {
            return(false);
        }
    }

    return( subtreeHolds(slotP->symP->leftP, slotP, now) && subtreeHolds(slotP->symP->rightP, slotP, now) );
}

// Whether every key under a slot sorts against its new key as against its old.
static bool
subtreeHolds(SymNodeP symP, SrcSlotP topP, const char *nowP)
{
char other[48];

    if( !symP )
    {
        return(true);
    }

    keyNow(findSlot(symP), other, sizeof(other));

    return( sameSide(strcmp(symP->name, topP->symP->name), strcmp(other, nowP)) &&
            subtreeHolds(symP->leftP, topP, nowP) && subtreeHolds(symP->rightP, topP, nowP) );
}

// Whether two comparisons came out on the same side, equal counting as its own.
static bool
sameSide(int a, int b)
{
    return( ((a < 0) && (b < 0)) || ((a > 0) && (b > 0)) || ((a == 0) && (b == 0)) );
}

// Walk the statement list as srcStatements() will write it, one pass.
static void
planStatements(PNodeP nodeP)
{
SrcNoteP noteP;

    for( ; nodeP; nodeP = nodeP->leftP )
    {
        if( planAtBol )
        {
            planHeadP = nodeP;
            ++planLine;
            planAtBol = false;
        }

        // A rewritten statement's note, if it has one.
        for( noteP = (nodeP->flags & SRC_NOTED)?notesP:NILP; noteP; noteP = noteP->nextP )
        {
            if( (noteP->nodeP == nodeP) && (noteP->kind == SN_REWRITE) )
            {
                break;
            }
        }

        switch( nodeP->type )
        {
        case TERMINATOR:
        case EMPTYLINE:
        case COMMENT:
            planAtBol = true;
            break;

        case ORIGIN:
            planRefs(nodeP->exprP);
            break;

        case LOCATION:
        case LCLLOCATION:
            // The label is met first; the rest of the line is the statement's.
            if( planPass == 2 )
            {
                planMention(nodeP->value.symP);
            }
            // fall through

        case EXPR:
            if( !noteP || (planPass == 1) )
            {
                planRefs(nodeP->rightP);
            }
            else if( noteP->early )
            {
                // The new expression is written first, then what the old one
                // created.  A T3's target named where the source never named
                // it would be created too soon, so it is written as its word.
                noteP->asWord = (!strncmp(noteP->text + 12, "T3,", 3) && createsEarly(nodeP->rightP));

                if( !noteP->asWord )
                {
                    planRefs(nodeP->rightP);
                }

                planCreation(noteP, noteP->oldP);
            }
            else
            {
                // A rewrite relayout made: its model already dropped what the
                // old expression created, and a word it orphaned stays here.
                planRemoved(noteP->oldP);
                planRefs(nodeP->rightP);
            }
            break;

        case TABLE:
            planRefs(nodeP->exprP);
            planRefs(nodeP->rightP);
            break;

        case VAR:
            planVars(nodeP->rightP);
            break;

        case CONSTANTS:
            if( planPass == 2 )
            {
                planClose(nodeP->value.symP);
            }
            break;

        case RELAYOUT:
            if( (nodeP->value.ival == RL_DELETED) && (planPass == 2) )
            {
                planRemoved(nodeP->exprP);
            }
            break;

        default:
            break;
        }
    }
}

// Plan a var statement's initializers in the order written: the parser chains
// the names last first through rightP.
static void
planVars(PNodeP nodeP)
{
    if( nodeP )
    {
        planVars(nodeP->rightP);
        planMention(nodeP->value.symP);
        planRefs(nodeP->leftP);
    }
}

// Plan the constant references in an expression, in the order the parser files
// their keys: a constant's own expression first, since it is reduced first.
// Pass 1 counts them; pass 2 files each key, after keepers for any of its
// ancestors not yet filed.
static void
planRefs(PNodeP nodeP)
{
SrcSlotP slotP;
SymNodeP chainPP[SRC_DEPTH];
SymNodeP symP;
SrcNoteP noteP;
int depth;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        planRefs(nodeP->leftP);
        planRefs(nodeP->rightP);
        break;

    case UNOP:
        planRefs(nodeP->rightP);
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
        planMention(nodeP->value.symP);
        break;

    case INTEGER:
        // A re-pointed number is written as what names it.
        if( (nodeP->flags & SRC_NOTED) && (noteP = numberNoteOf(nodeP)) )
        {
            if( noteP->spellP )
            {
                planRefs(noteP->spellP);
            }
            else if( noteP->farNumber && (planPass == 2) )
            {
                chooseLabel(noteP);
            }
        }
        break;

    case CONSTANT:
        planRefs((nodeP->value2.ptr)?(PNodeP)(nodeP->value2.ptr):(PNodeP)(nodeP->value.symP->ptr));

        if( !(slotP = findSlot(nodeP->value.symP)) )
        {
            break;
        }

        if( planPass == 1 )
        {
            ++slotP->refs;
            break;
        }

        planMention(slotP->symP);

        if( slotP->filed )
        {
            break;
        }

        // An ancestor filed earlier on this line precedes this reference; one
        // not filed at all gets a keeper ahead of the line.
        for( depth = 0, symP = slotP->parentP; symP && (depth < SRC_DEPTH); symP = findSlot(symP)->parentP )
        {
            chainPP[depth++] = symP;
        }

        while( depth-- > 0 )
        {
            if( !findSlot(chainPP[depth])->filed )
            {
                keeper(findSlot(chainPP[depth]), (PNodeP)(chainPP[depth]->ptr), false);
            }
        }

        slotP->filed = true;
        slotP->early = false;
        slotP->line = planLine;
        break;

    default:
        break;
    }
}

// Plan what a T3's or T8's old expression created, in the order the parser
// met it: relayout's model of the pool keys counts those creations as still
// made, so each is made again after the new expression, on the same line.  A
// symbol not yet met is named; a constant whose pool word it first named is
// named again, or, when the pool word has since been reclaimed, a symbol is
// made and dropped in its place.
static void
planCreation(SrcNoteP noteP, PNodeP nodeP)
{
SrcSlotP slotP;
SymNodeP symP;
PNodeP innerP;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        planCreation(noteP, nodeP->leftP);
        planCreation(noteP, nodeP->rightP);
        break;

    case UNOP:
        planCreation(noteP, nodeP->rightP);
        break;

    case ADDR:
    case LCLADDR:
    case BREF:
        if( nodeP->value.symP->serialNumber > planSerial )
        {
            addItem(noteP, SI_MENTION, nodeP);
            planMention(nodeP->value.symP);
        }
        break;

    case CONSTANT:
        innerP = (nodeP->value2.ptr)?(PNodeP)(nodeP->value2.ptr):(PNodeP)(nodeP->value.symP->ptr);
        planCreation(noteP, innerP);
        symP = nodeP->value.symP;
        slotP = findSlot(symP);

        if( !nodeP->value2.ptr || (nodeP->value2.ptr != firstRefOf(symP)) )
        {
            // Not its creation; a word nothing else names is still kept here.
            if( slotP && !slotP->refs && !slotP->filed )
            {
                keeper(slotP, innerP, true);
            }
            break;
        }

        if( !slotP )
        {
            addItem(noteP, SI_PHANTOM, nodeP);
        }
        else if( !slotP->filed )
        {
            if( slotP->parentP && !findSlot(slotP->parentP)->filed )
            {
                // Its ancestors must be filed first: all go ahead of the line.
                keeper(slotP, innerP, !slotP->refs);
            }
            else
            {
                addItem(noteP, SI_CONSTANT, innerP);
                slotP->filed = true;
                slotP->early = false;
                slotP->line = planLine;
            }
        }

        planMention(symP);
        break;

    default:
        break;
    }
}

// The rewrite note whose re-pointed number is nodeP.
static SrcNoteP
numberNoteOf(PNodeP nodeP)
{
SrcNoteP noteP;

    for( noteP = notesP; noteP; noteP = noteP->nextP )
    {
        if( (noteP->kind == SN_REWRITE) && (noteP->numberP == nodeP) )
        {
            return(noteP);
        }
    }

    return(NILP);
}

// Whether a constant's expression can stand in a word as it is: no '.', which
// would read the word's own location, and no constant of its own.
static bool
plainSpelling(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(true);
    }

    switch( nodeP->type )
    {
    case BINOP:
        return( plainSpelling(nodeP->leftP) && plainSpelling(nodeP->rightP) );

    case UNOP:
        return( plainSpelling(nodeP->rightP) );

    case ADDR:
    case LCLADDR:
    case BREF:
    case INTEGER:
    case CHAR:
    case FLEXO:
    case LITCHAR:
        return(true);

    default:
        return(false);
    }
}

// Choose the label a T3's far target is written from: the nearest global
// label of the word's bank at or below the address, within SRC_REACH, and one
// the program has already named, since naming it here first would make it
// sooner than the source did.  None found leaves the number, annotated.
static void
chooseLabel(SrcNoteP noteP)
{
BankContextP bankP;

    noteP->labelP = NILP;

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        if( bankP->bank == noteP->bank )
        {
            nearestLabel(bankP->globalSymP, noteP, noteP->numberP->value.ival);
        }
    }
}

// Keep in noteP the nearest label under symP to address, per chooseLabel().
static void
nearestLabel(SymNodeP symP, SrcNoteP noteP, int address)
{
int offset;

    for( ; symP; symP = symP->rightP )
    {
        nearestLabel(symP->leftP, noteP, address);

        offset = address - symP->value;

        if( (symP->flags & SYMF_RESOLVED) && ((symP->flags & SYM_MASK) == SYM_GLOB) && !(symP->flags & SYMF_VAR) &&
            (symP->serialNumber <= planSerial) && (offset >= 0) && (offset < SRC_REACH) &&
            (!noteP->labelP || (offset < noteP->offset) ||
             ((offset == noteP->offset) && (strcmp(symP->name, noteP->labelP->name) < 0))) )
        {
            noteP->labelP = symP;
            noteP->offset = offset;
        }
    }
}

// Whether an expression names a symbol the plan walk has not yet met.
static bool
createsEarly(PNodeP nodeP)
{
    if( !nodeP )
    {
        return(false);
    }

    switch( nodeP->type )
    {
    case BINOP:
        return(createsEarly(nodeP->leftP) || createsEarly(nodeP->rightP));

    case UNOP:
        return(createsEarly(nodeP->rightP));

    case ADDR:
    case LCLADDR:
    case BREF:
        return(nodeP->value.symP->serialNumber > planSerial);

    default:
        return(false);
    }
}

// The plan walk has met a symbol: every symbol made before it has been made.
static void
planMention(SymNodeP symP)
{
    if( symP && (symP->serialNumber > planSerial) )
    {
        planSerial = symP->serialNumber;
    }
}

// Add what an old expression created to its note, to be made again after it.
static void
addItem(SrcNoteP noteP, SrcItemKind kind, PNodeP exprP)
{
    if( noteP->itemCount >= SRC_ITEMS )
    {
        fprintf(stderr, "am1: -O=source: the expression rewritten on line %d created too much to keep\n",
            noteP->nodeP->lineNo);
        failed = true;
        return;
    }

    noteP->items[noteP->itemCount].kind = kind;
    noteP->items[noteP->itemCount].exprP = exprP;
    ++noteP->itemCount;
}

// Plan the constant references a rewrite took away: a word the pool still
// holds that nothing now names gets a keeper here, where it was named.
static void
planRemoved(PNodeP nodeP)
{
SrcSlotP slotP;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        planRemoved(nodeP->leftP);
        planRemoved(nodeP->rightP);
        break;

    case UNOP:
        planRemoved(nodeP->rightP);
        break;

    case CONSTANT:
        planRemoved((nodeP->value2.ptr)?(PNodeP)(nodeP->value2.ptr):(PNodeP)(nodeP->value.symP->ptr));

        if( (slotP = findSlot(nodeP->value.symP)) && !slotP->refs && !slotP->filed )
        {
            keeper(slotP, (nodeP->value2.ptr)?(PNodeP)(nodeP->value2.ptr):(PNodeP)(nodeP->value.symP->ptr), true);
        }
        break;

    default:
        break;
    }
}

// A 'constants' directive closes its pool: a word of it not yet filed gets a
// keeper on the directive's line, in pre-order, so its ancestors come first.
static void
planClose(SymNodeP symP)
{
SrcSlotP slotP;

    if( !symP )
    {
        return;
    }

    if( (slotP = findSlot(symP)) && !slotP->filed )
    {
        keeper(slotP, (PNodeP)(symP->ptr), !slotP->refs);
    }

    planClose(symP->leftP);
    planClose(symP->rightP);
}

// Add a keeper for a slot to the current line, after keepers for each of its
// ancestors that is not already filed ahead of the line: one filed by a
// reference on this same line would follow the keeper.
static void
keeper(SrcSlotP slotP, PNodeP exprP, bool orphan)
{
SymNodeP chainPP[SRC_DEPTH];
SymNodeP symP;
SrcSlotP upP;
SrcNoteP noteP;
int depth;

    for( depth = 0, symP = slotP->parentP; symP && (depth < SRC_DEPTH); symP = findSlot(symP)->parentP )
    {
        chainPP[depth++] = symP;
    }

    while( depth-- > 0 )
    {
        upP = findSlot(chainPP[depth]);

        if( !upP->filed || (!upP->early && (upP->line >= planLine)) )
        {
            noteP = addNote(SN_KEEPER, planHeadP);
            noteP->oldP = (PNodeP)(chainPP[depth]->ptr);
            upP->filed = true;
            upP->early = true;
            planMention(upP->symP);
        }
    }

    noteP = addNote(SN_KEEPER, planHeadP);
    noteP->oldP = exprP;
    noteP->orphan = orphan;
    slotP->filed = true;
    slotP->early = true;
    planMention(slotP->symP);
}

// Enter a pool tree's slots in the slot table, each with its parent; with no
// table yet, count them in slotSize instead.
static void
addSlots(SymNodeP symP, SymNodeP parentP)
{
SrcSlotP slotP;
unsigned int h;

    if( !symP )
    {
        return;
    }

    if( !slotsP )
    {
        ++slotSize;
    }
    else
    {
        for( h = (unsigned int)(((uintptr_t)symP >> 4) & (unsigned int)(slotSize - 1)); slotsP[h].symP;
             h = (h + 1) & (unsigned int)(slotSize - 1) )
        {
        }

        slotP = &slotsP[h];
        slotP->symP = symP;
        slotP->parentP = parentP;
    }

    addSlots(symP->leftP, symP);
    addSlots(symP->rightP, symP);
}

// Find a symbol's slot in the table.
// Returns the slot, or NILP for a symbol no pool of the program holds.
static SrcSlotP
findSlot(SymNodeP symP)
{
unsigned int h;

    if( !slotsP || !symP )
    {
        return(NILP);
    }

    for( h = (unsigned int)(((uintptr_t)symP >> 4) & (unsigned int)(slotSize - 1)); slotsP[h].symP;
         h = (h + 1) & (unsigned int)(slotSize - 1) )
    {
        if( slotsP[h].symP == symP )
        {
            return(&slotsP[h]);
        }
    }

    return(NILP);
}

// Write what follows a label on its line, or a text, ascii or type340
// statement: those three are statements here but a label's trailer too.
static void
srcTrailer(PNodeP nodeP)
{
    switch( nodeP->type )
    {
    case TEXT:
        srcTextStmt(nodeP);
        break;

    case TYPE340:
        srcType340Stmt(nodeP);
        break;

    case ASCII:
        put("ascii \"");
        srcAsciiString(nodeP->value.strP);
        put("\"");
        break;

    default:
        srcOperand(nodeP);
        break;
    }
}

// Write an optimizer directive; each spelling is one node type and a flag.
static void
srcDirective(PNodeP nodeP)
{
bool open;

    open = (nodeP->type == OPTIMIZE);

    if( nodeP->flags & PN_INLINE )
    {
        putf("\t%%%%inline %s", nodeP->value2.strP);
    }
    else if( nodeP->flags & PN_CEILING )
    {
        // Kept reduced to its bank, which is all the directive keeps.
        put("\t%%ceiling ");
        putNum(nodeP->value2.ival);
    }
    else if( nodeP->flags & PN_HANDSOFF )
    {
        put(open?"\t%%nooptimize":"\t%%endnooptimize");
    }
    else if( nodeP->flags & PN_SPEED )
    {
        put(open?"\t%%speed":"\t%%endspeed");
    }
    else
    {
        put(open?"\t%%optimize":"\t%%endoptimize");
    }
}

// Whether an operand is written starting with '('.
static bool
startsParen(PNodeP nodeP)
{
    while( nodeP && (nodeP->type == BINOP) )
    {
        nodeP = nodeP->leftP;
    }

    return( nodeP && (nodeP->type == UNOP) && (nodeP->value.ival == PARENS) );
}

// The symbol an operand is written ending with, or NILP when it ends with
// anything else.
static PNodeP
endName(PNodeP nodeP)
{
    while( nodeP && (nodeP->type == BINOP) )
    {
        nodeP = nodeP->rightP;
    }

    return( (nodeP && ((nodeP->type == ADDR) || (nodeP->type == LCLADDR)))?nodeP:NILP );
}

// Write a symbol's name, in parentheses when srcOperand has found it would
// otherwise stand before a '(' that cpp would take for a macro call.
static void
putName(PNodeP nodeP)
{
    if( nodeP == wrapP )
    {
        wrapP = NILP;
        put("(");
        put(nodeP->value.symP->name);
        put(")");
        return;
    }

    put(nodeP->value.symP->name);
}

// Write an expression so that it reads back as the same tree.  The tree is
// the parse of the source's own tokens, parentheses included, so writing its
// leaves in order with the same operators gives those tokens again.
static void
srcOperand(PNodeP nodeP)
{
const char *opP;
PNodeP outerP;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        // A name the source did not follow with '(' may be a function-like
        // macro all the same; written before a parenthesized operand, cpp
        // would call it when it reads the file again.  In parentheses it is
        // no call, and the same value.  An outer list's name may end this
        // operand's right side, so it is kept for that.
        outerP = wrapP;
        if( textLayer && (nodeP->value.ival == SEPARATOR) && startsParen(nodeP->rightP) )
        {
            wrapP = endName(nodeP->leftP);
        }

        srcOperand(nodeP->leftP);
        wrapP = outerP;

        switch( nodeP->value.ival )
        {
        case SEPARATOR:
            opP = "\t";
            break;
        case PLUS:
            opP = " + ";
            break;
        case MINUS:
            opP = " - ";
            break;
        case MUL:
            opP = " * ";
            break;
        case DIV:
            opP = " / ";
            break;
        case MOD:
            opP = " % ";
            break;
        case AND:
            opP = " & ";
            break;
        case OR:
            opP = " | ";
            break;
        case XOR:
            opP = " ^ ";
            break;
        case LSHIFT:
            opP = " << ";
            break;
        case RSHIFT:
            opP = " >> ";
            break;
        default:
            cannot(nodeP, "a binary operator");
            return;
        }

        put(opP);
        srcOperand(nodeP->rightP);
        break;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            put("(");
            srcOperand(nodeP->rightP);
            put(")");
            break;
        case UMINUS:
            put("-");
            srcOperand(nodeP->rightP);
            break;
        case CMPL:
            put("~");
            srcOperand(nodeP->rightP);
            break;
        default:
            cannot(nodeP, "a unary operator");
            break;
        }
        break;

    case CONSTANT:
        // A constant closed by a comment keeps it; the line's own terminator
        // follows, which the comment then ends instead.
        put("[");
        srcOperand((nodeP->value2.ptr)?(PNodeP)(nodeP->value2.ptr)
                                      :(PNodeP)(nodeP->value.symP->ptr));
        put("]");
        if( !inNote && nodeP->rightP && (nodeP->rightP->type == COMMENT) )
        {
            put("  //");
            put(nodeP->rightP->value.strP);
        }
        break;

    case LCLADDR:
        // A copy stands outside the scope of the routine it was copied from,
        // so a local of that routine is written as -m writes one.
        if( copyDepth && !inNote && (nodeP->value.symP->flags & SYMF_RESOLVED) &&
            ((nodeP->value.symP->flags & SYM_MASK) != SYM_GLOB) )
        {
            if( nodeP->value.symP->value >= nodeP->pc )
            {
                put(". + ");
                putNum(nodeP->value.symP->value - nodeP->pc);
            }
            else
            {
                put(". - ");
                putNum(nodeP->pc - nodeP->value.symP->value);
            }

            if( !strstr(pending, ", a local of the copied routine") )
            {
                char text[SRC_NOTE];

                snprintf(text, sizeof(text), "// am1 -O%%L: '.' names %s, a local of the copied routine",
                    nodeP->value.symP->name);
                addPending(text);
            }
            break;
        }
        putName(nodeP);
        break;

    case ADDR:
        putName(nodeP);
        break;

    case LAW:
    case OPORABLE:
    case OPCODE:
    case OPADDR:
    case VALUESPEC:
    case IMOD:
        put(nodeP->value.symP->name);
        break;

    case BREF:
        if( nodeP->flags & RL_WILD )
        {
            putf("%s:*", nodeP->value.symP->name);
        }
        else
        {
            put(nodeP->value.symP->name);
            put(":");
            putNum(nodeP->value2.ival);
        }
        break;

    case DOT:
        // A constant named again away from its word's line keeps its key only
        // as the location '.' was: that is the key's value.
        if( dotAsValue )
        {
            putNum(nodeP->value.ival & WRDMASK);
        }
        else
        {
            put(".");
        }
        break;

    case INTEGER:
        if( (nodeP->flags & SRC_NOTED) && !inNote )
        {
            srcNumber(nodeP);
            break;
        }
        // fall through

    case FLEXO:
    case CHAR:
        // A char or flexo constant is a number once lexed; its value is what
        // the tape and the constant pools see.
        putNum(nodeP->value.ival & WRDMASK);
        break;

    case LITCHAR:
        if( (nodeP->value.ival >= ' ') && (nodeP->value.ival < 0177) &&
            (nodeP->value.ival != '\'') && (nodeP->value.ival != '\\') )
        {
            putf("'%c'", nodeP->value.ival);
        }
        else if( (nodeP->value.ival >= 0) && (nodeP->value.ival <= 0377) )
        {
            putf("'\\%03o'", nodeP->value.ival);
        }
        else
        {
            putNum(nodeP->value.ival & WRDMASK);
        }
        break;

    case FORCELOC:
        put("%%forcelocal");
        break;

    case LOCAL:
        put("local");
        if( nodeP->rightP )
        {
            put(" ");
            srcNameList(nodeP->rightP);
        }
        break;

    case ADDLOCAL:
        put("addlocal");
        if( nodeP->rightP )
        {
            put(" ");
            srcNameList(nodeP->rightP);
        }
        break;

    case PRIVATE:
        put("private");
        if( nodeP->rightP )
        {
            put(" ");
            srcNameList(nodeP->rightP);
        }
        break;

    case ENDLOC:
        put("endloc");
        if( nodeP->value.ival != -1 )
        {
            put(" ");
            putNum(nodeP->value.ival);
        }
        break;

    default:
        cannot(nodeP, "an operand");
        break;
    }
}

// Write a number relayout re-points: as the constant a T8d loads, as a label
// and offset for a T3's far target, or as the number, annotated.
static void
srcNumber(PNodeP nodeP)
{
SrcNoteP noteP;

    if( !(noteP = numberNoteOf(nodeP)) )
    {
        putNum(nodeP->value.ival & WRDMASK);
    }
    else if( noteP->spellP )
    {
        if( noteP->spellP->type == BINOP )
        {
            put("(");
            srcOperand(noteP->spellP);
            put(")");
        }
        else
        {
            srcOperand(noteP->spellP);
        }
    }
    else if( noteP->labelP )
    {
        put(noteP->labelP->name);

        if( noteP->offset )
        {
            put(" + ");
            putNum(noteP->offset);
        }
        addPending("// am1 -O%L: the far target, written from the nearest label at or below it");
    }
    else
    {
        putNum(nodeP->value.ival & WRDMASK);
        addPending("// am1 -O%L: the number is an address, and no label reaches it");
    }
}

// Write a local, private, addlocal or export list: NAME nodes for names not
// yet defined, ADDR nodes for those that were, chained through leftP.
static void
srcNameList(PNodeP nodeP)
{
    for( ; nodeP; nodeP = nodeP->leftP )
    {
        put((nodeP->type == NAME)?nodeP->value.strP:nodeP->value.symP->name);
        if( nodeP->leftP )
        {
            put(", ");
        }
    }
}

// Write a var statement's names in the order written.  The parser chains them
// last first through rightP; each initializer hangs from leftP.
static void
srcVarList(PNodeP nodeP)
{
    if( !nodeP )
    {
        return;
    }

    if( nodeP->rightP )
    {
        srcVarList(nodeP->rightP);
        put(", ");
    }

    put(nodeP->value.symP->name);
    if( nodeP->leftP )
    {
        // No blanks around '=': a blank would be a separator token.
        put("=");
        srcOperand(nodeP->leftP);
    }
}

// Write the inside of a quoted string as the lexer's escapes read it back:
// processEscape() in lexfuncs.c.
static void
srcAsciiString(const char *strP)
{
unsigned char ch;

    for( ; *strP; ++strP )
    {
        ch = (unsigned char)*strP;

        switch( ch )
        {
        case '"':
            put("\\\"");
            break;
        case '\\':
            put("\\\\");
            break;
        case '\n':
            put("\\n");
            break;
        case '\t':
            put("\\t");
            break;
        case '\r':
            put("\\r");
            break;
        case '\f':
            put("\\f");
            break;
        case '\b':
            put("\\b");
            break;
        case '\033':
            put("\\e");
            break;
        default:
            if( (ch >= ' ') && (ch < 0177) )
            {
                putf("%c", ch);
            }
            else
            {
                // Always three digits, so a digit after it is not read into it.
                putf("\\%03o", ch);
            }
            break;
        }
    }
}

// Write a text statement.  The node holds Flexo codes with the case shifts the
// lexer inserted; they are decoded to the characters that make the lexer
// insert the same shifts again, and the result is encoded back to prove it.
// Codes no string can produce are written as the words they pack into.
static void
srcTextStmt(PNodeP nodeP)
{
char str[SRC_STRING];
const char *markP;

    if( decodeText(nodeP->value.flexText, str, sizeof(str)) && sameText(nodeP->value.flexText, str) )
    {
        // The delimiter is any character the string does not hold.
        for( markP = "\"/|#!$@&"; *markP && strchr(str, *markP); ++markP )
        {
        }

        if( *markP )
        {
            putf("text %c", *markP);
            put(str);
            putf("%c", *markP);
            return;
        }
    }

    srcTextWords(nodeP->value.flexText, "text");
}

// Decode Flexo codes into the characters a text string is written with:
// red, black and carriage return as their escapes.
// Returns true with the string in outP, false for a code no string can give.
static bool
decodeText(FlexText text, char *outP, int size)
{
int i;
int shift;
int ch;
int code;
int used;

    shift = 0;
    used = 0;

    for( i = 0; i < text.nchars; ++i )
    {
        if( (used + 3) >= size )
        {
            return(false);
        }

        code = text.bufP[i] & 077;

        if( code == RED )
        {
            outP[used++] = '\\';
            outP[used++] = 'R';
            continue;
        }

        if( code == BLACK )
        {
            outP[used++] = '\\';
            outP[used++] = 'B';
            continue;
        }

        if( code == FLEX_CR )
        {
            outP[used++] = '\\';
            outP[used++] = 'n';
            continue;
        }

        ch = flexoToAscii((char)code, &shift);
        if( ch == NONE )
        {
            // A case shift is inserted by the lexer; anything else is not text.
            if( (code == CSHIFT) || (code == CUNSHIFT) )
            {
                continue;
            }
            return(false);
        }

        // A backslash always starts an escape, and a control character other
        // than a tab would not survive being a character of the source.
        if( (ch == '\\') || ((ch < ' ') && (ch != '\t')) || (ch >= 0177) )
        {
            return(false);
        }

        outP[used++] = (char)ch;
    }

    outP[used] = '\0';
    return(true);
}

// Encode a text string as the lexer's text rule does, and compare it with the
// codes the node holds.
// Returns true when they are the same codes.
static bool
sameText(FlexText text, const char *strP)
{
int shift;
int code;
int count;

    shift = 0;
    count = 0;

    for( ; *strP; ++strP )
    {
        if( *strP == '\\' )
        {
            ++strP;
            code = (*strP == 'R')?RED:((*strP == 'B')?BLACK:FLEX_CR);
        }
        else
        {
            code = asciiToFlexo(*strP, &shift);
            if( code == NONE )
            {
                return(false);
            }

            if( (code == CSHIFT) || (code == CUNSHIFT) )
            {
                if( (count >= text.nchars) || ((text.bufP[count] & 077) != code) )
                {
                    return(false);
                }
                ++count;
                code = asciiToFlexo(*strP, &shift);
            }
        }

        if( (count >= text.nchars) || ((text.bufP[count] & 077) != code) )
        {
            return(false);
        }
        ++count;
    }

    return( count == text.nchars );
}

// Write a type340 statement: as readable characters where the lexer's own
// automatic shifts give the codes back, else every code as an escape, else as
// the words the codes pack into.
static void
srcType340Stmt(PNodeP nodeP)
{
char str[SRC_STRING];

    if( (decodeType340(nodeP->value.flexText, false, str, sizeof(str)) &&
         sameType340(nodeP->value.flexText, str)) ||
        (decodeType340(nodeP->value.flexText, true, str, sizeof(str)) &&
         sameType340(nodeP->value.flexText, str)) )
    {
        put("type340 \"");
        put(str);
        put("\"");
        return;
    }

    srcTextWords(nodeP->value.flexText, "type340");
}

// Decode Type 340 codes into a type340 string.  With escapes false, as the
// characters that shift automatically, the lexer supplying the shifts and the
// end marker; with escapes true, every code as a two-digit escape.
// Returns true with the string in outP, false for a code this form cannot give.
static bool
decodeType340(FlexText text, bool escapes, char *outP, int size)
{
int i;
int code;
int used;
bool upper;
char ch;

    used = 0;
    upper = true;       // every type340 string starts in upper case

    for( i = 0; i < text.nchars; ++i )
    {
        if( (used + 4) >= size )
        {
            return(false);
        }

        code = text.bufP[i] & 077;

        if( escapes )
        {
            // Two digits always, so a blob is \00.
            used += snprintf(outP + used, size - used, "\\%02o", code);
            continue;
        }

        if( code == TYPE340UPPER )
        {
            upper = true;
            continue;
        }

        if( code == TYPE340LOWER )
        {
            upper = false;
            continue;
        }

        if( code == TYPE340END )
        {
            // The lexer ends the string itself; one anywhere else is not
            // automatic.
            if( i != (text.nchars - 1) )
            {
                return(false);
            }
            continue;
        }

        if( code == TYPE340CR )
        {
            outP[used++] = '\\';
            outP[used++] = 'r';
            continue;
        }

        if( code == TYPE340LF )
        {
            outP[used++] = '\\';
            outP[used++] = 'l';
            continue;
        }

        // The first character that gives this code without moving the lexer
        // out of the case it is in.  A quote or a backslash is written escaped,
        // which the lexer reads as the plain character.
        for( ch = ' '; ch < 0177; ++ch )
        {
            if( (asciiToType340(ch) == code) &&
                ((type340Shift(ch) == 0) || ((type340Shift(ch) > 0) == upper)) )
            {
                break;
            }
        }

        if( ch >= 0177 )
        {
            return(false);
        }

        if( (ch == '"') || (ch == '\\') )
        {
            outP[used++] = '\\';
        }
        outP[used++] = ch;
    }

    outP[used] = '\0';
    return(true);
}

// Encode a type340 string as the lexer's type340 rule does, and compare it
// with the codes the node holds.
// Returns true when they are the same codes.
static bool
sameType340(FlexText text, const char *strP)
{
char codes[SRC_STRING];
int count;
int shift;
int number;
bool upper;
bool forced;
bool sawEnd;
bool plain;
char ch;

    count = 0;
    upper = true;
    forced = false;
    sawEnd = false;

    for( ; *strP && (count < (SRC_STRING - 2)); ++strP )
    {
        // An escaped quote or backslash is the plain character.
        plain = ((*strP == '\\') && ((strP[1] == '"') || (strP[1] == '\\')));
        if( plain )
        {
            ++strP;
        }

        if( !plain && (*strP == '\\') )
        {
            ++strP;
            switch( *strP )
            {
            case 'r':
                ch = TYPE340CR;
                break;
            case 'l':
                ch = TYPE340LF;
                break;
            default:
                // Only the two-digit escapes are written; they are raw codes.
                number = ((strP[0] - '0') * 8) + (strP[1] - '0');
                ++strP;
                ch = (char)number;
                break;
            }

            if( ch == TYPE340UPPER )
            {
                upper = true;
                forced = true;
            }
            else if( ch == TYPE340LOWER )
            {
                upper = false;
                forced = true;
            }
            else if( ch == TYPE340END )
            {
                sawEnd = true;
            }
        }
        else
        {
            shift = type340Shift(*strP);
            if( !forced && (shift > 0) && !upper )
            {
                upper = true;
                codes[count++] = TYPE340UPPER;
            }
            else if( !forced && (shift < 0) && upper )
            {
                upper = false;
                codes[count++] = TYPE340LOWER;
            }

            ch = asciiToType340(*strP);
        }

        codes[count++] = ch;
    }

    if( !sawEnd )
    {
        codes[count++] = TYPE340END;
    }

    return( (count == text.nchars) && !memcmp(codes, text.bufP, count) );
}

// Write text or type340 codes that no string gives back as the words they pack
// into, three codes to a word as writeText() in bincodegen.c packs them: the
// bytes unmasked, so a byte wider than six bits would spill into its neighbor
// exactly as it does on the tape, which keeps 18 bits.
// The tape is the same; the source says what it was.
static void
srcTextWords(FlexText text, const char *whatP)
{
int i;
int val;
int words;

    words = 0;

    for( val = i = 0; i < text.nchars; ++i )
    {
        if( i && !(i % 3) )
        {
            putf((words++ == 0)?"%06o":"\n\t%06o", val & WRDMASK);
            val = 0;
        }

        val = (val << 6) | text.bufP[i];
    }

    if( i % 3 )
    {
        while( i++ % 3 )
        {
            val <<= 6;
        }
    }

    putf((words == 0)?"%06o":"\n\t%06o", val & WRDMASK);
    putf("  // am1 -O=source: a %s string no string can spell, written as its words", whatP);
}

// Report a node the renderer has no spelling for; the run fails.
static void
cannot(PNodeP nodeP, const char *whereP)
{
    fprintf(stderr, "am1: -O=source cannot write node type %d as %s, source line %d\n",
        nodeP->type, whereP, nodeP->lineNo);
    putf("<<am1 -O=source: node type %d>>", nodeP->type);
    failed = true;
}

// End the current line if anything is on it or an annotation waits for it.
static void
startLine(void)
{
    if( pending[0] )
    {
        endLine();
    }
    else if( !atBol )
    {
        put("\n");
    }
}

// End the line, with the annotations waiting for it, after two blanks when
// something is on it.
static void
endLine(void)
{
    if( pending[0] )
    {
        if( !atBol )
        {
            put("  ");
        }

        put(pending);
        pending[0] = '\0';
    }

    put("\n");
}

// Write a string as it is, keeping track of whether it ended a line; while an
// expression is quoted for a note, append it to the note instead.
static void
put(const char *strP)
{
size_t len;
size_t used;

    len = strlen(strP);

    if( captureP )
    {
        used = strlen(captureP);
        snprintf(captureP + used, captureSize - used, "%s", strP);
        return;
    }

    if( len )
    {
        fputs(strP, srcfP);
        atBol = (strP[len - 1] == '\n');
    }
}

// Write a short formatted piece, SRC_FMT at most; long text goes through put().
static void
putf(const char *fmtP, ...)
{
va_list argP;
char buf[SRC_FMT];

    va_start(argP, fmtP);
    vsnprintf(buf, sizeof(buf), fmtP, argP);
    va_end(argP);
    put(buf);
}

// Write a number as the lexer reads one back: octal, which the lexer reads
// unless a copied 'decimal' line has switched it, and then with 0o, which it
// reads as octal either way.  A single digit reads the same in both.
static void
putNum(int value)
{
    putf((radixPrefix && (value > 7))?"0o%o":"%o", value);
}
