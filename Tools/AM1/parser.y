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

int cur_pc = 4;                         // the default if not set

// We maintain a stack of local symtab ptrs for nested local scopes
int localDepth = 0;
int maxLocalDepth = 0;                  // the deepest nexting we've seen
LocalContextP localContextP;	        // used while a local scope is enabled
LocalContextP localStack[MAXLOCALS];
bool sawForceLocal;
bool didBrefWarn;

// Bank contexts are kept as a linked list, not used often enough to need preallocation
int curBank;
BankContextP banksP;

static char scratchStr[128];            // and string

static PNodeP varNodesP;                // unemitted vars

extern SymNodeP globalSymP;		// global symtab
extern SymNodeP constSymP;		// literal constants
extern SymNodeP permSymP;	        // the instructions and other permanent values

extern bool noWarn;
extern bool sawBank;
extern int lineno;
extern char *filenameP;
extern PNodeP rootP;

SymNodeP addLocalSymbol(char *nameP);
LocalContextP newLocalContext(void);
int countAscii(char *strP);
int countText(char *strP);
void setConstPC(SymNodeP constSymP);
void setVarsPC(PNodeP varNodesP);
void addShare(char *strP);
void swapBanks(int newBank);
BankContextP findBank(int bank);

extern SymNodeP resolveLocalSymbol(char *);

void yyerror();
void verror(char *msgP, ...);
void vwarn(char *msgP, ...);

int yylex(void);

extern PNodeP binop(int, int, PNodeP, PNodeP);
extern PNodeP unop(int, int, PNodeP);
extern PNodeP newnode(int, int, PNodeP, PNodeP);
extern int evalExpr(PNodeP);
extern long int hashExpr(PNodeP);
extern void leave(int);

%}

%start program

%union {
    long int ival;
    char *strP;
    SymNodeP symP;
    PNodeP pnodeP;
    }

/* typed symbols */

%token <pnodeP> START
%token <pnodeP> CONSTANT
%token <pnodeP> NAMES
%token <pnodeP> VAR
%token <pnodeP> VARS
%token <symP> OPCODE
%token <symP> OPADDR
%token <symP> OPORABLE
%token <symP> LOCAL
%token <symP> ADDR
%token <symP> LCLADDR
%token <strP> NAME
%token <strP> LCLNAME
%token <strP> COMMENT
%token <strP> HEADER
%token <strP> ASCII
%token <strP> TEXT
%token <ival> CHAR
%token <ival> FLEXO
%token <ival> INTEGER
%token <ival> LITCHAR
%token <ival> BAD			/* returned for lexical errors */

/* various symbols */
%token ORIGIN
%token EXPR
%token BANK
%token LOCATION
%token LCLLOCATION
%token FORCELOC
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
%token ENDCONST
%token SEPARATOR TERMINATOR

%token ENDLOC CONSTANTS
%token RELOC ENDRELOC

/* union declarations for non-terminals */

%type <pnodeP> start
%type <pnodeP> body
%type <pnodeP> stmt_list
%type <pnodeP> stmt
%type <pnodeP> expr
%type <pnodeP> names
%type <pnodeP> var
%type <pnodeP> varname
%type <pnodeP> optExpr
%type <pnodeP> terminator
%type <ival> optINTEGER

/* precedence for operators */

%left LOCATION LCLLOCATION
%right CONSTANT
%left OR XOR SEPARATOR
%left AND
%left PLUS MINUS
%left MUL DIV MOD
%right CMPL
%right UMINUS
%right '='
%left ENDCONST
%left TERMINATOR

%expect 0   // uminus optINTEGER terminators

%%

program		: HEADER TERMINATOR body start
                {
                    rootP = newnode(cur_pc, HEADER, $3, $4);
                    rootP->value.strP = $1;
		}

start           : START expr TERMINATOR
                {
                    $$ = newnode(cur_pc, START, NILP, NILP);
                    $$->value.ival = evalExpr($2);
                }

body		: stmt_list
		{
                PNodeP nodeP;

		    if( $1 )
		    {
                        if( varNodesP )          // if not null, a variables wasn't given but vars declared
                        {
                            setVarsPC(varNodesP);
                            nodeP = newnode(cur_pc, VARS, $1->leftP, varNodesP);
                        }

                        if( constSymP )         // if not null, a constants wasn't given but constants were used
                        {
                            setConstPC(constSymP);
                            nodeP = newnode(cur_pc, CONSTANTS, $1->leftP, NILP);
                            nodeP->value.symP = constSymP;
                            $1->leftP = nodeP;
                            $1 = nodeP;
                        }

			$$ = $1->leftP;		/* recover head link */
			$1->leftP = NILP;
		    }
		    else
                    {
			$$ = NILP;
                    }
		}
		;

stmt_list	: stmt terminator
		{
                    $$ = $2;

                    if( $1 )
                    {
                        $2->leftP = $1;		/* keep head */
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

stmt		: expr
		{
		    $$ = newnode(cur_pc, EXPR, NILP, $1);
		    if( $1 && !($1->flags & PN_NOINC) )
                    {
                        ++cur_pc;
                    }
		}
                | BANK INTEGER
                {
                    if( ($2 < 0) || ($2 > 31) )
                    {
                        verror("Bank number must be between 0-32 decimal, 037 octal, 1F hex");
                    }

                    if( localContextP )
                    {
                        verror("Bank cannot be used inside a local context");
                    }

		    $$ = newnode(cur_pc, BANK, NILP, NILP);
                    $$->value.ival = $2;
                    swapBanks($2);
                    $$->value2.ival = cur_pc;   // is the pc for the new bank
                    sawBank = true;
                }
                | VAR names
                {
                    $$ = newnode(cur_pc, VAR, NILP, $2);
                }
                | VARS
                {
                    $$ = newnode(cur_pc, VARS, NILP, varNodesP);
                    if( !varNodesP )
                    {
                        vwarn("no variables have been declareed, variables ignored");
                    }
                    else
                    {
                        setVarsPC(varNodesP);
                        varNodesP = 0;
                    }
                }
                | expr ORIGIN
                {
		    $$ = newnode(cur_pc, ORIGIN, NILP, NILP);
                    $$->value.ival = cur_pc = evalExpr($1);
                }
		| NAME LOCATION optExpr
                {
                int locType;
                SymNodeP symP;

                    // Hack used by mactoam1 for symbols in defines.
                    // All new symbols are assumed local.
                    // We're defining this regular symbol in the local context.
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        symP = addLocalSymbol($1);
                        symP->flags = SYMF_RESOLVED | SYMF_FORCED | SYM_LOC;
                        symP->value = cur_pc;
                        locType = LCLLOCATION;
                    }
                    else
                    {
                        symP = sym_make($1, 0);
                        symP->flags = SYMF_RESOLVED | SYM_GLOB;
                        symP->value = cur_pc;
                        sym_add(&globalSymP, symP);
                        locType = LOCATION;
                    }

                    $$ = newnode($3?cur_pc++:cur_pc, locType, NILP, $3);
                    $$->value.symP = symP;
                }
		| ADDR LOCATION optExpr
                {
                    if( $1->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate label %s", $1->name);
                    }
                    else
                    {
                        if( ($1->flags & SYM_MASK) == SYM_LOC )
                        {
                            // This was from a local context when forced was in effect,
                            // fix it up.
                            $1->flags = SYM_GLOB;
                            $1->symP->flags = SYM_GLOB | SYMF_RESOLVED;
                            $1->symP->value = cur_pc;
                        }

                        $1->flags |= SYMF_RESOLVED;
                        $1->value = cur_pc;
                        $$ = newnode($3?cur_pc++:cur_pc, LOCATION, NILP, $3);
                        $$->value.symP = $1;
                    }
                }
		| LCLNAME LOCATION optExpr
                {
                SymNodeP symP;

                    if( !(symP = addLocalSymbol($1)) )
                    {
                        verror("local symbol used, but not inside a local scope");
                    }

                    symP->flags = SYMF_RESOLVED | SYM_LOC;
                    symP->value = cur_pc;
                    $$ = newnode($3?cur_pc++:cur_pc, LCLLOCATION, NILP, $3);
                    $$->value.symP = symP;
                }
		| LCLADDR LOCATION optExpr
                {
                    if( $1->value2 < localDepth )
                    {
                        verror("local label %s is defined in an outer scope, can't be declared here", $1->name);
                    }
                    else if( $1->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate local label %s", $1->name);
                    }
                    else
                    {
                        $1->flags |= SYMF_RESOLVED;
                        $1->value = cur_pc;
                        $$ = newnode($3?cur_pc++:cur_pc, LCLLOCATION, NILP, $3);
                        $$->value.symP = $1;
                    }
                }
                | CONSTANTS
                {
                    // End this constant scope
                    setConstPC(constSymP);
		    $$ = newnode(cur_pc, CONSTANTS, NILP, NILP);
                    $$->value.symP = constSymP;
                    sym_init(&constSymP);
                }
                | ASCII
                {
		    $$ = newnode(cur_pc, ASCII, NILP, NILP);
                    $$->value.strP = $1;
                    cur_pc += countAscii($1);
                }
                | TEXT
                {
		    $$ = newnode(cur_pc, TEXT, NILP, NILP);
                    $$->value.strP = $1;
                    cur_pc += countText($1);
                }
                ;

terminator      : TERMINATOR
                {
                    $$ = newnode(cur_pc, TERMINATOR, NILP, NILP);
                }
                | COMMENT
                {
                    $$ = newnode(cur_pc, COMMENT, NILP, NILP);
                    $$->value.strP = $1;
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

optINTEGER      : INTEGER
                {
                    $$ = $1;
                }
                |   // empty
                {
                    $$ = -1;
                }
                ;

expr		: expr SEPARATOR expr       { $$ = binop(cur_pc, SEPARATOR, $1, $3); }
                | MINUS expr %prec UMINUS   { $$ = unop(cur_pc, UMINUS, $2); }
                | expr PLUS expr            { $$ = binop(cur_pc, PLUS, $1, $3); }
                | expr MINUS expr           { $$ = binop(cur_pc, MINUS, $1, $3); }
                | expr MUL expr             { $$ = binop(cur_pc, MUL, $1, $3); }
                | expr DIV expr             { $$ = binop(cur_pc, DIV, $1, $3); }
                | expr MOD expr             { $$ = binop(cur_pc, MOD, $1, $3); }
                | expr AND expr             { $$ = binop(cur_pc, AND, $1, $3); }
                | expr OR expr              { $$ = binop(cur_pc, OR, $1, $3); }
                | expr XOR expr             { $$ = binop(cur_pc, XOR, $1, $3); }
                | '(' expr ')'              { $$ = unop(cur_pc, PARENS, $2); }
                | CMPL expr                 { $$ = unop(cur_pc, CMPL, $2); }
                | INTEGER
		{
		    $$ = newnode(cur_pc, INTEGER, NILP, NILP);
		    $$->value.ival = $1;
		}
                | OPCODE
                {
                    $$ = newnode(cur_pc, OPCODE, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPADDR
                {
                    $$ = newnode(cur_pc, OPADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPORABLE
                {
		    $$ = newnode(cur_pc, OPORABLE, NILP, NILP);
                    $$->value.symP = $1;
                }
		| CONSTANT expr ENDCONST
		{
                int hash;
                SymNodeP symP;
                char *nameP;

                    // Jump thru hoops for constant compression
                    sprintf(scratchStr,"%ld",hashExpr($2));
                    if( !(symP = sym_find(&constSymP, scratchStr)) )
                    {
                        nameP = malloc(strlen(scratchStr) + 1);
                        strcpy(nameP, scratchStr);
                        symP = sym_make(nameP, 0);
                        sym_add(&constSymP, symP);
                        symP->ptr = $2;
                    }
		    $$ = newnode(cur_pc, CONSTANT, NILP, NILP);
                    $$->value.symP = symP;
		}
		| DOT
                {
                    $$ = newnode(cur_pc, DOT, NILP, NILP);
                    $$->value.ival = cur_pc;
                }
                | ADDR
                {
                    $$ = newnode(cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | NAME BREF INTEGER
                {
                BankContextP bankP;
                SymNodeP symP;

                    if( !(bankP = findBank($3)) )
                    {
                        verror("bank %d has not been used, it cannot be referenced", $3);
                    }

                    if( !(symP = sym_find(&(bankP->globalSymP), $1)) )
                    {
                        // Nothing in that bank, go ahead and create it.
                        // If it's never resolved there, an error will be reported later.
                        symP = sym_make($1, 0);
                        sym_add(&(bankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                        vwarn("bank %d is creating symbol %s in bank %d", curBank, $1, $3);
                    }

                    if( !didBrefWarn )
                    {
                        vwarn(
                        "remember that a cross-bank reference is the full 16-bit address of %s, not 12 bits",
                        symP->name);
                        didBrefWarn = true;
                    }

                    $$ = newnode(cur_pc, BREF, NILP, NILP);
                    $$->value.symP = symP;
                    $$->value2.ival = $3;
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
                        sym_add(&globalSymP, symP2);
                        symP->flags = symP2->flags = SYMF_FORCED | SYM_LOC;
                        $$ = newnode(cur_pc, LCLADDR, NILP, NILP);
                    }
                    else
                    {
                        symP = sym_make($1, 0);
                        sym_add(&globalSymP, symP);
                        symP->flags = SYM_GLOB;
                        $$ = newnode(cur_pc, ADDR, NILP, NILP);
                    }

                    $$->value.symP = symP;
                }
                | LCLNAME
                {
                SymNodeP symP;

                    // a symbol we haven't seen yet, add to the local symtab
                    if( !localContextP )
                    {
                        verror("local %s used outside a local scope", $1);
                    }

                    symP = addLocalSymbol($1);
                    symP->flags = SYM_LOC;
                    $$ = newnode(cur_pc, LCLADDR, NILP, NILP);
                    $$->value.symP = symP;
                }
		| LCLADDR
                {
                    $$ = newnode(cur_pc, LCLADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | FLEXO
                {
                    $$ = newnode(cur_pc, FLEXO, NILP, NILP);
                    $$->value.ival = $1;
                }
                | CHAR
                {
                    $$ = newnode(cur_pc, CHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                | LITCHAR
                {
                    $$ = newnode(cur_pc, LITCHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                | LOCAL
                {
                    // We push any current local scope, establish a new one
                    // locaSymlPP can be null if there is no current scope
		    $$ = newnode(cur_pc, LOCAL, NILP, NILP);
                    $$->flags = PN_NOINC;

                    localStack[localDepth++] = localContextP;
                    if( localDepth > maxLocalDepth )
                    {
                        maxLocalDepth = localDepth;
                    }

                    localContextP = newLocalContext();
                    localContextP->pc = cur_pc;          // will be the origin for the local relative refs
                    sym_init( &(localContextP->symRootP) );
                }
                | FORCELOC
                {
                    if( localDepth == 0 )
                    {
                        verror("%%%%forcelocal without an opening local");
                    }

                    localContextP->flags = CTX_FORCELOCAL;
                    sawForceLocal = true;
		    $$ = newnode(cur_pc, FORCELOC, NILP, NILP);
                    $$->flags = PN_NOINC;
                }
                | ENDLOC optINTEGER
                {
                    if( $2 > 0 )
                    {
                        if( $2 != localDepth )
                        {
                            vwarn("endloc says ending level %d but the current level is %d", $2, localDepth);
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

                    $$ = NILP;
                }
		;

names           : var
                {
                    $$ = $1;
                    varNodesP = $$;
                }
                | names LOCATION var
                {
                    $1->rightP = $3;
                    $$ = $3;
                }
                ;

var             : varname
                {
                    $$ = $1;
                }
                | varname '=' expr
                {
                    $1->leftP = $3;
                    $$ = $1;
                }

varname         : NAME
                {
                SymNodeP symP;

                    if( sym_find(&globalSymP, $1) )
                    {
                        verror("variable %s is already declared", $1);
                    }

                    symP = sym_make($1, 0);
                    sym_add(&globalSymP, symP);
                    symP->flags = SYM_GLOB | SYMF_VAR;
                    $$ = newnode(cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = symP;
                }
%%

// Walk a symbol table of constants, set the pc and value for each
void
setConstPC(SymNodeP nodeP)
{
    if( !nodeP )
    {
        return;
    }

    nodeP->value = cur_pc++;
    nodeP->value2 = evalExpr((PNodeP)(nodeP->ptr));
    nodeP->flags |= SYMF_RESOLVED;
    setConstPC(nodeP->leftP);
    setConstPC(nodeP->rightP);
}

// Walk a list of var decls, set the pc for each VAR type found
void
setVarsPC(PNodeP nodeP)
{
SymNodeP symP;

    while( nodeP )
    {
        symP = nodeP->value.symP;

        if( (symP->flags & SYMF_VAR) && !(symP->flags & SYMF_RESOLVED) )
        {
            symP->flags |= SYMF_RESOLVED;
            symP->value = cur_pc++;
        }

        nodeP = nodeP->rightP;
    }
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

// Used when bank is processed to switch between bank states
void
swapBanks(int newBank)
{
BankContextP newP;
BankContextP curP;

    if( !(curP = findBank(curBank)) )
    {
        // First time we've left this bank, add an entry.
        curP = (BankContextP)calloc(1, sizeof(BankContext));
        curP->bank = curBank;
        curP->nextP = banksP;
        banksP = curP;
    }

    curP->cur_pc = cur_pc;
    curP->globalSymP = globalSymP;
    curP->constSymP = constSymP;

    newP = findBank(newBank);

    if( newP )
    {
        // we've been here before, restore the state
        cur_pc = newP->cur_pc;
        globalSymP = newP->globalSymP;
        constSymP = newP->constSymP;
    }
    else
    {
        // fresh start
        sym_init(&globalSymP);
        sym_init(&constSymP);
        //cur_pc = newBank << 12;     // the real 16 bit memory address

        // First time we've been to this bank, add an entry.
        newP = (BankContextP)calloc(1, sizeof(BankContext));
        newP->bank = newBank;
        newP->nextP = banksP;
        newP->cur_pc = cur_pc;
        banksP = newP;
    }

    curBank = newBank;
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

int
yywrap()				/* tell lex to clean up */
{
    return(1);
}

void
vwarn(char *msgP, ...)
{
va_list argP;
char format[1024];

    if( noWarn )
        return;
    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
}

void
verror(char *msgP, ...)
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
yyerror(char *errstr)
{
    fprintf(stderr,"am1: %s\nat line %d, file %s\n",
	errstr,lineno,filenameP);
    leave(0);
}
