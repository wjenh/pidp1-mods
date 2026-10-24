// mactoam1.c - convert a macro1 source to am1 source.
//
// Usage: mactoam1 [-o outfile] [-t tabwidth] file.mac
//
// The compiled binaries will be word-eqivalent other than am1's tape leader.
//
// M1engine.c runs macro1'same two passes over the source and determines every term, operator,
// expression, tag, definition and pseudo-instruction along with its source position.
// This then builds an expression tree from those reports, writes it in am1 syntax, and verifies it.
// The am1 text is evaluated with am1's rules including textual expansion of the #defines
// and macro/ parameters it writes and compared with macro1's eqivalents.
// The implied addition macro1 does for 'a b' instead of am1's default 'a or b' is handled.
// Where an expression would resolve differently, it is written with + for the spaces, resolved as a
// number, or an error given if it can't be resolved.
// The source is then written out with comments kept and tabs expanded to 8 spaces.
//
// A construct that cannot be converted is written as an am1 #error line and
// reports it on stderr, the exit status is then 1.
//
// 24-Oct-2026 Claude original rewritten, the flex scanner removed.
// 24-Oct-2026 wje cleanup for release.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m1engine.h"

#define MASK18          0777777
#define MAXLEVEL        3       // 0 as written, 1 every space a +, 2 a number, 3 refused
#define MAXPASS3        12
#define MAXDEPTH        40      // the deepest define or parameter expansion evaluated

// Expression tree node types.
enum
{
    NT_TERM, NT_UNARY, NT_BIN, NT_LIT, NT_GROUP
};

// am1 token types.
enum
{
    K_ATOM, K_OP, K_NEG, K_LP, K_RP, K_LB, K_RB
};

// What an atom's am1 value comes from.
enum
{
    A_VALUE,        // a fixed value
    A_DOT,          // . where the word is assembled
    A_DEFINE,       // a #define, expanded where it is used
    A_DUMMY,        // a macro parameter, expanded where it is used
    A_R,            // R, the local tag at an expansion's first word
    A_TAG           // a local tag of the expansion; val is macro1's offset from R
};

// Item types: the statements of a source line, in the order they stand.
enum
{
    IT_EXPR, IT_TAG, IT_NUMTAG, IT_ASSIGN, IT_INVOKE, IT_EMPTYINV, IT_DEFINE, IT_CONSTANTS,
    IT_VARIABLES, IT_TEXT, IT_REPEAT, IT_START, IT_COMMENT, IT_PSEUDO
};

// Line roles.
enum
{
    LR_CODE, LR_TITLE, LR_DEFHEAD, LR_DEFNAME, LR_BODY, LR_TERM, LR_TEXTCONT
};

// How a piece of output sits on a line.
enum
{
    CL_NONE, CL_TAG, CL_STMT, CL_COMMENT, CL_OWNLINE
};

typedef long long Key;

typedef struct Node
{
    int type;
    int kind;               // M1T_* for a term
    int op;
    int pos;
    int end;
    int val;                // macro1's value: of the term, the literal's address, the group
    int info;
    struct Node *aP;        // the left operand, or the only one
    struct Node *bP;
} Node;

typedef struct
{
    int type;
    int op;
    int akind;
    int val;                // A_VALUE, A_TAG: the value; A_DUMMY: the parameter number;
                            // K_LB: macro1's address of the literal
    int m1val;              // macro1's value of what the atom stands for; K_LB: the value inside
    Key key;                // A_DEFINE: the name
    const char *textP;
} Tok;

typedef struct
{
    Tok *vP;
    int n;
    int cap;
} TokList;

typedef struct
{
    char *bufP;
    int len;
    int cap;
} Str;

typedef struct
{
    Key *keysP;
    int *valsP;
    int cap;
    int n;
} Map;

typedef struct
{
    int kind;
    int pos;
    int end;
    int body;               // the macro whose body holds it, or -1
    Node *treeP;            // its structure, from the first evaluation
    int level;
    int escalatedIn;        // the pass 3 round that last raised its level
    int nEval;
    int val;
    int varies;
    int radix;
    char *textP;            // the am1 text, from the latest round
    int textVaries;
    char *varyP;            // why textVaries: the two writings
    char *whyP;
    int forcedSpace;        // the position of a space that must stay one (after law), or -1
    int broken;             // macro1 rejects it: written as it stands
    int preRadix;           // decimal or octal as its first term: M1P_ code + 1, or 0
    int postRadix;          // the same as its last term
    Key assignKey;          // M1E_ASSIGN: the name being given the value
} Site;

typedef struct
{
    int type;
    int pos;
    int end;
    int site;
    int a;
    int b;
    int c;
    int d;
    int *listP;
    int nlist;
    char *whyP;
} Item;

typedef struct
{
    char name[M1SYMLEN];
    Key key;
    int nargs;
    char args[M1MAXARGS][M1SYMLEN];
    char *paramP[M1MAXARGS + 1];
    int defLine;
    int nameLine;
    int termLine;
    int usesR;
    int invoked;
    Key *tagsP;
    int ntags;
    int defined;
} Macro;

typedef struct Frame
{
    int macIdx;
    int R;
    int given;
    int argSite[M1MAXARGS + 1];
    TokList argToks[M1MAXARGS + 1];
    int factor[M1MAXARGS + 1];
    char *bindTextP[M1MAXARGS + 1];
    struct Frame *parentP;
    int phantom;
    int serial;             // which expansion: its local tags are new symbols to am1
} Frame;

typedef struct
{
    Key key;
    int site;
    TokList toks;
    int numeric;
    int isTag;
    int val;
} DefRec;

typedef struct
{
    Key key;
    int addr;
    int size;
} Var;

typedef struct
{
    int line;
    int error;
    char *msgP;
} Note;

// The source and the options.
static const char *srcNameP;
static const char *outPathP;
static int tabWidth = 8;
static int *lineRoleP;
static int *lineMacP;
static int *lineTextColP;
static int **lineItemsP;
static int *lineNItemsP;

// What the passes found.
static Node *buildCur[64];
static Node **buildSlot[64];
static int buildOp[64];
static int buildOpPos[64];
static int buildDepth;
static int buildDirty;

static Site *sitesP;
static int nSites;
static int capSites;
static Map siteMap;

static Item *itemsP;
static int nItems;
static int capItems;
static Map itemMap;

static Macro *macrosP;
static int nMacros;

static Frame *frameTopP;
static Frame *pendingP;

static DefRec *defsP;
static int nDefs;
static int capDefs;
static Map defMap;          // name key: the current DefRec

static Var *varsP;
static int nVars;
static int capVars;

static Map plusOps;         // positions of spaces written as +
static Map spellMap;        // name key: index into spellings
static char **spellingsP;
static int nSpellings;
static int capSpellings;
static Map classMap;        // name key: what kinds of thing the name has been (bits)
static Map bodyTagMap;      // name key: the macro whose body tags it, + 1
static Map outNameMap;      // name key: index into outNames
static char **outNamesP;
static int nOutNames;
static Map usedOutNames;

static Note *notesP;
static int nNotes;
static int capNotes;
static int nErrors;

static int curPass;
static int round3;
static int changed;
static int lastAssignSite = -1;
static int lastStartSite = -1;
static int lastRepeatSite = -1;
static int sawDecimal;      // the source uses decimal somewhere
static int *litPosP;        // pass 2's literals, in the order met
static int nLitPos;
static int litMark;         // how many of them the last constants statement placed
static Map orphanMap;       // positions of literals macro1 never places

// am1 pools two constants in one word only when their values are known as they are read; one
// that names a tag not yet defined gets a word of its own (Tools/AM1/eval.c hashExpr()).
// Where that would split a word macro1 shares, the constant is written as its value.
typedef struct
{
    int pool;
    int val;
    unsigned long long key;     // 0 when am1 knows the value as it reads it
    int pos;
    int culprit;                // the #define or argument to write as a number, or -1
} LitUse;

static LitUse *litUsesP;
static int nLitUses;
static int capLitUses;
static int poolNo;
static Map seenMap;         // name key: tags defined so far in this round of pass 3
static Map bodyTagPos;      // name key and macro: where a body's tag is defined
static Map litNumeric;      // positions of constants written as their values
static Map fwdEquMap;       // name key: an = a define body uses before its #define
static int expansionSerial;
static int needXX;
static int needSkip;
static int needClc;

// The name classes of classMap.
#define NC_LABEL    001
#define NC_EQU      002
#define NC_VAR      004
#define NC_MACRO    010
#define NC_PERM     020
#define NC_UNDEF    040
#define NC_BODYTAG  0100
#define NC_DUMMY    0200
#define NC_EQUAM1   0400    // every = gives it am1's own value for that word
#define NC_EQUOTHER 01000   // an = gives it another value

static const char *am1Keywords[] =
{
    "loc", "local", "addloc", "addlocal", "priv", "private", "endloc", "endlocal", "var",
    "table", "bank", "thisbank", "ascii", "text", "type340", "flexo", "char", "decimal",
    "octal", "start", "stop", "constants", "variables", "import", "export", NULL
};

typedef struct
{
    const char *nameP;
    int val;
} PermSym;

// am1's permanent symbols (Tools/AM1/permsyms.def).
static const PermSym am1Perms[] =
{
    { "1s", 01 }, { "2s", 03 }, { "3s", 07 }, { "4s", 017 }, { "5s", 037 }, { "6s", 077 },
    { "7s", 0177 }, { "8s", 0377 }, { "9s", 0777 }, { "i", 010000 }, { "C", 004000 },
    { "and", 0020000 }, { "ior", 0040000 }, { "xor", 0060000 }, { "xct", 0100000 },
    { "cal", 0160000 }, { "jda", 0170000 }, { "lac", 0200000 }, { "lio", 0220000 },
    { "dac", 0240000 }, { "dap", 0260000 }, { "dip", 0300000 }, { "dio", 0320000 },
    { "dzm", 0340000 }, { "add", 0400000 }, { "sub", 0420000 }, { "idx", 0440000 },
    { "isp", 0460000 }, { "sad", 0500000 }, { "sas", 0520000 }, { "mus", 0540000 },
    { "dis", 0560000 }, { "mul", 0540000 }, { "div", 0560000 }, { "jmp", 0600000 },
    { "jsp", 0620000 }, { "law", 0700000 }, { "skp", 0640000 }, { "spi", 0642000 },
    { "szo", 0641000 }, { "sma", 0640400 }, { "spa", 0640200 }, { "sza", 0640100 },
    { "szf", 0640000 }, { "szs", 0640000 }, { "ral", 0661000 }, { "rar", 0671000 },
    { "ril", 0662000 }, { "rir", 0672000 }, { "rcl", 0663000 }, { "rcr", 0673000 },
    { "sal", 0665000 }, { "sar", 0675000 }, { "sil", 0666000 }, { "sir", 0676000 },
    { "scl", 0667000 }, { "scr", 0677000 }, { "nop", 0760000 }, { "opr", 0760000 },
    { "cli", 0764000 }, { "lat", 0762200 }, { "cma", 0761000 }, { "hlt", 0760400 },
    { "cla", 0760200 }, { "lap", 0760300 }, { "clf", 0760000 }, { "stf", 0760010 },
    { "lia", 0760020 }, { "lai", 0760040 }, { "lsw", 0760060 }, { "swp", 0760060 },
    { "cmi", 0770000 }, { "sni", 0644000 }, { "szi", 0654000 }, { "iif", 0744000 },
    { "ifi", 0742000 }, { "ida", 0740400 }, { "sci", 0740100 }, { "scf", 0740040 },
    { "iot", 0720000 }, { "ioh", 0730000 }, { "tyi", 0720004 }, { "rrb", 0720030 },
    { "cks", 0720033 }, { "rpa", 0730001 }, { "rpb", 0730002 }, { "tyo", 0730003 },
    { "ppa", 0730005 }, { "ppb", 0730006 }, { "dpy", 0730007 }, { "lem", 0720074 },
    { "eem", 0724074 }, { "esm", 0720055 }, { "lsm", 0720054 }, { "cbs", 0720056 },
    { "dsc", 0720050 }, { "asc", 0720051 }, { "isb", 0720052 }, { "cac", 0720053 },
    { "sdb", 0722007 }, { "dpyc", 0724007 }, { NULL, 0 }
};

// am1's ASCII to concise code (Tools/UtilLibs/flexoConversion.c): 0100 unshifted, 0200
// shifted, neither for either case, -1 none.
static const int am1Concise[128] =
{
    -1, -1, -1, -1, -1, -1, -1, -1, 0075, 0036, -1, -1, -1, 0077, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0000, 0205, 0201, 0204, -1, -1, 0206, 0202, 0157, 0155, 0273, 0254, 0133, 0154, 0173, 0121,
    0120, 0101, 0102, 0103, 0104, 0105, 0106, 0107, 0110, 0111, -1, -1, 0207, 0233, 0210, 0221,
    0140, 0261, 0262, 0263, 0264, 0265, 0266, 0267, 0270, 0271, 0241, 0242, 0243, 0244, 0245, 0246,
    0247, 0250, 0251, 0222, 0223, 0224, 0225, 0226, 0227, 0230, 0231, 0257, 0220, 0255, 0211, 0240,
    0156, 0161, 0162, 0163, 0164, 0165, 0166, 0167, 0170, 0171, 0141, 0142, 0143, 0144, 0145, 0146,
    0147, 0150, 0151, 0122, 0123, 0124, 0125, 0126, 0127, 0130, 0131, -1, 0256, -1, 0203, -1
};

static void render(Node *nP, TokList *outP, Site *sP, int *badP, char **whyP);
static int am1EvalToks(TokList *tP, Frame *frameP, int depth, int owner, int *blameP);
static const char *outName(Key key);

// Allocate memory, or exit.
static void *
xmalloc(size_t n)
{
void *p;

    p = calloc(1, (n > 0) ? n : 1);
    if( !p )
    {
        fprintf(stderr, "mactoam1: out of memory\n");
        exit(1);
    }
    return( p );
}

// Grow an allocation, or exit.
static void *
xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if( !p )
    {
        fprintf(stderr, "mactoam1: out of memory\n");
        exit(1);
    }
    return( p );
}

// A copy of a string.
static char *
xstrdup(const char *sP)
{
char *p;

    p = xmalloc(strlen(sP) + 1);
    strcpy(p, sP);
    return( p );
}

// Append to a growing string.
static void
sAddn(Str *sP, const char *tP, int n)
{
    if( (sP->len + n + 1) > sP->cap )
    {
        sP->cap = (2 * sP->cap) + n + 64;
        sP->bufP = xrealloc(sP->bufP, sP->cap);
    }
    memcpy(sP->bufP + sP->len, tP, n);
    sP->len += n;
    sP->bufP[sP->len] = '\0';
}

static void
sAdd(Str *sP, const char *tP)
{
    sAddn(sP, tP, (int)strlen(tP));
}

static void
sAddc(Str *sP, char c)
{
    sAddn(sP, &c, 1);
}

static void
sPrintf(Str *sP, const char *fmtP, ...)
{
char buf[512];
va_list ap;

    va_start(ap, fmtP);
    vsnprintf(buf, sizeof(buf), fmtP, ap);
    va_end(ap);
    sAdd(sP, buf);
}

// Empty a string without freeing it.
static void
sClear(Str *sP)
{
    sP->len = 0;
    if( sP->bufP )
    {
        sP->bufP[0] = '\0';
    }
}

// The string's text, never NULL.
static const char *
sText(Str *sP)
{
    return( sP->bufP ? sP->bufP : "" );
}

// Find key in a map.
// Returns its value, or -1 if not found.
static int
mapGet(Map *mP, Key key)
{
unsigned long long h;
int i;

    if( mP->cap == 0 )
    {
        return( -1 );
    }
    h = (unsigned long long)key * 0x9E3779B97F4A7C15ULL;
    i = (int)(h >> 40) & (mP->cap - 1);
    while( mP->keysP[i] != -1 )
    {
        if( mP->keysP[i] == key )
        {
            return( mP->valsP[i] );
        }
        i = (i + 1) & (mP->cap - 1);
    }
    return( -1 );
}

// Set key's value in a map.
static void
mapPut(Map *mP, Key key, int val)
{
unsigned long long h;
int i;
int oldCap;
Key *oldKeysP;
int *oldValsP;

    if( (2 * (mP->n + 1)) > mP->cap )
    {
        oldCap = mP->cap;
        oldKeysP = mP->keysP;
        oldValsP = mP->valsP;
        mP->cap = (oldCap == 0) ? 256 : (2 * oldCap);
        mP->keysP = xmalloc(mP->cap * sizeof(Key));
        mP->valsP = xmalloc(mP->cap * sizeof(int));
        for( i = 0; i < mP->cap; i++ )
        {
            mP->keysP[i] = -1;
        }
        mP->n = 0;
        for( i = 0; i < oldCap; i++ )
        {
            if( oldKeysP[i] != -1 )
            {
                mapPut(mP, oldKeysP[i], oldValsP[i]);
            }
        }
        free(oldKeysP);
        free(oldValsP);
    }
    h = (unsigned long long)key * 0x9E3779B97F4A7C15ULL;
    i = (int)(h >> 40) & (mP->cap - 1);
    while( (mP->keysP[i] != -1) && (mP->keysP[i] != key) )
    {
        i = (i + 1) & (mP->cap - 1);
    }
    if( mP->keysP[i] == -1 )
    {
        mP->n++;
    }
    mP->keysP[i] = key;
    mP->valsP[i] = val;
}

// Forget every key in a map.
static void
mapClear(Map *mP)
{
int i;

    for( i = 0; i < mP->cap; i++ )
    {
        mP->keysP[i] = -1;
    }
    mP->n = 0;
}

// A name's key: its first 6 characters, as macro1 compares names.
static Key
nameKey(const char *nameP)
{
Key k;
int i;

    k = 0;
    for( i = 0; (i < 6) && nameP[i]; i++ )
    {
        k |= ((Key)(unsigned char)nameP[i]) << (8 * i);
    }
    return( k | ((Key)1 << 56) );
}

// Record a note for stderr: an error (a refusal) or a warning, once each.
static void
note(int error, int line, const char *fmtP, ...)
{
char buf[512];
va_list ap;
int i;

    va_start(ap, fmtP);
    vsnprintf(buf, sizeof(buf), fmtP, ap);
    va_end(ap);
    for( i = 0; i < nNotes; i++ )
    {
        if( (notesP[i].line == line) && (notesP[i].error == error) && (strcmp(notesP[i].msgP, buf) == 0) )
        {
            return;
        }
    }
    if( nNotes == capNotes )
    {
        capNotes = (2 * capNotes) + 16;
        notesP = xrealloc(notesP, capNotes * sizeof(Note));
    }
    notesP[nNotes].line = line;
    notesP[nNotes].error = error;
    notesP[nNotes].msgP = xstrdup(buf);
    nNotes++;
    if( error )
    {
        nErrors++;
    }
}

// The text of source columns c0..c1 of a line.
static char *
lineSlice(int line, int c0, int c1)
{
const char *srcP;
int n;
char *p;

    srcP = ((line >= 0) && (line < m1LineCount)) ? m1Lines[line] : "";
    n = (int)strlen(srcP);
    if( c1 > n )
    {
        c1 = n;
    }
    if( c0 > c1 )
    {
        c0 = c1;
    }
    p = xmalloc(c1 - c0 + 1);
    memcpy(p, srcP + c0, c1 - c0);
    p[c1 - c0] = '\0';
    return( p );
}

// The source text from pos to end, which may run over lines (joined with a space).
static char *
spanText(int pos, int end)
{
Str s;
int line;
char *p;

    memset(&s, 0, sizeof(s));
    if( (pos < 0) || (end < pos) )
    {
        return( xstrdup("") );
    }
    for( line = M1LINE(pos); line <= M1LINE(end); line++ )
    {
        p = lineSlice(line, (line == M1LINE(pos)) ? M1COL(pos) : 0,
            (line == M1LINE(end)) ? M1COL(end) : M1MAXCOL);
        if( line != M1LINE(pos) )
        {
            sAddc(&s, ' ');
        }
        sAdd(&s, p);
        free(p);
    }
    return( s.bufP ? s.bufP : xstrdup("") );
}

// A symbol's spelling at pos..end with its overbars removed.
static char *
symbolSpelling(int pos, int end)
{
char *p;
char *q;
char *r;

    p = spanText(pos, end);
    for( q = r = p; *q; q++ )
    {
        if( (*q != '\\') && (*q != '~') )
        {
            *r++ = *q;
        }
    }
    *r = '\0';
    return( p );
}

// Note the spelling of the symbol at pos..end, and its class; the first spelling is the one
// written for every spelling with the same 6 characters.
static Key
noteSymbol(int pos, int end, int cls)
{
char *spP;
char k6[M1SYMLEN];
Key key;
int idx;

    spP = symbolSpelling(pos, end);
    strncpy(k6, spP, 6);
    k6[6] = '\0';
    key = nameKey(k6);
    idx = mapGet(&spellMap, key);
    if( idx < 0 )
    {
        if( nSpellings == capSpellings )
        {
            capSpellings = (2 * capSpellings) + 64;
            spellingsP = xrealloc(spellingsP, capSpellings * sizeof(char *));
        }
        spellingsP[nSpellings] = spP;
        mapPut(&spellMap, key, nSpellings++);
    }
    else
    {
        if( strcmp(spellingsP[idx], spP) != 0 )
        {
            note(0, M1LINE(pos), "%s and %s are one symbol to macro1 (6 characters); written as %s",
                spellingsP[idx], spP, spellingsP[idx]);
        }
        free(spP);
    }
    if( cls )
    {
        idx = mapGet(&classMap, key);
        mapPut(&classMap, key, ((idx < 0) ? 0 : idx) | cls);
    }
    return( key );
}

// The key of a name already spelled.
static Key
keyOfName(const char *nameP)
{
char k6[M1SYMLEN];

    strncpy(k6, nameP, 6);
    k6[6] = '\0';
    return( nameKey(k6) );
}

// The macro whose body holds a source line, or -1.
static int
bodyOf(int line)
{
    if( (line < 0) || (line >= m1LineCount) )
    {
        return( -1 );
    }
    return( (lineRoleP[line] == LR_BODY) ? lineMacP[line] : -1 );
}

// Return 1 when the 6-character key names a local tag of macro mac's body.
static int
isBodyTagOf(Key key, int mac)
{
int i;

    if( (mac < 0) || (mac >= nMacros) )
    {
        return( 0 );
    }
    for( i = 0; i < macrosP[mac].ntags; i++ )
    {
        if( macrosP[mac].tagsP[i] == key )
        {
            return( 1 );
        }
    }
    return( 0 );
}

// The site for kind at pos, made on first use.
// Returns its index.
static int
siteFor(int kind, int pos, int end)
{
Key key;
int idx;
Site *sP;

    key = ((Key)pos << 4) | kind;
    idx = mapGet(&siteMap, key);
    if( idx >= 0 )
    {
        return( idx );
    }
    if( nSites == capSites )
    {
        capSites = (2 * capSites) + 256;
        sitesP = xrealloc(sitesP, capSites * sizeof(Site));
    }
    sP = &sitesP[nSites];
    memset(sP, 0, sizeof(Site));
    sP->kind = kind;
    sP->pos = pos;
    sP->end = end;
    sP->body = bodyOf(M1LINE(pos));
    sP->escalatedIn = -1;
    sP->forcedSpace = -1;
    mapPut(&siteMap, key, nSites);
    return( nSites++ );
}

// The item of type at pos, made on first use.
// Returns its index.
static int
itemFor(int type, int pos, int end, int *newP)
{
Key key;
int idx;
Item *iP;

    key = ((Key)pos << 4) | type;
    idx = mapGet(&itemMap, key);
    *newP = 0;
    if( idx >= 0 )
    {
        return( idx );
    }
    if( nItems == capItems )
    {
        capItems = (2 * capItems) + 256;
        itemsP = xrealloc(itemsP, capItems * sizeof(Item));
    }
    iP = &itemsP[nItems];
    memset(iP, 0, sizeof(Item));
    iP->type = type;
    iP->pos = pos;
    iP->end = end;
    iP->site = -1;
    mapPut(&itemMap, key, nItems);
    *newP = 1;
    return( nItems++ );
}

// Record items only from the passes that see the source's structure.
static int
recording(int pass)
{
    return( (pass == 2) || (pass == 4) );
}

// Start building a new expression.
static void
buildReset(void)
{
    buildDepth = 0;
    buildCur[0] = NULL;
    buildSlot[0] = NULL;
    buildOp[0] = 0;
    buildDirty = 0;
}

// Add an operand to the expression being built.
static void
buildAttach(Node *nP)
{
Node *bP;
int d;

    d = buildDepth;
    if( !buildCur[d] )
    {
        buildCur[d] = nP;
        buildSlot[d] = &buildCur[d];
        return;
    }
    if( !buildOp[d] )
    {
        buildDirty = 1;
        return;
    }
    bP = xmalloc(sizeof(Node));
    bP->type = NT_BIN;
    bP->op = buildOp[d];
    bP->pos = buildOpPos[d];
    bP->aP = buildCur[d];
    bP->bP = nP;
    buildCur[d] = bP;
    buildSlot[d] = &bP->bP;
    buildOp[d] = 0;
}

// macro1's value of a tree, computed as getExpr() does.
static int
m1Eval(Node *nP)
{
int l;
int r;

    switch( nP->type )
    {
    case NT_TERM:
    case NT_LIT:
    case NT_GROUP:
        return( nP->val );
    case NT_UNARY:
        l = m1Eval(nP->aP);
        return( (nP->op == '-') ? (l ^ MASK18) : l );
    default:
        break;
    }
    l = m1Eval(nP->aP);
    r = m1Eval(nP->bP);
    switch( nP->op )
    {
    case ' ':
    case '+':
        l += r;
        break;
    case '-':
        l += r ^ MASK18;
        break;
    case '*':
        l *= r;
        break;
    case '&':
        return( l & r );
    case '!':
        return( l | r );
    }
    if( l >= 01000000 )
    {
        l = (l + 1) & MASK18;
    }
    return( l );
}

// Mark every space of a tree whose terms share bits or carry, to be written as +.
static void
noteOverlaps(Node *nP)
{
int l;
int r;

    if( !nP )
    {
        return;
    }
    if( nP->type == NT_BIN )
    {
        noteOverlaps(nP->aP);
        noteOverlaps(nP->bP);
        if( nP->op == ' ' )
        {
            l = m1Eval(nP->aP);
            r = m1Eval(nP->bP);
            if( (l & r) || ((l + r) >= 01000000) )
            {
                mapPut(&plusOps, nP->pos, 1);
            }
        }
    }
    else if( (nP->type == NT_UNARY) || (nP->type == NT_LIT) || (nP->type == NT_GROUP) )
    {
        noteOverlaps(nP->aP);
    }
}

// Return 1 when a tree holds a term of the given kind.
static int
treeHas(Node *nP, int kind)
{
    if( !nP )
    {
        return( 0 );
    }
    if( nP->type == NT_TERM )
    {
        return( nP->kind == kind );
    }
    if( (nP->type == NT_LIT) && (kind == M1T_LITERAL) )
    {
        return( 1 );
    }
    return( treeHas(nP->aP, kind) || treeHas(nP->bP, kind) );
}

// Return 1 when a tree has a space operator.
static int
treeHasSpace(Node *nP)
{
    if( !nP )
    {
        return( 0 );
    }
    if( (nP->type == NT_BIN) && (nP->op == ' ') )
    {
        return( 1 );
    }
    return( treeHasSpace(nP->aP) || treeHasSpace(nP->bP) );
}

// The leftmost term of a tree.
static Node *
leftmost(Node *nP)
{
    while( nP && (nP->type == NT_BIN) )
    {
        nP = nP->aP;
    }
    return( nP );
}

// Add a token.
static void
tokAdd(TokList *tP, int type, int op, int akind, int val, int m1val, Key key, const char *textP)
{
Tok *kP;

    if( tP->n == tP->cap )
    {
        tP->cap = (2 * tP->cap) + 16;
        tP->vP = xrealloc(tP->vP, tP->cap * sizeof(Tok));
    }
    kP = &tP->vP[tP->n++];
    kP->type = type;
    kP->op = op;
    kP->akind = akind;
    kP->val = val;
    kP->m1val = m1val;
    kP->key = key;
    kP->textP = textP;
}

// A copy of a token list.
static TokList
tokCopy(TokList *tP)
{
TokList c;

    c.n = tP->n;
    c.cap = tP->n;
    c.vP = xmalloc((tP->n + 1) * sizeof(Tok));
    memcpy(c.vP, tP->vP, tP->n * sizeof(Tok));
    return( c );
}

// The text of a token list, as written.
static char *
tokText(TokList *tP)
{
Str s;
int i;
Tok *kP;

    memset(&s, 0, sizeof(s));
    for( i = 0; i < tP->n; i++ )
    {
        kP = &tP->vP[i];
        switch( kP->type )
        {
        case K_ATOM:
            sAdd(&s, kP->textP);
            break;
        case K_OP:
            sAddc(&s, (char)kP->op);
            break;
        case K_NEG:
            sAddc(&s, '-');
            break;
        case K_LP:
            sAddc(&s, '(');
            break;
        case K_RP:
            sAddc(&s, ')');
            break;
        case K_LB:
            sAddc(&s, '[');
            break;
        case K_RB:
            sAddc(&s, ']');
            break;
        }
    }
    return( s.bufP ? s.bufP : xstrdup("") );
}

// A number as am1 text: octal with a leading 0, or 0o octal where the source uses decimal
// anywhere, since a #define is read in the radix of the place it is used.
static char *
numberText(int val, int radix)
{
char buf[32];

    val &= MASK18;
    if( val < 8 )
    {
        snprintf(buf, sizeof(buf), "%o", val);
    }
    else if( (radix == 10) || sawDecimal )
    {
        snprintf(buf, sizeof(buf), "0o%o", val);
    }
    else
    {
        snprintf(buf, sizeof(buf), "0%o", val);
    }
    return( xstrdup(buf) );
}

// am1's value of flexo applied to up to 3 characters (Tools/AM1/lexer.l), or -1 if a
// character has no code.
static int
am1Flexo(const char *cP, int nchars)
{
int i;
int fc;
int shift;
int rslt;
int ch;

    rslt = 0;
    shift = 0;
    for( i = 0; i < nchars; i++ )
    {
        ch = (unsigned char)*cP;
        fc = (ch < 128) ? am1Concise[ch] : -1;
        if( fc < 0 )
        {
            return( -1 );
        }
        if( (fc & 0200) && !shift )
        {
            shift = 1;
            fc = 074;
        }
        else if( (fc & 0100) && shift )
        {
            shift = 0;
            fc = 072;
        }
        else
        {
            fc &= 077;
            cP++;
        }
        rslt = (rslt << 6) | fc;
    }
    return( rslt );
}

// The am1 text for flexo or char at pos..end, or NULL where am1 would give another value.
static char *
charTermText(Node *nP)
{
char *srcP;
char *p;
char buf[32];
int n;
int v;
int ch;

    srcP = spanText(nP->pos, nP->end);
    p = srcP;
    while( isalpha((unsigned char)*p) )
    {
        p++;
    }
    while( (*p == ' ') || (*p == '\t') )
    {
        p++;
    }
    if( nP->kind == M1T_FLEX )
    {
        // am1 reads exactly the characters written, up to 3, and its preprocessor folds runs
        // of spaces, so a shorter run cannot be padded: only 3 characters are written as such.
        n = (int)strlen(p);
        if( (n != 3) || strchr(p, ' ') || strchr(p, '\t') )
        {
            free(srcP);
            return( NULL );
        }
        snprintf(buf, sizeof(buf), "%s", p);
        v = am1Flexo(buf, 3);
        free(srcP);
        if( isalpha((unsigned char)buf[0]) && isalnum((unsigned char)buf[1]) && isalnum((unsigned char)buf[2]) )
        {
            // am1's preprocessor would put a #define or a define of that name in its place,
            // clc among them
            n = mapGet(&classMap, keyOfName(buf));
            if( ((n > 0) && (n & (NC_EQU | NC_MACRO))) || (strcmp(buf, "clc") == 0) )
            {
                return( NULL );
            }
        }
        if( v != nP->val )
        {
            return( NULL );
        }
        p = xmalloc(48);
        snprintf(p, 48, "flexo %s", buf);
        return( p );
    }

    // char: macro1 writes the position first, am1 the character first.
    if( strlen(p) != 2 )
    {
        free(srcP);
        return( NULL );
    }
    ch = (unsigned char)p[1];
    v = (ch < 128) ? am1Concise[ch] : -1;
    if( (v < 0) || (v & 0200) )
    {
        free(srcP);
        return( NULL );
    }
    v &= 077;
    v <<= (p[0] == 'l') ? 12 : ((p[0] == 'm') ? 6 : 0);
    if( v != nP->val )
    {
        free(srcP);
        return( NULL );
    }
    p = xmalloc(16);
    snprintf(p, 16, "char %c%c", srcP[strlen(srcP) - 1], srcP[strlen(srcP) - 2]);
    free(srcP);
    return( p );
}

// The frame a dummy at this point of an expansion is looked up in.
static Frame *
currentFrame(void)
{
    return( frameTopP );
}

// Render one term.
static void
renderTerm(Node *nP, TokList *outP, Site *sP, int *badP, char **whyP, int absTag)
{
Key key;
int idx;
char *textP;
char *spP;
Frame *fP;
DefRec *dP;
Str s;

    switch( nP->kind )
    {
    case M1T_NUMBER:
        textP = spanText(nP->pos, nP->end);
        if( sawDecimal && (sP->kind == M1E_ASSIGN) )
        {
            // a #define is read in the radix where it is used, so it names its own
            spP = textP;
            while( (spP[0] == '0') && spP[1] )
            {
                spP++;
            }
            memset(&s, 0, sizeof(s));
            sPrintf(&s, "%s%s", (sP->radix == 10) ? "0d" : "0o", spP);
            free(textP);
            textP = s.bufP;
        }
        if( nP->val > MASK18 )
        {
            *badP = 1;
            *whyP = "a number too large for am1";
        }
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val & MASK18, nP->val & MASK18, 0, textP);
        return;

    case M1T_DOT:
        fP = currentFrame();
        if( (sP->kind == M1E_ARG) && (sP->body >= 0) && fP )
        {
            // macro1 takes . in an argument where the definition is made, am1 where the parameter is used.
            // Inside a define body that place is a fixed distance from R.
            idx = (nP->val - fP->R) & MASK18;
            macrosP[sP->body].usesR = 1;
            if( pendingP && (pendingP->macIdx >= 0) && (pendingP->macIdx < nMacros) &&
                macrosP[pendingP->macIdx].usesR )
            {
                *badP = 1;
                *whyP = "a . in an argument to a define that has its own R";
            }
            if( idx == 0 )
            {
                tokAdd(outP, K_ATOM, 0, A_R, 0, nP->val, 0, "R");
                return;
            }
            tokAdd(outP, K_LP, 0, 0, 0, 0, 0, NULL);
            tokAdd(outP, K_ATOM, 0, A_R, 0, fP->R, 0, "R");
            tokAdd(outP, K_OP, '+', 0, 0, 0, 0, NULL);
            tokAdd(outP, K_ATOM, 0, A_VALUE, idx, idx, 0, numberText(idx, sP->radix));
            tokAdd(outP, K_RP, 0, 0, 0, 0, 0, NULL);
            return;
        }
        tokAdd(outP, K_ATOM, 0, A_DOT, 0, nP->val, 0, ".");
        return;

    case M1T_PERM:
        spP = symbolSpelling(nP->pos, nP->end);
        if( (strcmp(spP, "law") == 0) && ((leftmost(sP->treeP) != nP) || (sP->kind != M1E_WORD)) )
        {
            // am1 takes law only as the first word of a statement
            tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, numberText(nP->val, 8));
            free(spP);
            return;
        }
        if( strcmp(spP, "xx") == 0 )
        {
            needXX = 1;
        }
        else if( strcmp(spP, "skip") == 0 )
        {
            needSkip = 1;
        }
        else if( strcmp(spP, "clc") == 0 )
        {
            needClc = 1;
        }
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, spP);
        return;

    case M1T_LABEL:
        key = noteSymbol(nP->pos, nP->end, 0);
        idx = mapGet(&bodyTagMap, key);
        if( idx > 0 )
        {
            // A tag of a define body is local to each expansion in am1.
            if( !isBodyTagOf(key, sP->body) || !currentFrame() )
            {
                *badP = 1;
                *whyP = "a tag of a define body used outside that body";
                tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, outName(key));
                return;
            }
            if( absTag )
            {
                tokAdd(outP, K_ATOM, 0, A_TAG, nP->val, nP->val, 0, outName(key));
                return;
            }
            tokAdd(outP, K_LP, 0, 0, 0, 0, 0, NULL);
            tokAdd(outP, K_ATOM, 0, A_TAG, nP->val, nP->val, 0, outName(key));
            tokAdd(outP, K_OP, '-', 0, 0, 0, 0, NULL);
            tokAdd(outP, K_ATOM, 0, A_R, 0, 0, 0, "R");
            tokAdd(outP, K_RP, 0, 0, 0, 0, 0, NULL);
            if( sP->body >= 0 )
            {
                macrosP[sP->body].usesR = 1;
            }
            return;
        }
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, outName(key));
        return;

    case M1T_VAR:
        key = noteSymbol(nP->pos, nP->end, 0);
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, outName(key));
        return;

    case M1T_EQU:
        key = noteSymbol(nP->pos, nP->end, 0);
        idx = mapGet(&defMap, key);
        if( (sP->body >= 0) && (idx < 0) && (mapGet(&fwdEquMap, key) < 0) )
        {
            // a body used before the #define: every expansion writes the value
            mapPut(&fwdEquMap, key, 1);
            changed = 1;
        }
        if( (idx < 0) || ((sP->body >= 0) && (mapGet(&fwdEquMap, key) >= 0)) ||
            ((sP->kind == M1E_ASSIGN) && (sP->assignKey == key)) )
        {
            // Not #defined yet where it is used, or a = that uses its own name, the old
            // value, which a #define cannot: written as its value.
            tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, numberText(nP->val, sP->radix));
            return;
        }
        dP = &defsP[idx];
        if( dP->isTag )
        {
            tokAdd(outP, K_ATOM, 0, A_VALUE, dP->val, nP->val, 0, outName(key));
            return;
        }
        tokAdd(outP, K_ATOM, 0, A_DEFINE, 0, nP->val, key, outName(key));
        return;

    case M1T_UNDEF:
        key = noteSymbol(nP->pos, nP->end, NC_UNDEF);
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, outName(key));
        return;

    case M1T_DUMMY:
        fP = currentFrame();
        if( !fP || (sP->body < 0) )
        {
            *badP = 1;
            *whyP = "a dummy outside its define body";
            tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, "0");
            return;
        }
        idx = nP->info;
        if( fP->bindTextP[idx] )
        {
            textP = fP->bindTextP[idx];
        }
        else
        {
            textP = macrosP[fP->macIdx].paramP[idx];
        }
        tokAdd(outP, K_ATOM, 0, A_DUMMY, idx, nP->val, 0, textP);
        return;

    case M1T_R:
        if( sP->body >= 0 )
        {
            macrosP[sP->body].usesR = 1;
        }
        tokAdd(outP, K_ATOM, 0, A_R, 0, nP->val, 0, "R");
        return;

    case M1T_FLEX:
    case M1T_CHAR:
        textP = charTermText(nP);
        if( !textP )
        {
            textP = numberText(nP->val, sP->radix);
        }
        tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, textP);
        return;

    case M1T_EPSEUDO:
        *badP = 1;
        *whyP = (nP->info == M1P_DECIMAL) ? "decimal inside an expression" : "octal inside an expression";
        tokAdd(outP, K_ATOM, 0, A_VALUE, 0, 0, 0, "0");
        return;

    default:
        memset(&s, 0, sizeof(s));
        *badP = 1;
        *whyP = (nP->kind == M1T_MACRO) ? "a macro name where a value is wanted" :
            "an expression macro1 cannot assemble";
        tokAdd(outP, K_ATOM, 0, A_VALUE, 0, 0, 0, "0");
        return;
    }
}

// am1 precedence of an operator as written: the space (SEPARATOR) lowest.
static int
am1Prec(int op)
{
    switch( op )
    {
    case ' ':
        return( 0 );
    case '|':
        return( 1 );
    case '^':
        return( 2 );
    case '&':
        return( 3 );
    case '+':
    case '-':
        return( 5 );
    case '*':
        return( 6 );
    }
    return( 9 );
}

// The operator a tree node is written with.
static int
writtenOp(Node *nP, Site *sP)
{
    if( nP->op == '!' )
    {
        return( '|' );
    }
    if( nP->op == ' ' )
    {
        if( nP->pos == sP->forcedSpace )
        {
            return( ' ' );
        }
        if( (sP->level >= 1) || (mapGet(&plusOps, nP->pos) >= 0) )
        {
            return( '+' );
        }
    }
    return( nP->op );
}

// Return 1 when a node is an additive chain operator.
static int
isAddOp(Node *nP)
{
    return( (nP->type == NT_BIN) && ((nP->op == ' ') || (nP->op == '+') || (nP->op == '-')) );
}

// In a define body, tag+R is the local tag itself.
// Find in the additive chain topped by nP
// a single R and a single added local tag.
// Return 1 and set them when found, else 0.
static int
findTagR(Node *nP, Site *sP, Node **rPP, Node **tagPP)
{
Node *chain[256];
int sign[256];
int n;
int i;
int nR;
int nTag;
Node *cP;
Node *tP;
Key key;

    n = 0;
    cP = nP;
    while( isAddOp(cP) && (n < 255) )
    {
        tP = cP->bP;
        chain[n] = tP;
        sign[n] = (cP->op == '-') ? -1 : 1;
        if( (tP->type == NT_UNARY) && (tP->op == '-') )
        {
            sign[n] = -sign[n];
            chain[n] = tP->aP;
        }
        n++;
        cP = cP->aP;
    }
    chain[n] = cP;
    sign[n] = 1;
    n++;

    nR = 0;
    nTag = 0;
    *rPP = NULL;
    *tagPP = NULL;
    for( i = 0; i < n; i++ )
    {
        tP = chain[i];
        if( tP->type != NT_TERM )
        {
            continue;
        }
        if( tP->kind == M1T_R )
        {
            nR++;
            if( sign[i] > 0 )
            {
                *rPP = tP;
            }
        }
        else if( tP->kind == M1T_LABEL )
        {
            key = noteSymbol(tP->pos, tP->end, 0);
            if( isBodyTagOf(key, sP->body) )
            {
                nTag++;
                if( sign[i] > 0 )
                {
                    *tagPP = tP;
                }
            }
        }
    }
    // R must not be the first term with a subtraction after it; that is left alone
    if( (chain[n - 1] == *rPP) && (n > 1) && (sign[n - 2] < 0) )
    {
        return( 0 );
    }

    return( (nR == 1) && (nTag == 1) && *rPP && *tagPP );
}

// Return 1 when am1 would read a constant's tree before knowing its value, a tag
// not defined yet, else 0.
// A local tag sets *localP, each expansion resets it.
// *culpritP is set to the #define or argument whose text names the tag,
// left -1 when the constant preceeds the tag.
static int
litForward(Node *nP, Site *sP, int litPos, int depth, int *localP, int *culpritP)
{
Key key;
int idx;
int inner;
Frame *fP;

    if( !nP || (depth > MAXDEPTH) )
    {
        return( 0 );
    }
    if( nP->type != NT_TERM )
    {
        return( litForward(nP->aP, sP, litPos, depth, localP, culpritP) |
            litForward(nP->bP, sP, litPos, depth, localP, culpritP) );
    }
    idx = -1;
    switch( nP->kind )
    {
    case M1T_LABEL:
    case M1T_VAR:
        key = noteSymbol(nP->pos, nP->end, 0);
        if( (sP->body >= 0) && isBodyTagOf(key, sP->body) )
        {
            if( mapGet(&bodyTagPos, key ^ ((Key)(sP->body + 1) << 57)) > litPos )
            {
                *localP = 1;
                return( 1 );
            }
            return( 0 );
        }
        return( mapGet(&seenMap, key) < 0 );

    case M1T_EQU:
        key = noteSymbol(nP->pos, nP->end, 0);
        idx = mapGet(&defMap, key);
        if( (idx < 0) || defsP[idx].numeric || defsP[idx].isTag )
        {
            return( 0 );
        }
        idx = defsP[idx].site;
        break;

    case M1T_DUMMY:
        fP = currentFrame();
        if( !fP || (nP->info > fP->given) || (fP->argSite[nP->info] < 0) )
        {
            return( 0 );
        }
        idx = fP->argSite[nP->info];
        break;

    default:
        return( 0 );
    }
    if( (sitesP[idx].level >= 2) || sitesP[idx].broken )
    {
        return( 0 );
    }
    inner = -1;
    if( litForward(sitesP[idx].treeP, &sitesP[idx], litPos, depth + 1, localP, &inner) )
    {
        if( *culpritP < 0 )
        {
            *culpritP = (inner >= 0) ? inner : idx;
        }
        return( 1 );
    }
    return( 0 );
}

// Record a constant of this round of pass 3, with how am1 will key its word.
static void
litRecord(Node *nP, Site *sP, TokList *tP, int start)
{
TokList part;
char *textP;
char *p;
int local;
unsigned long long h;
LitUse *uP;
int culprit;

    local = 0;
    h = 0;
    culprit = -1;
    if( litForward(nP->aP, sP, nP->pos, 0, &local, &culprit) )
    {
        part.vP = tP->vP + start;
        part.n = tP->n - start;
        textP = tokText(&part);
        h = 0xCBF29CE484222325ULL;
        for( p = textP; *p; p++ )
        {
            h = (h ^ (unsigned char)*p) * 0x100000001B3ULL;
        }
        if( local && currentFrame() )
        {
            h = (h ^ (unsigned long long)currentFrame()->serial) * 0x100000001B3ULL;
        }
        h |= 1;
        free(textP);
    }
    if( nLitUses == capLitUses )
    {
        capLitUses = (2 * capLitUses) + 256;
        litUsesP = xrealloc(litUsesP, capLitUses * sizeof(LitUse));
    }
    uP = &litUsesP[nLitUses++];
    uP->pool = poolNo;
    uP->val = nP->info & MASK18;
    uP->key = h;
    uP->pos = nP->pos;
    uP->culprit = culprit;
}

// Order constants by pool, value and key.
static int
cmpLitUses(const void *aP, const void *bP)
{
const LitUse *a;
const LitUse *b;

    a = aP;
    b = bP;
    if( a->pool != b->pool )
    {
        return( (a->pool < b->pool) ? -1 : 1 );
    }
    if( a->val != b->val )
    {
        return( (a->val < b->val) ? -1 : 1 );
    }
    if( a->key != b->key )
    {
        return( (a->key < b->key) ? -1 : 1 );
    }
    return( 0 );
}

// After a round: where one value of a pool would have more than one am1 key, write the
// constants am1 cannot key by value as their values.
static void
litSettle(void)
{
int i;
int j;
int k;
Site *sP;

    qsort(litUsesP, nLitUses, sizeof(LitUse), cmpLitUses);
    for( i = 0; i < nLitUses; i = j )
    {
        for( j = i + 1; (j < nLitUses) && (litUsesP[j].pool == litUsesP[i].pool) &&
            (litUsesP[j].val == litUsesP[i].val); j++ )
        {
        }
        if( litUsesP[i].key == litUsesP[j - 1].key )
        {
            continue;
        }
        for( k = i; k < j; k++ )
        {
            if( !litUsesP[k].key )
            {
                continue;
            }
            // the #define or argument that names the tag is written as its value, where it
            // has one value; otherwise the constant is
            sP = (litUsesP[k].culprit >= 0) ? &sitesP[litUsesP[k].culprit] : NULL;
            if( sP && (sP->level < 2) && (sP->body < 0) && !sP->varies &&
                !treeHas(sP->treeP, M1T_LITERAL) )
            {
                sP->level = 2;
                changed = 1;
            }
            else if( mapGet(&litNumeric, litUsesP[k].pos) < 0 )
            {
                mapPut(&litNumeric, litUsesP[k].pos, 1);
                changed = 1;
            }
        }
    }
}

// Render a tree into am1 tokens, in macro1's left-to-right order.
static void
renderNode(Node *nP, TokList *outP, Site *sP, int *badP, char **whyP, Node *skipRP, Node *absTagP)
{
Node *lP;
Node *rP;
Node *cP;
int op;
int lop;
int wrap;
int start;
int i;
Node *skipR;
Node *absTag;
TokList tmp;

    switch( nP->type )
    {
    case NT_TERM:
        renderTerm(nP, outP, sP, badP, whyP, nP == absTagP);
        return;

    case NT_UNARY:
        if( nP->op == '-' )
        {
            tokAdd(outP, K_NEG, 0, 0, 0, 0, 0, NULL);
        }
        renderNode(nP->aP, outP, sP, badP, whyP, skipRP, absTagP);
        return;

    case NT_LIT:
        if( mapGet(&orphanMap, nP->pos) >= 0 )
        {
            // macro1 never places it, and uses an address that holds something else
            note(0, M1LINE(nP->pos), "a constant after the last constants statement, macro1 never does this;"
                " written as the address macro1 uses");
            tokAdd(outP, K_ATOM, 0, A_VALUE, nP->val, nP->val, 0, numberText(nP->val, sP->radix));
            return;
        }
        tokAdd(outP, K_LB, 0, 0, nP->val, nP->info & MASK18, 0, NULL);
        if( mapGet(&litNumeric, nP->pos) >= 0 )
        {
            tokAdd(outP, K_ATOM, 0, A_VALUE, nP->info & MASK18, nP->info & MASK18, 0,
                numberText(nP->info, sP->radix));
            tokAdd(outP, K_RB, 0, 0, 0, 0, 0, NULL);
            return;
        }
        start = outP->n;
        renderNode(nP->aP, outP, sP, badP, whyP, NULL, NULL);
        if( curPass == 3 )
        {
            litRecord(nP, sP, outP, start);
        }
        tokAdd(outP, K_RB, 0, 0, 0, 0, 0, NULL);
        return;

    case NT_GROUP:
        tokAdd(outP, K_LP, 0, 0, 0, 0, 0, NULL);
        renderNode(nP->aP, outP, sP, badP, whyP, NULL, NULL);
        tokAdd(outP, K_RP, 0, 0, 0, 0, 0, NULL);
        return;
    }

    // A binary operator.
    // At the top of an additive chain in a define body, tag+R becomes the local tag.
    skipR = skipRP;
    absTag = absTagP;
    if( !skipR && (sP->body >= 0) && isAddOp(nP) && findTagR(nP, sP, &skipR, &absTag) )
    {
        // found: R is dropped and the tag written as itself
    }
    else if( !skipRP )
    {
        skipR = NULL;
        absTag = NULL;
    }

    lP = nP->aP;
    rP = nP->bP;
    op = writtenOp(nP, sP);

    // decimal or octal at either end of a statement is written on a line of its own.
    if( sP->preRadix && (lP == leftmost(sP->treeP)) )
    {
        renderNode(rP, outP, sP, badP, whyP, skipR, absTag);
        return;
    }
    if( sP->postRadix && (nP == sP->treeP) )
    {
        renderNode(lP, outP, sP, badP, whyP, skipR, absTag);
        return;
    }

    // The R being dropped: write the other side alone.
    if( rP == skipR )
    {
        renderNode(lP, outP, sP, badP, whyP, skipR, absTag);
        return;
    }
    if( (rP->type == NT_UNARY) && (rP->aP == skipR) )
    {
        renderNode(lP, outP, sP, badP, whyP, skipR, absTag);
        return;
    }
    if( lP == skipR )
    {
        // R first: the operator before the right term goes with it
        if( (rP->type == NT_UNARY) && (rP->op == '-') )
        {
            renderNode(rP, outP, sP, badP, whyP, skipR, absTag);
        }
        else
        {
            renderNode(rP, outP, sP, badP, whyP, skipR, absTag);
        }
        return;
    }

    // The left side keeps its own grouping when am1 would bind this operator tighter than
    // the left side's last one; a space on the left is left as is (the check decides).
    start = outP->n;
    memset(&tmp, 0, sizeof(tmp));
    renderNode(lP, &tmp, sP, badP, whyP, skipR, absTag);
    wrap = 0;
    cP = lP;
    if( cP->type == NT_BIN )
    {
        lop = writtenOp(cP, sP);
        if( (lop != ' ') && (am1Prec(lop) < am1Prec(op)) )
        {
            wrap = 1;
        }
    }
    if( (op == ' ') && (tmp.n > 0) )
    {
        // a space after a left side that is one term needs nothing
        wrap = 0;
    }
    if( wrap )
    {
        tokAdd(outP, K_LP, 0, 0, 0, 0, 0, NULL);
    }
    for( i = 0; i < tmp.n; i++ )
    {
        tokAdd(outP, tmp.vP[i].type, tmp.vP[i].op, tmp.vP[i].akind, tmp.vP[i].val,
            tmp.vP[i].m1val, tmp.vP[i].key, tmp.vP[i].textP);
    }
    free(tmp.vP);
    if( wrap )
    {
        tokAdd(outP, K_RP, 0, 0, 0, 0, 0, NULL);
    }
    (void)start;

    // A sign on the right term joins the operator: a -b is a-b, a--b is a+b.
    if( (rP->type == NT_UNARY) && ((op == ' ') || (op == '+') || (op == '-')) )
    {
        if( rP->op == '-' )
        {
            op = (op == '-') ? '+' : '-';
        }
        else if( op == ' ' )
        {
            op = '+';
        }
        tokAdd(outP, K_OP, op, 0, 0, 0, 0, NULL);
        renderNode(rP->aP, outP, sP, badP, whyP, skipR, absTag);
        return;
    }
    tokAdd(outP, K_OP, op, 0, 0, 0, 0, NULL);
    renderNode(rP, outP, sP, badP, whyP, skipR, absTag);
}

// Render a site's tree.
static void
render(Node *nP, TokList *outP, Site *sP, int *badP, char **whyP)
{
    renderNode(nP, outP, sP, badP, whyP, NULL, NULL);
}

// am1's one's complement helpers (Tools/AM1/eval.c).
static int
twosComplAdj(int v)
{
    return( (v < 0) ? (v + 1) : v );
}

static int
fixMinusZero(int v)
{
    if( v < 0 )
    {
        v--;
    }
    return( (v == -1) ? 0 : v );
}

typedef struct
{
    TokList *tP;
    int i;
    Frame *frameP;
    int depth;
    int *blameP;
    int owner;              // the site whose text this is
    int bad;
} Parser;

static int pSep(Parser *pP);

// The value of an atom where it is used.
static int
atomValue(Parser *pP, Tok *kP)
{
int v;
int idx;
int d;
Frame *fP;
DefRec *dP;

    switch( kP->akind )
    {
    case A_VALUE:
        return( kP->val );

    case A_DOT:
        return( m1Clc() );

    case A_DEFINE:
        idx = mapGet(&defMap, kP->key);
        if( (idx < 0) || (pP->depth > MAXDEPTH) )
        {
            pP->bad = 1;
            return( 0 );
        }
        dP = &defsP[idx];
        if( dP->numeric || dP->isTag )
        {
            v = dP->val;
        }
        else
        {
            v = am1EvalToks(&dP->toks, pP->frameP, pP->depth + 1, dP->site, pP->blameP);
        }
        if( ((v & MASK18) != (kP->m1val & MASK18)) && (*pP->blameP < 0) )
        {
            // When the name still has the value it was given, the difference is that macro1
            // used it before a later assignment: the text that used it is the one to fix.
            *pP->blameP = ((v & MASK18) == (dP->val & MASK18)) ? pP->owner : dP->site;
        }
        return( v );

    case A_DUMMY:
        fP = pP->frameP;
        d = kP->val;
        if( !fP || (pP->depth > MAXDEPTH) )
        {
            pP->bad = 1;
            return( 0 );
        }
        if( d > fP->given )
        {
            v = 0;
        }
        else
        {
            v = am1EvalToks(&fP->argToks[d], fP->parentP, pP->depth + 1, fP->argSite[d], pP->blameP);
        }
        if( fP->factor[d] > 1 )
        {
            v = fixMinusZero(twosComplAdj(v) * fP->factor[d]);
        }
        if( ((v & MASK18) != (kP->m1val & MASK18)) && (*pP->blameP < 0) && (d <= fP->given) &&
            (fP->factor[d] <= 1) )
        {
            *pP->blameP = fP->argSite[d];
        }
        return( v );

    case A_R:
        if( !pP->frameP )
        {
            pP->bad = 1;
            return( 0 );
        }
        return( pP->frameP->R );

    case A_TAG:
        if( !pP->frameP )
        {
            pP->bad = 1;
            return( 0 );
        }
        return( pP->frameP->R + kP->val );
    }
    return( 0 );
}

// A primary: an atom, (e) or [e].
static int
pPrimary(Parser *pP)
{
Tok *kP;
int v;
int inner;

    if( pP->i >= pP->tP->n )
    {
        pP->bad = 1;
        return( 0 );
    }
    kP = &pP->tP->vP[pP->i++];
    switch( kP->type )
    {
    case K_ATOM:
        return( atomValue(pP, kP) );

    case K_LP:
        v = pSep(pP);
        if( (pP->i < pP->tP->n) && (pP->tP->vP[pP->i].type == K_RP) )
        {
            pP->i++;
        }
        else
        {
            pP->bad = 1;
        }
        return( v );

    case K_LB:
        inner = pSep(pP) & MASK18;
        if( (pP->i < pP->tP->n) && (pP->tP->vP[pP->i].type == K_RB) )
        {
            pP->i++;
        }
        else
        {
            pP->bad = 1;
        }
        if( inner != kP->m1val )
        {
            pP->bad = 1;
        }
        return( kP->val );
    }
    pP->bad = 1;
    return( 0 );
}

// A unary minus: am1 complements, and a -0 result is 0.
static int
pUnary(Parser *pP)
{
int v;

    if( (pP->i < pP->tP->n) && (pP->tP->vP[pP->i].type == K_NEG) )
    {
        pP->i++;
        v = ~pUnary(pP);
        return( (v == -1) ? 0 : v );
    }
    return( pPrimary(pP) );
}

// Return the operator of the next token if it is one of ops, else 0.
static int
peekOp(Parser *pP, const char *opsP)
{
Tok *kP;

    if( pP->i >= pP->tP->n )
    {
        return( 0 );
    }
    kP = &pP->tP->vP[pP->i];
    if( (kP->type == K_OP) && strchr(opsP, kP->op) )
    {
        return( kP->op );
    }
    return( 0 );
}

// Apply am1's math to two values.
static int
am1Math(int op, int l, int r)
{
int rslt;

    l = twosComplAdj(l);
    r = twosComplAdj(r);
    switch( op )
    {
    case '+':
        rslt = l + r;
        break;
    case '-':
        rslt = l - r;
        break;
    default:
        rslt = l * r;
        break;
    }
    return( fixMinusZero(rslt) );
}

static int
pMul(Parser *pP)
{
int v;
int op;

    v = pUnary(pP);
    while( (op = peekOp(pP, "*")) )
    {
        pP->i++;
        v = am1Math(op, v, pUnary(pP));
    }
    return( v );
}

static int
pAdd(Parser *pP)
{
int v;
int op;

    v = pMul(pP);
    while( (op = peekOp(pP, "+-")) )
    {
        pP->i++;
        v = am1Math(op, v, pMul(pP));
    }
    return( v );
}

static int
pAnd(Parser *pP)
{
int v;

    v = pAdd(pP);
    while( peekOp(pP, "&") )
    {
        pP->i++;
        v &= pAdd(pP);
    }
    return( v );
}

static int
pOr(Parser *pP)
{
int v;

    v = pAnd(pP);
    while( peekOp(pP, "|") )
    {
        pP->i++;
        v |= pAnd(pP);
    }
    return( v );
}

// The lowest level: a space ORs.
static int
pSep(Parser *pP)
{
int v;

    v = pOr(pP);
    while( peekOp(pP, " ") )
    {
        pP->i++;
        v |= pOr(pP);
    }
    return( v );
}

// Return Am1's value of a token list in a frame.
// A define or parameter whose value differs from
// macro1's sets *blameP to its site, if not set already.
// Returns -1 << 20 when the list cannot be evaluated.
static int
am1EvalToks(TokList *tP, Frame *frameP, int depth, int owner, int *blameP)
{
Parser p;
int v;

    p.tP = tP;
    p.i = 0;
    p.frameP = frameP;
    p.depth = depth;
    p.blameP = blameP;
    p.owner = owner;
    p.bad = 0;
    if( tP->n == 0 )
    {
        return( 0 );
    }
    if( (depth == 0) && (tP->vP[0].type == K_ATOM) && tP->vP[0].textP &&
        (strcmp(tP->vP[0].textP, "law") == 0) )
    {
        // am1 reads law, a space or nothing, then an expression, and ORs them
        p.i = 1;
        if( (p.i < tP->n) && (tP->vP[p.i].type == K_OP) && (tP->vP[p.i].op == ' ') )
        {
            p.i++;
        }
        if( p.i == tP->n )
        {
            return( 0700000 );
        }
        if( tP->vP[p.i].type == K_OP )
        {
            return( -1 << 20 );
        }
        v = 0700000 | pSep(&p);
    }
    else
    {
        v = pSep(&p);
    }
    if( p.bad || (p.i != tP->n) )
    {
        return( -1 << 20 );
    }
    return( v );
}

// Mark every constant of a tree to be written as its value.
// Returns how many were not already marked.
static int
litsNumeric(Node *nP)
{
int n;

    if( !nP )
    {
        return( 0 );
    }
    n = 0;
    if( (nP->type == NT_LIT) && (mapGet(&litNumeric, nP->pos) < 0) )
    {
        mapPut(&litNumeric, nP->pos, 1);
        n = 1;
    }
    return( n + litsNumeric(nP->aP) + litsNumeric(nP->bP) );
}

// Raise a site's level one step, at most once a cycle.
// Returns 1 if it rose, else 0.
static int
escalate(int site, const char *whyP)
{
Site *sP;

    sP = &sitesP[site];
    if( (sP->escalatedIn == round3) || (sP->level >= MAXLEVEL) || sP->broken )
    {
        return( 0 );
    }
    sP->escalatedIn = round3;
    changed = 1;
    if( (sP->level == 0) && !treeHasSpace(sP->treeP) )
    {
        sP->level = 1;
    }
    if( (sP->level == 1) && litsNumeric(sP->treeP) )
    {
        // before giving up on a word with constants, their contents are written as values
        return( 1 );
    }
    sP->level++;
    if( (sP->level == 2) &&
        ((sP->body >= 0) || sP->varies || treeHas(sP->treeP, M1T_LITERAL) || (sP->kind == M1E_REPEAT)) )
    {
        sP->level = 3;
    }
    if( (sP->level == 3) && !sP->whyP )
    {
        sP->whyP = xstrdup(whyP);
    }
    return( 1 );
}

// Refuse a site outright.
static void
refuseSite(int site, const char *whyP)
{
Site *sP;

    sP = &sitesP[site];
    if( sP->level < MAXLEVEL )
    {
        sP->level = MAXLEVEL;
        changed = 1;
    }
    if( !sP->whyP )
    {
        sP->whyP = xstrdup(whyP);
    }
}

// Render and check a site's latest evaluation.
static void
checkSite(int site, Node *treeP, int val, int pass)
{
Site *sP;
TokList toks;
int bad;
int v;
int blame;
char *whyP;
char *textP;

    sP = &sitesP[site];
    if( sP->nEval++ == 0 )
    {
        sP->val = val;
    }
    else if( sP->val != val )
    {
        sP->varies = 1;
    }
    if( (m1Eval(treeP) & MASK18) != (val & MASK18) )
    {
        refuseSite(site, "an expression the converter could not follow");
        return;
    }
    if( sP->level >= MAXLEVEL )
    {
        return;
    }

    memset(&toks, 0, sizeof(toks));
    bad = 0;
    whyP = NULL;
    sP->treeP = treeP;
    render(treeP, &toks, sP, &bad, &whyP);
    if( bad )
    {
        refuseSite(site, whyP ? whyP : "an expression that cannot be written for am1");
        return;
    }
    if( sP->level == 2 )
    {
        if( sP->varies )
        {
            refuseSite(site, "an expression whose value differs from one use to the next");
            return;
        }
        toks.n = 0;
        tokAdd(&toks, K_ATOM, 0, A_VALUE, val & MASK18, val & MASK18, 0, numberText(val, sP->radix));
    }
    textP = tokText(&toks);

    if( (pass == 3) && (sP->kind != M1E_NOWORD) && (sP->kind != M1E_DIMENSION) )
    {
        blame = -1;
        v = am1EvalToks(&toks, currentFrame(), 0, site, &blame);
        if( (v == (-1 << 20)) || ((v & MASK18) != (val & MASK18)) )
        {
            if( (blame >= 0) && (blame != site) )
            {
                // the define or argument is at fault; once it is refused, that is the error
                escalate(blame, "an expression am1 evaluates differently from macro1");
            }
            else
            {
                escalate(site, "an expression am1 evaluates differently from macro1");
            }
        }
    }

    if( sP->textP && (strcmp(sP->textP, textP) != 0) && (sP->nEval > 1) )
    {
        if( !sP->textVaries )
        {
            free(sP->varyP);
            sP->varyP = xmalloc(strlen(sP->textP) + strlen(textP) + 96);
            sprintf(sP->varyP, "an expression written %s in one use and %s in another", sP->textP, textP);
        }
        sP->textVaries = 1;
    }
    sP->textP = textP;
    if( pendingP && (sP->kind == M1E_ARG) )
    {
        pendingP->argToks[pendingP->given + 1] = tokCopy(&toks);
    }
    if( sP->kind == M1E_ASSIGN )
    {
        lastAssignSite = site;
        free(defsP ? NULL : NULL);
        if( nDefs == capDefs )
        {
            capDefs = (2 * capDefs) + 64;
            defsP = xrealloc(defsP, capDefs * sizeof(DefRec));
        }
        memset(&defsP[nDefs], 0, sizeof(DefRec));
        defsP[nDefs].toks = tokCopy(&toks);
        defsP[nDefs].site = site;
        defsP[nDefs].val = val & MASK18;
        defsP[nDefs].numeric = (sP->level == 2);
        nDefs++;
    }
    free(toks.vP);
}

// Hooks.

void
hTitle(int pass, int line)
{
    if( recording(pass) && (line >= 0) && (line < m1LineCount) && (lineRoleP[line] == LR_CODE) )
    {
        lineRoleP[line] = LR_TITLE;
    }
}

void
hTerm(int pass, int kind, int pos, int end, int val, int info)
{
Node *nP;
Node *cP;

    if( pass == 1 )
    {
        return;
    }
    nP = xmalloc(sizeof(Node));
    nP->type = NT_TERM;
    nP->kind = kind;
    nP->pos = pos;
    nP->end = end;
    nP->val = val;
    nP->info = info;
    if( (kind == M1T_LITERAL) && (pass == 2) )
    {
        litPosP = xrealloc(litPosP, (nLitPos + 1) * sizeof(int));
        litPosP[nLitPos++] = pos;
    }
    if( (kind == M1T_EPSEUDO) && (info == M1P_DECIMAL) && (pass == 2) )
    {
        sawDecimal = 1;
    }
    if( (kind == M1T_LITERAL) || (kind == M1T_GROUP) )
    {
        if( buildDepth == 0 )
        {
            buildDirty = 1;
            return;
        }
        cP = buildCur[buildDepth];
        buildDepth--;
        nP->type = (kind == M1T_LITERAL) ? NT_LIT : NT_GROUP;
        nP->aP = cP;
        if( !cP )
        {
            buildDirty = 1;
            nP->aP = xmalloc(sizeof(Node));
            nP->aP->type = NT_TERM;
            nP->aP->kind = M1T_BAD;
        }
    }
    buildAttach(nP);
}

void
hUnary(int pass, int pos, int op)
{
Node *nP;
Node **slotP;

    if( pass == 1 )
    {
        return;
    }
    slotP = buildSlot[buildDepth];
    if( !slotP || !*slotP )
    {
        buildDirty = 1;
        return;
    }
    nP = xmalloc(sizeof(Node));
    nP->type = NT_UNARY;
    nP->op = op;
    nP->pos = pos;
    nP->aP = *slotP;
    *slotP = nP;
}

void
hOp(int pass, int pos, int op)
{
    if( pass == 1 )
    {
        return;
    }
    buildOp[buildDepth] = op;
    buildOpPos[buildDepth] = pos;
}

void
hOpen(int pass, int pos, int bracket)
{
    (void)pos;
    (void)bracket;
    if( pass == 1 )
    {
        return;
    }
    if( buildDepth < 62 )
    {
        buildDepth++;
        buildCur[buildDepth] = NULL;
        buildSlot[buildDepth] = NULL;
        buildOp[buildDepth] = 0;
    }
}

// The key of the name before the = that ends just before pos, or 0.
static Key
assignedName(int pos)
{
const char *lineP;
char name[64];
int c;
int c1;
int n;

    lineP = m1Lines[M1LINE(pos)];
    c = M1COL(pos) - 1;
    while( (c >= 0) && ((lineP[c] == ' ') || (lineP[c] == '\t')) )
    {
        c--;
    }
    if( (c < 0) || (lineP[c] != '=') )
    {
        return( 0 );
    }
    c--;
    while( (c >= 0) && ((lineP[c] == ' ') || (lineP[c] == '\t')) )
    {
        c--;
    }
    c1 = c + 1;
    while( (c >= 0) && (isalnum((unsigned char)lineP[c]) || (lineP[c] == '\\') || (lineP[c] == '~')) )
    {
        c--;
    }
    n = 0;
    for( c++; (c < c1) && (n < 63); c++ )
    {
        if( (lineP[c] != '\\') && (lineP[c] != '~') )
        {
            name[n++] = lineP[c];
        }
    }
    name[n] = '\0';
    return( n ? keyOfName(name) : 0 );
}

void
hExpr(int pass, int kind, int pos, int end, int val)
{
Node *treeP;
int site;
int isNew;
int item;
Site *sP;
Node *lmP;
char *spP;
Node *cP;

    treeP = buildCur[0];
    if( (buildDepth != 0) || buildDirty )
    {
        treeP = NULL;
    }
    buildReset();
    if( pass == 1 )
    {
        return;
    }
    if( pos < 0 )
    {
        return;
    }
    site = siteFor(kind, pos, end);
    sP = &sitesP[site];
    if( (pass == 2) && (m1Radix() == 10) )
    {
        sawDecimal = 1;
    }
    if( !treeP )
    {
        // macro1 rejects it; it is written as it stands
        sP->broken = 1;
    }
    if( recording(pass) && treeP && !sP->treeP )
    {
        sP->treeP = treeP;
        sP->radix = m1Radix();
        if( kind == M1E_ASSIGN )
        {
            sP->assignKey = assignedName(pos);
        }
        if( kind == M1E_ORIGIN )
        {
            // am1 takes no space in an origin
            sP->level = 1;
        }
        lmP = leftmost(treeP);
        if( (kind == M1E_WORD) && (lmP->type == NT_TERM) && (lmP->kind == M1T_PERM) )
        {
            spP = symbolSpelling(lmP->pos, lmP->end);
            if( strcmp(spP, "law") == 0 )
            {
                // the space after law stays: am1 takes law SEPARATOR expression only
                cP = treeP;
                while( (cP->type == NT_BIN) && (cP->aP != lmP) )
                {
                    cP = cP->aP;
                }
                if( (cP->type == NT_BIN) && (cP->op == ' ') )
                {
                    sP->forcedSpace = cP->pos;
                }
            }
            free(spP);
        }
        if( (kind == M1E_WORD) && (treeP->type == NT_BIN) )
        {
            // decimal or octal as the first or last term of a statement switches the radix
            // for the terms after it; am1 does that on a line of its own
            cP = treeP;
            while( cP->aP->type == NT_BIN )
            {
                cP = cP->aP;
            }
            if( (lmP->type == NT_TERM) && (lmP->kind == M1T_EPSEUDO) &&
                ((cP->op == ' ') || (cP->op == '+')) )
            {
                sP->preRadix = lmP->info + 1;
            }
            cP = treeP->bP;
            if( (cP->type == NT_TERM) && (cP->kind == M1T_EPSEUDO) &&
                ((treeP->op == ' ') || (treeP->op == '+')) )
            {
                sP->postRadix = cP->info + 1;
            }
        }
    }
    if( recording(pass) )
    {
        if( (kind == M1E_WORD) || (kind == M1E_LOCCHECK) || (kind == M1E_ORIGIN) || (kind == M1E_NOWORD) )
        {
            item = itemFor(IT_EXPR, pos, end, &isNew);
            itemsP[item].site = site;
        }
        if( (pass == 2) && treeP )
        {
            noteOverlaps(treeP);
        }
    }

    // Passes 3 and 4: write it, and on pass 3 check it.
    if( (pass >= 3) && treeP )
    {
        if( pass == 4 )
        {
            sP->nEval = 0;
        }
        checkSite(site, treeP, val, pass);
    }
    if( kind == M1E_ASSIGN )
    {
        lastAssignSite = site;
    }
    if( pendingP && (kind == M1E_ARG) )
    {
        pendingP->argSite[pendingP->given + 1] = site;
        pendingP->given++;
    }
    if( kind == M1E_START )
    {
        lastStartSite = site;
    }
    if( kind == M1E_REPEAT )
    {
        lastRepeatSite = site;
    }
}

void
hWord(int pass, int addr, int val)
{
    (void)pass;
    (void)addr;
    (void)val;
}

void
hLabel(int pass, int pos, int end, int val, int inMacro, int dummyTag)
{
int item;
int isNew;
int mac;
Key key;
Macro *mP;
int i;

    (void)val;
    if( (pass == 3) && (pos >= 0) && !dummyTag )
    {
        mapPut(&seenMap, noteSymbol(pos, end, 0), 1);
    }
    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_TAG, pos, end, &isNew);
    itemsP[item].a = inMacro;
    itemsP[item].b = dummyTag;
    if( dummyTag )
    {
        return;
    }
    mac = bodyOf(M1LINE(pos));
    key = noteSymbol(pos, end, (mac >= 0) ? NC_BODYTAG : NC_LABEL);
    if( (mac >= 0) && inMacro )
    {
        mP = &macrosP[mac];
        for( i = 0; i < mP->ntags; i++ )
        {
            if( mP->tagsP[i] == key )
            {
                return;
            }
        }
        mP->tagsP = xrealloc(mP->tagsP, (mP->ntags + 1) * sizeof(Key));
        mP->tagsP[mP->ntags++] = key;
        mapPut(&bodyTagMap, key, mac + 1);
        mapPut(&bodyTagPos, key ^ ((Key)(mac + 1) << 57), pos);
    }
}

void
hNumTag(int pass, int pos, int end)
{
int isNew;

    if( recording(pass) && (pos >= 0) )
    {
        itemFor(IT_NUMTAG, pos, end, &isNew);
    }
}

void
hAssign(int pass, int pos, int end, int val, int dummy)
{
int item;
int isNew;
int site;
Site *sP;
Frame *fP;
DefRec *dP;
Key key;
Node *tP;
int idx;
char buf[64];
int i;

    if( (pass == 1) || (pos < 0) )
    {
        return;
    }
    site = lastAssignSite;
    if( recording(pass) )
    {
        item = itemFor(IT_ASSIGN, pos, end, &isNew);
        itemsP[item].site = site;
        itemsP[item].a = dummy;
        if( dummy < 0 )
        {
            key = noteSymbol(pos, end, NC_EQU);
            idx = -1;
            for( i = 0; am1Perms[i].nameP; i++ )
            {
                if( keyOfName(am1Perms[i].nameP) == key )
                {
                    idx = i;
                }
            }
            if( idx >= 0 )
            {
                i = mapGet(&classMap, key);
                mapPut(&classMap, key, i | (((val & MASK18) == am1Perms[idx].val) ? NC_EQUAM1 : NC_EQUOTHER));
            }
        }
    }
    if( (site < 0) || ((pass != 3) && (pass != 4)) )
    {
        return;
    }
    sP = &sitesP[site];
    if( dummy > 0 )
    {
        // D=D+D on a dummy: later uses are written (D)*2, (D)*4, ...
        fP = currentFrame();
        tP = sP->treeP;
        if( !fP || !tP || (tP->type != NT_BIN) || ((tP->op != ' ') && (tP->op != '+')) ||
            (tP->aP->type != NT_TERM) || (tP->bP->type != NT_TERM) ||
            (tP->aP->kind != M1T_DUMMY) || (tP->bP->kind != M1T_DUMMY) ||
            (tP->aP->info != dummy) || (tP->bP->info != dummy) )
        {
            refuseSite(site, "an assignment to a dummy other than D=D+D");
            return;
        }
        fP->factor[dummy] = (fP->factor[dummy] > 0) ? (2 * fP->factor[dummy]) : 2;
        snprintf(buf, sizeof(buf), "(%s)*%s", macrosP[fP->macIdx].paramP[dummy],
            numberText(fP->factor[dummy], 8));
        fP->bindTextP[dummy] = xstrdup(buf);
        return;
    }
    if( dummy == 0 )
    {
        refuseSite(site, "an assignment to R");
        return;
    }
    if( sP->body >= 0 )
    {
        refuseSite(site, "an assignment inside a define body");
        return;
    }
    if( treeHas(sP->treeP, M1T_LITERAL) )
    {
        refuseSite(site, "an assignment of a constant's address");
        return;
    }
    if( nDefs == 0 )
    {
        return;
    }
    dP = &defsP[nDefs - 1];
    if( dP->site != site )
    {
        return;
    }
    key = noteSymbol(pos, end, 0);
    dP->key = key;
    tP = sP->treeP;
    if( (tP->type == NT_TERM) && (tP->kind == M1T_DOT) )
    {
        dP->isTag = 1;
    }
    else if( treeHas(tP, M1T_DOT) && (sP->level < 2) )
    {
        // a value that depends on where it stands is written as that value
        sP->level = 2;
        dP->numeric = 1;
        changed = 1;
    }
    mapPut(&defMap, key, nDefs - 1);
}

// A define's name as its line spells it: macro1 keeps 6 characters, the source may have more.
static char *
defineSpelling(int line, const char *nameP)
{
const char *srcP;
const char *p;
const char *q;
char *rP;
int n;

    srcP = m1Lines[line];
    n = (int)strlen(nameP);
    for( p = srcP; (p = strstr(p, nameP)) != NULL; p++ )
    {
        if( (p == srcP) || !isalnum((unsigned char)p[-1]) )
        {
            q = p;
            while( isalnum((unsigned char)*q) )
            {
                q++;
            }
            if( (q - p) >= n )
            {
                rP = xmalloc(q - p + 1);
                memcpy(rP, p, q - p);
                rP[q - p] = '\0';
                return( rP );
            }
        }
    }
    return( xstrdup(nameP) );
}

void
hDefine(int pass, int pos, int nameLine, int macIdx, const char *nameP,
        char args[][M1SYMLEN], int nargs, int termLine)
{
int isNew;
int item;
int line;
int last;
int i;
Macro *mP;
char buf[M1SYMLEN + 2];

    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_DEFINE, pos, pos, &isNew);
    itemsP[item].a = macIdx;
    if( macIdx < 0 )
    {
        note(1, M1LINE(pos), "a define macro1 cannot read");
        itemsP[item].whyP = "a define macro1 cannot read";
        return;
    }
    if( macIdx >= nMacros )
    {
        macrosP = xrealloc(macrosP, (macIdx + 1) * sizeof(Macro));
        memset(&macrosP[nMacros], 0, (macIdx + 1 - nMacros) * sizeof(Macro));
        nMacros = macIdx + 1;
    }
    mP = &macrosP[macIdx];
    if( mP->defined )
    {
        return;
    }
    mP->defined = 1;
    strcpy(mP->name, nameP);
    mP->key = keyOfName(nameP);
    mP->nargs = nargs;
    mP->defLine = M1LINE(pos);
    mP->nameLine = nameLine;
    mP->termLine = termLine;
    for( i = 0; i < nargs; i++ )
    {
        strcpy(mP->args[i], args[i]);
        if( isdigit((unsigned char)args[i][0]) )
        {
            snprintf(buf, sizeof(buf), "_%s", args[i]);
            mP->paramP[i + 1] = xstrdup(buf);
        }
        else if( strcmp(args[i], "R") == 0 )
        {
            // R is the expansion's location to macro1 before it is this dummy, and the
            // local tag R here
            mP->paramP[i + 1] = xstrdup("R_");
        }
        else
        {
            mP->paramP[i + 1] = xstrdup(args[i]);
        }
    }
    i = mapGet(&classMap, mP->key);
    mapPut(&classMap, mP->key, ((i < 0) ? 0 : i) | NC_MACRO);
    if( mapGet(&spellMap, mP->key) < 0 )
    {
        if( nSpellings == capSpellings )
        {
            capSpellings = (2 * capSpellings) + 64;
            spellingsP = xrealloc(spellingsP, capSpellings * sizeof(char *));
        }
        spellingsP[nSpellings] = defineSpelling(nameLine, nameP);
        mapPut(&spellMap, mP->key, nSpellings++);
    }

    lineRoleP[M1LINE(pos)] = LR_DEFHEAD;
    lineMacP[M1LINE(pos)] = macIdx;
    if( nameLine != M1LINE(pos) )
    {
        lineRoleP[nameLine] = LR_DEFNAME;
        lineMacP[nameLine] = macIdx;
    }
    last = (termLine >= 0) ? termLine : m1LineCount;
    for( line = nameLine + 1; line < last; line++ )
    {
        lineRoleP[line] = LR_BODY;
        lineMacP[line] = macIdx;
    }
    if( termLine >= 0 )
    {
        lineRoleP[termLine] = LR_TERM;
        lineMacP[termLine] = macIdx;
    }
    else
    {
        note(1, M1LINE(pos), "a define with no terminate");
    }
}

void
hInvokeStart(int pass, int pos, int end, int macIdx)
{
Frame *fP;
int item;
int isNew;

    if( pass == 1 )
    {
        return;
    }
    fP = xmalloc(sizeof(Frame));
    fP->macIdx = macIdx;
    fP->R = m1Clc();
    fP->parentP = frameTopP;
    fP->phantom = (pass == 4);
    pendingP = fP;
    if( (macIdx >= 0) && (macIdx < nMacros) && (pass != 4) )
    {
        macrosP[macIdx].invoked = 1;
    }
    if( recording(pass) && (pos >= 0) )
    {
        item = itemFor(IT_INVOKE, pos, end, &isNew);
        itemsP[item].a = macIdx;
    }
}

void
hInvokeArgs(int pass, int nargs, int end)
{
int item;
int isNew;
int i;
Frame *fP;
Key key;

    if( (pass == 1) || !pendingP )
    {
        return;
    }
    fP = pendingP;
    pendingP = NULL;
    fP->serial = ++expansionSerial;
    fP->given = (nargs < M1MAXARGS) ? nargs : M1MAXARGS;
    fP->parentP = frameTopP;
    frameTopP = fP;
    if( recording(pass) )
    {
        key = 0;
        (void)key;
        // the item of this use, found by its macro and the sites of its arguments
        for( item = nItems - 1; item >= 0; item-- )
        {
            if( (itemsP[item].type == IT_INVOKE) && (itemsP[item].a == fP->macIdx) && !itemsP[item].listP )
            {
                break;
            }
            if( (itemsP[item].type == IT_INVOKE) && (itemsP[item].a == fP->macIdx) )
            {
                item = -1;
                break;
            }
        }
        if( item >= 0 )
        {
            itemsP[item].end = (end >= 0) ? end : itemsP[item].end;
            itemsP[item].b = nargs;
            itemsP[item].nlist = fP->given;
            itemsP[item].listP = xmalloc((fP->given + 1) * sizeof(int));
            for( i = 1; i <= fP->given; i++ )
            {
                itemsP[item].listP[i - 1] = fP->argSite[i];
            }
        }
        (void)isNew;
    }
}

void
hInvokeEnd(int pass)
{
Frame *fP;

    if( (pass == 1) || !frameTopP )
    {
        return;
    }
    fP = frameTopP;
    frameTopP = fP->parentP;
}

void
hInvokeEmpty(int pass, int pos, int end, int macIdx)
{
int item;
int isNew;

    if( recording(pass) && (pos >= 0) )
    {
        item = itemFor(IT_EMPTYINV, pos, end, &isNew);
        itemsP[item].a = macIdx;
    }
}

// A pseudo-instruction.
// Expunge empties macro1's symbol table, pseudo-instructions and all,
// so pass 2 never sees it or any other existing symbol.
// This is not supported and is an error.
void
hPseudo(int pass, int pos, int end, int code)
{
int item;
int isNew;

    if( code == M1P_EXPUNGE )
    {
        hFatal(pass, pos, "expunge, which empties macro1's symbol table");
    }
    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    if( (code == M1P_NOINPUT) || (code == M1P_DIMENSION) )
    {
        item = itemFor(IT_PSEUDO, pos, end, &isNew);
        itemsP[item].a = code;
    }
}

void
hConstants(int pass, int pos, int start, int reserved, int nvals, const int *valsP)
{
int item;
int isNew;

    (void)valsP;
    if( pass == 3 )
    {
        poolNo++;
    }
    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_CONSTANTS, pos, pos, &isNew);
    if( pass == 2 )
    {
        litMark = nLitPos;
        itemsP[item].a = start;
        itemsP[item].b = reserved;
        itemsP[item].c = nvals;
    }
}

void
hVariables(int pass, int pos, int start, int end)
{
int item;
int isNew;
int i;

    if( pass == 3 )
    {
        for( i = 0; i < nVars; i++ )
        {
            if( (varsP[i].addr >= start) && (varsP[i].addr < end) )
            {
                mapPut(&seenMap, varsP[i].key, 1);
            }
        }
    }
    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_VARIABLES, pos, pos, &isNew);
    if( pass == 2 )
    {
        itemsP[item].a = start;
        itemsP[item].b = end;
    }
}

void
hVarAlloc(int pass, const char *nameP, int addr, int size)
{
Key key;
int i;

    if( pass != 2 )
    {
        return;
    }
    key = keyOfName(nameP);
    for( i = 0; i < nVars; i++ )
    {
        if( varsP[i].key == key )
        {
            return;
        }
    }
    if( nVars == capVars )
    {
        capVars = (2 * capVars) + 32;
        varsP = xrealloc(varsP, capVars * sizeof(Var));
    }
    varsP[nVars].key = key;
    varsP[nVars].addr = addr;
    varsP[nVars].size = size;
    nVars++;
    i = mapGet(&classMap, key);
    mapPut(&classMap, key, ((i < 0) ? 0 : i) | NC_VAR);
}

void
hText(int pass, int pos, int end, int nwords, const int *wordsP)
{
int item;
int isNew;
int line;
int i;

    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_TEXT, pos, end, &isNew);
    if( isNew )
    {
        itemsP[item].nlist = nwords;
        itemsP[item].listP = xmalloc((nwords + 1) * sizeof(int));
        for( i = 0; i < nwords; i++ )
        {
            itemsP[item].listP[i] = wordsP[i];
        }
        for( line = M1LINE(pos) + 1; line <= M1LINE(end); line++ )
        {
            if( line < m1LineCount )
            {
                if( line < M1LINE(end) )
                {
                    lineRoleP[line] = LR_TEXTCONT;
                }
                lineTextColP[line] = (line == M1LINE(end)) ? M1COL(end) : 0;
            }
        }
    }
}

void
hRepeat(int pass, int pos, int count, int bodyPos, int bodyEnd)
{
int item;
int isNew;

    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_REPEAT, pos, bodyPos, &isNew);
    if( isNew )
    {
        itemsP[item].a = count;
        itemsP[item].b = bodyPos;
        itemsP[item].c = bodyEnd;
        itemsP[item].site = lastRepeatSite;
    }
    else if( (itemsP[item].a != count) && (pass == 2) )
    {
        itemsP[item].d = 1;
    }
}

void
hRepeatEnd(int pass)
{
    (void)pass;
}

void
hStart(int pass, int pos, int end, int haveAddr, int addr)
{
int item;
int isNew;

    (void)addr;
    if( !recording(pass) || (pos < 0) )
    {
        return;
    }
    item = itemFor(IT_START, pos, end, &isNew);
    itemsP[item].a = haveAddr;
    itemsP[item].site = haveAddr ? lastStartSite : -1;
}

void
hComment(int pass, int pos)
{
int isNew;

    if( recording(pass) && (pos >= 0) )
    {
        itemFor(IT_COMMENT, pos, M1POS(M1LINE(pos), M1MAXCOL), &isNew);
    }
}

void
hError(int pass, int pos, const char *msgP)
{
    buildDirty = 1;
    if( (pass == 2) && (pos >= 0) )
    {
        note(0, M1LINE(pos), "macro1 reports: %s", msgP);
    }
}

void
hOrphanLiterals(int pass, int count)
{
int i;

    // the literals pass 2 met after the last constants statement
    (void)count;
    if( pass == 2 )
    {
        for( i = litMark; i < nLitPos; i++ )
        {
            mapPut(&orphanMap, litPosP[i], 1);
        }
    }
}

void
hFatal(int pass, int pos, const char *msgP)
{
FILE *fP;
int line;

    (void)pass;
    line = (pos >= 0) ? M1LINE(pos) : 0;
    fP = outPathP ? fopen(outPathP, "w") : stdout;
    if( fP )
    {
        fprintf(fP, "not converted\n#error \"mactoam1: line %d: %s\"\n    stop\n", line + 1, msgP);
        if( fP != stdout )
        {
            fclose(fP);
        }
    }
    fprintf(stderr, "%s:%d: error: %s\n", srcNameP, line + 1, msgP);
    exit(1);
}

// Return 1 when a name is an am1 keyword.
static int
isAm1Keyword(const char *nameP)
{
int i;

    for( i = 0; am1Keywords[i]; i++ )
    {
        if( strcmp(am1Keywords[i], nameP) == 0 )
        {
            return( 1 );
        }
    }
    return( 0 );
}

// Return 1 when a name is an am1 permanent symbol.
static int
isAm1Perm(const char *nameP)
{
int i;

    for( i = 0; am1Perms[i].nameP; i++ )
    {
        if( strcmp(am1Perms[i].nameP, nameP) == 0 )
        {
            return( 1 );
        }
    }
    return( 0 );
}

// The name written for a symbol: its first spelling, with _ in front of a digit and _ after a
// name am1 reserves.
static const char *
outName(Key key)
{
int idx;
int cls;
char buf[256];
const char *spP;

    idx = mapGet(&outNameMap, key);
    if( idx >= 0 )
    {
        return( outNamesP[idx] );
    }
    idx = mapGet(&spellMap, key);
    spP = (idx >= 0) ? spellingsP[idx] : "unknown";
    cls = mapGet(&classMap, key);
    if( cls < 0 )
    {
        cls = 0;
    }
    if( isdigit((unsigned char)spP[0]) )
    {
        snprintf(buf, sizeof(buf), "_%s", spP);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s", spP);
    }
    if( isAm1Keyword(buf) || (strcmp(buf, "R") == 0) ||
        (isAm1Perm(buf) && !((cls & NC_EQUAM1) && !(cls & NC_EQUOTHER) &&
        !(cls & (NC_LABEL | NC_VAR | NC_MACRO | NC_BODYTAG)))) )
    {
        strcat(buf, "_");
    }
    while( (mapGet(&usedOutNames, keyOfName(buf) ^ ((Key)strlen(buf) << 48)) >= 0) &&
        (strlen(buf) < 200) )
    {
        strcat(buf, "_");
    }
    if( strcmp(buf, spP) != 0 )
    {
        note(0, -1, "%s is written %s", spP, buf);
    }
    mapPut(&usedOutNames, keyOfName(buf) ^ ((Key)strlen(buf) << 48), 1);
    outNamesP = xrealloc(outNamesP, (nOutNames + 1) * sizeof(char *));
    outNamesP[nOutNames] = xstrdup(buf);
    mapPut(&outNameMap, key, nOutNames);
    return( outNamesP[nOutNames++] );
}

// Output.

static char **outLinesP;
static int nOut;
static int capOut;
static Map definedOut;      // name key: 1 while written as a #define, 2 as a tag
static int finalStart = -1; // the item of the start am1 is given, the last with an address
static char *startTextP;    // its am1 text
static int inBody;          // 1 while a #define block is being written
static char **heldP;        // the #error lines of that block
static int nHeld;
static int capHeld;

// Add a line to the output, its tabs expanded.
static void
outLine(const char *textP)
{
Str s;
int col;
const char *p;

    memset(&s, 0, sizeof(s));
    col = 0;
    for( p = textP; *p; p++ )
    {
        if( *p == '\t' )
        {
            do
            {
                sAddc(&s, ' ');
                col++;
            } while( col % tabWidth );
        }
        else
        {
            sAddc(&s, *p);
            col++;
        }
    }
    while( (s.len > 0) && (s.bufP[s.len - 1] == ' ') )
    {
        s.bufP[--s.len] = '\0';
    }
    if( nOut == capOut )
    {
        capOut = (2 * capOut) + 256;
        outLinesP = xrealloc(outLinesP, capOut * sizeof(char *));
    }
    // am1 will not take a #error inside a #define block, so one there waits for the #enddef
    // and a comment stands in its place
    if( inBody && s.bufP && (strncmp(s.bufP, "#error", 6) == 0) )
    {
        if( nHeld == capHeld )
        {
            capHeld = (2 * capHeld) + 8;
            heldP = xrealloc(heldP, capHeld * sizeof(char *));
        }
        heldP[nHeld++] = s.bufP;
        memset(&s, 0, sizeof(s));
        sAdd(&s, "// refused: the #error follows #enddef");
    }
    outLinesP[nOut++] = s.bufP ? s.bufP : xstrdup("");
}

// The #enddef is written: write the #error lines held from the body.
static void
endBody(void)
{
int i;

    inBody = 0;
    for( i = 0; i < nHeld; i++ )
    {
        outLine(heldP[i]);
        free(heldP[i]);
    }
    nHeld = 0;
}

// A printable reason for a #error string.
// The copy is never freed, there are too few to worry about.
static char *
quoteSafe(const char *whyP)
{
char *p;
char *q;

    p = xstrdup(whyP);
    for( q = p; *q; q++ )
    {
        if( *q == '"' )
        {
            *q = '\'';
        }
    }
    return( p );
}

// A #error line and its stderr note.
static void
outError(int line, const char *whyP)
{
char buf[600];

    note(1, line, "%s", whyP);
    snprintf(buf, sizeof(buf), "#error \"mactoam1: line %d: %s\"", line + 1, quoteSafe(whyP));
    outLine(buf);
}

// The visual column of a source column, tabs at tabWidth.
static int
visualCol(int line, int col)
{
const char *p;
int v;
int i;

    p = ((line >= 0) && (line < m1LineCount)) ? m1Lines[line] : "";
    v = 0;
    for( i = 0; (i < col) && p[i]; i++ )
    {
        v = (p[i] == '\t') ? (((v / tabWidth) + 1) * tabWidth) : (v + 1);
    }
    return( v );
}

// Spaces to a visual column.
static void
padTo(Str *sP, int vcol)
{
int i;

    for( i = 0; i < vcol; i++ )
    {
        sAddc(sP, ' ');
    }
}

// An expression macro1 rejects, in am1's brackets: ( is a constant and [ a group in macro1,
// the other way round in am1; a constant left open is closed; ! is |.
static char *
transliterate(int pos, int end)
{
char *srcP;
char *p;
char *q;
char open[256];
int nopen;
int v;
Str s;

    srcP = spanText(pos, end);
    memset(&s, 0, sizeof(s));
    nopen = 0;
    for( p = srcP; *p; p++ )
    {
        switch( *p )
        {
        case '(':
            sAddc(&s, '[');
            if( nopen < 255 )
            {
                open[nopen++] = ']';
            }
            break;
        case '[':
            sAddc(&s, '(');
            if( nopen < 255 )
            {
                open[nopen++] = ')';
            }
            break;
        case ')':
        case ']':
            if( nopen > 0 )
            {
                sAddc(&s, open[--nopen]);
            }
            break;
        case '!':
            sAddc(&s, '|');
            break;
        default:
            q = p + 4;
            while( (*q == ' ') || (*q == '\t') )
            {
                q++;
            }
            if( (strncmp(p, "char", 4) == 0) && ((p == srcP) || !isalnum((unsigned char)p[-1])) &&
                (q > (p + 4)) && q[0] && q[1] && strchr("lmr", q[0]) )
            {
                // macro1 writes the position first, am1 the character; am1 takes no shifted
                // character, so that is written as its code
                v = ((unsigned char)q[1] < 128) ? am1Concise[(unsigned char)q[1]] : -1;
                if( (v >= 0) && (v & 0200) )
                {
                    sPrintf(&s, "0%o", (v & 077) << ((q[0] == 'l') ? 12 : ((q[0] == 'm') ? 6 : 0)));
                }
                else
                {
                    sPrintf(&s, "char %c%c", q[1], q[0]);
                }
                p = q + 1;
                break;
            }
            sAddc(&s, *p);
            break;
        }
    }
    while( (s.len > 0) && ((s.bufP[s.len - 1] == ' ') || (s.bufP[s.len - 1] == '\t')) )
    {
        s.bufP[--s.len] = '\0';
    }
    while( nopen > 0 )
    {
        sAddc(&s, open[--nopen]);
    }
    free(srcP);
    return( s.bufP ? s.bufP : xstrdup("0") );
}

// The text of a site for output, or NULL with *whyP set when it is refused.
static const char *
siteText(int site, const char **whyP)
{
Site *sP;

    if( site < 0 )
    {
        *whyP = "an expression the converter did not see";
        return( NULL );
    }
    sP = &sitesP[site];
    if( sP->broken && !sP->treeP )
    {
        if( !sP->textP )
        {
            sP->textP = transliterate(sP->pos, sP->end);
            note(0, M1LINE(sP->pos), "macro1 rejects %s; written as-is", sP->textP);
        }
        return( sP->textP );
    }
    if( sP->level >= MAXLEVEL )
    {
        *whyP = sP->whyP ? sP->whyP : "an expression am1 evaluates differently from macro1";
        return( NULL );
    }
    if( sP->textVaries )
    {
        *whyP = sP->varyP;
        return( NULL );
    }
    if( !sP->textP )
    {
        *whyP = "an expression the converter did not see";
        return( NULL );
    }
    return( sP->textP );
}

// The am1 words of a text string as am1 would assemble text "s", or -1 for a character am1
// would not take.
static int
am1TextWords(const char *sP, int *wordsP, int max)
{
int codes[1024];
int n;
int shift;
int fc;
int ch;
int nw;
int i;
int v;

    n = 0;
    shift = 0;
    for( ; *sP && (n < 1000); sP++ )
    {
        ch = (unsigned char)*sP;
        fc = (ch < 128) ? am1Concise[ch] : -1;
        if( (fc < 0) || (ch == '\\') || (ch == '"') )
        {
            return( -1 );
        }
        if( (fc & 0200) && !shift )
        {
            shift = 1;
            codes[n++] = 074;
        }
        else if( (fc & 0100) && shift )
        {
            shift = 0;
            codes[n++] = 072;
        }
        codes[n++] = fc & 077;
    }
    nw = 0;
    for( i = 0; i < n; i += 3 )
    {
        v = (codes[i] << 12);
        v |= ((i + 1) < n) ? (codes[i + 1] << 6) : 0;
        v |= ((i + 2) < n) ? codes[i + 2] : 0;
        if( nw < max )
        {
            wordsP[nw] = v;
        }
        nw++;
    }
    if( n == 0 )
    {
        if( max > 0 )
        {
            wordsP[0] = 0;
        }
        nw = 1;
    }
    return( nw );
}

// Return 1 when an item lies in [p0, p1).
static int
within(Item *iP, int p0, int p1)
{
    return( (iP->pos >= p0) && (iP->pos < p1) );
}

static void emitItems(int line, int *idsP, int n, int fromCol, Str *curP, int *clsP, int *prevEndP, int stopDefine);

// Flush the line being built.
static void
flushCur(Str *curP, int *clsP)
{
    if( curP->len > 0 )
    {
        outLine(sText(curP));
    }
    sClear(curP);
    *clsP = CL_NONE;
}

// The text an item writes and how it is positione on a line.
// Multiple lines come back joined by \n
// with CL_OWNLINE.
static char *
itemText(Item *iP, int *clsP, int line)
{
Str s;
const char *tP;
const char *whyP;
char *p;
Key key;
int i;
int j;
int n;
int words[1024];
Macro *mP;
Site *sP;

    memset(&s, 0, sizeof(s));
    whyP = NULL;
    *clsP = CL_STMT;
    switch( iP->type )
    {
    case IT_EXPR:
        sP = &sitesP[iP->site];
        if( sP->kind == M1E_NOWORD )
        {
            if( sP->treeP && (sP->treeP->type == NT_TERM) && (sP->treeP->kind == M1T_EPSEUDO) )
            {
                *clsP = CL_OWNLINE;
                return( xstrdup((sP->treeP->info == M1P_DECIMAL) ? "    decimal" : "    octal") );
            }
            *clsP = CL_OWNLINE;
            note(1, line, "a pseudo-instruction used as a value");
            snprintf((char *)words, sizeof(words),
                "#error \"mactoam1: line %d: a pseudo-instruction is used as a value\"", line + 1);
            return( xstrdup((char *)words) );
        }
        if( sP->kind == M1E_LOCCHECK )
        {
            *clsP = CL_COMMENT;
            p = spanText(iP->pos, iP->end);
            sPrintf(&s, "// %s, (a location check)", p);
            free(p);
            return( s.bufP );
        }
        tP = siteText(iP->site, &whyP);
        if( !tP )
        {
            break;
        }
        sAdd(&s, tP);
        if( sP->kind == M1E_ORIGIN )
        {
            sAddc(&s, '/');
        }
        return( s.bufP );

    case IT_TAG:
        if( iP->b )
        {
            // a tag naming a dummy: the parameter, so the argument's symbol is the tag
            p = symbolSpelling(iP->pos, iP->end);
            mP = &macrosP[bodyOf(M1LINE(iP->pos))];
            for( i = 0; i < mP->nargs; i++ )
            {
                if( strncmp(mP->args[i], p, 6) == 0 )
                {
                    sAdd(&s, mP->paramP[i + 1]);
                    break;
                }
            }
            free(p);
        }
        else
        {
            key = noteSymbol(iP->pos, iP->end, 0);
            sAdd(&s, outName(key));
        }
        sAddc(&s, ',');
        *clsP = CL_TAG;
        return( s.bufP );

    case IT_NUMTAG:
        *clsP = CL_COMMENT;
        p = spanText(iP->pos, iP->end);
        sPrintf(&s, "// %s, (a location check)", p);
        free(p);
        return( s.bufP );

    case IT_ASSIGN:
        if( (iP->a > 0) && (sitesP[iP->site].level >= MAXLEVEL) )
        {
            whyP = sitesP[iP->site].whyP;
            break;
        }
        if( iP->a > 0 )
        {
            *clsP = CL_NONE;
            return( xstrdup("") );
        }
        tP = siteText(iP->site, &whyP);
        if( !tP || (iP->a == 0) )
        {
            if( !whyP )
            {
                whyP = "an assignment to R";
            }
            break;
        }
        *clsP = CL_OWNLINE;
        key = noteSymbol(iP->pos, iP->end, 0);
        sP = &sitesP[iP->site];
        if( mapGet(&definedOut, key) == 1 )
        {
            sPrintf(&s, "#undef %s\n", outName(key));
        }
        mapPut(&definedOut, key, 1);
        if( sP->treeP && (sP->treeP->type == NT_TERM) && (sP->treeP->kind == M1T_DOT) )
        {
            mapPut(&definedOut, key, 2);
        }
        if( sP->treeP && (sP->treeP->type == NT_TERM) && (sP->treeP->kind == M1T_DOT) )
        {
            sPrintf(&s, "%s,", outName(key));
        }
        else
        {
            sPrintf(&s, "#define %s (%s)", outName(key), tP);
        }
        return( s.bufP );

    case IT_INVOKE:
    case IT_EMPTYINV:
        mP = &macrosP[iP->a];
        sAdd(&s, outName(mP->key));
        sAddc(&s, '(');
        n = (iP->type == IT_INVOKE) ? iP->nlist : 0;
        for( i = 0; i < mP->nargs; i++ )
        {
            if( i > 0 )
            {
                sAdd(&s, ", ");
            }
            if( i < n )
            {
                tP = siteText(iP->listP[i], &whyP);
                if( !tP )
                {
                    break;
                }
                // an argument of more than one term is parenthesized, as parameters are
                // textual in am1
                p = (char *)tP;
                j = (strpbrk(p, " +-*&|") != NULL) && !((p[0] == '(') && (p[strlen(p) - 1] == ')'));
                if( j && (strncmp(p, "flexo ", 6) != 0) && (strncmp(p, "char ", 5) != 0) )
                {
                    sPrintf(&s, "(%s)", p);
                }
                else
                {
                    sAdd(&s, p);
                }
            }
            else
            {
                sAddc(&s, '0');
            }
        }
        if( whyP )
        {
            break;
        }
        if( n > mP->nargs )
        {
            note(0, line, "%d arguments given to %s which expect %d; the rest are dropped, macro1 ignores them",
                n, mP->name, mP->nargs);
        }
        sAddc(&s, ')');
        return( s.bufP );

    case IT_CONSTANTS:
        *clsP = CL_OWNLINE;
        if( bodyOf(line) >= 0 )
        {
            whyP = "constants inside a define body";
            break;
        }
        sAdd(&s, "    constants");
        if( iP->b != iP->c )
        {
            // macro1 leaves a word for each use; am1 one for each value
            sPrintf(&s, "\n0%o/              // macro1's location after the constants", (iP->a + iP->b) & 07777);
        }
        return( s.bufP );

    case IT_VARIABLES:
        *clsP = CL_OWNLINE;
        if( bodyOf(line) >= 0 )
        {
            whyP = "variables inside a define body";
            break;
        }
        sAdd(&s, "// variables");
        n = 0;
        for( i = iP->a; i < iP->b; )
        {
            for( j = 0; j < nVars; j++ )
            {
                if( varsP[j].addr == i )
                {
                    break;
                }
            }
            if( j == nVars )
            {
                sPrintf(&s, "\n    table %d", iP->b - i);
                break;
            }
            sPrintf(&s, "\n%s,\n    table %d", outName(varsP[j].key), varsP[j].size);
            i += varsP[j].size;
            n++;
        }
        return( s.bufP );

    case IT_TEXT:
        p = spanText(iP->pos, iP->end);
        tP = p + 4;
        while( *tP == ' ' )
        {
            tP++;
        }
        // the delimiter, then the characters up to its next appearance
        if( (M1LINE(iP->pos) == M1LINE(iP->end)) && (strlen(tP) >= 2) )
        {
            char delim;
            char body[1024];

            delim = tP[0];
            n = (int)strlen(tP);
            if( (tP[n - 1] == delim) && ((n - 2) < (int)sizeof(body)) )
            {
                memcpy(body, tP + 1, n - 2);
                body[n - 2] = '\0';
                j = am1TextWords(body, words, 1024);
                if( (j == iP->nlist) && (j > 0) )
                {
                    for( i = 0; i < j; i++ )
                    {
                        if( words[i] != iP->listP[i] )
                        {
                            break;
                        }
                    }
                    if( i == j )
                    {
                        sPrintf(&s, "text \"%s\"", body);
                        free(p);
                        return( s.bufP );
                    }
                }
            }
        }
        // otherwise the words themselves
        *clsP = CL_OWNLINE;
        for( i = 0; i < iP->nlist; i++ )
        {
            sPrintf(&s, "%s    0%06o", (i > 0) ? "\n" : "", iP->listP[i]);
            if( i == 0 )
            {
                sPrintf(&s, "         // %s", p);
            }
        }
        free(p);
        return( s.bufP );

    case IT_START:
        // macro1 continues after a start with a further program, am1 has one start, at the end.
        // Each is kept as a comment.
        p = spanText(iP->pos, iP->end);
        if( (iP - itemsP) == finalStart )
        {
            tP = siteText(iP->site, &whyP);
            if( !tP )
            {
                free(p);
                break;
            }
            startTextP = xmalloc(strlen(tP) + 16);
            sprintf(startTextP, "    start %s", tP);
            sPrintf(&s, "// %s (written at the end)", p);
        }
        else if( iP->a )
        {
            note(0, line, "a start that is not the last");
            sPrintf(&s, "// %s (not the last start)", p);
        }
        else
        {
            sPrintf(&s, "// %s", p);
        }
        free(p);
        *clsP = CL_COMMENT;
        return( s.bufP );

    case IT_COMMENT:
        *clsP = CL_COMMENT;
        p = lineSlice(M1LINE(iP->pos), M1COL(iP->pos) + 1, M1MAXCOL);
        sPrintf(&s, "//%s", p);
        free(p);
        return( s.bufP );

    case IT_PSEUDO:
        *clsP = CL_COMMENT;
        if( iP->a == M1P_NOINPUT )
        {
            return( xstrdup("// noinput: assemble with am1 -r for a tape with no loader") );
        }
        p = spanText(iP->pos, iP->end);
        sPrintf(&s, "// %s: placed with the variables", p);
        free(p);
        return( s.bufP );

    default:
        *clsP = CL_NONE;
        return( xstrdup("") );
    }

    // refused: whatever was begun is dropped
    *clsP = CL_OWNLINE;
    note(1, line, "%s", whyP);
    sClear(&s);
    sPrintf(&s, "#error \"mactoam1: line %d: %s\"", line + 1, quoteSafe(whyP));
    return( s.bufP );
}

// Write lines of an own-line block, the first at the given indent.
static void
outBlock(const char *textP)
{
const char *p;
const char *q;
char *lineP;

    for( p = textP; *p; p = q + ((*q == '\n') ? 1 : 0) )
    {
        q = strchr(p, '\n');
        if( !q )
        {
            q = p + strlen(p);
        }
        lineP = xmalloc(q - p + 1);
        memcpy(lineP, p, q - p);
        lineP[q - p] = '\0';
        outLine(lineP);
        free(lineP);
        if( !*q )
        {
            break;
        }
    }
}

// Compare items by position.
static int
cmpItems(const void *aP, const void *bP)
{
const Item *a;
const Item *b;

    a = &itemsP[*(const int *)aP];
    b = &itemsP[*(const int *)bP];
    if( a->pos != b->pos )
    {
        return( (a->pos < b->pos) ? -1 : 1 );
    }
    return( (a->type < b->type) ? -1 : ((a->type > b->type) ? 1 : 0) );
}

// Write a line's items, from column fromCol, into cur.
// With stopDefine, stop at a define.
static void
emitItems(int line, int *idsP, int n, int fromCol, Str *curP, int *clsP, int *prevEndP, int stopDefine)
{
int k;
int r;
int cls;
int c0;
int c1;
int nested;
int count;
int *subP;
int nsub;
int subCls;
int subPrev;
char *textP;
char *gapP;
char *p;
Item *iP;
Item *rP;
Str sub;
Site *sP;
Key key;

    for( k = 0; k < n; k++ )
    {
        iP = &itemsP[idsP[k]];
        if( M1COL(iP->pos) < fromCol )
        {
            continue;
        }
        if( (iP->type == IT_DEFINE) && stopDefine )
        {
            return;
        }

        c0 = M1COL(iP->pos);
        if( iP->type == IT_REPEAT )
        {
            // The statements after repeat n, written n times, each on a line of its own.
            textP = NULL;
            if( iP->d )
            {
                flushCur(curP, clsP);
                outError(line, "a repeat whose count differs from one use to the next");
            }
            subP = xmalloc((n + 1) * sizeof(int));
            nsub = 0;
            nested = 0;
            for( r = k + 1; r < n; r++ )
            {
                rP = &itemsP[idsP[r]];
                if( within(rP, iP->b, iP->c) )
                {
                    if( (rP->type == IT_TAG) || (rP->type == IT_NUMTAG) )
                    {
                        nested = 1;
                    }
                    subP[nsub++] = idsP[r];
                }
            }
            flushCur(curP, clsP);
            if( nested )
            {
                outError(line, "a tag inside a repeat");
            }
            else if( (iP->site >= 0) && treeHasSpace(sitesP[iP->site].treeP) )
            {
                // repeat 3 lac x, with no comma: macro1 adds lac x to the count
                outError(line, "a repeat count with a space in it, which macro1 adds to the count");
            }
            else if( !iP->d )
            {
                p = spanText(iP->pos, iP->b);
                memset(&sub, 0, sizeof(sub));
                sPrintf(&sub, "// %s", p);
                free(p);
                padTo(curP, visualCol(line, c0));
                sAdd(curP, sText(&sub));
                flushCur(curP, clsP);
                for( count = 0; count < iP->a; count++ )
                {
                    sClear(&sub);
                    subCls = CL_NONE;
                    subPrev = M1COL(iP->b);
                    padTo(&sub, visualCol(line, M1COL(iP->b)));
                    emitItems(line, subP, nsub, M1COL(iP->b), &sub, &subCls, &subPrev, 0);
                    flushCur(&sub, &subCls);
                }
                free(sub.bufP);
            }
            free(subP);
            *prevEndP = M1MAXCOL;
            // the repeated items are done
            k = n;
            continue;
        }

        textP = itemText(iP, &cls, line);
        c1 = M1COL(iP->end);
        if( iP->type == IT_TAG )
        {
            c1++;
        }
        gapP = lineSlice(line, *prevEndP, c0);
        for( p = gapP; *p; p++ )
        {
            if( (*p != ' ') && (*p != '\t') )
            {
                *p = ' ';
            }
        }

        switch( cls )
        {
        case CL_NONE:
            break;

        case CL_OWNLINE:
            flushCur(curP, clsP);
            outBlock(textP);
            *clsP = CL_OWNLINE;
            break;

        case CL_TAG:
            if( (*clsP == CL_TAG) || (*clsP == CL_STMT) || (*clsP == CL_COMMENT) )
            {
                flushCur(curP, clsP);
            }
            key = (iP->type == IT_TAG) && !iP->b ? noteSymbol(iP->pos, iP->end, 0) : 0;
            if( key && (mapGet(&definedOut, key) == 1) )
            {
                // macro1 lets a tag take a name = gave; am1's #define must go first
                flushCur(curP, clsP);
                p = xmalloc(strlen(outName(key)) + 16);
                sprintf(p, "#undef %s", outName(key));
                outLine(p);
                free(p);
                mapPut(&definedOut, key, 2);
            }
            if( (*clsP == CL_OWNLINE) || (curP->len == 0) )
            {
                sClear(curP);
                padTo(curP, visualCol(line, c0));
            }
            else
            {
                sAdd(curP, gapP);
            }
            sAdd(curP, textP);
            *clsP = CL_TAG;
            break;

        case CL_STMT:
            sP = (iP->type == IT_EXPR) ? &sitesP[iP->site] : NULL;
            if( *clsP == CL_COMMENT )
            {
                // a comment written for an earlier statement, a location check
                flushCur(curP, clsP);
            }
            if( sP && (sP->preRadix || (sP->kind == M1E_ORIGIN)) && (*clsP == CL_STMT) )
            {
                flushCur(curP, clsP);
            }
            if( sP && sP->preRadix )
            {
                outLine(((sP->preRadix - 1) == M1P_DECIMAL) ? "    decimal" : "    octal");
            }
            if( sP && (sP->kind == M1E_ORIGIN) && (curP->len > 0) )
            {
                // am1 takes an origin only first on its line
                flushCur(curP, clsP);
            }
            if( *clsP == CL_STMT )
            {
                sAddc(curP, ';');
                sAdd(curP, (*gapP) ? gapP : " ");
            }
            else if( (*clsP == CL_OWNLINE) || (curP->len == 0) )
            {
                sClear(curP);
                padTo(curP, visualCol(line, c0));
            }
            else
            {
                sAdd(curP, (*gapP || (*clsP != CL_TAG)) ? gapP : " ");
            }
            sAdd(curP, textP);
            *clsP = CL_STMT;
            if( sP && (sP->kind == M1E_ORIGIN) )
            {
                // what follows an origin needs no ;
                *clsP = CL_TAG;
            }
            if( sP && sP->postRadix )
            {
                flushCur(curP, clsP);
                outLine(((sP->postRadix - 1) == M1P_DECIMAL) ? "    decimal" : "    octal");
            }
            break;

        case CL_COMMENT:
            if( (*clsP == CL_OWNLINE) || (curP->len == 0) )
            {
                sClear(curP);
                padTo(curP, visualCol(line, c0));
            }
            else
            {
                sAdd(curP, (*gapP) ? gapP : " ");
            }
            sAdd(curP, textP);
            *clsP = CL_COMMENT;
            break;
        }
        free(gapP);
        free(textP);
        *prevEndP = (c1 > *prevEndP) ? c1 : *prevEndP;
    }
}

// The items of each line, sorted.
static void
sortItems(void)
{
int i;
int line;

    lineItemsP = xmalloc(m1LineCount * sizeof(int *));
    lineNItemsP = xmalloc(m1LineCount * sizeof(int));
    for( i = 0; i < nItems; i++ )
    {
        line = M1LINE(itemsP[i].pos);
        if( (line >= 0) && (line < m1LineCount) )
        {
            lineItemsP[line] = xrealloc(lineItemsP[line], (lineNItemsP[line] + 1) * sizeof(int));
            lineItemsP[line][lineNItemsP[line]++] = i;
        }
    }
    for( line = 0; line < m1LineCount; line++ )
    {
        if( lineNItemsP[line] > 1 )
        {
            qsort(lineItemsP[line], lineNItemsP[line], sizeof(int), cmpItems);
        }
    }
}

// Return 1 when a source line is blank.
static int
isBlank(int line)
{
const char *p;

    for( p = m1Lines[line]; *p; p++ )
    {
        if( (*p != ' ') && (*p != '\t') )
        {
            return( 0 );
        }
    }
    return( 1 );
}

// The comment at the end of a define or terminate line, from its first / on, or "".
static char *
lineTailComment(int line, int fromCol)
{
const char *p;
const char *q;
Str s;

    memset(&s, 0, sizeof(s));
    p = m1Lines[line];
    if( fromCol > (int)strlen(p) )
    {
        return( xstrdup("") );
    }
    q = strchr(p + fromCol, '/');
    if( q )
    {
        sPrintf(&s, "    //%s", q + 1);
    }
    return( s.bufP ? s.bufP : xstrdup("") );
}

// Write the #define line of a macro and its local declarations.
static void
emitDefineHead(int mac, int line)
{
Macro *mP;
Str s;
int i;
int any;
char *cP;
const char *p;

    mP = &macrosP[mac];
    memset(&s, 0, sizeof(s));
    sPrintf(&s, "#define %s(", outName(mP->key));
    for( i = 1; i <= mP->nargs; i++ )
    {
        sPrintf(&s, "%s%s", (i > 1) ? "," : "", mP->paramP[i]);
    }
    sAddc(&s, ')');
    // the comment of the define line, skipping the names
    p = m1Lines[mP->nameLine];
    cP = lineTailComment(mP->nameLine, (int)(strstr(p, mP->name) ? (strstr(p, mP->name) - p) : 0));
    sAdd(&s, cP);
    free(cP);
    if( mP->nameLine != line )
    {
        cP = lineTailComment(line, 0);
        if( *cP )
        {
            outLine(cP + 4);
        }
        free(cP);
    }
    outLine(sText(&s));
    inBody = 1;
    sClear(&s);
    any = 0;
    if( mP->usesR || (mP->ntags > 0) )
    {
        sAdd(&s, "    local ");
        if( mP->usesR )
        {
            sAdd(&s, "R");
            any = 1;
        }
        for( i = 0; i < mP->ntags; i++ )
        {
            sPrintf(&s, "%s%s", any ? ", " : "", outName(mP->tagsP[i]));
            any = 1;
        }
        outLine(sText(&s));
    }
    if( mP->usesR )
    {
        outLine("R,");
    }
    free(s.bufP);
}

// Write the whole conversion.
static void
emitAll(void)
{
int line;
int firstTitle;
int mac;
int cls;
int prevEnd;
int i;
char *p;
const char *baseP;
Str cur;
Str s;

    sortItems();
    memset(&cur, 0, sizeof(cur));
    memset(&s, 0, sizeof(s));

    firstTitle = -1;
    for( line = 0; line < m1LineCount; line++ )
    {
        if( lineRoleP[line] == LR_TITLE )
        {
            firstTitle = line;
            break;
        }
    }
    if( firstTitle >= 0 )
    {
        outLine(m1Lines[firstTitle]);
    }
    else
    {
        outLine("untitled");
    }
    baseP = strrchr(srcNameP, '/');
    sPrintf(&s, "// converted from macro1 by mactoam1: %s", baseP ? (baseP + 1) : srcNameP);
    outLine(sText(&s));
    if( needXX && (mapGet(&defMap, keyOfName("xx")) < 0) )
    {
        outLine("#define xx (0760400)      // macro1's xx, hlt");
    }
    if( needSkip && (mapGet(&defMap, keyOfName("skip")) < 0) )
    {
        outLine("#define skip (0640000)    // macro1's skip, skp");
    }
    if( needClc && (mapGet(&defMap, keyOfName("clc")) < 0) )
    {
        outLine("#define clc (0761200)     // macro1's clc, cla cma");
    }

    // am1 is given the last start with an address; code after it is loaded first.
    finalStart = -1;
    for( i = 0; i < nItems; i++ )
    {
        if( (itemsP[i].type == IT_START) && itemsP[i].a &&
            ((finalStart < 0) || (itemsP[i].pos > itemsP[finalStart].pos)) )
        {
            finalStart = i;
        }
    }
    if( finalStart >= 0 )
    {
        for( i = 0; i < nItems; i++ )
        {
            if( (itemsP[i].type == IT_EXPR) && (itemsP[i].pos > itemsP[finalStart].pos) &&
                (sitesP[itemsP[i].site].kind == M1E_WORD) )
            {
                note(0, M1LINE(itemsP[finalStart].pos), "code after start, macro1 punches it;"
                    " am1 writes start at the end");
                break;
            }
        }
    }

    for( line = 0; line < m1LineCount; line++ )
    {
        if( (line == firstTitle) || (line < firstTitle) )
        {
            continue;
        }
        switch( lineRoleP[line] )
        {
        case LR_TITLE:
            sClear(&s);
            sPrintf(&s, "// %s", m1Lines[line]);
            outLine(sText(&s));
            continue;

        case LR_DEFNAME:
        case LR_TEXTCONT:
            continue;

        case LR_DEFHEAD:
            mac = lineMacP[line];
            cls = CL_NONE;
            prevEnd = 0;
            emitItems(line, lineItemsP[line], lineNItemsP[line], 0, &cur, &cls, &prevEnd, 1);
            flushCur(&cur, &cls);
            for( i = 0; i < lineNItemsP[line]; i++ )
            {
                if( (itemsP[lineItemsP[line][i]].type == IT_DEFINE) && itemsP[lineItemsP[line][i]].whyP )
                {
                    outError(line, itemsP[lineItemsP[line][i]].whyP);
                }
            }
            if( (mac >= 0) && (mac < nMacros) && macrosP[mac].defined )
            {
                emitDefineHead(mac, line);
            }
            continue;

        case LR_TERM:
            mac = lineMacP[line];
            if( (macrosP[mac].usesR || (macrosP[mac].ntags > 0)) )
            {
                outLine("    endloc");
            }
            p = lineTailComment(line, 0);
            sClear(&s);
            sPrintf(&s, "#enddef%s", p);
            free(p);
            outLine(sText(&s));
            endBody();
            continue;
        }

        // code, or a body line
        if( (lineNItemsP[line] == 0) && !isBlank(line) && (lineTextColP[line] == 0) )
        {
            outError(line, "a line macro1 never assembles");
            continue;
        }
        cls = CL_NONE;
        prevEnd = lineTextColP[line];
        if( lineNItemsP[line] == 0 )
        {
            outLine("");
            continue;
        }
        emitItems(line, lineItemsP[line], lineNItemsP[line], lineTextColP[line], &cur, &cls, &prevEnd, 0);
        flushCur(&cur, &cls);
    }

    if( startTextP )
    {
        outLine(startTextP);
    }
    else
    {
        note(0, m1LineCount - 1, "no start; written as stop");
        outLine("    stop");
    }
    free(cur.bufP);
    free(s.bufP);
}

// Make a Unicode overline (U+203E, or the combining U+0305) on a symbol macro1's overbar, ~.
static void
fixOverlines(char *lineP)
{
char *p;
char *q;
int n;

    for( p = q = lineP; *p; )
    {
        n = 0;
        if( ((unsigned char)p[0] == 0xe2) && ((unsigned char)p[1] == 0x80) && ((unsigned char)p[2] == 0xbe) )
        {
            n = 3;
        }
        else if( ((unsigned char)p[0] == 0xcc) && ((unsigned char)p[1] == 0x85) )
        {
            n = 2;
        }
        if( n && (((q > lineP) && isalnum((unsigned char)q[-1])) || isalnum((unsigned char)p[n])) )
        {
            *q++ = '~';
            p += n;
            continue;
        }
        *q++ = *p++;
    }
    *q = '\0';
}

// Read the source into lines: LF, CRLF and CR alike end a line; form feeds are dropped.
static void
loadSource(const char *pathP)
{
FILE *fP;
char *bufP;
long size;
long i;
int cap;
Str s;

    fP = fopen(pathP, "rb");
    if( !fP )
    {
        fprintf(stderr, "mactoam1: cannot open %s\n", pathP);
        exit(1);
    }
    fseek(fP, 0, SEEK_END);
    size = ftell(fP);
    fseek(fP, 0, SEEK_SET);
    bufP = xmalloc(size + 1);
    if( fread(bufP, 1, size, fP) != (size_t)size )
    {
        fprintf(stderr, "mactoam1: cannot read %s\n", pathP);
        exit(1);
    }
    fclose(fP);

    memset(&s, 0, sizeof(s));
    cap = 0;
    m1LineCount = 0;
    for( i = 0; i <= size; i++ )
    {
        if( (i == size) || (bufP[i] == '\n') || (bufP[i] == '\r') )
        {
            if( (i == size) && (s.len == 0) )
            {
                break;
            }
            if( m1LineCount == cap )
            {
                cap = (2 * cap) + 256;
                m1Lines = xrealloc(m1Lines, cap * sizeof(char *));
            }
            m1Lines[m1LineCount] = s.bufP ? xstrdup(s.bufP) : xstrdup("");
            fixOverlines(m1Lines[m1LineCount++]);
            sClear(&s);
            if( (i < size) && (bufP[i] == '\r') && ((i + 1) < size) && (bufP[i + 1] == '\n') )
            {
                i++;
            }
            continue;
        }
        if( bufP[i] != '\f' )
        {
            sAddc(&s, bufP[i]);
        }
    }
    free(bufP);
    free(s.bufP);
    if( m1LineCount == 0 )
    {
        m1Lines = xmalloc(sizeof(char *));
        m1Lines[0] = xstrdup("");
        m1LineCount = 1;
    }
    lineRoleP = xmalloc(m1LineCount * sizeof(int));
    lineMacP = xmalloc(m1LineCount * sizeof(int));
    lineTextColP = xmalloc(m1LineCount * sizeof(int));
    for( i = 0; i < m1LineCount; i++ )
    {
        lineMacP[i] = -1;
    }
}

// Reset what pass 3 builds up, for another round.
static void
resetRound(void)
{
int i;

    nDefs = 0;
    mapClear(&defMap);
    mapClear(&seenMap);
    nLitUses = 0;
    poolNo = 0;
    frameTopP = NULL;
    pendingP = NULL;
    for( i = 0; i < nSites; i++ )
    {
        sitesP[i].nEval = 0;
        sitesP[i].varies = 0;
        sitesP[i].textVaries = 0;
    }
    buildReset();
}

// Print the notes, in line order.
static int
cmpNotes(const void *aP, const void *bP)
{
const Note *a;
const Note *b;

    a = aP;
    b = bP;
    return( (a->line < b->line) ? -1 : ((a->line > b->line) ? 1 : 0) );
}

int
main(int argc, char *argv[])
{
FILE *outP;
int i;
int mac;

    outPathP = NULL;
    srcNameP = NULL;
    for( i = 1; i < argc; i++ )
    {
        if( (strcmp(argv[i], "-o") == 0) && ((i + 1) < argc) )
        {
            outPathP = argv[++i];
        }
        else if( (strcmp(argv[i], "-t") == 0) && ((i + 1) < argc) )
        {
            tabWidth = atoi(argv[++i]);
            if( (tabWidth < 1) || (tabWidth > 32) )
            {
                fprintf(stderr, "mactoam1: -t takes a width from 1 to 32\n");
                return( 2 );
            }
        }
        else if( (argv[i][0] == '-') && argv[i][1] )
        {
            fprintf(stderr, "usage: mactoam1 [-o outfile] [-t tabwidth] file.mac\n");
            return( 2 );
        }
        else
        {
            srcNameP = argv[i];
        }
    }
    if( !srcNameP )
    {
        fprintf(stderr, "usage: mactoam1 [-o outfile] [-t tabwidth] file.mac\n");
        return( 2 );
    }

    loadSource(srcNameP);
    m1Init();
    m1Pass(1);
    curPass = 2;
    m1Pass(2);
    for( mac = 0; mac < nMacros; mac++ )
    {
        if( macrosP[mac].defined && !macrosP[mac].invoked )
        {
            m1Phantom(mac);
        }
    }

    // Pass 3 until no expression changes how it is written.
    for( round3 = 0; round3 < MAXPASS3; round3++ )
    {
        changed = 0;
        resetRound();
        curPass = 3;
        m1Pass(3);
        litSettle();
        frameTopP = NULL;
        pendingP = NULL;
        curPass = 4;
        for( mac = 0; mac < nMacros; mac++ )
        {
            if( macrosP[mac].defined && !macrosP[mac].invoked )
            {
                m1Phantom(mac);
            }
        }
        if( !changed )
        {
            break;
        }
    }
    if( changed )
    {
        note(1, -1, "the conversion did not resolve; some expressions may be refused");
    }

    emitAll();

    outP = stdout;
    if( outPathP )
    {
        outP = fopen(outPathP, "w");
        if( !outP )
        {
            fprintf(stderr, "mactoam1: cannot write %s\n", outPathP);
            return( 1 );
        }
    }
    for( i = 0; i < nOut; i++ )
    {
        fprintf(outP, "%s\n", outLinesP[i]);
    }
    if( outP != stdout )
    {
        fclose(outP);
    }

    qsort(notesP, nNotes, sizeof(Note), cmpNotes);
    for( i = 0; i < nNotes; i++ )
    {
        if( notesP[i].line >= 0 )
        {
            fprintf(stderr, "%s:%d: %s: %s\n", srcNameP, notesP[i].line + 1,
                notesP[i].error ? "error" : "warning", notesP[i].msgP);
        }
        else
        {
            fprintf(stderr, "%s: %s: %s\n", srcNameP, notesP[i].error ? "error" : "warning", notesP[i].msgP);
        }
    }
    return( (nErrors > 0) ? 1 : 0 );
}
