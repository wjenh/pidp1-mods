%{
/* parser.y - yacc for the PDP-1 new macro assembler */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"


// We maintain a stack of local symtab ptrs for nested local scopes
int localDepth = 0;
int maxLocalDepth = 0;                  // the deepest nesting we've seen
LocalContextP localContextP;            // used while a local scope is enabled
LocalContextP localStack[MAXLOCALS];
bool sawForceLocal;
bool atSol = true;                      // initially true

// Bank contexts are kept as a linked list, not used often enough to need preallocation
int curBank;
BankContextP banksP;
BankContextP curBankP;
static SymNodeP pendingLabelSymP;
static int pendingLabelLocType;
static int pendingLabelPC;
// A label's own line.  The lexer sets labelTokenLine at the label's comma; the
// ADDR rule copies it to pendingLabelLine because it records the line in its
// final action, after a comma inside labelTrailer may have moved it on.
int labelTokenLine;
static int pendingLabelLine;
// A var name's own line, set by the lexer as it scans the name, for the same
// reason.  The varname rules record it; setVarsPC() placing the var later
// leaves it alone.
int nameTokenLine;
int nameTokenSrcLine;

static char scratchStr[128];            // a scratch string

// The optimizer directives.  Each kind of span is flat and independent of the
// others, so one open span per kind is the whole state.  They emit nothing and
// reserve no memory; what they mean is the optimizer's business.
int optRegionTokenLine;                 // set by the lexer: the line the directive
char *optRegionTokenFileP;              // it just scanned is on, and its file
static int optRegionOpenLine;           // the open region's 'optimize' line, 0 when none
static char *optRegionOpenFileP;        // and the file it is in, NILP when none
// nooptimize spans: the hands-off mark wins wherever one overlaps a region.
static int handsOffOpenLine;            // the open span's 'nooptimize' line, 0 when none
static char *handsOffOpenFileP;         // and the file it is in, NILP when none
// speed regions: "spend words here", a different claim from "P5 holds here".
static int speedOpenLine;               // the open speed region's line, 0 when none
static char *speedOpenFileP;            // and the file it is in, NILP when none

extern PNodeListP wildcardsP;           // any wildcarded cross-bank refs
extern SymNodeP permSymP;            // the instructions and other permanent values
extern SymListP constsListP;            // the list of all constant groups

extern bool noWarn;
extern bool doMacro;
extern bool sawBank;
extern bool dropIncludeText;
extern bool doSymtab;
extern int lineno;
extern char *filenameP;
extern char *incroot;
extern PNodeP rootP;

int setConstPC(int pc, SymNodeP symP);
void setConstVal(SymNodeP symP);
void setVarsPC(int bank, PNodeListP listP);
void addExports(PNodeP nodesP);
void importSymbols(char *filenameP);
void resolveWildcards(PNodeListP wildsP, BankContextP banksP);
bool resolveWildcard(PNodeListP itemP, BankContextP bankP);
BankContextP findBank(int bank);
BankContextP addBank(int bank);
BankContextP swapBanks(int newBank);
void initParser(void);
SymNodeP addLocalSymbol(char *nameP);
PNodeP openOptRegion(void);
PNodeP closeOptRegion(void);
void checkOptRegionClosed(void);
PNodeP openHandsOff(void);
PNodeP closeHandsOff(void);
PNodeP openSpeedRegion(void);
PNodeP closeSpeedRegion(void);
PNodeP markInline(char *nameP);
PNodeP markCeiling(PNodeP exprP);
SymListP addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc);
SymNodeP findSymbolInBank(int bank, char *nameP);

int yyerror(const char *errstr);
void verror(const char *msgP, ...);
void vwarn(int errtype, const char *msgP, ...);
void vwarnl(int errType, int lineno, const char *msgP, ...);
void verrorl(int lineno, const char *msgP, ...);
void verrorlf(int lineno, const char *fileNameP, const char *msgP, ...);
void setRefSource(PNodeP nodeP);

int yylex(void);

extern int evalExpr(PNodeP);
extern int countAscii(char *strP);
extern int countType340(char *strP);
extern int countText(FlexText text);
extern long int hashExpr(PNodeP);
extern bool doWarn(int errType);
extern bool fileIsMain(char *nameP);
extern void checkPCBound(char *msgP, int pc, int lineNo);
extern void leave(int);
extern LocalContextP newLocalContext(void);
extern PNodeP binop(int lineNo, int pc, int value, PNodeP leftP, PNodeP rightP);
extern PNodeP unop(int lineNo, int pc, int value, PNodeP expP);
extern PNodeP newnode(int lineNo, int pc, int val, PNodeP leftP, PNodeP rightP);

%}

%start program

%union {
    long int ival;
    char *strP;
    SymNodeP symP;
    PNodeP pnodeP;
    FlexText flexText;
    }

/* typed symbols */

%token <pnodeP> START
%token <pnodeP> STOP
%token <pnodeP> CONSTANT
%token <pnodeP> NAMES
%token <pnodeP> VAR
%token <pnodeP> VARS
%token <pnodeP> TABLE
%token <pnodeP> ASCII
%token <pnodeP> TYPE340
%token <pnodeP> EXPORT
%token <pnodeP> IMPORT
%token <symP> OPCODE
%token <symP> OPADDR
%token <symP> OPORABLE
%token <symP> VALUESPEC
%token <symP> IMOD
%token <symP> ADDR
%token <symP> LCLADDR
%token <symP> LAW 
%token <strP> NAME
%token <strP> COMMENT
%token <strP> CSCOMMENT
%token <strP> ENDLOC
%token <strP> HEADER
%token <strP> STRING
%token <flexText> T340STRING
%token <flexText> TEXT
%token <strP> FILENAME
%token <strP> LIBFILE
%token <ival> CSSTART
%token <ival> CHAR
%token <ival> FLEXO
%token <ival> INTEGER
%token <ival> LITCHAR
%token <ival> BAD            /* returned for lexical errors */

/* various symbols */
%token ORIGIN
%token EXPR
%token BANK
%token THISBANK
%token OPTIMIZE
%token ENDOPTIMIZE
%token NOOPTIMIZE
%token ENDNOOPTIMIZE
%token SPEED
%token ENDSPEED
%token INLINEDECL
%token LOCATION
%token LCLLOCATION
%token LOCAL
%token ADDLOCAL
%token FORCELOC
%token PRIVATE
%token DOT
%token SLASH
%token AND
%token OR
%token XOR
%token CMPL
%token MINUS
%token PLUS
%token DIV
%token MOD
%token UNOP
%token BINOP
%token PARENS
%token BREF
%token WILDREF
%token ENDCONST
%token SEPARATOR TERMINATOR SEMI EMPTYLINE

%token CONSTANTS
%token CEILINGDECL      /* last, so that no other token's number moves */

/* union declarations for non-terminals */

%type <pnodeP> start
%type <pnodeP> body
%type <pnodeP> stmt_list
%type <pnodeP> stmt
%type <pnodeP> one_stmt
%type <pnodeP> expr
%type <pnodeP> simple_expr
%type <pnodeP> directive_expr
%type <pnodeP> var
%type <pnodeP> varnames
%type <strP> inline_name
%type <pnodeP> varname
%type <pnodeP> optExpr
%type <pnodeP> labelTrailer
%type <pnodeP> optLocals
%type <pnodeP> localSymDef
%type <pnodeP> symList
%type <pnodeP> symbol
%type <pnodeP> terminator
%type <pnodeP> terminators
%type <pnodeP> endConst
%type <ival> optINTEGER
%type <ival> bref
%type <ival> LOCorADDLOCorPRIVATE

/* precedence for operators */

%left TERMINATOR
%left SEPARATOR
%left LOCATION LCLLOCATION
%right CONSTANT
%left OR
%left XOR
%left AND
%left LSHIFT RSHIFT
%left PLUS MINUS
%left MUL DIV MOD
%left CMPL
%right UMINUS
%right '='
%left ENDCONST

%expect 2       // terminators

// A node only optrelayout.c makes; no rule uses it and the lexer never returns
// it.  Declared last so that every other token keeps its number.
%token RELAYOUT

%%

program         : optfilenames HEADER TERMINATOR body start
                {
                    // An optimizer span left open at the end of the source is
                    // an error, not a warning.
                    checkOptRegionClosed();
                    rootP = newnode(lineno, curBankP->cur_pc, HEADER, $4, $5);
                    rootP->value.strP = $2;
                }

optfilenames    : filenames
                |
                ;

filenames       : FILENAME
                | filenames FILENAME
                ;

start           : START simple_expr TERMINATOR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, START, NILP, NILP);
                    $$->value.ival = evalExpr($2);
                    $$->exprP = $2;         // relayout re-evaluates it
                }
                | STOP TERMINATOR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, STOP, NILP, NILP);
                }

body   : stmt_list
                {
                SymListP symlistP;

                    if( $1 )
                    {
                        // Place any constants and variables that were never emitted by an
                        // explicit directive.
                        // They go at the end of their own bank, constants first, then variables.
                        // That is the order every code generator emits them in,
                        // and the start of each block is recorded in the bank context
                        // so no generator has to infer it from cur_pc.
                        for(BankContextP bp = banksP; bp; bp = bp->nextP)
                        {
                            curBankP = bp;

                            if( bp->constSymP )
                            {
                                bp->constPC = bp->cur_pc;
                                bp->cur_pc = setConstPC(bp->cur_pc, bp->constSymP);
                                constsListP = addToSymlist(constsListP, bp->constSymP,
                                    bp->bank, bp->constPC);
                            }

                            if( bp->varNodesP )
                            {
                                bp->varPC = bp->cur_pc;
                                setVarsPC(bp->bank, bp->varNodesP);
                            }
                        }
                        curBankP = findBank(curBank);

                        // Fix any wildcarded brefs
                        resolveWildcards(wildcardsP, banksP);

                        // Resolve all constant values
                        for( symlistP = constsListP; symlistP; symlistP = symlistP->nextP )
                        {
                            setConstVal(symlistP->symP);
                        }

                        $$ = $1->leftP;        /* recover head link */
                        $1->leftP = NILP;
                    }
                    else
                    {
                        $$ = NILP;
                    }
                }
                ;

stmt_list       : stmt terminator
                {
                    $$ = $2;

                    if( $1 )
                    {
                        $2->leftP = $1;        /* keep head */
                        $1->leftP = $2;
                    }
                    else
                    {
                        $2->leftP = $2;
                    }
                }
                | terminator
                {
                    $$ = $1;
                    $1->leftP = $1;
                }
                | stmt_list terminator
                {
                    $$ = $2;
                    if( $1 )
                    {
                        $$->leftP = $1->leftP;
                        $1->leftP = $2;
                    }
                }
                | stmt_list stmt terminator
                {
                    $$ = $3;
                    if( $2 )
                    {
                        $3->leftP = $2;
                        $2->leftP = $3;
                        if( $1 )
                        {
                            $$->leftP = $1->leftP;
                            $1->leftP = $2;
                        }
                    }
                    else if( $1 )
                    {
                        $$->leftP = $1->leftP;
                        $1->leftP = $3;
                    }
                    else
                    {
                        $$ = $3;
                    }
                }
                ;

                // Many rules have to look ahead a token, and if that's a terminator,
                // the line number will have been incremented.
                // So, newnode() automatically decrements it.
                // But, for some other statements, they don't look ahead, so the line number won't
                // have been incremented yet.
                // Adjust it for those.
stmt            : one_stmt
                {
                    $$ = $1;
                    atSol = false;
                }

one_stmt        : expr
                {
                    $$ = newnode(lineno, curBankP->cur_pc, EXPR, NILP, $1);
                    checkPCBound("Code", curBankP->cur_pc, $$->lineNo);
                    if( $1 && !($1->flags & PN_NOINC) )
                    {
                        ++curBankP->cur_pc;
                    }
                }
                | BANK INTEGER
                {
                    if( ($2 < 0) || ($2 > MAXBANK) )
                    {
                        verror("Bank number must be between 0-%d decimal, %o octal, %x hex",
                            MAXBANK, MAXBANK, MAXBANK);
                    }

                    if( localContextP )
                    {
                        verror("Bank cannot be used inside a local context");
                    }

                    $$ = newnode(lineno+1, curBankP->cur_pc, BANK, NILP, NILP);
                    $$->value.ival = $2;
                    swapBanks($2);
                    $$->value2.ival = curBankP->cur_pc;   // is the pc for the new bank
                }
                | OPTIMIZE
                {
                    $$ = openOptRegion();
                }
                | ENDOPTIMIZE
                {
                    $$ = closeOptRegion();
                }
                | NOOPTIMIZE
                {
                    $$ = openHandsOff();
                }
                | ENDNOOPTIMIZE
                {
                    $$ = closeHandsOff();
                }
                | SPEED
                {
                    $$ = openSpeedRegion();
                }
                | ENDSPEED
                {
                    $$ = closeSpeedRegion();
                }
                | INLINEDECL inline_name
                {
                    $$ = markInline($2);
                }
                | CEILINGDECL simple_expr
                {
                    $$ = markCeiling($2);
                }
                | VAR varnames
                {
                    $$ = newnode(lineno, curBankP->cur_pc, VAR, NILP, $2);
                }
                | VARS
                {
                    $$ = newnode(lineno+1, curBankP->cur_pc, VARS, NILP, NILP);
                    $$->value.ptr = curBankP->varNodesP;
                    if( !curBankP->varNodesP )
                    {
                        vwarn(WARN_VARS, "no variables have been declared, variables ignored");
                    }
                    else
                    {
                        setVarsPC(curBank, curBankP->varNodesP);
                        checkPCBound("Variables", curBankP->cur_pc - 1, $$->lineNo);
                        curBankP->varNodesP = 0;
                    }
                }
                | simple_expr ORIGIN
                {
                    $$ = newnode(lineno, curBankP->cur_pc, ORIGIN, NILP, NILP);
                    $$->value.ival = curBankP->cur_pc = evalExpr($1);
                    $$->exprP = $1;         // relayout holds the value and reads this
                }
                | NAME LOCATION
                {
                    // Register the label's symbol here, before labelTrailer is parsed.
                    // This matters for a same-line self-reference like "foo, jmp foo".
                    // If registration were deferred until after labelTrailer,
                    // the lexer would not yet know "foo" when it scans the second occurrence.
                    //
                    // pendingLabelPC is saved here so the final action can
                    // use the label's starting address even if labelTrailer's
                    // TEXT/ASCII/TYPE340 alternatives advance curBankP->cur_pc before
                    // that action fires.
                    //
                    // Hack used by mactoam1 for symbols in defines.
                    // All new symbols are assumed local.
                    // We're defining this regular symbol in the local context.
                    pendingLabelPC = curBankP->cur_pc;
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        pendingLabelSymP = addLocalSymbol($1);
                        pendingLabelSymP->flags = SYMF_RESOLVED | SYMF_FORCED | SYM_LOC;
                        pendingLabelSymP->value = curBankP->cur_pc;
                        pendingLabelLocType = LCLLOCATION;
                    }
                    else
                    {
                        pendingLabelSymP = sym_make($1, 0);
                        pendingLabelSymP->flags |= SYMF_RESOLVED | SYM_GLOB;
                        pendingLabelSymP->lineno = labelTokenLine;
                        pendingLabelSymP->value = curBankP->cur_pc;
                        pendingLabelSymP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), pendingLabelSymP);
                        pendingLabelLocType = LOCATION;
                    }
                }
                labelTrailer
                {
                    // Use pendingLabelPC rather than curBankP->cur_pc here.
                    // If labelTrailer matched a TEXT/ASCII/TYPE340 alternative it already
                    // advanced curBankP->cur_pc and also set PN_NOINC on the node.
                    if( $4 && !($4->flags & PN_NOINC) )
                    {
                        ++curBankP->cur_pc;
                    }
                    $$ = newnode(lineno, pendingLabelPC, pendingLabelLocType, NILP, $4);
                    $$->value.symP = pendingLabelSymP;
                }
                | ADDR LOCATION
                {
                    // Capture curBankP->cur_pc before labelTrailer is parsed; a
                    // TEXT/ASCII/TYPE340 in labelTrailer will
                    // advance curBankP->cur_pc, and we need the label's starting
                    // address for both the symbol value and the node pc.
                    pendingLabelPC = curBankP->cur_pc;
                    pendingLabelLine = labelTokenLine;
                }
                labelTrailer
                {
                    // A var has no address until the vars are placed, so it is not
                    // yet resolved; without this test the label would take its name.
                    if( $1->flags & SYMF_VAR )
                    {
                        verrorl(pendingLabelLine, "label %s is already declared as a variable", $1->name);
                    }
                    else if( $1->flags & SYMF_RESOLVED )
                    {
                        verrorl(pendingLabelLine, "Duplicate label %s", $1->name);
                    }
                    else
                    {
                        if( ($1->flags & SYM_MASK) == SYM_LOC )
                        {
                            // This was from a local context when forced was in effect,
                            // fix it up.
                            $1->flags = SYM_GLOB;
                            $1->symP->flags = SYM_GLOB | SYMF_RESOLVED;
                            $1->symP->value = pendingLabelPC;
                        }

                        $1->lineno = pendingLabelLine;
                        $1->flags |= SYMF_RESOLVED;
                        $1->value = pendingLabelPC;
                        if( $4 && !($4->flags & PN_NOINC) )
                        {
                            ++curBankP->cur_pc;
                        }
                        $$ = newnode(lineno, pendingLabelPC, LOCATION, NILP, $4);
                        $$->value.symP = $1;
                    }
                }
                // A local label is always one declared by local/addlocal/private
                // (or made by %%forcelocal), which arrives here as LCLADDR.
                | LCLADDR LOCATION
                {  
                    // Capture curBankP->cur_pc before labelTrailer is parsed.
                    pendingLabelPC = curBankP->cur_pc;
                }
                labelTrailer
                {
                    if( ($1->value2 < localDepth) && !($1->flags & SYMF_PRIVATE) )
                    {
                        verror(
                    "local label %s is defined in outer scope %d, this is scope %d, can't be declared here",
                            $1->name, $1->value2, localDepth);
                    }
                    else if( $1->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate local label %s", $1->name);
                    }
                    else
                    {
                        $1->flags |= SYMF_RESOLVED;
                        $1->value = pendingLabelPC;
                        if( $4 && !($4->flags & PN_NOINC) )
                        {
                            ++curBankP->cur_pc;
                        }
                        $$ = newnode(lineno, pendingLabelPC, LCLLOCATION, NILP, $4);
                        $$->value.symP = $1;
                    }
                }
                | CONSTANTS
                {
                BankContextP ctxP;
                    // End this constant scope, if there is one, but include the node for listings
                    $$ = newnode(lineno+1, curBankP->cur_pc, CONSTANTS, NILP, NILP);

                    if( curBankP->constSymP )
                    {
                        constsListP = addToSymlist(constsListP, curBankP->constSymP, curBank, curBankP->cur_pc);
                        $$->value.symP = curBankP->constSymP;
                        curBankP->cur_pc = setConstPC(curBankP->cur_pc, curBankP->constSymP);
                        sym_init(&(curBankP->constSymP));

                        // Be sure we clear from our bank context, if we have one
                        if( (ctxP = findBank(curBank)) )
                        {
                            ctxP->constSymP = NILP;
                        }
                    }
                }
                | ASCII STRING
                {
                    $$ = newnode(lineno+1, curBankP->cur_pc, ASCII, NILP, NILP);
                    $$->value.strP = $2;
                    curBankP->cur_pc += countAscii($2);
                    checkPCBound("Ascii", curBankP->cur_pc - 1, $$->lineNo);
                }
                | TYPE340 T340STRING
                {
                    // Will already have been converted in the lexer.
                    // We reuse the Flex struct because this is also a counted-length string.
                    $$ = newnode(lineno+1, curBankP->cur_pc, TYPE340, NILP, NILP);
                    $$->value.flexText = $2;
                    curBankP->cur_pc += countText($2);
                    checkPCBound("Type340", curBankP->cur_pc - 1, $$->lineNo);
                }
                | TEXT
                {
                    $$ = newnode(lineno+1, curBankP->cur_pc, TEXT, NILP, NILP);
                    $$->value.flexText = $1;
                    curBankP->cur_pc += countText($1);
                    checkPCBound("Text", curBankP->cur_pc - 1, $$->lineNo);
                }
                | TABLE simple_expr
                {
                    $$ = newnode(lineno, curBankP->cur_pc, TABLE, NILP, NILP);
                    $$->value.ival = evalExpr($2);
                    $$->exprP = $2;         // relayout holds the count and reads this
                    curBankP->cur_pc += $$->value.ival;
                    checkPCBound("Table", curBankP->cur_pc - 1, $$->lineNo);
                }
                | TABLE simple_expr LOCATION simple_expr
                {
                    $$ = newnode(lineno, curBankP->cur_pc, TABLE, NILP, $4);
                    $$->value.ival = evalExpr($2);
                    $$->exprP = $2;         // as above
                    curBankP->cur_pc += $$->value.ival;
                    checkPCBound("Table", curBankP->cur_pc - 1, $$->lineNo);
                }
                | EXPORT symList
                {
                PNodeP nodeP;

                    nodeP = $2->leftP;      // recover head link
                    $2->leftP = NILP;

                    addExports(nodeP);
                    $$ = newnode(lineno, curBankP->cur_pc, EXPORT, NILP, nodeP);
                    $$->flags |= PN_NOINC;
                    doSymtab = true;        // and force output
                }
                | IMPORT STRING
                {
                    $$ = newnode(lineno, curBankP->cur_pc, IMPORT, NILP, NILP);
                    $$->value.strP = $2;
                    importSymbols($2);
                }
                | IMPORT LIBFILE
                {
                    $$ = newnode(lineno, curBankP->cur_pc, IMPORT, NILP, NILP);
                    $$->value.strP = $2;
                    importSymbols($2);
                }
                | CSSTART CSCOMMENT
                {
                    $$ = newnode($1, curBankP->cur_pc, CSCOMMENT, NILP, NILP);
                    $$->value.strP = $2;
                }
                ;

terminator      : terminators
                {
                    $$ = $1;
                    atSol = true;
                }
                | SEMI
                {
                    $$ = newnode(lineno, curBankP->cur_pc, SEMI, NILP, NILP);
                }
                | COMMENT
                {
                    $$ = newnode(lineno, curBankP->cur_pc, COMMENT, NILP, NILP);
                    $$->value.strP = $1;
                    if( atSol )
                    {
                        $$->flags |= PN_SOL;
                    }
                    atSol = true;
                }
                | FILENAME
                {
                    $$ = newnode(lineno, curBankP->cur_pc, FILENAME, NILP, NILP);
                    $$->value.strP = $1;
                    if( dropIncludeText && !fileIsMain($1) )
                    {
                        $$->flags |= PN_NOTEXT;
                    }
                    atSol = true;
                }
                ;

terminators     : TERMINATOR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, TERMINATOR, NILP, NILP);
                }
                | EMPTYLINE
                {
                    $$ = newnode(lineno, curBankP->cur_pc, EMPTYLINE, NILP, NILP);
                }
                | terminators TERMINATOR
                {
                    $$ = $1;
                }
                | terminators EMPTYLINE
                {
                    $$ = $1;
                }
                ;

optExpr         : expr
                {
                    $$ = $1;
                }
                |
                {
                    // empty
            $$ = NILP;
                }
                ;

labelTrailer    : optExpr
                {
                    $$ = $1;
                }
                | ASCII STRING
                {
                    $$ = newnode(lineno+1, pendingLabelPC, ASCII, NILP, NILP);
                    $$->value.strP = $2;
                    curBankP->cur_pc += countAscii($2);
                    checkPCBound("Ascii", curBankP->cur_pc - 1, $$->lineNo);
                    $$->flags |= PN_NOINC;
                }
                | TYPE340 T340STRING
                {
                    // Will already have been converted in the lexer.
                    $$ = newnode(lineno+1, pendingLabelPC, TYPE340, NILP, NILP);
                    $$->value.flexText = $2;
                    curBankP->cur_pc += countText($2);
                    checkPCBound("Type340", curBankP->cur_pc - 1, $$->lineNo);
                    $$->flags |= PN_NOINC;
                }
                | TEXT
                {
                    $$ = newnode(lineno+1, pendingLabelPC, TEXT, NILP, NILP);
                    $$->value.flexText = $1;
                    curBankP->cur_pc += countText($1);
                    checkPCBound("Text", curBankP->cur_pc - 1, $$->lineNo);
                    $$->flags |= PN_NOINC;
                }
                ;

optINTEGER      : INTEGER
                {
                    $$ = $1;
                }
                |   // empty
                {
                    $$ = -1;
                }
                ;

expr            : simple_expr               { $$ = $1; }
                | directive_expr            { $$ = $1; }
                | LAW
                {
                    $$ = newnode(lineno, curBankP->cur_pc, LAW, NILP, NILP);
                    $$->value.symP = $1;
                }
                | LAW optSEPARATOR simple_expr
                {
                    $$ = newnode(lineno, curBankP->cur_pc, LAW, NILP, NILP);
                    $$->value.symP = $1;
                    $$ = binop(lineno, curBankP->cur_pc, SEPARATOR, $$, $3);
                }
                ;

optSEPARATOR    : SEPARATOR
                |
                ;

simple_expr     : simple_expr SEPARATOR simple_expr { $$ = binop(lineno, curBankP->cur_pc, SEPARATOR, $1, $3); }
                | MINUS simple_expr %prec UMINUS   { $$ = unop(lineno, curBankP->cur_pc, UMINUS, $2); }
                | simple_expr PLUS simple_expr     { $$ = binop(lineno, curBankP->cur_pc, PLUS, $1, $3); }
                | simple_expr MINUS simple_expr    { $$ = binop(lineno, curBankP->cur_pc, MINUS, $1, $3); }
                | simple_expr MUL simple_expr      { $$ = binop(lineno, curBankP->cur_pc, MUL, $1, $3); }
                | simple_expr DIV simple_expr      { $$ = binop(lineno, curBankP->cur_pc, DIV, $1, $3); }
                | simple_expr MOD simple_expr      { $$ = binop(lineno, curBankP->cur_pc, MOD, $1, $3); }
                | simple_expr AND simple_expr      { $$ = binop(lineno, curBankP->cur_pc, AND, $1, $3); }
                | simple_expr OR simple_expr       { $$ = binop(lineno, curBankP->cur_pc, OR, $1, $3); }
                | simple_expr XOR simple_expr      { $$ = binop(lineno, curBankP->cur_pc, XOR, $1, $3); }
                | simple_expr LSHIFT simple_expr   { $$ = binop(lineno, curBankP->cur_pc, LSHIFT, $1, $3); }
                | simple_expr RSHIFT simple_expr   { $$ = binop(lineno, curBankP->cur_pc, RSHIFT, $1, $3); }
                | '(' simple_expr ')'              { $$ = unop(lineno, curBankP->cur_pc, PARENS, $2); }
                | CMPL simple_expr                 { $$ = unop(lineno, curBankP->cur_pc, CMPL, $2); }
                | INTEGER
                {
                    $$ = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    $$->value.ival = $1;
                }
                | THISBANK
                {
                    $$ = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    $$->value.ival = curBankP->bank << 12;
                }
                | OPCODE
                {
                    $$ = newnode(lineno, curBankP->cur_pc, OPCODE, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPADDR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, OPADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPORABLE
                {
                    $$ = newnode(lineno, curBankP->cur_pc, OPORABLE, NILP, NILP);
                    $$->value.symP = $1;
                    if( doMacro && ($1->flags & SYMF_1DOP) )
                    {
                        vwarn(WARN_1D, "%s is a PDP-1D instruction", $1->name );
                    }
                }
                | VALUESPEC
                {
                    if( $1->flags & SYMF_INDIRECT )
                    {
                        $$ = newnode(lineno, curBankP->cur_pc, IMOD, NILP, NILP);
                    }
                    else
                    {
                        $$ = newnode(lineno, curBankP->cur_pc, VALUESPEC, NILP, NILP);
                    }

                    $$->value.symP = $1;
                }
                | CONSTANT simple_expr endConst
                {
                SymNodeP symP;
                char *nameP;

                    // Jump thru hoops for constant compression
                    sprintf(scratchStr,"%ld",hashExpr($2));
                    if( !(symP = sym_find(&(curBankP->constSymP), scratchStr)) )
                    {
                        nameP = malloc(strlen(scratchStr) + 1);
                        strcpy(nameP, scratchStr);
                        symP = sym_make(nameP, 0);
                        sym_add(&(curBankP->constSymP), symP);
                        symP->ptr = $2;
                    }
                    $$ = newnode(lineno, curBankP->cur_pc, CONSTANT, NILP, $3);
                    $$->value.symP = symP;

                    // Keep this reference's expression tree on the node.
                    // The pooled symbol's ->ptr holds only the expression that
                    // was placed into the slot first, and literals are pooled
                    // by value, so every later reference whose value coincides
                    // shares the same slot and would otherwise be listed
                    // under an unrelated symbol, complete with the wrong bank
                    // qualifier.
                    $$->value2.ptr = $2;
                }
                | DOT
                {
                    $$ = newnode(lineno, curBankP->cur_pc, DOT, NILP, NILP);
                    $$->value.ival = curBankP->cur_pc;
                }
                | ADDR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | INTEGER bref
                {
                    $$ = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    $$->value.ival = $1 + ($2 << 12);
                }
                | NAME bref
                {
                SymNodeP symP;

                    if( $2 != curBank )
                    {
                        symP = findSymbolInBank($2, $1);
                    }
                    else
                    {
                       // This is just a regular global in the current bank
                        symP = sym_make($1, 0);
                        symP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                    }

                    $$ = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                    $$->value.symP = symP;
                    $$->value2.ival = $2;
                    setRefSource($$);
                }
                | ADDR bref
                {
                SymNodeP symP;

                    if( $2 != curBank )
                    {
                        symP = findSymbolInBank($2, $1->name);
                    }
                    else
                    {
                        // This is a symbol in our own bank, but that's ok
                        symP = $1;
                    }

                    $$ = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                    $$->value.symP = symP;
                    $$->value2.ival = $2;
                    setRefSource($$);
                }
                | NAME wildref
                {
                PNodeListP wildP;

                    $$ = newnode(lineno, curBankP->cur_pc, WILDREF, NILP, NILP);
                    $$->value.strP = $1;

                    // We need this for fixup later
                    wildP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    wildP->nodeP = $$;
                    wildP->nextP = wildcardsP;
                    wildcardsP = wildP;
                }
                | ADDR wildref
                {
                PNodeListP wildP;

                    if( $1->bank == curBank )
                    {
                        // This is a symbol in our own bank, resolve it now
                        $$ = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                        $$->value.symP = $1;
                        $$->value2.ival = curBank;
                        setRefSource($$);
                    }
                    else
                    {
                        // In another bank, usual wildcard processing
                        $$ = newnode(lineno, curBankP->cur_pc, WILDREF, NILP, NILP);
                        $$->value.strP = $1->name;

                        // We need this for fixup later
                        wildP = (PNodeListP)malloc(sizeof(PNodeListItem));
                        wildP->nodeP = $$;
                        wildP->nextP = wildcardsP;
                        wildcardsP = wildP;
                    }
                }
                | NAME
                {
                SymNodeP symP, symP2;

                    // a symbol we haven't seen yet, add to the global symtab
                    // unless forcelocal is in effect, then add to locals and globals
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        symP = addLocalSymbol($1);
                        // We add a new sym to globals with a ref to the local
                        symP2 = sym_make($1, 0);
                        symP2->symP = symP;
                        symP2->bank = curBank;
                        symP2->flags = SYM_LOC;
                        sym_add(&(curBankP->globalSymP), symP2);
                        symP->flags = symP2->flags = SYMF_FORCED | SYM_LOC;
                        $$ = newnode(lineno, curBankP->cur_pc, LCLADDR, NILP, NILP);
                    }
                    else
                    {
                        symP = sym_make($1, 0);
                        symP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                        $$ = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    }

                    $$->value.symP = symP;
                }
                | LCLADDR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, LCLADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | FLEXO
                {
                    $$ = newnode(lineno, curBankP->cur_pc, FLEXO, NILP, NILP);
                    $$->value.ival = $1;
                }
                | CHAR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, CHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                | LITCHAR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, LITCHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                ;

directive_expr  : FORCELOC
                {
                    if( localDepth == 0 )
                    {
                        verror("%%%%forcelocal without an opening local");    // prints %%forcelocal
                    }

                    localContextP->flags = CTX_FORCELOCAL;
                    sawForceLocal = true;
                    $$ = newnode(lineno, curBankP->cur_pc, FORCELOC, NILP, NILP);
                    $$->flags |= PN_NOINC;
                }
                | LOCorADDLOCorPRIVATE optLocals
                {
                SymNodeP symP;
                PNodeP nodeP;
                char *cP;

                    // This can be a local, private, or an addlocal.
                    // If local, push any current local scope, establish a new one.
                    // localContextP can be null if there is no current scope.
                    // If addlocal, the scope must exist and the symbols are added to it
                    // If private, if a scope exists, add to it, otherwise establish a new one.
                    if( $2 )
                    {
                        nodeP = $2->leftP;      // recover head link
                        $2->leftP = NILP;
                    }
                    else
                    {
                        nodeP = NILP;
                    }

                    $$ = newnode(lineno, curBankP->cur_pc, $1, NILP, nodeP);
                    $$->flags |= PN_NOINC;

                    if( $1 == LOCAL )
                    {
                        localStack[localDepth++] = localContextP;
                        if( localDepth > maxLocalDepth )
                        {
                            maxLocalDepth = localDepth;
                        }

                        localContextP = newLocalContext();
                        localContextP->pc = curBankP->cur_pc;    // will be the origin for any local relative refs
                        sym_init( &(localContextP->symRootP) );
                    }
                    else if( $1 == ADDLOCAL )
                    {
                        if( !localContextP )
                        {
                            verror("addlocal but not in a local context");
                        }
                    }
                    else if( $1 == PRIVATE )
                    {
                        // If there's not a context, start one.
                        if( !localContextP )
                        {
                            localStack[localDepth++] = localContextP;
                            if( localDepth > maxLocalDepth )
                            {
                                maxLocalDepth = localDepth;
                            }

                            localContextP = newLocalContext();
                            localContextP->pc = curBankP->cur_pc;
                            sym_init( &(localContextP->symRootP) );
                        }
                    }

                    while( nodeP )         // add local predefines
                    {
                        if( nodeP->type == ADDR )
                        {
                            cP = nodeP->value.symP->name;   // already a global by this name
                        }
                        else
                        {
                            cP = nodeP->value.strP;
                        }

                        symP = addLocalSymbol(cP);
                        symP->flags = SYM_LOC;
                        if( $1 == PRIVATE )
                        {
                            symP->flags |= SYMF_PRIVATE;
                        }

                        nodeP = nodeP->leftP;
                    }
                }
                | ENDLOC optINTEGER
                {
                    if( $2 > 0 )
                    {
                        if( $2 != localDepth )
                        {
                            vwarn(WARN_LOCALS, "endloc says ending level %d but the current level is %d",
                                $2, localDepth);
                        }
                    }

                    // We pop the local stack
                    if( localDepth == 0 )
                    {
                        verror("endloc without an opening local");
                    }
                    else
                    {
                        localContextP = localStack[--localDepth];
                    }

                    $$ = newnode(lineno, curBankP->cur_pc, ENDLOC, NILP, NILP);
                    $$->flags |= PN_NOINC;
                    $$->value.ival = $2;
                }
                ;

endConst        : ENDCONST
                {
                    $$ = NILP;
                }
                | COMMENT
                {
                    $$ = newnode(lineno, curBankP->cur_pc, COMMENT, NILP, NILP);
                    $$->value.strP = $1;
                }
                ;

LOCorADDLOCorPRIVATE : LOCAL
                {
                    $$ = LOCAL;
                }
                | ADDLOCAL
                {
                    $$ = ADDLOCAL;
                }
                | PRIVATE
                {
                    $$ = PRIVATE;
                }
                ;

optLocals       : localSymDef
                {
                    $$ = $1;
                    $$->leftP = $$; // keep head
                }
                | optLocals LOCATION localSymDef
                {
                    $3->leftP = $1->leftP;
                    $1->leftP = $3;
                    $$ = $3;
                }
                |
                {
                    $$ = NILP;
                }
                ;

localSymDef     : symbol
                {
                    if( $1->type == ADDR )
                    {
                        vwarn(WARN_LOCALS, "local %s will hide a global of the same name",$1->value.symP->name);
                    }

                    $$ = $1;
                }
                | LCLADDR
                {
                    if( $1->value2 > localDepth )
                    {
                        verror(
                            "local %s is already defined in this scope %d, can't be declared here",
                            $1->name, $1->value2, localDepth);
                    }

                    // We are defining a nested local with the same name as an outer one,
                    // that's fine, but warn them.
                    vwarn(WARN_LOCALS, "local %s will hide a local of the same name defined in scope %d",
                        $1->name, $1->value2);

                    $$ = newnode(lineno, curBankP->cur_pc, NAME, NILP, NILP);
                    $$->value.strP = $1->name;
                }
                ;

symList         : symbol
                {
                    $$ = $1;
                    $$->leftP = $$; // keep head
                }
                | symList LOCATION symbol
                {
                    $3->leftP = $1->leftP;
                    $1->leftP = $3;
                    $$ = $3;
                }
                ;

symbol          : NAME
                {
                    $$ = newnode(lineno, curBankP->cur_pc, NAME, NILP, NILP);
                    $$->value.strP = $1;
                }
                | ADDR
                {
                    $$ = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                ;

                /* The routine an 'inline' marking names.  The lexer returns NAME
                   for a routine not yet defined and ADDR or LCLADDR for one that
                   is; all three yield only the spelling, which optregion.c
                   resolves against the call graph, so a marking may stand before
                   or after its routine. */
inline_name     : NAME
                {
                    $$ = $1;
                }
                | ADDR
                {
                    $$ = strdup($1->name);

                    if( !$$ )
                    {
                        verror("out of memory recording an inline marking");
                    }
                }
                | LCLADDR
                {
                    $$ = strdup($1->name);

                    if( !$$ )
                    {
                        verror("out of memory recording an inline marking");
                    }
                }

varnames        : var
                {
                PNodeListP varP;

                    $$ = $1;

                    // Chain it into the list of unemitted vars
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = $$;
                    varP->nextP = curBankP->varNodesP;
                    curBankP->varNodesP = varP;
                }
                | varnames LOCATION var
                {
                PNodeListP varP;

                    $3->rightP = $1;
                    $$ = $3;

                    // And chain in the new var
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = $3;
                    varP->nextP = curBankP->varNodesP;
                    curBankP->varNodesP = varP;
                }
                ;

bref            : BREF INTEGER
                {
                    if( ($2 < 0) || ($2 > 15) )
                    {
                        verror("bank number must be 0-15 decimal, 0-17 octal");
                    }

                    $$ = $2;
                }
                | BREF DOT
                {
                    $$ = curBank;        // dot is a marker to indicate 'this bank'
                }
                ;

wildref         : BREF MUL
                ;

var             : varname
                {
                    $$ = $1;
                }
                | varname '=' simple_expr
                {
                    $1->leftP = $3;
                    $$ = $1;
                }

varname         : NAME
                {
                SymNodeP symP;

                    symP = sym_make($1, 0);
                    symP->bank = curBank;
                    sym_add(&(curBankP->globalSymP), symP);
                    symP->flags = SYM_GLOB | SYMF_VAR;
                    symP->lineno = nameTokenLine;
                    $$ = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = symP;
                }
                | ADDR
                {
                    // An earlier var of this name is not resolved until it is placed.
                    if( $1->flags & (SYMF_RESOLVED | SYMF_VAR) )
                    {
                        verrorl(nameTokenLine, "variable %s is already declared", $1->name);
                    }

                    $$ = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                    $1->lineno = nameTokenLine;
                    $1->flags = SYM_GLOB | SYMF_VAR | ($1->flags & SYMF_EXPORTED);   // 'export' may come first
                }
%%

// Add exported symbols to the current global symtab
void
addExports(PNodeP nodesP)
{
SymNodeP symP;

    while( nodesP )
    {
        switch( nodesP->type )
        {
        case NAME:
            symP = sym_make(nodesP->value.strP, 0);     // first time seen, declare it as a global
            symP->bank = curBank;
            sym_add(&(curBankP->globalSymP), symP);
            symP->flags = SYM_GLOB | SYMF_EXPORTED;
            break;

        case ADDR:
            nodesP->value.symP->flags |= SYMF_EXPORTED;
            break;
        }

        nodesP = nodesP->leftP;
    }
}

// Walk a symbol table of constants, set the pc for each,
// return the updated pc.
int
setConstPC(int pc, SymNodeP symP)
{
    if( !symP )
    {
        return(pc);
    }

    if( !(symP->flags & SYMF_ASSIGNED) )
    {
        symP->value = pc++;
        symP->flags |= SYMF_ASSIGNED;
    }

    pc = setConstPC(pc, symP->leftP);
    pc = setConstPC(pc, symP->rightP);

    checkPCBound("Constants", pc - 1, lineno);
    return( pc );
}

// Add a new entry to the passed symlist, return new head.
SymListP
addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc)
{
SymListP newP;

    newP = (SymListP)malloc(sizeof(SymList));
    newP->nextP = listP;
    newP->symP = symP;
    newP->bank = bank;
    newP->pc = pc;
    return( newP );
}

// Walk a symbol table of constants, set the value for each
void
setConstVal(SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EVALED) )
    {
        symP->value2 = evalExpr((PNodeP)(symP->ptr));
        symP->flags |= SYMF_EVALED;
    }

    setConstVal(symP->leftP);
    setConstVal(symP->rightP);
}

// Walk a list of var decls, set the pc for each VAR type found
void
setVarsPC(int bank, PNodeListP listP)
{
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        if( (symP->flags & SYMF_VAR) && !(symP->flags & SYMF_RESOLVED) )
        {
            // The .sym line is the declaration's, recorded by varname.
            symP->flags |= SYMF_RESOLVED;
            symP->value = curBankP->cur_pc++;
            symP->bank = bank;
            nodeP->pc = symP->value;
            nodeP->value2.ival = bank;
        }

        listP = listP->nextP;
    }

    checkPCBound("Variables", curBankP->cur_pc - 1, lineno);
}

// Process a symbol file, bring in all exported ones.
// If the filename starts with <, it will be terminated with > and means system include.
// Exported symbols found are created as globals in the bank they were defined in, a bank context
// will be created if needed.
void
importSymbols(char *filenameP)
{
int bank = curBank;
int origBank;
int lastBank;
int lineno;
int address;
char typec;
char *cP;
FILE *infP;
SymNodeP symP;
char str[256];
char symbol[256];

    if( *filenameP == '<' )
    {
        sprintf(str, "%s/%s", incroot, filenameP + 1);
        *strchr(str, '>') = 0;
        cP = str;
    }
    else
    {
        cP = filenameP;
    }

    if( !(infP = fopen(filenameP, "r")) )
    {
        verror("can't open import file '%s'", filenameP);
    }

    origBank = curBank;                     // will need this later
    lastBank = curBank;

    // discard first line, the file name
    if( !fgets(str, sizeof(str), infP) )
    {
        verror("file '%s' is empty", filenameP);
    }

    if( strcmp(str, "%%am1 symtab file%%\n") )
    {
        verror("file '%s' is not a symbol file", filenameP);
    }

    if( !fgets(str, sizeof(str), infP) )          // second line, the version number
    {
        verror("file '%s' is not a symbol file", filenameP);
    }

    if( strncmp(str, SYMFILEVERSION, strlen(SYMFILEVERSION)) )
    {
        verror("file '%s' is not a version %s symbol file", filenameP, SYMFILEVERSION);
    }

    if( !fgets(str, sizeof(str), infP) )              // third line, file name, just skip it
    {
        verror("error reading file '%s'", filenameP, SYMFILEVERSION);
    }

    // Ok, now the symbols
    while( fgets(str, sizeof(str), infP) )
    {
        // Remember, symbol addresses in the sym file are always octal.
        if( sscanf(str, "%o %c %s %d\n", &address, &typec, symbol, &lineno) != 4 )
        {
            verror("symbol file '%s' has an incorrect line format", filenameP);
        }

        bank = (address >> 12) & MAXBANK;
        address &= 07777;

        if( typec != 'X' )
        {
            continue;                       // not an exported symbol
        }

        if( bank != lastBank )
        {
            swapBanks(bank);
            lastBank = bank;
        }

        if( sym_find(&(curBankP->globalSymP), symbol) )
        {
            fclose(infP);
            verror("imported symbol '%s' has already been defined", symbol);
        }

        symP = sym_make(symbol, 0);
        symP->lineno = lineno;
        symP->flags |= SYMF_IMPORTED | SYMF_RESOLVED | SYM_GLOB;
        symP->value = address;
        symP->bank = bank;
        sym_add(&(curBankP->globalSymP), symP);
    }

    if( bank != origBank )
    {
        swapBanks(origBank);             // back to where we were
    }

    fclose(infP);
}

// Resolve cross-bank wildcards, turn into BREFs.
// Not the most efficient, O(wildcards*banks), but there won't be that many of them.
void
resolveWildcards(PNodeListP listP, BankContextP banksP)
{
    while( listP )
    {
        if( !resolveWildcard(listP, banksP) )
        {
            // another hack to get offending line, we're at the end of the program
            lineno = listP->nodeP->lineNo + 1;
            verror("wildcarded symbol '%s' cannot be found", listP->nodeP->value.strP);
        }

        listP = listP->nextP;
    }
}

// We search the banks backwards because the banks are listed in reverse order of first use.
// Returns true if found, else false.
bool
resolveWildcard(PNodeListP itemP, BankContextP bankP)
{
SymNodeP symP;

    if( bankP->nextP )
    {
        if( resolveWildcard(itemP, bankP->nextP) )
        {
            return(true);
        }
    }

    if( (symP = sym_find(&(bankP->globalSymP), itemP->nodeP->value.strP)) )
    {
        if( !(symP->flags & SYMF_RESOLVED) )
        {
            verror("wildcarded symbol '%s' in bank %d was never resolved", symP->name, bankP->bank);
        }

        // A bit of a hack, we turn it into a BREF
        itemP->nodeP->type = BREF;
        itemP->nodeP->value.symP = symP;
        itemP->nodeP->value2.ival = symP->bank;
        return( true );
    }

    return( false );
}

// Add a local symbol, setting the scope level
SymNodeP
addLocalSymbol(char *nameP)
{
SymNodeP symP;

    if( !localContextP )
    {
        return( NILP );
    }

    symP = sym_make(nameP, 0);
    symP->value2 = localDepth;
    sym_add(&(localContextP->symRootP), symP);
    return( symP );
}

// Pre-allocate bank 0 context so curBankP is valid before any grammar action fires.
void
initParser(void)
{
    curBankP = addBank(0);
    curBankP->cur_pc = 4;
    sym_init(&(curBankP->globalSymP));
    sym_init(&(curBankP->constSymP));
    curBankP->varNodesP = NILP;
}

// Switch to newBank: curBankP is already current; just redirect to the new context.
BankContextP
swapBanks(int newBank)
{
BankContextP newP;

    if( !(newP = findBank(newBank)) )
    {
        newP = addBank(newBank);
        sym_init(&(newP->globalSymP));
        sym_init(&(newP->constSymP));
        newP->varNodesP = NILP;
        newP->cur_pc = 0;
    }

    curBankP = newP;
    sawBank = true;
    curBank = newBank;
    return( curBankP );
}

BankContextP
addBank(int bank)
{
BankContextP newP;

    newP = (BankContextP)calloc(1, sizeof(BankContext));
    newP->bank = bank;
    newP->cur_pc = 0;       // new banks start at 0
    newP->constPC = -1;     // nothing placed automatically yet
    newP->varPC = -1;
    newP->nextP = banksP;
    banksP = newP;
    return( newP );
}

// Return the bank context if one exists for the given bank, else NILP
BankContextP
findBank(int bank)
{
BankContextP ctxP;

    for( ctxP = banksP; ctxP; ctxP = ctxP->nextP )
    {
        if( ctxP->bank == bank )
        {
            break;
        }
    }

    return( ctxP );
}

// Record where a BREF made from a name was written, for evalExpr's error if its
// symbol never resolves. The node's lineNo will not do: the reference is
// reduced mid-line, before the terminator that newnode() allows for.
void
setRefSource(PNodeP nodeP)
{
    nodeP->srcLine = nameTokenSrcLine;
    nodeP->srcFileP = strdup(filenameP);
}

// Look up a symbol in a bank, create it if not found
SymNodeP
findSymbolInBank(int bank, char *nameP)
{
BankContextP bankP;
SymNodeP symP;

    // See if it's already defined
    if( !(bankP = findBank(bank)) )
    {
        // First time we've seen this bank, add an entry.
        bankP = addBank(bank);
        sawBank = true;
    }

    if( !(symP = sym_find(&(bankP->globalSymP), nameP)) )
    {
        // Nothing in that bank, go ahead and create it.
        // If it's never resolved there, an error will be reported later.
        symP = sym_make(nameP, 0);
        symP->flags = SYM_GLOB;
        symP->bank = bank;
        sym_add(&(bankP->globalSymP), symP);
        vwarn(WARN_BANKS, "creating symbol %s in bank %d, be sure to resolve it", nameP, bank);
    }

    return( symP );
}


int
yywrap()                /* tell lex to clean up */
{
    return(1);
}

// The optimize/endoptimize region directive.  A region declares its code
// unmodified at run time, with no address taken and no timing requirement.  The
// directives emit no word, reserve no memory and move no pc; their nodes are for
// the optimizer.  A nested region, a close with none open and a region crossing
// a file boundary are refused here, one left open by checkOptRegionClosed().

// Open a region at the 'optimize' the lexer has just scanned; regions are flat,
// so one inside another is refused.
// Returns the new OPTIMIZE node, carrying the file in value.strP.
PNodeP
openOptRegion(void)
{
PNodeP nodeP;

    if( optRegionOpenLine )
    {
        verror("%%%%optimize at line %d is inside the region opened at line %d in file %s; regions do not nest",
            optRegionTokenLine, optRegionOpenLine, optRegionOpenFileP);
    }

    optRegionOpenLine = optRegionTokenLine;
    optRegionOpenFileP = strdup(optRegionTokenFileP);

    if( !optRegionOpenFileP )
    {
        verror("out of memory recording an optimize region");
    }

    // newnode() stores one less than the line it is given, for rules that have
    // already taken their terminator as lookahead; this one has not.
    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, OPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->flags |= PN_NOINC;

    if( !nodeP->value.strP )
    {
        verror("out of memory recording an optimize region");
    }

    return(nodeP);
}

// Close the open region at the 'endoptimize' just scanned.  One with no region
// open, or opened in another file (an include expands wherever it is used), is
// refused.  Returns the ENDOPTIMIZE node, the opening line in value2.
PNodeP
closeOptRegion(void)
{
PNodeP nodeP;

    if( !optRegionOpenLine )
    {
        verror("%%%%endoptimize at line %d with no optimize region open",
            optRegionTokenLine);
    }

    if( strcmp(optRegionOpenFileP, optRegionTokenFileP) )
    {
        verror("%%%%endoptimize at line %d is in file %s and the region it closes was opened at line %d in file %s; a region may not cross a file boundary",
            optRegionTokenLine, optRegionTokenFileP, optRegionOpenLine, optRegionOpenFileP);
    }

    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, ENDOPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->value2.ival = optRegionOpenLine;
    nodeP->flags |= PN_NOINC;

    if( !nodeP->value.strP )
    {
        verror("out of memory recording an optimize region");
    }

    free(optRegionOpenFileP);
    optRegionOpenFileP = NILP;
    optRegionOpenLine = 0;
    return(nodeP);
}

// Refuse an optimize region, nooptimize span or speed region still open at the
// end of the source; called from the 'program' rule.
void
checkOptRegionClosed(void)
{
    if( optRegionOpenLine )
    {
        verror("the optimize region opened at line %d in file %s was never closed",
            optRegionOpenLine, optRegionOpenFileP);
    }

    // A hands-off span left open errs in the safe direction, but the span the
    // author wrote is still not the span meant.
    if( handsOffOpenLine )
    {
        verror("the nooptimize span opened at line %d in file %s was never closed",
            handsOffOpenLine, handsOffOpenFileP);
    }

    // A speed region left open offers the rest of the file to rewrites that
    // change lengths.
    if( speedOpenLine )
    {
        verror("the speed region opened at line %d in file %s was never closed",
            speedOpenLine, speedOpenFileP);
    }
}

// The speed/endspeed region and the 'inline NAME' marking: the only places a
// length-changing rewrite may happen; -O2's guess never licenses one.  A speed
// region is its own span, not a modifier on an optimize region.  An 'inline'
// marking names one routine to inline at every call, over the inline cap and
// budget.  The nodes are OPTIMIZE and ENDOPTIMIZE carrying PN_SPEED or
// PN_INLINE.

// Open a speed region at the 'speed' just scanned; speed regions do not nest,
// but one may open inside an optimize region or a nooptimize span, or hold one.
// Returns the new node.
PNodeP
openSpeedRegion(void)
{
PNodeP nodeP;

    if( speedOpenLine )
    {
        verror("%%%%speed at line %d is inside the speed region opened at line %d in file %s; speed regions do not nest",
            optRegionTokenLine, speedOpenLine, speedOpenFileP);
    }

    speedOpenLine = optRegionTokenLine;
    speedOpenFileP = strdup(optRegionTokenFileP);

    if( !speedOpenFileP )
    {
        verror("out of memory recording a speed region");
    }

    // One higher, as openOptRegion() explains.
    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, OPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->flags |= (PN_NOINC | PN_SPEED);

    if( !nodeP->value.strP )
    {
        verror("out of memory recording a speed region");
    }

    return(nodeP);
}

// Close the open speed region at the 'endspeed' just scanned; one with none
// open, or opened in another file, is refused, as for a region.
// Returns the new node, with the line the region opened at in value2.
PNodeP
closeSpeedRegion(void)
{
PNodeP nodeP;

    if( !speedOpenLine )
    {
        verror("%%%%endspeed at line %d with no speed region open",
            optRegionTokenLine);
    }

    if( strcmp(speedOpenFileP, optRegionTokenFileP) )
    {
        verror("%%%%endspeed at line %d is in file %s and the speed region it closes was opened at line %d in file %s; a speed region may not cross a file boundary",
            optRegionTokenLine, optRegionTokenFileP, speedOpenLine, speedOpenFileP);
    }

    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, ENDOPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->value2.ival = speedOpenLine;
    nodeP->flags |= (PN_NOINC | PN_SPEED);

    if( !nodeP->value.strP )
    {
        verror("out of memory recording a speed region");
    }

    free(speedOpenFileP);
    speedOpenFileP = NILP;
    speedOpenLine = 0;
    return(nodeP);
}

// Record an 'inline NAME' marking; nameP is a copy the node takes over.  It is
// not looked up here: optregion.c resolves it and reports one naming no routine.
// Returns the new node, the file in value.strP and the name in value2.strP.
PNodeP
markInline(char *nameP)
{
PNodeP nodeP;

    // One higher, as openOptRegion() explains.
    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, OPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->value2.strP = nameP;
    nodeP->flags |= (PN_NOINC | PN_INLINE);

    if( !nodeP->value.strP || !nameP )
    {
        verror("out of memory recording an inline marking");
    }

    return(nodeP);
}

// The '%%ceiling EXPR' directive: the first word of the current bank that a
// length-changing rewrite may not grow into, for memory the program fills only
// at run time.  EXPR is evaluated here, so it may use only what is already
// defined: a forward reference cannot be told from a mistake, and the value
// must not depend on the layout it limits.  Whether the assembled program
// already reaches it is the optimizer's check, under -O only.

// The name of the first symbol in an expression whose value is not yet known,
// or of a constant, whose address is not known until the pools are placed.
// Returns the name, "a constant", or NILP when every leaf is known.
static const char *
ceilingUnknown(PNodeP nodeP)
{
const char *nameP;

    for( ; nodeP; nodeP = nodeP->rightP )
    {
        switch( nodeP->type )
        {
        case ADDR:
        case LCLADDR:
        case BREF:
            if( !(nodeP->value.symP->flags & SYMF_RESOLVED) )
            {
                return(nodeP->value.symP->name);
            }
            break;

        case WILDREF:
            return(nodeP->value.strP);

        case CONSTANT:
            return("a constant");

        default:
            break;
        }

        if( (nameP = ceilingUnknown(nodeP->leftP)) )
        {
            return(nameP);
        }
    }

    return(NILP);
}

// Record a '%%ceiling EXPR' for the current bank, reduced to the bank (0166000
// is 06000 in bank 14); undefined names, 0 and values above the default are
// refused.  Returns the node, the ceiling in value2.ival and the bank in bank.
PNodeP
markCeiling(PNodeP exprP)
{
PNodeP nodeP;
const char *nameP;
int value;

    if( (nameP = ceilingUnknown(exprP)) )
    {
        verror("%%%%ceiling at line %d names %s, which has no value yet; a ceiling may use only what is already defined",
            optRegionTokenLine, nameP);
    }

    value = (evalExpr(exprP) & ADDRMASK);

    if( value == 0 )
    {
        verror("%%%%ceiling at line %d is 0 in bank %d; it names the first word no rewrite may reach, so 0 would forbid the whole bank",
            optRegionTokenLine, curBank);
    }

    if( (curBank == 0) && (value > 07751) )
    {
        verror("%%%%ceiling at line %d is %04o in bank 0, above the default of 07751 (the read-in loader's first word); a ceiling may only lower it",
            optRegionTokenLine, value);
    }

    // One higher, as openOptRegion() explains.
    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, OPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->value2.ival = value;
    nodeP->bank = curBank;
    nodeP->flags |= (PN_NOINC | PN_CEILING);

    if( !nodeP->value.strP )
    {
        verror("out of memory recording a ceiling");
    }

    return(nodeP);
}

// The nooptimize/endnooptimize hands-off span: code no optimization level may
// rewrite, whatever the analysis or -O2's guess says.  The same errors as a
// region; the nodes are OPTIMIZE and ENDOPTIMIZE carrying PN_HANDSOFF.

// Open a span at the 'nooptimize' the lexer has just scanned.  Refuses a
// second one while a span is open.  A span may open inside a region.
// Returns the new node; does not return if the span nests.
PNodeP
openHandsOff(void)
{
PNodeP nodeP;

    if( handsOffOpenLine )
    {
        verror("%%%%nooptimize at line %d is inside the span opened at line %d in file %s; spans do not nest",
            optRegionTokenLine, handsOffOpenLine, handsOffOpenFileP);
    }

    handsOffOpenLine = optRegionTokenLine;
    handsOffOpenFileP = strdup(optRegionTokenFileP);

    if( !handsOffOpenFileP )
    {
        verror("out of memory recording a nooptimize span");
    }

    // One higher, as openOptRegion() explains.
    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, OPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->flags |= (PN_NOINC | PN_HANDSOFF);

    if( !nodeP->value.strP )
    {
        verror("out of memory recording a nooptimize span");
    }

    return(nodeP);
}

// Close the open span at the 'endnooptimize' the lexer has just scanned.
// Refuses one with no span open, and one that closes a span opened in another
// file.
// Returns the new node, with the line the span opened at in value2; does not
// return on either error.
PNodeP
closeHandsOff(void)
{
PNodeP nodeP;

    if( !handsOffOpenLine )
    {
        verror("%%%%endnooptimize at line %d with no nooptimize span open",
            optRegionTokenLine);
    }

    if( strcmp(handsOffOpenFileP, optRegionTokenFileP) )
    {
        verror("%%%%endnooptimize at line %d is in file %s and the span it closes was opened at line %d in file %s; a span may not cross a file boundary",
            optRegionTokenLine, optRegionTokenFileP, handsOffOpenLine, handsOffOpenFileP);
    }

    nodeP = newnode(optRegionTokenLine + 1, curBankP->cur_pc, ENDOPTIMIZE, NILP, NILP);
    nodeP->value.strP = strdup(optRegionTokenFileP);
    nodeP->value2.ival = handsOffOpenLine;
    nodeP->flags |= (PN_NOINC | PN_HANDSOFF);

    if( !nodeP->value.strP )
    {
        verror("out of memory recording a nooptimize span");
    }

    free(handsOffOpenFileP);
    handsOffOpenFileP = NILP;
    handsOffOpenLine = 0;
    return(nodeP);
}

void
vwarn(int errType, const char *msgP, ...)
{
va_list argP;
char format[1024];

    if( !doWarn(errType) )
    {
        return;
    }

    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
}

// Same as above except the line number is passed explicitly
void
vwarnl(int errType, int lineno, const char *msgP, ...)
{
va_list argP;
char format[1024];

    if( !doWarn(errType) )
    {
        return;
    }

    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
}

// Same as verror except the line number is passed explicitly.
// A diagnostic raised from a parser action cannot rely on the global
// lineno: if bison read the statement terminator as lookahead before the
// rule reduced, the lexer has already advanced it to the next line.
// The node's own lineNo, set by newnode(), is the line the user wrote.
void
verrorl(int lineno, const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"am1: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
    leave(0);
}

void
verror(const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"am1: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
    leave(0);
}

// Same as verrorl, with the file passed too, for a diagnostic given after the
// parse, when filenameP is the last file read rather than the node's. A line
// of 0 and a NILP file fall back to lineno and filenameP.
void
verrorlf(int lineNo, const char *fileNameP, const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"am1: %s\nat line %d, file %s\n",
        msgP,(lineNo > 0)?lineNo:lineno,(fileNameP)?fileNameP:filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
    leave(0);
}

int
yyerror(const char *errstr)
{
    fprintf(stderr,"am1: %s\nat line %d, file %s\n",
    errstr,lineno,filenameP);
    leave(0);
    // never returns, just to shut up overly-picky c compilers
    return(0);
}
