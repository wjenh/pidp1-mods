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

static int lastShowAddress = NONE;
static int lastShowBase = NONE;
static int lastShowRegister = NOREG;
static bool lastShowDeref = false;

extern int curFileNo;
extern int lastAddr;
extern int getCurrentPC(void);
extern int lastFormat;
extern int curStartAddr;
extern int base;
extern int curBank;
extern char *fmt8P;
extern char *fmt10P;
extern char *fmt16P;
extern char *fmt2P;


int yyerror(const char *errstr);

extern int getLineFromAddress(int address, int fileno);
extern void formatAndPrintOne(char fmt, int val2P);
extern void formatAndPrintTwo(char fmt, int valP, int fmt2, int val2P);
extern int eval(int op, int lval, int rval);
extern int yylex(void);
extern SymbolP findSymbolByName(int bank, int fileno, char *nameP);
extern int getLineOfSymbol(SymbolP symP);

static SymbolP listedSymP;      // the symbol listSym found, for its overlay's line
static OverlayRef watchRef;     // the overlay a watch's address named, not counting its value's
static OverlayRef breakRef;     // the overlay a breakpoint's address named, not counting its condition's
static Ad1Cond userCond;        // the command's "when word = value [mask m]"
static int condRefsBefore;      // name@overlay references before the condition's word
static bool condAllowed(OverlayRefP refP, const char *whatP);
extern void listBreaks(void);
extern void listWatches(void);
extern char *getUnrestrictedFormat(int fmt);

extern void helpFn(char *nameP);
extern void showFn(int addr, int base, bool noDeref);
extern void showRegisterFn(int reg, int base);
extern void setFn(int type, int addr, int value);
extern void startFn(int addr);
extern void stopFn(void);
extern void stepFn(int count);
extern void continueFn(void);
extern void nextFn(void);
extern void formatFn(int base);
extern void setBankFn(int num);
extern void setBpFn(int num, int count, OverlayRefP refP, const Ad1Cond *userCondP);
extern void deleteBpFn(int num);
extern void enableBpFn(int num);
extern void disableBpFn(int num);
extern void setWatchFn(int addr, int value, OverlayRefP refP, const Ad1Cond *userCondP);
extern void deleteWatchFn(int num);
extern void enableWatchFn(int num);
extern void disableWatchFn(int num);
extern void setBaseFn(int num);
extern void setFileFn(char *nameP, bool add);
extern void listFn(int lineNo, int fileNo);
extern void traceFn(int addr);
extern void loadFn(char *nameP);
extern void monitorFn(int count, char *nameP);
extern void setWindowFn(int size);
extern void debugFn(void);
%}

%start stmt

// Reduce only on a lookahead that permits it.  A default reduction ran a command's action
// before the rest of the line was read, so "bank 3 junk" set the bank, then reported the
// error.  cmd is followed only by the end of the line.
%define lr.default-reduction accepting

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
%token EXIT
%token FORMAT
%token HELP
%token LIST
%token LOAD
%token MONITOR
%token NEXT
%token NODEREF
%token SET
%token SETFILE
%token SHOW
%token START
%token STOP
%token STEP
%token TRACE
%token QUIT
%token WATCH
%token WINDOW

/* typed symbols */

%token INTEGER
%type <ival> INTEGER
%token DECINTEGER
%type <ival> DECINTEGER
%token SYMBOL
%type <strP> SYMBOL
%token BREF
%type <ival> BREF
%token FILENO
%type <ival> FILENO
%token FILESTRING
%type <strP> FILESTRING

/* registers, etc */
%token REGISTER
%type <ival> REGISTER

/* other tokens */
%token LPAREN
%token RPAREN
%token LINEAT
%token LOCAL
%token SEPARATOR
%token EQUALS
%token WHEN

/* non-terminals */
%type <cmdP> cmd
%type <ival> integer
%type <ival> optDECINTEGER
%type <ival> baseSpec
%type <ival> optBase
%type <ival> optBREF
%type <ival> optFILENO
%type <ival> expr
%type <ival> dotexpr
%type <ival> address
%type <ival> watchAddress
%type <ival> breakAddress
%type <ival> condWord
%type <ival> optMask
%type <ival> optAddress
%type <ival> listSym

/* precedence for operators */

%left BREF
%left OR
%left XOR
%left AND
%left LSHIFT RSHIFT
%left PLUS MINUS
%left MUL DIV MOD
%left CMPL
%right UMINUS
%left SYMBREF

/* s-r UMINUS */
%expect 1

%%

stmt            : cmd
                ;
cmd             : QUIT
                {
                    return(QUIT);
                }
                | EXIT
                {
                    return(EXIT);
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
                | START
                {
                    startFn(curStartAddr);
                }
                | START SEPARATOR address
                {
                    startFn($3);
                }
                | STOP
                {
                    stopFn();
                }
                | CONTINUE
                {
                    continueFn();
                }
                | STEP optDECINTEGER
                {
                    stepFn(($2 == BADNUM)?1:$2);
                }
                | BANK SEPARATOR integer
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
                    curBank = BANKOF(getCurrentPC());
                }
                | BANK
                {
                    printf("Bank is ");
                    printf(getUnrestrictedFormat(lastFormat), curBank);
                    NEWLINE;
                }
                | BREAK SEPARATOR breakAddress optDECINTEGER
                {
                    // An address that names an overlay, name@overlay, binds the breakpoint to it.
                    if( breakRef.mixed )
                    {
                        printf("A breakpoint can be bound to one overlay only.\n");
                        return(0);
                    }

                    setBpFn($3, $4, (breakRef.count > 0)?&breakRef:NIL, NIL);
                }
                | BREAK SEPARATOR breakAddress SEPARATOR condition
                {
                    if( !condAllowed(&breakRef, "breakpoint") )
                    {
                        return(0);
                    }

                    setBpFn($3, BADNUM, NIL, &userCond);
                }
                | BREAK SEPARATOR breakAddress SEPARATOR DECINTEGER SEPARATOR condition
                {
                    if( !condAllowed(&breakRef, "breakpoint") )
                    {
                        return(0);
                    }

                    setBpFn($3, $5, NIL, &userCond);
                }
                | DELETE SEPARATOR DECINTEGER
                {
                    deleteBpFn($3);
                }
                | DELETE SEPARATOR WATCH SEPARATOR DECINTEGER
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
                | ENABLE SEPARATOR DECINTEGER
                {
                    enableBpFn($3);
                }
                | ENABLE SEPARATOR WATCH SEPARATOR DECINTEGER
                {
                    enableWatchFn($5);
                }
                | DISABLE SEPARATOR DECINTEGER
                {
                    disableBpFn($3);
                }
                | DISABLE SEPARATOR WATCH SEPARATOR DECINTEGER
                {
                    disableWatchFn($5);
                }
                | FORMAT optBase
                {
                    formatFn($2);
                }
                | SHOW SEPARATOR address optBase
                {
                    lastShowAddress = $3;
                    lastShowBase = ($4 == NONE)?base:$4;
                    lastShowDeref = false;
                    lastShowRegister = NOREG;
                    showFn(lastShowAddress, lastShowBase, false);
                }
                | SHOW SEPARATOR NODEREF address optBase
                {
                    lastShowAddress = $4;
                    lastShowBase = $5;
                    lastShowDeref = true;
                    lastShowRegister = NOREG;
                    showFn($4, $5, true);
                }
                | SHOW SEPARATOR REGISTER optBase
                {
                    lastShowAddress = NONE;
                    lastShowBase = $4;
                    lastShowDeref = false;
                    lastShowRegister = $3;
                    showRegisterFn($3, $4);
                }
                | SHOW
                {
                    // figure out what to repeat
                    if( lastShowRegister != NOREG )
                    {
                        showRegisterFn(lastShowRegister, lastShowBase);
                    }
                    else if( lastShowAddress != NONE )
                    {
                        showFn(lastShowAddress, lastShowBase, lastShowDeref);
                    }
                    else
                    {
                        printf("There was no previous show command to repeat.\n");
                    }
                }
                | SET SEPARATOR address SEPARATOR expr
                {
                    setFn(INTEGER, $3, $5);
                }
                | SET SEPARATOR REGISTER SEPARATOR expr
                {
                    setFn(REGISTER, $3, $5);
                }
                | BASE SEPARATOR DECINTEGER
                {
                    setBaseFn($3);
                }
                | BASE
                {
                    printf("Base is %d\n", base);
                }
                | SETFILE FILESTRING
                {
                    if( $2 && (*$2 == '+') )
                    {
                        setFileFn($2 + 1, true);
                    }
                    else
                    {
                        // $2 can be an empty string
                        setFileFn($2, false);
                    }

                    return(0);          // we consumed the input, no more lexing on this line
                }
                | LIST
                {
                    listFn(NOARG, NOARG);
                }
                | LIST SEPARATOR DECINTEGER optFILENO
                {
                    listFn($3, $4);
                }
                | LIST SEPARATOR dotexpr optFILENO
                {
                int line;

                    line = getLineFromAddress(lastAddr, $4);
                    if( line <= 0 )
                    {
                        printf("No line can be found for the current address.\n");
                        return(0);
                    }

                    listFn(line + $3, NOARG);
                }
                | LIST SEPARATOR LINEAT address optFILENO
                {
                int line;

                    line = getLineFromAddress($4, $5);
                    if( line <= 0 )
                    {
                        printf("No line can be found for that address.\n");
                        return(0);
                    }

                    listFn(line, NOARG);
                }
                | LIST SEPARATOR listSym
                {
                int line;

                    // An overlay's symbol lists at its overlay's line, whichever is in the window.
                    if( listedSymP && listedSymP->overlay )
                    {
                        line = getLineOfSymbol(listedSymP);
                    }
                    else
                    {
                        line = getLineFromAddress($3, NOARG);
                    }

                    if( line <= 0 )
                    {
                        printf("No line can be found for that address.\n");
                        return(0);
                    }

                    listFn(line, NOARG);
                }
                | TRACE optAddress
                {
                    traceFn($2);
                }
                | LOAD FILESTRING
                {
                    loadFn($2);
                }
                | MONITOR SEPARATOR DECINTEGER FILESTRING
                {
                    monitorFn($3, $4);
                }
                | NEXT
                {
                    nextFn();
                }
                | WATCH SEPARATOR watchAddress SEPARATOR expr
                {
                    if( watchRef.mixed )
                    {
                        printf("A watch can be bound to one overlay only.\n");
                        return(0);
                    }

                    setWatchFn($3, $5,(watchRef.count > 0)?&watchRef:NIL, NIL);
                }
                | WATCH SEPARATOR watchAddress
                {
                    if( watchRef.mixed )
                    {
                        printf("A watch can be bound to one overlay only.\n");
                        return(0);
                    }

                    setWatchFn($3, BADNUM,(watchRef.count > 0)?&watchRef:NIL, NIL);
                }
                | WATCH SEPARATOR watchAddress SEPARATOR expr SEPARATOR condition
                {
                    if( !condAllowed(&watchRef, "watch") )
                    {
                        return(0);
                    }

                    setWatchFn($3, $5, NIL, &userCond);
                }
                | WATCH SEPARATOR watchAddress SEPARATOR condition
                {
                    if( !condAllowed(&watchRef, "watch") )
                    {
                        return(0);
                    }

                    setWatchFn($3, BADNUM, NIL, &userCond);
                }
                | WINDOW SEPARATOR DECINTEGER
                {
                    setWindowFn($3);
                }
                | DOT
                {
                    formatAndPrintTwo(SYMBOLIC, lastAddr, base, tgtRead(lastAddr));
                    NEWLINE;
                }
                ;

integer         : INTEGER
                {
                    $$ = $1;
                }
                | DECINTEGER
                {
                char str[64];
                char *cP;
                    
                    // Messy base enforcement
                    sprintf(str, "%d", $1);
                    if( base == 2 )
                    {
                        for( cP = str; *cP; )
                        {
                            switch( *cP++ )
                            {
                            case '0':
                            case '1':
                                break;

                            default:
                                // Give up on the whole command, or it runs with a wrong value.
                                yyerror("decimal number given, but in binary mode");
                                return(0);
                            }
                        }

                        $$ = strtol(str, NIL, 2);
                    }
                    else if( base == 8 )
                    {
                        if( strchr(str, '8') || strchr(str, '9') )
                        {
                            yyerror("decimal number given, but in octal mode");
                            return(0);
                        }

                        $$ = strtol(str, NIL, 8);
                    }
                    else if( base == 16 )
                    {
                        $$ = strtol(str, NIL, 16);
                    }
                    else
                    {
                        $$ = $1;
                    }
                }

optDECINTEGER   : SEPARATOR DECINTEGER
                {
                    $$ = $2;
                }
                | { $$ = BADNUM; }
                ;

baseSpec        : SEPARATOR SYMBOL
                {
                    // We do this here because lex can't tell that one of these isn't a symbol
                    // without much handwaving.
                    if( strlen($2) != 1 )
                    {
                        printf("A base specifier must be one character b, o, d, x, a, f, s, i or c.\n");
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
                    case 's':
                        $$ = SYMBOLIC;
                        break;
                    case 'i':
                        $$ = INSTRUCTION;
                        break;
                    default:
                        printf("A base specifier must be one character b, o, d, x, a, f, s, i or c.\n");
                        return(0);
                    }
                }
                ;

optBase         : baseSpec
                {
                    $$ = $1;
                }
                | { $$ = NONE; } 
                ;

// A watch's address: the overlay it names, name@overlay, binds the watch, and is taken here
// before the value is parsed, so a value naming an overlay does not.
watchAddress    : address
                {
                    watchRef = *ovlRef();
                    $$ = $1;
                }
                ;

// A breakpoint's address: the overlay it names binds the breakpoint, taken here before a
// condition is parsed, whose word is refused if it names one.
breakAddress    : address
                {
                    breakRef = *ovlRef();
                    $$ = $1;
                }
                ;

// "when word = value [mask m]", into userCond.  The word is an address as break takes one; the
// lexer gives WHEN only before that shape, so when is no reserved word.
condition       : WHEN SEPARATOR
                {
                    condRefsBefore = ovlRef()->count;
                }
                  condWord condSep EQUALS condSep expr optMask
                {
                    userCond.isSet = 1;
                    userCond.address = (u32)$4 & 0177777;
                    userCond.mask = (u32)$9 & 0777777;
                    userCond.value = (u32)$8 & 0777777;
                }
                ;

condWord        : address
                {
                    // A window word holds whichever overlay is there, so name@overlay would test
                    // another overlay's data whenever that one is not resident.
                    if( ovlRef()->count != condRefsBefore )
                    {
                        printf("A condition's word cannot name an overlay's symbol; give its address.\n");
                        return(0);
                    }

                    $$ = $1;
                }
                ;

condSep         : SEPARATOR
                |
                ;

optMask         : SEPARATOR SYMBOL SEPARATOR expr
                {
                    if( strcmp($2, "mask") )
                    {
                        printf("Expected mask after the condition's value, not %s.\n", $2);
                        return(0);
                    }

                    $$ = $4;
                }
                | { $$ = 0777777; }
                ;

address         : expr optBREF
                {
                    if( ($1 < 0) || ($1 >= MAXMEM) )
                    {
                        printf("A memory address must be between 0-65535 decimal, 177777 octal, FFFF hex.\n");
                        return(0);
                    }

                    $$ = $1;

                    // An explicit bank ref overrides all
                    if( $2 != NOARG )
                    {
                        $$ = FULLADDR($2, $1);
                    }
                    else if( !($1 & 0170000) )
                    {
                        // If the address has no bank, use the current bank.
                        $$ |= curBank << 12;
                    }
                }
                ;

optAddress      : SEPARATOR address
                {
                    $$ = $2;
                }
                | { $$ = NOARG; }
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
                | expr MOD expr
                {
                    $$ = eval(MOD, $1, $3);
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
                | SYMBOL
                {
                SymbolP symP;

                    if( !(symP = findSymbolByName(curBank, NOARG, $1)) )
                    {
                        if( !symReported )
                        {
                            printf("Can't find symbol '%s' in bank %d.\n", $1, curBank);
                        }
                        return(0);
                    }

                    curFileNo = symP->fileNo;
                    $$ = symP->address;
                }
                | SYMBOL BREF %prec SYMBREF
                {
                SymbolP symP;

                    if( !(symP = findSymbolByName($2, NOARG, $1)) )
                    {
                        if( !symReported )
                        {
                            printf("Can't find symbol '%s' in bank %d.\n", $1, $2);
                        }
                        return(0);
                    }

                    curFileNo = symP->fileNo;
                    $$ = symP->address;
                }
                | SYMBOL LINEAT SYMBOL
                {
                SymbolP symP;

                    // name@overlay: findOverlaySymbol() says why when there is none
                    if( !(symP = findOverlaySymbol($1, $3)) )
                    {
                        return(0);
                    }

                    ovlRefNote(symP);       // for break
                    curFileNo = symP->fileNo;
                    $$ = symP->address;
                }
                | integer
                {
                    $$ = $1;
                }
                | DOT
                {
                    $$ = lastAddr;
                }
                ;

dotexpr         : DOT
                {
                    $$ = 0;
                }
                | DOT PLUS DECINTEGER
                {
                    $$ = $3;
                }
                | DOT MINUS DECINTEGER
                {
                    $$ = -$3;
                }
                ;

listSym         : SYMBOL optBREF optFILENO
                {
                SymbolP symP;

                    if( $2 == NOARG )
                    {
                        $2 = curBank;
                    }

                    if( !(symP = findSymbolByName($2, $3, $1)) )
                    {
                        if( !symReported )
                        {
                            printf("Can't find symbol '%s' in bank %d.\n", $1, $2);
                        }
                        return(0);
                    }

                    curFileNo = symP->fileNo;
                    listedSymP = symP;
                    $$ = symP->address;
                }
                | SYMBOL LINEAT SYMBOL
                {
                SymbolP symP;

                    if( !(symP = findOverlaySymbol($1, $3)) )
                    {
                        return(0);
                    }

                    curFileNo = symP->fileNo;
                    listedSymP = symP;
                    $$ = symP->address;
                }

optBREF         : BREF
                {
                    $$ = $1;
                }
                | {$$ = NOARG;}
                ;

optFILENO       : FILENO
                {
                    if( ($1 < 1) || ($1 > MAXFILES) )
                    {
                        printf("File numbers must be between 1 and %d.\n", MAXFILES);
                        return(0);
                    }

                    $$ = $1 - 1;
                }
                | { $$ = NOARG; }
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

// A condition on a breakpoint or watch bound to an overlay is refused: the emulator keeps one
// condition per slot, and the binding is it.  False, having said so, if refP binds it.
static bool
condAllowed(OverlayRefP refP, const char *whatP)
{
    if( refP->count > 0 )
    {
        printf("A %s bound to an overlay cannot take a condition too; its binding is its condition.\n",
            whatP);
        return( false );
    }

    return( true );
}
