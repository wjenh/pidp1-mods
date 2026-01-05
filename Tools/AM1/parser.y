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
extern SymListP constsListP;            // the list of all constant groups

extern bool noWarn;
extern bool sawBank;
extern int lineno;
extern char *filenameP;
extern PNodeP rootP;

SymNodeP addLocalSymbol(char *nameP);
LocalContextP newLocalContext(void);
int countAscii(char *strP);
int countText(char *strP);
int setConstPC(int pc, SymNodeP constSymP);
void setConstVal(SymNodeP constSymP);
void setVarsPC(PNodeP varNodesP);
void addShare(char *strP);
void swapBanks(int newBank);
BankContextP findBank(int bank);
BankContextP addBank(int bank);
SymListP addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc);

extern SymNodeP resolveLocalSymbol(char *);

int yyerror(const char *errstr);
void verror(const char *msgP, ...);
void vwarn(const char *msgP, ...);

int yylex(void);

extern PNodeP binop(int lineNo, int pc, int value, PNodeP leftP, PNodeP rightP);
extern PNodeP unop(int lineNo, int pc, int value, PNodeP expP);
extern PNodeP newnode(int lineNo, int pc, int val, PNodeP leftP, PNodeP rightP);
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
%token <pnodeP> TABLE
%token <symP> OPCODE
%token <symP> OPADDR
%token <symP> OPORABLE
%token <symP> VALUESPEC
%token <symP> LOCAL
%token <symP> ADDR
%token <symP> LCLADDR
%token <strP> NAME
%token <strP> LCLNAME
%token <strP> COMMENT
%token <strP> HEADER
%token <strP> ASCII
%token <strP> TEXT
%token <strP> FILENAME
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
%token SEPARATOR TERMINATOR SEMI

%token ENDLOC CONSTANTS
%token RELOC ENDRELOC

/* union declarations for non-terminals */

%type <pnodeP> start
%type <pnodeP> body
%type <pnodeP> stmt_list
%type <pnodeP> stmt
%type <pnodeP> expr
%type <pnodeP> var
%type <pnodeP> varnames
%type <pnodeP> varname
%type <pnodeP> optExpr
%type <pnodeP> terminator
%type <pnodeP> terminators
%type <ival> optINTEGER
%type <ival> bref

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

%expect 1       // terminators

%%

program		: optfilenames HEADER TERMINATOR body start
                {
                    rootP = newnode(lineno, cur_pc, HEADER, $4, $5);
                    rootP->value.strP = $2;
		}

optfilenames    : filenames
                |
                ;

filenames       : FILENAME
                | filenames FILENAME
                ;

start           : START expr TERMINATOR
                {
                    $$ = newnode(lineno, cur_pc, START, NILP, NILP);
                    $$->value.ival = evalExpr($2);
                }

body		: stmt_list
		{
                PNodeP nodeP;
                SymListP symlistP;
                BankContextP bankP;

		    if( $1 )
		    {
                        if( varNodesP )          // if not null, a variables wasn't given but vars declared
                        {
                            setVarsPC(varNodesP);
                            nodeP = newnode(lineno, cur_pc, VARS, $1->leftP, varNodesP);
                            $1->leftP = nodeP;
                            $1 = nodeP;
                        }

                        if( constSymP )     // if not null, finish constants
                        {
                            if( !sawBank )
                            {
                                constsListP = addToSymlist(constsListP, constSymP, curBank, cur_pc);
                                // no extended banks, codegen will handle directly
                                setConstPC(cur_pc, constSymP);
                                nodeP = newnode(lineno, cur_pc, CONSTANTS, $1->leftP, NILP);
                                nodeP->value.symP = constSymP;
                                constSymP = NILP;
                                $1->leftP = nodeP;
                                $1 = nodeP;
                            }
                            else
                            {
                                // update this bank's consts
                                bankP = findBank(curBank);
                                bankP->cur_pc = cur_pc;
                                bankP->constSymP = constSymP;
                                constSymP = NILP;
                            }
                        }

                        // now update all banks that need it
                        for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
                        {
                            if( bankP->constSymP )
                            {
                                setConstPC(bankP->cur_pc, bankP->constSymP);
                                constsListP = addToSymlist(constsListP, bankP->constSymP,
                                    bankP->bank, bankP->cur_pc);
                            }
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
		    $$ = newnode(lineno, cur_pc, EXPR, NILP, $1);
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

                    $$ = newnode(lineno, cur_pc, BANK, NILP, NILP);
                    $$->value.ival = $2;
                    swapBanks($2);
                    $$->value2.ival = cur_pc;   // is the pc for the new bank
                    sawBank = true;
                }
                | VAR varnames
                {
                    $$ = newnode(lineno, cur_pc, VAR, NILP, $2);
                }
                | VARS
                {
                    $$ = newnode(lineno, cur_pc, VARS, NILP, varNodesP);
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
		    $$ = newnode(lineno, cur_pc, ORIGIN, NILP, NILP);
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

                    $$ = newnode(lineno, ($3 && !($3->flags & PN_NOINC))?cur_pc++:cur_pc, locType, NILP, $3);
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
                        $$ = newnode(lineno, ($3 && !($3->flags & PN_NOINC))?cur_pc++:cur_pc, LOCATION, NILP, $3);
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
                    $$ = newnode(lineno, ($3 && !($3->flags & PN_NOINC))?cur_pc++:cur_pc, LCLLOCATION, NILP, $3);
                    $$->value.symP = symP;
                }
		| LCLADDR LOCATION optExpr
                {
                    if( $1->value2 < localDepth )
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
                        $1->value = cur_pc;
                        $$ = newnode(lineno, ($3 && !($3->flags & PN_NOINC))?cur_pc++:cur_pc, LCLLOCATION, NILP, $3);
                        $$->value.symP = $1;
                    }
                }
                | CONSTANTS
                {
                SymListP symlistP;
                BankContextP ctxP;

                    // End this constant scope
                    constsListP = addToSymlist(constsListP, constSymP, curBank, cur_pc);
		    $$ = newnode(lineno, cur_pc, CONSTANTS, NILP, NILP);
                    $$->value.symP = constSymP;
                    cur_pc = setConstPC(cur_pc, constSymP);
                    sym_init(&constSymP);

                    // Be sure we clear from our bank context, if we have one
                    if( (ctxP = findBank(curBank)) )
                    {
                        ctxP->constSymP = NILP;
                    }
                }
                | ASCII
                {
		    $$ = newnode(lineno, cur_pc, ASCII, NILP, NILP);
                    $$->value.strP = $1;
                    cur_pc += countAscii($1);
                }
                | TEXT
                {
		    $$ = newnode(lineno, cur_pc, TEXT, NILP, NILP);
                    $$->value.strP = $1;
                    cur_pc += countText($1);
                }
                | TABLE expr
                {
                    $$ = newnode(lineno, cur_pc, TABLE, NILP, NILP);
                    $$->value.ival = evalExpr($2);
                    cur_pc += $$->value.ival;
                }
                | TABLE expr LOCATION expr
                {
                    $$ = newnode(lineno, cur_pc, TABLE, NILP, $4);
                    $$->value.ival = evalExpr($2);
                    cur_pc += $$->value.ival;
                }
                ;

terminator      : terminators
                {
                    $$ = $1;
                }
                | SEMI
                {
                    $$ = newnode(lineno, cur_pc, SEMI, NILP, NILP);
                }
                | COMMENT
                {
                    $$ = newnode(lineno, cur_pc, COMMENT, NILP, NILP);
                    $$->value.strP = $1;
                }
                | FILENAME
                {
		    $$ = newnode(lineno, cur_pc, FILENAME, NILP, NILP);
                    $$->value.strP = $1;
                }
                ;

terminators     : TERMINATOR
                {
                    $$ = newnode(lineno, cur_pc, TERMINATOR, NILP, NILP);
                }
                | terminators TERMINATOR
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

optINTEGER      : INTEGER
                {
                    $$ = $1;
                }
                |   // empty
                {
                    $$ = -1;
                }
                ;

expr		: expr SEPARATOR expr       { $$ = binop(lineno, cur_pc, SEPARATOR, $1, $3); }
                | MINUS expr %prec UMINUS   { $$ = unop(lineno, cur_pc, UMINUS, $2); }
                | expr PLUS expr            { $$ = binop(lineno, cur_pc, PLUS, $1, $3); }
                | expr MINUS expr           { $$ = binop(lineno, cur_pc, MINUS, $1, $3); }
                | expr MUL expr             { $$ = binop(lineno, cur_pc, MUL, $1, $3); }
                | expr DIV expr             { $$ = binop(lineno, cur_pc, DIV, $1, $3); }
                | expr MOD expr             { $$ = binop(lineno, cur_pc, MOD, $1, $3); }
                | expr AND expr             { $$ = binop(lineno, cur_pc, AND, $1, $3); }
                | expr OR expr              { $$ = binop(lineno, cur_pc, OR, $1, $3); }
                | expr XOR expr             { $$ = binop(lineno, cur_pc, XOR, $1, $3); }
                | '(' expr ')'              { $$ = unop(lineno, cur_pc, PARENS, $2); }
                | CMPL expr                 { $$ = unop(lineno, cur_pc, CMPL, $2); }
                | INTEGER
		{
		    $$ = newnode(lineno, cur_pc, INTEGER, NILP, NILP);
		    $$->value.ival = $1;
		}
                | OPCODE
                {
                    $$ = newnode(lineno, cur_pc, OPCODE, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPADDR
                {
                    $$ = newnode(lineno, cur_pc, OPADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | OPORABLE
                {
		    $$ = newnode(lineno, cur_pc, OPORABLE, NILP, NILP);
                    $$->value.symP = $1;
                }
                | VALUESPEC
                {
		    $$ = newnode(lineno, cur_pc, VALUESPEC, NILP, NILP);
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
		    $$ = newnode(lineno, cur_pc, CONSTANT, NILP, NILP);
                    $$->value.symP = symP;
		}
		| DOT
                {
                    $$ = newnode(lineno, cur_pc, DOT, NILP, NILP);
                    $$->value.ival = cur_pc;
                }
                | ADDR
                {
                    $$ = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | INTEGER bref
                {
		    $$ = newnode(lineno, cur_pc, INTEGER, NILP, NILP);
		    $$->value.ival = $1 + ($2 << 12);
                }
                | NAME bref
                {
                BankContextP bankP;
                SymNodeP symP;

                    if( !(bankP = findBank($2)) )
                    {
                        verror("bank %d has not been used, it cannot be referenced", $2);
                    }

                    if( !(symP = sym_find(&(bankP->globalSymP), $1)) )
                    {
                        // Nothing in that bank, go ahead and create it.
                        // If it's never resolved there, an error will be reported later.
                        symP = sym_make($1, 0);
                        sym_add(&(bankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                        vwarn("bank %d is creating symbol %s in bank %d", curBank, $1, $2);
                    }

                    if( !didBrefWarn )
                    {
                        vwarn(
                        "remember that a cross-bank reference is the full 16-bit address of %s, not 12 bits",
                        symP->name);
                        didBrefWarn = true;
                    }

                    $$ = newnode(lineno, cur_pc, BREF, NILP, NILP);
                    $$->value.symP = symP;
                    $$->value2.ival = $2;
                }
                | ADDR bref
                {
                    // This is a symbol in our own bank, but that's ok
                    $$ = newnode(lineno, cur_pc, BREF, NILP, NILP);
                    $$->value.symP = $1;
                    $$->value2.ival = $2;
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
                        $$ = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    }
                    else
                    {
                        symP = sym_make($1, 0);
                        sym_add(&globalSymP, symP);
                        symP->flags = SYM_GLOB;
                        $$ = newnode(lineno, cur_pc, ADDR, NILP, NILP);
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
                    $$ = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    $$->value.symP = symP;
                }
		| LCLADDR
                {
                    $$ = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    $$->value.symP = $1;
                }
                | FLEXO
                {
                    $$ = newnode(lineno, cur_pc, FLEXO, NILP, NILP);
                    $$->value.ival = $1;
                }
                | CHAR
                {
                    $$ = newnode(lineno, cur_pc, CHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                | LITCHAR
                {
                    $$ = newnode(lineno, cur_pc, LITCHAR, NILP, NILP);
                    $$->value.ival = $1;
                }
                | LOCAL
                {
                    // We push any current local scope, establish a new one
                    // locaSymlPP can be null if there is no current scope
		    $$ = newnode(lineno, cur_pc, LOCAL, NILP, NILP);
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
		    $$ = newnode(lineno, cur_pc, FORCELOC, NILP, NILP);
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

		    $$ = newnode(lineno, cur_pc, ENDLOC, NILP, NILP);
                    $$->flags = PN_NOINC;
                }
		;

varnames        : var
                {
                    $$ = $1;

                    if( varNodesP )
                    {
                        $1->rightP = varNodesP;
                    }

                    varNodesP = $$;
                }
                | varnames LOCATION var
                {
                    $1->rightP = $3;
                    $$ = $3;
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

                    symP = sym_make($1, 0);
                    sym_add(&globalSymP, symP);
                    symP->flags = SYM_GLOB | SYMF_VAR;
                    $$ = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = symP;
                }
                | ADDR
                {
                    if( $1->flags & SYMF_RESOLVED )
                    {
                        verror("variable %s is already declared", $1);
                    }

                    $$ = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    $$->value.symP = $1;
                    $1->flags = SYM_GLOB | SYMF_VAR;
                }
%%

// Walk a symbol table of constants, set the pc for each,
// return the updated pc.
int
setConstPC(int pc, SymNodeP symP)
{
int newPC;

    if( !symP )
    {
        return(0);
    }

    if( !(symP->flags & SYMF_ASSIGNED) )
    {
        symP->value = pc++;
        symP->flags |= SYMF_ASSIGNED;
    }

    newPC = setConstPC(pc, symP->leftP);
    if( newPC > pc )
    {
        pc = newPC;
    }

    newPC = setConstPC(pc, symP->rightP);
    return( (newPC > pc)?newPC:pc );
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
            nodeP->pc = symP->value;
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

        // First time we've been to this bank, add an entry.
        newP = addBank(newBank);
        newP->cur_pc = cur_pc;
    }

    curBank = newBank;
}

BankContextP
addBank(int bank)
{
BankContextP newP;

    newP = (BankContextP)calloc(1, sizeof(BankContext));
    newP->bank = bank;
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

int
yywrap()				/* tell lex to clean up */
{
    return(1);
}

void
vwarn(const char *msgP, ...)
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

int
yyerror(const char *errstr)
{
    fprintf(stderr,"am1: %s\nat line %d, file %s\n",
	errstr,lineno,filenameP);
    leave(0);
    // never returns, just to shut up overly-picky c compilers
    return(0);
}
