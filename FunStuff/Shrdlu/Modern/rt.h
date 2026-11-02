// The Lisp run-time SHRDLU's generated C runs on: values, list cells, symbols,
// dynamic binding and non-local exits. The rules are MacLisp's, the ones the
// original was written for: every variable is dynamically bound, a GO or a
// RETURN may leave the functions it is called from, and functions are
// properties of symbols. rt.c holds the code; code.c and image.c, generated,
// hold the program. All of it runs on the program's one thread.

#ifndef RT_H
#define RT_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

// A value is a pointer to a cell or a symbol, a small number or a boxed float,
// told apart by the low three bits. NIL is 0, so a C test of a value is a Lisp
// test of it.
typedef uintptr_t V;

#define NIL ((V)0)
#define TAGMASK ((V)7)
#define TAGCONS ((V)0)
#define TAGSYM ((V)1)
#define TAGFIX ((V)2)
#define TAGFLT ((V)3)
// The value cell of a symbol that has none, never a pointer or a number.
#define UNBOUNDV ((V)4)

#define FIX(n) ((((V)(intptr_t)(n)) << 3) | TAGFIX)
#define FIXV(x) (((intptr_t)(x)) >> 3)
#define IS_CONS(x) ((((x) & TAGMASK) == TAGCONS) && ((x) != NIL))
#define IS_SYM(x) (((x) & TAGMASK) == TAGSYM)
#define IS_FIX(x) (((x) & TAGMASK) == TAGFIX)
#define IS_FLT(x) (((x) & TAGMASK) == TAGFLT)

typedef struct Cell
{
    V car;
    V cdr;
} Cell;

// What a symbol's function is.
enum
{
    KIND_NONE = 0,
    KIND_SUBR,
    KIND_FSUBR,
    KIND_EXPR,
    KIND_FEXPR,
    KIND_MACRO
};

// Every function, of whatever kind, takes its arguments this way; an FEXPR or
// an FSUBR takes one, the list of its forms.
typedef V (*RtFn)(int n, V *argsP);

typedef struct Symbol
{
    V value;
    V plist;
    V def;                  // the EXPR, FEXPR or MACRO's list
    RtFn fn;                // compiled or primitive; NULL to interpret def
    const char *nameP;
    uint8_t kind;
    uint8_t mark;
    uint8_t interned;
} Symbol;

#define CELLP(x) ((Cell *)(x))
#define SYMP(x) ((Symbol *)((x) - TAGSYM))
#define SYMV(p) (((V)(p)) + TAGSYM)

typedef struct Binding
{
    Symbol *symP;
    V old;
} Binding;

// What catches a non-local exit.
enum
{
    FRAME_PROG = 1,
    FRAME_ERRSET,
    FRAME_CATCH,
    FRAME_TOP
};

// How a sentence ended, as a FRAME_TOP's setjmp returns it.
enum
{
    STOP_NONE = 0,
    STOP_ABORT,         // ERTERR, GLOBAL-ERR, an error nothing caught
    STOP_QUIT,          // (IOC G), after which what it said is all
    STOP_INPUT          // the Lisp reader ran off the typed text
};

typedef struct Frame
{
    struct Frame *prevP;
    jmp_buf jump;
    int kind;
    int nTags;
    const V *tagsP;     // a PROG's tags; setjmp returns a tag's index plus 2
    V tag;              // a CATCH's tag
    V value;            // what the exit carries
    size_t bindTop;
    int lexN;
    V *lexArgsP;
} Frame;

extern Binding *rtBindP;
extern Frame *rtFrameP;
extern int rtLexN;
extern V *rtLexArgsP;

extern Symbol imgSyms[];
extern V K[];

// The list cells' free list; empty, rtConsSlow refills it.
extern Cell *rtFreeP;

V rtConsSlow(V a, V d);
V rtCarAtom(V x);
V rtCdrAtom(V x);
V rtUnboundVar(Symbol *symP);

// The primitives the generated code calls by name.
V rtEval(V x);
V rtApply(V fn, int n, V *argsP);
V rtApplyList(V fn, V list);
V rtCallVar(Symbol *symP, int n, V *argsP);
V rtLam(V lam, int n, V *argsP);
V rtList(int n, V *argsP);
V rtEqual(V a, V b);
V rtGet(V sym, V ind);
V rtPutprop(V sym, V val, V ind);
V rtMemq(V x, V list);
V rtAssq(V x, V list);
void rtError(const char *msgP, V what) __attribute__((noreturn));
void rtGo(V tag) __attribute__((noreturn));
void rtReturn(V value) __attribute__((noreturn));
void rtAbort(const char *msgP) __attribute__((noreturn));
void rtStopInput(void) __attribute__((noreturn));
void rtFrameEnter(Frame *frameP, int kind);
void rtProgEnter(Frame *frameP, const V *tagsP, int nTags);
void rtUnbind(size_t top);
void rtCountParse(void);
V rtErrsetCall(V (*fn)(void));
V rtCatchCall(V (*fn)(void), V tag);
void rtThrow(V value, V tag) __attribute__((noreturn));
void rtCheckCallable(Symbol *symP);
V rtArith(int op, V a, V b);
void rtSentenceStart(void);

// Set up from the image, with the program's initial state.
void rtInit(void *stackBaseP);
// A value that lives in the host's C data, seen by the collector.
void rtAddRoot(V *rootP);
Symbol *rtIntern(const char *nameP);
// The Lisp world back as it started.
void rtReload(void);
void rtEachSymbol(void (*fn)(Symbol *symP));
// A number's value.
double rtNumber(V x);
size_t rtCellsLive(void);
size_t rtCollections(void);

// A value's printed form in malloc'd space: PRIN1's when slashify is 1, PRINC's when
// 0, and when 2 PRINC's but with the numbers' points.
char *rtToString(V x, int slashify);
// Text to the terminal, the column CHRCT reports kept.
void rtOut(const char *textP, size_t n);
// The column output has reached.
int rtOutColumn(void);
// The terminal is at a line's start, by way of text that did not pass through here.
void rtOutLineStart(void);

// The generated image's tables (image.c).
extern const int imgNSym;
extern const int imgNCell;
extern const int imgNK;
extern const int imgNFn;
extern const int imgNLam;
extern const int imgNFloat;
extern const char *const imgSymName[];
extern const uint8_t imgSymKind[];
extern const uint8_t imgSymInterned[];
extern const int64_t imgSymValue[];
extern const int64_t imgSymPlist[];
extern const int64_t imgSymDef[];
extern const int64_t imgCell[];
extern const int64_t imgK[];
extern const double imgFloat[];

typedef struct ImgFn
{
    int sym;
    RtFn fn;
} ImgFn;

// A quoted LAMBDA, or a unit's PROG list, compiled. Holds K's index and the function.
typedef struct ImgLam
{
    int k;
    RtFn fn;
} ImgLam;

extern const ImgFn imgFn[];
extern const ImgLam imgLam[];

// Host hooks, in the program that links this.
void hostOut(const char *textP, size_t n);
V hostRead(void);
int hostTyi(int peek);
void hostDisplay(int which, int n, V *argsP);
V hostRandom(void);
void hostBuilt(V structure);
void hostPlanDone(void);
// The time in the host a sentence's clock should not count, so far.
long hostIdleMicros(void);

static inline V
rtCons(V a, V d)
{
    Cell *cP;

    cP = rtFreeP;
    if(cP)
    {
        rtFreeP = (Cell *)cP->cdr;
        cP->car = a;
        cP->cdr = d;
        return (V)cP;
    }
    return rtConsSlow(a, d);
}

static inline V
rtCar(V x)
{
    if(IS_CONS(x))
    {
        return CELLP(x)->car;
    }
    return (x == NIL) ? NIL : rtCarAtom(x);
}

static inline V
rtCdr(V x)
{
    if(IS_CONS(x))
    {
        return CELLP(x)->cdr;
    }
    return (x == NIL) ? NIL : rtCdrAtom(x);
}

static inline void
rtBind(Symbol *symP, V value)
{
    rtBindP->symP = symP;
    rtBindP->old = symP->value;
    rtBindP++;
    symP->value = value;
}

// Undoes the last k bindings, a function's on its way out.
static inline void
rtUnbindN(int k)
{
    while(k > 0)
    {
        k--;
        rtBindP--;
        rtBindP->symP->value = rtBindP->old;
    }
}

// An FSUBR called with its forms.
static inline V
rtFsub(Symbol *symP, V forms)
{
    return symP->fn(1, &forms);
}

// Arithmetic on small numbers here, anything else in rtArith.
enum
{
    ARITH_ADD,
    ARITH_SUB,
    ARITH_GREATER,
    ARITH_LESS
};

#define BOTH_FIX(a, b) (IS_FIX(a) && IS_FIX(b))

static inline V
rtAdd1(V a)
{
    return IS_FIX(a) ? (a + ((V)1 << 3)) : rtArith(ARITH_ADD, a, FIX(1));
}

static inline V
rtSub1(V a)
{
    return IS_FIX(a) ? (a - ((V)1 << 3)) : rtArith(ARITH_SUB, a, FIX(1));
}

static inline V
rtPlus2(V a, V b)
{
    return BOTH_FIX(a, b) ? ((a + b) - TAGFIX) : rtArith(ARITH_ADD, a, b);
}

static inline V
rtDiff2(V a, V b)
{
    return BOTH_FIX(a, b) ? ((a - b) + TAGFIX) : rtArith(ARITH_SUB, a, b);
}

static inline int
rtGreater2(V a, V b)
{
    return BOTH_FIX(a, b) ? (((intptr_t)a) > ((intptr_t)b))
                          : (rtArith(ARITH_GREATER, a, b) != NIL);
}

static inline int
rtLess2(V a, V b)
{
    return BOTH_FIX(a, b) ? (((intptr_t)a) < ((intptr_t)b))
                          : (rtArith(ARITH_LESS, a, b) != NIL);
}

extern Binding *rtBindBaseP;

static inline size_t
rtBindMark(void)
{
    return (size_t)(rtBindP - rtBindBaseP);
}

static inline void
rtFramePop(Frame *frameP)
{
    rtFrameP = frameP->prevP;
}

// A variable's value, which the checks of an interpreter keep.
static inline V
rtVal(Symbol *symP)
{
    V v;

    v = symP->value;
    if(v == UNBOUNDV)
    {
        return rtUnboundVar(symP);
    }
    return v;
}

#endif
