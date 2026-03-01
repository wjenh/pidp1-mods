%{
/* parser.y - yacc for the PDP-1 new symbolic debugger, ad1 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#include "ad1.h"
#include "pdp1inc.h"

extern int lastAddr;
extern int base;
extern int curBank;
extern char *fmt8P;
extern char *fmt10P;
extern char *fmt16P;
extern char *fmt2P;
extern PDP1P pdp1P;

extern int brkCount;           // number of set breakpoints
extern BreakpointP activeBrkP; // we hit a breakpoint, this is it

int yyerror(const char *errstr);

extern int getLineFromAddress(int address);
extern void formatAndPrintOne(char fmt, int val2P);
extern void formatAndPrintTwo(char fmt, int valP, int fmt2, int val2P);
extern int eval(int op, int lval, int rval);
extern int yylex(void);
extern bool loadFileData(void);
extern SymbolP findSymbolByName(int bank, char *nameP);
extern void listBreaks(void);
extern void listWatches(void);

extern void helpFn(char *nameP);
extern void showFn(int addr, int base);
extern void showRegisterFn(int reg, int base);
extern void setFn(int type, int addr, int value);
extern void startFn(int addr);
extern void stopFn(void);
extern void stepFn(void);
extern void continueFn(void);
extern void nextFn(void);
extern void setBankFn(int num);
extern void setBpFn(int num, int count);
extern void deleteBpFn(int num);
extern void enableBpFn(int num);
extern void disableBpFn(int num);
extern void setWatchFn(int addr, int value);
extern void deleteWatchFn(int num);
extern void enableWatchFn(int num);
extern void disableWatchFn(int num);
extern void setBaseFn(int num);
extern void setFileFn(char *nameP);
extern void listFn(int lineNo);
extern void setWindowFn(int size);
extern void debugFn(void);
%}

%start stmt

%code requires {
typedef struct argitem_t {
    struct argitem_t *nextP;
    int value;
    } ArgItem, *ArgItemP;
}

%union {
    int ival;
    char *strP;
    SymbolP symP;
    DispatchP cmdP;
    }

/* commands */
%token BASE
%token BANK
%token BREAK
%token CONTINUE
%token DEBUG
%token DELETE
%token DISABLE
%token DOT
%token ENABLE
%token HELP
%token LIST
%token NEXT
%token SET
%token SETFILE
%token SHOW
%token START
%token STOP
%token STEP
%token QUIT
%token WATCH
%token WINDOW

%type <cmdP> BASE
%type <cmdP> BREAK
%type <cmdP> CONTINUE
%type <cmdP> DELETE
%type <cmdP> DISABLE
%type <cmdP> QUIT
%type <cmdP> ENABLE
%type <cmdP> HELP
%type <cmdP> NEXT
%type <cmdP> SET
%type <cmdP> SHOW
%type <cmdP> START
%type <cmdP> STOP
%type <cmdP> STEP
%type <cmdP> DOT
%type <cmdP> WATCH

/* typed symbols */

%token INTEGER
%type <ival> INTEGER
%token SYMBOL
%type <strP> SYMBOL
%token BREF
%type <ival> BREF
%token STRING
%type <strP> STRING

/* registers, etc */
%token REGISTER
%type <ival> REGISTER

/* other tokens */
%token LPAREN
%token RPAREN
%token LINEAT
%token SEPARATOR

/* non-terminals */
%type <cmdP> cmd
%type <ival> optINTEGER
%type <ival> optBREF
%type <ival> optBase
%type <ival> expr
%type <ival> address
%type <symP> symbol
%type <ival> arg

/* precedence for operators */

%left OR
%left XOR
%left AND
%left LSHIFT RSHIFT
%left PLUS MINUS
%left MUL DIV
%left CMPL
%left UMINUS

%expect 0

%%

stmt		: cmd
                ;
cmd		: QUIT
                {
                    return(-1);
                }
                | HELP
                {
                    helpFn(NIL);
                }
                | HELP SEPARATOR SYMBOL
                {
                    helpFn($3);
                }
                | HELP SEPARATOR DOT
                {
                    helpFn(".");
                }
                | START address
                {
                    startFn($2);
                }
                | STOP
                {
                    stopFn();
                }
                | CONTINUE
                {
                    continueFn();
                }
                | STEP
                {
                    stepFn();
                }
                | BANK SEPARATOR INTEGER
                {
                    setBankFn($3);
                }
                | BANK SEPARATOR REGISTER
                {
                    if( $3 != PCREG )
                    {
                        printf("Bank can only use pc for setting by register.\n");
                        return(0);
                    }

                    // set from the current extended address in use
                    curBank = (pdp1P->epc) >> 12 & 0xF;
                }
                | BANK
                {
                    printf("Bank %d\n", curBank);
                }
                | BREAK SEPARATOR expr optINTEGER
                {
                    setBpFn($3, $4);
                }
                | DELETE SEPARATOR INTEGER
                {
                    deleteBpFn($3);
                }
                | DELETE SEPARATOR WATCH SEPARATOR INTEGER
                {
                    deleteWatchFn($5);
                }
                | DELETE
                {
                    deleteBpFn(0);
                }
                | DELETE SEPARATOR WATCH
                {
                    deleteWatchFn(0);
                }
                | DEBUG
                {
                    debugFn();
                }
                | ENABLE SEPARATOR expr
                {
                    enableBpFn($3);
                }
                | ENABLE SEPARATOR WATCH SEPARATOR expr
                {
                    enableWatchFn($5);
                }
                | DISABLE SEPARATOR expr
                {
                    disableBpFn($3);
                }
                | DISABLE SEPARATOR WATCH SEPARATOR expr
                {
                    disableWatchFn($5);
                }
                | SHOW address optBase
                {
                    showFn($2, $3);
                }
                | SHOW SEPARATOR REGISTER optBase
                {
                    showRegisterFn($3, $4);
                }
                | SET address arg
                {
                    setFn(INTEGER, $2, $3);
                }
                | SET SEPARATOR REGISTER arg
                {
                    setFn(REGISTER, $3, $4);
                }
                | BASE SEPARATOR INTEGER
                {
                    setBaseFn($3);
                }
                | SETFILE SEPARATOR SYMBOL
                {
                    setFileFn($3);
                }
                | LIST
                {
                    listFn(NOARG);
                }
                | LIST SEPARATOR INTEGER
                {
                    listFn($3);
                }
                | LIST SEPARATOR symbol
                {
                    listFn($3->lineno);
                }
                | LIST SEPARATOR DOT 
                {
                int line;

                    if( !loadFileData() )
                    {
                        return(0);
                    }

                    line = getLineFromAddress(lastAddr);
                    if( line <= 0 )
                    {
                        printf("No line can be found for the current addres.\n");
                        return(0);
                    }

                    listFn(line);
                }
                | LIST expr
                {
                    listFn($2);
                }
                | LIST SEPARATOR LINEAT expr
                {
                int line;

                    if( !loadFileData() )
                    {
                        return(0);
                    }

                    line = getLineFromAddress($4);
                    if( line <= 0 )
                    {
                        printf("No line can be found for that addres.\n");
                        return(0);
                    }

                    listFn(line);
                }
                | NEXT
                {
                    nextFn();
                }
                | WATCH SEPARATOR expr optINTEGER
                {
                    setWatchFn($3, $4);
                }
                | WINDOW SEPARATOR INTEGER
                {
                    setWindowFn($3);
                }
                | DOT
                {
                    formatAndPrintTwo(SYMBOLIC, lastAddr, base, pdp1P->core[lastAddr]);
                    printf("\n");
                }
		;

optINTEGER      : SEPARATOR INTEGER
                {
                    $$ = $2;
                }
                | { $$ = BADNUM; }
                ;

optBase         : SEPARATOR SYMBOL
                {
                    // We do this here because lex can't tell that one of these isn't a symbol
                    // without much hadwaving.
                    if( strlen($2) != 1 )
                    {
                        printf("A base specifier must be one character b, o, d, x or c.\n");
                        return(0);
                    }

                    switch( *$2 )
                    {
                    case 'b':
                        $$ = BINARY;
                        break;
                    case 'o':
                       $$ = OCTAL;
                        break;
                    case 'd':
                        $$ = DECIMAL;
                        break;
                    case 'x':
                        $$ = HEX;
                        break;
                    case 'c':
                        $$ = ONESCMPL;
                        break;
                    case 'a':
                        $$ = ASCII;
                        break;
                    case 'f':
                        $$ = FLEX;
                        break;
                    default:
                        printf("A base speciifer must be one of b, o, d, x, a, f, or c.\n");
                        return(0);
                    }
                }
                | { $$ = base; }    // defaults to the current base
                ;

arg             : SEPARATOR expr
                {
                    $$ = $2;
                }
                ;

symbol          : SYMBOL optBREF
                {
                SymbolP symP;

                    if( !(symP = findSymbolByName($2, $1)) )
                    {
                        printf("Can't find symbol '%s' in bank %d.\n", $1, $2);
                        return(0);
                    }

                    $$ = symP;
                }

optBREF         : BREF
                {
                    $$ = $1;
                }
                | {$$ = curBank;}
                ;

address         : SEPARATOR expr
                {
                    if( ($2 < 0) || ($2 >= MAXMEM) )
                    {
                        printf("A memory address must be between 0-65535 declimal, 177777 octal, FFFF hex.\n");
                        return(0);
                    }

                    $$ = $2;
                }
                ;

expr            : MINUS expr %prec UMINUS
                {
                    $$ = eval(UMINUS, $2, 0);
                }
                | expr PLUS expr
                {
                    $$ = eval(PLUS, $1, $3);
                }
                | expr MINUS expr
                {
                    $$ = eval(MINUS, $1, $3);
                }
                | expr MUL expr
                {
                    $$ = eval(MUL, $1, $3);
                }
                | expr DIV expr
                {
                    $$ = eval(DIV, $1, $3);
                }
                | expr AND expr
                {
                    $$ = eval(AND, $1, $3);
                }
                | expr OR expr
                {
                    $$ = eval(OR, $1, $3);
                }
                | expr XOR expr
                {
                    $$ = eval(XOR, $1, $3);
                }
                | expr LSHIFT expr
                {
                    $$ = eval(LSHIFT, $1, $3);
                }
                | expr RSHIFT expr
                {
                    $$ = eval(RSHIFT, $1, $3);
                }
                | CMPL expr
                {
                    $$ = eval(CMPL, $2, 0);
                }
                | LPAREN expr RPAREN
                {
                    $$ = $2;
                }
                | symbol
                {
                    $$ = $1->address;
                }
                | INTEGER
                {
                    $$ = $1;
                }
                | DOT
                {
                    $$ = lastAddr;
                }
                ;
%%
int
yywrap()
{
    return(1);
}

int
yyerror(const char *errstr)
{
    printf("\nInvalid command line, %s\n", errstr);
    return(0);
}
