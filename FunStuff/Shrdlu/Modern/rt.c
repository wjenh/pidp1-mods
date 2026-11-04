// The Lisp run-time: the list cells and their collector, the symbols, the
// non-local exits, the evaluator and the primitives. The evaluator runs what
// the program makes as data at run time (a word's meaning, a Planner step, an
// answer's action) and calls the compiled functions by their symbols. Every
// value lives in the cells, the symbols, the binding stack or the C stack, and
// the collector reads the C stack conservatively, so generated code holds
// values in plain C variables. Runs on the program's own thread.

#include <alloca.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "rt.h"

// Address space only; pages are touched as they are used.
#define CELL_SPACE ((size_t)1 << 36)
#define BIND_SPACE ((size_t)1 << 33)
#define SYM_SPACE ((size_t)1 << 32)
#define MARK_SPACE ((size_t)1 << 32)
#define EXTEND_CELLS ((size_t)1 << 19)
#define MIN_GC_CELLS ((size_t)1 << 21)
// Past this many cells live after a collection, a sentence is running away.
#define MAX_LIVE_CELLS ((size_t)1 << 25)
// Past this much of the processor, likewise: the longest sentence takes a
// hundredth of a second, and a planner going round for good may make few cells.
#define MAX_SENTENCE_NANOS (10LL * 1000000000LL)
// How many evaluations between looks at the clock.
#define EVAL_CHECK_MASK 0xFFFFu
#define BUCKETS 8192
#define LAM_BUCKETS 16384

Binding *rtBindP;
Binding *rtBindBaseP;
Frame *rtFrameP;
int rtLexN;
V *rtLexArgsP;
Cell *rtFreeP;

static Cell *cellBaseP;
static size_t cellTop;
static size_t cellImage;
static uint8_t *cellMarkP;
static size_t cellsSinceGc;
static size_t minGcCells = MIN_GC_CELLS;
static size_t extendBy = EXTEND_CELLS;
static int gcTight;
static size_t gcThreshold = MIN_GC_CELLS;
static long long sentenceStartNanos;
static unsigned evalCount;
static size_t cellsLive;
static size_t gcCount;

static Symbol *symBaseP;
static size_t symTop;
static Symbol *symFreeP;
static void **internP[BUCKETS];
static int internN[BUCKETS];

static void *stackBaseP;
static V *rootsP[64];
static int nRoots;
static V *markStackP;

static struct LamEntry
{
    V key;
    RtFn fn;
} lamTable[LAM_BUCKETS];

static Symbol *symT;
static Symbol *symLambda;
static Symbol *symHeader;
static Symbol *symChrct;
static Symbol *symLinel;
static Symbol *symParse;

static int parseCalls;
static int gensymN;
static int outCol;

static void gcNow(void);

// Allocates address space that reads as zero until written.
static void *
reserve(size_t bytes)
{
    void *p;

    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if(p == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
    return p;
}

// Carves more of the space into the free list.
static void
extendCells(void)
{
    size_t i;
    Cell *listP;

    listP = rtFreeP;
    for(i = cellTop + extendBy; i > cellTop; i--)
    {
        cellBaseP[i - 1].cdr = (V)listP;
        listP = &cellBaseP[i - 1];
    }
    cellTop += extendBy;
    rtFreeP = listP;
    cellsSinceGc += extendBy;
}

// A cons when the free list is empty: collect if enough has been made since
// the last collection, else take more space. Under SHRDLU_GC_CELLS every empty
// list collects, and space is added only to leave that many cells free.
V
rtConsSlow(V a, V d)
{
    volatile V keepA;
    volatile V keepD;

    keepA = a;
    keepD = d;
    if(gcTight || (cellsSinceGc >= gcThreshold))
    {
        gcNow();
        if(cellsLive > MAX_LIVE_CELLS)
        {
            rtAbort("THE LIST CELLS ARE FULL");
        }
    }
    if(!rtFreeP || (gcTight && (cellsSinceGc < minGcCells)))
    {
        extendCells();
    }
    return rtCons(keepA, keepD);
}

// Pushes what a value holds onto the mark stack, if it is not marked yet.
static inline V *
markValue(V x, V *spP)
{
    size_t k;
    Symbol *symP;

    if(x == NIL)
    {
        return spP;
    }
    if((x & TAGMASK) == TAGCONS)
    {
        k = (size_t)(CELLP(x) - cellBaseP);
        if(!cellMarkP[k])
        {
            cellMarkP[k] = 1;
            *spP = x;
            spP++;
        }
    }
    else if(IS_SYM(x))
    {
        symP = SYMP(x);
        if((symP >= symBaseP) && (symP < (symBaseP + symTop)) && !symP->mark)
        {
            symP->mark = 1;
            *spP = x;
            spP++;
        }
    }
    return spP;
}

// Marks everything reachable from the mark stack.
static void
markDrain(V *spP)
{
    V x;
    Symbol *symP;

    while(spP > markStackP)
    {
        spP--;
        x = *spP;
        if(IS_SYM(x))
        {
            symP = SYMP(x);
            spP = markValue(symP->value, spP);
            spP = markValue(symP->plist, spP);
            spP = markValue(symP->def, spP);
        }
        else
        {
            spP = markValue(CELLP(x)->car, spP);
            spP = markValue(CELLP(x)->cdr, spP);
        }
    }
}

static void
markRoot(V x)
{
    V *spP;

    spP = markValue(x, markStackP);
    markDrain(spP);
}

// A word on the C stack that may be a value: anything pointing into a live
// cell or at a made symbol counts, interior pointers too, since the compiler
// may keep only the address of a cell's cdr.
static void
markWord(V x)
{
    uintptr_t base;
    uintptr_t off;

    base = (uintptr_t)cellBaseP;
    if((x >= base) && (x < (base + (cellTop * sizeof(Cell)))))
    {
        off = (x - base) & ~(uintptr_t)(sizeof(Cell) - 1);
        markRoot(base + off);
        return;
    }
    base = (uintptr_t)symBaseP;
    if((x >= base) && (x < (base + (symTop * sizeof(Symbol)))))
    {
        off = (x - base) / sizeof(Symbol);
        // A freed symbol's value cell links the free ones.
        if(symBaseP[off].nameP)
        {
            markRoot(SYMV(symBaseP + off));
        }
    }
}

// Reads the C stack, from here to the thread's base. noinline, so that the
// registers the caller spilled are below this frame's address. Every word of it
// is read on purpose, the unused ones too, so a sanitizer is told not to mind.
__attribute__((noinline, no_sanitize("address"))) static void
markStack(void)
{
    volatile uintptr_t here;
    uintptr_t *wordP;
    uintptr_t *topP;

    __builtin_unwind_init();
    here = 0;
    wordP = (uintptr_t *)(((uintptr_t)&here) & ~(uintptr_t)7);
    topP = (uintptr_t *)stackBaseP;
    for(; wordP < topP; wordP++)
    {
        markWord(*wordP);
    }
}

// The collection: mark from the symbols, the constants, the binding stack and
// the C stack, then sweep, every free cell in address order for the caches.
__attribute__((noinline)) static void
gcNow(void)
{
    int i;
    size_t k;
    Binding *bP;
    Frame *fP;
    Cell *listP;
    size_t live;
    Symbol *symP;
    Cell *freeP;
    Cell *nextP;

    gcCount++;
    // A stale word on the C stack may point at a free cell. Cleared, it holds
    // that one cell, where the list's own links would hold all the rest.
    for(freeP = rtFreeP; freeP; freeP = nextP)
    {
        nextP = (Cell *)freeP->cdr;
        freeP->car = NIL;
        freeP->cdr = NIL;
    }
    memset(cellMarkP, 0, cellTop);
    for(k = 0; k < symTop; k++)
    {
        symBaseP[k].mark = 0;
    }
    for(i = 0; i < imgNSym; i++)
    {
        markRoot(imgSyms[i].value);
        markRoot(imgSyms[i].plist);
        markRoot(imgSyms[i].def);
    }
    for(i = 0; i < imgNK; i++)
    {
        markRoot(K[i]);
    }
    for(k = 0; k < symTop; k++)
    {
        if(symBaseP[k].interned)
        {
            markRoot(SYMV(&symBaseP[k]));
        }
    }
    for(bP = rtBindBaseP; bP < rtBindP; bP++)
    {
        markRoot(SYMV(bP->symP));
        markRoot(bP->old);
    }
    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        markRoot(fP->value);
        markRoot(fP->tag);
    }
    for(i = 0; i < nRoots; i++)
    {
        markRoot(*rootsP[i]);
    }
    markStack();

    // Image cells stay allocated whatever marks them: they are the program's
    // constants, and K's own list of them is not every one.
    listP = NULL;
    live = cellImage;
    for(k = cellTop; k > cellImage; k--)
    {
        if(cellMarkP[k - 1])
        {
            live++;
        }
        else
        {
            cellBaseP[k - 1].cdr = (V)listP;
            listP = &cellBaseP[k - 1];
        }
    }
    rtFreeP = listP;
    cellsLive = live;
    // The free cells count as made, so the next collection comes once they
    // are used and what the threshold still allows has been carved.
    cellsSinceGc = cellTop - live;
    gcThreshold = ((live * 2) > minGcCells) ? (live * 2) : minGcCells;

    for(k = 0; k < symTop; k++)
    {
        symP = &symBaseP[k];
        if(!symP->mark && symP->nameP)
        {
            free((void *)symP->nameP);
            symP->nameP = NULL;
            symP->value = (V)symFreeP;
            symFreeP = symP;
        }
    }
}

size_t
rtCellsLive(void)
{
    return cellsLive;
}

size_t
rtCollections(void)
{
    return gcCount;
}

// A collector root in the host's data.
void
rtAddRoot(V *rootP)
{
    rootsP[nRoots] = rootP;
    nRoots++;
}

static unsigned
hashName(const char *nameP)
{
    unsigned h;

    h = 5381;
    while(*nameP)
    {
        h = (h * 33) + (unsigned char)*nameP;
        nameP++;
    }
    return h & (BUCKETS - 1);
}

// A new symbol, not interned; a freed made symbol's slot if there is one.
static Symbol *
newSymbol(const char *nameP)
{
    Symbol *symP;

    symP = symFreeP;
    if(symP)
    {
        symFreeP = (Symbol *)symP->value;
    }
    else
    {
        symP = &symBaseP[symTop];
        symTop++;
    }
    memset(symP, 0, sizeof(*symP));
    symP->nameP = strdup(nameP);
    symP->value = UNBOUNDV;
    return symP;
}

static void
addInterned(Symbol *symP)
{
    unsigned h;

    h = hashName(symP->nameP);
    internP[h] = realloc(internP[h], (size_t)(internN[h] + 1) * sizeof(void *));
    internP[h][internN[h]] = symP;
    internN[h]++;
}

// The interned symbol of a name, made if it is new.
Symbol *
rtIntern(const char *nameP)
{
    unsigned h;
    int i;
    Symbol *symP;

    h = hashName(nameP);
    for(i = 0; i < internN[h]; i++)
    {
        symP = internP[h][i];
        if(!strcmp(symP->nameP, nameP))
        {
            return symP;
        }
    }
    symP = newSymbol(nameP);
    symP->interned = 1;
    addInterned(symP);
    return symP;
}

static V
decode(int64_t e)
{
    int64_t k;

    k = e >> 3;
    switch(e & 7)
    {
    case 1:
        return (V)&cellBaseP[k];
    case 2:
        return SYMV(&imgSyms[k]);
    case 3:
        return FIX(k);
    case 4:
        return UNBOUNDV;
    case 5:
        return (((V)&imgFloat[k]) | TAGFLT);
    case 6:
        return SYMV(symT);
    default:
        return NIL;
    }
}

static unsigned
lamHash(V key)
{
    return (unsigned)((key >> 4) * 2654435761u) & (LAM_BUCKETS - 1);
}

// The compiled function of a quoted LAMBDA or a unit's list; NULL if none.
static RtFn
lamFind(V key)
{
    unsigned h;

    h = lamHash(key);
    while(lamTable[h].key)
    {
        if(lamTable[h].key == key)
        {
            return lamTable[h].fn;
        }
        h = (h + 1) & (LAM_BUCKETS - 1);
    }
    return NULL;
}

static void
lamAdd(V key, RtFn fn)
{
    unsigned h;

    h = lamHash(key);
    while(lamTable[h].key)
    {
        h = (h + 1) & (LAM_BUCKETS - 1);
    }
    lamTable[h].key = key;
    lamTable[h].fn = fn;
}

static void installSubrs(void);
static double numD(V x);

// The symbols' and the cells' contents from the tables.
static void
loadImage(void)
{
    int i;

    for(i = 0; i < imgNSym; i++)
    {
        imgSyms[i].value = decode(imgSymValue[i]);
        imgSyms[i].plist = decode(imgSymPlist[i]);
        imgSyms[i].def = decode(imgSymDef[i]);
        imgSyms[i].kind = imgSymKind[i];
        imgSyms[i].fn = NULL;
    }
    for(i = 0; i < imgNCell; i++)
    {
        cellBaseP[i].car = decode(imgCell[2 * i]);
        cellBaseP[i].cdr = decode(imgCell[(2 * i) + 1]);
    }
    cellTop = (size_t)imgNCell;
    cellImage = cellTop;
    cellsLive = cellImage;
    cellsSinceGc = 0;
    gcThreshold = minGcCells;
    rtFreeP = NULL;
    for(i = 0; i < imgNK; i++)
    {
        K[i] = decode(imgK[i]);
    }
    for(i = 0; i < imgNFn; i++)
    {
        imgSyms[imgFn[i].sym].fn = imgFn[i].fn;
    }
}

// Builds the Lisp world from the generated tables. The image's pieces are
// spread across the binary as integers rather than as initialized pointers so
// that a compile of the tables stays quick and the binary stays position
// independent.
void
rtInit(void *baseP)
{
    int i;
    const char *cellsP;

    cellsP = getenv("SHRDLU_GC_CELLS");
    if(cellsP)
    {
        minGcCells = strtoul(cellsP, NULL, 0);
        gcThreshold = minGcCells;
        extendBy = minGcCells;
        gcTight = 1;
    }
    stackBaseP = baseP;
    cellBaseP = reserve(CELL_SPACE);
    cellMarkP = reserve(MARK_SPACE);
    rtBindBaseP = reserve(BIND_SPACE);
    rtBindP = rtBindBaseP;
    symBaseP = reserve(SYM_SPACE);
    markStackP = reserve(MARK_SPACE);

    for(i = 0; i < imgNSym; i++)
    {
        imgSyms[i].nameP = imgSymName[i];
        imgSyms[i].kind = imgSymKind[i];
        imgSyms[i].interned = imgSymInterned[i];
        if(imgSyms[i].interned)
        {
            addInterned(&imgSyms[i]);
        }
    }
    symT = rtIntern("T");
    loadImage();
    for(i = 0; i < imgNLam; i++)
    {
        lamAdd(K[imgLam[i].k], imgLam[i].fn);
    }
    symLambda = rtIntern("LAMBDA");
    symHeader = newSymbol("header word");
    symHeader->interned = 1;
    symChrct = rtIntern("CHRCT");
    symLinel = rtIntern("LINEL");
    symParse = rtIntern("PARSE");
    installSubrs();
}

// The Lisp world back as it starts: only the words typed since stay as names,
// with nothing on them. Nothing may be running.
void
rtReload(void)
{
    size_t k;
    Symbol *symP;

    for(k = 0; k < symTop; k++)
    {
        symP = &symBaseP[k];
        if(symP->interned && symP->nameP)
        {
            symP->value = UNBOUNDV;
            symP->plist = NIL;
            symP->def = NIL;
            symP->kind = KIND_NONE;
            symP->fn = NULL;
        }
    }
    rtBindP = rtBindBaseP;
    gensymN = 0;
    parseCalls = 0;
    loadImage();
    installSubrs();
}

// Calls fn with every symbol the program started with.
void
rtEachSymbol(void (*fn)(Symbol *symP))
{
    int i;

    for(i = 0; i < imgNSym; i++)
    {
        fn(&imgSyms[i]);
    }
}

// The CAR of an atom is its header word, EQ to nothing a program names.
V
rtCarAtom(V x)
{
    (void)x;
    return SYMV(symHeader);
}

// The CDR of a symbol is its property list; of a number, an error.
V
rtCdrAtom(V x)
{
    if(IS_SYM(x))
    {
        return SYMP(x)->plist;
    }
    rtError("CDR OF ATOM", x);
}

V
rtUnboundVar(Symbol *symP)
{
    rtError("UNBOUND VARIABLE", SYMV(symP));
}

// Restores the bindings made since a mark.
void
rtUnbind(size_t top)
{
    Binding *lowP;

    lowP = rtBindBaseP + top;
    while(rtBindP > lowP)
    {
        rtBindP--;
        rtBindP->symP->value = rtBindP->old;
    }
}

void
rtFrameEnter(Frame *frameP, int kind)
{
    frameP->prevP = rtFrameP;
    frameP->kind = kind;
    frameP->nTags = 0;
    frameP->tagsP = NULL;
    frameP->tag = NIL;
    frameP->value = NIL;
    frameP->bindTop = rtBindMark();
    frameP->lexN = rtLexN;
    frameP->lexArgsP = rtLexArgsP;
    rtFrameP = frameP;
}

void
rtProgEnter(Frame *frameP, const V *tagsP, int nTags)
{
    rtFrameEnter(frameP, FRAME_PROG);
    frameP->tagsP = tagsP;
    frameP->nTags = nTags;
}

// Leaves everything above a frame and jumps to it.
static void unwindTo(Frame *frameP, int code) __attribute__((noreturn));
static void
unwindTo(Frame *frameP, int code)
{
    rtUnbind(frameP->bindTop);
    rtFrameP = frameP;
    rtLexN = frameP->lexN;
    rtLexArgsP = frameP->lexArgsP;
    longjmp(frameP->jump, code);
}

// The sentence's frame.
static Frame *
topFrame(void)
{
    Frame *fP;

    fP = rtFrameP;
    while(fP->kind != FRAME_TOP)
    {
        fP = fP->prevP;
    }
    return fP;
}

void
rtAbort(const char *msgP)
{
    if(getenv("SHRDLU_DEBUG"))
    {
        fprintf(stderr, "abort: %s\n", msgP);
    }
    unwindTo(topFrame(), STOP_ABORT);
}

void
rtStopInput(void)
{
    unwindTo(topFrame(), STOP_INPUT);
}

// An error MacLisp would signal: the innermost ERRSET gives NIL, or the
// sentence is abandoned.
void
rtError(const char *msgP, V what)
{
    Frame *fP;
    char *textP;

    if(getenv("SHRDLU_DEBUG"))
    {
        textP = rtToString(what, 1);
        fprintf(stderr, "error: %s %s\n", msgP, textP);
        free(textP);
    }
    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        if(fP->kind == FRAME_ERRSET)
        {
            fP->value = NIL;
            unwindTo(fP, 1);
        }
        if(fP->kind == FRAME_TOP)
        {
            unwindTo(fP, STOP_ABORT);
        }
    }
    abort();
}

// A GO: the innermost active PROG with the tag.
void
rtGo(V tag)
{
    Frame *fP;
    int i;

    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        if(fP->kind == FRAME_PROG)
        {
            for(i = 0; i < fP->nTags; i++)
            {
                if(fP->tagsP[i] == tag)
                {
                    unwindTo(fP, i + 2);
                }
            }
        }
        if(fP->kind == FRAME_TOP)
        {
            rtError("GO TO A MISSING TAG", tag);
        }
    }
    abort();
}

// A RETURN: the innermost active PROG.
void
rtReturn(V value)
{
    Frame *fP;

    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        if(fP->kind == FRAME_PROG)
        {
            fP->value = value;
            unwindTo(fP, 1);
        }
        if(fP->kind == FRAME_TOP)
        {
            rtError("RETURN WITH NO PROG", value);
        }
    }
    abort();
}

// (ERR x): the innermost ERRSET returns x.
static void errExit(V value) __attribute__((noreturn));
static void
errExit(V value)
{
    Frame *fP;

    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        if(fP->kind == FRAME_ERRSET)
        {
            fP->value = value;
            unwindTo(fP, 1);
        }
        if(fP->kind == FRAME_TOP)
        {
            unwindTo(fP, STOP_ABORT);
        }
    }
    abort();
}

// The evaluator. A form's arguments go on the C stack, where the collector
// sees them.
// The processor time this thread has had, which waiting does not add to.
static long long
threadNanos(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ((long long)ts.tv_sec * 1000000000LL) + ts.tv_nsec;
}

// Abandons a sentence that has had its share of the processor.
static void
evalCheck(void)
{
    if((threadNanos() - sentenceStartNanos) > MAX_SENTENCE_NANOS)
    {
        rtAbort("THE SENTENCE RAN AWAY");
    }
}

V
rtEval(V x)
{
    V f;
    V rest;
    V v;
    V *aP;
    int n;
    Symbol *symP;

    evalCount++;
    if((evalCount & EVAL_CHECK_MASK) == 0)
    {
        evalCheck();
    }
    if(IS_SYM(x))
    {
        return rtVal(SYMP(x));
    }
    if(!IS_CONS(x))
    {
        return x;
    }
    f = CELLP(x)->car;
    rest = CELLP(x)->cdr;
    if(IS_SYM(f))
    {
        symP = SYMP(f);
        switch(symP->kind)
        {
        case KIND_FSUBR:
            return symP->fn(1, &rest);
        case KIND_FEXPR:
            if(symP->fn)
            {
                return symP->fn(1, &rest);
            }
            return rtLam(symP->def, 1, &rest);
        case KIND_MACRO:
            aP = &x;
            if(symP->fn)
            {
                return rtEval(symP->fn(1, aP));
            }
            return rtEval(rtLam(symP->def, 1, aP));
        default:
            break;
        }
    }
    if(IS_SYM(f) && (SYMP(f)->kind == KIND_NONE))
    {
        rtCheckCallable(SYMP(f));
    }
    n = 0;
    for(v = rest; IS_CONS(v); v = CELLP(v)->cdr)
    {
        n++;
    }
    aP = alloca(((size_t)n + 1) * sizeof(V));
    n = 0;
    for(v = rest; IS_CONS(v); v = CELLP(v)->cdr)
    {
        aP[n] = rtEval(CELLP(v)->car);
        n++;
    }
    if(IS_SYM(f))
    {
        symP = SYMP(f);
        if(symP->kind == KIND_NONE)
        {
            return rtCallVar(symP, n, aP);
        }
        if(symP->fn)
        {
            return symP->fn(n, aP);
        }
        return rtLam(symP->def, n, aP);
    }
    if(IS_CONS(f) && (CELLP(f)->car == SYMV(symLambda)))
    {
        return rtApply(f, n, aP);
    }
    return rtApply(rtEval(f), n, aP);
}

// A form's head names no function: its variable must hold one, and that is
// asked before the arguments are evaluated.
void
rtCheckCallable(Symbol *symP)
{
    if((symP->kind == KIND_NONE) && ((symP->value == UNBOUNDV) || (symP->value == NIL)))
    {
        rtError("UNDEFINED FUNCTION", SYMV(symP));
    }
}

// A form whose head names no function: its variable's value is called.
V
rtCallVar(Symbol *symP, int n, V *argsP)
{
    return rtApply((symP->kind == KIND_NONE) ? symP->value : SYMV(symP), n, argsP);
}

// Applies a (LAMBDA params . body) list. The bindings are undone by the
// frame an exit lands on, if one is taken.
V
rtLam(V lam, int n, V *argsP)
{
    V params;
    V body;
    V v;
    size_t mark;
    int savedN;
    V *savedArgsP;
    int i;

    // A function property may name another function (DEFPROP THERT ERT FEXPR).
    while(IS_SYM(lam) && (SYMP(lam)->kind != KIND_NONE))
    {
        if(SYMP(lam)->fn)
        {
            return SYMP(lam)->fn(n, argsP);
        }
        lam = SYMP(lam)->def;
    }
    if(!IS_CONS(lam) || (CELLP(lam)->car != SYMV(symLambda)))
    {
        rtError("NOT A LAMBDA", lam);
    }
    params = rtCar(CELLP(lam)->cdr);
    body = rtCdr(CELLP(lam)->cdr);
    mark = rtBindMark();
    savedN = rtLexN;
    savedArgsP = rtLexArgsP;
    if(IS_SYM(params))
    {
        rtBind(SYMP(params), FIX(n));
        rtLexN = n;
        rtLexArgsP = argsP;
    }
    else
    {
        i = 0;
        for(; IS_CONS(params); params = CELLP(params)->cdr)
        {
            if(!IS_SYM(CELLP(params)->car))
            {
                rtError("BAD LAMBDA VARIABLE", lam);
            }
            rtBind(SYMP(CELLP(params)->car), (i < n) ? argsP[i] : NIL);
            i++;
        }
        if(i != n)
        {
            rtError("WRONG NUMBER OF ARGS", lam);
        }
    }
    v = NIL;
    for(; IS_CONS(body); body = CELLP(body)->cdr)
    {
        v = rtEval(CELLP(body)->car);
    }
    rtUnbind(mark);
    rtLexN = savedN;
    rtLexArgsP = savedArgsP;
    return v;
}

// APPLY's function of a list of values, or of the arguments as an FEXPR or
// FSUBR takes them.
V
rtApply(V fn, int n, V *argsP)
{
    Symbol *symP;
    RtFn compiledFn;
    V list;

    if(IS_SYM(fn))
    {
        symP = SYMP(fn);
        switch(symP->kind)
        {
        case KIND_NONE:
            if((symP->value != UNBOUNDV) && (symP->value != NIL) && (symP->value != fn))
            {
                return rtApply(symP->value, n, argsP);
            }
            rtError("UNDEFINED FUNCTION", fn);
        case KIND_SUBR:
            return symP->fn(n, argsP);
        case KIND_EXPR:
            if(symP->fn)
            {
                return symP->fn(n, argsP);
            }
            return rtLam(symP->def, n, argsP);
        case KIND_FEXPR:
            list = rtList(n, argsP);
            if(symP->fn)
            {
                return symP->fn(1, &list);
            }
            return rtLam(symP->def, 1, &list);
        case KIND_FSUBR:
            list = rtList(n, argsP);
            return symP->fn(1, &list);
        default:
            rtError("CANNOT APPLY", fn);
        }
    }
    if(IS_CONS(fn) && (CELLP(fn)->car == SYMV(symLambda)))
    {
        compiledFn = lamFind(fn);
        if(compiledFn)
        {
            return compiledFn(n, argsP);
        }
        return rtLam(fn, n, argsP);
    }
    rtError("CANNOT APPLY", fn);
}

V
rtApplyList(V fn, V list)
{
    int n;
    V p;
    V *aP;

    if(IS_SYM(fn) && (SYMP(fn)->kind == KIND_FEXPR))
    {
        if(SYMP(fn)->fn)
        {
            return SYMP(fn)->fn(1, &list);
        }
        return rtLam(SYMP(fn)->def, 1, &list);
    }
    if(IS_SYM(fn) && (SYMP(fn)->kind == KIND_FSUBR))
    {
        return SYMP(fn)->fn(1, &list);
    }
    n = 0;
    for(p = list; IS_CONS(p); p = CELLP(p)->cdr)
    {
        n++;
    }
    aP = alloca(((size_t)n + 1) * sizeof(V));
    n = 0;
    for(p = list; IS_CONS(p); p = CELLP(p)->cdr)
    {
        aP[n] = CELLP(p)->car;
        n++;
    }
    return rtApply(fn, n, aP);
}

V
rtList(int n, V *argsP)
{
    V x;

    x = NIL;
    while(n > 0)
    {
        n--;
        x = rtCons(argsP[n], x);
    }
    return x;
}

V
rtEqual(V a, V b)
{
    for(;;)
    {
        if(a == b)
        {
            return SYMV(symT);
        }
        if(IS_CONS(a) && IS_CONS(b))
        {
            if(!rtEqual(CELLP(a)->car, CELLP(b)->car))
            {
                return NIL;
            }
            a = CELLP(a)->cdr;
            b = CELLP(b)->cdr;
            continue;
        }
        if(IS_FLT(a) && IS_FLT(b))
        {
            return (*(double *)(a - TAGFLT) == *(double *)(b - TAGFLT)) ? SYMV(symT) : NIL;
        }
        return NIL;
    }
}

V
rtMemq(V x, V list)
{
    while(IS_CONS(list))
    {
        if(CELLP(list)->car == x)
        {
            return list;
        }
        list = CELLP(list)->cdr;
    }
    return NIL;
}

V
rtAssq(V x, V list)
{
    V e;

    while(IS_CONS(list))
    {
        e = CELLP(list)->car;
        if(IS_CONS(e) && (CELLP(e)->car == x))
        {
            return e;
        }
        list = CELLP(list)->cdr;
    }
    return NIL;
}

// GET: of a symbol its property list; of a list its CDR is a property list too,
// which Micro-Planner's assertions lean on.
V
rtGet(V sym, V ind)
{
    V p;

    if(IS_SYM(sym))
    {
        p = SYMP(sym)->plist;
    }
    else if(IS_CONS(sym))
    {
        p = CELLP(sym)->cdr;
    }
    else
    {
        return NIL;
    }
    while(IS_CONS(p) && IS_CONS(CELLP(p)->cdr))
    {
        if(CELLP(p)->car == ind)
        {
            return CELLP(CELLP(p)->cdr)->car;
        }
        p = CELLP(CELLP(p)->cdr)->cdr;
    }
    return NIL;
}

static int
funKind(V ind)
{
    static const char *const namesP[] = {"SUBR", "FSUBR", "EXPR", "FEXPR", "MACRO"};
    int i;

    if(!IS_SYM(ind))
    {
        return 0;
    }
    for(i = 0; i < 5; i++)
    {
        if(!strcmp(SYMP(ind)->nameP, namesP[i]))
        {
            return i + KIND_SUBR;
        }
    }
    return 0;
}

V
rtPutprop(V sym, V val, V ind)
{
    V p;
    V *headP;
    int kind;

    if(IS_CONS(sym))
    {
        headP = &CELLP(sym)->cdr;
    }
    else if(IS_SYM(sym))
    {
        headP = &SYMP(sym)->plist;
    }
    else
    {
        rtError("PUTPROP ON", sym);
    }
    p = *headP;
    while(IS_CONS(p) && IS_CONS(CELLP(p)->cdr))
    {
        if(CELLP(p)->car == ind)
        {
            CELLP(CELLP(p)->cdr)->car = val;
            break;
        }
        p = CELLP(CELLP(p)->cdr)->cdr;
    }
    if(!IS_CONS(p) || !IS_CONS(CELLP(p)->cdr))
    {
        *headP = rtCons(ind, rtCons(val, *headP));
    }
    if(IS_SYM(sym))
    {
        kind = funKind(ind);
        if(kind)
        {
            SYMP(sym)->kind = (uint8_t)kind;
            SYMP(sym)->def = val;
            SYMP(sym)->fn = NULL;
        }
    }
    return val;
}

static V
remprop(V sym, V ind)
{
    V *linkP;
    V p;
    V old;
    int kind;

    if(IS_CONS(sym))
    {
        linkP = &CELLP(sym)->cdr;
    }
    else if(IS_SYM(sym))
    {
        linkP = &SYMP(sym)->plist;
    }
    else
    {
        return NIL;
    }
    for(;;)
    {
        p = *linkP;
        if(!IS_CONS(p) || !IS_CONS(CELLP(p)->cdr))
        {
            return NIL;
        }
        if(CELLP(p)->car == ind)
        {
            old = CELLP(CELLP(p)->cdr)->car;
            *linkP = CELLP(CELLP(p)->cdr)->cdr;
            kind = funKind(ind);
            if(kind && IS_SYM(sym) && (SYMP(sym)->def == old))
            {
                SYMP(sym)->kind = KIND_NONE;
                SYMP(sym)->def = NIL;
                SYMP(sym)->fn = NULL;
                for(p = SYMP(sym)->plist; IS_CONS(p) && IS_CONS(CELLP(p)->cdr);
                    p = CELLP(CELLP(p)->cdr)->cdr)
                {
                    kind = funKind(CELLP(p)->car);
                    if(kind)
                    {
                        SYMP(sym)->kind = (uint8_t)kind;
                        SYMP(sym)->def = CELLP(CELLP(p)->cdr)->car;
                        break;
                    }
                }
            }
            return SYMV(symT);
        }
        linkP = &CELLP(CELLP(p)->cdr)->cdr;
    }
}

// A number's value as a double, for the rare float; an error for a non-number.
double
rtNumber(V x)
{
    return numD(x);
}

static double
numD(V x)
{
    if(IS_FIX(x))
    {
        return (double)FIXV(x);
    }
    if(IS_FLT(x))
    {
        return *(double *)(x - TAGFLT);
    }
    rtError("NON-NUMERIC", x);
}

// A float's box. Floats are few, the system's two constants and what is
// computed from them, so the boxes are left to the C heap and not collected.
static V
floatValue(double d)
{
    double *boxP;

    boxP = malloc(sizeof(double));
    *boxP = d;
    return ((V)boxP) | TAGFLT;
}

static V
numResult(double d, int anyFloat)
{
    if(anyFloat)
    {
        return floatValue(d);
    }
    return FIX((intptr_t)d);
}

#define SUB(name) static V name(int n, V *a)
#define TV(b) ((b) ? SYMV(symT) : NIL)

SUB(sCar)
{
    (void)n;
    return rtCar(a[0]);
}

SUB(sCdr)
{
    (void)n;
    return rtCdr(a[0]);
}

// The compositions, by their names: CADR walks D then A.
static V
cxr(const char *nameP, V x)
{
    size_t k;

    k = strlen(nameP) - 1;
    while(k > 1)
    {
        k--;
        x = (nameP[k] == 'A') ? rtCar(x) : rtCdr(x);
    }
    return x;
}

#define CXR(fname, lname) SUB(fname) { (void)n; return cxr(lname, a[0]); }
CXR(sCaar, "CAAR") CXR(sCadr, "CADR") CXR(sCdar, "CDAR") CXR(sCddr, "CDDR")
CXR(sCaaar, "CAAAR") CXR(sCaadr, "CAADR") CXR(sCadar, "CADAR") CXR(sCaddr, "CADDR")
CXR(sCdaar, "CDAAR") CXR(sCdadr, "CDADR") CXR(sCddar, "CDDAR") CXR(sCdddr, "CDDDR")
CXR(sCaaaar, "CAAAAR") CXR(sCaaadr, "CAAADR") CXR(sCaadar, "CAADAR") CXR(sCaaddr, "CAADDR")
CXR(sCadaar, "CADAAR") CXR(sCadadr, "CADADR") CXR(sCaddar, "CADDAR") CXR(sCadddr, "CADDDR")
CXR(sCdaaar, "CDAAAR") CXR(sCdaadr, "CDAADR") CXR(sCdadar, "CDADAR") CXR(sCdaddr, "CDADDR")
CXR(sCddaar, "CDDAAR") CXR(sCddadr, "CDDADR") CXR(sCdddar, "CDDDAR") CXR(sCddddr, "CDDDDR")

SUB(sCons)
{
    (void)n;
    return rtCons(a[0], a[1]);
}

SUB(sList)
{
    return rtList(n, a);
}

SUB(sEq)
{
    (void)n;
    return TV(a[0] == a[1]);
}

SUB(sEqual)
{
    (void)n;
    return rtEqual(a[0], a[1]);
}

SUB(sAtom)
{
    (void)n;
    return TV(!IS_CONS(a[0]));
}

SUB(sNull)
{
    (void)n;
    return TV(a[0] == NIL);
}

SUB(sListp)
{
    (void)n;
    return TV((a[0] == NIL) || IS_CONS(a[0]));
}

SUB(sNumberp)
{
    (void)n;
    return TV(IS_FIX(a[0]) || IS_FLT(a[0]));
}

SUB(sFixp)
{
    (void)n;
    return TV(IS_FIX(a[0]));
}

SUB(sSymbolp)
{
    (void)n;
    return TV((a[0] == NIL) || IS_SYM(a[0]));
}

SUB(sZerop)
{
    (void)n;
    return TV(numD(a[0]) == 0.0);
}

SUB(sMinusp)
{
    (void)n;
    return TV(numD(a[0]) < 0.0);
}

SUB(sAdd1)
{
    (void)n;
    return numResult(numD(a[0]) + 1.0, IS_FLT(a[0]));
}

SUB(sSub1)
{
    (void)n;
    return numResult(numD(a[0]) - 1.0, IS_FLT(a[0]));
}

SUB(sPlus)
{
    double v;
    int i;
    int anyFloat;

    v = 0.0;
    anyFloat = 0;
    for(i = 0; i < n; i++)
    {
        v += numD(a[i]);
        anyFloat |= IS_FLT(a[i]);
    }
    return numResult(v, anyFloat);
}

SUB(sDifference)
{
    double v;
    int i;
    int anyFloat;

    anyFloat = IS_FLT(a[0]);
    v = numD(a[0]);
    if(n == 1)
    {
        return numResult(-v, anyFloat);
    }
    for(i = 1; i < n; i++)
    {
        v -= numD(a[i]);
        anyFloat |= IS_FLT(a[i]);
    }
    return numResult(v, anyFloat);
}

SUB(sMinus)
{
    (void)n;
    return numResult(-numD(a[0]), IS_FLT(a[0]));
}

SUB(sTimes)
{
    double v;
    int i;
    int anyFloat;

    v = 1.0;
    anyFloat = 0;
    for(i = 0; i < n; i++)
    {
        v *= numD(a[i]);
        anyFloat |= IS_FLT(a[i]);
    }
    return numResult(v, anyFloat);
}

// Integer division truncates toward zero.
SUB(sQuotient)
{
    double v;
    int i;
    int anyFloat;

    anyFloat = IS_FLT(a[0]);
    v = numD(a[0]);
    for(i = 1; i < n; i++)
    {
        anyFloat |= IS_FLT(a[i]);
        v = anyFloat ? (v / numD(a[i])) : (double)(((intptr_t)v) / FIXV(a[i]));
    }
    return numResult(v, anyFloat);
}

// The sign of the dividend, as the PDP-10's IDIV leaves it.
SUB(sRemainder)
{
    (void)n;
    return FIX(FIXV(a[0]) % FIXV(a[1]));
}

SUB(sMax)
{
    V best;
    int i;

    best = a[0];
    for(i = 1; i < n; i++)
    {
        if(numD(a[i]) > numD(best))
        {
            best = a[i];
        }
    }
    return best;
}

SUB(sMin)
{
    V best;
    int i;

    best = a[0];
    for(i = 1; i < n; i++)
    {
        if(numD(a[i]) < numD(best))
        {
            best = a[i];
        }
    }
    return best;
}

SUB(sGreaterp)
{
    int i;

    for(i = 0; i < (n - 1); i++)
    {
        if(!(numD(a[i]) > numD(a[i + 1])))
        {
            return NIL;
        }
    }
    return SYMV(symT);
}

SUB(sLessp)
{
    int i;

    for(i = 0; i < (n - 1); i++)
    {
        if(!(numD(a[i]) < numD(a[i + 1])))
        {
            return NIL;
        }
    }
    return SYMV(symT);
}

SUB(sNumEq)
{
    (void)n;
    return TV(numD(a[0]) == numD(a[1]));
}

SUB(sZero)
{
    (void)n;
    (void)a;
    return FIX(0);
}

// What the inline arithmetic leaves here: anything not two small numbers.
V
rtArith(int op, V a, V b)
{
    int anyFloat;

    anyFloat = IS_FLT(a) || IS_FLT(b);
    switch(op)
    {
    case ARITH_ADD:
        return numResult(numD(a) + numD(b), anyFloat);
    case ARITH_SUB:
        return numResult(numD(a) - numD(b), anyFloat);
    case ARITH_GREATER:
        return TV(numD(a) > numD(b));
    default:
        return TV(numD(a) < numD(b));
    }
}

SUB(sMemq)
{
    (void)n;
    return rtMemq(a[0], a[1]);
}

SUB(sMember)
{
    V l;

    (void)n;
    for(l = a[1]; IS_CONS(l); l = CELLP(l)->cdr)
    {
        if(rtEqual(a[0], CELLP(l)->car))
        {
            return l;
        }
    }
    return NIL;
}

SUB(sAssq)
{
    (void)n;
    return rtAssq(a[0], a[1]);
}

static V
assoc(V x, V l)
{
    V e;

    for(; IS_CONS(l); l = CELLP(l)->cdr)
    {
        e = CELLP(l)->car;
        if(IS_CONS(e) && rtEqual(x, CELLP(e)->car))
        {
            return e;
        }
    }
    return NIL;
}

SUB(sAssoc)
{
    (void)n;
    return assoc(a[0], a[1]);
}

SUB(sSassoc)
{
    V e;

    (void)n;
    e = assoc(a[0], a[1]);
    return e ? e : rtApply(a[2], 0, NULL);
}

// A copy of a list's cells ending in tail.
static V
copyOnto(V l, V tail)
{
    V head;
    V last;
    V c;

    if(!IS_CONS(l))
    {
        return tail;
    }
    head = rtCons(CELLP(l)->car, tail);
    last = head;
    for(l = CELLP(l)->cdr; IS_CONS(l); l = CELLP(l)->cdr)
    {
        c = rtCons(CELLP(l)->car, tail);
        CELLP(last)->cdr = c;
        last = c;
    }
    return head;
}

SUB(sAppend)
{
    V res;
    int i;

    if(n == 0)
    {
        return NIL;
    }
    res = a[n - 1];
    for(i = n - 2; i >= 0; i--)
    {
        res = copyOnto(a[i], res);
    }
    return res;
}

SUB(sNconc)
{
    V res;
    V last;
    int i;

    res = NIL;
    last = NIL;
    for(i = 0; i < n; i++)
    {
        if(a[i] == NIL)
        {
            continue;
        }
        if(last == NIL)
        {
            res = a[i];
        }
        else
        {
            CELLP(last)->cdr = a[i];
        }
        if(IS_CONS(a[i]))
        {
            last = a[i];
            while(IS_CONS(CELLP(last)->cdr))
            {
                last = CELLP(last)->cdr;
            }
        }
    }
    return res;
}

SUB(sReverse)
{
    V res;
    V l;

    (void)n;
    res = NIL;
    for(l = a[0]; IS_CONS(l); l = CELLP(l)->cdr)
    {
        res = rtCons(CELLP(l)->car, res);
    }
    return res;
}

SUB(sNreverse)
{
    V prev;
    V l;
    V next;

    (void)n;
    prev = NIL;
    l = a[0];
    while(IS_CONS(l))
    {
        next = CELLP(l)->cdr;
        CELLP(l)->cdr = prev;
        prev = l;
        l = next;
    }
    return prev;
}

SUB(sLast)
{
    V l;

    (void)n;
    l = a[0];
    if(!IS_CONS(l))
    {
        return l;
    }
    while(IS_CONS(CELLP(l)->cdr))
    {
        l = CELLP(l)->cdr;
    }
    return l;
}

SUB(sLength)
{
    V l;
    intptr_t k;

    (void)n;
    k = 0;
    for(l = a[0]; IS_CONS(l); l = CELLP(l)->cdr)
    {
        k++;
    }
    return FIX(k);
}

SUB(sRplaca)
{
    (void)n;
    if(!IS_CONS(a[0]))
    {
        rtError("RPLACA OF", a[0]);
    }
    CELLP(a[0])->car = a[1];
    return a[0];
}

SUB(sRplacd)
{
    (void)n;
    if(!IS_CONS(a[0]))
    {
        rtError("RPLACD OF", a[0]);
    }
    CELLP(a[0])->cdr = a[1];
    return a[0];
}

static V
subst(V new, V old, V tree)
{
    V carV;

    if(rtEqual(old, tree))
    {
        return new;
    }
    if(!IS_CONS(tree))
    {
        return tree;
    }
    carV = subst(new, old, CELLP(tree)->car);
    return rtCons(carV, subst(new, old, CELLP(tree)->cdr));
}

SUB(sSubst)
{
    (void)n;
    return subst(a[0], a[1], a[2]);
}

// DELQ and DELETE make a new list here, as the reference's do.
static V
deleted(V x, V l, int byEqual)
{
    V head;
    V *tailP;
    V e;
    V c;

    head = NIL;
    tailP = &head;
    for(; IS_CONS(l); l = CELLP(l)->cdr)
    {
        e = CELLP(l)->car;
        if(byEqual ? (rtEqual(x, e) != NIL) : (x == e))
        {
            continue;
        }
        c = rtCons(e, NIL);
        *tailP = c;
        tailP = &CELLP(c)->cdr;
    }
    return head;
}

SUB(sDelq)
{
    (void)n;
    return deleted(a[0], a[1], 0);
}

SUB(sDelete)
{
    (void)n;
    return deleted(a[0], a[1], 1);
}

SUB(sGet)
{
    (void)n;
    return rtGet(a[0], a[1]);
}

SUB(sPutprop)
{
    (void)n;
    return rtPutprop(a[0], a[1], a[2]);
}

SUB(sRemprop)
{
    (void)n;
    return remprop(a[0], a[1]);
}

SUB(sPlist)
{
    (void)n;
    return IS_SYM(a[0]) ? SYMP(a[0])->plist : NIL;
}

SUB(sSet)
{
    (void)n;
    if(!IS_SYM(a[0]))
    {
        rtError("SET OF", a[0]);
    }
    SYMP(a[0])->value = a[1];
    return a[1];
}

SUB(sSymeval)
{
    (void)n;
    return rtEval(a[0]);
}

SUB(sBoundp)
{
    (void)n;
    return TV(IS_SYM(a[0]) && (SYMP(a[0])->value != UNBOUNDV));
}

SUB(sEval)
{
    (void)n;
    return rtEval(a[0]);
}

SUB(sApply)
{
    (void)n;
    return rtApplyList(a[0], a[1]);
}

SUB(sFuncall)
{
    return rtApply(a[0], n - 1, a + 1);
}

SUB(sProg2)
{
    return (n > 1) ? a[1] : NIL;
}

// The mapping functions, of one or more lists: tails hands the function the
// lists themselves, collect 1 makes a list of the values, 2 joins them.
static V
mapper(int n, V *a, int collect, int tails)
{
    int nLists;
    int i;
    V *lP;
    V *argP;
    V head;
    V *tailP;
    V v;
    V c;
    V p;

    nLists = n - 1;
    lP = alloca((size_t)nLists * sizeof(V));
    argP = alloca((size_t)nLists * sizeof(V));
    for(i = 0; i < nLists; i++)
    {
        lP[i] = a[i + 1];
    }
    head = NIL;
    tailP = &head;
    for(;;)
    {
        for(i = 0; i < nLists; i++)
        {
            if(!IS_CONS(lP[i]))
            {
                return collect ? head : a[1];
            }
            argP[i] = tails ? lP[i] : CELLP(lP[i])->car;
        }
        v = rtApply(a[0], nLists, argP);
        if(collect == 1)
        {
            c = rtCons(v, NIL);
            *tailP = c;
            tailP = &CELLP(c)->cdr;
        }
        else if((collect == 2) && (v != NIL))
        {
            *tailP = v;
            if(IS_CONS(v))
            {
                p = v;
                while(IS_CONS(CELLP(p)->cdr))
                {
                    p = CELLP(p)->cdr;
                }
                tailP = &CELLP(p)->cdr;
            }
        }
        for(i = 0; i < nLists; i++)
        {
            lP[i] = CELLP(lP[i])->cdr;
        }
    }
}

SUB(sMapc)
{
    return mapper(n, a, 0, 0);
}

SUB(sMap)
{
    return mapper(n, a, 0, 1);
}

SUB(sMapcar)
{
    return mapper(n, a, 1, 0);
}

SUB(sMaplist)
{
    return mapper(n, a, 1, 1);
}

SUB(sMapcan)
{
    return mapper(n, a, 2, 0);
}

SUB(sReturn)
{
    rtReturn((n > 0) ? a[0] : NIL);
}

SUB(sErr)
{
    errExit((n > 0) ? a[0] : NIL);
}

// A growing string, for printing to.
typedef struct Text
{
    char *bufP;
    size_t len;
    size_t cap;
} Text;

static void
textAdd(Text *tP, const char *sP, size_t n)
{
    if((tP->len + n + 1) > tP->cap)
    {
        tP->cap = (tP->cap * 2) + n + 64;
        tP->bufP = realloc(tP->bufP, tP->cap);
    }
    memcpy(tP->bufP + tP->len, sP, n);
    tP->len += n;
    tP->bufP[tP->len] = 0;
}

static int
isSlashed(char c)
{
    return (strchr(" \t\r\n\f\v()';/", c) != NULL) || ((c >= 'a') && (c <= 'z'));
}

// Whether a name would read back as a number.
static int
looksNumeric(const char *sP)
{
    const char *pP;
    int digits;
    int dots;

    pP = sP;
    if((*pP == '+') || (*pP == '-'))
    {
        pP++;
    }
    digits = 0;
    dots = 0;
    for(; *pP; pP++)
    {
        if(isdigit((unsigned char)*pP))
        {
            digits++;
        }
        else if(*pP == '.')
        {
            dots++;
        }
        else
        {
            return 0;
        }
    }
    return (digits > 0) && (dots <= 1);
}

static void
atomText(Text *tP, V x, int slashify)
{
    char buf[64];
    const char *sP;

    if(x == NIL)
    {
        textAdd(tP, "NIL", 3);
    }
    else if(IS_SYM(x))
    {
        sP = SYMP(x)->nameP;
        if(slashify != 1)
        {
            textAdd(tP, sP, strlen(sP));
            return;
        }
        if(looksNumeric(sP) || !strcmp(sP, ".") || !*sP)
        {
            textAdd(tP, "/", 1);
        }
        for(; *sP; sP++)
        {
            if(isSlashed(*sP))
            {
                textAdd(tP, "/", 1);
            }
            textAdd(tP, sP, 1);
        }
    }
    else if(IS_FIX(x))
    {
        snprintf(buf, sizeof(buf), slashify ? "%ld." : "%ld", (long)FIXV(x));
        textAdd(tP, buf, strlen(buf));
    }
    else if(IS_FLT(x))
    {
        snprintf(buf, sizeof(buf), "%.1f", *(double *)(x - TAGFLT));
        textAdd(tP, buf, strlen(buf));
    }
    else
    {
        textAdd(tP, "#", 1);
    }
}

static void
valueText(Text *tP, V x, int slashify)
{
    int first;

    if(!IS_CONS(x))
    {
        atomText(tP, x, slashify);
        return;
    }
    textAdd(tP, "(", 1);
    first = 1;
    while(IS_CONS(x))
    {
        if(!first)
        {
            textAdd(tP, " ", 1);
        }
        valueText(tP, CELLP(x)->car, slashify);
        first = 0;
        x = CELLP(x)->cdr;
    }
    if(x != NIL)
    {
        textAdd(tP, " . ", 3);
        atomText(tP, x, slashify);
    }
    textAdd(tP, ")", 1);
}

// A value's printed form in malloc'd space: PRIN1's when slashify, else PRINC's.
char *
rtToString(V x, int slashify)
{
    Text t;

    memset(&t, 0, sizeof(t));
    textAdd(&t, "", 0);
    valueText(&t, x, slashify);
    return t.bufP;
}

static V
charList(const char *sP)
{
    size_t k;
    V res;
    char one[2];

    res = NIL;
    k = strlen(sP);
    one[1] = 0;
    while(k > 0)
    {
        k--;
        one[0] = sP[k];
        res = rtCons(SYMV(rtIntern(one)), res);
    }
    return res;
}

SUB(sExplode)
{
    char *sP;
    V res;

    (void)n;
    sP = rtToString(a[0], 1);
    res = charList(sP);
    free(sP);
    return res;
}

SUB(sExplodec)
{
    char *sP;
    V res;

    (void)n;
    sP = rtToString(a[0], 0);
    res = charList(sP);
    free(sP);
    return res;
}

SUB(sFlatsize)
{
    char *sP;
    V res;

    (void)n;
    sP = rtToString(a[0], 1);
    res = FIX((intptr_t)strlen(sP));
    free(sP);
    return res;
}

SUB(sFlatc)
{
    char *sP;
    V res;

    (void)n;
    sP = rtToString(a[0], 0);
    res = FIX((intptr_t)strlen(sP));
    free(sP);
    return res;
}

// The spelling a list of atoms makes.
static char *
nameOf(V l)
{
    Text t;
    char *sP;

    memset(&t, 0, sizeof(t));
    textAdd(&t, "", 0);
    for(; IS_CONS(l); l = CELLP(l)->cdr)
    {
        sP = rtToString(CELLP(l)->car, 0);
        textAdd(&t, sP, strlen(sP));
        free(sP);
    }
    return t.bufP;
}

SUB(sMaknam)
{
    char *sP;
    V res;

    (void)n;
    sP = nameOf(a[0]);
    res = SYMV(newSymbol(sP));
    free(sP);
    return res;
}

// INTERN of a made symbol: the interned one of that name, or this one.
SUB(sIntern)
{
    Symbol *symP;
    const char *nameP;
    int i;
    unsigned h;

    (void)n;
    if(!IS_SYM(a[0]))
    {
        return a[0];
    }
    symP = SYMP(a[0]);
    if(symP->interned)
    {
        return a[0];
    }
    nameP = symP->nameP;
    h = hashName(nameP);
    for(i = 0; i < internN[h]; i++)
    {
        if(!strcmp(((Symbol *)internP[h][i])->nameP, nameP))
        {
            return SYMV((Symbol *)internP[h][i]);
        }
    }
    symP->interned = 1;
    addInterned(symP);
    return a[0];
}

// READLIST of a word: a token of digits is a number, octal unless it has an
// 8 or a 9 or ends in a point; anything else is a symbol.
SUB(sReadlist)
{
    char *sP;
    V res;
    size_t k;
    size_t len;
    int base;
    int digits;

    (void)n;
    sP = nameOf(a[0]);
    len = strlen(sP);
    if(!len)
    {
        free(sP);
        rtError("READLIST OF NOTHING", NIL);
    }
    digits = 1;
    base = 8;
    for(k = 0; k < len; k++)
    {
        if((k == (len - 1)) && (sP[k] == '.') && (len > 1))
        {
            base = 10;
        }
        else if(!isdigit((unsigned char)sP[k]))
        {
            digits = 0;
        }
        else if(sP[k] >= '8')
        {
            base = 10;
        }
    }
    if(digits)
    {
        res = FIX(strtol(sP, NULL, base));
    }
    else if(!strcmp(sP, "NIL"))
    {
        res = NIL;
    }
    else
    {
        res = SYMV(rtIntern(sP));
    }
    free(sP);
    return res;
}

SUB(sGensym)
{
    char buf[16];

    (void)n;
    (void)a;
    gensymN++;
    snprintf(buf, sizeof(buf), "G%04d", gensymN);
    return SYMV(newSymbol(buf));
}

// A word's name from its characters, newest first, whatever they spell. Empty,
// it is the name of nothing.
SUB(sWordname)
{
    char *sP;
    size_t k;
    V x;
    Symbol *symP;

    (void)n;
    k = 0;
    for(x = a[0]; IS_CONS(x); x = CELLP(x)->cdr)
    {
        k++;
    }
    sP = malloc(k + 1);
    sP[k] = 0;
    for(x = a[0]; IS_CONS(x); x = CELLP(x)->cdr)
    {
        k--;
        sP[k] = SYMP(CELLP(x)->car)->nameP[0];
    }
    symP = rtIntern(sP);
    free(sP);
    return SYMV(symP);
}

// T for a word of digits.
SUB(sDigitword)
{
    const char *sP;

    (void)n;
    for(sP = SYMP(a[0])->nameP; *sP; sP++)
    {
        if(!isdigit((unsigned char)*sP))
        {
            return NIL;
        }
    }
    return SYMV(symT);
}

// T for the word of digits that is 1, whatever zeros lead it.
SUB(sOneword)
{
    (void)n;
    return TV(strtol(SYMP(a[0])->nameP, NULL, 10) == 1);
}

static int
featureOrder(const void *aP, const void *bP)
{
    return strcmp(SYMP(*(const V *)aP)->nameP, SYMP(*(const V *)bP)->nameP);
}

// A word's features in the order of their names, as the dictionary has them.
SUB(sSortfeatures)
{
    V *itemP;
    V x;
    V res;
    int k;
    int i;

    (void)n;
    k = 0;
    for(x = a[0]; IS_CONS(x); x = CELLP(x)->cdr)
    {
        k++;
    }
    itemP = alloca(((size_t)k + 1) * sizeof(V));
    k = 0;
    for(x = a[0]; IS_CONS(x); x = CELLP(x)->cdr)
    {
        itemP[k] = CELLP(x)->car;
        k++;
    }
    qsort(itemP, (size_t)k, sizeof(V), featureOrder);
    res = NIL;
    for(i = k - 1; i >= 0; i--)
    {
        res = rtCons(itemP[i], res);
    }
    return res;
}

SUB(sSmrecord)
{
    (void)n;
    hostBuilt(a[0]);
    return a[0];
}

SUB(sPlandone)
{
    (void)n;
    (void)a;
    hostPlanDone();
    return NIL;
}

SUB(sAscii)
{
    char one[2];

    (void)n;
    one[0] = (char)FIXV(a[0]);
    one[1] = 0;
    return SYMV(rtIntern(one));
}

SUB(sArg)
{
    (void)n;
    if(a[0] == NIL)
    {
        return FIX(rtLexN);
    }
    return rtLexArgsP[FIXV(a[0]) - 1];
}

// Output keeps the column CHRCT reports, what PRINT3 reads to start a line.
void
rtOut(const char *sP, size_t n)
{
    size_t i;
    intptr_t left;

    hostOut(sP, n);
    for(i = 0; i < n; i++)
    {
        if(sP[i] == '\n')
        {
            outCol = 0;
        }
        else
        {
            outCol++;
        }
    }
    left = FIXV(symLinel->value) - outCol;
    symChrct->value = FIX((left > 0) ? left : 0);
}

int
rtOutColumn(void)
{
    return outCol;
}

void
rtOutLineStart(void)
{
    outCol = 0;
    symChrct->value = symLinel->value;
}

// The apostrophe of 'S and N'T is an atom's `"` in the Lisp. A `"` standing
// alone is a quote, and so is one before a point.
static void
outName(const char *sP)
{
    size_t i;
    size_t n;
    char *copyP;

    if(!strcmp(sP, "\"") || !strcmp(sP, "\"."))
    {
        rtOut(sP, strlen(sP));
        return;
    }
    n = strlen(sP);
    copyP = alloca(n + 1);
    for(i = 0; i <= n; i++)
    {
        copyP[i] = (sP[i] == '"') ? '\'' : sP[i];
    }
    rtOut(copyP, n);
}

static void
outValue(V x, int slashify)
{
    char *sP;
    int first;

    if(IS_SYM(x) && !slashify)
    {
        outName(SYMP(x)->nameP);
        return;
    }
    if(!IS_CONS(x))
    {
        sP = rtToString(x, slashify);
        rtOut(sP, strlen(sP));
        free(sP);
        return;
    }
    rtOut("(", 1);
    first = 1;
    while(IS_CONS(x))
    {
        if(!first)
        {
            rtOut(" ", 1);
        }
        outValue(CELLP(x)->car, slashify);
        first = 0;
        x = CELLP(x)->cdr;
    }
    rtOut(")", 1);
}

SUB(sPrinc)
{
    (void)n;
    outValue(a[0], 0);
    return a[0] ? a[0] : SYMV(symT);
}

SUB(sPrin1)
{
    (void)n;
    outValue(a[0], 1);
    return a[0] ? a[0] : SYMV(symT);
}

SUB(sPrint)
{
    (void)n;
    rtOut("\n", 1);
    outValue(a[0], 1);
    rtOut(" ", 1);
    return a[0];
}

SUB(sTerpri)
{
    (void)n;
    (void)a;
    rtOut("\n", 1);
    return SYMV(symT);
}

SUB(sTyo)
{
    char c;

    (void)n;
    c = (FIXV(a[0]) == 12) ? '\n' : (char)FIXV(a[0]);
    rtOut(&c, 1);
    return a[0];
}

SUB(sRuntime)
{
    struct timespec ts;

    (void)n;
    (void)a;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return FIX(((intptr_t)ts.tv_sec * 1000000) + (ts.tv_nsec / 1000));
}

SUB(sNothing)
{
    (void)n;
    (void)a;
    return NIL;
}

SUB(sTyi)
{
    (void)n;
    (void)a;
    return FIX(hostTyi(0));
}

SUB(sTyipeek)
{
    (void)n;
    (void)a;
    return FIX(hostTyi(1));
}

SUB(sReadch)
{
    char one[2];

    (void)n;
    (void)a;
    one[0] = (char)hostTyi(0);
    one[1] = 0;
    return SYMV(rtIntern(one));
}

SUB(sRandom)
{
    (void)n;
    (void)a;
    return hostRandom();
}

SUB(sRead)
{
    (void)n;
    (void)a;
    return hostRead();
}

SUB(sMoveto)
{
    hostDisplay(0, n, a);
    return SYMV(symT);
}

SUB(sGrasp)
{
    hostDisplay(1, n, a);
    return SYMV(symT);
}

SUB(sUngrasp)
{
    hostDisplay(2, n, a);
    return SYMV(symT);
}

SUB(sBlink)
{
    hostDisplay(3, n, a);
    return SYMV(symT);
}

// ERT and BUG are taken as the user proceeding; ERTERR and GLOBAL-ERR abandon
// the sentence.
SUB(sErtex)
{
    (void)n;
    if(IS_CONS(a[0]) && IS_SYM(CELLP(a[0])->car)
       && !strcmp(SYMP(CELLP(a[0])->car)->nameP, "BUG!!!!!!!!!!"))
    {
        return SYMV(symT);
    }
    if(a[1] != NIL)
    {
        rtAbort("ERTERR");
    }
    return SYMV(symT);
}

SUB(sParseStatistics)
{
    Symbol *symP;
    V w;
    V wins;

    (void)n;
    (void)a;
    symP = rtIntern("PARSINGS");
    wins = SYMV(rtIntern("WINS"));
    if(symP->value == FIX(0))
    {
        rtPutprop(SYMV(symP), FIX(0), wins);
    }
    if(rtIntern("RE")->value != NIL)
    {
        w = rtGet(SYMV(symP), wins);
        rtPutprop(SYMV(symP), FIX(FIXV(w) + 1), wins);
    }
    symP->value = FIX(FIXV(symP->value) + 1);
    return symP->value;
}

// A4: a parse past this many calls of PARSE is abandoned.
#define PARSE_BOUND 1000

void
rtCountParse(void)
{
    parseCalls++;
    if(parseCalls > PARSE_BOUND)
    {
        rtAbort("A4: too many calls of PARSE");
    }
}

// The start of a sentence, for A4's count.
void
rtSentenceStart(void)
{
    parseCalls = 0;
    sentenceStartNanos = threadNanos();
}

// PARSE, when no compiled form of it counts its own calls.
static V
parseInterpreted(int n, V *a)
{
    rtCountParse();
    return rtLam(symParse->def, n, a);
}

// The special forms.
#define FSUB(name) static V name(int n, V *a)

FSUB(fQuote)
{
    (void)n;
    return rtCar(a[0]);
}

FSUB(fSetq)
{
    V p;
    V v;

    (void)n;
    v = NIL;
    p = a[0];
    while(IS_CONS(p))
    {
        v = rtEval(rtCar(CELLP(p)->cdr));
        if(!IS_SYM(CELLP(p)->car))
        {
            rtError("SETQ OF", CELLP(p)->car);
        }
        SYMP(CELLP(p)->car)->value = v;
        p = rtCdr(CELLP(p)->cdr);
    }
    return v;
}

FSUB(fCond)
{
    V p;
    V clause;
    V body;
    V v;

    (void)n;
    for(p = a[0]; IS_CONS(p); p = CELLP(p)->cdr)
    {
        clause = CELLP(p)->car;
        v = rtEval(rtCar(clause));
        if(v != NIL)
        {
            for(body = rtCdr(clause); IS_CONS(body); body = CELLP(body)->cdr)
            {
                v = rtEval(CELLP(body)->car);
            }
            return v;
        }
    }
    return NIL;
}

FSUB(fAnd)
{
    V p;
    V v;

    (void)n;
    v = SYMV(symT);
    for(p = a[0]; IS_CONS(p); p = CELLP(p)->cdr)
    {
        v = rtEval(CELLP(p)->car);
        if(v == NIL)
        {
            return NIL;
        }
    }
    return v;
}

FSUB(fOr)
{
    V p;
    V v;

    (void)n;
    for(p = a[0]; IS_CONS(p); p = CELLP(p)->cdr)
    {
        v = rtEval(CELLP(p)->car);
        if(v != NIL)
        {
            return v;
        }
    }
    return NIL;
}

FSUB(fProgn)
{
    V p;
    V v;

    (void)n;
    v = NIL;
    for(p = a[0]; IS_CONS(p); p = CELLP(p)->cdr)
    {
        v = rtEval(CELLP(p)->car);
    }
    return v;
}

// Splits a PROG's body into its statements and tags, each tag with the
// statement it comes before.
static void
progScan(V body, V *stmtP, V *tagP, int *tagPosP, int *nStmtP, int *nTagP)
{
    V p;
    V s;
    int count;
    int nTags;

    count = 0;
    nTags = 0;
    for(p = body; IS_CONS(p); p = CELLP(p)->cdr)
    {
        s = CELLP(p)->car;
        if(IS_CONS(s))
        {
            stmtP[count] = s;
            count++;
        }
        else if(s != NIL)
        {
            tagP[nTags] = s;
            tagPosP[nTags] = count;
            nTags++;
        }
    }
    *nStmtP = count;
    *nTagP = nTags;
}

// An interpreted PROG. A unit's PROG comes here by (APPLY 'PROG ...), and its
// compiled form is found by its list.
FSUB(fProg)
{
    Frame frame;
    size_t mark;
    V p;
    V body;
    int count;
    int nTags;
    int pc;
    int r;
    V *stmtP;
    V *tagP;
    int *tagPosP;
    V v;
    RtFn compiledFn;

    compiledFn = lamFind(a[0]);
    if(compiledFn)
    {
        return compiledFn(n, a);
    }
    mark = rtBindMark();
    for(p = rtCar(a[0]); IS_CONS(p); p = CELLP(p)->cdr)
    {
        rtBind(SYMP(CELLP(p)->car), NIL);
    }
    body = rtCdr(a[0]);
    count = 0;
    for(p = body; IS_CONS(p); p = CELLP(p)->cdr)
    {
        count++;
    }
    stmtP = alloca(((size_t)count + 1) * sizeof(V));
    tagP = alloca(((size_t)count + 1) * sizeof(V));
    tagPosP = alloca(((size_t)count + 1) * sizeof(int));
    progScan(body, stmtP, tagP, tagPosP, &count, &nTags);
    rtProgEnter(&frame, tagP, nTags);
    r = setjmp(frame.jump);
    if(r == 1)
    {
        v = frame.value;
        rtFramePop(&frame);
        rtUnbind(mark);
        return v;
    }
    pc = (r >= 2) ? tagPosP[r - 2] : 0;
    for(; pc < count; pc++)
    {
        rtEval(stmtP[pc]);
    }
    rtFramePop(&frame);
    rtUnbind(mark);
    return NIL;
}

FSUB(fGo)
{
    V tag;

    (void)n;
    tag = rtCar(a[0]);
    if(IS_CONS(tag))
    {
        tag = rtEval(tag);
    }
    rtGo(tag);
}

static void
defineFunction(V name, V kind, V rest)
{
    V lam;

    lam = rtCons(SYMV(symLambda), rest);
    rtPutprop(name, lam, kind);
}

FSUB(fDefun)
{
    V name;
    V rest;
    V kind;

    (void)n;
    name = rtCar(a[0]);
    rest = rtCdr(a[0]);
    kind = SYMV(rtIntern("EXPR"));
    if(IS_SYM(rtCar(rest)) && funKind(rtCar(rest)))
    {
        kind = rtCar(rest);
        rest = rtCdr(rest);
    }
    else if(funKind(name))
    {
        kind = name;
        name = rtCar(rest);
        rest = rtCdr(rest);
    }
    defineFunction(name, kind, rest);
    return name;
}

FSUB(fDefprop)
{
    V p;

    (void)n;
    p = a[0];
    rtPutprop(rtCar(p), rtCar(rtCdr(p)), rtCar(rtCdr(rtCdr(p))));
    return rtCar(p);
}

// DO appears once in the Lisp, in a debugging function the program never calls.
FSUB(fDo)
{
    (void)n;
    (void)a;
    rtError("DO IS NOT RUN BY THE PROGRAM", NIL);
}

FSUB(fErrset)
{
    Frame frame;
    V v;

    (void)n;
    rtFrameEnter(&frame, FRAME_ERRSET);
    if(setjmp(frame.jump))
    {
        v = frame.value;
        rtFramePop(&frame);
        return v;
    }
    v = rtEval(rtCar(a[0]));
    rtFramePop(&frame);
    return rtCons(v, NIL);
}

FSUB(fCatch)
{
    Frame frame;
    V v;

    (void)n;
    rtFrameEnter(&frame, FRAME_CATCH);
    frame.tag = rtCar(rtCdr(a[0]));
    if(setjmp(frame.jump))
    {
        v = frame.value;
        rtFramePop(&frame);
        return v;
    }
    v = rtEval(rtCar(a[0]));
    rtFramePop(&frame);
    return v;
}

void
rtThrow(V value, V tag)
{
    Frame *fP;

    for(fP = rtFrameP; fP; fP = fP->prevP)
    {
        if((fP->kind == FRAME_CATCH)
           && ((fP->tag == NIL) || (tag == NIL) || (fP->tag == tag)))
        {
            fP->value = value;
            unwindTo(fP, 1);
        }
        if(fP->kind == FRAME_TOP)
        {
            rtAbort("THROW WITH NO CATCH");
        }
    }
    abort();
}

FSUB(fThrow)
{
    V v;

    (void)n;
    v = rtEval(rtCar(a[0]));
    rtThrow(v, rtCar(rtCdr(a[0])));
}

// A compiled ERRSET: the form's value in a list, or what stopped it.
V
rtErrsetCall(V (*fn)(void))
{
    Frame frame;
    V v;

    rtFrameEnter(&frame, FRAME_ERRSET);
    if(setjmp(frame.jump))
    {
        v = frame.value;
        rtFramePop(&frame);
        return v;
    }
    v = fn();
    rtFramePop(&frame);
    return rtCons(v, NIL);
}

// A compiled CATCH.
V
rtCatchCall(V (*fn)(void), V tag)
{
    Frame frame;
    V v;

    rtFrameEnter(&frame, FRAME_CATCH);
    frame.tag = tag;
    if(setjmp(frame.jump))
    {
        v = frame.value;
        rtFramePop(&frame);
        return v;
    }
    v = fn();
    rtFramePop(&frame);
    return v;
}

FSUB(fIgnore)
{
    (void)n;
    (void)a;
    return NIL;
}

FSUB(fStatus)
{
    (void)n;
    if(rtCar(a[0]) == SYMV(rtIntern("GCTIME")))
    {
        return FIX(0);
    }
    return NIL;
}

FSUB(fIoc)
{
    (void)n;
    if(rtCar(a[0]) == SYMV(rtIntern("G")))
    {
        unwindTo(topFrame(), STOP_QUIT);
    }
    return NIL;
}

static const struct SubrName
{
    const char *nameP;
    RtFn fn;
} subrNames[] =
{
    {"CAR", sCar}, {"CDR", sCdr},
    {"CAAR", sCaar}, {"CADR", sCadr}, {"CDAR", sCdar}, {"CDDR", sCddr},
    {"CAAAR", sCaaar}, {"CAADR", sCaadr}, {"CADAR", sCadar}, {"CADDR", sCaddr},
    {"CDAAR", sCdaar}, {"CDADR", sCdadr}, {"CDDAR", sCddar}, {"CDDDR", sCdddr},
    {"CAAAAR", sCaaaar}, {"CAAADR", sCaaadr}, {"CAADAR", sCaadar}, {"CAADDR", sCaaddr},
    {"CADAAR", sCadaar}, {"CADADR", sCadadr}, {"CADDAR", sCaddar}, {"CADDDR", sCadddr},
    {"CDAAAR", sCdaaar}, {"CDAADR", sCdaadr}, {"CDADAR", sCdadar}, {"CDADDR", sCdaddr},
    {"CDDAAR", sCddaar}, {"CDDADR", sCddadr}, {"CDDDAR", sCdddar}, {"CDDDDR", sCddddr},
    {"CONS", sCons}, {"LIST", sList}, {"EQ", sEq}, {"EQUAL", sEqual}, {"ATOM", sAtom},
    {"NULL", sNull}, {"NOT", sNull}, {"LISTP", sListp}, {"NUMBERP", sNumberp},
    {"FIXP", sFixp}, {"SYMBOLP", sSymbolp}, {"ZEROP", sZerop}, {"MINUSP", sMinusp},
    {"ADD1", sAdd1}, {"1+", sAdd1}, {"SUB1", sSub1}, {"1-", sSub1},
    {"PLUS", sPlus}, {"+", sPlus}, {"DIFFERENCE", sDifference}, {"-", sDifference},
    {"*DIF", sDifference}, {"MINUS", sMinus}, {"TIMES", sTimes}, {"*", sTimes},
    {"QUOTIENT", sQuotient}, {"REMAINDER", sRemainder}, {"MAX", sMax}, {"MIN", sMin},
    {"GREATERP", sGreaterp}, {">", sGreaterp}, {"LESSP", sLessp}, {"<", sLessp},
    {"=", sNumEq}, {"BOOLE", sZero}, {"MEMQ", sMemq}, {"MEMBER", sMember},
    {"ASSQ", sAssq}, {"ASSOC", sAssoc}, {"SASSOC", sSassoc}, {"APPEND", sAppend},
    {"NCONC", sNconc}, {"REVERSE", sReverse}, {"NREVERSE", sNreverse}, {"LAST", sLast},
    {"LENGTH", sLength}, {"RPLACA", sRplaca}, {"RPLACD", sRplacd}, {"SUBST", sSubst},
    {"DELQ", sDelq}, {"DELETE", sDelete}, {"GET", sGet}, {"PUTPROP", sPutprop},
    {"REMPROP", sRemprop}, {"PLIST", sPlist}, {"SET", sSet}, {"SYMEVAL", sSymeval},
    {"BOUNDP", sBoundp}, {"EVAL", sEval}, {"APPLY", sApply}, {"FUNCALL", sFuncall},
    {"PROG2", sProg2}, {"MAPC", sMapc}, {"MAP", sMap}, {"MAPCAR", sMapcar},
    {"MAPLIST", sMaplist}, {"MAPCAN", sMapcan}, {"RETURN", sReturn}, {"ERR", sErr},
    {"ASCII", sAscii}, {"EXPLODE", sExplode}, {"EXPLODEC", sExplodec},
    {"FLATSIZE", sFlatsize}, {"FLATC", sFlatc}, {"MAKNAM", sMaknam}, {"INTERN", sIntern},
    {"WORDNAME", sWordname}, {"DIGITWORD", sDigitword}, {"ONEWORD", sOneword},
    {"SMRECORD", sSmrecord}, {"PLANDONE", sPlandone}, {"SORTFEATURES", sSortfeatures},
    {"READLIST", sReadlist}, {"GENSYM", sGensym}, {"REMOB", sNothing}, {"ARG", sArg},
    {"PRINC", sPrinc}, {"PRIN1", sPrin1}, {"PRINT", sPrint}, {"TERPRI", sTerpri},
    {"TYO", sTyo}, {"SPRINT", sPrin1}, {"RUNTIME", sRuntime}, {"CURSORPOS", sNothing},
    {"SUSPEND", sNothing}, {"UFILE", sNothing}, {"VALRET", sNothing}, {"HELP", sNothing},
    {"SHSTPO", sNothing}, {"RANDOM", sRandom}, {"READ", sRead}, {"MOVETO", sMoveto},
    {"GRASP", sGrasp}, {"UNGRASP", sUngrasp}, {"BLINK", sBlink}, {"ERTEX", sErtex},
    {"PARSE-STATISTICS", sParseStatistics}, {"MAKOBLIST", sNothing},
    {"TYI", sTyi}, {"TYIPEEK", sTyipeek}, {"READCH", sReadch},
    {"QUOTE", fQuote}, {"FUNCTION", fQuote}, {"SETQ", fSetq}, {"COND", fCond},
    {"AND", fAnd}, {"OR", fOr}, {"PROGN", fProgn}, {"PROG", fProg}, {"GO", fGo},
    {"DEFUN", fDefun}, {"DEFPROP", fDefprop}, {"DO", fDo}, {"ERRSET", fErrset},
    {"CATCH", fCatch}, {"THROW", fThrow}, {"DECLARE", fIgnore}, {"COMMENT", fIgnore},
    {"LABELTRACE", fIgnore}, {"SSTATUS", fIgnore}, {"STATUS", fStatus}, {"IOC", fIoc},
    {NULL, NULL}
};

// Gives each primitive's symbol its C function; the image names the kinds.
static void
installSubrs(void)
{
    int i;
    Symbol *symP;

    for(i = 0; subrNames[i].nameP; i++)
    {
        symP = rtIntern(subrNames[i].nameP);
        if((symP->kind == KIND_SUBR) || (symP->kind == KIND_FSUBR))
        {
            symP->fn = subrNames[i].fn;
        }
    }
    for(i = 0; i < imgNSym; i++)
    {
        symP = &imgSyms[i];
        if(((symP->kind == KIND_SUBR) || (symP->kind == KIND_FSUBR)) && !symP->fn)
        {
            fprintf(stderr, "no primitive for %s\n", symP->nameP);
            exit(1);
        }
    }
    if(!symParse->fn)
    {
        symParse->fn = parseInterpreted;
    }
}
