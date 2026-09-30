/*
 * The am1 optimizer's relayout: it applies edits that delete, move or copy
 * words, lays the program out again from the tree as the parser would, and
 * refuses any edit it cannot show leaves every reference meaning what it meant.
 * am1 fixes every address while it parses, so any edit that changes the number
 * or order of words must recompute all of them.  Edits come from the
 * transforms (inline, fall-through placement, unrolling, space mode, the
 * extend window's deletions) and from the test instrument -O=edits=FILE, one
 * per line ('#' starts a comment; bank decimal, addresses octal, as assembled):
 *
 *     delete BANK ADDR                 delete the one word of a statement
 *     move BANK FROM TO after ADDR     move the run of statements emitting
 *                                      FROM..TO after the statement whose last
 *                                      word is ADDR
 *     copy BANK FROM TO over ADDR      copy that run in place of the word at
 *                                      ADDR, which is deleted
 *
 * The edit model is the statement list.  A deleted statement becomes a
 * RELAYOUT node holding its expression; every back end but the listing steps
 * over it.  A moved run is relinked whole, with RELAYOUT markers so the
 * listing keeps source order.  A copy splices CLONE statements after the
 * deleted word's line, so a label on that line labels the copy's first word.
 *
 * The layout walk mirrors the grammar actions in parser.y exactly: node pcs,
 * every '.', labels, resume pcs, pools, variables, bank contexts and 'start'.
 * Every field it owns is first set to a sentinel, so a field it forgot shows.
 * The pools are then rebuilt as the parser would build them from the edited
 * source: the same hashExpr() keys, filed in the same kind of unbalanced tree
 * in statement order, slots placed in pre-order.  Layout and rebuild repeat
 * until the pools settle.  Every remaining word is then checked against the
 * word table, which still describes the program as assembled, by perturbing
 * each address an expression names to see how its address field depends on
 * it.  A refused edit is undone.
 *
 * Runs single threaded, once per optimize() call, after the report is written
 * and before the table is freed, and only when there is an edit or a pool
 * reclaim to make or -O=relayout was given.  With opttransform.c it is the
 * only part of the optimizer that writes a parse node.  Uses optimizer.h's
 * word table, eval.c's evaluators, parser.y's banksP and constsListP, and
 * am1.c's noWarn and warnings[].  Out of memory and an inconsistent result
 * are fatal: no code generator has run yet.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "am1.h"
#include "y.tab.h"
#include "optimizer.h"

// A check of the zero-edit layout: a build with -DOPTRELAYOUT_SKIP=n leaves
// recompute n undone, so its sentinel survives and must be caught.
#ifdef OPTRELAYOUT_SKIP
#define RL_SKIP(n)      (OPTRELAYOUT_SKIP == (n))
#else
#define RL_SKIP(n)      0
#endif

#define RLSKIP_PC       1       // node pcs
#define RLSKIP_DOT      2       // every '.'
#define RLSKIP_LABEL    3       // label symbols
#define RLSKIP_RESUME   4       // a bank directive's resume pc
#define RLSKIP_POOLPC   5       // pool slot addresses
#define RLSKIP_POOLVAL  6       // pool slot values
#define RLSKIP_VARPC    7       // variable addresses
#define RLSKIP_BANKCTX  8       // the bank contexts' cur_pc, constPC and varPC
#define RLSKIP_START    9       // 'start' and the root
#define RLSKIP_REKEY    10      // the pools rebuilt in the edited order

#define RL_SENTINEL     07070   // what every recomputed address holds before the walk
#define RL_VSENTINEL    070707  // and every recomputed value
#define RL_MAXPASSES    16      // layouts one settle may take while the pools change
#define RL_MAXLEAVES    32      // addresses one expression may name and still be analyzed
#define RL_MAXKEYLEAVES 12      // symbols one constant's key may read and still be modeled
#define RL_LINE         256     // the longest edits line read whole
#define RL_TAG          0x40000000  // a node flag, set only while a move is checked
                                    // (RL_WILD, the other node flag, is in optimizer.h)

#define RLW_DELETED     1       // the word is deleted
#define RLW_MOVED       2       // the word is in a moved run

extern BankContextP banksP;
extern SymListP constsListP;
extern PNodeListP wildcardsP;
extern bool noWarn;
extern Warning warnings[];

extern int evalExpr(PNodeP);
extern long int hashExpr(PNodeP);
extern int countAscii(char *strP);
extern int countText(FlexText flexText);

typedef enum
{
    RLE_DELETE,
    RLE_MOVE,
    RLE_COPY,
    RLE_INLINE,         // a copy an inline made, with its site's edits
    RLE_UNROLL          // repeat copies of a loop's body, and its jmp's delete
} RlKind;

// What became of an edit.  The dump prints the names in this order.
typedef enum
{
    RLR_ACCEPTED,
    RLR_NOWORD,         // nothing was emitted at an address the edit names
    RLR_NOTSTATEMENT,   // the address is not a statement's word, or not its first or last
    RLR_NEXTWORD,       // a deleted word's label would have no word to move to
    RLR_UNIT,           // the run holds a directive a move cannot carry
    RLR_SEGMENT,        // an origin or a bank directive lies between the run and its destination
    RLR_REGION,         // the run and its destination are not in the same kind of region
    RLR_CONFLICT,       // the edit overlaps an earlier accepted one
    RLR_OVERLAY,        // an overlaid address would move
    RLR_OFFSET,         // an address plus an offset would name a different word
    RLR_DOT,            // the same, relative to '.'
    RLR_NUMBER,         // a memory reference written as a number would name a different word
    RLR_LABEL,          // a label would be left on a different word than the one it named
    RLR_BOUND,          // a word would go past the end of the bank
    RLR_OVERLAP,        // two words would share an address
    RLR_POOL,           // a pool would appear or vanish, or a key could not be made again
    RLR_COPYLABEL,      // a copied run defines a label, which the copy would define twice
    RLR_COPYINTERNAL,   // a copied word names a word of the run, which the copy would still
                        // name in the original
    RLR_SKIPPED,        // a callee's delete, not tried because its copy, or an
                        // earlier delete of the same callee, was refused; or a freed
                        // eem's, because a lem it depends on was not deleted
    RLR_INTERNAL,       // the result is not consistent; not expected to happen
    RLR_CEILING,        // a bank would reach its declared %%ceiling; the budget
                        // should have stopped it, so like internal it is a defect guard
    RLR_COUNT
} RlReason;

typedef struct
{
    RlKind kind;
    int bank;
    int from;           // delete: the word; move and copy: the run's first word
    int to;             // move and copy: the run's last word
    int addr;           // move: the destination's last word; copy: the word it replaces
    const char *fileP;
    int lineNo;
    RlReason reason;

    // Undo state, valid while the edit is applied.
    PNodeP stmtP;       // delete and copy: the deleted statement
    PNodeP markP;       // and its RELAYOUT node, the statement itself or a new one
    PNodeValue savedValue;
    PNodeValue savedValue2;
    PNodeP firstP;      // move: the run's first node
    PNodeP endP;        // and its last, the line end of its last statement
    PNodeP nextP;       // the node that followed the run
    PNodeP *linkPP;     // the link that pointed at firstP
    PNodeP destP;       // the node the run now follows
    PNodeP movedFromP;  // the three markers
    PNodeP beginP;
    PNodeP runEndP;

    PNodeP copyFirstP;  // copy: the clone chain's first node
    PNodeP copyLastP;   // and its last
    PNodeP copyAfterP;  // the node it was spliced after
    int copyWords;      // the words it emits
    int copyRefs;       // the cloned constant references it appended to oldRefsP

    int *entriesP;      // the words it deletes or moves, as table indexes; a copy's
                        // is the one word it deletes, not the run, which stays put
    int entryCount;

    // An edit an inline made rather than an -O=edits line.
    OptInlineP inlineP; // the site's record, NILP for a line of the file
    int parent;         // a callee's delete: the index of the copy it follows, else -1;
                        // a placement's delete of J: the index of its move

    // An edit a placement made: its move of the run or its delete of the jmp.
    OptPlaceP placeP;   // the site's record, NILP otherwise

    // An edit an unroll made: its copies or a delete of its setup.  The copies
    // are made repeat times over, n - 1, and the jmp back is deleted in the
    // same edit, with undo state of its own.
    OptUnrollP unrollP; // the loop's record, NILP otherwise
    int repeat;         // unroll: the copies to make, 0 for a loop of one trip
    PNodeP jStmtP;      // unroll: the jmp's deleted statement, NILP until it is deleted
    PNodeP jMarkP;      // and its RELAYOUT node
    PNodeValue jSavedValue;
    PNodeValue jSavedValue2;

    // A deleting rule's delete.  When the rule keeps a word too, its new
    // expression goes into the kept statement in the same edit, and comes out
    // again with the delete.
    OptXformP xformP;   // the rule's record, NILP otherwise
    PNodeP keepStmtP;   // the kept word's statement while the new expression is in it
    PNodeP keepOldP;    // and the expression it had

    // An extend-window eem or lem's delete.
    OptWinDelP windelP; // its record, NILP otherwise
} RlEdit;

// A pool slot the program did not have, made for a key the edited program
// has; kept for reuse while it is out of the pools.
typedef struct rlextra
{
    struct rlextra *nextP;
    SymNodeP symP;
    int pool;               // index into the pool array
    int live;               // it is in its pool's tree
} RlExtra, *RlExtraP;

// A pool slot the program had.
typedef struct
{
    SymNodeP symP;
    int pool;
    PNodeP firstRefP;       // its first reference, as assembled
    int live;               // it is in its pool's tree
    int orphan;             // no reference names it: a transform rewrote them all
    int reclaim;            // and every rewrite that stopped naming it
                            // was licensed by a declaration, so the word may go
    int child[2];           // its children as assembled, as slot indexes, or -1
    int anchored;           // filed after its parent, not where the walk meets it
    int order;              // where the walk met its key, as assembled; INT_MAX for never
    int emitted;            // the walk that last filed it anchored
} RlSlot;

// A symbol a walk has met.
typedef struct
{
    SymNodeP symP;
    int oldCreated;         // the creations before it, as assembled
    int created;            // and in the walk, or -1 before its first mention
    int defined;            // the walk has passed its definition
    int walk;               // the walk the two above belong to
} RlSym;

// A key a walk made, in the order it made it.
typedef struct
{
    long hash;
    char name[24];          // as the parser names the slot
    PNodeP firstRefP;       // the reference that made it
    SymNodeP slotP;         // the slot given it
    int refs;
    int left;               // its children in the pool's tree, as key indexes, or -1
    int right;
    int placed;             // filed by orderSet()
} RlKey, *RlKeyP;

// The keys one bank has gathered since its last constants directive.
typedef struct
{
    int *keysP;             // key indexes, in order
    int count;
    int capacity;
    int pool;               // the pool they became, or -1
    int bank;
} RlSet, *RlSetP;

// An address an expression names, while the expression is analyzed.
typedef struct
{
    int isDot;
    SymNodeP symP;
    PNodeP nodeP;
    int coeff;
} RlLeaf;

// A constant reference: the value it had as assembled, and what made its key.
typedef struct
{
    PNodeP refP;
    int value;
    SymNodeP slotP;         // its slot, as assembled
    int leafStart;          // the symbols its key reads, in keySymsPP
    int leafCount;          // or -1 when there are too many to model
    unsigned oldOrder;      // bit n: symbol n was defined before it in the walk, as assembled
    unsigned mask;          // bit n: symbol n was resolved when it was parsed
    int modeled;            // 0: no mask gives its key, which is held
    long oldAllHash;        // its key with every symbol resolved, as assembled
    int walk;               // the last walk that met it
    int key;                // and the key that walk gave it
    void *cloneOfP;         // the RlEdit whose copy made this reference, NILP for
                            // one the program has.  A clone shares its original's
                            // model, the expressions being the same tree twice;
                            // only the walk state its key is read under differs
} RlRefValue;

static RlEdit *editsP;
static int editCount;
static int editsGiven;
static const char *editsPathP;

// The state of one optRelayout() call.
static OptTableP rlTableP;
static PNodeP rlRootP;
static unsigned char *wordStateP;   // RLW_ bits per table index
static int *offsetP;                // a word's address less its statement's pc, as assembled
static OptXformP *recordOfP;        // the applied, fired record of a rewritten word, per index
static int rlPC[MAXBANK + 1];
static int rlBank;
static int rlBound;
static int rlTop[MAXBANK + 1];      // each bank's highest word as laid out, -1 for none
static int rlCeilingCheck;          // settle() refuses a bank at its declared ceiling
static int rlPoolBank;              // the bank placePool() is placing into
static int heldOrigins;
static int heldTables;

static SymNodeP *poolsPP;           // every pool root, in constsListP order
static int *poolBankP;
static int poolCount;

static SymListP *poolListsPP;       // each pool's constsListP entry
static RlSlot *slotsP;              // every slot the program had, sorted by address
static int *slotByNameP;            // their indexes, sorted by pool and key
static int slotCount;
static RlExtraP extrasP;
static PNodeP *directivesPP;        // each constants directive that closed a pool
static int *directivePoolP;         // and the pool
static int directiveCount;
static int bankEndPool[MAXBANK + 1];    // the pool each bank's end closed, or -1
static int unkeyedCount;            // references whose key no mask gives
static int *poolOrphansP;           // each pool's orphan slots
static int *poolAnchoredP;          // and its anchored ones, orphans included
static int *poolReclaimP;           // and its reclaimable ones
static int rlReclaim;               // the reclaim is in force: 0 while the program as
                                    // assembled is laid out again, 1 after it
static int reclaimedSlots;          // slots the reclaim dropped

static RlSym *symMapP;              // the walk's symbols, open addressing
static int symMapSize;
static int symMapCount;
static SymNodeP *keySymsPP;         // the symbols every key reads
static int keySymCount;
static int keySymCapacity;
static PNodeP *wildNodesPP;         // the 'sym:*' nodes rlHash() has flipped
static SymNodeP *wildSymsPP;        // and the symbol each held
static int wildCount;
static int wildCapacity;
static RlKeyP keysP;                // the walk's keys
static int keyCount;
static int keyCapacity;
static RlSetP setsP;                // the walk's pools
static int setCount;
static int setCapacity;
static int openSet[MAXBANK + 1];    // each bank's open set, or -1
static int rlWalk;
static int rlRecord;
static int rlCreations;
static int rekeyChanges;            // rebuilds that changed a pool, this settle
static PNodeP *refsPP;              // the live constant references
static int refCount;
static int refCapacity;
static RlRefValue *oldRefsP;        // as assembled, sorted by node address
static int oldRefCount;

static PNodeP *clonePairsPP;        // the copy being made: its constant references, as
static int clonePairCount;          // (original, clone) pairs, two entries each
static int clonePairCapacity;

static int savedNoWarn;
static int savedLawEnabled;

static int readNumber(const char *textP, int base, long max, long *valueP, const char **endPP);
static void editsError(const char *pathP, int lineNo, const char *msgP);
static const char *reasonName(RlReason reason);

static void quiet(void);
static void loud(void);

static RlReason tryDelete(RlEdit *editP);
static RlReason tryMove(RlEdit *editP);
static RlReason tryCopy(RlEdit *editP);
static RlReason tryInline(RlEdit *editP);
static void addInlineEdits(OptTableP tableP);
static void addPlaceEdits(OptTableP tableP);
static void addUnrollEdits(OptTableP tableP);
static void addSpaceEdits(OptTableP tableP);
static void addWindowEdits(OptTableP tableP);
static int windelDepsMade(OptTableP tableP, OptWinDelP delP);
static RlReason tryUnroll(RlEdit *editP);
static void undoJmpDelete(RlEdit *editP);
static RlEdit *newEdit(void);
static void inlineOutcomes(void);
static void noteCopies(void);
static void deleteWord(RlEdit *editP, PNodeP stmtP);
static void undoDelete(RlEdit *editP);
static int inRun(OptWordP *runPP, int count, int bank, int addr);
static PNodeP cloneRun(PNodeP firstP, PNodeP endP, PNodeP *lastPP, int *wordsP);
static PNodeP labelLinesBefore(PNodeP stmtP);
static PNodeP cloneExpr(PNodeP nodeP);
static void clonePair(PNodeP origP, PNodeP cloneP);
static void cloneRefs(RlEdit *editP);
static int copiedWords(int *copiedP);
static void undoEdit(RlEdit *editP);
static int statementSize(PNodeP nodeP);
static PNodeP lineEnd(PNodeP stmtP, int *terminatedP);
static PNodeP *linkTo(PNodeP targetP);
static PNodeP newMarker(PNodeP modelP, int what);
static int nextWordFollows(PNodeP nodeP);
static int isFixedPoint(PNodeP nodeP);

static RlReason settle(void);
static void poison(void);
static void poisonTree(PNodeP nodeP);
static void layout(void);
static void setStatement(PNodeP nodeP, int pc);
static void setExpr(PNodeP nodeP, int pc);
static void advance(int words);
static int placePool(int pc, SymNodeP symP);
static void placeVars(int bank, PNodeListP listP);
static void setPoolValues(SymNodeP symP);
static void poisonPool(SymNodeP symP);
static void collectRefs(void);
static void collectExprRefs(PNodeP nodeP);
static void repointT3(void);
static void repointT8(void);
static RlReason checkT8(int index, OptXformP recP);

static void recordPools(void);
static void countSlots(SymNodeP symP, int pool);
static void addSlots(SymNodeP symP, int pool);
static int rootPool(SymNodeP rootP);
static int compareSlots(const void *aP, const void *bP);
static int compareSlotNames(const void *aP, const void *bP);
static RlSlot *findSlot(SymNodeP symP);
static RlSlot *findSlotByName(int pool, const char *nameP);
static RlRefValue *findRef(PNodeP refP);
static RlSym *symInfo(SymNodeP symP);
static RlSym *symSlot(SymNodeP symP);
static void mentionSym(SymNodeP symP);
static int firstMention(SymNodeP symP);
static void defineSym(SymNodeP symP);
static RlReason rekeyWalk(int record);
static void defineLabel(PNodeP nodeP);
static int directivePool(PNodeP nodeP);
static RlReason walkExpr(int bank, PNodeP nodeP);
static RlReason keyRef(int bank, PNodeP refP);
static void recordKey(RlRefValue *infoP);
static void flipWild(PNodeP nodeP);
static long rlHash(PNodeP exprP);
static int addKeySyms(RlRefValue *infoP, PNodeP nodeP);
static unsigned currentMask(RlRefValue *infoP);
static long keyUnder(RlRefValue *infoP, unsigned mask, int shifted);
static RlReason bindSet(int bank, int pool);
static void buildPools(int *changedP);
static void orderSet(RlSetP setP);
static int placeKey(RlSetP setP, int key, RlSlot *slotP, int *orderP, int n);
static int placeSlot(RlSetP setP, RlSlot *slotP, int *orderP, int n);
static int anchorSlots(RlSlot *slotP);
static void markReclaimable(void);
static RlReason reclaimPools(void);
static int newKey(void);
static int newSet(int bank);
static void setRoot(int pool, SymNodeP rootP, int *changedP);
static int poolChanges(int *grownP, int *droppedP);

static int currentAddr(int index);
static RlReason checkHazards(void);
static RlReason checkWord(int index);
static RlReason checkExpr(int bank, PNodeP exprP, int oldValue, int topDotAddr);
static int collectLeaves(PNodeP nodeP, RlLeaf *leavesP, int *countP);
static int leafValue(RlLeaf *leafP);
static void setLeafValue(RlLeaf *leafP, int value);
static int oldRefValue(PNodeP refP, int *valueP);
static int compareRefValues(const void *aP, const void *bP);
static RlReason checkTable(int *dupCountP);

static unsigned long checksum(void);
static void sumTree(PNodeP nodeP, unsigned long *sumP);
static void sumPool(SymNodeP symP, unsigned long *sumP);
static void mix(unsigned long *sumP, long value);
static int heldExpr(PNodeP nodeP);

static void dump(FILE *fP, int idempotent);

// The -O=edits instrument.

// Read one -O=edits file: '#' comments and blank lines skipped, one edit per
// line.  A malformed line is fatal, naming the file and line.
// Returns 1 if read, 0 for an empty file name.
int
optRelayoutReadEdits(const char *pathP)
{
FILE *fP;
char line[RL_LINE];
char *cP;
const char *textP;
const char *endP;
long numbers[4];
int wanted;
int i;
int lineNo;
RlEdit *editP;
RlKind kind;
const char *joinP;      // the word before a three-address edit's last address
const char *formP;      // and what the line should have looked like

    if( !*pathP )
    {
        fprintf(stderr, "am1: -O=edits= needs a file name\n");
        return(0);
    }

    if( !(fP = fopen(pathP, "r")) )
    {
        fprintf(stderr, "am1: -O=edits=%s: cannot open the file\n", pathP);
        exit(1);
    }

    lineNo = 0;

    while( fgets(line, sizeof(line), fP) )
    {
        ++lineNo;

        if( !strchr(line, '\n') && !feof(fP) )
        {
            editsError(pathP, lineNo, "the line is too long");
        }

        if( (cP = strchr(line, '#')) )
        {
            *cP = '\0';
        }

        for( textP = line; (*textP == ' ') || (*textP == '\t'); ++textP )
        {
        }

        if( (*textP == '\0') || (*textP == '\n') || (*textP == '\r') )
        {
            continue;
        }

        joinP = NILP;
        formP = NILP;

        if( !strncmp(textP, "delete", 6) && ((textP[6] == ' ') || (textP[6] == '\t')) )
        {
            kind = RLE_DELETE;
            wanted = 2;
            textP += 6;
        }
        else if( !strncmp(textP, "move", 4) && ((textP[4] == ' ') || (textP[4] == '\t')) )
        {
            kind = RLE_MOVE;
            wanted = 4;
            joinP = "after";
            formP = "expected \"move BANK FROM TO after ADDR\"";
            textP += 4;
        }
        else if( !strncmp(textP, "copy", 4) && ((textP[4] == ' ') || (textP[4] == '\t')) )
        {
            kind = RLE_COPY;
            wanted = 4;
            joinP = "over";
            formP = "expected \"copy BANK FROM TO over ADDR\"";
            textP += 4;
        }
        else
        {
            editsError(pathP, lineNo,
                "expected \"delete BANK ADDR\", \"move BANK FROM TO after ADDR\" or \"copy BANK FROM TO over ADDR\"");
            return(0);
        }

        for( i = 0; i < wanted; ++i )
        {
            for( ; (*textP == ' ') || (*textP == '\t'); ++textP )
            {
            }

            if( joinP && (i == 3) )
            {
                if( strncmp(textP, joinP, strlen(joinP)) ||
                    ((textP[strlen(joinP)] != ' ') && (textP[strlen(joinP)] != '\t')) )
                {
                    editsError(pathP, lineNo, formP);
                }

                for( textP += strlen(joinP); (*textP == ' ') || (*textP == '\t'); ++textP )
                {
                }
            }

            if( !readNumber(textP, (i == 0)?10:8, (i == 0)?MAXBANK:ADDRMASK, &numbers[i], &endP) )
            {
                editsError(pathP, lineNo, "expected a bank 0 to 15 in decimal and addresses in octal");
            }

            textP = endP;
        }

        for( ; (*textP == ' ') || (*textP == '\t') || (*textP == '\r') || (*textP == '\n'); ++textP )
        {
        }

        if( *textP )
        {
            editsError(pathP, lineNo, "expected the edit and nothing more but a comment");
        }

        editP = newEdit();
        editP->kind = kind;
        editP->bank = (int)numbers[0];
        editP->from = (int)numbers[1];
        editP->to = (kind == RLE_DELETE)?editP->from:(int)numbers[2];
        editP->addr = (kind == RLE_DELETE)?-1:(int)numbers[3];
        editP->fileP = pathP;
        editP->lineNo = lineNo;
    }

    fclose(fP);

    if( !editsGiven )
    {
        editsPathP = pathP;
    }

    editsGiven = 1;
    return(1);
}

// Append a zeroed edit to the list.  The list may move, so an RlEdit pointer
// held across this call goes stale.
// Returns the new edit; out of memory is fatal.
static RlEdit *
newEdit(void)
{
RlEdit *editP;

    if( !(editsP = (RlEdit *)realloc(editsP, ((size_t)(editCount + 1) * sizeof(RlEdit)))) )
    {
        fprintf(stderr, "am1: out of memory reading -O=edits\n");
        exit(1);
    }

    editP = &editsP[editCount++];
    memset(editP, 0, sizeof(RlEdit));
    editP->parent = -1;
    return(editP);
}

// Returns non-zero when an -O=edits file was given.
int
optRelayoutGiven(void)
{
    return(editsGiven);
}

// Read one unsigned number in the given base, of at least one digit and no
// more than max, followed by a space, a tab, a line end or nothing.
// Returns 1 with *valueP set and *endPP after the digits, or 0.
static int
readNumber(const char *textP, int base, long max, long *valueP, const char **endPP)
{
long value;
int digits;

    value = 0;
    digits = 0;

    for( ; (*textP >= '0') && (*textP < ('0' + base)); ++textP )
    {
        value = ((value * base) + (*textP - '0'));
        ++digits;

        if( value > max )
        {
            return(0);
        }
    }

    if( !digits || (*textP && (*textP != ' ') && (*textP != '\t') && (*textP != '\r') && (*textP != '\n')) )
    {
        return(0);
    }

    *valueP = value;
    *endPP = textP;
    return(1);
}

// Refuse an edits file, naming the file and line.  Does not return.
static void
editsError(const char *pathP, int lineNo, const char *msgP)
{
    fprintf(stderr, "am1: -O=edits=%s, line %d: %s\n", pathP, lineNo, msgP);
    exit(1);
}

// The name the dump gives a reason.
// Returns a static string, never NILP.
static const char *
reasonName(RlReason reason)
{
static const char *namesPP[RLR_COUNT] =
{
    "accepted", "noword", "notstatement", "nextword", "unit", "segment", "region",
    "conflict", "overlay", "offset", "dot", "number", "label", "bound", "overlap", "pool",
    "copylabel", "copyinternal", "skipped", "internal", "ceiling"
};

    if( (reason < 0) || (reason >= RLR_COUNT) )
    {
        return("?");
    }

    return(namesPP[reason]);
}

// Silence evalExpr()'s law range warning while relayout evaluates: the code
// generators give it for the program, and a second copy would be misleading.
static void
quiet(void)
{
WarningP warnP;

    savedNoWarn = noWarn;
    noWarn = true;

    for( warnP = warnings; warnP->id; ++warnP )
    {
        if( warnP->id == WARN_LAW )
        {
            savedLawEnabled = warnP->enabled;
            warnP->enabled = false;
        }
    }
}

// Undo quiet().
static void
loud(void)
{
WarningP warnP;

    noWarn = savedNoWarn;

    for( warnP = warnings; warnP->id; ++warnP )
    {
        if( warnP->id == WARN_LAW )
        {
            warnP->enabled = savedLawEnabled;
        }
    }
}

// The pass.

// Apply every edit in order, each laid out and checked before the next, then
// check the final program: idempotent, consistent and every bank reconciled.
// An inconsistent final result is fatal.
void
optRelayout(OptTableP tableP, PNodeP rootP, FILE *dumpP)
{
int i;
int j;
int idempotent;
int accepted;
int refused;
int fileEdits;
int dupCount;
RlReason reclaimReason;
int grown[MAXBANK + 1];
int before[MAXBANK + 1];
int deleted[MAXBANK + 1];
int copied[MAXBANK + 1];
int dropped;
unsigned long sum1;
OptXformP recP;
OptTableP newTableP;
OptWordP entryP;
RlEdit *editP;
RlReason reason;
SymListP listP;
PNodeListP wildP;

    quiet();

    rlTableP = tableP;
    rlRootP = rootP;

    if( !(wordStateP = (unsigned char *)calloc((size_t)tableP->count + 1, 1)) ||
        !(offsetP = (int *)calloc((size_t)tableP->count + 1, sizeof(int))) ||
        !(recordOfP = (OptXformP *)calloc((size_t)tableP->count + 1, sizeof(OptXformP))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        offsetP[i] = (entryP->nodeP)?(entryP->addr - entryP->nodeP->pc):0;
    }

    if( tableP->xformApplied )
    {
        for( recP = tableP->xformsP; recP; recP = recP->nextP )
        {
            if( (recP->fate == OPTXF_FIRED) && recP->findingP && recP->findingP->wordsP[0] )
            {
                recordOfP[recP->findingP->wordsP[0]->index] = recP;
            }
        }
    }

    for( poolCount = 0, listP = constsListP; listP; listP = listP->nextP )
    {
        ++poolCount;
    }

    if( !(poolsPP = (SymNodeP *)calloc((size_t)poolCount + 1, sizeof(SymNodeP))) ||
        !(poolBankP = (int *)calloc((size_t)poolCount + 1, sizeof(int))) ||
        !(poolListsPP = (SymListP *)calloc((size_t)poolCount + 1, sizeof(SymListP))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0, listP = constsListP; listP; listP = listP->nextP, ++i )
    {
        poolsPP[i] = listP->symP;
        poolBankP[i] = listP->bank;
        poolListsPP[i] = listP;
    }

    // The parser keyed a 'sym:*' by its name, and only then made it a BREF,
    // which hashExpr() would key by its address.  Mark every one so the re-key
    // can key it as the parser did.  A copy keeps the mark.
    for( wildP = wildcardsP; wildP; wildP = wildP->nextP )
    {
        if( wildP->nodeP && (wildP->nodeP->type == BREF) )
        {
            wildP->nodeP->flags |= RL_WILD;
        }
    }

    // Every constant reference's value as assembled, before anything moves.
    collectRefs();

    if( refCount && !(oldRefsP = (RlRefValue *)calloc((size_t)refCount, sizeof(RlRefValue))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0; i < refCount; ++i )
    {
        oldRefsP[i].refP = refsPP[i];
        oldRefsP[i].value = evalExpr((PNodeP)(refsPP[i]->value2.ptr));
        oldRefsP[i].slotP = refsPP[i]->value.symP;
    }

    oldRefCount = refCount;
    qsort(oldRefsP, (size_t)oldRefCount, sizeof(RlRefValue), compareRefValues);

    // What made every pool key, before anything moves.
    recordPools();

    // The program as assembled, laid out again.  With no edit this must
    // reproduce every output, the pools' trees included.
    rlCeilingCheck = 1;

    if( ((reason = settle()) != RLR_ACCEPTED) || rekeyChanges )
    {
        fprintf(stderr, "am1: optimizer: relayout: the program as assembled does not lay out again (%s)\n",
            (reason != RLR_ACCEPTED)?reasonName(reason):"its pools rebuild differently");
        exit(1);
    }

    // The pool words the declared rewrites freed, reclaimed before the edits,
    // so each edit is laid out over the program the reclaim leaves.  The
    // ceiling check stays on through it: a reclaim only shrinks a bank.
    reclaimReason = reclaimPools();

    // The transforms' edits follow the file's, each kind after the last, and a
    // single-site callee's deletes follow every inline copy.
    fileEdits = editCount;
    addInlineEdits(tableP);
    addPlaceEdits(tableP);
    addUnrollEdits(tableP);
    addSpaceEdits(tableP);
    addWindowEdits(tableP);

    accepted = 0;
    refused = 0;

    for( j = 0; j < editCount; ++j )
    {
        editP = &editsP[j];

        // A delete with a parent is not tried once the parent was refused.  A
        // callee's delete follows its copy, and every earlier delete of it:
        // once one is refused the rest of the callee stays, dead but checked.
        // A placement's delete of J follows its move; if the move was refused
        // J still jumps to the run, where it always went.
        if( (editP->parent >= 0) &&
            ((editsP[editP->parent].reason != RLR_ACCEPTED) || (editP->inlineP && editP->inlineP->deleteRefused)) )
        {
            editP->reason = RLR_SKIPPED;

            if( editP->inlineP )
            {
                ++editP->inlineP->deleteRefused;
            }

            ++refused;
            continue;
        }

        // A freed eem is redundant only once every lem it depends on is gone.
        if( editP->windelP && !windelDepsMade(tableP, editP->windelP) )
        {
            editP->reason = RLR_SKIPPED;
            editP->windelP->outcome = (int)RLR_SKIPPED;
            ++refused;
            continue;
        }

        rlCeilingCheck = (j < fileEdits);   // see settle()

        switch( editP->kind )
        {
        case RLE_DELETE:
            reason = tryDelete(editP);
            break;

        case RLE_MOVE:
            reason = tryMove(editP);
            break;

        case RLE_INLINE:
            reason = tryInline(editP);
            break;

        case RLE_UNROLL:
            reason = tryUnroll(editP);
            break;

        default:
            reason = tryCopy(editP);
            break;
        }

        if( reason != RLR_ACCEPTED )
        {
            free(editP->entriesP);      // refused before anything was changed
            editP->entriesP = NILP;
            editP->entryCount = 0;
        }
        else
        {
            if( (reason = settle()) == RLR_ACCEPTED )
            {
                reason = checkHazards();
            }

            if( reason == RLR_ACCEPTED )
            {
                reason = checkTable(&dupCount);
            }

            if( reason != RLR_ACCEPTED )
            {
                undoEdit(editP);

                if( settle() != RLR_ACCEPTED )
                {
                    fprintf(stderr, "am1: optimizer: relayout: undoing the edit at %s line %d did not lay out again\n",
                        editP->fileP, editP->lineNo);
                    exit(1);
                }
            }
        }

        editP->reason = reason;

        // Recorded now: a later freed eem's edit reads it.
        if( editP->windelP )
        {
            editP->windelP->outcome = (int)reason;
        }

        if( reason == RLR_ACCEPTED )
        {
            ++accepted;

            for( i = 0; i < editP->entryCount; ++i )
            {
                wordStateP[editP->entriesP[i]] |= (editP->kind == RLE_MOVE)?RLW_MOVED:RLW_DELETED;
            }
        }
        else
        {
            ++refused;

            if( (editP->parent >= 0) && editP->inlineP )
            {
                ++editP->inlineP->deleteRefused;
            }

            // An unroll's setup word kept: it loads AC and stores the dead
            // counter, and the judge made sure neither matters.
            if( (editP->parent >= 0) && editP->unrollP )
            {
                ++editP->unrollP->deleteRefused;
            }
        }
    }

    // The warning below counts the file's edits; the transforms' edits report
    // their own, from inlineOutcomes() and below.
    for( j = 0, refused = 0; j < fileEdits; ++j )
    {
        refused += (editsP[j].reason != RLR_ACCEPTED);
    }

    inlineOutcomes();

    // -O=source brackets each copy that stands with a comment line at each end.
    if( optSourceWanted() )
    {
        noteCopies();
    }

    // The transforms' edits are checked against the declared ceilings once all
    // are made.  Their budgets count to the ceiling, so a bank at or above it is
    // a budget in error, or a setup delete refused.  rlTop is the last settle()'s.
    for( i = 0; i <= MAXBANK; ++i )
    {
        if( optBankCeilingDeclared(tableP, i) && (rlTop[i] >= optBankCeilingDeclared(tableP, i)) )
        {
            fprintf(stderr, "am1: optimizer: relayout: internal error: the rewrites leave bank %d's highest word at %04o, at or above its %%%%ceiling %04o\n",
                i, rlTop[i], optBankCeilingDeclared(tableP, i));
            exit(1);
        }
    }

    // Idempotence: laying the result out again changes nothing.
    sum1 = checksum();
    settle();
    idempotent = ((checksum() == sum1) && !rekeyChanges);

    if( checkTable(&dupCount) != RLR_ACCEPTED )
    {
        fprintf(stderr, "am1: optimizer: relayout: the final program's word table is not consistent\n");
        exit(1);
    }

    if( !idempotent )
    {
        fprintf(stderr, "am1: optimizer: relayout: laying the program out a second time changed it\n");
        exit(1);
    }

    // Reconcile every bank: the words as assembled, less those deleted or no
    // longer pooled, plus the pool slots made and the words the copies cloned,
    // are the words there now.
    memset(before, 0, sizeof(before));
    memset(deleted, 0, sizeof(deleted));
    poolChanges(grown, &dropped);
    copiedWords(copied);

    for( i = 0; i < tableP->count; ++i )
    {
        entryP = tableP->entriesPP[i];
        ++before[entryP->bank];

        if( currentAddr(i) < 0 )
        {
            ++deleted[entryP->bank];
        }
    }

    newTableP = optBuildTable(rootP);

    for( i = 0; newTableP && (i <= MAXBANK); ++i )
    {
        if( (before[i] - deleted[i] + grown[i] + copied[i]) !=
            ((newTableP->banksP[i])?newTableP->banksP[i]->count:0) )
        {
            fprintf(stderr, "am1: optimizer: relayout: bank %d does not reconcile\n", i);
            exit(1);
        }
    }

    optFreeTable(newTableP);

    // What the reclaim took, for the report's Realized line, and the words the
    // pool rebuild added or merged, which the reclaim section names.  A new
    // slot usually stands in for a dropped one whose key moved with the code,
    // so the change is the new slots less the dropped ones the reclaim does not
    // account for; negative when constants came to share a slot.
    tableP->poolReclaimed = reclaimedSlots;

    for( tableP->poolGrown = 0, i = 0; i <= MAXBANK; ++i )
    {
        tableP->poolGrown += grown[i];
    }

    tableP->poolGrown -= (dropped - reclaimedSlots);

    if( dumpP )
    {
        dump(dumpP, idempotent);
    }

    loud();

    // A refused reclaim leaves a correct program, but a saving the report has
    // promised is lost, so it is warned of as a refused deletion is.
    if( reclaimReason != RLR_ACCEPTED )
    {
        fprintf(stderr, "am1: warning: relayout: the %d pool word%s the declared rewrites freed could not be reclaimed (%s), so %s kept\n",
            tableP->poolFreeable, (tableP->poolFreeable == 1)?"":"s", reasonName(reclaimReason),
            (tableP->poolFreeable == 1)?"it is":"they are");
    }

    if( refused )
    {
        fprintf(stderr, "am1: warning: relayout: %d of the %d edits in %s were refused; -O=relayout names the reasons\n",
            refused, fileEdits, editsPathP);
    }

    for( j = fileEdits; j < editCount; ++j )
    {
        editP = &editsP[j];

        if( (editP->kind == RLE_UNROLL) && (editP->reason != RLR_ACCEPTED) )
        {
            fprintf(stderr, "am1: warning: unroll: relayout refused the loop at bank %d %04o (%s), so it stays; %s:%d\n",
                editP->bank, editP->from, reasonName(editP->reason), (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( (editP->kind == RLE_UNROLL) && editP->unrollP->deleteRefused )
        {
            fprintf(stderr, "am1: warning: unroll: the loop at bank %d %04o was unrolled and %d of its setup words kept; %s:%d\n",
                editP->bank, editP->from, editP->unrollP->deleteRefused, (editP->fileP)?editP->fileP:"-",
                editP->lineNo);
        }
        else if( (editP->kind == RLE_INLINE) && (editP->reason != RLR_ACCEPTED) )
        {
            fprintf(stderr, "am1: warning: inline: relayout refused the copy at bank %d %04o (%s), so the call stays; %s:%d\n",
                editP->bank, editP->addr, reasonName(editP->reason), (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( (editP->kind == RLE_INLINE) && editP->inlineP->deleteRefused )
        {
            fprintf(stderr, "am1: warning: inline: the callee at bank %d %04o was copied and not wholly deleted (%d of its deletes refused); %s:%d\n",
                editP->bank, editP->inlineP->shape.entryP->addr, editP->inlineP->deleteRefused,
                (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( editP->placeP && (editP->kind == RLE_MOVE) && (editP->reason != RLR_ACCEPTED) )
        {
            fprintf(stderr, "am1: warning: place: relayout refused the move after bank %d %04o (%s), so the jmp stays; %s:%d\n",
                editP->bank, editP->addr, reasonName(editP->reason), (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( editP->placeP && (editP->kind == RLE_DELETE) && (editP->reason != RLR_ACCEPTED) &&
            (editP->reason != RLR_SKIPPED) )
        {
            fprintf(stderr, "am1: warning: place: the run was moved and relayout refused the jmp's delete at bank %d %04o (%s); %s:%d\n",
                editP->bank, editP->from, reasonName(editP->reason), (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( editP->xformP && (editP->reason != RLR_ACCEPTED) )
        {
            fprintf(stderr, "am1: warning: space: relayout refused %s's delete at bank %d %04o (%s), so its words stay as written; %s:%d\n",
                optRuleName(editP->xformP->findingP->rule), editP->bank, editP->from, reasonName(editP->reason),
                (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( editP->windelP && (editP->reason == RLR_SKIPPED) )
        {
            fprintf(stderr, "am1: warning: window: the %s at bank %d %04o stays, because a lem it depends on stays; %s:%d\n",
                optWinDelKindName(editP->windelP->kind), editP->bank, editP->from,
                (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
        else if( editP->windelP && (editP->reason != RLR_ACCEPTED) )
        {
            fprintf(stderr, "am1: warning: window: relayout refused the %s's delete at bank %d %04o (%s), so it stays; %s:%d\n",
                optWinDelKindName(editP->windelP->kind), editP->bank, editP->from, reasonName(editP->reason),
                (editP->fileP)?editP->fileP:"-", editP->lineNo);
        }
    }
}

// Append, for every FIRED placement, a move of its run to follow its jmp J and
// then a delete of J that depends on the move.  Move first: a refused move then
// leaves J jumping where it always did, its label on the right word.
static void
addPlaceEdits(OptTableP tableP)
{
OptPlaceP pP;
RlEdit *editP;
int parent;

    for( pP = tableP->placesP; pP; pP = pP->nextP )
    {
        if( pP->fate != OPTPL_FIRED )
        {
            continue;
        }

        editP = newEdit();
        editP->kind = RLE_MOVE;
        editP->bank = pP->shape.jmpP->bank;
        editP->from = pP->shape.targetP->addr;
        editP->to = pP->shape.endP->addr;
        editP->addr = pP->shape.jmpP->addr;
        editP->fileP = pP->shape.jmpP->fileP;
        editP->lineNo = pP->shape.jmpP->lineNo;
        editP->placeP = pP;
        parent = (editCount - 1);

        editP = newEdit();
        editP->kind = RLE_DELETE;
        editP->bank = pP->shape.jmpP->bank;
        editP->from = pP->shape.jmpP->addr;
        editP->to = pP->shape.jmpP->addr;
        editP->addr = -1;
        editP->fileP = pP->shape.jmpP->fileP;
        editP->lineNo = pP->shape.jmpP->lineNo;
        editP->placeP = pP;
        editP->parent = parent;
    }
}

// Append a delete for every FIRED deleting rule, carrying the record, whose
// kept word's new expression tryDelete() installs with it.
static void
addSpaceEdits(OptTableP tableP)
{
OptXformP recP;
RlEdit *editP;

    for( recP = tableP->xformsP; recP; recP = recP->nextP )
    {
        if( (recP->fate != OPTXF_FIRED) || !recP->delP || !optRuleDeletes(recP->findingP->rule) )
        {
            continue;
        }

        editP = newEdit();
        editP->kind = RLE_DELETE;
        editP->bank = recP->delP->bank;
        editP->from = recP->delP->addr;
        editP->to = recP->delP->addr;
        editP->addr = -1;
        editP->fileP = recP->findingP->wordsP[0]->fileP;
        editP->lineNo = recP->findingP->wordsP[0]->lineNo;
        editP->xformP = recP;
    }
}

// Append a delete for every FIRED extend-window record, in record order:
// the redundant words and dead lems before the freed eems that depend on them.
static void
addWindowEdits(OptTableP tableP)
{
OptWinDelP delP;
RlEdit *editP;

    for( delP = tableP->windelsP; delP; delP = delP->nextP )
    {
        if( delP->fate != OPTWD_FIRED )
        {
            continue;
        }

        editP = newEdit();
        editP->kind = RLE_DELETE;
        editP->bank = delP->wordP->bank;
        editP->from = delP->wordP->addr;
        editP->to = delP->wordP->addr;
        editP->addr = -1;
        editP->fileP = delP->wordP->fileP;
        editP->lineNo = delP->wordP->lineNo;
        editP->windelP = delP;
    }
}

// Test whether relayout made the delete of every lem a freed eem depends on.
// Those edits come first, so each already has its outcome.
// Returns 1 if it made them all (or there are none), 0 if not.
static int
windelDepsMade(OptTableP tableP, OptWinDelP delP)
{
OptWinDelP depP;
int i;

    for( i = 0; i < delP->depCount; ++i )
    {
        for( depP = tableP->windelsP; depP; depP = depP->nextP )
        {
            if( depP->wordP == delP->depsPP[i] )
            {
                break;
            }
        }

        if( !depP || (depP->outcome != (int)RLR_ACCEPTED) )
        {
            return(0);
        }
    }

    return(1);
}

// The unrolls' edits.

// Append, for every FIRED loop, its RLE_UNROLL edit (the body H..I-1 copied
// n - 1 times over the isp I, the jmp J deleted), then dependent deletes of the
// dac D and the setup S.  D first, so a label on S moves to H, not to D.
static void
addUnrollEdits(OptTableP tableP)
{
OptUnrollP uP;
OptUnrollShapeP sP;
RlEdit *editP;
int parent;

    for( uP = tableP->unrollsP; uP; uP = uP->nextP )
    {
        if( uP->fate != OPTUN_FIRED )
        {
            continue;
        }

        sP = &uP->shape;
        editP = newEdit();
        editP->kind = RLE_UNROLL;
        editP->bank = sP->ispP->bank;
        editP->from = sP->headP->addr;
        editP->to = (sP->ispP->addr - 1);
        editP->addr = sP->ispP->addr;
        editP->repeat = (sP->trips - 1);
        editP->fileP = sP->ispP->fileP;
        editP->lineNo = sP->ispP->lineNo;
        editP->unrollP = uP;
        parent = (editCount - 1);

        editP = newEdit();
        editP->kind = RLE_DELETE;
        editP->bank = sP->ispP->bank;
        editP->from = sP->depP->addr;
        editP->to = sP->depP->addr;
        editP->addr = -1;
        editP->fileP = sP->ispP->fileP;
        editP->lineNo = sP->ispP->lineNo;
        editP->unrollP = uP;
        editP->parent = parent;

        editP = newEdit();
        editP->kind = RLE_DELETE;
        editP->bank = sP->ispP->bank;
        editP->from = sP->setP->addr;
        editP->to = sP->setP->addr;
        editP->addr = -1;
        editP->fileP = sP->ispP->fileP;
        editP->lineNo = sP->ispP->lineNo;
        editP->unrollP = uP;
        editP->parent = parent;
    }
}

// Unroll one loop: copy the body repeat times over the isp, as tryCopy() does,
// and delete the jmp J, both undone together if either is refused.
// Returns RLR_ACCEPTED with the edit applied, or the refusal, nothing changed.
static RlReason
tryUnroll(RlEdit *editP)
{
OptUnrollShapeP sP;
OptWordP entryP;
OptWordP jmpP;
PNodeP stmtP;
RlEdit jmpEdit;
RlReason reason;
int *entriesP;
int i;

    sP = &editP->unrollP->shape;

    // A T3 or T8 number in the loop would be re-pointed in the original only;
    // planning keeps fired rewrites out, so this is a guard.
    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];

        if( (entryP->bank == editP->bank) && (entryP->addr >= sP->setP->addr) && (entryP->addr <= sP->jmpP->addr) &&
            recordOfP[i] && recordOfP[i]->numberP )
        {
            return(RLR_NUMBER);
        }
    }

    // J, as a delete takes it, and a word of one statement.
    if( !(jmpP = wordAt(rlTableP, editP->bank, sP->jmpP->addr)) )
    {
        return(RLR_NOWORD);
    }

    if( wordStateP[jmpP->index] )
    {
        return(RLR_CONFLICT);
    }

    if( (jmpP->kind != OPTK_EXPR) || !(stmtP = jmpP->nodeP) || (offsetP[jmpP->index] != 0) ||
        ((stmtP->type != EXPR) && (stmtP->type != LOCATION) && (stmtP->type != LCLLOCATION)) ||
        (statementSize(stmtP) != 1) )
    {
        return(RLR_NOTSTATEMENT);
    }

    if( jmpP->flags & OPTF_DUPADDR )
    {
        return(RLR_OVERLAY);
    }

    if( (reason = tryCopy(editP)) != RLR_ACCEPTED )
    {
        return(reason);
    }

    // J's delete, with its own undo state, kept on the edit.
    memset(&jmpEdit, 0, sizeof(jmpEdit));
    deleteWord(&jmpEdit, stmtP);

    if( jmpP->labelsP && !nextWordFollows(jmpEdit.markP->leftP) )
    {
        undoDelete(&jmpEdit);
        undoEdit(editP);
        return(RLR_NEXTWORD);
    }

    editP->jStmtP = jmpEdit.stmtP;
    editP->jMarkP = jmpEdit.markP;
    editP->jSavedValue = jmpEdit.savedValue;
    editP->jSavedValue2 = jmpEdit.savedValue2;

    if( !(entriesP = (int *)realloc(editP->entriesP, (2 * sizeof(int)))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    entriesP[1] = jmpP->index;
    editP->entriesP = entriesP;
    editP->entryCount = 2;
    return(RLR_ACCEPTED);
}

// Put back the jmp an unroll deleted, if it did.
static void
undoJmpDelete(RlEdit *editP)
{
RlEdit jmpEdit;

    if( !editP->jStmtP )
    {
        return;
    }

    memset(&jmpEdit, 0, sizeof(jmpEdit));
    jmpEdit.stmtP = editP->jStmtP;
    jmpEdit.markP = editP->jMarkP;
    jmpEdit.savedValue = editP->jSavedValue;
    jmpEdit.savedValue2 = editP->jSavedValue2;
    undoDelete(&jmpEdit);
    editP->jStmtP = NILP;
    editP->jMarkP = NILP;
}

// The inlines' edits.

// Append a copy for every FIRED inline, its body over the call word; then, per
// single-site callee, deletes of entry to return and of a return data word
// outside the body.  A jda's Y is kept: the copy's "dac Y" writes it.
static void
addInlineEdits(OptTableP tableP)
{
OptInlineP inP;
OptWordP wordP;
RlEdit *editP;
int addr;
int parent;
int first;

    first = editCount;

    for( inP = tableP->inlinesP; inP; inP = inP->nextP )
    {
        if( inP->fate != OPTIN_FIRED )
        {
            continue;
        }

        editP = newEdit();
        editP->kind = RLE_INLINE;
        editP->bank = inP->siteP->bank;
        editP->from = (inP->shape.entryP->addr + 1);
        editP->to = (inP->shape.lastP->addr - 1);
        editP->addr = inP->siteP->addr;
        editP->fileP = inP->siteP->fileP;
        editP->lineNo = inP->siteP->lineNo;
        editP->inlineP = inP;
    }

    for( parent = first; parent < editCount; ++parent )
    {
        if( (editsP[parent].kind != RLE_INLINE) || !(inP = editsP[parent].inlineP)->deletes )
        {
            continue;
        }

        for( addr = inP->shape.entryP->addr; addr <= inP->shape.lastP->addr; ++addr )
        {
            editP = newEdit();
            editP->kind = RLE_DELETE;
            editP->bank = inP->siteP->bank;
            editP->from = addr;
            editP->to = addr;
            editP->addr = -1;
            editP->fileP = inP->siteP->fileP;
            editP->lineNo = inP->siteP->lineNo;
            editP->inlineP = inP;
            editP->parent = parent;
        }

        wordP = inP->shape.rtnP;

        if( !inP->shape.dapForm && wordP &&
            ((wordP->addr < inP->shape.entryP->addr) || (wordP->addr > inP->shape.lastP->addr)) )
        {
            editP = newEdit();
            editP->kind = RLE_DELETE;
            editP->bank = wordP->bank;
            editP->from = wordP->addr;
            editP->to = wordP->addr;
            editP->addr = -1;
            editP->fileP = inP->siteP->fileP;
            editP->lineNo = inP->siteP->lineNo;
            editP->inlineP = inP;
            editP->parent = parent;
        }
    }
}

// Make one inline's copy over the call word as tryCopy() does, then give each
// retargeted word its "jmp .+k" to the copy's end and a jda its "dac Y".
// Returns RLR_ACCEPTED with the edit applied, or the refusal, nothing changed.
static RlReason
tryInline(RlEdit *editP)
{
OptInlineP inP;
OptWordP entryP;
PNodeP origP;
PNodeP cloneP;
PNodeP stmtP;
PNodeP termP;
PNodeP endP;
RlReason reason;
int i;
int terminated;
int lastAt;

    inP = editP->inlineP;

    // A T3 far target or a T8d written as a number is re-pointed in the
    // original only; the clone's would name the old word, so refuse.
    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];

        if( (entryP->bank == editP->bank) && (entryP->addr >= editP->from) && (entryP->addr <= editP->to) &&
            recordOfP[i] && recordOfP[i]->numberP )
        {
            return(RLR_NUMBER);
        }
    }

    if( (reason = tryCopy(editP)) != RLR_ACCEPTED )
    {
        return(reason);
    }

    // The clone is the run node for node, so the two lists are walked
    // together; the run ends at its last statement's line end.
    lastAt = currentAddr(inP->shape.lastP->index);
    endP = lineEnd(wordAt(rlTableP, editP->bank, editP->to)->nodeP, &terminated);
    origP = wordAt(rlTableP, editP->bank, editP->from)->nodeP;
    cloneP = editP->copyFirstP;

    for( ;; )
    {
        for( i = 0; i < inP->retargetCount; ++i )
        {
            if( inP->retargetsPP[i]->nodeP == origP )
            {
                inP->offsetsPP[i]->value.ival = (lastAt - currentAddr(inP->retargetsPP[i]->index));
                cloneP->rightP = inP->retargetTreesPP[i];
            }
        }

        if( (origP == endP) || !cloneP )
        {
            break;
        }

        origP = origP->leftP;
        cloneP = cloneP->leftP;
    }

    if( inP->prefixP )
    {
        stmtP = newMarker(inP->siteP->nodeP, 0);
        stmtP->type = EXPR;
        stmtP->value.ival = 0;
        stmtP->rightP = inP->prefixP;
        termP = newMarker(inP->siteP->nodeP, 0);
        termP->type = TERMINATOR;
        termP->value.ival = 0;

        stmtP->leftP = termP;
        termP->leftP = editP->copyFirstP;
        editP->copyAfterP->leftP = stmtP;
        editP->copyFirstP = stmtP;
        ++editP->copyWords;
    }

    return(RLR_ACCEPTED);
}

// Tell -O=source about every copy that stands: an inline's, an unroll's, and
// an -O=edits copy.  The clone chain is the copy, its prefix included.
static void
noteCopies(void)
{
int j;
RlEdit *editP;
OptWordP entryP;
char what[128];

    for( j = 0; j < editCount; ++j )
    {
        editP = &editsP[j];

        if( (editP->reason != RLR_ACCEPTED) || !editP->copyFirstP )
        {
            continue;
        }

        switch( editP->kind )
        {
        case RLE_INLINE:
            entryP = editP->inlineP->shape.entryP;
            if( entryP->labelsP )
            {
                snprintf(what, sizeof(what), "the inline copy of %s, for the call on line %d",
                    entryP->labelsP->symP->name, editP->lineNo);
            }
            else
            {
                snprintf(what, sizeof(what), "the inline copy of the routine at %04o, for the call on line %d",
                    entryP->addr, editP->lineNo);
            }
            break;

        case RLE_UNROLL:
            snprintf(what, sizeof(what), "the loop on line %d unrolled: %d more copies of its body",
                editP->lineNo, editP->repeat);
            break;

        default:
            snprintf(what, sizeof(what), "the -O=edits copy of %04o-%04o over %04o",
                editP->from, editP->to, editP->addr);
            break;
        }

        srcNoteCopy(editP->copyFirstP, editP->copyLastP, what);
    }
}

// Write back into each transform's record what relayout made of its edits:
// the outcome, and how many of its deletes were made.
static void
inlineOutcomes(void)
{
int j;
RlEdit *editP;

    for( j = 0; j < editCount; ++j )
    {
        editP = &editsP[j];

        // An eem or lem's delete; its outcome was recorded as it was made.
        if( editP->windelP )
        {
            editP->windelP->outcomeP = reasonName(editP->reason);
            rlTableP->windelRelaid = 1;

            if( editP->reason == RLR_ACCEPTED )
            {
                ++rlTableP->windelMade;
            }

            continue;
        }

        // A deleting rule's delete, and with it its kept word.
        if( editP->xformP )
        {
            editP->xformP->outcomeP = reasonName(editP->reason);
            rlTableP->spaceRelaid = 1;

            if( editP->reason == RLR_ACCEPTED )
            {
                ++rlTableP->spaceMade;
            }

            continue;
        }

        // An unroll's copies, and its setup's deletes.
        if( editP->unrollP )
        {
            if( editP->kind == RLE_UNROLL )
            {
                editP->unrollP->outcome = (int)editP->reason;
                editP->unrollP->outcomeP = reasonName(editP->reason);
            }
            else if( editP->reason == RLR_ACCEPTED )
            {
                ++editP->unrollP->deleted;
            }

            continue;
        }

        // A placement's move and its delete of J.
        if( editP->placeP )
        {
            if( editP->kind == RLE_MOVE )
            {
                editP->placeP->outcome = (int)editP->reason;
                editP->placeP->outcomeP = reasonName(editP->reason);
            }
            else if( editP->reason != RLR_SKIPPED )
            {
                editP->placeP->deleteOutcome = (int)editP->reason;
                editP->placeP->deleteOutcomeP = reasonName(editP->reason);
            }

            continue;
        }

        if( !editP->inlineP )
        {
            continue;
        }

        if( editP->kind == RLE_INLINE )
        {
            editP->inlineP->outcome = (int)editP->reason;
            editP->inlineP->outcomeP = reasonName(editP->reason);
        }
        else if( editP->reason == RLR_ACCEPTED )
        {
            ++editP->inlineP->deleted;
        }
    }
}

// The edits.

// Apply a delete if this pass can make it, recording how to undo it.
// Returns RLR_ACCEPTED with the edit applied, or the refusal, nothing changed.
static RlReason
tryDelete(RlEdit *editP)
{
OptWordP entryP;
OptWordP keepP;
PNodeP stmtP;

    if( !(entryP = wordAt(rlTableP, editP->bank, editP->from)) )
    {
        return(RLR_NOWORD);
    }

    // First, since an earlier edit may have changed the statement's node.
    if( wordStateP[entryP->index] )
    {
        return(RLR_CONFLICT);
    }

    // The word a deleting rule keeps must still be the statement it was,
    // holding the expression it was assembled from.
    keepP = (editP->xformP)?editP->xformP->keepP:NILP;

    if( keepP && (wordStateP[keepP->index] || !keepP->nodeP || (keepP->nodeP->rightP != keepP->exprP) ||
        !editP->xformP->treeP) )
    {
        return(RLR_CONFLICT);
    }

    if( (entryP->kind != OPTK_EXPR) || !(stmtP = entryP->nodeP) ||
        ((stmtP->type != EXPR) && (stmtP->type != LOCATION) && (stmtP->type != LCLLOCATION)) )
    {
        return(RLR_NOTSTATEMENT);
    }

    if( entryP->flags & OPTF_DUPADDR )
    {
        return(RLR_OVERLAY);
    }

    deleteWord(editP, stmtP);

    // A label on the word, on its line or on a line before it, moves to the
    // next word; there has to be one before a fixed point.
    if( entryP->labelsP && !nextWordFollows(editP->markP->leftP) )
    {
        undoDelete(editP);
        return(RLR_NEXTWORD);
    }

    // The kept word's new expression goes in with the delete.
    if( keepP )
    {
        editP->keepStmtP = keepP->nodeP;
        editP->keepOldP = keepP->nodeP->rightP;
        keepP->nodeP->rightP = editP->xformP->treeP;
    }

    if( !(editP->entriesP = (int *)malloc(sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    editP->entriesP[0] = entryP->index;
    editP->entryCount = 1;
    return(RLR_ACCEPTED);
}

// Apply a move if this pass can make it, recording how to undo it.
// Returns RLR_ACCEPTED with the edit applied, or the refusal, nothing changed.
static RlReason
tryMove(RlEdit *editP)
{
OptWordP fromP;
OptWordP toP;
OptWordP addrP;
OptWordP entryP;
PNodeP firstP;
PNodeP lastP;
PNodeP endP;
PNodeP destP;
PNodeP nodeP;
PNodeP beforeP;
int terminated;
int found;
int fixed;
int i;
unsigned int regionBits;
RlReason reason;

    fromP = wordAt(rlTableP, editP->bank, editP->from);
    toP = wordAt(rlTableP, editP->bank, editP->to);
    addrP = wordAt(rlTableP, editP->bank, editP->addr);

    if( !fromP || !toP || !addrP )
    {
        return(RLR_NOWORD);
    }

    // First, since an earlier edit may have changed a statement's node.  The
    // words between FROM and TO are checked once the run is known.
    if( wordStateP[fromP->index] || wordStateP[toP->index] || wordStateP[addrP->index] )
    {
        return(RLR_CONFLICT);
    }

    if( !(firstP = fromP->nodeP) || !(lastP = toP->nodeP) || !addrP->nodeP ||
        (fromP->kind == OPTK_VAR) || (toP->kind == OPTK_VAR) || (addrP->kind == OPTK_VAR) ||
        (offsetP[fromP->index] != 0) || (offsetP[toP->index] != (statementSize(lastP) - 1)) ||
        (offsetP[addrP->index] != (statementSize(addrP->nodeP) - 1)) )
    {
        return(RLR_NOTSTATEMENT);
    }

    // The run: every statement from the first through the last, which must
    // follow it, and the last one's line end.
    reason = RLR_ACCEPTED;

    for( nodeP = firstP; nodeP && (nodeP != lastP); nodeP = nodeP->leftP )
    {
    }

    if( !nodeP )
    {
        return(RLR_NOTSTATEMENT);
    }

    // A placement carries the label lines that name its first word.
    if( editP->placeP )
    {
        firstP = labelLinesBefore(firstP);
    }

    if( !(endP = lineEnd(lastP, &terminated)) || !terminated )
    {
        return(RLR_UNIT);
    }

    for( nodeP = firstP; ; nodeP = nodeP->leftP )
    {
        switch( nodeP->type )
        {
        case ORIGIN:
        case BANK:
        case CONSTANTS:
        case VARS:
        case VAR:
        case OPTIMIZE:
        case ENDOPTIMIZE:
        case IMPORT:
        case EXPORT:
        case FILENAME:
        case RELAYOUT:
            reason = RLR_UNIT;
            break;

        default:
            break;
        }

        nodeP->flags |= RL_TAG;

        if( nodeP == endP )
        {
            break;
        }
    }

    destP = lineEnd(addrP->nodeP, &terminated);

    if( (reason == RLR_ACCEPTED) && !terminated )
    {
        reason = RLR_UNIT;
    }

    if( (reason == RLR_ACCEPTED) && (addrP->nodeP->flags & RL_TAG) )
    {
        reason = RLR_CONFLICT;
    }

    // The words it moves.
    editP->entryCount = 0;

    if( !(editP->entriesP = (int *)malloc(((size_t)rlTableP->count + 1) * sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];

        if( entryP->nodeP && (entryP->kind != OPTK_VAR) && (entryP->nodeP->flags & RL_TAG) )
        {
            editP->entriesP[editP->entryCount++] = i;

            if( (reason == RLR_ACCEPTED) && wordStateP[i] )
            {
                reason = RLR_CONFLICT;
            }

            if( (reason == RLR_ACCEPTED) && (entryP->flags & OPTF_DUPADDR) )
            {
                reason = RLR_OVERLAY;
            }
        }
    }

    for( nodeP = firstP; ; nodeP = nodeP->leftP )
    {
        nodeP->flags &= ~RL_TAG;

        if( nodeP == endP )
        {
            break;
        }
    }

    if( reason != RLR_ACCEPTED )
    {
        return(reason);
    }

    if( wordStateP[addrP->index] )
    {
        return(RLR_CONFLICT);
    }

    if( addrP->flags & OPTF_DUPADDR )
    {
        return(RLR_OVERLAY);
    }

    // The same segment: no origin and no bank directive between the run and
    // where it goes, in either direction.
    found = 0;
    fixed = 0;

    for( nodeP = endP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP == addrP->nodeP )
        {
            found = 1;
            break;
        }

        fixed |= isFixedPoint(nodeP);
    }

    if( !found )
    {
        fixed = 0;

        for( nodeP = rlRootP->leftP; nodeP && (nodeP != firstP); nodeP = nodeP->leftP )
        {
            if( nodeP == addrP->nodeP )
            {
                found = 1;
                fixed = 0;
            }
            else if( found )
            {
                fixed |= isFixedPoint(nodeP);
            }
        }
    }

    if( !found )
    {
        return(RLR_INTERNAL);
    }

    if( fixed )
    {
        return(RLR_SEGMENT);
    }

    regionBits = (OPTF_INREGION | OPTF_HANDSOFF);

    if( (fromP->flags & regionBits) != (addrP->flags & regionBits) )
    {
        return(RLR_REGION);
    }

    // Relink: a marker where the run stood, and the run between a pair of
    // markers after the destination's line end.
    editP->firstP = firstP;
    editP->endP = endP;
    editP->nextP = endP->leftP;
    editP->linkPP = linkTo(firstP);
    editP->destP = destP;
    editP->movedFromP = newMarker(firstP, RL_MOVEDFROM);
    editP->beginP = newMarker(firstP, RL_BEGIN);
    editP->runEndP = newMarker(endP, RL_END);
    editP->movedFromP->value2.ptr = editP->beginP;
    editP->beginP->value2.ptr = editP->runEndP;

    *editP->linkPP = editP->movedFromP;
    editP->movedFromP->leftP = editP->nextP;

    beforeP = destP->leftP;
    destP->leftP = editP->beginP;
    editP->beginP->leftP = firstP;
    endP->leftP = editP->runEndP;
    editP->runEndP->leftP = beforeP;

    return(RLR_ACCEPTED);
}

// Apply a copy if this pass can make it, recording how to undo it.  The run
// stays; the word at ADDR is deleted and a clone of the run spliced after its
// line.  Returns RLR_ACCEPTED with the edit applied, or the refusal, nothing changed.
static RlReason
tryCopy(RlEdit *editP)
{
OptWordP fromP;
OptWordP toP;
OptWordP addrP;
OptWordP entryP;
OptWordP *runPP;
OptEdgeP edgeP;
PNodeP firstP;
PNodeP lastP;
PNodeP endP;
PNodeP destEndP;
PNodeP nodeP;
PNodeP headP;
PNodeP tailP;
int terminated;
int runCount;
int copies;
int words;
int i;
int k;
RlReason reason;

    fromP = wordAt(rlTableP, editP->bank, editP->from);
    toP = wordAt(rlTableP, editP->bank, editP->to);
    addrP = wordAt(rlTableP, editP->bank, editP->addr);

    if( !fromP || !toP || !addrP )
    {
        return(RLR_NOWORD);
    }

    // First, since an earlier edit may have changed a statement's node.  The
    // words between FROM and TO are checked once the run is known.
    if( wordStateP[fromP->index] || wordStateP[toP->index] || wordStateP[addrP->index] )
    {
        return(RLR_CONFLICT);
    }

    // The run, as a move takes it: whole statements, the first word of the
    // first and the last word of the last.
    if( !(firstP = fromP->nodeP) || !(lastP = toP->nodeP) ||
        (fromP->kind == OPTK_VAR) || (toP->kind == OPTK_VAR) ||
        (offsetP[fromP->index] != 0) || (offsetP[toP->index] != (statementSize(lastP) - 1)) )
    {
        return(RLR_NOTSTATEMENT);
    }

    // The destination, as a delete takes it, and the one word of its
    // statement, since the copy stands in its place.
    if( (addrP->kind != OPTK_EXPR) || !addrP->nodeP || (offsetP[addrP->index] != 0) ||
        ((addrP->nodeP->type != EXPR) && (addrP->nodeP->type != LOCATION) &&
         (addrP->nodeP->type != LCLLOCATION)) || (statementSize(addrP->nodeP) != 1) )
    {
        return(RLR_NOTSTATEMENT);
    }

    for( nodeP = firstP; nodeP && (nodeP != lastP); nodeP = nodeP->leftP )
    {
    }

    if( !nodeP )
    {
        return(RLR_NOTSTATEMENT);
    }

    if( !(endP = lineEnd(lastP, &terminated)) || !terminated )
    {
        return(RLR_UNIT);
    }

    // A directive a move cannot carry, a copy cannot clone either: the layout
    // would meet it twice.  A label the copy would define a second time, taking
    // every reference to the first with it.
    reason = RLR_ACCEPTED;

    for( nodeP = firstP; ; nodeP = nodeP->leftP )
    {
        switch( nodeP->type )
        {
        case ORIGIN:
        case BANK:
        case CONSTANTS:
        case VARS:
        case VAR:
        case OPTIMIZE:
        case ENDOPTIMIZE:
        case IMPORT:
        case EXPORT:
        case FILENAME:
        case RELAYOUT:
            if( reason == RLR_ACCEPTED )
            {
                reason = RLR_UNIT;
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            // An unroll's body may start on its loop's label, which the copies
            // are made without.  Only a word the label's line holds whole,
            // one word, becomes a plain statement.
            if( (editP->kind == RLE_UNROLL) && (nodeP == firstP) && (statementSize(nodeP) == 1) &&
                !(nodeP->rightP->flags & PN_NOINC) )
            {
                break;
            }

            if( reason == RLR_ACCEPTED )
            {
                reason = RLR_COPYLABEL;
            }
            break;

        default:
            break;
        }

        nodeP->flags |= RL_TAG;

        if( nodeP == endP )
        {
            break;
        }
    }

    // The destination's line has to have an end to splice after, and may not
    // be in the run: a run holding its own destination would be cloned with
    // the RELAYOUT node the deletion leaves in it.
    destEndP = lineEnd(addrP->nodeP, &terminated);

    if( (reason == RLR_ACCEPTED) && !terminated )
    {
        reason = RLR_UNIT;
    }

    if( (reason == RLR_ACCEPTED) && (addrP->nodeP->flags & RL_TAG) )
    {
        reason = RLR_CONFLICT;
    }

    // The words the run holds, which are the words the copy clones.
    runCount = 0;

    if( !(runPP = (OptWordP *)malloc(((size_t)rlTableP->count + 1) * sizeof(OptWordP))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];

        if( entryP->nodeP && (entryP->kind != OPTK_VAR) && (entryP->nodeP->flags & RL_TAG) )
        {
            runPP[runCount++] = entryP;

            if( (reason == RLR_ACCEPTED) && wordStateP[i] )
            {
                reason = RLR_CONFLICT;
            }

            if( (reason == RLR_ACCEPTED) && (entryP->flags & OPTF_DUPADDR) )
            {
                reason = RLR_OVERLAY;
            }
        }
    }

    for( nodeP = firstP; ; nodeP = nodeP->leftP )
    {
        nodeP->flags &= ~RL_TAG;

        if( nodeP == endP )
        {
            break;
        }
    }

    // A word of the run that names a word of the run would still name the
    // original's word in the copy.  The model is -O=speed's copyable test,
    // edge for edge, so what it calls copyable is what this will copy.
    for( i = 0; (reason == RLR_ACCEPTED) && (i < runCount); ++i )
    {
        for( edgeP = runPP[i]->outP; edgeP; edgeP = edgeP->nextOutP )
        {
            if( inRun(runPP, runCount, edgeP->toBank, edgeP->toAddr) )
            {
                reason = RLR_COPYINTERNAL;
                break;
            }
        }
    }

    if( (reason == RLR_ACCEPTED) && !runCount )
    {
        reason = RLR_NOTSTATEMENT;      // a run that emits nothing is not a run of words
    }

    free(runPP);

    if( reason != RLR_ACCEPTED )
    {
        return(reason);
    }

    if( addrP->flags & OPTF_DUPADDR )
    {
        return(RLR_OVERLAY);
    }

    // Delete the destination word, then splice the clone after its line, so
    // that a label left on that line labels the copy's first word.
    deleteWord(editP, addrP->nodeP);
    destEndP = lineEnd(editP->markP, &terminated);

    // An unroll's copies are chained, repeat of them, each first node made a
    // plain statement when it held the loop's label; a loop of one trip gets
    // none, its body standing as written.
    copies = (editP->kind == RLE_UNROLL)?editP->repeat:1;
    editP->copyFirstP = NILP;
    editP->copyLastP = NILP;
    editP->copyWords = 0;
    editP->copyRefs = 0;

    for( k = 0; k < copies; ++k )
    {
        headP = cloneRun(firstP, endP, &tailP, &words);
        cloneRefs(editP);

        if( (editP->kind == RLE_UNROLL) && ((headP->type == LOCATION) || (headP->type == LCLLOCATION)) )
        {
            headP->type = EXPR;
            headP->value.ival = 0;
        }

        if( editP->copyLastP )
        {
            editP->copyLastP->leftP = headP;
        }
        else
        {
            editP->copyFirstP = headP;
        }

        editP->copyLastP = tailP;
        editP->copyWords += words;
    }

    editP->copyAfterP = destEndP;

    if( editP->copyFirstP )
    {
        editP->copyLastP->leftP = destEndP->leftP;
        destEndP->leftP = editP->copyFirstP;
    }

    if( !(editP->entriesP = (int *)malloc(sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    editP->entriesP[0] = addrP->index;
    editP->entryCount = 1;
    return(RLR_ACCEPTED);
}

// Is an address one of a run's words?
// Returns 1 if it is, else 0.
static int
inRun(OptWordP *runPP, int count, int bank, int addr)
{
int i;

    for( i = 0; i < count; ++i )
    {
        if( (runPP[i]->bank == bank) && (runPP[i]->addr == addr) )
        {
            return(1);
        }
    }

    return(0);
}

// Turn a one-word statement into a deleted word, recording how to undo it.  An
// expression statement becomes the RELAYOUT node itself; a word on a label's
// line leaves the label, with a RELAYOUT node after it.
static void
deleteWord(RlEdit *editP, PNodeP stmtP)
{
PNodeP markP;

    editP->stmtP = stmtP;
    editP->savedValue = stmtP->value;
    editP->savedValue2 = stmtP->value2;

    if( stmtP->type == EXPR )
    {
        stmtP->type = RELAYOUT;
        stmtP->value.ival = RL_DELETED;
        stmtP->value2.ival = 0;
        stmtP->exprP = stmtP->rightP;
        stmtP->rightP = NILP;
        editP->markP = stmtP;
        return;
    }

    markP = newMarker(stmtP, RL_DELETED);
    markP->value2.ival = 1;
    markP->exprP = stmtP->rightP;
    stmtP->rightP = NILP;
    markP->leftP = stmtP->leftP;
    stmtP->leftP = markP;
    editP->markP = markP;
}

// Put back a word deleteWord() deleted.
static void
undoDelete(RlEdit *editP)
{
PNodeP stmtP;
PNodeP markP;

    stmtP = editP->stmtP;
    markP = editP->markP;

    if( markP == stmtP )
    {
        stmtP->type = EXPR;
        stmtP->rightP = stmtP->exprP;
        stmtP->exprP = NILP;
    }
    else
    {
        stmtP->rightP = markP->exprP;
        stmtP->leftP = markP->leftP;
        free(markP);
    }

    stmtP->value = editP->savedValue;
    stmtP->value2 = editP->savedValue2;
}

// Clone the statements firstP..endP, trees and all, as a chain not yet on the
// list; *lastPP gets its last node, *wordsP the words it emits.
// Returns the chain's first node; out of memory is fatal.
static PNodeP
cloneRun(PNodeP firstP, PNodeP endP, PNodeP *lastPP, int *wordsP)
{
PNodeP nodeP;
PNodeP newP;
PNodeP headP;
PNodeP tailP;

    // Every constant reference met is recorded as a pair, for cloneRefs().
    clonePairCount = 0;
    headP = NILP;
    tailP = NILP;
    *wordsP = 0;

    for( nodeP = firstP; ; nodeP = nodeP->leftP )
    {
        if( !(newP = (PNodeP)calloc(1, sizeof(PNode))) )
        {
            fprintf(stderr, "am1: out of memory in relayout\n");
            exit(1);
        }

        *newP = *nodeP;
        newP->flags &= ~RL_TAG;
        newP->leftP = NILP;
        newP->rightP = cloneExpr(nodeP->rightP);
        newP->exprP = cloneExpr(nodeP->exprP);
        *wordsP += statementSize(newP);

        if( tailP )
        {
            tailP->leftP = newP;
        }
        else
        {
            headP = newP;
        }

        tailP = newP;

        if( nodeP == endP )
        {
            break;
        }
    }

    *lastPP = tailP;
    return(headP);
}

// Clone one expression tree, recording each constant's pair.  A string or flexo
// text is shared, never written and never freed.
// Returns the clone, NILP for NILP; out of memory is fatal.
static PNodeP
cloneExpr(PNodeP nodeP)
{
PNodeP newP;

    if( !nodeP )
    {
        return(NILP);
    }

    if( !(newP = (PNodeP)calloc(1, sizeof(PNode))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    *newP = *nodeP;
    newP->flags &= ~RL_TAG;

    // The clone keeps the original's slot until the pools are rebuilt, which
    // decides whether it shares that slot or splits one off.
    if( nodeP->type == CONSTANT )
    {
        newP->value2.ptr = cloneExpr((PNodeP)(nodeP->value2.ptr));
        clonePair(nodeP, newP);
    }

    newP->leftP = cloneExpr(nodeP->leftP);
    newP->rightP = cloneExpr(nodeP->rightP);
    return(newP);
}

// Record one original-and-clone constant reference pair.
static void
clonePair(PNodeP origP, PNodeP cloneP)
{
    if( (clonePairCount + 2) > clonePairCapacity )
    {
        clonePairCapacity = (clonePairCapacity)?(clonePairCapacity * 2):64;

        if( !(clonePairsPP = (PNodeP *)realloc(clonePairsPP, ((size_t)clonePairCapacity * sizeof(PNodeP)))) )
        {
            fprintf(stderr, "am1: out of memory in relayout\n");
            exit(1);
        }
    }

    clonePairsPP[clonePairCount++] = origP;
    clonePairsPP[clonePairCount++] = cloneP;
}

// Give every cloned constant reference its original's model: value, key
// symbols, their order as assembled and the mask that gives the key.  The count
// is added to the edit's, since an unroll clones its run more than once.
static void
cloneRefs(RlEdit *editP)
{
int i;
int n;
RlRefValue *foundP;
RlRefValue *newP;

    // The two expressions are the same tree twice, so the model is the same;
    // only the walk state the key is read under differs, and currentMask()
    // reads that from the walk.  The table is sorted by node address, so the
    // clones are appended and it is sorted again.
    if( !clonePairCount )
    {
        return;
    }

    if( !(oldRefsP = (RlRefValue *)realloc(oldRefsP,
            ((size_t)(oldRefCount + (clonePairCount / 2)) * sizeof(RlRefValue)))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( n = 0, i = 0; i < clonePairCount; i += 2 )
    {
        if( !(foundP = findRef(clonePairsPP[i])) )
        {
            fprintf(stderr, "am1: optimizer: relayout: a copied constant is not in the program\n");
            exit(1);
        }

        newP = &oldRefsP[oldRefCount + n];
        *newP = *foundP;
        newP->refP = clonePairsPP[i + 1];
        newP->cloneOfP = editP;
        newP->walk = 0;
        newP->key = 0;
        ++n;
    }

    editP->copyRefs += n;
    oldRefCount += n;
    qsort(oldRefsP, (size_t)oldRefCount, sizeof(RlRefValue), compareRefValues);
}

// Count the words each bank gained from the accepted copies into *copiedP.
// Returns the total.
static int
copiedWords(int *copiedP)
{
int i;
int total;

    total = 0;
    memset(copiedP, 0, ((size_t)(MAXBANK + 1) * sizeof(int)));

    for( i = 0; i < editCount; ++i )
    {
        if( ((editsP[i].kind == RLE_COPY) || (editsP[i].kind == RLE_INLINE) || (editsP[i].kind == RLE_UNROLL)) &&
            (editsP[i].reason == RLR_ACCEPTED) )
        {
            copiedP[editsP[i].bank] += editsP[i].copyWords;
            total += editsP[i].copyWords;
        }
    }

    return(total);
}

// Undo the most recently applied edit.
static void
undoEdit(RlEdit *editP)
{
int i;
int n;

    if( (editP->kind == RLE_COPY) || (editP->kind == RLE_INLINE) || (editP->kind == RLE_UNROLL) )
    {
        // An unroll's jmp first, then its copies; a loop of one trip has none
        // to unsplice.
        undoJmpDelete(editP);

        // The clones are unspliced but not freed, nor are their cloneRefs()
        // entries: buildPools() may have pointed a slot at a clone's expression,
        // and the next layout evaluates every slot BEFORE the walk that drops
        // it.  Nothing reaches them again; the walks follow the statement list.
        if( editP->copyFirstP )
        {
            editP->copyAfterP->leftP = editP->copyLastP->leftP;
            editP->copyLastP->leftP = NILP;
        }

        for( i = 0, n = 0; i < oldRefCount; ++i )
        {
            if( oldRefsP[i].cloneOfP != editP )
            {
                oldRefsP[n++] = oldRefsP[i];
            }
        }

        oldRefCount = n;
        undoDelete(editP);
    }
    else if( editP->kind == RLE_DELETE )
    {
        // A kept word's new expression comes out with its delete.
        if( editP->keepStmtP )
        {
            editP->keepStmtP->rightP = editP->keepOldP;
            editP->keepStmtP = NILP;
            editP->keepOldP = NILP;
        }

        undoDelete(editP);
    }
    else
    {
        editP->destP->leftP = editP->runEndP->leftP;
        editP->endP->leftP = editP->nextP;
        *editP->linkPP = editP->firstP;
        free(editP->movedFromP);
        free(editP->beginP);
        free(editP->runEndP);
    }

    free(editP->entriesP);
    editP->entriesP = NILP;
    editP->entryCount = 0;
}

// Returns the number of words a statement emits, as the parser counted them.
static int
statementSize(PNodeP nodeP)
{
    switch( nodeP->type )
    {
    case EXPR:
        return( (nodeP->rightP && !(nodeP->rightP->flags & PN_NOINC))?1:0 );

    case LOCATION:
    case LCLLOCATION:
        if( !nodeP->rightP )
        {
            return(0);
        }

        if( !(nodeP->rightP->flags & PN_NOINC) )
        {
            return(1);
        }

        switch( nodeP->rightP->type )
        {
        case TEXT:
        case TYPE340:
            return( countText(nodeP->rightP->value.flexText) );

        case ASCII:
            return( countAscii(nodeP->rightP->value.strP) );

        default:
            return(0);
        }

    case TEXT:
    case TYPE340:
        return( countText(nodeP->value.flexText) );

    case ASCII:
        return( countAscii(nodeP->value.strP) );

    case TABLE:
        return( nodeP->value.ival );

    default:
        return(0);
    }
}

// Find a statement's line end: the comment, terminator, empty line or semicolon
// after it, and then a same-line empty line; *terminatedP says one was found.
// Returns that node, or the statement itself when nothing ends its line.
static PNodeP
lineEnd(PNodeP stmtP, int *terminatedP)
{
PNodeP endP;
PNodeP nextP;

    endP = stmtP;
    *terminatedP = 0;

    // A comment on the line takes the newline with it, so it is the line's
    // end by itself; nothing follows it on the list but the next line.
    if( (nextP = stmtP->leftP) &&
        (((nextP->type == COMMENT) && !(nextP->flags & PN_SOL)) ||
         (nextP->type == TERMINATOR) || (nextP->type == EMPTYLINE) || (nextP->type == SEMI)) )
    {
        endP = nextP;
        *terminatedP = 1;
    }

    // The trailing empty line is the newline a constant with no closing
    // bracket hands back, after the node -O1 inserts to end a rewritten one.
    if( *terminatedP && endP->leftP && (endP->leftP->type == EMPTYLINE) &&
        (endP->type != EMPTYLINE) && (endP->leftP->lineNo == endP->lineNo) )
    {
        endP = endP->leftP;
    }

    return(endP);
}

// Find the link that points at a node of the statement list.
// Returns its address; a node not on the list is fatal.
static PNodeP *
linkTo(PNodeP targetP)
{
PNodeP nodeP;

    if( rlRootP->leftP == targetP )
    {
        return( &rlRootP->leftP );
    }

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP->leftP == targetP )
        {
            return( &nodeP->leftP );
        }
    }

    fprintf(stderr, "am1: optimizer: relayout: a statement is not on the statement list\n");
    exit(1);
}

// Find the first of the label-only lines, with any comment or blank lines among
// them, directly before a statement; each names the statement's word.
// Returns that node, or stmtP itself when no such line precedes it.
static PNodeP
labelLinesBefore(PNodeP stmtP)
{
PNodeP nodeP;
PNodeP startP;
PNodeP endP;
int terminated;

    // A placement's move starts here, since the jmp it deletes named the word
    // by one of these labels; left behind, a label would name the word after
    // the run and be refused "label".  An -O=edits move's run is exactly its
    // words' statements, so it does not call this.
    startP = NILP;
    nodeP = rlRootP->leftP;

    while( nodeP && (nodeP != stmtP) )
    {
        if( ((nodeP->type == LOCATION) || (nodeP->type == LCLLOCATION)) && !nodeP->rightP &&
            (endP = lineEnd(nodeP, &terminated)) && terminated && (endP != nodeP) )
        {
            if( !startP )
            {
                startP = nodeP;
            }

            nodeP = endP->leftP;
            continue;
        }

        // A comment or blank line between the labels and the word: the
        // labels still name the word, and the lines go with them.
        if( startP && (((nodeP->type == COMMENT) && (nodeP->flags & PN_SOL)) || (nodeP->type == EMPTYLINE)) )
        {
            nodeP = nodeP->leftP;
            continue;
        }

        startP = NILP;
        nodeP = nodeP->leftP;
    }

    return( ((nodeP == stmtP) && startP)?startP:stmtP );
}

// Make a RELAYOUT node of the given kind, credited to modelP's line and bank.
// Returns the node; out of memory is fatal.
static PNodeP
newMarker(PNodeP modelP, int what)
{
PNodeP nodeP;

    if( !(nodeP = (PNodeP)calloc(1, sizeof(PNode))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    nodeP->type = RELAYOUT;
    nodeP->value.ival = what;
    nodeP->lineNo = modelP->lineNo;
    nodeP->bank = modelP->bank;
    nodeP->pc = modelP->pc;
    return(nodeP);
}

// Does a word follow, from nodeP on, before a fixed point or a pool or
// variable block?  Returns 1 if it does, 0 if not.
static int
nextWordFollows(PNodeP nodeP)
{
    for( ; nodeP; nodeP = nodeP->leftP )
    {
        if( isFixedPoint(nodeP) )
        {
            return(0);
        }

        switch( nodeP->type )
        {
        case CONSTANTS:
            if( nodeP->value.symP )
            {
                return(0);
            }
            break;

        case VARS:
            if( nodeP->value.ptr )
            {
                return(0);
            }
            break;

        case EXPR:
        case LOCATION:
        case LCLLOCATION:
        case TEXT:
        case TYPE340:
        case ASCII:
        case TABLE:
            if( statementSize(nodeP) > 0 )
            {
                return(1);
            }
            break;

        default:
            break;
        }
    }

    return(0);
}

// Is this node a fixed point of the layout?
// Returns 1 for an origin or a bank directive, else 0.
static int
isFixedPoint(PNodeP nodeP)
{
    return( (nodeP->type == ORIGIN) || (nodeP->type == BANK) );
}

// The layout.

// Lay the program out and rebuild the pools until they settle, then re-point
// the T3 and T8 numbers.  Returns RLR_ACCEPTED, RLR_BOUND (past a bank's end),
// RLR_CEILING, RLR_POOL (pools not rebuildable) or RLR_INTERNAL (never settled).
static RlReason
settle(void)
{
int pass;
int changed;
int b;
RlReason reason;

    rekeyChanges = 0;

    for( pass = 0; pass < RL_MAXPASSES; ++pass )
    {
        poison();
        layout();

        // A word past the end of its bank counts only once the pools are
        // settled: they may still be the ones a refused edit left.
        if( RL_SKIP(RLSKIP_REKEY) )
        {
            break;
        }

        if( (reason = rekeyWalk(0)) != RLR_ACCEPTED )
        {
            return(reason);
        }

        buildPools(&changed);

        if( !changed )
        {
            break;
        }

        ++rekeyChanges;
    }

    if( pass >= RL_MAXPASSES )
    {
        return(RLR_INTERNAL);
    }

    if( rlBound )
    {
        return(RLR_BOUND);
    }

    // No bank may reach its declared ceiling.  Checked only for the program
    // as assembled and an -O=edits edit: a transform's edits pass through
    // states the budget never counted (an unroll's copies land before its
    // setup is deleted), so optRelayout() checks their result after the last.
    for( b = 0; rlCeilingCheck && (b <= MAXBANK); ++b )
    {
        if( optBankCeilingDeclared(rlTableP, b) && (rlTop[b] >= optBankCeilingDeclared(rlTableP, b)) )
        {
            return(RLR_CEILING);
        }
    }

    repointT3();
    repointT8();
    return(RLR_ACCEPTED);
}

// Set every field the layout owns to a sentinel, so a field it fails to
// recompute cannot pass for one it did.
static void
poison(void)
{
PNodeP nodeP;
SymListP listP;
BankContextP bankP;
PNodeListP varP;

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        nodeP->pc = RL_SENTINEL;
        poisonTree(nodeP->rightP);
        poisonTree(nodeP->exprP);

        switch( nodeP->type )
        {
        case LOCATION:
        case LCLLOCATION:
            nodeP->value.symP->value = RL_SENTINEL;

            if( (nodeP->type == LOCATION) && nodeP->value.symP->symP )
            {
                nodeP->value.symP->symP->value = RL_SENTINEL;
            }
            break;

        case BANK:
            nodeP->value2.ival = RL_SENTINEL;
            break;

        case VARS:
            for( varP = (PNodeListP)(nodeP->value.ptr); varP; varP = varP->nextP )
            {
                varP->nodeP->pc = RL_SENTINEL;
                varP->nodeP->value.symP->value = RL_SENTINEL;
            }
            break;

        default:
            break;
        }
    }

    for( listP = constsListP; listP; listP = listP->nextP )
    {
        poisonPool(listP->symP);
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        bankP->cur_pc = RL_SENTINEL;

        if( bankP->constSymP )
        {
            bankP->constPC = RL_SENTINEL;
        }

        if( bankP->varNodesP )
        {
            bankP->varPC = RL_SENTINEL;

            for( varP = bankP->varNodesP; varP; varP = varP->nextP )
            {
                varP->nodeP->pc = RL_SENTINEL;
                varP->nodeP->value.symP->value = RL_SENTINEL;
            }
        }
    }

    rlRootP->pc = RL_SENTINEL;

    if( rlRootP->rightP )
    {
        rlRootP->rightP->pc = RL_SENTINEL;
        poisonTree(rlRootP->rightP->exprP);

        if( rlRootP->rightP->type == START )
        {
            rlRootP->rightP->value.ival = RL_VSENTINEL;
        }
    }
}

// Poison the pc and '.' of every node of an expression tree.
static void
poisonTree(PNodeP nodeP)
{
    while( nodeP )
    {
        nodeP->pc = RL_SENTINEL;

        if( nodeP->type == DOT )
        {
            nodeP->value.ival = RL_SENTINEL;
        }
        else if( nodeP->type == CONSTANT )
        {
            poisonTree((PNodeP)(nodeP->value2.ptr));
        }

        poisonTree(nodeP->leftP);
        nodeP = nodeP->rightP;
    }
}

// Poison a pool's slot addresses and values.
static void
poisonPool(SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    symP->value = RL_SENTINEL;
    symP->value2 = RL_VSENTINEL;
    poisonPool(symP->leftP);
    poisonPool(symP->rightP);
}

// The walk: the grammar actions of parser.y, in the order they ran.
static void
layout(void)
{
PNodeP nodeP;
PNodeP startP;
SymListP listP;
BankContextP bankP;
SymNodeP symP;
int pc;
int b;

    rlBound = 0;
    heldOrigins = 0;
    heldTables = 0;

    for( b = 0; b <= MAXBANK; ++b )
    {
        rlPC[b] = (b == 0)?4:0;
        rlTop[b] = -1;
    }

    rlBank = 0;

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        pc = rlPC[rlBank];

        switch( nodeP->type )
        {
        case ORIGIN:
            // Held: the value the parser gave it is a fixed point.
            setStatement(nodeP, pc);
            rlPC[rlBank] = nodeP->value.ival;
            heldOrigins += heldExpr(nodeP->exprP);
            break;

        case BANK:
            // The node belongs to the bank being left; its value2 is where the
            // new one resumes.
            setStatement(nodeP, pc);
            rlBank = nodeP->value.ival;

            if( !RL_SKIP(RLSKIP_RESUME) )
            {
                nodeP->value2.ival = rlPC[rlBank];
            }
            break;

        case EXPR:
            setStatement(nodeP, pc);
            advance(statementSize(nodeP));
            break;

        case LOCATION:
        case LCLLOCATION:
            setStatement(nodeP, pc);
            symP = nodeP->value.symP;

            if( !RL_SKIP(RLSKIP_LABEL) )
            {
                symP->value = pc;

                // A forced local's global twin, fixed up when it was defined.
                if( (nodeP->type == LOCATION) && symP->symP )
                {
                    symP->symP->value = pc;
                }
            }

            advance(statementSize(nodeP));
            break;

        case CONSTANTS:
            setStatement(nodeP, pc);

            if( nodeP->value.symP )
            {
                rlPoolBank = rlBank;
                rlPC[rlBank] = placePool(pc, nodeP->value.symP);
            }
            break;

        case VARS:
            setStatement(nodeP, pc);
            placeVars(rlBank, (PNodeListP)(nodeP->value.ptr));
            break;

        case TEXT:
        case TYPE340:
        case ASCII:
            setStatement(nodeP, pc);
            advance(statementSize(nodeP));
            break;

        case TABLE:
            setStatement(nodeP, pc);
            advance(nodeP->value.ival);
            heldTables += heldExpr(nodeP->exprP);
            break;

        default:
            // Everything else emits nothing: its nodes carry the pc.
            setStatement(nodeP, pc);
            break;
        }
    }

    // What no explicit directive placed, at the end of its own bank.
    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        b = bankP->bank;

        if( bankP->constSymP )
        {
            if( !RL_SKIP(RLSKIP_BANKCTX) )
            {
                bankP->constPC = rlPC[b];
            }

            rlPoolBank = b;
            rlPC[b] = placePool(rlPC[b], bankP->constSymP);
        }

        if( bankP->varNodesP )
        {
            if( !RL_SKIP(RLSKIP_BANKCTX) )
            {
                bankP->varPC = rlPC[b];
            }

            placeVars(b, bankP->varNodesP);
        }
    }

    for( bankP = banksP; bankP && !RL_SKIP(RLSKIP_BANKCTX); bankP = bankP->nextP )
    {
        bankP->cur_pc = rlPC[bankP->bank];
    }

    // 'start' or 'stop', and the root, at the current bank's pc.
    pc = rlPC[rlBank];

    if( !RL_SKIP(RLSKIP_START) )
    {
        rlRootP->pc = pc;

        if( (startP = rlRootP->rightP) )
        {
            startP->pc = pc;
            setExpr(startP->exprP, pc);
        }
    }

    // The pool values, once every address is known.
    for( listP = constsListP; listP; listP = listP->nextP )
    {
        setPoolValues(listP->symP);
    }

    if( !RL_SKIP(RLSKIP_START) && (startP = rlRootP->rightP) && (startP->type == START) )
    {
        startP->value.ival = evalExpr(startP->exprP);
    }
}

// Set a statement node's pc and that of every node under it.  The statement's
// own leftP is the next statement and is not followed.
static void
setStatement(PNodeP nodeP, int pc)
{
    if( !RL_SKIP(RLSKIP_PC) )
    {
        nodeP->pc = pc;
    }

    setExpr(nodeP->rightP, pc);
    setExpr(nodeP->exprP, pc);
}

// Set the pc of every node of an expression tree, and every '.' in it,
// including those inside a [..] constant.
static void
setExpr(PNodeP nodeP, int pc)
{
    while( nodeP )
    {
        if( !RL_SKIP(RLSKIP_PC) )
        {
            nodeP->pc = pc;
        }

        if( nodeP->type == DOT )
        {
            if( !RL_SKIP(RLSKIP_DOT) )
            {
                nodeP->value.ival = pc;
            }
        }
        else if( nodeP->type == CONSTANT )
        {
            setExpr((PNodeP)(nodeP->value2.ptr), pc);
        }

        setExpr(nodeP->leftP, pc);
        nodeP = nodeP->rightP;
    }
}

// Advance the current bank's pc past words just placed, noting a word that
// would pass the end of the bank.
static void
advance(int words)
{
    if( (words > 0) && ((rlPC[rlBank] + words) > BANKSIZE) )
    {
        rlBound = 1;
    }

    if( (words > 0) && ((rlPC[rlBank] + words - 1) > rlTop[rlBank]) )
    {
        rlTop[rlBank] = rlPC[rlBank] + words - 1;
    }

    rlPC[rlBank] += words;
}

// Place a pool's slots from pc, in the parser's pre-order.
// Returns the pc after the last slot.
static int
placePool(int pc, SymNodeP symP)
{
    if( !symP )
    {
        return(pc);
    }

    if( !RL_SKIP(RLSKIP_POOLPC) )
    {
        symP->value = pc;
    }

    if( pc > rlTop[rlPoolBank] )
    {
        rlTop[rlPoolBank] = pc;
    }

    if( ++pc > BANKSIZE )
    {
        rlBound = 1;
    }

    pc = placePool(pc, symP->leftP);
    pc = placePool(pc, symP->rightP);
    return(pc);
}

// Place a list of variables at the bank's pc, in list order, as setVarsPC() does.
static void
placeVars(int bank, PNodeListP listP)
{
PNodeP nodeP;
SymNodeP symP;

    for( ; listP; listP = listP->nextP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        if( !(symP->flags & SYMF_VAR) )
        {
            continue;
        }

        if( !RL_SKIP(RLSKIP_VARPC) )
        {
            symP->value = rlPC[bank];
            nodeP->pc = rlPC[bank];
        }

        if( rlPC[bank] > rlTop[bank] )
        {
            rlTop[bank] = rlPC[bank];
        }

        if( ++rlPC[bank] > BANKSIZE )
        {
            rlBound = 1;
        }
    }
}

// Evaluate every slot of a pool, as setConstVal() does.
static void
setPoolValues(SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    if( !RL_SKIP(RLSKIP_POOLVAL) )
    {
        symP->value2 = evalExpr((PNodeP)(symP->ptr));
    }

    setPoolValues(symP->leftP);
    setPoolValues(symP->rightP);
}

// Does a held origin's or table count's expression name a symbol, a constant
// or '.', so its author meant something relative?  Returns 1 if so, else 0.
static int
heldExpr(PNodeP nodeP)
{
    for( ; nodeP; nodeP = nodeP->rightP )
    {
        switch( nodeP->type )
        {
        case ADDR:
        case LCLADDR:
        case BREF:
        case WILDREF:
        case CONSTANT:
        case DOT:
            return(1);

        default:
            break;
        }

        if( heldExpr(nodeP->leftP) )
        {
            return(1);
        }
    }

    return(0);
}

// The pool re-key.
//
// The parser pools a constant under hashExpr() of its expression as it stood
// when parsed: a defined symbol gives its address, an undefined one its
// creation serial number.  It files the keys in a binary tree in the order met,
// and setConstPC() places the slots in pre-order, so slot order and sharing
// follow the addresses and the source order, and an edited source would pool
// differently.  The re-key walks the statements in their edited order and
// makes every key again: each symbol resolved as it was when its constant was
// parsed, unless the edit moved its definition across the constant, and each
// serial number moved by the change in creations before it.  It files the keys
// as sym_add() does, reusing each slot the program had under the same key.

// Record, from the program as assembled, every pool slot and its pool, the pool
// each constants directive and bank end closed, and for every constant
// reference the symbols its key reads; a key no mask reproduces is held.
static void
recordPools(void)
{
int i;
int k;
int s;
int order;
PNodeP nodeP;
RlSlot *slotP;
BankContextP bankP;

    for( i = 0; i < poolCount; ++i )
    {
        countSlots(poolsPP[i], i);
    }

    if( slotCount &&
        (!(slotsP = (RlSlot *)calloc((size_t)slotCount, sizeof(RlSlot))) ||
         !(slotByNameP = (int *)calloc((size_t)slotCount, sizeof(int)))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    slotCount = 0;

    for( i = 0; i < poolCount; ++i )
    {
        addSlots(poolsPP[i], i);
    }

    qsort(slotsP, (size_t)slotCount, sizeof(RlSlot), compareSlots);

    for( i = 0; i < slotCount; ++i )
    {
        slotByNameP[i] = i;
        slotsP[i].live = 1;
    }

    qsort(slotByNameP, (size_t)slotCount, sizeof(int), compareSlotNames);

    // A slot no reference names is an orphan, which the rebuild keeps where
    // the parser's tree had it.
    if( !(poolOrphansP = (int *)calloc((size_t)poolCount + 1, sizeof(int))) ||
        !(poolAnchoredP = (int *)calloc((size_t)poolCount + 1, sizeof(int))) ||
        !(poolReclaimP = (int *)calloc((size_t)poolCount + 1, sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0; i < slotCount; ++i )
    {
        slotsP[i].orphan = 1;
        slotsP[i].child[0] = (slotsP[i].symP->leftP)?(int)(findSlot(slotsP[i].symP->leftP) - slotsP):-1;
        slotsP[i].child[1] = (slotsP[i].symP->rightP)?(int)(findSlot(slotsP[i].symP->rightP) - slotsP):-1;
    }

    for( i = 0; i < oldRefCount; ++i )
    {
        if( (slotP = findSlot(oldRefsP[i].slotP)) )
        {
            slotP->orphan = 0;
        }
    }

    for( i = 0; i < slotCount; ++i )
    {
        poolOrphansP[slotsP[i].pool] += slotsP[i].orphan;
    }

    markReclaimable();

    // Which pool each directive and each bank's end closed.
    for( i = 0; i <= MAXBANK; ++i )
    {
        bankEndPool[i] = -1;
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        bankEndPool[bankP->bank] = rootPool(bankP->constSymP);
    }

    for( directiveCount = 0, nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( (nodeP->type == CONSTANTS) && nodeP->value.symP )
        {
            ++directiveCount;
        }
    }

    if( directiveCount &&
        (!(directivesPP = (PNodeP *)calloc((size_t)directiveCount, sizeof(PNodeP))) ||
         !(directivePoolP = (int *)calloc((size_t)directiveCount, sizeof(int)))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    for( i = 0, nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( (nodeP->type == CONSTANTS) && nodeP->value.symP )
        {
            directivesPP[i] = nodeP;
            directivePoolP[i++] = rootPool(nodeP->value.symP);
        }
    }

    if( rekeyWalk(1) != RLR_ACCEPTED )
    {
        fprintf(stderr, "am1: optimizer: relayout: the constant pools as assembled do not walk again\n");
        exit(1);
    }

    // Where the walk met each slot's key, and so which slots are anchored.
    for( i = 0; i < slotCount; ++i )
    {
        slotsP[i].order = INT_MAX;
    }

    for( s = 0, order = 0; s < setCount; ++s )
    {
        for( k = 0; (setsP[s].pool >= 0) && (k < setsP[s].count); ++k )
        {
            if( (slotP = findSlotByName(setsP[s].pool, keysP[setsP[s].keysP[k]].name)) )
            {
                slotP->order = order++;
            }
        }
    }

    for( i = 0; i < poolCount; ++i )
    {
        if( (slotP = findSlot(poolsPP[i])) )
        {
            anchorSlots(slotP);
        }
    }
}

// Count the slots of one pool tree.
static void
countSlots(SymNodeP symP, int pool)
{
    if( symP )
    {
        ++slotCount;
        countSlots(symP->leftP, pool);
        countSlots(symP->rightP, pool);
    }
}

// Add the slots of one pool tree to the slot array.
static void
addSlots(SymNodeP symP, int pool)
{
    if( symP )
    {
        slotsP[slotCount].symP = symP;
        slotsP[slotCount].pool = pool;
        ++slotCount;
        addSlots(symP->leftP, pool);
        addSlots(symP->rightP, pool);
    }
}

// Which pool a tree, as assembled, is the root of.
// Returns the index into the pool array, or -1 for none.
static int
rootPool(SymNodeP rootP)
{
int i;

    for( i = 0; rootP && (i < poolCount); ++i )
    {
        if( poolsPP[i] == rootP )
        {
            return(i);
        }
    }

    return(-1);
}

// Order slots by address, for bsearch().
static int
compareSlots(const void *aP, const void *bP)
{
uintptr_t a;
uintptr_t b;

    a = (uintptr_t)(((const RlSlot *)aP)->symP);
    b = (uintptr_t)(((const RlSlot *)bP)->symP);
    return( (a < b)?-1:((a > b)?1:0) );
}

// Order slot indexes by pool, then key.
static int
compareSlotNames(const void *aP, const void *bP)
{
const RlSlot *slotAP;
const RlSlot *slotBP;

    slotAP = &slotsP[*(const int *)aP];
    slotBP = &slotsP[*(const int *)bP];

    if( slotAP->pool != slotBP->pool )
    {
        return( (slotAP->pool < slotBP->pool)?-1:1 );
    }

    return( strcmp(slotAP->symP->name, slotBP->symP->name) );
}

// Find a slot the program had, by its symbol.
// Returns the slot, or NILP when the symbol is not one.
static RlSlot *
findSlot(SymNodeP symP)
{
RlSlot key;

    if( !slotCount )
    {
        return(NILP);
    }

    key.symP = symP;
    return( (RlSlot *)bsearch(&key, slotsP, (size_t)slotCount, sizeof(RlSlot), compareSlots) );
}

// Find a slot the program had in a pool under a key.
// Returns the slot, or NILP.
static RlSlot *
findSlotByName(int pool, const char *nameP)
{
int low;
int high;
int mid;
int cmp;
RlSlot *slotP;

    low = 0;
    high = slotCount - 1;

    while( low <= high )
    {
        mid = (low + high) / 2;
        slotP = &slotsP[slotByNameP[mid]];
        cmp = (slotP->pool != pool)?((slotP->pool < pool)?-1:1):strcmp(slotP->symP->name, nameP);

        if( cmp == 0 )
        {
            return(slotP);
        }

        if( cmp < 0 )
        {
            low = mid + 1;
        }
        else
        {
            high = mid - 1;
        }
    }

    return(NILP);
}

// Find a constant reference's record.
// Returns it, or NILP when the node is not a reference the program had.
static RlRefValue *
findRef(PNodeP refP)
{
RlRefValue key;

    if( !oldRefCount )
    {
        return(NILP);
    }

    key.refP = refP;
    return( (RlRefValue *)bsearch(&key, oldRefsP, (size_t)oldRefCount, sizeof(RlRefValue), compareRefValues) );
}

// Find a symbol's entry in the walk's map, adding it, and clearing what a
// previous walk left.  Returns the entry, never NILP.
static RlSym *
symInfo(SymNodeP symP)
{
int i;
int oldSize;
RlSym *oldP;
RlSym *entryP;

    if( (symMapCount * 2) >= symMapSize )
    {
        oldP = symMapP;
        oldSize = symMapSize;
        symMapSize = (symMapSize)?(symMapSize * 2):4096;

        if( !(symMapP = (RlSym *)calloc((size_t)symMapSize, sizeof(RlSym))) )
        {
            fprintf(stderr, "am1: out of memory in relayout\n");
            exit(1);
        }

        symMapCount = 0;

        for( i = 0; i < oldSize; ++i )
        {
            if( oldP[i].symP )
            {
                entryP = symSlot(oldP[i].symP);
                *entryP = oldP[i];
                ++symMapCount;
            }
        }

        free(oldP);
    }

    entryP = symSlot(symP);

    if( !entryP->symP )
    {
        entryP->symP = symP;
        entryP->oldCreated = -1;
        entryP->walk = 0;
        ++symMapCount;
    }

    if( entryP->walk != rlWalk )
    {
        entryP->walk = rlWalk;
        entryP->created = -1;
        entryP->defined = 0;
    }

    return(entryP);
}

// The map entry a symbol has or would have: open addressing on its address.
// Returns the entry, which is empty when the symbol is not in the map.
static RlSym *
symSlot(SymNodeP symP)
{
uintptr_t h;

    h = ((uintptr_t)symP >> 4) * 0x9E3779B97F4A7C15UL;
    h &= (uintptr_t)(symMapSize - 1);

    while( symMapP[h].symP && (symMapP[h].symP != symP) )
    {
        h = (h + 1) & (uintptr_t)(symMapSize - 1);
    }

    return(&symMapP[h]);
}

// Mention a symbol: the walk's first mention creates it, as the lexer or a
// label's action does.
static void
mentionSym(SymNodeP symP)
{
RlSym *infoP;

    infoP = symInfo(symP);

    if( infoP->created < 0 )
    {
        infoP->created = rlCreations++;

        if( rlRecord )
        {
            infoP->oldCreated = infoP->created;
        }
    }
}

// Is this the walk's first mention of a symbol?
// Returns 1 if the walk has not met it, else 0.
static int
firstMention(SymNodeP symP)
{
    return( symInfo(symP)->created < 0 );
}

// Mark a symbol's definition as passed.
static void
defineSym(SymNodeP symP)
{
    symInfo(symP)->defined = 1;
}

// Walk the statements in their present order as the parser read them, keying
// every constant and gathering each pool's keys; recording keeps what made them.
// Returns RLR_ACCEPTED, RLR_POOL (a pool gained, lost or unkeyable) or RLR_INTERNAL.
static RlReason
rekeyWalk(int record)
{
int b;
RlReason reason;
PNodeP nodeP;
PNodeListP varP;
BankContextP bankP;

    ++rlWalk;
    rlRecord = record;
    rlCreations = 0;
    keyCount = 0;
    setCount = 0;

    for( b = 0; b <= MAXBANK; ++b )
    {
        openSet[b] = -1;
    }

    b = 0;

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        reason = RLR_ACCEPTED;

        switch( nodeP->type )
        {
        case RELAYOUT:
            break;

        case BANK:
            b = nodeP->value.ival;
            break;

        case CONSTANTS:
            reason = bindSet(b, directivePool(nodeP));
            break;

        case VARS:
            for( varP = (PNodeListP)(nodeP->value.ptr); varP; varP = varP->nextP )
            {
                defineSym(varP->nodeP->value.symP);
            }
            break;

        case LOCATION:
        case LCLLOCATION:
            // A new global label is made, and defined, before the rest of its
            // line is read; any other label after it.
            if( (nodeP->type == LOCATION) && firstMention(nodeP->value.symP) )
            {
                mentionSym(nodeP->value.symP);
                defineLabel(nodeP);
            }

            if( (reason = walkExpr(b, nodeP->rightP)) == RLR_ACCEPTED )
            {
                reason = walkExpr(b, nodeP->exprP);
            }

            mentionSym(nodeP->value.symP);
            defineLabel(nodeP);
            break;

        default:
            if( (reason = walkExpr(b, nodeP->rightP)) == RLR_ACCEPTED )
            {
                reason = walkExpr(b, nodeP->exprP);
            }
            break;
        }

        if( reason != RLR_ACCEPTED )
        {
            return(reason);
        }
    }

    if( rlRootP->rightP && ((reason = walkExpr(b, rlRootP->rightP->exprP)) != RLR_ACCEPTED) )
    {
        return(reason);
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        if( (reason = bindSet(bankP->bank, bankEndPool[bankP->bank])) != RLR_ACCEPTED )
        {
            return(reason);
        }
    }

    for( b = 0; b <= MAXBANK; ++b )
    {
        if( (openSet[b] >= 0) && setsP[openSet[b]].count )
        {
            return(RLR_POOL);
        }
    }

    return(RLR_ACCEPTED);
}

// Define a label statement's symbol, and a forced local's global twin.
static void
defineLabel(PNodeP nodeP)
{
    defineSym(nodeP->value.symP);

    if( (nodeP->type == LOCATION) && nodeP->value.symP->symP )
    {
        defineSym(nodeP->value.symP->symP);
    }
}

// The pool a constants directive closed as assembled.
// Returns its index, or -1 when the directive closed none.
static int
directivePool(PNodeP nodeP)
{
int i;

    for( i = 0; i < directiveCount; ++i )
    {
        if( directivesPP[i] == nodeP )
        {
            return(directivePoolP[i]);
        }
    }

    return(-1);
}

// Walk one expression tree in source order: its symbols are mentioned, and
// each constant is keyed once its own expression has been read.
// Returns RLR_ACCEPTED, or the reason the walk stops.
static RlReason
walkExpr(int bank, PNodeP nodeP)
{
RlReason reason;

    while( nodeP )
    {
        if( nodeP->type == CONSTANT )
        {
            if( ((reason = walkExpr(bank, (PNodeP)(nodeP->value2.ptr))) != RLR_ACCEPTED) ||
                ((reason = keyRef(bank, nodeP)) != RLR_ACCEPTED) )
            {
                return(reason);
            }
        }
        else if( (nodeP->type == ADDR) || (nodeP->type == LCLADDR) || (nodeP->type == BREF) )
        {
            mentionSym(nodeP->value.symP);
        }

        if( (reason = walkExpr(bank, nodeP->leftP)) != RLR_ACCEPTED )
        {
            return(reason);
        }

        nodeP = nodeP->rightP;
    }

    return(RLR_ACCEPTED);
}

// Open a set for a bank.
// Returns its index.
static int
newSet(int bank)
{
int set;

    if( setCount >= setCapacity )
    {
        setCapacity = (setCapacity)?(setCapacity * 2):16;

        if( !(setsP = (RlSetP)realloc(setsP, ((size_t)setCapacity * sizeof(RlSet)))) )
        {
            fprintf(stderr, "am1: out of memory in relayout\n");
            exit(1);
        }

        memset(&setsP[setCount], 0, ((size_t)(setCapacity - setCount) * sizeof(RlSet)));
    }

    set = openSet[bank] = setCount++;
    setsP[set].count = 0;
    setsP[set].pool = -1;
    setsP[set].bank = bank;
    return(set);
}

// Make an empty key; a pointer into the keys may not survive the call.
// Returns its index.
static int
newKey(void)
{
    if( keyCount >= keyCapacity )
    {
        keyCapacity = (keyCapacity)?(keyCapacity * 2):1024;

        if( !(keysP = (RlKeyP)realloc(keysP, ((size_t)keyCapacity * sizeof(RlKey)))) )
        {
            fprintf(stderr, "am1: out of memory in relayout\n");
            exit(1);
        }
    }

    memset(&keysP[keyCount], 0, sizeof(RlKey));
    keysP[keyCount].left = -1;
    keysP[keyCount].right = -1;
    return(keyCount++);
}

// Key one constant reference and file it in its bank's open pool; a key the
// pool has not met makes a slot, which is a creation.
// Returns RLR_ACCEPTED, RLR_POOL or RLR_INTERNAL.
static RlReason
keyRef(int bank, PNodeP refP)
{
int i;
int set;
int key;
long hash;
RlRefValue *infoP;
RlSetP setP;
RlSlot *slotP;

    if( !(infoP = findRef(refP)) )
    {
        return(RLR_INTERNAL);
    }

    infoP->walk = rlWalk;

    if( rlRecord )
    {
        recordKey(infoP);
        hash = strtol(infoP->slotP->name, NILP, 10);

        if( (slotP = findSlot(infoP->slotP)) && !slotP->firstRefP )
        {
            slotP->firstRefP = refP;
        }
    }
    else if( !infoP->modeled )
    {
        // Held: good only while nothing it could read has changed.
        if( rlHash((PNodeP)(refP->value2.ptr)) != infoP->oldAllHash )
        {
            return(RLR_POOL);
        }

        for( i = 0; i < infoP->leafCount; ++i )
        {
            if( symInfo(keySymsPP[infoP->leafStart + i])->created !=
                symInfo(keySymsPP[infoP->leafStart + i])->oldCreated )
            {
                return(RLR_POOL);
            }
        }

        hash = strtol(infoP->slotP->name, NILP, 10);
    }
    else
    {
        hash = keyUnder(infoP, currentMask(infoP), 1);
    }

    // The bank's open pool.
    if( (set = openSet[bank]) < 0 )
    {
        set = newSet(bank);
    }

    setP = &setsP[set];

    for( i = 0; i < setP->count; ++i )
    {
        if( keysP[setP->keysP[i]].hash == hash )
        {
            break;
        }
    }

    if( i >= setP->count )
    {
        if( setP->count >= setP->capacity )
        {
            setP->capacity = (setP->capacity)?(setP->capacity * 2):64;

            if( !(setP->keysP = (int *)realloc(setP->keysP, ((size_t)setP->capacity * sizeof(int)))) )
            {
                fprintf(stderr, "am1: out of memory in relayout\n");
                exit(1);
            }
        }

        key = newKey();
        keysP[key].hash = hash;
        sprintf(keysP[key].name, "%ld", hash);
        keysP[key].firstRefP = refP;
        setP->keysP[setP->count++] = key;
        ++rlCreations;              // the parser's sym_make() for the slot
    }

    infoP->key = setP->keysP[i];
    ++keysP[infoP->key].refs;
    return(RLR_ACCEPTED);
}

// Turn every marked BREF under a node back into the WILDREF it was, which
// holds the symbol's name, and push each with its symbol for rlHash() to
// restore.
static void
flipWild(PNodeP nodeP)
{
    for( ; nodeP; nodeP = nodeP->rightP )
    {
        if( (nodeP->flags & RL_WILD) && (nodeP->type == BREF) )
        {
            if( wildCount >= wildCapacity )
            {
                wildCapacity = (wildCapacity)?(wildCapacity * 2):16;

                if( !(wildNodesPP = (PNodeP *)realloc(wildNodesPP, ((size_t)wildCapacity * sizeof(PNodeP)))) ||
                    !(wildSymsPP = (SymNodeP *)realloc(wildSymsPP, ((size_t)wildCapacity * sizeof(SymNodeP)))) )
                {
                    fprintf(stderr, "am1: out of memory in relayout\n");
                    exit(1);
                }
            }

            wildNodesPP[wildCount] = nodeP;
            wildSymsPP[wildCount++] = nodeP->value.symP;
            nodeP->value.strP = nodeP->value.symP->name;
            nodeP->type = WILDREF;
        }

        flipWild(nodeP->leftP);
    }
}

// A constant's key as the parser would make it now: hashExpr() of the
// expression, with each 'sym:*' keyed by its name, as it was when parsed.
// Returns the key.
static long
rlHash(PNodeP exprP)
{
long hash;

    wildCount = 0;
    flipWild(exprP);
    hash = hashExpr(exprP);

    while( wildCount > 0 )
    {
        --wildCount;
        wildNodesPP[wildCount]->value.symP = wildSymsPP[wildCount];
        wildNodesPP[wildCount]->type = BREF;
    }

    return(hash);
}

// Record what made a reference's key as assembled: the symbols it reads, which
// were defined before it, and a resolved mask that gives the program's key,
// trying the walk's order first.  A key no mask gives is counted and held.
static void
recordKey(RlRefValue *infoP)
{
unsigned mask;
unsigned limit;
long want;
int i;

    infoP->leafStart = keySymCount;
    infoP->leafCount = 0;
    infoP->oldOrder = 0;
    infoP->modeled = 0;
    infoP->oldAllHash = rlHash((PNodeP)(infoP->refP->value2.ptr));

    if( !addKeySyms(infoP, (PNodeP)(infoP->refP->value2.ptr)) )
    {
        keySymCount = infoP->leafStart;
        infoP->leafCount = -1;
        ++unkeyedCount;
        return;
    }

    for( i = 0; i < infoP->leafCount; ++i )
    {
        if( symInfo(keySymsPP[infoP->leafStart + i])->defined )
        {
            infoP->oldOrder |= (1U << i);
        }
    }

    want = strtol(infoP->slotP->name, NILP, 10);

    if( keyUnder(infoP, infoP->oldOrder, 0) == want )
    {
        infoP->mask = infoP->oldOrder;
        infoP->modeled = 1;
        return;
    }

    limit = (1U << infoP->leafCount);

    for( mask = 0; mask < limit; ++mask )
    {
        if( keyUnder(infoP, mask, 0) == want )
        {
            infoP->mask = mask;
            infoP->modeled = 1;
            return;
        }
    }

    ++unkeyedCount;
}

// Add the symbols a key reads, each once, in the order hashExpr() meets them.
// A 'sym:*' reads none: rlHash() keys it by its name.
// Returns 1, or 0 when there are more than a key can be modeled with.
static int
addKeySyms(RlRefValue *infoP, PNodeP nodeP)
{
int i;
SymNodeP symP;

    for( ; nodeP; nodeP = nodeP->rightP )
    {
        if( ((nodeP->type == ADDR) || (nodeP->type == LCLADDR) || (nodeP->type == BREF)) &&
            !(nodeP->flags & RL_WILD) )
        {
            symP = nodeP->value.symP;

            for( i = 0; i < infoP->leafCount; ++i )
            {
                if( keySymsPP[infoP->leafStart + i] == symP )
                {
                    break;
                }
            }

            if( i >= infoP->leafCount )
            {
                if( infoP->leafCount >= RL_MAXKEYLEAVES )
                {
                    return(0);
                }

                if( keySymCount >= keySymCapacity )
                {
                    keySymCapacity = (keySymCapacity)?(keySymCapacity * 2):1024;

                    if( !(keySymsPP = (SymNodeP *)realloc(keySymsPP, ((size_t)keySymCapacity * sizeof(SymNodeP)))) )
                    {
                        fprintf(stderr, "am1: out of memory in relayout\n");
                        exit(1);
                    }
                }

                keySymsPP[keySymCount++] = symP;
                ++infoP->leafCount;
            }
        }

        if( !addKeySyms(infoP, nodeP->leftP) )
        {
            return(0);
        }
    }

    return(1);
}

// The resolved mask of a reference's symbols now: as assembled, except where
// the walk now meets a definition on the other side of the reference.
// Returns the mask, bit n set for symbol n resolved.
static unsigned
currentMask(RlRefValue *infoP)
{
int i;
unsigned bit;
unsigned mask;

    mask = infoP->mask;

    for( i = 0; i < infoP->leafCount; ++i )
    {
        bit = (1U << i);

        if( (symInfo(keySymsPP[infoP->leafStart + i])->defined != 0) != ((infoP->oldOrder & bit) != 0) )
        {
            mask = (symInfo(keySymsPP[infoP->leafStart + i])->defined)?(mask | bit):(mask & ~bit);
        }
    }

    return(mask);
}

// A reference's key with its symbols resolved by mask and, when shifted, each
// unresolved serial number moved by the change in creations before it.  The
// real hashExpr() makes it; every flag and serial number is put back.
static long
keyUnder(RlRefValue *infoP, unsigned mask, int shifted)
{
int i;
int flagsSaved[RL_MAXKEYLEAVES];
int serialSaved[RL_MAXKEYLEAVES];
long hash;
RlSym *symInfoP;
SymNodeP symP;

    for( i = 0; i < infoP->leafCount; ++i )
    {
        symP = keySymsPP[infoP->leafStart + i];
        flagsSaved[i] = symP->flags;
        serialSaved[i] = symP->serialNumber;
    }

    for( i = 0; i < infoP->leafCount; ++i )
    {
        symP = keySymsPP[infoP->leafStart + i];

        if( mask & (1U << i) )
        {
            symP->flags |= SYMF_RESOLVED;
        }
        else
        {
            symP->flags &= ~SYMF_RESOLVED;

            if( shifted )
            {
                symInfoP = symInfo(symP);

                if( (symInfoP->created >= 0) && (symInfoP->oldCreated >= 0) )
                {
                    symP->serialNumber = serialSaved[i] + (symInfoP->created - symInfoP->oldCreated);
                }
            }
        }
    }

    hash = rlHash((PNodeP)(infoP->refP->value2.ptr));

    for( i = infoP->leafCount - 1; i >= 0; --i )
    {
        symP = keySymsPP[infoP->leafStart + i];
        symP->flags = flagsSaved[i];
        symP->serialNumber = serialSaved[i];
    }

    return(hash);
}

// Close a bank's gathered keys into a pool, at a constants directive or the
// bank's end.  Returns RLR_ACCEPTED, or RLR_POOL when the program had no pool
// there and there are keys, or had one and there are neither keys nor orphans.
static RlReason
bindSet(int bank, int pool)
{
int set;

    set = openSet[bank];

    // An orphan the reclaim is taking still counts here: a pool it empties
    // must reach buildPools(), which empties it.  What this refusal is for is
    // a pool the edited program has lost a live constant from.
    if( ((set < 0) || !setsP[set].count) && ((pool < 0) || !poolOrphansP[pool]) )
    {
        return( (pool < 0)?RLR_ACCEPTED:RLR_POOL );
    }

    if( pool < 0 )
    {
        return(RLR_POOL);
    }

    if( set < 0 )
    {
        set = newSet(bank);
    }

    setsP[set].pool = pool;
    openSet[bank] = -1;

    return(RLR_ACCEPTED);
}

// Make the pools the walk gathered: keys filed in walk order, a slot the
// program had under the same key reused, every live reference pointed at its
// key's slot.  *changedP is set when any pointer the layout reads changed.
static void
buildPools(int *changedP)
{
int i;
int k;
int s;
int at;
int *linkP;
RlSetP setP;
RlKeyP keyP;
RlSlot *slotP;
RlRefValue *infoP;
RlExtraP extraP;
SymNodeP symP;
SymNodeP newP;

    *changedP = 0;

    for( i = 0; i < slotCount; ++i )
    {
        slotsP[i].live = 0;
    }

    for( extraP = extrasP; extraP; extraP = extraP->nextP )
    {
        extraP->live = 0;
    }

    for( s = 0; s < setCount; ++s )
    {
        setP = &setsP[s];

        if( setP->pool < 0 )
        {
            continue;
        }

        orderSet(setP);

        // The reclaim took every slot this pool had and no reference makes a
        // new one, so the pool is empty and its root holds nothing.  Only the
        // reclaim reaches here: bindSet() refuses an edit that empties a pool.
        if( !setP->count )
        {
            setRoot(setP->pool, NILP, changedP);
            continue;
        }

        // File the keys: sym_add()'s unbalanced tree, by strcmp().
        for( k = 1; k < setP->count; ++k )
        {
            keyP = &keysP[setP->keysP[k]];
            at = setP->keysP[0];

            for( ; ; )
            {
                linkP = (strcmp(keyP->name, keysP[at].name) < 0)?&keysP[at].left:&keysP[at].right;

                if( *linkP < 0 )
                {
                    *linkP = setP->keysP[k];
                    break;
                }

                at = *linkP;
            }
        }

        // A slot for each key.
        for( k = 0; k < setP->count; ++k )
        {
            keyP = &keysP[setP->keysP[k]];

            if( (slotP = findSlotByName(setP->pool, keyP->name)) )
            {
                slotP->live = 1;
                keyP->slotP = slotP->symP;

                // Its value is the expression of the FIRST reference THIS
                // walk keyed, the one the parser would take from the edited
                // source.  It is asked of every walk, not only the one that
                // first moves the slot: a later edit can put the keys back,
                // and a slot whose expression only moved forward would keep
                // another constant's value; a move and a delete that cancel
                // in the layout reach this.
                if( keyP->firstRefP && (slotP->symP->ptr != keyP->firstRefP->value2.ptr) )
                {
                    slotP->symP->ptr = keyP->firstRefP->value2.ptr;

                    if( evalExpr((PNodeP)(slotP->symP->ptr)) != slotP->symP->value2 )
                    {
                        *changedP = 1;
                    }
                }
                continue;
            }

            for( extraP = extrasP; extraP; extraP = extraP->nextP )
            {
                if( !extraP->live && (extraP->pool == setP->pool) && !strcmp(extraP->symP->name, keyP->name) )
                {
                    break;
                }
            }

            if( !extraP )
            {
                symP = findRef(keyP->firstRefP)->slotP;

                if( !(extraP = (RlExtraP)calloc(1, sizeof(RlExtra))) ||
                    !(newP = sym_make(keyP->name, 0)) )
                {
                    fprintf(stderr, "am1: out of memory in relayout\n");
                    exit(1);
                }

                newP->flags = symP->flags;
                newP->bank = symP->bank;
                extraP->symP = newP;
                extraP->pool = setP->pool;
                extraP->nextP = extrasP;
                extrasP = extraP;
                *changedP = 1;
            }

            extraP->live = 1;
            keyP->slotP = extraP->symP;

            if( extraP->symP->ptr != keyP->firstRefP->value2.ptr )
            {
                extraP->symP->ptr = keyP->firstRefP->value2.ptr;
                *changedP = 1;
            }
        }

        // The links, and the root wherever the program keeps it.
        for( k = 0; k < setP->count; ++k )
        {
            keyP = &keysP[setP->keysP[k]];
            newP = (keyP->left >= 0)?keysP[keyP->left].slotP:NILP;

            if( keyP->slotP->leftP != newP )
            {
                keyP->slotP->leftP = newP;
                *changedP = 1;
            }

            newP = (keyP->right >= 0)?keysP[keyP->right].slotP:NILP;

            if( keyP->slotP->rightP != newP )
            {
                keyP->slotP->rightP = newP;
                *changedP = 1;
            }
        }

        setRoot(setP->pool, keysP[setP->keysP[0]].slotP, changedP);
    }

    for( i = 0; i < oldRefCount; ++i )
    {
        infoP = &oldRefsP[i];

        if( (infoP->walk == rlWalk) && (infoP->refP->value.symP != keysP[infoP->key].slotP) )
        {
            infoP->refP->value.symP = keysP[infoP->key].slotP;
            *changedP = 1;
        }
    }
}

// Put a pool's anchored slots among a set's keys where the parser's tree had
// them: the root first, each other right after its parent, an orphaned one
// last.  Any order filing every slot after its ancestors reproduces the tree.
static void
orderSet(RlSetP setP)
{
int i;
int k;
int n;
int *orderP;
RlSlot *slotP;

    if( !poolAnchoredP[setP->pool] )
    {
        return;
    }

    if( !(orderP = (int *)malloc(((size_t)(setP->count + poolAnchoredP[setP->pool]) + 1) * sizeof(int))) )
    {
        fprintf(stderr, "am1: out of memory in relayout\n");
        exit(1);
    }

    n = 0;

    if( (slotP = findSlot(poolsPP[setP->pool])) && slotP->anchored )
    {
        n = placeSlot(setP, slotP, orderP, n);
    }

    for( k = 0; k < setP->count; ++k )
    {
        slotP = findSlotByName(setP->pool, keysP[setP->keysP[k]].name);

        if( !slotP || !slotP->anchored )
        {
            n = placeKey(setP, setP->keysP[k], slotP, orderP, n);
        }
    }

    for( i = 0; i < slotCount; ++i )
    {
        if( (slotsP[i].pool == setP->pool) && slotsP[i].anchored )
        {
            n = placeSlot(setP, &slotsP[i], orderP, n);
        }
    }

    free(setP->keysP);
    setP->keysP = orderP;
    setP->count = n;
    setP->capacity = n;
}

// File a key, then the anchored children of its slot, left first.
// Returns the count of keys ordered so far.
static int
placeKey(RlSetP setP, int key, RlSlot *slotP, int *orderP, int n)
{
int c;
int child;

    if( keysP[key].placed )
    {
        return(n);
    }

    keysP[key].placed = 1;
    orderP[n++] = key;

    for( c = 0; slotP && (c < 2); ++c )
    {
        if( ((child = slotP->child[c]) >= 0) && slotsP[child].anchored )
        {
            n = placeSlot(setP, &slotsP[child], orderP, n);
        }
    }

    return(n);
}

// File an anchored slot under the key a reference makes for it, or an orphan
// under a key of its own; a slot whose references are all gone is not filed.
// Returns the count of keys ordered so far.
static int
placeSlot(RlSetP setP, RlSlot *slotP, int *orderP, int n)
{
int k;
int key;
long hash;

    if( slotP->emitted == rlWalk )
    {
        return(n);
    }

    slotP->emitted = rlWalk;
    hash = strtol(slotP->symP->name, NILP, 10);

    for( k = 0; k < setP->count; ++k )
    {
        if( keysP[setP->keysP[k]].hash == hash )
        {
            return( placeKey(setP, setP->keysP[k], slotP, orderP, n) );
        }
    }

    // An orphan the reclaim takes is not filed either, so no key names it,
    // buildPools() leaves it out and the word goes.  That is the whole of the
    // reclaim; the layout does the rest, as for a deleted word.
    if( !slotP->orphan || (rlReclaim && slotP->reclaim) )
    {
        return(n);
    }

    key = newKey();
    keysP[key].hash = hash;
    sprintf(keysP[key].name, "%ld", hash);
    return( placeKey(setP, key, slotP, orderP, n) );
}

// Find which slots are anchored: met by the walk after one of their own
// descendants, or never, because a transform rewrote the reference the parser
// met.  Returns the least walk order in the subtree.
static int
anchorSlots(RlSlot *slotP)
{
int c;
int sub;
int least;

    least = INT_MAX;

    for( c = 0; c < 2; ++c )
    {
        if( (slotP->child[c] >= 0) && ((sub = anchorSlots(&slotsP[slotP->child[c]])) < least) )
        {
            least = sub;
        }
    }

    slotP->anchored = (slotP->orphan || (least < slotP->order));
    poolAnchoredP[slotP->pool] += slotP->anchored;
    return( (least < slotP->order)?least:slotP->order );
}

// Mark the orphan slots the reclaim may take: named nowhere in the edited
// program, and every rewrite that stopped naming them licensed by a
// declaration, as tallyFreed() decided.
static void
markReclaimable(void)
{
OptXformP recP;
RlSlot *slotP;

    // The two tests are independent: orphan is read from the references the
    // program still has, the license from the record, so a rewrite that
    // misreports what it freed can at worst license a word no program reads.
    // The license matters: an undeclared -O2 T8 is a same-length rewrite, and
    // collecting its freed word would make it one that moves every later word.
    if( !rlTableP->xformApplied )
    {
        return;     // a dry run rewrote nothing, so nothing is freed
    }

    for( recP = rlTableP->xformsP; recP; recP = recP->nextP )
    {
        if( !recP->freesPool || !recP->licensedPool ||
            !recP->throughP || (recP->throughP->kind != OPTK_CONST) )
        {
            continue;
        }

        if( (slotP = findSlot(recP->throughP->symP)) && slotP->orphan && !slotP->reclaim )
        {
            slotP->reclaim = 1;
            ++poolReclaimP[slotP->pool];
        }
    }
}

// Reclaim the pool words the declared rewrites freed, checked as an edit is
// and refused whole, since a half-made reclaim is a layout no rewrite asked for.
// Returns RLR_ACCEPTED, or the reason it was refused and undone.
static RlReason
reclaimPools(void)
{
int i;
int dupCount;
RlReason reason;
OptXformP recP;
RlSlot *slotP;

    // Called once, after the baseline layout and before the first edit:
    // rlReclaim stays 0 until then so the baseline reproduces the program as
    // assembled.  The reclaim has no bisection number of its own; it follows
    // the rewrites that freed the slots, and switching one off leaves its
    // slot no orphan, with nothing here to take.
    for( i = 0, reclaimedSlots = 0; i < poolCount; ++i )
    {
        reclaimedSlots += poolReclaimP[i];
    }

    if( optReclaimOff() )
    {
        reclaimedSlots = 0;     // -O=reclaim=off: the testing build's hold
    }

    if( !reclaimedSlots )
    {
        return(RLR_ACCEPTED);
    }

    rlReclaim = 1;

    if( ((reason = settle()) == RLR_ACCEPTED) && ((reason = checkHazards()) == RLR_ACCEPTED) )
    {
        reason = checkTable(&dupCount);
    }

    if( reason != RLR_ACCEPTED )
    {
        rlReclaim = 0;

        if( settle() != RLR_ACCEPTED )
        {
            fprintf(stderr, "am1: optimizer: relayout: undoing the pool reclaim did not lay out again\n");
            exit(1);
        }
    }

    // What it took: a marked slot another reference keeps alive under its own
    // key is still in its pool, and is not one of them.  Each rewrite that
    // freed one is told, for the report and the dump.
    for( i = 0, reclaimedSlots = 0; i < slotCount; ++i )
    {
        reclaimedSlots += (slotsP[i].reclaim && !slotsP[i].live);
    }

    for( recP = rlTableP->xformsP; recP; recP = recP->nextP )
    {
        recP->poolCollected = (recP->freesPool && recP->licensedPool && recP->throughP &&
            (recP->throughP->kind == OPTK_CONST) && (slotP = findSlot(recP->throughP->symP)) &&
            slotP->reclaim && !slotP->live);
    }

    return(reason);
}

// Point everything that holds a pool's root at a new one: its constants list
// entry, and the directive or the bank context that closed it.
static void
setRoot(int pool, SymNodeP rootP, int *changedP)
{
int i;
BankContextP bankP;

    if( poolListsPP[pool]->symP == rootP )
    {
        return;
    }

    *changedP = 1;
    poolListsPP[pool]->symP = rootP;

    for( i = 0; i < directiveCount; ++i )
    {
        if( directivePoolP[i] == pool )
        {
            directivesPP[i]->value.symP = rootP;
            return;
        }
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        if( bankEndPool[bankP->bank] == pool )
        {
            bankP->constSymP = rootP;
            return;
        }
    }
}

// Count, per bank, what the pools now hold that the program did not, and what
// they no longer hold.  Returns the number of new slots; *droppedP gets the
// slots gone.
static int
poolChanges(int *grownP, int *droppedP)
{
int i;
int splits;
RlExtraP extraP;

    splits = 0;
    *droppedP = 0;

    if( grownP )
    {
        memset(grownP, 0, (MAXBANK + 1) * sizeof(int));
    }

    for( extraP = extrasP; extraP; extraP = extraP->nextP )
    {
        if( extraP->live )
        {
            ++splits;

            if( grownP )
            {
                ++grownP[poolBankP[extraP->pool]];
            }
        }
    }

    for( i = 0; i < slotCount; ++i )
    {
        if( !slotsP[i].live )
        {
            ++*droppedP;
        }
    }

    return(splits);
}

// Collect every constant reference in the live program, in statement order:
// the statements' expressions, the variables' initializers, 'start', and the
// constants nested inside other constants.  A deleted word's are not live.
static void
collectRefs(void)
{
PNodeP nodeP;

    refCount = 0;

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP->type == RELAYOUT )
        {
            continue;
        }

        collectExprRefs(nodeP->rightP);
        collectExprRefs(nodeP->exprP);
    }

    if( rlRootP->rightP )
    {
        collectExprRefs(rlRootP->rightP->exprP);
    }
}

// Collect the constant references of one expression tree.
static void
collectExprRefs(PNodeP nodeP)
{
    while( nodeP )
    {
        if( nodeP->type == CONSTANT )
        {
            if( refCount >= refCapacity )
            {
                refCapacity = (refCapacity)?(refCapacity * 2):256;

                if( !(refsPP = (PNodeP *)realloc(refsPP, ((size_t)refCapacity * sizeof(PNodeP)))) )
                {
                    fprintf(stderr, "am1: out of memory in relayout\n");
                    exit(1);
                }
            }

            refsPP[refCount++] = nodeP;
            collectExprRefs((PNodeP)(nodeP->value2.ptr));
        }

        collectExprRefs(nodeP->leftP);
        nodeP = nodeP->rightP;
    }
}

// Re-point every applied T3 whose far target is a number at the word it named.
// A target that was deleted keeps its number; checkHazards() refuses that.
static void
repointT3(void)
{
int i;
OptXformP recP;

    for( i = 0; i < rlTableP->count; ++i )
    {
        if( (recP = recordOfP[i]) && recP->numberP && recP->targetP &&
            (currentAddr(recP->targetP->index) >= 0) )
        {
            recP->numberP->value.ival = currentAddr(recP->targetP->index);
        }
    }
}

// Re-point every applied T8d at the value its constant holds now: once a move
// shifts tbl, "law 6254" from "lac [tbl:0]" must follow it.  A value
// the law cannot carry keeps the old number, and checkWord() refuses it.
static void
repointT8(void)
{
int i;
int value;
OptXformP recP;
PNodeP innerP;

    // The word table keeps the original expression (installTree() leaves it),
    // so the constant is evaluated as the pool word it replaced would be.
    for( i = 0; i < rlTableP->count; ++i )
    {
        if( !(recP = recordOfP[i]) || !recP->numberP || (recP->findingP->rule != OPTRULE_T8D) ||
            !(innerP = optFirstConstInner(rlTableP->entriesPP[i]->exprP)) )
        {
            continue;
        }

        value = (evalExpr(innerP) & WRDMASK);

        if( recP->after & 010000 )
        {
            // law i n loads the complement of n.
            if( ((~value) & WRDMASK) <= ADDRMASK )
            {
                recP->numberP->value.ival = ((~value) & WRDMASK);
            }
        }
        else if( value <= ADDRMASK )
        {
            recP->numberP->value.ival = value;
        }
    }
}

// The checks.

// Find where a word of the table is now.
// Returns its address, or -1 when it is deleted.
static int
currentAddr(int index)
{
OptWordP entryP;
RlSlot *slotP;

    if( wordStateP[index] & RLW_DELETED )
    {
        return(-1);
    }

    entryP = rlTableP->entriesPP[index];

    if( entryP->nodeP )
    {
        // A deleted statement being checked has no address either.
        if( (entryP->nodeP->type == RELAYOUT) ||
            (((entryP->nodeP->type == LOCATION) || (entryP->nodeP->type == LCLLOCATION)) &&
             (entryP->kind == OPTK_EXPR) && !entryP->nodeP->rightP) )
        {
            return(-1);
        }

        return(entryP->nodeP->pc + offsetP[index]);
    }

    // A pool slot the edited program no longer has.
    if( (entryP->kind == OPTK_CONST) && (slotP = findSlot(entryP->symP)) && !slotP->live )
    {
        return(-1);
    }

    return(entryP->symP->value);
}

// Check every word that remains, after an edit has been laid out.
// Returns RLR_ACCEPTED, or the first reason found to refuse the edit.
static RlReason
checkHazards(void)
{
int i;
int value;
RlReason reason;
OptWordP entryP;

    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];

        // An overlaid address may not move at all.
        if( (entryP->flags & OPTF_DUPADDR) && (currentAddr(i) != entryP->addr) )
        {
            return(RLR_OVERLAY);
        }

        if( (reason = checkWord(i)) != RLR_ACCEPTED )
        {
            return(reason);
        }
    }

    // Every live constant reference, against the value it had.
    collectRefs();

    for( i = 0; i < refCount; ++i )
    {
        if( !oldRefValue(refsPP[i], &value) )
        {
            return(RLR_INTERNAL);
        }

        if( (reason = checkExpr(refsPP[i]->bank, (PNodeP)(refsPP[i]->value2.ptr), value, -1)) != RLR_ACCEPTED )
        {
            return(reason);
        }
    }

    return(RLR_ACCEPTED);
}

// Check one word: its expression, and the memory reference it makes by number.
// Returns RLR_ACCEPTED or the reason to refuse.
static RlReason
checkWord(int index)
{
int oldValue;
int newValue;
int addr;
int k;
int to;
RlReason reason;
OptWordP entryP;
OptXformP recP;
OptEdgeP edgeP;
PNodeP exprP;

    entryP = rlTableP->entriesPP[index];

    if( (addr = currentAddr(index)) < 0 )
    {
        return(RLR_ACCEPTED);
    }

    oldValue = entryP->value;
    exprP = NILP;
    recP = recordOfP[index];

    // A deleting rule's word is checked from the value it has now: its kept
    // word's new one while its delete stands, else the word as assembled.
    if( recP && optRuleDeletes(recP->findingP->rule) )
    {
        if( recP->keepP && recP->treeP && entryP->nodeP && (entryP->nodeP->rightP == recP->treeP) )
        {
            oldValue = recP->after;
        }

        recP = NILP;
    }

    if( recP )
    {
        if( recP->findingP->rule != OPTRULE_T3 )
        {
            // T8: the word must load what its constant holds now, since a
            // constant can hold an address (lac [tbl:0]) that a move
            // shifts.
            return(checkT8(index, recP));
        }

        if( recP->numberP )
        {
            // Re-pointed; a far target that was deleted cannot be.
            return( (recP->targetP && (currentAddr(recP->targetP->index) < 0))?RLR_NUMBER:RLR_ACCEPTED );
        }

        oldValue = recP->after;
    }

    switch( entryP->kind )
    {
    case OPTK_EXPR:
        exprP = entryP->nodeP->rightP;
        newValue = (exprP && (exprP->type == DOT))?addr:evalExpr(exprP);
        break;

    case OPTK_TABLE:
        exprP = (offsetP[index] == 0)?entryP->nodeP->rightP:NILP;
        newValue = (entryP->nodeP->rightP)?evalExpr(entryP->nodeP->rightP):0;
        break;

    case OPTK_VAR:
        exprP = entryP->nodeP->leftP;
        newValue = (exprP && (exprP->type == DOT))?addr:evalExpr(exprP);
        break;

    case OPTK_CONST:
        newValue = entryP->symP->value2;
        break;

    default:
        newValue = entryP->value;
        break;
    }

    if( exprP && ((reason = checkExpr(entryP->bank, exprP, oldValue, addr)) != RLR_ACCEPTED) )
    {
        return(reason);
    }

    // The references the word makes that no symbol names: a memory reference
    // written as a number, jda's entry, cal's 100 and 101.
    if( recP )
    {
        return(RLR_ACCEPTED);
    }

    for( edgeP = entryP->outP; edgeP; edgeP = edgeP->nextOutP )
    {
        if( !(edgeP->flags & OPTEF_IMPLICIT) || !edgeP->toP ||
            (edgeP->flags & (OPTEF_VIAPOINTER | OPTEF_NOWORD | OPTEF_UNKNOWN | OPTEF_PLACEHOLDER)) )
        {
            continue;
        }

        if( (to = currentAddr(edgeP->toP->index)) < 0 )
        {
            return(RLR_NUMBER);
        }

        if( (entryP->decode.opcode == 016) && !entryP->decode.indirect )
        {
            // cal: 100 and 101 whatever the field says.
            if( to != edgeP->toAddr )
            {
                return(RLR_NUMBER);
            }

            continue;
        }

        k = (edgeP->toAddr - (oldValue & ADDRMASK));

        if( (((newValue & ADDRMASK) + k) & ADDRMASK) != to )
        {
            return(RLR_NUMBER);
        }
    }

    return(RLR_ACCEPTED);
}

// Check one T8 rewrite: what its word loads (0 for cli and cla, 777777 for cla
// cma, n for law n, ~n for law i n) must be what its constant evaluates to now.
// Returns RLR_ACCEPTED, or RLR_NUMBER when they differ.
static RlReason
checkT8(int index, OptXformP recP)
{
PNodeP innerP;
int loads;
int value;

    if( !(innerP = optFirstConstInner(rlTableP->entriesPP[index]->exprP)) )
    {
        return(RLR_ACCEPTED);
    }

    value = (evalExpr(innerP) & WRDMASK);

    switch( recP->findingP->rule )
    {
    case OPTRULE_T8C:
        loads = WRDMASK;
        break;

    case OPTRULE_T8D:
        // A constant of literals keeps no number node: the word is as fired.
        loads = (recP->numberP)?recP->numberP->value.ival:(recP->after & ADDRMASK);
        loads = (recP->after & 010000)?((~loads) & WRDMASK):loads;
        break;

    default:
        loads = 0;
        break;
    }

    return( (loads == value)?RLR_ACCEPTED:RLR_NUMBER );
}

// Check one expression against the value it had.  bank is the bank it was
// written in; topDotAddr is the word's address when a bare '.' means it, else -1.
// Returns RLR_ACCEPTED or the reason to refuse.
static RlReason
checkExpr(int bank, PNodeP exprP, int oldValue, int topDotAddr)
{
RlLeaf leaves[RL_MAXLEAVES];
int count;
int nonlinear;
int i;
int base;
int step;
int v0;
int v1;
int v2;
int d1;
int d2;
int sum;
int nonZero;
int dotInvolved;
int oldField;
int newField;
int now;
RlLeaf *plainP;
OptWordP wordP;
RlReason reason;

    if( !exprP || ((exprP->type == DOT) && (topDotAddr >= 0)) )
    {
        return(RLR_ACCEPTED);
    }

    count = 0;
    nonlinear = collectLeaves(exprP, leaves, &count);

    if( !count )
    {
        return(RLR_ACCEPTED);
    }

    v0 = evalExpr(exprP);
    sum = 0;
    nonZero = 0;
    dotInvolved = 0;
    plainP = NILP;

    // Perturb each address the expression names to find how the address field
    // depends on it.  A field moving one for one with a single symbol and equal
    // to it names that symbol's word, which must still be the one there (a
    // label moved on by a deletion is by design); any other one-for-one field
    // is an address plus an offset and must still name its word; a field the
    // addresses cancel out of is a length, recomputed; anything else must keep
    // its value.
    for( i = 0; (i < count) && !nonlinear; ++i )
    {
        base = leafValue(&leaves[i]);
        step = ((base & ADDRMASK) < 04000)?1:-1;

        setLeafValue(&leaves[i], (base + step));
        v1 = evalExpr(exprP);
        setLeafValue(&leaves[i], (base + (2 * step)));
        v2 = evalExpr(exprP);
        setLeafValue(&leaves[i], base);

        if( ((v1 ^ v0) & ~ADDRMASK) || ((v2 ^ v0) & ~ADDRMASK) )
        {
            nonlinear = 1;
            break;
        }

        d1 = ((((v1 - v0) & ADDRMASK) ^ 04000) - 04000);
        d2 = ((((v2 - v0) & ADDRMASK) ^ 04000) - 04000);

        if( d2 != (2 * d1) )
        {
            nonlinear = 1;
            break;
        }

        leaves[i].coeff = (d1 * step);
        sum += leaves[i].coeff;

        if( leaves[i].coeff )
        {
            ++nonZero;
            plainP = &leaves[i];

            if( leaves[i].isDot )
            {
                dotInvolved = 1;
            }
            else if( leaves[i].nodeP && (leaves[i].nodeP->type == BREF) )
            {
                bank = leaves[i].nodeP->value2.ival;
            }
        }
    }

    for( i = 0; i < count; ++i )
    {
        if( leaves[i].isDot )
        {
            dotInvolved |= nonlinear;
        }
    }

    reason = (dotInvolved)?RLR_DOT:RLR_OFFSET;

    if( nonlinear || ((sum != 0) && (sum != 1)) )
    {
        return( (v0 == oldValue)?RLR_ACCEPTED:reason );
    }

    if( sum == 0 )
    {
        return(RLR_ACCEPTED);       // a length, recomputed
    }

    // An address.  A single symbol with nothing added is a plain reference.
    if( (nonZero == 1) && (plainP->coeff == 1) && !plainP->isDot &&
        ((v0 & ADDRMASK) == (leafValue(plainP) & ADDRMASK)) )
    {
        if( plainP->nodeP && (plainP->nodeP->type == CONSTANT) )
        {
            return(RLR_ACCEPTED);   // its slot, by construction
        }

        reason = RLR_LABEL;
    }
    else
    {
        plainP = NILP;
    }

    oldField = (oldValue & ADDRMASK);
    newField = (v0 & ADDRMASK);

    if( !(wordP = wordAt(rlTableP, bank, oldField)) )
    {
        return( (plainP || (newField == oldField))?RLR_ACCEPTED:reason );
    }

    if( wordP->flags & OPTF_DUPADDR )
    {
        return( (newField == oldField)?RLR_ACCEPTED:RLR_OVERLAY );
    }

    if( (now = currentAddr(wordP->index)) < 0 )
    {
        return( (plainP)?RLR_ACCEPTED:reason );
    }

    return( (now == newField)?RLR_ACCEPTED:reason );
}

// Collect the addresses an expression names: each symbol once, each '.' and
// each [..] slot, not entering a constant's own expression.  Returns 1 if the
// expression divides or names more than RL_MAXLEAVES addresses, else 0.
static int
collectLeaves(PNodeP nodeP, RlLeaf *leavesP, int *countP)
{
int i;
int nonlinear;

    nonlinear = 0;

    while( nodeP && !nonlinear )
    {
        switch( nodeP->type )
        {
        case BINOP:
            if( (nodeP->value.ival == DIV) || (nodeP->value.ival == MOD) )
            {
                nonlinear = 1;
            }

            nonlinear |= collectLeaves(nodeP->leftP, leavesP, countP);
            nodeP = nodeP->rightP;
            continue;

        case UNOP:
            nodeP = nodeP->rightP;
            continue;

        case ADDR:
        case LCLADDR:
        case BREF:
        case CONSTANT:
            for( i = 0; i < *countP; ++i )
            {
                if( !leavesP[i].isDot && (leavesP[i].symP == nodeP->value.symP) )
                {
                    break;
                }
            }

            if( i < *countP )
            {
                break;
            }

            if( *countP >= RL_MAXLEAVES )
            {
                return(1);
            }

            leavesP[*countP].isDot = 0;
            leavesP[*countP].symP = nodeP->value.symP;
            leavesP[*countP].nodeP = nodeP;
            leavesP[*countP].coeff = 0;
            ++*countP;
            break;

        case DOT:
            if( *countP >= RL_MAXLEAVES )
            {
                return(1);
            }

            leavesP[*countP].isDot = 1;
            leavesP[*countP].symP = NILP;
            leavesP[*countP].nodeP = nodeP;
            leavesP[*countP].coeff = 0;
            ++*countP;
            break;

        default:
            break;
        }

        break;
    }

    return(nonlinear);
}

// Returns the address a leaf stands for.
static int
leafValue(RlLeaf *leafP)
{
    return( (leafP->isDot)?leafP->nodeP->value.ival:leafP->symP->value );
}

// Set the address a leaf stands for.
static void
setLeafValue(RlLeaf *leafP, int value)
{
    if( leafP->isDot )
    {
        leafP->nodeP->value.ival = value;
    }
    else
    {
        leafP->symP->value = value;
    }
}

// The value a constant reference had as assembled.
// Returns 1 with *valueP set, or 0 for a reference that did not exist then.
static int
oldRefValue(PNodeP refP, int *valueP)
{
RlRefValue key;
RlRefValue *foundP;

    key.refP = refP;
    key.value = 0;

    if( !oldRefCount ||
        !(foundP = (RlRefValue *)bsearch(&key, oldRefsP, (size_t)oldRefCount, sizeof(RlRefValue), compareRefValues)) )
    {
        return(0);
    }

    *valueP = foundP->value;
    return(1);
}

// Order reference values by node address, for bsearch().
static int
compareRefValues(const void *aP, const void *bP)
{
uintptr_t a;
uintptr_t b;

    a = (uintptr_t)(((const RlRefValue *)aP)->refP);
    b = (uintptr_t)(((const RlRefValue *)bP)->refP);
    return( (a < b)?-1:((a > b)?1:0) );
}

// Build the word table of the program as it now is and require that the
// parser's addresses and the pc agree on every word, no address used more often
// than it was.  Returns RLR_ACCEPTED, RLR_OVERLAP or RLR_INTERNAL; sets *dupCountP.
static RlReason
checkTable(int *dupCountP)
{
OptTableP tableP;
RlReason reason;

    if( !(tableP = optBuildTable(rlRootP)) )
    {
        *dupCountP = 0;
        return(RLR_INTERNAL);
    }

    *dupCountP = tableP->dupCount;
    reason = RLR_ACCEPTED;

    if( tableP->mismatchCount )
    {
        reason = RLR_INTERNAL;
    }
    else if( tableP->dupCount > rlTableP->dupCount )
    {
        reason = RLR_OVERLAP;
    }

    optFreeTable(tableP);
    return(reason);
}

// Idempotence.

// Returns a digest of every field the layout owns.
static unsigned long
checksum(void)
{
unsigned long sum;
PNodeP nodeP;
SymListP listP;
BankContextP bankP;
PNodeListP varP;

    sum = 0xCBF29CE484222325UL;

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        mix(&sum, nodeP->pc);
        sumTree(nodeP->rightP, &sum);
        sumTree(nodeP->exprP, &sum);

        switch( nodeP->type )
        {
        case LOCATION:
        case LCLLOCATION:
            mix(&sum, nodeP->value.symP->value);
            break;

        case BANK:
            mix(&sum, nodeP->value2.ival);
            break;

        case VARS:
            for( varP = (PNodeListP)(nodeP->value.ptr); varP; varP = varP->nextP )
            {
                mix(&sum, varP->nodeP->value.symP->value);
            }
            break;

        default:
            break;
        }
    }

    for( listP = constsListP; listP; listP = listP->nextP )
    {
        sumPool(listP->symP, &sum);
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        mix(&sum, bankP->cur_pc);
        mix(&sum, bankP->constPC);
        mix(&sum, bankP->varPC);

        for( varP = bankP->varNodesP; varP; varP = varP->nextP )
        {
            mix(&sum, varP->nodeP->value.symP->value);
        }
    }

    mix(&sum, rlRootP->pc);

    if( rlRootP->rightP )
    {
        mix(&sum, rlRootP->rightP->pc);
        mix(&sum, rlRootP->rightP->value.ival);
    }

    return(sum);
}

// Fold an expression tree's pcs and '.' values into the digest.
static void
sumTree(PNodeP nodeP, unsigned long *sumP)
{
    while( nodeP )
    {
        mix(sumP, nodeP->pc);

        if( nodeP->type == DOT )
        {
            mix(sumP, nodeP->value.ival);
        }
        else if( nodeP->type == CONSTANT )
        {
            mix(sumP, nodeP->value.symP->value);
            sumTree((PNodeP)(nodeP->value2.ptr), sumP);
        }
        else if( nodeP->type == INTEGER )
        {
            mix(sumP, nodeP->value.ival);
        }

        sumTree(nodeP->leftP, sumP);
        nodeP = nodeP->rightP;
    }
}

// Fold a pool's slot addresses and values into the digest.
static void
sumPool(SymNodeP symP, unsigned long *sumP)
{
    if( !symP )
    {
        return;
    }

    mix(sumP, symP->value);
    mix(sumP, symP->value2);
    sumPool(symP->leftP, sumP);
    sumPool(symP->rightP, sumP);
}

// Fold one value into the digest, FNV-1a style.
static void
mix(unsigned long *sumP, long value)
{
    *sumP ^= (unsigned long)value;
    *sumP *= 0x100000001B3UL;
}

// The dump.

// Print -O=relayout: a summary line, the segments, each edit and its outcome,
// each accepted copy's clone, every word whose address changed, every pool
// slot made, and a reconcile line per bank.
static void
dump(FILE *fP, int idempotent)
{
int i;
int b;
int k;
int accepted;
int refused;
int grownTotal;
int copiedTotal;
int splitCount;
int dropped;
int now;
int grownP[MAXBANK + 1];
int copiedP[MAXBANK + 1];
int started[MAXBANK + 1];
int before[MAXBANK + 1];
int deleted[MAXBANK + 1];
int bank;
PNodeP nodeP;
OptWordP entryP;
OptXformP recP;
RlEdit *editP;
RlExtraP extraP;
RlKeyP keyP;

    accepted = 0;
    refused = 0;
    grownTotal = 0;

    for( i = 0; i < editCount; ++i )
    {
        if( editsP[i].reason == RLR_ACCEPTED )
        {
            ++accepted;
        }
        else
        {
            ++refused;
        }
    }

    splitCount = poolChanges(grownP, &dropped);
    copiedTotal = copiedWords(copiedP);

    for( b = 0; b <= MAXBANK; ++b )
    {
        grownTotal += grownP[b];
    }

    fprintf(fP, "relayout: edits %d accepted %d refused %d splits %d dropped %d grown %d copied %d unkeyed %d heldorigins %d heldtables %d idempotent %s\n",
        editCount, accepted, refused, splitCount, dropped, grownTotal, copiedTotal, unkeyedCount,
        heldOrigins, heldTables, (idempotent)?"yes":"no");

    // The segments.
    memset(started, 0, sizeof(started));
    started[0] = 1;
    bank = 0;
    fprintf(fP, "segment: bank 0 from 0004 start\n");

    for( nodeP = rlRootP->leftP; nodeP; nodeP = nodeP->leftP )
    {
        if( nodeP->type == ORIGIN )
        {
            fprintf(fP, "segment: bank %d from %04o origin line %d\n", bank, (nodeP->value.ival & ADDRMASK), nodeP->lineNo);
        }
        else if( nodeP->type == BANK )
        {
            bank = nodeP->value.ival;
            fprintf(fP, "segment: bank %d from %04o %s line %d\n", bank, (nodeP->value2.ival & ADDRMASK),
                (started[bank])?"resume":"start", nodeP->lineNo);
            started[bank] = 1;
        }
    }

    // The pool words the reclaim took, each with the rewrite that freed it.
    // They are not edits, so they are printed before the edits.
    for( recP = rlTableP->xformsP; recP; recP = recP->nextP )
    {
        if( recP->poolCollected )
        {
            fprintf(fP, "reclaim: bank %d %04o freed by %s at %04o\n",
                recP->throughP->bank, recP->throughP->addr,
                optRuleName(recP->findingP->rule), recP->findingP->addr);
        }
    }

    for( i = 0; i < editCount; ++i )
    {
        editP = &editsP[i];

        // A transform's edits are not the file's, and name the transform.
        if( editP->windelP )
        {
            fprintf(fP, "window: %s:%d %s delete %d %04o %s\n", (editP->fileP)?editP->fileP:"-", editP->lineNo,
                optWinDelKindName(editP->windelP->kind), editP->bank, editP->from, reasonName(editP->reason));
        }
        else if( editP->xformP )
        {
            fprintf(fP, "space: %s:%d %s delete %d %04o", (editP->fileP)?editP->fileP:"-", editP->lineNo,
                optRuleName(editP->xformP->findingP->rule), editP->bank, editP->from);

            if( editP->xformP->keepP )
            {
                fprintf(fP, " keep %04o", editP->xformP->keepP->addr);
            }

            fprintf(fP, " %s\n", reasonName(editP->reason));
        }
        else if( editP->unrollP )
        {
            if( editP->kind == RLE_UNROLL )
            {
                fprintf(fP, "unroll: %s:%d copy %d %04o %04o x%d over %04o, delete %04o %s\n",
                    (editP->fileP)?editP->fileP:"-", editP->lineNo, editP->bank, editP->from, editP->to,
                    editP->repeat, editP->addr, editP->unrollP->shape.jmpP->addr, reasonName(editP->reason));
            }
            else
            {
                fprintf(fP, "unroll: %s:%d delete %d %04o %s\n", (editP->fileP)?editP->fileP:"-",
                    editP->lineNo, editP->bank, editP->from, reasonName(editP->reason));
            }
        }
        else if( editP->placeP )
        {
            if( editP->kind == RLE_MOVE )
            {
                fprintf(fP, "place: %s:%d move %d %04o %04o after %04o %s\n", (editP->fileP)?editP->fileP:"-",
                    editP->lineNo, editP->bank, editP->from, editP->to, editP->addr, reasonName(editP->reason));
            }
            else
            {
                fprintf(fP, "place: %s:%d delete %d %04o %s\n", (editP->fileP)?editP->fileP:"-",
                    editP->lineNo, editP->bank, editP->from, reasonName(editP->reason));
            }
        }

        else if( editP->inlineP )
        {
            if( editP->kind == RLE_INLINE )
            {
                fprintf(fP, "inline: %s:%d copy %d %04o %04o over %04o%s %s\n", (editP->fileP)?editP->fileP:"-",
                    editP->lineNo, editP->bank, editP->from, editP->to, editP->addr,
                    (editP->inlineP->prefixP)?" after dac":"", reasonName(editP->reason));
            }
            else
            {
                fprintf(fP, "inline: %s:%d delete %d %04o %s\n", (editP->fileP)?editP->fileP:"-",
                    editP->lineNo, editP->bank, editP->from, reasonName(editP->reason));
            }
        }
        else if( editP->kind == RLE_DELETE )
        {
            fprintf(fP, "edit: line %d delete %d %04o %s\n", editP->lineNo, editP->bank, editP->from,
                reasonName(editP->reason));
        }
        else
        {
            fprintf(fP, "edit: line %d %s %d %04o %04o %s %04o %s\n", editP->lineNo,
                (editP->kind == RLE_MOVE)?"move":"copy", editP->bank, editP->from, editP->to,
                (editP->kind == RLE_MOVE)?"after":"over", editP->addr, reasonName(editP->reason));
        }
    }

    // Where each accepted copy put its clone, and how many words it is.
    for( i = 0; i < editCount; ++i )
    {
        editP = &editsP[i];

        if( ((editP->kind == RLE_COPY) || (editP->kind == RLE_INLINE) || (editP->kind == RLE_UNROLL)) &&
            (editP->reason == RLR_ACCEPTED) && editP->copyFirstP )
        {
            fprintf(fP, "copy: bank %d line %d over %04o words %d at %04o\n", editP->bank,
                editP->lineNo, editP->addr, editP->copyWords, (editP->copyFirstP->pc & ADDRMASK));
        }
    }

    memset(before, 0, sizeof(before));
    memset(deleted, 0, sizeof(deleted));

    for( i = 0; i < rlTableP->count; ++i )
    {
        entryP = rlTableP->entriesPP[i];
        ++before[entryP->bank];
        now = currentAddr(i);

        if( now < 0 )
        {
            ++deleted[entryP->bank];
            fprintf(fP, "map: %d %04o deleted\n", entryP->bank, entryP->addr);
        }
        else if( now != entryP->addr )
        {
            fprintf(fP, "map: %d %04o %04o\n", entryP->bank, entryP->addr, now);
        }
    }

    // A slot made, named by its key's first reference in the last walk.
    for( extraP = extrasP; extraP; extraP = extraP->nextP )
    {
        if( !extraP->live )
        {
            continue;
        }

        for( k = 0, keyP = NILP; k < keyCount; ++k )
        {
            if( keysP[k].slotP == extraP->symP )
            {
                keyP = &keysP[k];
                break;
            }
        }

        fprintf(fP, "split: bank %d line %d slot %04o value %06o refs %d\n",
            poolBankP[extraP->pool], (keyP)?keyP->firstRefP->lineNo:0, extraP->symP->value,
            (extraP->symP->value2 & WRDMASK), (keyP)?keyP->refs:0);
    }

    for( b = 0; b <= MAXBANK; ++b )
    {
        if( before[b] || grownP[b] || copiedP[b] )
        {
            fprintf(fP, "reconcile: bank %d before %d deleted %d grown %d copied %d after %d\n",
                b, before[b], deleted[b], grownP[b], copiedP[b],
                (before[b] - deleted[b] + grownP[b] + copiedP[b]));
        }
    }
}
