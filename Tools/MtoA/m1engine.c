// m1engine.c - macro1's assembly passes, reporting what they do instead of writing a tape.
//
// The parsing and evaluation here are those of src/macro/macro1_1.c (Gary A. Messenbrink,
// Bob Supnik, Phil Budne), kept close to it on purpose.
// mactoam1 must reproduce the program as macro1 would, quirks included, so every macro1 rule decides a value or a
// location unchanged.
// Its listing, tape and file handling are left out.
// The hooks in m1engine.h tell mactoam1.c what each step evaluated and where in the source it
// came from.
// linePos[] is the source positions for every character of the line being assembled so a macro body's text
// still corresponds to the source line it was written on.
//
// The departures from macro1 are listed in m1engine.h.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m1engine.h"

#define LINELEN             1024
#define SYMLEN              M1SYMLEN
#define SYMBOL_TABLE_SIZE   8192
#define MAC_MAX_ARGS        M1MAXARGS
#define MAC_TABLE_LENGTH    1024
#define MAX_LITERALS        200000
#define MAX_MACRO_DEPTH     200
#define MAX_CONSTANTS       100

#define ADDRESS_FIELD       07777
#define CONCISE_LC          072
#define CONCISE_UC          074

#define DIM(a)              (sizeof(a) / sizeof(a[0]))
#define ISBLANK(c)          (((c) == ' ') || ((c) == '\f'))
#define ISEND(c)            (((c) == '\0') || ((c) == '\n') || ((c) == '\t'))
#define ISDONE(c)           (((c) == '/') || ISEND(c))
#define ISOVERBAR(c)        (((c) == '\\') || ((c) == '~'))

// Symbol types, macro1's.
#define UNDEFINED   0000
#define DEFINED     0001
#define FIXED       0002
#define LABEL       (0010 | DEFINED)
#define REDEFINED   (0020 | DEFINED)
#define DUPLICATE   (0040 | DEFINED)
#define PSEUDO      (0100 | FIXED | DEFINED)
#define EPSEUDO     (0200 | FIXED | DEFINED)
#define MACRO       (0400 | DEFINED)
#define DEFFIX      (DEFINED | FIXED)
#define NOTRDEF     ((MACRO | PSEUDO | LABEL | FIXED) & ~DEFINED)

#define M_DEFINED(s)    (((s) & DEFINED) == DEFINED)
#define M_FIXED(s)      (((s) & FIXED) == FIXED)
#define M_LABEL(s)      (((s) & LABEL) == LABEL)
#define M_PSEUDO(s)     (((s) & PSEUDO) == PSEUDO)
#define M_EPSEUDO(s)    (((s) & EPSEUDO) == EPSEUDO)
#define M_MACRO(s)      (((s) & MACRO) == MACRO)
#define M_NOTRDEF(s)    (((s) & NOTRDEF) != 0)

// Flags of this file's own, beside macro1's type.
#define SF_VAR      01      // a variable: overbarred, or dimensioned

// Character codes, macro1's table (from SIMH pdp1_stddev.c).
#define UC          0100
#define LC          0200
#define CHARBITS    077
#define BC          (LC | UC)
#define BAD         014

typedef struct
{
    int type;
    char name[SYMLEN];
    int val;
    int flags;
} Sym;

typedef struct
{
    int nargs;
    Sym args[MAC_MAX_ARGS + 1];     // args[0] is R, the location the expansion starts at
    char *bodyP;
    int *bodyPosP;                  // the source position of each character of the body
} MacDef;

typedef struct MacInv
{
    char line[LINELEN];             // the line the macro was used on, to go back to
    int linePos[LINELEN];
    int cc;
    char *ptrP;                     // the next body character
    int *posPtrP;
    MacDef *defnP;
    char argName[MAC_MAX_ARGS + 1][SYMLEN];     // an argument that is one symbol, by name
    struct MacInv *prevP;
} MacInv;

// The state pass 2 starts in, kept so that pass 3 can start from it again.
typedef struct
{
    Sym *symtabP;
    int symbolTop;
    int vars_addr;
    int noinput;
} Snapshot;

char **m1Lines;
int m1LineCount;

static Sym pseudos[] =
{
    { PSEUDO,  "consta", M1P_CONSTANTS, 0 },
    { PSEUDO,  "define", M1P_DEFINE, 0 },
    { PSEUDO,  "repeat", M1P_REPEAT, 0 },
    { PSEUDO,  "start",  M1P_START, 0 },
    { PSEUDO,  "variab", M1P_VARIABLES, 0 },
    { PSEUDO,  "text",   M1P_TEXT, 0 },
    { PSEUDO,  "noinpu", M1P_NOINPUT, 0 },
    { PSEUDO,  "expung", M1P_EXPUNGE, 0 },
    { EPSEUDO, "charac", M1P_CHAR, 0 },
    { EPSEUDO, "decima", M1P_DECIMAL, 0 },
    { EPSEUDO, "flexo",  M1P_FLEX, 0 },
    { EPSEUDO, "octal",  M1P_OCTAL, 0 },
};

static Sym permanentSymbols[] =
{
    { DEFFIX, "and",  0020000, 0 }, { DEFFIX, "ior",  0040000, 0 },
    { DEFFIX, "xor",  0060000, 0 }, { DEFFIX, "xct",  0100000, 0 },
    { DEFFIX, "lac",  0200000, 0 }, { DEFFIX, "lio",  0220000, 0 },
    { DEFFIX, "dac",  0240000, 0 }, { DEFFIX, "dap",  0260000, 0 },
    { DEFFIX, "dip",  0300000, 0 }, { DEFFIX, "dio",  0320000, 0 },
    { DEFFIX, "dzm",  0340000, 0 }, { DEFFIX, "add",  0400000, 0 },
    { DEFFIX, "sub",  0420000, 0 }, { DEFFIX, "idx",  0440000, 0 },
    { DEFFIX, "isp",  0460000, 0 }, { DEFFIX, "sad",  0500000, 0 },
    { DEFFIX, "sas",  0520000, 0 }, { DEFFIX, "mul",  0540000, 0 },
    { DEFFIX, "mus",  0540000, 0 }, { DEFFIX, "div",  0560000, 0 },
    { DEFFIX, "dis",  0560000, 0 }, { DEFFIX, "jmp",  0600000, 0 },
    { DEFFIX, "jsp",  0620000, 0 }, { DEFFIX, "skip", 0640000, 0 },
    { DEFFIX, "cal",  0160000, 0 }, { DEFFIX, "jda",  0170000, 0 },
    { DEFFIX, "i",    0010000, 0 }, { DEFFIX, "skp",  0640000, 0 },
    { DEFFIX, "law",  0700000, 0 }, { DEFFIX, "iot",  0720000, 0 },
    { DEFFIX, "opr",  0760000, 0 }, { DEFFIX, "nop",  0760000, 0 },
    { DEFFIX, "ral",  0661000, 0 }, { DEFFIX, "ril",  0662000, 0 },
    { DEFFIX, "rcl",  0663000, 0 }, { DEFFIX, "sal",  0665000, 0 },
    { DEFFIX, "sil",  0666000, 0 }, { DEFFIX, "scl",  0667000, 0 },
    { DEFFIX, "rar",  0671000, 0 }, { DEFFIX, "rir",  0672000, 0 },
    { DEFFIX, "rcr",  0673000, 0 }, { DEFFIX, "sar",  0675000, 0 },
    { DEFFIX, "sir",  0676000, 0 }, { DEFFIX, "scr",  0677000, 0 },
    { DEFFIX, "1s",   0000001, 0 }, { DEFFIX, "2s",   0000003, 0 },
    { DEFFIX, "3s",   0000007, 0 }, { DEFFIX, "4s",   0000017, 0 },
    { DEFFIX, "5s",   0000037, 0 }, { DEFFIX, "6s",   0000077, 0 },
    { DEFFIX, "7s",   0000177, 0 }, { DEFFIX, "8s",   0000377, 0 },
    { DEFFIX, "9s",   0000777, 0 }, { DEFFIX, "sza",  0640100, 0 },
    { DEFFIX, "spa",  0640200, 0 }, { DEFFIX, "sma",  0640400, 0 },
    { DEFFIX, "szo",  0641000, 0 }, { DEFFIX, "spi",  0642000, 0 },
    { DEFFIX, "szs",  0640000, 0 }, { DEFFIX, "szf",  0640000, 0 },
    { DEFFIX, "clf",  0760000, 0 }, { DEFFIX, "stf",  0760010, 0 },
    { DEFFIX, "cla",  0760200, 0 }, { DEFFIX, "hlt",  0760400, 0 },
    { DEFFIX, "xx",   0760400, 0 }, { DEFFIX, "cma",  0761000, 0 },
    { DEFFIX, "clc",  0761200, 0 }, { DEFFIX, "lat",  0762200, 0 },
    { DEFFIX, "cli",  0764000, 0 }, { DEFFIX, "rpa",  0730001, 0 },
    { DEFFIX, "rpb",  0730002, 0 }, { DEFFIX, "rrb",  0720030, 0 },
    { DEFFIX, "ppa",  0730005, 0 }, { DEFFIX, "ppb",  0730006, 0 },
    { DEFFIX, "tyo",  0730003, 0 }, { DEFFIX, "tyi",  0720004, 0 },
    { DEFFIX, "dpy",  0730007, 0 }, { DEFFIX, "lsm",  0720054, 0 },
    { DEFFIX, "esm",  0720055, 0 }, { DEFFIX, "cbs",  0720056, 0 },
    { DEFFIX, "lem",  0720074, 0 }, { DEFFIX, "eem",  0724074, 0 },
    { DEFFIX, "cks",  0720033, 0 },
};

static unsigned char asciiToFiodec[128] =
{
    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,
    BC|075, BC|036, BAD,    BAD,    BAD,    BC|077, BAD,    BAD,
    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,
    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,    BAD,
    BC|000, UC|005, UC|001, UC|004, BAD,    BAD,    UC|006, UC|002,
    LC|057, LC|055, UC|073, UC|054, LC|033, LC|054, LC|073, LC|021,
    LC|020, LC|001, LC|002, LC|003, LC|004, LC|005, LC|006, LC|007,
    LC|010, LC|011, BAD,    BAD,    UC|007, UC|033, UC|010, UC|021,
    LC|040, UC|061, UC|062, UC|063, UC|064, UC|065, UC|066, UC|067,
    UC|070, UC|071, UC|041, UC|042, UC|043, UC|044, UC|045, UC|046,
    UC|047, UC|050, UC|051, UC|022, UC|023, UC|024, UC|025, UC|026,
    UC|027, UC|030, UC|031, UC|057, LC|056, UC|055, UC|011, UC|040,
    UC|020, LC|061, LC|062, LC|063, LC|064, LC|065, LC|066, LC|067,
    LC|070, LC|071, LC|041, LC|042, LC|043, LC|044, LC|045, LC|046,
    LC|047, LC|050, LC|051, LC|022, LC|023, LC|024, LC|025, LC|026,
    LC|027, LC|030, LC|031, BAD,    UC|056, BAD,    UC|003, BC|075
};

static Sym *symtabP;
static int symbolTop;
static Sym symUndefined;

static MacDef *macDefs[MAC_TABLE_LENGTH];
static int macCount;
static MacInv *curMacroP;
static int nrepeats;

static char line[LINELEN];
static int linePos[LINELEN + 1];
static int nextLine;                // the source line readLine() reads next
static int cc;
static int maxcc;
static int clc;
static int endOfInput;
static int errorInLine;
static int lexstartprev;
static int lexstart;
static int lexterm;
static int overbar;
static int titleSet;

static int nconst;
static int litCount[MAX_CONSTANTS];
static int litLoc[MAX_CONSTANTS];
static int nlit;
static int litter[MAX_LITERALS];
static int noinput;
static int nvars;
static int varsAddr;
static int pass;
static int radix;

static Snapshot pass2Start;

static void processLine(void);
static void next(int op);
static Sym getExpr(void);
static int getExprs(void);
static void readLine(void);

// Report an error at a column of the current line.
// Macro1 reports errors on pass 2 only.
static void
errorAt(const char *msgP, int col)
{
    if( pass >= 2 )
    {
        hError(pass, linePos[col], msgP);
    }
    errorInLine = 1;
}

// Make the name of the lexeme from..term, as macro1 keys it: overbars dropped, 6 characters.
static char *
lexemeToName(char *nameP, int from, int term)
{
int to;
char c;

    to = 0;
    while( (from < term) && (to < (SYMLEN - 1)) )
    {
        c = line[from++];
        if( ISOVERBAR(c) )
        {
            continue;
        }
        nameP[to++] = c;
    }
    nameP[to] = '\0';
    return( nameP );
}

// Find a symbol, entering it as undefined when it is not there.
// A miss on a lookup (type UNDEFINED) returns instead a pseudo-instruction or macro sharing the first 3 characters
// as macro1 does.
// The current macro's dummies come first.
static Sym *
lookup(const char *nameP, int type)
{
int ix;
int lx;
int rx;
int mx;
int compare;
int i;
Sym *bestP;
Sym *symP;
MacDef *mdP;

    if( curMacroP && curMacroP->defnP )
    {
        mdP = curMacroP->defnP;
        for( i = 0; i <= mdP->nargs; i++ )
        {
            if( strcmp(nameP, mdP->args[i].name) == 0 )
            {
                return( &mdP->args[i] );
            }
        }
    }

    lx = 0;
    rx = symbolTop - 1;
    bestP = NULL;
    while( lx <= rx )
    {
        mx = (lx + rx) / 2;
        symP = symtabP + mx;
        compare = strcmp(nameP, symP->name);
        if( compare < 0 )
        {
            rx = mx - 1;
        }
        else if( compare > 0 )
        {
            lx = mx + 1;
        }
        else
        {
            // A variable is placed when pass 2 first meets it overbarred.
            if( overbar && !M_DEFINED(symP->type) && (pass >= 2) )
            {
                symP->type = DEFINED;
                symP->flags |= SF_VAR;
                symP->val = varsAddr++;
                nvars++;
                hVarAlloc(pass, symP->name, symP->val, 1);
            }
            return( symP );
        }

        if( (M_PSEUDO(symP->type) || M_EPSEUDO(symP->type) || M_MACRO(symP->type)) &&
            (strncmp(nameP, symP->name, 3) == 0) )
        {
            bestP = symP;
        }
    }

    if( bestP && (type == UNDEFINED) )
    {
        return( bestP );
    }

    ix = lx;
    if( (symbolTop + 1) >= SYMBOL_TABLE_SIZE )
    {
        fprintf(stderr, "mactoam1: symbol table full\n");
        exit(1);
    }
    for( rx = symbolTop; rx >= ix; rx-- )
    {
        symtabP[rx + 1] = symtabP[rx];
    }
    symbolTop++;

    symP = symtabP + ix;
    strcpy(symP->name, nameP);
    symP->type = UNDEFINED;
    symP->val = 0;
    symP->flags = 0;
    if( overbar )
    {
        nvars++;
    }
    return( symP );
}

// Give a symbol a value, entering it if needed.
// On pass 2 a tag, macro or permanent symbol keeps its value when given a different one,
// with an error, as in macro1.
static Sym *
defineSymbol(const char *nameP, int val, int type, int col)
{
Sym *symP;

    if( strlen(nameP) < 1 )
    {
        return( &symUndefined );
    }
    symP = lookup(nameP, type);
    if( M_DEFINED(symP->type) && (symP->val != val) && M_NOTRDEF(symP->type) )
    {
        if( pass >= 2 )
        {
            errorAt("redefined symbol", col);
            return( symP );
        }
    }
    symP->val = val & 0777777;
    symP->type = type;
    return( symP );
}

// Define the lexeme start..term.
static Sym *
defineLexeme(int start, int term, int val, int type)
{
char name[SYMLEN];

    lexemeToName(name, start, term);
    return( defineSymbol(name, val, type, start) );
}

// Look up the current lexeme.
static Sym *
evalSymbol(void)
{
char name[SYMLEN];

    return( lookup(lexemeToName(name, lexstart, lexterm), UNDEFINED) );
}

// Return 1 when the current lexeme is a symbol: it has a letter anywhere.
static int
isLexSymbol(void)
{
int ix;

    for( ix = lexstart; ix < lexterm; ix++ )
    {
        if( isalpha((unsigned char)line[ix]) )
        {
            return( 1 );
        }
    }
    return( 0 );
}

// Return the dummy number (1 up) of sym in the current expansion, 0 for R, -1 for neither.
static int
dummyIndex(Sym *symP)
{
MacDef *mdP;

    if( !curMacroP || !curMacroP->defnP )
    {
        return( -1 );
    }
    mdP = curMacroP->defnP;
    if( (symP >= &mdP->args[0]) && (symP <= &mdP->args[mdP->nargs]) )
    {
        return( (int)(symP - &mdP->args[0]) );
    }
    return( -1 );
}

// Move to the end of the current line, as macro1 does after an error (stopping at a tab).
static void
moveToEndOfLine(void)
{
    while( !ISEND(line[cc]) )
    {
        cc++;
    }
    lexstart = cc;
    lexterm = cc;
    lexstartprev = lexstart;
}

// Frame the next lexeme of line[] in lexstart..lexterm.
// With op set, a run of spaces is/ returned as one space, the add operator.
static void
next(int op)
{
char c;

    lexstartprev = lexstart;
    c = line[cc];
    if( c == ' ' )
    {
        do
        {
            c = line[++cc];
        } while( c == ' ' );
        if( op )
        {
            cc--;
        }
    }

    overbar = 0;
    lexstart = cc;
    c = line[cc];
    if( isalnum((unsigned char)c) || ISOVERBAR(c) )
    {
        if( ISOVERBAR(c) )
        {
            overbar = 1;
        }
        do
        {
            c = line[++cc];
            if( ISOVERBAR(c) )
            {
                overbar = 1;
            }
        } while( isalnum((unsigned char)c) || ISOVERBAR(c) );
    }
    else if( !ISDONE(c) || (c == '\t') )
    {
        cc++;
    }
    lexterm = cc;
}

// Read the next line: the next segment of the macro body being expanded (each tab-ended piece
// is a line of its own there), the use line again when a body ends, or the next source line.
static void
readLine(void)
{
MacInv *mpP;
const char *srcP;
int n;
char mc;

    errorInLine = 0;
    if( curMacroP && (*curMacroP->ptrP == '\0') )
    {
        memcpy(line, curMacroP->line, sizeof(line));
        memcpy(linePos, curMacroP->linePos, sizeof(curMacroP->linePos));
        cc = curMacroP->cc;
        lexstartprev = cc;
        maxcc = (int)strlen(line);
        linePos[LINELEN] = linePos[maxcc];
        mpP = curMacroP->prevP;
        free(curMacroP);
        curMacroP = mpP;
        hInvokeEnd(pass);
        return;
    }

    cc = 0;
    lexstartprev = 0;
    if( curMacroP )
    {
        maxcc = 0;
        do
        {
            mc = *curMacroP->ptrP++;
            linePos[maxcc] = *curMacroP->posPtrP++;
            line[maxcc++] = mc;
        } while( !ISEND(mc) && (maxcc < (LINELEN - 1)) );
        line[maxcc] = '\0';
        linePos[maxcc] = linePos[maxcc - 1] + 1;
        return;
    }

    if( nextLine >= m1LineCount )
    {
        endOfInput = 1;
        line[0] = '\0';
        maxcc = 0;
        linePos[0] = M1POS(nextLine, 0);
        return;
    }

    srcP = m1Lines[nextLine];
    n = (int)strlen(srcP);
    if( n > (LINELEN - 3) )
    {
        n = LINELEN - 3;
    }
    for( maxcc = 0; maxcc < n; maxcc++ )
    {
        line[maxcc] = srcP[maxcc];
        linePos[maxcc] = M1POS(nextLine, maxcc);
    }
    line[maxcc] = '\n';
    linePos[maxcc] = M1POS(nextLine, maxcc);
    maxcc++;
    line[maxcc] = '\0';
    linePos[maxcc] = M1POS(nextLine, maxcc);
    nextLine++;
}

// Add a value to the current constants block.
// Pass 1 counts uses; pass 2 pools by value and
// returns the address of the value's word.
static int
literal(int value)
{
int i;

    if( nconst >= MAX_CONSTANTS )
    {
        fprintf(stderr, "mactoam1: too many constants blocks\n");
        exit(1);
    }
    if( pass == 1 )
    {
        if( ++litCount[nconst] == MAX_LITERALS )
        {
            hFatal(pass, -1, "more constants than the converter holds");
        }
        return( litCount[nconst] );
    }
    for( i = 0; i < nlit; i++ )
    {
        if( litter[i] == value )
        {
            return( litLoc[nconst] + i );
        }
    }
    if( nlit == MAX_LITERALS )
    {
        hFatal(pass, -1, "more constants than the converter holds");
    }
    litter[nlit] = value;
    return( litLoc[nconst] + nlit++ );
}

// Return the next character code of a text string, with case shifts as needed, or -1 at the
// closing delimiter (or at the end, delim -1).
// It reads on into the next line.
static int
nextFiodec(int *ccaseP, int delim)
{
unsigned char c;

    for( ;; )
    {
        if( cc >= maxcc )
        {
            if( delim == -1 )
            {
                return( -1 );
            }
            readLine();
            if( endOfInput )
            {
                return( -1 );
            }
        }
        c = (unsigned char)line[cc];
        if( c == '\n' )
        {
            c = '\r';
            break;
        }
        if( c == '\r' )
        {
            continue;
        }
        break;
    }

    if( (delim != -1) && (c == delim) )
    {
        if( *ccaseP == LC )
        {
            cc++;
            return( -1 );
        }
        *ccaseP = LC;
        return( CONCISE_LC );
    }

    if( c > 0177 )
    {
        errorAt("illegal character", cc);
        c = 0;
    }
    c = asciiToFiodec[c & 0177];
    if( c == BAD )
    {
        errorAt("illegal character", cc);
        c = 0;
    }
    if( !(c & *ccaseP) )
    {
        *ccaseP ^= BC;
        if( *ccaseP == LC )
        {
            return( CONCISE_LC );
        }
        return( CONCISE_UC );
    }
    cc++;
    return( c & CHARBITS );
}

// The value of flexo: the next 3 characters, case shifts among them.
static int
flex(void)
{
int w;
int shift;
int ccase;
unsigned char c;

    if( line[lexstart] == ' ' )
    {
        next(0);
    }
    w = 0;
    ccase = LC;
    for( shift = 12; shift >= 0; shift -= 6 )
    {
        if( lexstart >= maxcc )
        {
            break;
        }
        c = (unsigned char)line[lexstart];
        if( (c == '\t') || (c == '\n') )
        {
            if( ccase == LC )
            {
                break;
            }
            c = CONCISE_LC;
        }
        else
        {
            if( c > 0177 )
            {
                errorAt("illegal character", lexstart);
                c = 0;
            }
            c = asciiToFiodec[c & 0177];
            if( c == BAD )
            {
                errorAt("illegal character", lexstart);
                c = 0;
            }
            if( !(c & ccase) )
            {
                ccase ^= BC;
                c = (ccase == LC) ? CONCISE_LC : CONCISE_UC;
            }
            else
            {
                lexstart++;
            }
        }
        w |= (c & CHARBITS) << shift;
    }
    return( w );
}

// The value of char: a position (l, m or r) then a character.
static int
getChar(void)
{
unsigned char c;
unsigned char pos;

    if( cc >= maxcc )
    {
        return( 0 );
    }
    pos = (unsigned char)line[cc++];
    if( (pos != 'l') && (pos != 'm') && (pos != 'r') )
    {
        errorAt("illegal character", lexstart);
        return( 0 );
    }
    if( cc >= maxcc )
    {
        return( 0 );
    }
    c = (unsigned char)line[cc++];
    if( c > 0177 )
    {
        errorAt("illegal character", lexstart);
        c = 0;
    }
    c = asciiToFiodec[c];
    if( c == BAD )
    {
        errorAt("illegal character", lexstart);
        c = 0;
    }
    if( !(c & LC) )
    {
        c = CONCISE_UC;
        cc--;
    }
    c &= CHARBITS;
    switch( pos )
    {
    case 'l':
        return( c << 12 );
    case 'm':
        return( c << 6 );
    default:
        return( c );
    }
}

// Evaluate one term and advance past it, reporting it.
static Sym
eval2(void)
{
int digit;
int from;
int val;
int pos;
int endIdx;
int closeIdx;
int idx;
int kind;
Sym *symP;
Sym symEval;

    symEval.type = DEFINED;
    symEval.name[0] = '\0';
    symEval.val = 0;
    symEval.flags = 0;
    pos = linePos[lexstart];

    if( isLexSymbol() )
    {
        symP = evalSymbol();
        idx = dummyIndex(symP);
        if( !M_DEFINED(symP->type) )
        {
            if( pass >= 2 )
            {
                errorAt("undefined symbol", lexstart);
            }
            endIdx = lexterm;
            next(1);
            hTerm(pass, M1T_UNDEF, pos, linePos[endIdx], symP->val, 0);
            return( *symP );
        }
        if( M_PSEUDO(symP->type) || M_EPSEUDO(symP->type) )
        {
            kind = M1T_EPSEUDO;
            switch( symP->val )
            {
            case M1P_DECIMAL:
                radix = 10;
                symEval.type = PSEUDO;
                break;
            case M1P_OCTAL:
                radix = 8;
                symEval.type = PSEUDO;
                break;
            case M1P_FLEX:
                kind = M1T_FLEX;
                next(1);
                symEval.val = flex();
                break;
            case M1P_CHAR:
                kind = M1T_CHAR;
                next(1);
                symEval.val = getChar();
                break;
            default:
                kind = M1T_PSEUDO;
                errorAt("value required", lexstart);
                symEval.type = symP->type;
                break;
            }
            endIdx = (cc > lexterm) ? cc : lexterm;
            next(1);
            hTerm(pass, kind, pos, linePos[endIdx], symEval.val, symP->val);
            return( symEval );
        }
        if( M_MACRO(symP->type) )
        {
            if( pass >= 2 )
            {
                errorAt("misplaced symbol", lexstart);
            }
            symEval.type = symP->type;
            endIdx = lexterm;
            next(1);
            hTerm(pass, M1T_MACRO, pos, linePos[endIdx], 0, symP->val);
            return( symEval );
        }
        endIdx = lexterm;
        next(1);
        if( idx == 0 )
        {
            kind = M1T_R;
        }
        else if( idx > 0 )
        {
            kind = M1T_DUMMY;
        }
        else if( symP->flags & SF_VAR )
        {
            kind = M1T_VAR;
        }
        else if( M_FIXED(symP->type) )
        {
            kind = M1T_PERM;
        }
        else if( M_LABEL(symP->type) )
        {
            kind = M1T_LABEL;
        }
        else
        {
            kind = M1T_EQU;
        }
        hTerm(pass, kind, pos, linePos[endIdx], symP->val, (idx > 0) ? idx : 0);
        return( *symP );
    }

    if( isdigit((unsigned char)line[lexstart]) )
    {
        from = lexstart;
        val = 0;
        while( from < lexterm )
        {
            if( isdigit((unsigned char)line[from]) )
            {
                digit = line[from++] - '0';
                if( digit >= radix )
                {
                    errorAt("number not in current radix", from - 1);
                    val = 0;
                    break;
                }
                val = (val * radix) + digit;
            }
            else
            {
                errorAt("numeric syntax", lexstart);
                val = 0;
                break;
            }
        }
        endIdx = lexterm;
        next(1);
        symEval.val = val;
        hTerm(pass, M1T_NUMBER, pos, linePos[endIdx], val, 0);
        return( symEval );
    }

    switch( line[lexstart] )
    {
    case '.':
        val = clc;
        endIdx = lexterm;
        next(1);
        hTerm(pass, M1T_DOT, pos, linePos[endIdx], val, 0);
        break;

    case '(':
        hOpen(pass, pos, '(');
        next(1);
        val = getExprs();
        endIdx = lexstart;
        if( line[lexstart] == ')' )
        {
            closeIdx = lexstart;
            next(1);
            endIdx = closeIdx + 1;
        }
        symEval.val = literal(val);
        hTerm(pass, M1T_LITERAL, pos, linePos[endIdx], symEval.val, val);
        return( symEval );

    case '[':
        hOpen(pass, pos, '[');
        next(1);
        symEval.val = getExprs();
        endIdx = lexstart;
        if( line[lexstart] == ']' )
        {
            closeIdx = lexstart;
            next(1);
            endIdx = closeIdx + 1;
        }
        else
        {
            errorAt("illegal character", lexstart);
        }
        hTerm(pass, M1T_GROUP, pos, linePos[endIdx], symEval.val, 0);
        return( symEval );

    default:
        if( line[lexstart] == '=' )
        {
            errorAt("illegal equals", lexstart);
            moveToEndOfLine();
        }
        else
        {
            errorAt("illegal character", lexstart);
        }
        val = 0;
        endIdx = lexterm;
        next(1);
        hTerm(pass, M1T_BAD, pos, linePos[endIdx], 0, 0);
        break;
    }
    symEval.val = val;
    return( symEval );
}

// Evaluate a term with its unary sign.
// A minus complements the term's bits, so -0 is 0777777.
static Sym
eval(void)
{
Sym sym;
int pos;
int op;

    op = line[lexstart];
    pos = linePos[lexstart];
    switch( op )
    {
    case '-':
        next(1);
        sym = eval2();
        sym.val ^= 0777777;
        hUnary(pass, pos, '-');
        break;
    case '+':
        next(1);
        sym = eval2();
        hUnary(pass, pos, '+');
        break;
    default:
        sym = eval2();
        break;
    }
    return( sym );
}

// Evaluate an expression, strictly left to right.
// A space and + add with end-around carry,
// - adds the complement, * multiplies and folds the same way, & and ! are bitwise.
static Sym
getExpr(void)
{
Sym sym;
int space;
int pos;

    sym = eval();
    for( ;; )
    {
        space = 0;
        pos = linePos[lexstart];
        switch( line[lexstart] )
        {
        case ' ':
            space = 1;
            // fall through
        case '+':
            next(1);
            if( space && ISEND(line[lexstart]) )
            {
                return( sym );
            }
            hOp(pass, pos, space ? ' ' : '+');
            sym.val += eval().val;
            sym.type = DEFINED;
            if( sym.val >= 01000000 )
            {
                sym.val = (sym.val + 1) & 0777777;
            }
            continue;

        case '-':
            next(1);
            hOp(pass, pos, '-');
            sym.val += eval().val ^ 0777777;
            sym.type = DEFINED;
            if( sym.val >= 01000000 )
            {
                sym.val = (sym.val + 1) & 0777777;
            }
            continue;

        case '*':
            next(1);
            hOp(pass, pos, '*');
            sym.val *= eval().val;
            sym.type = DEFINED;
            if( sym.val >= 01000000 )
            {
                sym.val = (sym.val + 1) & 0777777;
            }
            continue;

        case '&':
            next(1);
            hOp(pass, pos, '&');
            sym.val &= eval().val;
            sym.type = DEFINED;
            continue;

        case '!':
            next(1);
            hOp(pass, pos, '!');
            sym.val |= eval().val;
            sym.type = DEFINED;
            continue;

        case '/':
        case ')':
        case ']':
        case ':':
        case ',':
            break;

        case '=':
            errorAt("illegal equals", lexstart);
            moveToEndOfLine();
            sym.val = 0;
            break;

        default:
            if( !ISEND(line[lexstart]) )
            {
                errorAt("illegal expression", lexstart);
                moveToEndOfLine();
                sym.val = 0;
            }
            break;
        }
        break;
    }
    return( sym );
}

// Evaluate an expression where a value is required.
static int
getExprs(void)
{
Sym sym;

    sym = getExpr();
    if( sym.type == PSEUDO )
    {
        errorAt("value required", lexstart);
    }
    return( sym.val & 0777777 );
}

// Evaluate an expression and report it as a site of the given kind.
static int
siteExpr(int kind)
{
int start;
int val;

    start = linePos[lexstart];
    val = getExprs();
    hExpr(pass, kind, start, linePos[lexstart], val);
    return( val );
}

// Start an expansion: evaluate the arguments, missing ones are 0, extras are ignored,
// and push the body.
// Returns 0 when the macro has no body, nothing is expanded.
static int
invokeMacro(int index, int pos, int end)
{
MacInv *mipP;
MacDef *mdP;
int jx;
int val;
int depth;
char name[SYMLEN];

    mdP = macDefs[index];
    if( !mdP || (mdP->bodyP[0] == '\0') )
    {
        hInvokeEmpty(pass, pos, end, index);
        return( 0 );
    }

    // macro1 has no limit, and a use inside its own body never ends
    depth = 0;
    for( mipP = curMacroP; mipP; mipP = mipP->prevP )
    {
        depth++;
    }
    if( depth >= MAX_MACRO_DEPTH )
    {
        hFatal(pass, pos, "macros used inside macros over 200 deep; macro1 never ends this expansion");
    }
    while( ISBLANK(line[lexstart]) )
    {
        next(0);
    }

    mipP = calloc(1, sizeof(MacInv));
    if( !mipP )
    {
        fprintf(stderr, "mactoam1: out of memory\n");
        exit(1);
    }
    mipP->defnP = mdP;
    hInvokeStart(pass, pos, end, index);

    mdP->args[0].val = clc;
    for( jx = 1; !ISDONE(line[lexstart]) && (jx <= MAC_MAX_ARGS); )
    {
        next(0);
        if( ISDONE(line[lexstart]) )
        {
            break;
        }
        if( line[lexstart] == ',' )
        {
            next(0);
        }
        while( ISBLANK(line[lexstart]) )
        {
            next(0);
        }
        if( ISDONE(line[lexstart]) )
        {
            break;
        }

        // An argument that is one symbol is remembered by name, for a tag that names its dummy.
        if( isLexSymbol() && ((line[lexterm] == ',') || ISDONE(line[lexterm])) )
        {
            strcpy(mipP->argName[jx], lexemeToName(name, lexstart, lexterm));
        }
        val = siteExpr(M1E_ARG);
        if( jx <= mdP->nargs )
        {
            mdP->args[jx].val = val;
        }
        jx++;
    }
    hInvokeArgs(pass, jx - 1, linePos[lexstart]);
    while( jx <= mdP->nargs )
    {
        mdP->args[jx++].val = 0;
    }

    memcpy(mipP->line, line, sizeof(line));
    memcpy(mipP->linePos, linePos, sizeof(mipP->linePos));
    mipP->cc = cc;
    mipP->ptrP = mdP->bodyP;
    mipP->posPtrP = mdP->bodyPosP;
    mipP->prevP = curMacroP;
    curMacroP = mipP;
    return( 1 );
}

// define: the name and dummies (the name may be on the next line), then each line up to one
// whose first word begins with term is the body.
static void
defineMacro(int pos)
{
int lexstartsave;
int index;
int error;
int count;
int length;
int capacity;
int value;
int nameLine;
int termLine;
int i;
int ll;
char termin[SYMLEN];
char args[MAC_MAX_ARGS][SYMLEN];
char *bodyP;
int *bodyPosP;
MacDef *mdP;

    if( nrepeats )
    {
        errorAt("define in a repeat", lexstartprev);
        return;
    }
    while( (line[lexstart] == ' ') || (line[lexstart] == '\t') )
    {
        next(0);
    }
    if( ISEND(line[lexstart]) )
    {
        readLine();
        next(0);
        while( (line[lexstart] == ' ') || (line[lexstart] == '\t') )
        {
            next(0);
        }
    }
    nameLine = M1LINE(linePos[lexstart]);

    // index 0 means no bad name found, so (as in macro1) a bad name in column 0 goes unseen
    count = 0;
    index = 0;
    error = 0;
    value = -1;
    lexstartsave = lexstart;
    while( !ISDONE(line[lexstart]) && (count < MAC_MAX_ARGS) )
    {
        if( !isalnum((unsigned char)line[lexstart]) && (index == 0) )
        {
            index = lexstart;
        }
        lexemeToName(args[count++], lexstart, lexterm);
        if( line[lexterm] == ',' )
        {
            next(0);
        }
        next(0);
        if( line[lexstart] == ' ' )
        {
            next(0);
        }
    }
    if( count == 0 )
    {
        errorAt("no name following define", lexstartsave);
        error = 1;
    }
    else if( index )
    {
        errorAt("bad dummy argument following define", index);
        error = 1;
    }
    else if( macCount >= MAC_TABLE_LENGTH )
    {
        errorAt("macro table full", lexstartsave);
        error = 1;
    }
    else
    {
        value = macCount++;
        defineSymbol(args[0], value, MACRO, lexstartsave);
    }

    length = 0;
    capacity = 1024;
    bodyP = malloc(capacity);
    bodyPosP = malloc(capacity * sizeof(int));
    termLine = -1;
    for( ;; )
    {
        readLine();
        if( endOfInput )
        {
            break;
        }
        next(0);
        while( (line[lexstart] == ' ') || (line[lexstart] == '\t') )
        {
            next(0);
        }
        lexemeToName(termin, lexstart, lexterm);
        if( strncmp(termin, "term", 4) == 0 )
        {
            termLine = M1LINE(linePos[0]);
            break;
        }
        ll = (int)strlen(line);
        if( (length + ll + 1) >= capacity )
        {
            capacity = (2 * capacity) + ll;
            bodyP = realloc(bodyP, capacity);
            bodyPosP = realloc(bodyPosP, capacity * sizeof(int));
        }
        for( i = 0; i < ll; i++ )
        {
            bodyP[length + i] = line[i];
            bodyPosP[length + i] = linePos[i];
        }
        length += ll;
    }
    bodyP[length] = '\0';
    bodyPosP[length] = (length > 0) ? (bodyPosP[length - 1] + 1) : 0;

    if( error )
    {
        free(bodyP);
        free(bodyPosP);
        hDefine(pass, pos, nameLine, -1, "", args, 0, termLine);
        return;
    }

    mdP = calloc(1, sizeof(MacDef));
    if( !mdP )
    {
        fprintf(stderr, "mactoam1: out of memory\n");
        exit(1);
    }
    macDefs[value] = mdP;
    mdP->bodyP = bodyP;
    mdP->bodyPosP = bodyPosP;
    mdP->nargs = count - 1;
    mdP->args[0].type = DEFINED;
    strcpy(mdP->args[0].name, "R");
    for( i = 1; i <= mdP->nargs; i++ )
    {
        mdP->args[i].type = DEFINED;
        strcpy(mdP->args[i].name, args[i]);
    }
    hDefine(pass, pos, nameLine, value, args[0], args + 1, mdP->nargs, termLine);
}

// variables: the block the overbarred symbols seen so far are placed in.
static void
variables(int pos)
{
int start;

    start = clc;
    varsAddr = clc;
    clc = (clc + nvars) & ADDRESS_FIELD;
    hVariables(pass, pos, start, clc);
}

// text: the characters between a delimiter and its next appearance, 3 to a word.
static void
text(int pos)
{
char delim;
int w;
int count;
int ccase;
int c;
int nwords;
int words[LINELEN];

    do
    {
        if( cc >= maxcc )
        {
            errorAt("text with no delimiter", lexstart);
            return;
        }
        delim = line[cc++];
    } while( delim == ' ' );

    w = 0;
    count = 0;
    nwords = 0;
    ccase = LC;
    for( ;; )
    {
        c = nextFiodec(&ccase, delim);
        if( c == -1 )
        {
            break;
        }
        w |= c << ((2 - count) * 6);
        if( ++count == 3 )
        {
            hWord(pass, clc, w);
            if( nwords < LINELEN )
            {
                words[nwords++] = w;
            }
            clc = (clc + 1) & ADDRESS_FIELD;
            count = 0;
            w = 0;
        }
    }
    if( count > 0 )
    {
        hWord(pass, clc, w);
        if( nwords < LINELEN )
        {
            words[nwords++] = w;
        }
        clc = (clc + 1) & ADDRESS_FIELD;
    }
    hText(pass, pos, linePos[cc], nwords, words);
}

// Constants, the pool of the literals since the last constant statement.
// Pass 1 reserves a word for each use, pass 2 fills the words with the actual values and leaves the rest.
static void
constants(int pos)
{
int i;
int start;

    if( pass == 1 )
    {
        litLoc[nconst] = clc;
        for( i = 0; i < litCount[nconst]; i++ )
        {
            clc = (clc + 1) & ADDRESS_FIELD;
        }
        nconst++;
        return;
    }

    start = clc;
    for( i = 0; i < litCount[nconst]; i++ )
    {
        if( i < nlit )
        {
            hWord(pass, clc, litter[i] & 0777777);
        }
        clc = (clc + 1) & ADDRESS_FIELD;
    }
    hConstants(pass, pos, start, litCount[nconst], nlit, litter);
    nconst++;
    nlit = 0;
}

// dimension a(n), b(m): DEC MACRO's arrays, placed with the variables.
static void
dimension(int pos)
{
int start;
int term;
int size;
Sym *symP;
char name[SYMLEN];

    hPseudo(pass, pos, linePos[lexterm], M1P_DIMENSION);
    next(0);
    while( !ISDONE(line[lexstart]) )
    {
        if( line[lexstart] == ',' )
        {
            next(0);
            continue;
        }
        if( !isLexSymbol() || (line[lexterm] != '(') )
        {
            errorAt("dimension syntax", lexstart);
            moveToEndOfLine();
            return;
        }
        start = lexstart;
        term = lexterm;
        next(0);
        next(0);
        size = siteExpr(M1E_DIMENSION) & ADDRESS_FIELD;
        if( line[lexstart] != ')' )
        {
            errorAt("dimension syntax", lexstart);
            moveToEndOfLine();
            return;
        }
        next(0);
        lexemeToName(name, start, term);
        if( pass == 1 )
        {
            symP = lookup(name, DEFINED);
            if( !M_DEFINED(symP->type) )
            {
                symP->type = DEFINED;
                symP->val = 0777777;
            }
        }
        else
        {
            symP = defineSymbol(name, varsAddr, DEFINED, start);
            hVarAlloc(pass, symP->name, varsAddr, size);
            varsAddr += size;
        }
        symP->flags |= SF_VAR;
        nvars += size;
    }
}

// A statement pseudo-instruction, its name the current lexeme.
static void
pseudo(int code)
{
int count;
int repeatStart;
int pos;
int end;
int addr;

    pos = linePos[lexstart];
    end = linePos[lexterm];
    switch( code )
    {
    case M1P_CONSTANTS:
        hPseudo(pass, pos, end, code);
        next(0);
        constants(pos);
        break;

    case M1P_VARIABLES:
        hPseudo(pass, pos, end, code);
        next(0);
        variables(pos);
        break;

    case M1P_DEFINE:
        next(0);
        defineMacro(pos);
        break;

    case M1P_REPEAT:
        hPseudo(pass, pos, end, code);
        next(0);
        count = siteExpr(M1E_REPEAT) & ADDRESS_FIELD;
        if( line[lexstart] == ',' )
        {
            next(0);
        }
        nrepeats++;
        repeatStart = lexstart;
        hRepeat(pass, pos, count, linePos[repeatStart], linePos[maxcc]);
        while( count-- > 0 )
        {
            cc = repeatStart;
            processLine();
        }
        cc = maxcc;
        nrepeats--;
        hRepeatEnd(pass);
        break;

    case M1P_START:
        next(0);
        if( !ISDONE(line[lexstart]) )
        {
            if( line[lexstart] == ' ' )
            {
                next(0);
            }
            addr = siteExpr(M1E_START) & ADDRESS_FIELD;
            hStart(pass, pos, linePos[lexstart], 1, addr);
            next(0);
        }
        else
        {
            hStart(pass, pos, end, 0, 0);
        }
        // macro1 takes the line after start as the title of a further program
        titleSet = 0;
        break;

    case M1P_TEXT:
        hPseudo(pass, pos, end, code);
        text(pos);
        break;

    case M1P_NOINPUT:
        hPseudo(pass, pos, end, code);
        next(0);
        noinput = 1;
        break;

    case M1P_EXPUNGE:
        hPseudo(pass, pos, end, code);
        next(0);
        if( pass == 1 )
        {
            symbolTop = 0;
        }
        break;

    default:
        break;
    }
}

// Assemble one line: the title if none is set yet, else each statement on it.
static void
processLine(void)
{
int jx;
int val;
int start;
int term;
int idx;
int pos;
int newClc;
Sym evalue;
Sym *symP;
MacDef *mdP;
char name[SYMLEN];

    if( !titleSet )
    {
        if( (line[0] != '\0') && (line[0] != '\n') )
        {
            titleSet = 1;
            hTitle(pass, M1LINE(linePos[0]));
        }
        return;
    }

    for( ;; )
    {
        next(0);
        if( endOfInput )
        {
            return;
        }
        if( ISEND(line[lexstart]) )
        {
            if( line[lexstart] != '\t' )
            {
                return;
            }
            continue;
        }
        if( line[lexstart] == '/' )
        {
            hComment(pass, linePos[lexstart]);
            return;
        }

        // An origin, e/, is found by looking ahead to the first blank.
        for( jx = lexstart; jx < maxcc; jx++ )
        {
            if( ISBLANK(line[jx]) || ISDONE(line[jx]) )
            {
                break;
            }
        }
        if( line[jx] == '/' )
        {
            start = linePos[lexstart];
            newClc = getExprs();
            hExpr(pass, M1E_ORIGIN, start, linePos[jx] + 1, newClc);
            if( !errorInLine )
            {
                clc = newClc;
            }
            cc = jx + 1;
            next(0);
            continue;
        }

        switch( line[lexterm] )
        {
        case ',':
            if( isLexSymbol() )
            {
                lexemeToName(name, lexstart, lexterm);
                symP = lookup(name, UNDEFINED);
                idx = dummyIndex(symP);
                pos = linePos[lexstart];
                if( idx > 0 )
                {
                    // A tag naming a dummy defines the argument's symbol, as DEC's MACRO does.
                    if( curMacroP->argName[idx][0] == '\0' )
                    {
                        errorAt("tag names a dummy whose argument is not a symbol", lexstart);
                    }
                    else
                    {
                        symP = lookup(curMacroP->argName[idx], DEFINED);
                        if( M_DEFINED(symP->type) && (symP->val != clc) && (pass >= 2) )
                        {
                            errorAt("duplicate label", lexstart);
                        }
                        defineSymbol(curMacroP->argName[idx], clc, LABEL, lexstart);
                        hLabel(pass, pos, linePos[lexterm], clc, 1, 1);
                    }
                }
                else
                {
                    // A tag in an expansion is relative to where the expansion began.
                    val = clc;
                    if( curMacroP )
                    {
                        val = clc - curMacroP->defnP->args[0].val;
                    }
                    if( M_DEFINED(symP->type) )
                    {
                        if( (symP->val != val) && (pass >= 2) )
                        {
                            errorAt("duplicate label", lexstart);
                        }
                        symP->type |= DUPLICATE;
                    }
                    defineLexeme(lexstart, lexterm, val, LABEL);
                    hLabel(pass, pos, linePos[lexterm], val, curMacroP != NULL, 0);
                }
            }
            else if( isdigit((unsigned char)line[lexstart]) )
            {
                hNumTag(pass, linePos[lexstart], linePos[lexterm]);
                val = 0;
                for( jx = lexstart; jx < lexterm; jx++ )
                {
                    if( !isdigit((unsigned char)line[jx]) || ((line[jx] - '0') >= radix) )
                    {
                        errorAt("numeric syntax", jx);
                        break;
                    }
                    val = (val * radix) + (line[jx] - '0');
                }
                if( (jx == lexterm) && (clc != val) && (pass >= 2) )
                {
                    errorAt("duplicate label", lexstart);
                }
            }
            else
            {
                errorAt("label syntax", lexstart);
            }
            next(0);
            continue;

        case '=':
            if( isLexSymbol() )
            {
                start = lexstart;
                term = lexterm;
                pos = linePos[lexstart];
                lexemeToName(name, start, term);
                idx = -1;
                if( curMacroP )
                {
                    mdP = curMacroP->defnP;
                    for( jx = 0; jx <= mdP->nargs; jx++ )
                    {
                        if( strcmp(name, mdP->args[jx].name) == 0 )
                        {
                            idx = jx;
                        }
                    }
                }
                next(0);
                next(0);
                val = siteExpr(M1E_ASSIGN);
                defineLexeme(start, term, val, DEFINED);
                hAssign(pass, pos, linePos[term], val, idx);
            }
            else
            {
                errorAt("symbol syntax", lexstartprev);
                next(0);
                next(0);
                getExprs();
            }
            continue;
        }

        if( isLexSymbol() )
        {
            lexemeToName(name, lexstart, lexterm);
            symP = evalSymbol();
            if( M_MACRO(symP->type) && (dummyIndex(symP) < 0) )
            {
                if( !invokeMacro(symP->val, linePos[lexstart], linePos[lexterm]) )
                {
                    next(0);
                }
                continue;
            }
            if( M_PSEUDO(symP->type) )
            {
                pseudo(symP->val);
                continue;
            }
            if( (strcmp(name, "dimens") == 0) && !M_DEFINED(symP->type) )
            {
                dimension(linePos[lexstart]);
                continue;
            }
        }

        start = linePos[lexstart];
        evalue = getExpr();
        if( evalue.type == PSEUDO )
        {
            hExpr(pass, M1E_NOWORD, start, linePos[lexstart], 0);
            continue;
        }
        if( line[lexstart] == ',' )
        {
            hExpr(pass, M1E_LOCCHECK, start, linePos[lexstart], evalue.val & 0777777);
            if( (evalue.val != clc) && (pass >= 2) )
            {
                errorAt("duplicate label", lexstart);
            }
        }
        else if( line[lexstart] == '/' )
        {
            hExpr(pass, M1E_ORIGIN, start, linePos[lexstart] + 1, evalue.val & 0777777);
            clc = evalue.val;
            next(0);
        }
        else
        {
            hExpr(pass, M1E_WORD, start, linePos[lexstart], evalue.val & 0777777);
            hWord(pass, clc, evalue.val & 0777777);
            clc = (clc + 1) & ADDRESS_FIELD;
        }
    }
}

// Set up the symbol table with macro1's pseudo-instructions and permanent symbols.
void
m1Init(void)
{
unsigned int ix;

    symtabP = calloc(SYMBOL_TABLE_SIZE, sizeof(Sym));
    if( !symtabP )
    {
        fprintf(stderr, "mactoam1: out of memory\n");
        exit(1);
    }
    symbolTop = 0;
    pass = 0;
    for( ix = 0; ix < DIM(pseudos); ix++ )
    {
        defineSymbol(pseudos[ix].name, pseudos[ix].val, pseudos[ix].type, 0);
    }
    for( ix = 0; ix < DIM(permanentSymbols); ix++ )
    {
        defineSymbol(permanentSymbols[ix].name, permanentSymbols[ix].val,
            permanentSymbols[ix].type, 0);
    }
}

// One pass over the source.
// Pass 3 repeats pass 2 from the state pass 2 began in.
void
m1Pass(int p)
{
int ix;
MacInv *mpP;

    if( p == 2 )
    {
        pass2Start.symtabP = malloc(SYMBOL_TABLE_SIZE * sizeof(Sym));
        memcpy(pass2Start.symtabP, symtabP, SYMBOL_TABLE_SIZE * sizeof(Sym));
        pass2Start.symbolTop = symbolTop;
        pass2Start.vars_addr = varsAddr;
        pass2Start.noinput = noinput;
    }
    else if( p == 3 )
    {
        memcpy(symtabP, pass2Start.symtabP, SYMBOL_TABLE_SIZE * sizeof(Sym));
        symbolTop = pass2Start.symbolTop;
        varsAddr = pass2Start.vars_addr;
        noinput = pass2Start.noinput;
    }
    pass = p;

    clc = 4;
    nconst = 0;
    nvars = 0;
    nlit = 0;
    while( curMacroP )
    {
        mpP = curMacroP->prevP;
        free(curMacroP);
        curMacroP = mpP;
    }
    for( ix = 0; ix < macCount; ix++ )
    {
        if( macDefs[ix] )
        {
            free(macDefs[ix]->bodyP);
            free(macDefs[ix]->bodyPosP);
            free(macDefs[ix]);
        }
        macDefs[ix] = NULL;
    }
    macCount = 0;
    nrepeats = 0;
    titleSet = 0;
    radix = 8;
    endOfInput = 0;
    nextLine = 0;
    lexstart = 0;
    lexterm = 0;

    for( ;; )
    {
        readLine();
        if( endOfInput )
        {
            break;
        }
        processLine();
    }
    if( (pass >= 2) && (nlit > 0) )
    {
        hOrphanLiterals(pass, nlit);
    }
}

// Expand macro macIdx once with no arguments, as pass 4, then put every piece of assembly
// state back, so the expansion changes nothing but what the hooks saw.
void
m1Phantom(int macIdx)
{
int savedPass;
int savedClc;
int savedNlit;
int savedNconst;
int savedNvars;
int savedVarsAddr;
int savedRadix;
int savedTitleSet;
int savedNextLine;
int savedEndOfInput;
int savedSymbolTop;
int savedCc;
int savedMaxcc;
int i;
int *litterP;
Sym *symsP;
char *lineP;
int *posP;
MacInv *mipP;
MacDef *mdP;

    mdP = macDefs[macIdx];
    if( !mdP || (mdP->bodyP[0] == '\0') )
    {
        return;
    }
    savedPass = pass;
    savedClc = clc;
    savedNlit = nlit;
    savedNconst = nconst;
    savedNvars = nvars;
    savedVarsAddr = varsAddr;
    savedRadix = radix;
    savedTitleSet = titleSet;
    savedNextLine = nextLine;
    savedEndOfInput = endOfInput;
    savedSymbolTop = symbolTop;
    savedCc = cc;
    savedMaxcc = maxcc;
    litterP = malloc(MAX_LITERALS * sizeof(int));
    symsP = malloc(SYMBOL_TABLE_SIZE * sizeof(Sym));
    lineP = malloc(sizeof(line));
    posP = malloc(sizeof(linePos));
    memcpy(litterP, litter, MAX_LITERALS * sizeof(int));
    memcpy(symsP, symtabP, SYMBOL_TABLE_SIZE * sizeof(Sym));
    memcpy(lineP, line, sizeof(line));
    memcpy(posP, linePos, sizeof(linePos));

    pass = 4;
    titleSet = 1;
    endOfInput = 0;
    nextLine = m1LineCount;
    strcpy(line, "\n");
    linePos[0] = -1;
    linePos[1] = -1;
    cc = 0;
    maxcc = 1;
    mipP = calloc(1, sizeof(MacInv));
    memcpy(mipP->line, line, sizeof(line));
    memcpy(mipP->linePos, linePos, sizeof(mipP->linePos));
    mipP->cc = 0;
    mipP->defnP = mdP;
    mipP->ptrP = mdP->bodyP;
    mipP->posPtrP = mdP->bodyPosP;
    mdP->args[0].val = clc;
    for( i = 1; i <= mdP->nargs; i++ )
    {
        mdP->args[i].val = 0;
    }
    hInvokeStart(pass, -1, -1, macIdx);
    hInvokeArgs(pass, 0, -1);
    mipP->prevP = NULL;
    curMacroP = mipP;
    while( curMacroP )
    {
        readLine();
        if( !curMacroP || endOfInput )
        {
            break;
        }
        processLine();
    }
    while( curMacroP )
    {
        mipP = curMacroP->prevP;
        free(curMacroP);
        curMacroP = mipP;
    }

    pass = savedPass;
    clc = savedClc;
    nlit = savedNlit;
    nconst = savedNconst;
    nvars = savedNvars;
    varsAddr = savedVarsAddr;
    radix = savedRadix;
    titleSet = savedTitleSet;
    nextLine = savedNextLine;
    endOfInput = savedEndOfInput;
    symbolTop = savedSymbolTop;
    cc = savedCc;
    maxcc = savedMaxcc;
    memcpy(litter, litterP, MAX_LITERALS * sizeof(int));
    memcpy(symtabP, symsP, SYMBOL_TABLE_SIZE * sizeof(Sym));
    memcpy(line, lineP, sizeof(line));
    memcpy(linePos, posP, sizeof(linePos));
    free(litterP);
    free(symsP);
    free(lineP);
    free(posP);
}

// The location counter.
int
m1Clc(void)
{
    return( clc );
}

// The radix numbers are read in, 8 or 10.
int
m1Radix(void)
{
    return( radix );
}
