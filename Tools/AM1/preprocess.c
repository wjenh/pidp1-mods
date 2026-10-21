/* preprocess.c - am1's preprocessor
 *
 * Reads an am1 source, parses its directives (#include, #includeonce,
 * #uninclude, #define and its #enddef, #undef, #if and its family, #error,
 * #warning) and expands its macros.
 * It writes the text am1's lexer reads and writes it as
 * gcc's "cpp -nostdinc -isystem <root> -C -x assembler-with-cpp" did when am1
 * ran it, the same line markers, blank lines and spacing.
 * The listing and the optimizer's -O=source read that text, its markers and its blank lines, so
 * gcc's rules for them are followed exactly: where a space goes between two
 * tokens, when a gap becomes blank lines or a marker, how an expansion is
 * spaced.
 * The macro expander follows gcc's libcpp for the same reason.
 * Comments are kept as tokens, except inside a directive, where they are blanks.
 *
 * What gcc's assembler mode does that C does not is kept: an unterminated
 * quote is a token running to the end of its line, a line starting with '#'
 * that is no directive passes through as text, and a '#' in a function-like
 * macro's body need not come before a parameter.
 *
 * Added: a function-like #define with nothing after its parameter list opens
 * a block, whose body is the lines that follow, up to #enddef.
 * The lines are joined by ';', am1's statement separator, so the body is the token list the
 * same macro written on one line with '\' and ';' has, and its expansion
 * keeps the line numbers as that one does.
 *
 * Added: origins.  Each statement written is recorded with the macros that
 * made it and the body line each levelcame from.
 * am1's lexer asks for them by file, line, and the statement's place
 * on that line and am1 writes them to its .map.
 * The * text written is unchanged by them.
 *
 * Refused by name: stringizing (#parameter), variadic macros, __FILE__,
 * __LINE__, _Pragma, and the directives am1 does not support (#pragma, #line,
 * #include_next and the like).
 * The only predefined macros are those am1.c passes as -D, AM1 among them.
 *
 * Called once per run from am1.c's main().
 * Nothing is freed, the process ends soon after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "preprocess.h"

#define PP_MAXDEPTH 200         // gcc's limit on #include nesting
#define PP_MAXGAP 8             // gcc writes a gap shorter than this as blank lines, else a marker
#define PP_HASHSIZE 4093
#define PP_MAXLOOKAHEAD 8
#define PP_MAXSCAN 4096         // how far ppStatementOrigin() looks ahead for a line

// Token flags
#define TF_PREV_WHITE 1         // blanks or a comment come before it
#define TF_BOL 2                // the first token of its line
#define TF_PASTE_LEFT 4         // a ## follows it in a macro body
#define TF_NO_EXPAND 8          // a macro's name met inside its own expansion
#define TF_FILE_END 16          // the EOF ending a file, its buffer not yet popped
#define TF_BODY 32              // a macro body's own token, given its expansion's chain when read

// Diagnostic kinds
#define PPD_ERROR 0
#define PPD_WARNING 1
#define PPD_NOTE 2
#define PPD_FATAL 3
#define PPD_DIRWARNING 4        // a #warning: reported even from a system header

// Token types.
// The punctuators are in gcc's order: those up to PP_LSHIFT are
// the ones that take a following '=', which cpp_avoid_paste relies on.
enum
{
    PP_EQ, PP_NOT, PP_GREATER, PP_LESS, PP_PLUS, PP_MINUS, PP_MULT, PP_DIV,
    PP_MOD, PP_AND, PP_OR, PP_XOR, PP_RSHIFT, PP_LSHIFT,
    PP_COMPL, PP_AND_AND, PP_OR_OR, PP_QUERY, PP_COLON, PP_COMMA,
    PP_OPEN_PAREN, PP_CLOSE_PAREN, PP_EQ_EQ, PP_NOT_EQ, PP_GREATER_EQ,
    PP_LESS_EQ, PP_PLUS_EQ, PP_MINUS_EQ, PP_MULT_EQ, PP_DIV_EQ, PP_MOD_EQ,
    PP_AND_EQ, PP_OR_EQ, PP_XOR_EQ, PP_RSHIFT_EQ, PP_LSHIFT_EQ, PP_HASH,
    PP_PASTE, PP_OPEN_SQUARE, PP_CLOSE_SQUARE, PP_OPEN_BRACE, PP_CLOSE_BRACE,
    PP_SEMICOLON, PP_ELLIPSIS, PP_PLUS_PLUS, PP_MINUS_MINUS, PP_DEREF, PP_DOT,
    PP_NAME, PP_NUMBER, PP_CHAR, PP_WCHAR, PP_STRING, PP_WSTRING, PP_HEADER,
    PP_OTHER, PP_COMMENT, PP_PADDING, PP_MACRO_ARG, PP_EOF
};

#define PP_LAST_EQ PP_LSHIFT
#define PP_LASTOP PP_DOT

// Directives
enum
{
    D_DEFINE, D_UNDEF, D_INCLUDE, D_INCLUDEONCE, D_UNINCLUDE, D_IF, D_IFDEF,
    D_IFNDEF, D_ELIF, D_ELSE, D_ENDIF, D_ERROR, D_WARNING, D_ENDDEF, D_REFUSED
};

typedef struct PPToken
{
    int type;
    int flags;
    char *textP;                // the spelling, splices removed
    int len;
    int line;                   // physical line and column of its first character
    int col;
    struct PPToken *sourceP;    // padding: whose PREV_WHITE decides the space; NULL avoids a paste
    int argNo;                  // PP_MACRO_ARG: the parameter, from 0
    struct PPChain *chainP;     // the expansion that made it; NULL for a token the program wrote
    int originLine;             // the body line its origin gives, when line is not it (a token
                                // ## made of arguments: its macro's line); else 0
} PPToken;

// One macro expansion, for the origins am1 writes to its .map: the macro, and
// where its name was, which is a line of the program when outerP is NULL,
// else a body line of the expansion outerP.
typedef struct PPChain
{
    struct PPMacro *macroP;
    int useLine;
    struct PPChain *outerP;
} PPChain;

typedef struct PPVec
{
    PPToken **vPP;
    int n;
    int cap;
} PPVec;

// One #include of one file, for #includeonce and #uninclude
typedef struct PPInclusion
{
    char *pathP;
    dev_t dev;
    ino_t ino;
    struct PPInclusion *parentP;
    bool active;                // counts as included
    bool reading;               // its buffer is on the stack
    struct PPInclusion *nextP;  // every inclusion, newest first
} PPInclusion;

// What gcc's multiple-include optimization knows of an include file: the
// macro that guards it, whose definition makes a later #include of it read
// nothing and write no markers.
// gcc keeps one of these per name as written and place its search began,
// shared when a "..." search goes on to the -I chain, where a <...> search begins.
typedef struct PPFile
{
    const char *guardP;         // NULL until known
} PPFile;

typedef struct PPFileKey
{
    char *nameP;                // as the directive wrote it
    char *startP;               // "/" absolute, "<" the -I chain, else 'q' and the includer's directory
    PPFile *fileP;
    struct PPFileKey *nextP;
} PPFileKey;

typedef struct PPCond
{
    int line;
    int col;
    int kind;                   // D_IF, D_IFDEF, D_IFNDEF, D_ELIF or D_ELSE: the last one seen
    bool wasSkipping;
    bool skipElses;
    const char *guardP;         // the macro an outermost #ifndef or #if !defined tests, at a file's top
    struct PPCond *prevP;
} PPCond;

// A file being read, and its current logical line: physical lines joined at
// backslash-newline, each character's physical line and column kept.
typedef struct PPBuffer
{
    char *dataP;
    size_t len;
    size_t pos;
    int nextLine;               // physical line number at dataP[pos]
    char *lineP;
    int *lineNoP;
    int *colP;
    int lineLen;
    int linePos;
    int lineCap;
    bool needLine;
    const char *nameP;          // as the markers name it
    char *dirP;                 // where its "..." includes are looked for, with its '/'
    int sysp;                   // a system header: its markers carry gcc's " 3 4"
    PPInclusion *incP;
    PPFile *fileP;              // NULL for the source file itself
    PPCond *condsP;
    struct PPBuffer *prevP;
} PPBuffer;

// One state of a macro name: a definition, or an #undef's record.
// prevP is the state it replaced, which #uninclude restores.
typedef struct PPMacro
{
    char *nameP;
    bool defined;
    bool funLike;
    int paramc;
    char **paramsPP;
    PPToken **bodyPP;
    int count;
    int diag;                   // PPD_ERROR or PPD_DIRWARNING when the body is #error or #warning, else -1
    const char *fileP;
    int line;
    int col;
    int sysp;
    PPInclusion *ownerP;        // the inclusion that made it; NULL for -D
    bool disabled;              // inside its own expansion
    struct PPMacro *prevP;
} PPMacro;

typedef struct PPName
{
    char *nameP;
    PPMacro *curP;
    struct PPName *nextP;
} PPName;

typedef struct PPContext
{
    PPToken **toksPP;
    int count;
    int pos;
    PPMacro *macroP;            // re-enabled when the context is popped
    PPChain *chainP;            // a macro's expansion: given to each body token read from it
    struct PPContext *prevP;
} PPContext;

// The statements of one output line, in order, each with its origin, for
// ppStatementOrigin().
typedef struct PPLineRec
{
    const char *fileP;          // the file and line the markers give the line
    int line;
    int count;
    const PPOrigin **originsPP; // NULL for a statement from no macro
} PPLineRec;

typedef struct PPArg
{
    PPToken **firstPP;          // as written, ended by an EOF
    int count;
    PPToken **expandedPP;
    int expCount;
    bool expandedDone;
} PPArg;

typedef struct PPDir
{
    char *nameP;
    int sysp;
} PPDir;

typedef struct PPOpt
{
    char type;
    char *valueP;
    struct PPOpt *nextP;
} PPOpt;

typedef struct PPVal
{
    intmax_t v;
    bool u;                     // unsigned, as C's #if arithmetic has it
} PPVal;

static const struct
{
    const char *nameP;
    int kind;
    bool cond;                  // obeyed while skipping
} directives[] =
{
    {"define", D_DEFINE, false},
    {"include", D_INCLUDE, false},
    {"includeonce", D_INCLUDEONCE, false},
    {"uninclude", D_UNINCLUDE, false},
    {"endif", D_ENDIF, true},
    {"ifdef", D_IFDEF, true},
    {"if", D_IF, true},
    {"else", D_ELSE, true},
    {"ifndef", D_IFNDEF, true},
    {"undef", D_UNDEF, false},
    {"elif", D_ELIF, true},
    {"error", D_ERROR, false},
    {"warning", D_WARNING, false},
    {"enddef", D_ENDDEF, false},
    {"line", D_REFUSED, false},
    {"pragma", D_REFUSED, false},
    {"include_next", D_REFUSED, false},
    {"import", D_REFUSED, false},
    {"ident", D_REFUSED, false},
    {"sccs", D_REFUSED, false},
    {"assert", D_REFUSED, false},
    {"unassert", D_REFUSED, false},
    {"elifdef", D_REFUSED, false},
    {"elifndef", D_REFUSED, false},
    {NULL, 0, false}
};

static FILE *outfP;
static PPBuffer *bufP;
static PPContext *ctxP;
static PPToken *lookaheadPP[PP_MAXLOOKAHEAD];
static int lookaheads;
static bool inDirective;
static int parsingArgs;         // 1 looking for a macro's '(', 2 collecting its arguments
static int preventExpansion;
static bool skipping;
static bool angledHeaders;
static int errorCount;
static int depth;
static int dirLine;             // the directive being obeyed
static int dirCol;
static int useLine;             // where the outermost macro being expanded was used
static int useCol;
static jmp_buf fatalJmp;
static PPToken avoidPasteTok;
static PPToken endArgTok;
static PPName *namesPP[PP_HASHSIZE];
static PPInclusion *inclusionsP;
static PPFileKey *fileKeysP;
static bool miValid;            // gcc's mi_valid: nothing yet but a possible guard
static const char *miGuardP;    // gcc's mi_cmacro: the guard just closed
static const char *ifGuardP;    // gcc's mi_ind_cmacro: an #if's !defined NAME
static PPDir *searchP;
static int nSearch;
static PPOpt *optsP;
static PPOpt **optTailPP = &optsP;

// The printer: gcc's c-ppoutput.c state
static struct
{
    int srcLine;
    const char *srcFileP;
    bool printed;               // a line has been started and not ended
    PPToken *prevP;
    PPToken *sourceP;
    bool avoidPaste;
} pr;

// What the printer records of each output line's statements, split where am1
// splits them, at ';' and the line's end.
// A statement's origin is that of its/ first token after any labels,
// since a label generates no word.
#define REC_START 0             // no token of the statement yet, or only labels
#define REC_NAME 1              // a name, which a comma would make a label
#define REC_DONE 2              // the origin's token is known

static struct
{
    bool open;                  // an output line is being recorded
    int state;
    PPToken *firstP;            // the statement's first token, NULL while it has none
    PPToken *originP;           // the token that gives its origin
} rec;

static PPLineRec *recsP;
static int nRecs;
static int recCap;
static int recCursor;           // where ppStatementOrigin() last found a line

static PPToken *lexToken(void);
static PPToken *getToken(void);
static bool handleDirective(PPToken *hashP);
static void doDefine(PPBuffer *defBufP);
static void recLine(void);
static void recToken(PPToken *tP);
static void recEndStatement(void);

// Allocate or die: am1 cannot go on without the memory.
static void *
ppAlloc(size_t size)
{
void *vP;

    if( !(vP = calloc(1, size)) )
    {
        fprintf(stderr, "am1: out of memory preprocessing\n");
        exit(1);
    }

    return( vP );
}

static char *
ppStrndup(const char *sP, size_t len)
{
char *dP;

    dP = ppAlloc(len + 1);
    memcpy(dP, sP, len);
    dP[len] = '\0';
    return( dP );
}

// Report a diagnostic in gcc's first-line form, file:line:col: kind: text.
// Warnings from a system header are dropped, as gcc drops them; #warning is not.
static void
ppDiagV(int kind, const char *fileP, int line, int col, int sysp, const char *fmtP, va_list ap)
{
static const char *kindsPP[] = {"error", "warning", "note", "fatal error", "warning"};

    if( (kind == PPD_WARNING) && sysp )
    {
        return;
    }

    if( fileP )
    {
        fprintf(stderr, "%s:", fileP);
        if( line > 0 )
        {
            fprintf(stderr, "%d:", line);
            if( col > 0 )
            {
                fprintf(stderr, "%d:", col);
            }
        }
        fputc(' ', stderr);
    }

    fprintf(stderr, "%s: ", kindsPP[kind]);
    vfprintf(stderr, fmtP, ap);
    fputc('\n', stderr);

    if( (kind == PPD_ERROR) || (kind == PPD_FATAL) )
    {
        ++errorCount;
    }
}

// A diagnostic at line and column of the file being read.
static void
diagHere(int kind, int line, int col, const char *fmtP, ...)
{
va_list ap;

    va_start(ap, fmtP);
    ppDiagV(kind, bufP->nameP, line, col, bufP->sysp, fmtP, ap);
    va_end(ap);
}

// A diagnostic at a token: its own place when it was read from the file, else
// the place its outermost macro was used.
static void
diagTok(int kind, PPToken *tP, const char *fmtP, ...)
{
va_list ap;

    va_start(ap, fmtP);
    if( ctxP || !tP )
    {
        ppDiagV(kind, bufP->nameP, useLine, useCol, bufP->sysp, fmtP, ap);
    }
    else
    {
        ppDiagV(kind, bufP->nameP, tP->line, tP->col, bufP->sysp, fmtP, ap);
    }
    va_end(ap);
}

static void
vecPush(PPVec *vP, PPToken *tP)
{
    if( vP->n >= vP->cap )
    {
        vP->cap = (vP->cap)?(vP->cap * 2):8;
        if( !(vP->vPP = realloc(vP->vPP, (vP->cap * sizeof(PPToken *)))) )
        {
            fprintf(stderr, "am1: out of memory preprocessing\n");
            exit(1);
        }
    }

    vP->vPP[vP->n++] = tP;
}

static PPToken *
newTok(int type, const char *textP, int len, int line, int col, int flags)
{
PPToken *tP;

    tP = ppAlloc(sizeof(PPToken));
    tP->type = type;
    tP->textP = ppStrndup(textP, len);
    tP->len = len;
    tP->line = line;
    tP->col = col;
    tP->flags = flags;
    return( tP );
}

static PPToken *
copyTok(PPToken *tP, int flags)
{
PPToken *nP;

    nP = ppAlloc(sizeof(PPToken));
    *nP = *tP;
    nP->flags = flags;
    return( nP );
}

// gcc's padding token: no text, but source's PREV_WHITE decides a space.
static PPToken *
padding(PPToken *sourceP)
{
PPToken *tP;

    tP = ppAlloc(sizeof(PPToken));
    tP->type = PP_PADDING;
    tP->textP = "";
    tP->sourceP = sourceP;
    return( tP );
}

static PPToken *
makeEof(int flags)
{
PPToken *tP;

    tP = ppAlloc(sizeof(PPToken));
    tP->type = PP_EOF;
    tP->textP = "";
    tP->flags = flags;
    return( tP );
}

// The message of #error or #warning: the directive's
// name (toksPP[0]), a blank, then the rest of the tokens, one blank apart
// where blanks were with leading and trailing quotes removed.
static char *
diagText(PPToken **toksPP, int n)
{
int i;
bool first;
char *textP, *cP, *cP2;
PPToken *sourceP;
size_t len;

    for( len = 3, i = 1; i < n; ++i )
    {
        len += toksPP[i]->len + 1;
    }

    textP = ppAlloc(len);
    first = true;
    sourceP = NULL;
    for( i = 1; i < n; ++i )
    {
        if( toksPP[i]->type == PP_PADDING )
        {
            if( !sourceP || (!(sourceP->flags & TF_PREV_WHITE) && !toksPP[i]->sourceP) )
            {
                sourceP = toksPP[i]->sourceP;
            }
            continue;
        }

        if( !sourceP )
        {
            sourceP = toksPP[i];
        }

        if( !first && (sourceP->flags & TF_PREV_WHITE) )
        {
            strcat(textP, " ");
        }

        // Skip single and double quotes around a token.
        cP = toksPP[i]->textP;
        if( strchr("\"'", *cP) )
        {
            ++cP;
        }

        cP2 = cP + strlen(cP) - 1;
        if( (cP2 >= cP) && strrchr("\"'", *cP2) )
        {
            *cP2 = 0;
        }

        cP = 
        strncat(textP, cP, strlen(cP));
        first = false;
        sourceP = NULL;
    }

    return( textP );
}

// Read a whole file, ended with a newline if it lacks one.
// NULL if it cannot be read, errno saying why.
static char *
readFile(const char *pathP, size_t *lenP)
{
FILE *fP;
char *dataP;
size_t len, got;
struct stat st;

    if( !(fP = fopen(pathP, "rb")) )
    {
        return( NULL );
    }

    if( fstat(fileno(fP), &st) || !S_ISREG(st.st_mode) )
    {
        fclose(fP);
        errno = EISDIR;
        return( NULL );
    }

    len = st.st_size;
    dataP = ppAlloc(len + 2);
    got = fread(dataP, 1, len, fP);
    fclose(fP);

    if( got != len )
    {
        errno = EIO;
        return( NULL );
    }

    if( (len > 0) && (dataP[len - 1] != '\n') && (dataP[len - 1] != '\r') )
    {
        dataP[len++] = '\n';
    }

    *lenP = len;
    return( dataP );
}

// The directory a file's "..." includes are looked for in: its path up to and
// including the last '/', or "" for a bare name, as gcc forms it.
static char *
dirOf(const char *pathP)
{
const char *sP;

    sP = strrchr(pathP, '/');
    return( ppStrndup(pathP, (sP)?((size_t)(sP - pathP) + 1):0) );
}

static PPBuffer *
newBuffer(char *dataP, size_t len, const char *nameP, int sysp)
{
PPBuffer *bP;

    bP = ppAlloc(sizeof(PPBuffer));
    bP->dataP = dataP;
    bP->len = len;
    bP->nextLine = 1;
    bP->needLine = true;
    bP->nameP = nameP;
    bP->dirP = dirOf(nameP);
    bP->sysp = sysp;
    return( bP );
}

static void
lineAppend(PPBuffer *bP, char c, int line, int col)
{
    if( bP->lineLen >= bP->lineCap )
    {
        bP->lineCap = (bP->lineCap)?(bP->lineCap * 2):256;
        bP->lineP = realloc(bP->lineP, bP->lineCap);
        bP->lineNoP = realloc(bP->lineNoP, (bP->lineCap * sizeof(int)));
        bP->colP = realloc(bP->colP, (bP->lineCap * sizeof(int)));
        if( !bP->lineP || !bP->lineNoP || !bP->colP )
        {
            fprintf(stderr, "am1: out of memory preprocessing\n");
            exit(1);
        }
    }

    bP->lineP[bP->lineLen] = c;
    bP->lineNoP[bP->lineLen] = line;
    bP->colP[bP->lineLen] = col;
    ++bP->lineLen;
}

// Blanks other than newline, as gcc's is_nvspace has them; a NUL is one.
static bool
isNvspace(char c)
{
    return( (c == ' ') || (c == '\t') || (c == '\f') || (c == '\v') || (c == '\0') );
}

// Make the next logical line current: physical lines are joined where one
// ends in a backslash, blanks after the backslash allowed with a warning, as
// gcc's _cpp_clean_line does.
// A CR, alone or before LF, ends a line.
// False at the end of the file.
static bool
fetchLine(PPBuffer *bP)
{
size_t p;
int line, col, k;
char c;

    if( bP->pos >= bP->len )
    {
        return( false );
    }

    bP->lineLen = 0;
    bP->linePos = 0;
    line = bP->nextLine;
    col = 1;
    p = bP->pos;

    while( p < bP->len )
    {
        c = bP->dataP[p];
        if( (c == '\n') || (c == '\r') )
        {
            if( (c == '\r') && ((p + 1) < bP->len) && (bP->dataP[p + 1] == '\n') )
            {
                ++p;
            }
            ++p;

            for( k = bP->lineLen; (k > 0) && isNvspace(bP->lineP[k - 1]); --k )
            {
            }

            if( (k > 0) && (bP->lineP[k - 1] == '\\') && (p < bP->len) )
            {
                if( k != bP->lineLen )
                {
                    diagHere(PPD_WARNING, line, bP->colP[k - 1], "backslash and newline separated by space");
                }

                bP->lineLen = k - 1;
                ++line;
                col = 1;
                continue;
            }

            ++line;
            break;
        }

        lineAppend(bP, c, line, col);
        ++col;
        ++p;
    }

    bP->pos = p;
    bP->nextLine = line;
    return( true );
}

// The character at offset ahead of the line position, or -1 past its end.
static int
peekc(PPBuffer *bP, int ahead)
{
int i;

    i = bP->linePos + ahead;
    return( (i < bP->lineLen)?(unsigned char)bP->lineP[i]:-1 );
}

static bool
isIdStart(int c)
{
    return( ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_') || (c == '$') );
}

static bool
isIdChar(int c)
{
    return( isIdStart(c) || ((c >= '0') && (c <= '9')) );
}

static bool
isDigit(int c)
{
    return( (c >= '0') && (c <= '9') );
}

// A hex digit's value, or -1.
static int
hexValue(int c)
{
    if( isDigit(c) )
    {
        return( c - '0' );
    }

    if( (c >= 'a') && (c <= 'f') )
    {
        return( (c - 'a') + 10 );
    }

    if( (c >= 'A') && (c <= 'F') )
    {
        return( (c - 'A') + 10 );
    }

    return( -1 );
}

// Scan a quoted token from just after its opening quote to its terminator.
// Unterminated, it runs to the end of the line and is PP_OTHER, as gcc's
// assembler mode has it; an unterminated <...> is just the '<'.
// Returns the token type and leaves the line position after the token.
static int
scanQuoted(PPBuffer *bP, int term, int type)
{
int c;

    for(;;)
    {
        if( bP->linePos >= bP->lineLen )
        {
            return( (term == '>')?-1:PP_OTHER );
        }

        c = (unsigned char)bP->lineP[bP->linePos++];
        if( (c == '\\') && !angledHeaders && (bP->linePos < bP->lineLen) )
        {
            ++bP->linePos;
        }
        else if( c == term )
        {
            return( type );
        }
    }
}

// The rest of a block comment that runs past its line: later lines are read
// into it, each after a newline, until the "*/".
static PPToken *
lexBlockComment(PPBuffer *bP, int start, int flags)
{
char *textP;
size_t len, cap;
int line, col;
bool closed;

    line = bP->lineNoP[start];
    col = bP->colP[start];
    cap = 256;
    textP = ppAlloc(cap);
    len = 0;
    bP->linePos = start + 2;
    closed = false;

    for(;;)
    {
        while( bP->linePos < bP->lineLen )
        {
            if( (bP->lineP[bP->linePos] == '*') && (peekc(bP, 1) == '/') )
            {
                bP->linePos += 2;
                closed = true;
                break;
            }
            ++bP->linePos;
        }

        // copy what this line holds of the comment
        if( (len + (bP->linePos - start) + 2) >= cap )
        {
            cap = (cap * 2) + (bP->linePos - start) + 2;
            textP = realloc(textP, cap);
        }
        memcpy(textP + len, bP->lineP + start, (bP->linePos - start));
        len += (bP->linePos - start);

        if( closed )
        {
            break;
        }

        textP[len++] = '\n';
        if( !fetchLine(bP) )
        {
            diagHere(PPD_ERROR, line, col, "unterminated comment");
            bP->needLine = true;
            break;
        }
        start = 0;
    }

    return( newTok(PP_COMMENT, textP, len, line, col, flags) );
}

// gcc's _cpp_lex_direct: the next token of the current buffer.
// In a directive the end of the line is an EOF, elsewhere the next line is read,
// its first token flagged BOL, and the end of the file is an EOF flagged
// TF_FILE_END.
static PPToken *
lexDirect(void)
{
PPBuffer *bP;
PPToken *tP;
int flags, start, type, c, n1, n2, len, white;
bool saveComments, sawNul;

    bP = bufP;
    flags = 0;
    saveComments = !inDirective;

    for(;;)
    {
        if( bP->needLine )
        {
            if( inDirective )
            {
                tP = makeEof(0);
                tP->line = bP->nextLine - 1;
                return( tP );
            }

            if( !fetchLine(bP) )
            {
                tP = makeEof(TF_FILE_END | TF_BOL);
                tP->line = bP->nextLine;
                return( tP );
            }

            bP->needLine = false;
            flags = TF_BOL;
            if( parsingArgs == 2 )
            {
                flags |= TF_PREV_WHITE;     // a newline inside arguments is a blank
            }
        }

        // gcc warns once for each run of blanks holding a NUL, at its start
        white = bP->linePos;
        sawNul = false;
        while( (bP->linePos < bP->lineLen) && isNvspace(bP->lineP[bP->linePos]) )
        {
            flags |= TF_PREV_WHITE;
            if( bP->lineP[bP->linePos] == '\0' )
            {
                sawNul = true;
            }
            ++bP->linePos;
        }

        if( sawNul )
        {
            diagHere(PPD_WARNING, bP->lineNoP[white], bP->colP[white], "null character(s) ignored");
        }

        if( bP->linePos >= bP->lineLen )
        {
            bP->needLine = true;
            continue;
        }

        start = bP->linePos;
        c = (unsigned char)bP->lineP[start];

        if( (c == '/') && (peekc(bP, 1) == '*') )
        {
            tP = lexBlockComment(bP, start, flags);
            if( saveComments )
            {
                return( tP );
            }
            flags |= TF_PREV_WHITE;
            continue;
        }

        if( (c == '/') && (peekc(bP, 1) == '/') )
        {
            bP->linePos = bP->lineLen;
            if( !saveComments )
            {
                flags |= TF_PREV_WHITE;
                continue;
            }

            len = bP->lineLen - start;

            // gcc keeps a // comment read while it gathers a macro's
            // arguments as a /* */ comment, and writes it so
            if( parsingArgs )
            {
                tP = newTok(PP_COMMENT, bP->lineP + start, len, bP->lineNoP[start], bP->colP[start], flags);
                tP->textP = ppAlloc(len + 3);
                tP->textP[0] = '/';
                tP->textP[1] = '*';
                memcpy(tP->textP + 2, bP->lineP + start + 2, (len - 2));
                tP->textP[len] = '*';
                tP->textP[len + 1] = '/';
                tP->len = len + 2;
                return( tP );
            }

            return( newTok(PP_COMMENT, bP->lineP + start, len, bP->lineNoP[start], bP->colP[start], flags) );
        }

        break;
    }

    ++bP->linePos;
    n1 = peekc(bP, 0);
    n2 = peekc(bP, 1);
    type = PP_OTHER;

    if( isDigit(c) || ((c == '.') && isDigit(n1)) )
    {
        // a pp-number: digits, letters, '_', '.', and a sign after e or E
        type = PP_NUMBER;
        for(;;)
        {
            n1 = peekc(bP, 0);
            if( isIdChar(n1) || (n1 == '.') )
            {
                ++bP->linePos;
            }
            else if( ((n1 == '+') || (n1 == '-')) &&
                     ((bP->lineP[bP->linePos - 1] == 'e') || (bP->lineP[bP->linePos - 1] == 'E')) )
            {
                ++bP->linePos;
            }
            else
            {
                break;
            }
        }
    }
    else if( isIdStart(c) )
    {
        while( isIdChar(peekc(bP, 0)) )
        {
            ++bP->linePos;
        }

        type = PP_NAME;
        if( ((bP->linePos - start) == 1) && (c == 'L') && ((peekc(bP, 0) == '\'') || (peekc(bP, 0) == '"')) )
        {
            n1 = peekc(bP, 0);
            ++bP->linePos;
            type = scanQuoted(bP, n1, (n1 == '\'')?PP_WCHAR:PP_WSTRING);
        }
    }
    else
    {
        switch( c )
        {
        case '\'':
            type = scanQuoted(bP, '\'', PP_CHAR);
            break;

        case '"':
            type = scanQuoted(bP, '"', PP_STRING);
            break;

        case '<':
            if( angledHeaders )
            {
                type = scanQuoted(bP, '>', PP_HEADER);
                if( type < 0 )
                {
                    bP->linePos = start + 1;
                    type = PP_LESS;
                }
            }
            else if( n1 == '<' )
            {
                bP->linePos += (n2 == '=')?2:1;
                type = (n2 == '=')?PP_LSHIFT_EQ:PP_LSHIFT;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_LESS_EQ;
            }
            else
            {
                type = PP_LESS;
            }
            break;

        case '>':
            if( n1 == '>' )
            {
                bP->linePos += (n2 == '=')?2:1;
                type = (n2 == '=')?PP_RSHIFT_EQ:PP_RSHIFT;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_GREATER_EQ;
            }
            else
            {
                type = PP_GREATER;
            }
            break;

        case '+':
            type = PP_PLUS;
            if( n1 == '+' )
            {
                ++bP->linePos;
                type = PP_PLUS_PLUS;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_PLUS_EQ;
            }
            break;

        case '-':
            type = PP_MINUS;
            if( n1 == '-' )
            {
                ++bP->linePos;
                type = PP_MINUS_MINUS;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_MINUS_EQ;
            }
            else if( n1 == '>' )
            {
                ++bP->linePos;
                type = PP_DEREF;
            }
            break;

        case '&':
            type = PP_AND;
            if( n1 == '&' )
            {
                ++bP->linePos;
                type = PP_AND_AND;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_AND_EQ;
            }
            break;

        case '|':
            type = PP_OR;
            if( n1 == '|' )
            {
                ++bP->linePos;
                type = PP_OR_OR;
            }
            else if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_OR_EQ;
            }
            break;

        case '*':
            type = PP_MULT;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_MULT_EQ;
            }
            break;

        case '/':
            type = PP_DIV;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_DIV_EQ;
            }
            break;

        case '%':
            type = PP_MOD;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_MOD_EQ;
            }
            break;

        case '^':
            type = PP_XOR;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_XOR_EQ;
            }
            break;

        case '=':
            type = PP_EQ;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_EQ_EQ;
            }
            break;

        case '!':
            type = PP_NOT;
            if( n1 == '=' )
            {
                ++bP->linePos;
                type = PP_NOT_EQ;
            }
            break;

        case '#':
            type = PP_HASH;
            if( n1 == '#' )
            {
                ++bP->linePos;
                type = PP_PASTE;
            }
            break;

        case '.':
            type = PP_DOT;
            if( (n1 == '.') && (n2 == '.') )
            {
                bP->linePos += 2;
                type = PP_ELLIPSIS;
            }
            break;

        case ':':
            type = PP_COLON;
            break;

        case '?':
            type = PP_QUERY;
            break;

        case '~':
            type = PP_COMPL;
            break;

        case ',':
            type = PP_COMMA;
            break;

        case '(':
            type = PP_OPEN_PAREN;
            break;

        case ')':
            type = PP_CLOSE_PAREN;
            break;

        case '[':
            type = PP_OPEN_SQUARE;
            break;

        case ']':
            type = PP_CLOSE_SQUARE;
            break;

        case '{':
            type = PP_OPEN_BRACE;
            break;

        case '}':
            type = PP_CLOSE_BRACE;
            break;

        case ';':
            type = PP_SEMICOLON;
            break;

        default:
            type = PP_OTHER;
            break;
        }
    }

    return( newTok(type, bP->lineP + start, (bP->linePos - start), bP->lineNoP[start], bP->colP[start], flags) );
}

// Write a file name as gcc's markers quote it.
static void
writeQuoted(const char *sP)
{
    for( ; *sP; ++sP )
    {
        if( (*sP == '\\') || (*sP == '"') )
        {
            putc('\\', outfP);
            putc(*sP, outfP);
        }
        else if( *sP == '\n' )
        {
            fputs("\\n", outfP);
        }
        else
        {
            putc(*sP, outfP);
        }
    }
}

// gcc's print_line: end any line begun, and write a marker.
static void
printLine(int line, const char *fileP, const char *flagsP, int sysp)
{
    if( pr.printed )
    {
        putc('\n', outfP);
    }

    pr.printed = false;
    pr.srcLine = line;
    pr.srcFileP = fileP;
    fprintf(outfP, "# %d \"", line);
    writeQuoted(fileP);
    fprintf(outfP, "\"%s%s\n", flagsP, (sysp)?" 3 4":"");
}

// gcc's maybe_print_line: bring the output to line of fileP, with blank lines
// for a short gap forward in the same file, else with a marker.
static void
maybePrintLine(int line, const char *fileP, int sysp)
{
    if( pr.printed )
    {
        putc('\n', outfP);
        ++pr.srcLine;
        pr.printed = false;
    }

    if( (line >= pr.srcLine) && (line < (pr.srcLine + PP_MAXGAP)) && !strcmp(fileP, pr.srcFileP) )
    {
        while( line > pr.srcLine )
        {
            putc('\n', outfP);
            ++pr.srcLine;
        }
    }
    else
    {
        printLine(line, fileP, "", sysp);
    }
}

// gcc's cb_line_change, for the first token of a line: start its output line,
// with one space per column past the second, since the token's PREV_WHITE
// supplies the last.
static void
lineChange(PPToken *tP)
{
int spaces;

    if( (tP->type == PP_EOF) || parsingArgs )
    {
        return;
    }

    maybePrintLine(tP->line, bufP->nameP, bufP->sysp);
    pr.prevP = NULL;
    pr.sourceP = NULL;
    pr.printed = true;
    recLine();

    for( spaces = tP->col - 2; spaces > 0; --spaces )
    {
        putc(' ', outfP);
    }
}

// gcc's cpp_avoid_paste: true when a and b written together would read back
// as other tokens.
static bool
avoidPaste(PPToken *aP, PPToken *bP)
{
int a, b, c;

    a = aP->type;
    b = bP->type;
    c = (b <= PP_LASTOP)?(unsigned char)bP->textP[0]:-1;

    if( (a <= PP_LAST_EQ) && (c == '=') )
    {
        return( true );
    }

    switch( a )
    {
    case PP_GREATER:
        return( c == '>' );
    case PP_LESS:
        return( (c == '<') || (c == '%') || (c == ':') );
    case PP_PLUS:
        return( c == '+' );
    case PP_MINUS:
        return( (c == '-') || (c == '>') );
    case PP_DIV:
        return( (c == '/') || (c == '*') );
    case PP_MOD:
        return( (c == ':') || (c == '%') );
    case PP_AND:
        return( c == '&' );
    case PP_OR:
        return( c == '|' );
    case PP_COLON:
        return( (c == ':') || (c == '>') );
    case PP_DEREF:
        return( c == '*' );
    case PP_DOT:
        return( (c == '.') || (c == '%') || (b == PP_NUMBER) );
    case PP_HASH:
        return( (c == '#') || (c == '%') );
    case PP_NAME:
        return( (b == PP_NAME) || (b == PP_CHAR) || (b == PP_STRING) );
    case PP_NUMBER:
        return( (b == PP_NUMBER) || (b == PP_NAME) || (b == PP_CHAR) || (c == '.') || (c == '+') || (c == '-') );
    case PP_OTHER:
        return( (aP->textP[0] == '\\') && (b == PP_NAME) );
    case PP_LESS_EQ:
        return( c == '>' );
    default:
        break;
    }

    return( false );
}

// gcc's token streamer: write a token, with a space before it only where gcc puts one.
// Padding writes nothing but decides the next token's space.
static void
streamToken(PPToken *tP)
{
PPToken *srcP;
int i;

    if( tP->type == PP_PADDING )
    {
        pr.avoidPaste = true;
        if( !pr.sourceP || (!(pr.sourceP->flags & TF_PREV_WHITE) && !tP->sourceP) )
        {
            pr.sourceP = tP->sourceP;
        }
        return;
    }

    if( tP->type == PP_EOF )
    {
        return;
    }

    if( pr.avoidPaste )
    {
        srcP = (pr.sourceP)?pr.sourceP:tP;
        if( (srcP->flags & TF_PREV_WHITE) || (pr.prevP && avoidPaste(pr.prevP, tP)) ||
            (!pr.prevP && (tP->type == PP_HASH)) )
        {
            putc(' ', outfP);
        }
    }
    else if( tP->flags & TF_PREV_WHITE )
    {
        putc(' ', outfP);
    }

    pr.avoidPaste = false;
    pr.sourceP = NULL;
    pr.prevP = tP;
    pr.printed = true;
    fwrite(tP->textP, 1, tP->len, outfP);

    // a block comment's newlines move the output's line
    if( tP->type == PP_COMMENT )
    {
        for( i = 0; i < tP->len; ++i )
        {
            if( tP->textP[i] == '\n' )
            {
                ++pr.srcLine;
            }
        }
    }

    recToken(tP);
}

// The origin of a statement whose first token, after its labels, is tP: NULL
// for a token the program wrote, else each expansion that made it, outermost
// first, with the body line it came from.
static const PPOrigin *
originOf(PPToken *tP)
{
PPOrigin *oP;
PPChain *cP;
int n, line;

    if( !tP->chainP )
    {
        return( NULL );
    }

    for( n = 0, cP = tP->chainP; cP; cP = cP->outerP )
    {
        ++n;
    }

    oP = ppAlloc(sizeof(PPOrigin));
    oP->count = n;
    oP->levelsP = ppAlloc(n * sizeof(PPOriginLevel));

    // the innermost expansion's body line is the token's own; each outer one's
    // is where the inner macro's name was
    line = (tP->originLine)?tP->originLine:tP->line;
    for( cP = tP->chainP; cP; cP = cP->outerP )
    {
        --n;
        oP->levelsP[n].macroP = cP->macroP->nameP;
        oP->levelsP[n].fileP = cP->macroP->fileP;
        oP->levelsP[n].line = line;
        line = cP->useLine;
    }

    return( oP );
}

// End the statement being recorded, adding it to its line if it has a token.
static void
recEndStatement(void)
{
PPLineRec *lP;

    if( rec.open && rec.firstP )
    {
        lP = &recsP[nRecs - 1];
        lP->originsPP = realloc(lP->originsPP, ((lP->count + 1) * sizeof(PPOrigin *)));
        if( !lP->originsPP )
        {
            fprintf(stderr, "am1: out of memory preprocessing\n");
            exit(1);
        }
        lP->originsPP[lP->count++] = originOf((rec.originP)?rec.originP:rec.firstP);
    }

    rec.firstP = NULL;
    rec.originP = NULL;
    rec.state = REC_START;
}

// Start recording the output line the printer is on.
static void
recLine(void)
{
    recEndStatement();

    if( nRecs >= recCap )
    {
        recCap = (recCap)?(recCap * 2):1024;
        recsP = realloc(recsP, (recCap * sizeof(PPLineRec)));
        if( !recsP )
        {
            fprintf(stderr, "am1: out of memory preprocessing\n");
            exit(1);
        }
    }

    memset(&recsP[nRecs], 0, sizeof(PPLineRec));
    recsP[nRecs].fileP = pr.srcFileP;
    recsP[nRecs].line = pr.srcLine;
    ++nRecs;
    rec.open = true;
}

// Record a token the printer has written.
static void
recToken(PPToken *tP)
{
    if( !rec.open )
    {
        return;
    }

    if( tP->type == PP_COMMENT )
    {
        // after a block comment's newlines the printer is on a later line
        if( memchr(tP->textP, '\n', tP->len) )
        {
            recLine();
        }
        return;
    }

    if( tP->type == PP_SEMICOLON )
    {
        recEndStatement();
        return;
    }

    if( !rec.firstP )
    {
        rec.firstP = tP;
    }

    switch( rec.state )
    {
    case REC_START:
        rec.originP = tP;
        rec.state = (tP->type == PP_NAME)?REC_NAME:REC_DONE;
        break;

    case REC_NAME:
        if( tP->type == PP_COMMA )
        {
            rec.originP = NULL;     // a label: the origin is what follows it
            rec.state = REC_START;
        }
        else
        {
            rec.state = REC_DONE;
        }
        break;

    default:
        break;
    }
}

const PPOrigin *
ppStatementOrigin(const char *fileP, int line, int ordinal)
{
int i;

    // am1 reads the lines in the order they were recorded, skipping those it
    // takes no statement from (the title, a text table's lines), so the line
    // is the one found or a few after it.
    // One not found is given no origin and the location is kept.
    for( i = recCursor; (i < nRecs) && (i < (recCursor + PP_MAXSCAN)); ++i )
    {
        if( (recsP[i].line == line) && fileP && !strcmp(recsP[i].fileP, fileP) )
        {
            recCursor = i;
            return( (ordinal < recsP[i].count)?recsP[i].originsPP[ordinal]:NULL );
        }
    }

    return( NULL );
}

static unsigned
hashName(const char *sP)
{
unsigned h;

    for( h = 5381; *sP; ++sP )
    {
        h = (h * 33) + (unsigned char)*sP;
    }

    return( h % PP_HASHSIZE );
}

// The slot of a name, made if absent.
static PPName *
nameSlot(const char *nameP)
{
PPName *sP;
unsigned h;

    h = hashName(nameP);
    for( sP = namesPP[h]; sP; sP = sP->nextP )
    {
        if( !strcmp(sP->nameP, nameP) )
        {
            return( sP );
        }
    }

    sP = ppAlloc(sizeof(PPName));
    sP->nameP = ppStrndup(nameP, strlen(nameP));
    sP->nextP = namesPP[h];
    namesPP[h] = sP;
    return( sP );
}

// The macro a name is defined as, or NULL.
static PPMacro *
findMacro(const char *nameP)
{
PPName *sP;

    for( sP = namesPP[hashName(nameP)]; sP; sP = sP->nextP )
    {
        if( !strcmp(sP->nameP, nameP) )
        {
            return( (sP->curP && sP->curP->defined)?sP->curP:NULL );
        }
    }

    return( NULL );
}

static void
pushContext(PPToken **toksPP, int count, PPMacro *macroP)
{
PPContext *cP;

    cP = ppAlloc(sizeof(PPContext));
    cP->toksPP = toksPP;
    cP->count = count;
    cP->macroP = macroP;
    cP->prevP = ctxP;
    ctxP = cP;
}

static void
pushContext1(PPToken *tP)
{
PPToken **toksPP;

    toksPP = ppAlloc(sizeof(PPToken *));
    toksPP[0] = tP;
    pushContext(toksPP, 1, NULL);
}

static void
popContext(void)
{
    if( ctxP->macroP )
    {
        ctxP->macroP->disabled = false;
    }

    ctxP = ctxP->prevP;
}

// Give a token back, to be read again next: gcc's _cpp_backup_tokens.
static void
backup(PPToken *tP)
{
    if( ctxP )
    {
        --ctxP->pos;
    }
    else if( lookaheads < PP_MAXLOOKAHEAD )
    {
        lookaheadPP[lookaheads++] = tP;
    }
}

// Paste lhs and rhs into one token, as gcc's paste_tokens: their spellings
// joined must lex as exactly one token.
// On failure, which gcc's assembler mode does not report, lhs stays, its ## dropped.
static bool
pasteTokens(PPToken **lhsPP, PPToken *rhsP)
{
PPBuffer tmp;
PPBuffer *savedP;
PPToken *lhsP, *tP;
char *sP;
int len, i;
bool ok;

    lhsP = *lhsPP;
    sP = ppAlloc(lhsP->len + rhsP->len + 2);
    memcpy(sP, lhsP->textP, lhsP->len);
    len = lhsP->len;

    // gcc puts a blank after a '/' so that no comment can start
    if( (lhsP->type == PP_DIV) && (rhsP->type != PP_EQ) )
    {
        sP[len++] = ' ';
    }

    memcpy(sP + len, rhsP->textP, rhsP->len);
    len += rhsP->len;

    memset(&tmp, 0, sizeof(tmp));
    tmp.lineP = sP;
    tmp.lineLen = len;
    tmp.lineCap = len;
    tmp.lineNoP = ppAlloc((len + 1) * sizeof(int));
    tmp.colP = ppAlloc((len + 1) * sizeof(int));
    for( i = 0; i < len; ++i )
    {
        tmp.lineNoP[i] = lhsP->line;
        tmp.colP[i] = lhsP->col;
    }
    tmp.nameP = bufP->nameP;
    tmp.sysp = bufP->sysp;

    savedP = bufP;
    bufP = &tmp;
    tP = lexDirect();
    ok = (tmp.linePos >= tmp.lineLen);
    bufP = savedP;

    if( !ok )
    {
        *lhsPP = copyTok(lhsP, (lhsP->flags & ~TF_PASTE_LEFT));
        return( false );
    }

    tP->flags = (lhsP->flags & TF_PREV_WHITE);
    tP->line = lhsP->line;
    tP->col = lhsP->col;

    // the ## in the body made it, so it is the expansion's, even from two
    // arguments; its line is then a caller's, so its origin names the macro's line.
    // The ## itself is gone by now, so in a block that is the #define's.
    tP->chainP = lhsP->chainP;
    tP->originLine = lhsP->originLine;
    if( ctxP && ctxP->chainP && (ctxP->chainP != lhsP->chainP) )
    {
        tP->chainP = ctxP->chainP;
        tP->originLine = ctxP->chainP->macroP->line;
    }
    *lhsPP = tP;
    return( true );
}

// gcc's paste_all_tokens: paste lhs with what follows it in the current
// context while each ## lasts, and make the result a context of its own.
static void
pasteAll(PPToken *lhsP)
{
PPToken *rhsP;

    do
    {
        if( ctxP->pos >= ctxP->count )
        {
            break;
        }

        rhsP = ctxP->toksPP[ctxP->pos++];

        // as gcc's: the loop's test then reads the padding's flags, and ends
        if( rhsP->type == PP_PADDING )
        {
            continue;
        }

        if( !pasteTokens(&lhsP, rhsP) )
        {
            --ctxP->pos;
            break;
        }
    } while( rhsP->flags & TF_PASTE_LEFT );

    pushContext1(lhsP);
}

// gcc's expand_arg: an argument with its macros expanded, done once.
static void
expandArg(PPArg *argP)
{
PPVec v;
PPToken *tP;

    if( argP->expandedDone )
    {
        return;
    }

    argP->expandedDone = true;
    memset(&v, 0, sizeof(v));
    pushContext(argP->firstPP, (argP->count + 1), NULL);

    for(;;)
    {
        tP = getToken();
        if( tP->type == PP_EOF )
        {
            break;
        }
        vecPush(&v, tP);
    }

    popContext();
    argP->expandedPP = v.vPP;
    argP->expCount = v.n;
}

// gcc's collect_args, after a function-like macro's '(': its arguments, split
// at the commas outside parentheses.
// NULL after reporting an error.
static PPArg *
collectArgs(PPMacro *mP)
{
PPArg *argsP;
PPVec v;
PPToken *tP;
int argc, nAlloc, parens;

    nAlloc = (mP->paramc > 0)?mP->paramc:1;
    argsP = ppAlloc(nAlloc * sizeof(PPArg));
    argc = 0;

    do
    {
        ++argc;
        memset(&v, 0, sizeof(v));
        parens = 0;

        for(;;)
        {
            tP = getToken();
            if( tP->type == PP_PADDING )
            {
                if( v.n == 0 )
                {
                    continue;       // leading padding is dropped
                }
            }
            else if( tP->type == PP_OPEN_PAREN )
            {
                ++parens;
            }
            else if( tP->type == PP_CLOSE_PAREN )
            {
                if( parens-- == 0 )
                {
                    break;
                }
            }
            else if( tP->type == PP_COMMA )
            {
                if( parens == 0 )
                {
                    break;
                }
            }
            else if( (tP->type == PP_EOF) || ((tP->type == PP_HASH) && (tP->flags & TF_BOL)) )
            {
                break;
            }

            vecPush(&v, tP);
        }

        while( (v.n > 0) && (v.vPP[v.n - 1]->type == PP_PADDING) )
        {
            --v.n;          // and trailing padding
        }

        if( argc <= nAlloc )
        {
            vecPush(&v, &endArgTok);
            argsP[argc - 1].firstPP = v.vPP;
            argsP[argc - 1].count = v.n - 1;
        }
    } while( (tP->type != PP_CLOSE_PAREN) && (tP->type != PP_EOF) && (tP->type != PP_HASH) );

    if( tP->type != PP_CLOSE_PAREN )
    {
        // the EOF must still end a directive or an argument being expanded
        if( ctxP || inDirective || (tP->type == PP_HASH) )
        {
            backup(tP);
        }
        diagTok(PPD_ERROR, NULL, "unterminated argument list invoking macro '%s'", mP->nameP);
        return( NULL );
    }

    // one empty argument is none
    if( (argc == 1) && (mP->paramc == 0) && (argsP[0].count == 0) )
    {
        argc = 0;
    }

    if( argc < mP->paramc )
    {
        diagTok(PPD_ERROR, NULL, "macro '%s' requires %d arguments, but only %d given", mP->nameP, mP->paramc, argc);
        return( NULL );
    }

    if( argc > mP->paramc )
    {
        diagTok(PPD_ERROR, NULL, "macro '%s' passed %d arguments, but takes just %d", mP->nameP, argc, mP->paramc);
        return( NULL );
    }

    return( argsP );
}

// gcc's funlike_invocation_p: whether a '(' follows a function-like macro's
// name, padding skipped.
// If not, the token read is given back, and the padding put before it.
static PPArg *
funlikeInvocation(PPMacro *mP)
{
PPToken *tP, *padP;

    padP = NULL;
    for(;;)
    {
        tP = getToken();
        if( tP->type != PP_PADDING )
        {
            break;
        }

        if( !padP || !padP->sourceP || (!(padP->sourceP->flags & TF_PREV_WHITE) && !tP->sourceP) )
        {
            padP = tP;
        }
    }

    if( tP->type == PP_OPEN_PAREN )
    {
        parsingArgs = 2;
        return( collectArgs(mP) );
    }

    backup(tP);
    if( padP )
    {
        pushContext1(padP);
    }

    return( NULL );
}

// gcc's replace_args: a function-like macro's body with its arguments put in,
// expanded unless an operand of ##, with padding around each.
static void
replaceArgs(PPMacro *mP, PPArg *argsP, PPVec *outP)
{
PPToken *srcP, *tP, **fromPP;
PPArg *argP;
int i, k, count, pasteIdx, want;

    for( i = 0; i < mP->count; ++i )
    {
        srcP = mP->bodyPP[i];
        if( srcP->type != PP_MACRO_ARG )
        {
            vecPush(outP, srcP);
            continue;
        }

        argP = &argsP[srcP->argNo];
        pasteIdx = -1;

        if( (srcP->flags & TF_PASTE_LEFT) || ((i > 0) && (mP->bodyPP[i - 1]->flags & TF_PASTE_LEFT)) )
        {
            fromPP = argP->firstPP;
            count = argP->count;

            // an empty operand of ##: the token before it takes this one's ##
            if( (count == 0) && (outP->n > 0) )
            {
                pasteIdx = outP->n - 1;
            }
        }
        else
        {
            expandArg(argP);
            fromPP = argP->expandedPP;
            count = argP->expCount;
        }

        if( !inDirective && (i > 0) && !(mP->bodyPP[i - 1]->flags & TF_PASTE_LEFT) )
        {
            vecPush(outP, padding(srcP));
        }

        if( count )
        {
            for( k = 0; k < count; ++k )
            {
                vecPush(outP, fromPP[k]);
            }

            if( srcP->flags & TF_PASTE_LEFT )
            {
                pasteIdx = outP->n - 1;
            }
        }

        if( !inDirective && !(srcP->flags & TF_PASTE_LEFT) )
        {
            vecPush(outP, &avoidPasteTok);
        }

        if( pasteIdx >= 0 )
        {
            tP = outP->vPP[pasteIdx];
            want = srcP->flags & TF_PASTE_LEFT;
            if( (tP->flags & TF_PASTE_LEFT) != want )
            {
                outP->vPP[pasteIdx] = copyTok(tP, ((tP->flags & ~TF_PASTE_LEFT) | want));
            }
        }
    }
}

// gcc's enter_macro_context: start expanding mP, whose name is nameP.
// 1 if a context was pushed, 0 if a function-like macro had no arguments after it.
static int
enterMacro(PPMacro *mP, PPToken *nameP)
{
PPArg *argsP;
PPVec v;
int savedPrevent;

    if( !ctxP )
    {
        useLine = nameP->line;
        useCol = nameP->col;
    }

    argsP = NULL;
    if( mP->funLike )
    {
        savedPrevent = preventExpansion;
        ++preventExpansion;
        parsingArgs = 1;
        argsP = funlikeInvocation(mP);
        parsingArgs = 0;
        preventExpansion = savedPrevent;

        if( !argsP )
        {
            return( 0 );
        }
    }

    memset(&v, 0, sizeof(v));
    if( mP->funLike && (mP->paramc > 0) )
    {
        replaceArgs(mP, argsP, &v);
    }
    else
    {
        v.vPP = mP->bodyPP;
        v.n = mP->count;
    }

    // A body of #error or #warning reports where the macro is used, its
    // arguments put in, and expands to nothing.
    if( mP->diag >= 0 )
    {
        diagTok(mP->diag, nameP, "%s", diagText(v.vPP + 1, v.n - 1));
        v.n = 0;
    }

    mP->disabled = true;
    pushContext(v.vPP, v.n, mP);
    ctxP->chainP = ppAlloc(sizeof(PPChain));
    ctxP->chainP->macroP = mP;
    ctxP->chainP->useLine = (nameP->originLine)?nameP->originLine:nameP->line;
    ctxP->chainP->outerP = nameP->chainP;
    return( 1 );
}

// Refuse a name am1's preprocessor does not support, where it would expand.
static void
refuseName(PPToken *tP)
{
    if( !strcmp(tP->textP, "__FILE__") || !strcmp(tP->textP, "__LINE__") || !strcmp(tP->textP, "_Pragma") )
    {
        diagTok(PPD_ERROR, tP, "%s is not supported by am1's preprocessor", tP->textP);
    }
}

// gcc's cpp_get_token: the next token, macros expanded.
// Padding comes back too, outside a directive; it says how the expansion is spaced.
static PPToken *
getToken(void)
{
PPToken *tP;
PPMacro *mP;

    for(;;)
    {
        if( !ctxP )
        {
            tP = lexToken();
        }
        else if( ctxP->pos < ctxP->count )
        {
            tP = ctxP->toksPP[ctxP->pos++];

            // a body token leaves as a copy that knows its expansion; an
            // argument's tokens already carry their own
            if( (tP->flags & TF_BODY) && ctxP->chainP )
            {
                tP = copyTok(tP, (tP->flags & ~TF_BODY));
                tP->chainP = ctxP->chainP;
            }

            if( tP->flags & TF_PASTE_LEFT )
            {
                pasteAll(tP);
                if( inDirective )
                {
                    continue;
                }
                return( padding(tP) );
            }
        }
        else
        {
            popContext();
            if( inDirective )
            {
                continue;
            }
            return( &avoidPasteTok );
        }

        if( (tP->type != PP_NAME) || (tP->flags & TF_NO_EXPAND) )
        {
            return( tP );
        }

        if( !(mP = findMacro(tP->textP)) )
        {
            if( !preventExpansion )
            {
                refuseName(tP);
            }
            return( tP );
        }

        if( mP->disabled )
        {
            return( copyTok(tP, (tP->flags | TF_NO_EXPAND)) );
        }

        if( preventExpansion )
        {
            return( tP );
        }

        if( enterMacro(mP, tP) )
        {
            if( inDirective )
            {
                continue;
            }
            return( padding(tP) );
        }

        return( tP );
    }
}

// Report the open conditionals of the buffer being left.
static void
endConditionals(PPBuffer *bP)
{
PPCond *cP;
static const char *namesPP[] = {"", "", "", "", "", "if", "ifdef", "ifndef", "elif", "else"};

    for( cP = bP->condsP; cP; cP = cP->prevP )
    {
        diagHere(PPD_ERROR, cP->line, cP->col, "unterminated #%s", namesPP[cP->kind]);
    }

    bP->condsP = NULL;
    skipping = false;
}

// Leave the buffer whose end was read.
// False at the end of the main file.
static bool
popBuffer(void)
{
PPBuffer *bP;

    bP = bufP;
    endConditionals(bP);
    if( !bP->prevP )
    {
        return( false );
    }

    // a file that was all one guarded conditional records its guard
    if( miValid && bP->fileP && !bP->fileP->guardP )
    {
        bP->fileP->guardP = miGuardP;
    }
    miValid = false;

    bP->incP->reading = false;
    bufP = bP->prevP;
    --depth;
    printLine(bufP->nextLine, bufP->nameP, " 2", bufP->sysp);
    return( true );
}

// gcc's _cpp_lex_token: the next token from the files, directives obeyed,
// lines skipped where a conditional says so, and the printer told of each
// new line.
static PPToken *
lexToken(void)
{
PPToken *tP;

    for(;;)
    {
        if( lookaheads )
        {
            tP = lookaheadPP[--lookaheads];
        }
        else
        {
            tP = lexDirect();
        }

        if( (tP->flags & TF_BOL) && !inDirective )
        {
            if( (tP->type == PP_HASH) && (parsingArgs != 1) )
            {
                if( handleDirective(tP) )
                {
                    continue;
                }
                // an assembler '#' line: the '#' is text
            }

            if( !skipping )
            {
                lineChange(tP);
            }
        }

        if( inDirective )
        {
            return( tP );
        }

        if( (tP->type == PP_EOF) && (tP->flags & TF_FILE_END) )
        {
            // seen while looking for a '(', it is read again afterwards
            if( parsingArgs )
            {
                return( tP );
            }

            if( popBuffer() )
            {
                continue;
            }

            return( tP );
        }

        // any token outside a directive, read or skipped, ends a possible guard
        miValid = false;

        if( !skipping )
        {
            return( tP );
        }
    }
}

// Read and drop the rest of a directive's line.
static void
skipRestOfLine(void)
{
    while( lexToken()->type != PP_EOF )
    {
    }
}

// gcc's check_eol: warn of tokens after a directive's own.
static void
checkEol(const char *dirNameP)
{
PPToken *tP;

    tP = lexToken();
    if( tP->type != PP_EOF )
    {
        diagHere(PPD_WARNING, tP->line, tP->col, "extra tokens at end of #%s directive", dirNameP);
    }
}

// A directive's macro name: NULL after an error.
static PPToken *
macroName(const char *dirNameP)
{
PPToken *tP;

    tP = lexToken();
    if( tP->type == PP_NAME )
    {
        if( !strcmp(tP->textP, "defined") )
        {
            diagHere(PPD_ERROR, tP->line, tP->col, "\"defined\" cannot be used as a macro name");
            return( NULL );
        }
        return( tP );
    }

    if( tP->type == PP_EOF )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, "no macro name given in #%s directive", dirNameP);
    }
    else
    {
        diagHere(PPD_ERROR, tP->line, tP->col, "macro names must be identifiers");
    }

    return( NULL );
}

// True when two definitions are the same, as gcc's warn_of_redefinition
// compares them.
static bool
sameMacro(PPMacro *aP, PPMacro *bP)
{
int i;

    if( (aP->funLike != bP->funLike) || (aP->paramc != bP->paramc) || (aP->count != bP->count) ||
        (aP->diag != bP->diag) )
    {
        return( false );
    }

    for( i = 0; i < aP->paramc; ++i )
    {
        if( strcmp(aP->paramsPP[i], bP->paramsPP[i]) )
        {
            return( false );
        }
    }

    for( i = 0; i < aP->count; ++i )
    {
        if( (aP->bodyPP[i]->type != bP->bodyPP[i]->type) ||
            ((aP->bodyPP[i]->flags & (TF_PREV_WHITE | TF_PASTE_LEFT)) != (bP->bodyPP[i]->flags & (TF_PREV_WHITE | TF_PASTE_LEFT))) ||
            (aP->bodyPP[i]->argNo != bP->bodyPP[i]->argNo) ||
            (aP->bodyPP[i]->len != bP->bodyPP[i]->len) ||
            memcmp(aP->bodyPP[i]->textP, bP->bodyPP[i]->textP, aP->bodyPP[i]->len) )
        {
            return( false );
        }
    }

    return( true );
}

// Make mP the current state of its name, after mP->prevP.
static void
installMacro(PPMacro *mP)
{
PPName *sP;

    sP = nameSlot(mP->nameP);
    mP->prevP = sP->curP;
    sP->curP = mP;
}

// A block macro's body being read: the lines after its #define, to #enddef.
typedef struct PPBlock
{
    bool lineStart;             // the next token read is the first of its line
    bool bad;                   // an error was reported: read to #enddef, define nothing
    PPToken *pendingP;          // the token a ';' was given in front of
} PPBlock;

// The next token of a block macro's body: an EOF at its #enddef, NULL when
// the file ends first.
// Between two lines a ';' is given, unless the earlier
// one (lastP its last token) ends in one, so the body is the token list the
// macro written on one line with '\' and ';' has.
// Blank lines and comments give nothing.
// A line starting with '#' holds a directive, which a block
// cannot: an error, and the block is bad.
static PPToken *
blockToken(PPBlock *blkP, PPToken *lastP)
{
PPToken *tP, *nameP;

    if( blkP->pendingP )
    {
        tP = blkP->pendingP;
        blkP->pendingP = NULL;
        return( tP );
    }

    for(;;)
    {
        if( bufP->needLine )
        {
            if( !fetchLine(bufP) )
            {
                diagHere(PPD_ERROR, dirLine, dirCol, "#define block not closed by #enddef");
                return( NULL );
            }
            bufP->needLine = false;
            blkP->lineStart = true;
        }

        // in a directive the end of the line is an EOF, and needLine is set
        tP = lexToken();
        if( tP->type == PP_EOF )
        {
            continue;
        }

        if( !blkP->lineStart )
        {
            return( tP );
        }

        blkP->lineStart = false;
        if( tP->type == PP_HASH )
        {
            nameP = lexToken();
            if( (nameP->type == PP_NAME) && !strcmp(nameP->textP, "enddef") )
            {
                checkEol("enddef");
                return( makeEof(0) );
            }

            diagHere(PPD_ERROR, tP->line, tP->col, "#%s inside a #define block", nameP->textP);
            blkP->bad = true;
            if( nameP->type != PP_EOF )
            {
                skipRestOfLine();
            }
            continue;
        }

        // a new line reads as a blank after the ';'
        tP->flags |= TF_PREV_WHITE;
        if( lastP && (lastP->type != PP_SEMICOLON) )
        {
            blkP->pendingP = tP;
            return( newTok(PP_SEMICOLON, ";", 1, tP->line, tP->col, 0) );
        }
        return( tP );
    }
}

// #define.
// A function-like macro's '(' follows its name with no blank; with
// nothing after its ')', the macro is a block, its body the lines up to
// #enddef.
static void
doDefine(PPBuffer *defBufP)
{
PPToken *nameP, *tP, *argP;
PPMacro *mP, *oldP;
PPVec body;
PPBlock blk;
int i;
bool paste, block, failed;

    if( !(nameP = macroName("define")) )
    {
        return;
    }

    mP = ppAlloc(sizeof(PPMacro));
    mP->nameP = nameP->textP;
    mP->defined = true;
    mP->diag = -1;
    mP->fileP = defBufP->nameP;
    mP->line = nameP->line;
    mP->col = nameP->col;
    mP->sysp = defBufP->sysp;
    mP->ownerP = defBufP->incP;

    tP = lexToken();
    if( (tP->type == PP_OPEN_PAREN) && !(tP->flags & TF_PREV_WHITE) )
    {
        mP->funLike = true;
        mP->paramsPP = ppAlloc(sizeof(char *));

        for(;;)
        {
            tP = lexToken();
            if( (tP->type == PP_CLOSE_PAREN) && (mP->paramc == 0) )
            {
                break;
            }

            if( tP->type == PP_ELLIPSIS )
            {
                diagHere(PPD_ERROR, tP->line, tP->col, "variadic macros are not supported by am1's preprocessor");
                return;
            }

            if( tP->type != PP_NAME )
            {
                diagHere(PPD_ERROR, tP->line, tP->col, "expected parameter name, found \"%s\"", tP->textP);
                return;
            }

            for( i = 0; i < mP->paramc; ++i )
            {
                if( !strcmp(mP->paramsPP[i], tP->textP) )
                {
                    diagHere(PPD_ERROR, tP->line, tP->col, "duplicate macro parameter \"%s\"", tP->textP);
                    return;
                }
            }

            mP->paramsPP = realloc(mP->paramsPP, ((mP->paramc + 1) * sizeof(char *)));
            mP->paramsPP[mP->paramc++] = tP->textP;

            tP = lexToken();
            if( tP->type == PP_CLOSE_PAREN )
            {
                break;
            }

            if( tP->type == PP_ELLIPSIS )
            {
                diagHere(PPD_ERROR, tP->line, tP->col, "variadic macros are not supported by am1's preprocessor");
                return;
            }

            if( tP->type != PP_COMMA )
            {
                diagHere(PPD_ERROR, tP->line, tP->col, "expected ',' or ')', found \"%s\"", tP->textP);
                return;
            }
        }

        tP = lexToken();
    }

    memset(&blk, 0, sizeof(blk));
    block = (mP->funLike && (tP->type == PP_EOF));
    if( block && !(tP = blockToken(&blk, NULL)) )
    {
        return;
    }

    // the body; ## joins its neighbors, and is not kept itself
    memset(&body, 0, sizeof(body));
    paste = false;
    failed = false;
    for( ; tP && (tP->type != PP_EOF);
         tP = (block)?blockToken(&blk, (body.n > 0)?body.vPP[body.n - 1]:NULL):lexToken() )
    {
        if( !strcmp(tP->textP, "__VA_ARGS__") && (tP->type == PP_NAME) )
        {
            diagHere(PPD_ERROR, tP->line, tP->col, "variadic macros are not supported by am1's preprocessor");
            failed = true;
            break;
        }

        if( mP->funLike && (tP->type == PP_NAME) )
        {
            for( i = 0; i < mP->paramc; ++i )
            {
                if( !strcmp(mP->paramsPP[i], tP->textP) )
                {
                    argP = copyTok(tP, tP->flags);
                    argP->type = PP_MACRO_ARG;
                    argP->argNo = i;
                    tP = argP;
                    break;
                }
            }
        }

        if( mP->funLike && (tP->type == PP_MACRO_ARG) && (body.n > 0) && (body.vPP[body.n - 1]->type == PP_HASH) )
        {
            diagHere(PPD_ERROR, body.vPP[body.n - 1]->line, body.vPP[body.n - 1]->col,
                "'#' (stringizing) is not supported by am1's preprocessor");
            failed = true;
            break;
        }

        if( tP->type == PP_PASTE )
        {
            if( body.n == 0 )
            {
                diagHere(PPD_ERROR, tP->line, tP->col, "'##' cannot appear at either end of a macro expansion");
                failed = true;
                break;
            }
            body.vPP[body.n - 1]->flags |= TF_PASTE_LEFT;
            paste = true;
            continue;
        }

        paste = false;
        tP->flags |= TF_BODY;
        vecPush(&body, tP);
    }

    // a block in error is read to its #enddef, so its lines are not taken as code
    if( failed && block )
    {
        while( tP && (tP->type != PP_EOF) )
        {
            tP = blockToken(&blk, NULL);
        }
    }

    if( failed || !tP || blk.bad )
    {
        return;
    }

    if( paste )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, "'##' cannot appear at either end of a macro expansion");
        return;
    }

    if( body.n > 0 )
    {
        body.vPP[0]->flags &= ~TF_PREV_WHITE;
    }

    mP->bodyPP = body.vPP;
    mP->count = body.n;

    // A body of #error or #warning reports when the macro is used
    if( (body.n >= 2) && (body.vPP[0]->type == PP_HASH) && (body.vPP[1]->type == PP_NAME) &&
        (!strcmp(body.vPP[1]->textP, "error") || !strcmp(body.vPP[1]->textP, "warning")) )
    {
        mP->diag = (body.vPP[1]->textP[0] == 'e')?PPD_ERROR:PPD_DIRWARNING;
    }

    oldP = findMacro(mP->nameP);
    if( oldP && !sameMacro(oldP, mP) && !mP->sysp )
    {
        diagHere(PPD_WARNING, nameP->line, nameP->col, "'%s' redefined", mP->nameP);
        if( !bufP->sysp )
        {
            fprintf(stderr, "%s:%d:%d: note: this is the location of the previous definition\n",
                oldP->fileP, oldP->line, oldP->col);
        }
    }

    installMacro(mP);
}

// #undef: the name's new state is undefined, remembered for #uninclude.
static void
doUndef(void)
{
PPToken *nameP;
PPMacro *mP;

    if( !(nameP = macroName("undef")) )
    {
        return;
    }

    checkEol("undef");
    if( findMacro(nameP->textP) )
    {
        mP = ppAlloc(sizeof(PPMacro));
        mP->nameP = nameP->textP;
        mP->defined = false;
        mP->diag = -1;
        mP->ownerP = bufP->incP;
        installMacro(mP);
    }
}

// A system header's path as gcc names it (its maybe_shorter_path): the real
// path, when that is shorter, so "sub/../x.ah" is named "x.ah".
static char *
systemPath(char *pathP)
{
char *realP;

    if( (realP = realpath(pathP, NULL)) && (strlen(realP) < strlen(pathP)) )
    {
        return( realP );
    }

    free(realP);
    return( pathP );
}

// The path of an include file and whether it is a system header, searched
// for as gcc does: a "..." name first in the including file's directory, then
// the -I directories, then the include root.
// NULL if not found.
// *whereP is -2 for an absolute name, -1 for the including file's directory, else the
// search path entry it was found in.
static char *
findInclude(const char *nameP, bool angled, int *syspP, int *whereP)
{
struct stat st;
char *pathP;
size_t dlen;
int i;

    if( nameP[0] == '/' )
    {
        *syspP = 0;
        *whereP = -2;
        return( (!stat(nameP, &st) && S_ISREG(st.st_mode))?ppStrndup(nameP, strlen(nameP)):NULL );
    }

    if( !angled )
    {
        pathP = ppAlloc(strlen(bufP->dirP) + strlen(nameP) + 1);
        sprintf(pathP, "%s%s", bufP->dirP, nameP);
        if( !stat(pathP, &st) && S_ISREG(st.st_mode) )
        {
            *syspP = bufP->sysp;
            *whereP = -1;
            return( (*syspP)?systemPath(pathP):pathP );
        }
    }

    for( i = 0; i < nSearch; ++i )
    {
        dlen = strlen(searchP[i].nameP);
        pathP = ppAlloc(dlen + strlen(nameP) + 2);
        if( (dlen > 0) && (searchP[i].nameP[dlen - 1] == '/') )
        {
            sprintf(pathP, "%s%s", searchP[i].nameP, nameP);
        }
        else
        {
            sprintf(pathP, "%s/%s", searchP[i].nameP, nameP);
        }

        if( !stat(pathP, &st) && S_ISREG(st.st_mode) )
        {
            *syspP = searchP[i].sysp;
            *whereP = i;
            return( (*syspP)?systemPath(pathP):pathP );
        }
    }

    return( NULL );
}

static PPFile *
lookupFile(const char *nameP, const char *startP)
{
PPFileKey *kP;

    for( kP = fileKeysP; kP; kP = kP->nextP )
    {
        if( !strcmp(kP->nameP, nameP) && !strcmp(kP->startP, startP) )
        {
            return( kP->fileP );
        }
    }

    return( NULL );
}

static void
addFileKey(const char *nameP, const char *startP, PPFile *fileP)
{
PPFileKey *kP;

    kP = ppAlloc(sizeof(PPFileKey));
    kP->nameP = ppStrndup(nameP, strlen(nameP));
    kP->startP = ppStrndup(startP, strlen(startP));
    kP->fileP = fileP;
    kP->nextP = fileKeysP;
    fileKeysP = kP;
}

// The record gcc's _cpp_find_file gives an #include of nameP, found where
// findInclude says.
static PPFile *
includedFile(const char *nameP, bool angled, int where)
{
PPFile *fP;
char *startP;

    if( where == -2 )
    {
        startP = ppStrndup("/", 1);
    }
    else if( angled )
    {
        startP = ppStrndup("<", 1);
    }
    else
    {
        startP = ppAlloc(strlen(bufP->dirP) + 2);
        sprintf(startP, "q%s", bufP->dirP);
    }

    if( !(fP = lookupFile(nameP, startP)) )
    {
        // a "..." search that went on to the -I chain shares what a <...> one has
        if( (where >= 0) && (startP[0] == 'q') )
        {
            if( !(fP = lookupFile(nameP, "<")) )
            {
                fP = ppAlloc(sizeof(PPFile));
                addFileKey(nameP, "<", fP);
            }
        }
        else
        {
            fP = ppAlloc(sizeof(PPFile));
        }
        addFileKey(nameP, startP, fP);
    }

    free(startP);
    return( fP );
}

// True when inner is outer or was included, however deeply, while outer was read.
static bool
inSubtree(PPInclusion *innerP, PPInclusion *outerP)
{
    for( ; innerP; innerP = innerP->parentP )
    {
        if( innerP == outerP )
        {
            return( true );
        }
    }

    return( false );
}

// Undo an inclusion: every macro state it or its own includes made goes back
// to the state before it, unless a later state outside it has replaced that
// one since, and each file it brought in counts as not included.
static void
uninclude(PPInclusion *rootP)
{
PPInclusion *iP;
PPName *sP;
PPMacro *mP;
int h;

    for( h = 0; h < PP_HASHSIZE; ++h )
    {
        for( sP = namesPP[h]; sP; sP = sP->nextP )
        {
            for( mP = sP->curP; mP && inSubtree(mP->ownerP, rootP); mP = mP->prevP )
            {
            }
            sP->curP = mP;
        }
    }

    for( iP = inclusionsP; iP; iP = iP->nextP )
    {
        if( inSubtree(iP, rootP) )
        {
            iP->active = false;
        }
    }
}

// #include, #includeonce and #uninclude.
static void
doInclude(int kind, const char *dirNameP, PPBuffer **pushPP)
{
PPToken *tP;
PPInclusion *iP, *newP;
PPBuffer *nbP;
PPFile *fileP;
struct stat st;
char *nameP, *pathP, *dataP;
size_t len;
int sysp, where;
bool angled, any;

    angledHeaders = true;
    tP = lexToken();
    angledHeaders = false;

    if( (tP->type == PP_STRING) || (tP->type == PP_HEADER) )
    {
        angled = (tP->type == PP_HEADER);
        nameP = ppStrndup(tP->textP + 1, tP->len - 2);
    }
    else
    {
        diagHere(PPD_ERROR, (tP->type == PP_EOF)?dirLine:tP->line, (tP->type == PP_EOF)?dirCol:tP->col,
            "#%s expects \"FILENAME\" or <FILENAME>", dirNameP);
        return;
    }

    if( !nameP[0] )
    {
        diagHere(PPD_ERROR, tP->line, tP->col, "empty filename in #%s", dirNameP);
        return;
    }

    checkEol(dirNameP);

    if( !(pathP = findInclude(nameP, angled, &sysp, &where)) )
    {
        diagHere(PPD_FATAL, tP->line, tP->col, "%s: No such file or directory", nameP);
        longjmp(fatalJmp, 1);
    }

    // a file is known by its device and inode, however its path is spelled
    if( stat(pathP, &st) )
    {
        diagHere(PPD_FATAL, tP->line, tP->col, "%s: %s", pathP, strerror(errno));
        longjmp(fatalJmp, 1);
    }

    if( kind == D_UNINCLUDE )
    {
        any = false;
        for( iP = inclusionsP; iP; iP = iP->nextP )
        {
            if( iP->active && (iP->dev == st.st_dev) && (iP->ino == st.st_ino) )
            {
                if( iP->reading )
                {
                    diagHere(PPD_ERROR, dirLine, dirCol, "#uninclude of '%s', which is still being read", nameP);
                    return;
                }
                uninclude(iP);
                any = true;
            }
        }

        if( !any )
        {
            diagHere(PPD_WARNING, dirLine, dirCol, "#uninclude of '%s', which is not included", nameP);
        }
        return;
    }

    if( kind == D_INCLUDEONCE )
    {
        for( iP = inclusionsP; iP; iP = iP->nextP )
        {
            if( iP->active && (iP->dev == st.st_dev) && (iP->ino == st.st_ino) )
            {
                return;
            }
        }
    }

    if( depth >= PP_MAXDEPTH )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, "#include nested depth %d exceeds maximum of %d", depth, PP_MAXDEPTH);
        return;
    }

    // gcc's multiple-include optimization: a file whose guard is defined is not read again
    fileP = includedFile(nameP, angled, where);
    if( fileP->guardP && findMacro(fileP->guardP) )
    {
        return;
    }

    if( !(dataP = readFile(pathP, &len)) )
    {
        diagHere(PPD_FATAL, tP->line, tP->col, "%s: %s", pathP, strerror(errno));
        longjmp(fatalJmp, 1);
    }

    newP = ppAlloc(sizeof(PPInclusion));
    newP->pathP = pathP;
    newP->dev = st.st_dev;
    newP->ino = st.st_ino;
    newP->parentP = bufP->incP;
    newP->active = true;
    newP->reading = true;
    newP->nextP = inclusionsP;
    inclusionsP = newP;

    nbP = newBuffer(dataP, len, pathP, sysp);
    nbP->incP = newP;
    nbP->fileP = fileP;
    *pushPP = nbP;
}

// #error and #warning on a line of their own like gcc's message, the directive's
// tokens one blank apart where one or more blanks were.
static void
doDiagnostic(int kind, PPToken *dnameP)
{
PPToken *tP;
PPVec v;

    memset(&v, 0, sizeof(v));
    vecPush(&v, dnameP);
    while( (tP = lexToken())->type != PP_EOF )
    {
        vecPush(&v, tP);
    }

    diagHere(kind, dnameP->line, dnameP->col, "%s", diagText(v.vPP, v.n));
}

static PPToken **exprPP;
static int exprN;
static int exprPos;
static bool exprBad;
static int exprSkip;            // inside a branch && || ?: does not evaluate

static PPVal evalCond(void);

static PPToken *
exprPeek(void)
{
    return( (exprPos < exprN)?exprPP[exprPos]:&endArgTok );
}

static void
exprError(const char *fmtP, const char *argP)
{
    if( !exprBad )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, fmtP, argP);
    }
    exprBad = true;
}

// A C integer constant's value: decimal, octal or hex, with u and l suffixes.
static PPVal
numberValue(PPToken *tP)
{
PPVal r;
const char *sP;
char *endP, *suffixP;
uintmax_t v;
int base, us, ls;

    r.v = 0;
    r.u = false;
    sP = tP->textP;
    base = 10;
    if( (sP[0] == '0') && ((sP[1] == 'x') || (sP[1] == 'X')) )
    {
        base = 16;
    }
    else if( sP[0] == '0' )
    {
        base = 8;
    }

    errno = 0;
    v = strtoumax(sP, &endP, base);
    if( (base == 16) && (endP == (sP + 2)) )
    {
        exprError("invalid suffix \"%s\" on integer constant", sP + 1);
        return( r );
    }

    if( strchr(sP, '.') || ((base != 16) && (strchr(sP, 'e') || strchr(sP, 'E'))) )
    {
        exprError("floating constant in preprocessor expression%s", "");
        return( r );
    }

    suffixP = endP;
    for( us = ls = 0; *endP; ++endP )
    {
        if( (*endP == 'u') || (*endP == 'U') )
        {
            ++us;
        }
        else if( (*endP == 'l') || (*endP == 'L') )
        {
            ++ls;
        }
        else
        {
            break;
        }
    }

    if( *endP || (us > 1) || (ls > 2) )
    {
        exprError("invalid suffix \"%s\" on integer constant", suffixP);
        return( r );
    }

    r.v = (intmax_t)v;
    r.u = (us > 0) || (v > (uintmax_t)INTMAX_MAX);
    return( r );
}

// A character constant's value, escapes read as C reads them.
static PPVal
charValue(PPToken *tP)
{
PPVal r;
const char *sP;
intmax_t v;
int c, n;

    sP = tP->textP + ((tP->type == PP_WCHAR)?2:1);
    v = 0;
    while( *sP && (*sP != '\'') )
    {
        c = (unsigned char)*sP++;
        if( c == '\\' )
        {
            c = (unsigned char)*sP++;
            switch( c )
            {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'v': c = '\v'; break;
            case 'e': c = 033; break;
            case 'x':
                for( c = 0; hexValue((unsigned char)*sP) >= 0; ++sP )
                {
                    c = (c * 16) + hexValue((unsigned char)*sP);
                }
                break;
            default:
                if( (c >= '0') && (c <= '7') )
                {
                    for( c -= '0', n = 1; (n < 3) && (*sP >= '0') && (*sP <= '7'); ++n )
                    {
                        c = (c * 8) + (*sP++ - '0');
                    }
                }
                break;
            }
        }
        v = (v << 8) | (c & 0377);
    }

    r.v = (tP->type == PP_WCHAR)?v:(intmax_t)(signed char)v;
    r.u = false;
    return( r );
}

static PPVal
evalPrimary(void)
{
PPToken *tP;
PPVal r;

    r.v = 0;
    r.u = false;
    tP = exprPeek();

    switch( tP->type )
    {
    case PP_NUMBER:
        ++exprPos;
        return( numberValue(tP) );

    case PP_CHAR:
    case PP_WCHAR:
        ++exprPos;
        return( charValue(tP) );

    case PP_NAME:
        ++exprPos;
        return( r );        // a name left after expansion is 0

    case PP_OPEN_PAREN:
        ++exprPos;
        r = evalCond();
        if( exprPeek()->type != PP_CLOSE_PAREN )
        {
            exprError("missing ')' in expression%s", "");
        }
        else
        {
            ++exprPos;
        }
        return( r );

    case PP_PLUS:
        ++exprPos;
        return( evalPrimary() );

    case PP_MINUS:
        ++exprPos;
        r = evalPrimary();
        r.v = -r.v;
        return( r );

    case PP_COMPL:
        ++exprPos;
        r = evalPrimary();
        r.v = ~r.v;
        return( r );

    case PP_NOT:
        ++exprPos;
        r = evalPrimary();
        r.v = !r.v;
        r.u = false;
        return( r );

    case PP_EOF:
        exprError("#if with no expression%s", "");
        return( r );

    default:
        exprError("token \"%s\" is not valid in preprocessor expressions", tP->textP);
        ++exprPos;
        return( r );
    }
}

// The binding strength of a binary operator, 0 for none.
static int
binaryPrec(int type)
{
    switch( type )
    {
    case PP_MULT: case PP_DIV: case PP_MOD: return( 10 );
    case PP_PLUS: case PP_MINUS: return( 9 );
    case PP_LSHIFT: case PP_RSHIFT: return( 8 );
    case PP_LESS: case PP_GREATER: case PP_LESS_EQ: case PP_GREATER_EQ: return( 7 );
    case PP_EQ_EQ: case PP_NOT_EQ: return( 6 );
    case PP_AND: return( 5 );
    case PP_XOR: return( 4 );
    case PP_OR: return( 3 );
    case PP_AND_AND: return( 2 );
    case PP_OR_OR: return( 1 );
    default: return( 0 );
    }
}

// Apply a binary operator with C's usual conversions.
static PPVal
applyBinary(int op, PPVal a, PPVal b)
{
PPVal r;
bool u;

    u = a.u || b.u;
    r.u = u;
    r.v = 0;

    switch( op )
    {
    case PP_MULT:
        r.v = (intmax_t)((uintmax_t)a.v * (uintmax_t)b.v);
        break;
    case PP_DIV:
    case PP_MOD:
        if( b.v == 0 )
        {
            if( !exprSkip )
            {
                exprError("division by zero in #if%s", "");
            }
            break;
        }
        if( u )
        {
            r.v = (intmax_t)((op == PP_DIV)?((uintmax_t)a.v / (uintmax_t)b.v):((uintmax_t)a.v % (uintmax_t)b.v));
        }
        else
        {
            r.v = (op == PP_DIV)?(a.v / b.v):(a.v % b.v);
        }
        break;
    case PP_PLUS:
        r.v = (intmax_t)((uintmax_t)a.v + (uintmax_t)b.v);
        break;
    case PP_MINUS:
        r.v = (intmax_t)((uintmax_t)a.v - (uintmax_t)b.v);
        break;
    case PP_LSHIFT:
        r.u = a.u;
        r.v = (b.v >= 64)?0:(intmax_t)((uintmax_t)a.v << b.v);
        break;
    case PP_RSHIFT:
        r.u = a.u;
        r.v = (b.v >= 64)?0:((a.u)?(intmax_t)((uintmax_t)a.v >> b.v):(a.v >> b.v));
        break;
    case PP_LESS:
        r.v = (u)?((uintmax_t)a.v < (uintmax_t)b.v):(a.v < b.v);
        r.u = false;
        break;
    case PP_GREATER:
        r.v = (u)?((uintmax_t)a.v > (uintmax_t)b.v):(a.v > b.v);
        r.u = false;
        break;
    case PP_LESS_EQ:
        r.v = (u)?((uintmax_t)a.v <= (uintmax_t)b.v):(a.v <= b.v);
        r.u = false;
        break;
    case PP_GREATER_EQ:
        r.v = (u)?((uintmax_t)a.v >= (uintmax_t)b.v):(a.v >= b.v);
        r.u = false;
        break;
    case PP_EQ_EQ:
        r.v = (a.v == b.v);
        r.u = false;
        break;
    case PP_NOT_EQ:
        r.v = (a.v != b.v);
        r.u = false;
        break;
    case PP_AND:
        r.v = a.v & b.v;
        break;
    case PP_XOR:
        r.v = a.v ^ b.v;
        break;
    case PP_OR:
        r.v = a.v | b.v;
        break;
    case PP_AND_AND:
        r.v = (a.v && b.v);
        r.u = false;
        break;
    case PP_OR_OR:
        r.v = (a.v || b.v);
        r.u = false;
        break;
    default:
        break;
    }

    return( r );
}

// Precedence climbing over the binary operators; && and || evaluate their
// right side only where C would.
static PPVal
evalBinary(int minPrec)
{
PPVal a, b;
int op, prec, skip;

    a = evalPrimary();
    for(;;)
    {
        op = exprPeek()->type;
        prec = binaryPrec(op);
        if( (prec == 0) || (prec < minPrec) )
        {
            return( a );
        }

        ++exprPos;
        skip = ((op == PP_AND_AND) && !a.v) || ((op == PP_OR_OR) && a.v);
        exprSkip += skip;
        b = evalBinary(prec + 1);
        exprSkip -= skip;
        a = applyBinary(op, a, b);
    }
}

// ?: and the comma, the loosest operators.
static PPVal
evalCond(void)
{
PPVal c, a, b;

    c = evalBinary(1);
    if( exprPeek()->type == PP_QUERY )
    {
        ++exprPos;
        exprSkip += !c.v;
        a = evalCond();
        exprSkip -= !c.v;
        if( exprPeek()->type != PP_COLON )
        {
            exprError("'?' without following ':'%s", "");
            return( c );
        }
        ++exprPos;
        exprSkip += !!c.v;
        b = evalCond();
        exprSkip -= !!c.v;
        c = (c.v)?a:b;
        c.u = a.u || b.u;
    }

    while( exprPeek()->type == PP_COMMA )
    {
        ++exprPos;
        c = evalCond();
    }

    return( c );
}

// An #if or #elif expression: macros expanded, defined NAME and
// defined(NAME) answered first.
// True when nonzero; an error counts false.
// ifGuardP is NAME when the whole expression is !defined NAME.
static bool
evalIf(void)
{
PPVec v;
PPToken *tP, *nP;
PPVal r;
const char *defNameP;
bool paren, isDef, leadingNot;
int count;

    memset(&v, 0, sizeof(v));
    defNameP = NULL;
    leadingNot = false;
    count = 0;
    for(;;)
    {
        tP = getToken();
        if( tP->type == PP_EOF )
        {
            break;
        }

        if( tP->type != PP_PADDING )
        {
            ++count;
            if( (count == 1) && (tP->type == PP_NOT) )
            {
                leadingNot = true;
            }
        }

        if( (tP->type == PP_NAME) && !strcmp(tP->textP, "defined") )
        {
            ++preventExpansion;
            nP = getToken();
            paren = (nP->type == PP_OPEN_PAREN);
            if( paren )
            {
                nP = getToken();
            }

            isDef = false;
            if( nP->type == PP_NAME )
            {
                defNameP = nP->textP;
                isDef = (findMacro(nP->textP) != NULL);
                if( paren && (getToken()->type != PP_CLOSE_PAREN) )
                {
                    diagHere(PPD_ERROR, dirLine, dirCol, "missing ')' after \"defined\"");
                }
            }
            else
            {
                diagHere(PPD_ERROR, dirLine, dirCol, "operator \"defined\" requires an identifier");
            }
            --preventExpansion;

            tP = newTok(PP_NUMBER, (isDef)?"1":"0", 1, tP->line, tP->col, tP->flags);
        }

        vecPush(&v, tP);
    }

    ifGuardP = (leadingNot && (count == 2))?defNameP:NULL;

    exprPP = v.vPP;
    exprN = v.n;
    exprPos = 0;
    exprBad = false;
    exprSkip = 0;

    r = evalCond();
    if( !exprBad && (exprPos < exprN) )
    {
        exprError("missing binary operator before token \"%s\"", exprPP[exprPos]->textP);
    }

    return( !exprBad && (r.v != 0) );
}

// gcc's push_conditional.
// guardP is the macro an #ifndef or #if !defined
// tests, which guards the file when nothing came before it.
static void
pushCond(int kind, bool skip, const char *guardP)
{
PPCond *cP;

    cP = ppAlloc(sizeof(PPCond));
    cP->line = dirLine;
    cP->col = dirCol;
    cP->kind = kind;
    cP->wasSkipping = skipping;
    cP->skipElses = skipping || !skip;
    cP->guardP = (miValid && !miGuardP)?guardP:NULL;
    cP->prevP = bufP->condsP;
    bufP->condsP = cP;
    skipping = skip;
}

// gcc's do_ifdef and do_ifndef.
static void
doIfdef(int kind)
{
PPToken *nameP;
const char *guardP;
bool skip;

    skip = true;
    guardP = NULL;
    if( !skipping )
    {
        if( (nameP = macroName((kind == D_IFDEF)?"ifdef":"ifndef")) )
        {
            if( kind == D_IFNDEF )
            {
                guardP = nameP->textP;
            }
            skip = (findMacro(nameP->textP) != NULL);
            if( kind == D_IFDEF )
            {
                skip = !skip;
            }
            checkEol((kind == D_IFDEF)?"ifdef":"ifndef");
        }
    }

    pushCond(kind, skip, guardP);
}

// gcc's do_else, do_elif and do_endif.
static void
doElse(int kind)
{
PPCond *cP;
const char *nameP;

    cP = bufP->condsP;
    nameP = (kind == D_ELSE)?"else":((kind == D_ELIF)?"elif":"endif");

    if( !cP )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, "#%s without #if", nameP);
        return;
    }

    if( kind == D_ENDIF )
    {
        if( !cP->wasSkipping )
        {
            checkEol("endif");
        }
        bufP->condsP = cP->prevP;
        skipping = cP->wasSkipping;

        // the end of a possible guard: nothing else may follow it
        if( !cP->prevP && cP->guardP )
        {
            miValid = true;
            miGuardP = cP->guardP;
        }
        return;
    }

    cP->guardP = NULL;

    if( cP->kind == D_ELSE )
    {
        diagHere(PPD_ERROR, dirLine, dirCol, "#%s after #else", nameP);
        fprintf(stderr, "%s:%d:%d: note: the conditional began here\n", bufP->nameP, cP->line, cP->col);
    }

    cP->kind = kind;
    if( kind == D_ELSE )
    {
        skipping = cP->skipElses;
        cP->skipElses = true;
        if( !cP->wasSkipping )
        {
            checkEol("else");
        }
        return;
    }

    // #elif: only the first true group is read
    if( cP->skipElses )
    {
        skipping = true;
    }
    else
    {
        skipping = false;
        skipping = !evalIf();
        cP->skipElses = !skipping;
    }
}

// gcc's _cpp_handle_directive, for a '#' starting a line.
// True when the line
// was a directive and has been consumed; false for an assembler '#' line,
// whose '#' is then text and whose next token has been given back.
static bool
handleDirective(PPToken *hashP)
{
PPToken *dnameP;
PPBuffer *pushP;
int i, kind, savedArgs, savedPrevent;
bool cond, skip;

    savedArgs = parsingArgs;
    savedPrevent = preventExpansion;
    parsingArgs = 0;
    preventExpansion = 0;
    inDirective = true;
    dirLine = hashP->line;
    dirCol = hashP->col;
    pushP = NULL;

    dnameP = lexToken();
    kind = -1;
    cond = false;
    if( dnameP->type == PP_NAME )
    {
        for( i = 0; directives[i].nameP; ++i )
        {
            if( !strcmp(directives[i].nameP, dnameP->textP) )
            {
                kind = directives[i].kind;
                cond = directives[i].cond;
                break;
            }
        }
    }

    // only #if, #ifdef, #ifndef and the null directive leave a possible guard open
    if( (dnameP->type != PP_EOF) && (kind != D_IF) && (kind != D_IFDEF) && (kind != D_IFNDEF) )
    {
        miValid = false;
    }

    if( (kind < 0) && (dnameP->type != PP_EOF) && !skipping )
    {
        // not a directive: in assembler the line is text, '#' and all
        inDirective = false;
        parsingArgs = savedArgs;
        preventExpansion = savedPrevent;
        backup(dnameP);
        return( false );
    }

    if( (kind >= 0) && (!skipping || cond) )
    {
        dirLine = dnameP->line;
        dirCol = dnameP->col;

        switch( kind )
        {
        case D_DEFINE:
            doDefine(bufP);
            break;

        case D_UNDEF:
            doUndef();
            break;

        case D_INCLUDE:
        case D_INCLUDEONCE:
        case D_UNINCLUDE:
            doInclude(kind, dnameP->textP, &pushP);
            break;

        case D_IF:
            if( skipping )
            {
                pushCond(D_IF, true, NULL);
            }
            else
            {
                skip = !evalIf();
                pushCond(D_IF, skip, ifGuardP);
            }
            break;

        case D_IFDEF:
        case D_IFNDEF:
            doIfdef(kind);
            break;

        case D_ELIF:
        case D_ELSE:
        case D_ENDIF:
            doElse(kind);
            break;

        case D_ERROR:
            doDiagnostic(PPD_ERROR, dnameP);
            break;

        case D_WARNING:
            doDiagnostic(PPD_DIRWARNING, dnameP);
            break;

        case D_ENDDEF:
            // a block's own #enddef is read by doDefine, so this one closes nothing
            diagHere(PPD_ERROR, dirLine, dirCol, "#enddef without #define block");
            break;

        default:
            diagHere(PPD_ERROR, dnameP->line, dnameP->col, "#%s is not supported by am1's preprocessor", dnameP->textP);
            break;
        }
    }

    if( dnameP->type != PP_EOF )
    {
        skipRestOfLine();
    }

    inDirective = false;
    parsingArgs = savedArgs;
    preventExpansion = savedPrevent;

    // The included file starts once its directive is done; the printer
    // first brings the output to the #include's line.
    if( pushP )
    {
        maybePrintLine(hashP->line, bufP->nameP, bufP->sysp);
        pushP->prevP = bufP;
        bufP = pushP;
        ++depth;
        printLine(1, bufP->nameP, " 1", bufP->sysp);
        miValid = true;
        miGuardP = NULL;
    }

    return( true );
}

void
ppAddOption(char type, const char *valueP)
{
PPOpt *oP;

    oP = ppAlloc(sizeof(PPOpt));
    oP->type = type;
    oP->valueP = ppStrndup(valueP, strlen(valueP));
    *optTailPP = oP;
    optTailPP = &oP->nextP;
}

// The search path: the -I directories that exist, each once, then the include
// root as a system directory.
// gcc drops an -I naming the root itself.
static void
buildSearchPath(const char *incRootP)
{
PPOpt *oP;
struct stat st, rootSt, *seenP;
int nSeen, i;
bool rootOk, dup;

    rootOk = (incRootP && !stat(incRootP, &rootSt) && S_ISDIR(rootSt.st_mode));
    seenP = ppAlloc(sizeof(struct stat));
    nSeen = 0;

    for( oP = optsP; oP; oP = oP->nextP )
    {
        if( (oP->type != 'I') || stat(oP->valueP, &st) || !S_ISDIR(st.st_mode) )
        {
            continue;
        }

        dup = (rootOk && (st.st_dev == rootSt.st_dev) && (st.st_ino == rootSt.st_ino));
        for( i = 0; !dup && (i < nSeen); ++i )
        {
            dup = ((seenP[i].st_dev == st.st_dev) && (seenP[i].st_ino == st.st_ino));
        }

        if( dup )
        {
            continue;
        }

        seenP = realloc(seenP, ((nSeen + 1) * sizeof(struct stat)));
        seenP[nSeen++] = st;
        searchP = realloc(searchP, ((nSearch + 1) * sizeof(PPDir)));
        searchP[nSearch].nameP = oP->valueP;
        searchP[nSearch].sysp = 0;
        ++nSearch;
    }

    if( rootOk )
    {
        searchP = realloc(searchP, ((nSearch + 1) * sizeof(PPDir)));
        searchP[nSearch].nameP = (char *)incRootP;
        searchP[nSearch].sysp = 1;
        ++nSearch;
    }
}

// Each -D, read as gcc reads one: "name" defines name as 1, "name=value" as
// value.
static void
defineOptions(void)
{
PPOpt *oP;
PPBuffer *bP;
char *textP, *eqP;

    for( oP = optsP; oP; oP = oP->nextP )
    {
        if( oP->type != 'D' )
        {
            continue;
        }

        textP = ppAlloc(strlen(oP->valueP) + 4);
        strcpy(textP, oP->valueP);
        if( (eqP = strchr(textP, '=')) )
        {
            *eqP = ' ';
        }
        else
        {
            strcat(textP, " 1");
        }
        strcat(textP, "\n");

        bP = newBuffer(textP, strlen(textP), "<command-line>", 0);
        bufP = bP;
        fetchLine(bP);
        bP->needLine = false;
        inDirective = true;
        doDefine(bP);
        skipRestOfLine();
        inDirective = false;
    }
}

bool
preprocess(const char *srcNameP, const char *outNameP, const char *incRootP)
{
PPInclusion *rootP;
PPBuffer *mainP;
PPToken *tP;
struct stat st;
char *dataP;
size_t len;

    if( !(dataP = readFile(srcNameP, &len)) )
    {
        fprintf(stderr, "am1: can't open source file '%s'\n", srcNameP);
        return( false );
    }

    if( !(outfP = fopen(outNameP, "w")) )
    {
        fprintf(stderr, "am1: can't open output file '%s'\n", outNameP);
        return( false );
    }

    avoidPasteTok.type = PP_PADDING;
    avoidPasteTok.textP = "";
    endArgTok.type = PP_EOF;
    endArgTok.textP = "";

    buildSearchPath(incRootP);
    defineOptions();

    rootP = ppAlloc(sizeof(PPInclusion));
    rootP->pathP = (char *)srcNameP;
    if( !stat(srcNameP, &st) )
    {
        rootP->dev = st.st_dev;
        rootP->ino = st.st_ino;
    }
    rootP->active = true;
    rootP->reading = true;
    inclusionsP = rootP;

    mainP = newBuffer(dataP, len, srcNameP, 0);
    mainP->incP = rootP;
    bufP = mainP;

    // gcc's opening markers: the source, its built-ins and command line, the source again
    fprintf(outfP, "# 0 \"");
    writeQuoted(srcNameP);
    fprintf(outfP, "\"\n# 0 \"<built-in>\"\n# 0 \"<command-line>\"\n");
    printLine(1, srcNameP, "", 0);

    if( !setjmp(fatalJmp) )
    {
        for(;;)
        {
            tP = getToken();
            if( tP->type == PP_EOF )
            {
                break;
            }
            streamToken(tP);
        }
    }

    recEndStatement();
    if( pr.printed )
    {
        putc('\n', outfP);
    }

    if( fclose(outfP) )
    {
        fprintf(stderr, "am1: can't write '%s'\n", outNameP);
        return( false );
    }

    return( errorCount == 0 );
}
