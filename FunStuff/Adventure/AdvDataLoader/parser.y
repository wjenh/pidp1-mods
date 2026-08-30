%{
/* parser.y - yacc for the Adventure game definition file */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#include "advdataloader.h"
#include "symtab.h"

extern int yylineno;

extern SymNodeP msgSymsP;
extern SymNodeP flagSymsP;
extern SymNodeP actionSymsP;
extern SymNodeP directionSymsP;
extern SymNodeP roomSymsP;
extern SymNodeP verbSymsP;

extern char msgTextBuf[MAX_TEXT];

int yyerror(const char *errstr);

extern int yylex(void);

extern void beginMessage(void);
extern void joinMsgLine(char *lineP);
extern void addMessage(char *nameP, char *textP);
extern void addDirection(char *nameP, int value);
extern void addFlag(char *nameP, int value);
extern void addAction(char *nameP, int value);
extern void registerRoom(char *nameP);
extern void finishRoom(void);
extern void setRoomLongMsg(char *nameP);
extern void setRoomShortMsg(char *nameP);
extern void addRoomFlagAttr(char *flagNameP, bool value);
extern void addRoomExit(char *dirNameP, char *destNameP);
extern void addRoomExitCond(char *dirNameP, char *destNameP, char *condNameP, char *msgNameP);
extern void addRoomExitRand(char *dirNameP, char *destNameP, int percent);
extern void addObjectDef(char *nameP, char *vocSymP, char *locTextP, bool take,
    char *invMsgNameP, char *hereMsgNameP, char *treasureTextP);
extern void addVerbDef(char *nameP, char *vocWordP, VerbArgP argP, char *handlerP);
extern void fail(void);

%}

%start definitions

%union {
    int ival;
    bool yesNo;
    char *strP;
    SymNodeP symP;
    AttributeP attrP;
    VerbArgP argP;
    }

/* Commands and untyped tokens */
%token MESSAGE
%token OBJECT
%token FLAG
%token DIRECTION
%token VERB

%token ENDMSG
%token BIT
%token ROOM
%token LONGMSG
%token SHORTMSG
%token TREASURE
%token FIXED
%token EXIT
%token END
%token NONE
%token COMMA
%token ACTION
%token ATTRIBUTES
%token COND
%token MSG
%token RAND

%token NAME
%token LOC
%token TAKE
%token INVMSG
%token HEREMSG

/* Verbs section keywords */
%token VOC
%token MOVEKW
%token MSGREF
%token KARG
%token HANDLERKW

/* typed tokens */

%token INTEGER
%type <ival> INTEGER
%token YESNO
%type <yesNo> YESNO
%token STRING
%type <strP> STRING
%token MSGLINE
%type <strP> MSGLINE

/* typed non-terminals */
%type <strP> stringOrNone
%type <strP> locVal
%type <strP> treasureVal
%type <argP> argSpec

%%

definitions : messages movement flags actions rooms objects verbs
            ;

messages    : message
            | messages message
            ;

message     : MESSAGE STRING
            {
                beginMessage();
            }
            msgtext ENDMSG
            {
                addMessage($2, msgTextBuf);
            }
            ;

msgtext     : MSGLINE
            {
                joinMsgLine($1);
            }
            | msgtext MSGLINE
            {
                joinMsgLine($2);
            }
            ;

/* Every section after 'messages' is optional (zero or more entries),
 * per SPEC-PHASE1.md, so a corpus needs only what it needs. */

movement    : /* empty */
            | movement direction
            ;

direction   : DIRECTION STRING INTEGER
            {
                addDirection($2, $3);
            }
            ;

flags       : /* empty */
            | flags flag
            ;

flag        : FLAG STRING INTEGER
            {
                // INTEGER is a bitmask for the flag, e.g. 01000
                addFlag($2, $3);
            }
            ;

actions     : /* empty */
            | actions action
            ;

action      : ACTION STRING INTEGER
            {
                addAction($2, $3);
            }
            ;

rooms       : /* empty */
            | rooms room
            ;

room        : ROOM STRING
            {
                registerRoom($2);
            }
            roomspecs END
            {
                finishRoom();
            }
            ;

roomspecs   : roomspec
            | roomspecs roomspec
            ;

/* Each EXIT form is its own alternative (rather than a nested
 * exit/exits list) so the grammar stays LALR(1)-clean; roomspecs
 * above already provides the repetition. */
roomspec    : FLAG STRING YESNO
            {
                addRoomFlagAttr($2, $3);
            }
            | LONGMSG STRING
            {
                setRoomLongMsg($2);
            }
            | SHORTMSG STRING
            {
                setRoomShortMsg($2);
            }
            | EXIT STRING STRING
            {
                addRoomExit($2, $3);
            }
            | EXIT STRING STRING COND STRING MSG STRING
            {
                addRoomExitCond($2, $3, $5, $7);
            }
            | EXIT STRING STRING RAND INTEGER
            {
                addRoomExitRand($2, $3, $5);
            }
            ;

objects     : /* empty */
            | objects object
            ;

object      : OBJECT STRING NAME STRING LOC locVal TAKE YESNO
              INVMSG stringOrNone HEREMSG stringOrNone TREASURE treasureVal
            {
                addObjectDef($2, $4, $6, $8, $10, $12, $14);
            }
            ;

/* loc is emitted VERBATIM, a room/sentinel #define
 * name (STRING) or a bare number; the generator does not interpret it
 * beyond storing text, so an INTEGER is just re-rendered as decimal text.
 */
locVal      : STRING
            {
                $$ = $1;
            }
            | INTEGER
            {
            char buf[32];

                sprintf(buf, "%d", $1);
                $$ = (char *)malloc(strlen(buf) + 1);
                strcpy($$, buf);
            }
            ;

treasureVal : YESNO
            {
                $$ = (char *)malloc(2);
                strcpy($$, $1 ? "1" : "0");
            }
            | INTEGER
            {
            char buf[32];

                if( $1 == 0 )
                {
                    strcpy(buf, "0");
                }
                else
                {
                    sprintf(buf, "0d%d", $1);
                }
                $$ = (char *)malloc(strlen(buf) + 1);
                strcpy($$, buf);
            }
            ;

verbs       : /* empty */
            | verbs verb
            ;

verb        : VERB STRING VOC STRING argSpec HANDLERKW STRING
            {
                addVerbDef($2, $4, $5, $7);
            }
            ;

argSpec     : MOVEKW STRING
            {
                $$ = (VerbArgP)malloc(sizeof(VerbArg));
                $$->kind = VERBARG_MOVE;
                $$->strVal = $2;
                $$->intVal = 0;
            }
            | NONE
            {
                $$ = (VerbArgP)malloc(sizeof(VerbArg));
                $$->kind = VERBARG_NONE;
                $$->strVal = NULL;
                $$->intVal = 0;
            }
            | MSGREF STRING
            {
                $$ = (VerbArgP)malloc(sizeof(VerbArg));
                $$->kind = VERBARG_MSGREF;
                $$->strVal = $2;
                $$->intVal = 0;
            }
            | KARG INTEGER
            {
                $$ = (VerbArgP)malloc(sizeof(VerbArg));
                $$->kind = VERBARG_KARG;
                $$->strVal = NULL;
                $$->intVal = $2;
            }
            ;

stringOrNone: STRING
            {
                $$ = $1;
            }
            | NONE
            {
                $$ = NULL;
            }
            ;

%%
void
verror(const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"advdataloader: %s at line %d\n", msgP, yylineno);
    vfprintf(stderr,format,argP);
    va_end(argP);
    fail();
}

// Like verror(), but for errors raised after the parse has already completed.
// The caller passes the line number.
void
verrorAt(int lineno, const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"advdataloader: %s at line %d\n", msgP, lineno);
    vfprintf(stderr,format,argP);
    va_end(argP);
    fail();
}

int
yyerror(const char *errstr)
{
    fprintf(stderr,"advdataloader: %s at line %d\n", errstr, yylineno);
    fail();
    // never returns, just to shut up overly-picky c compilers
    return(0);
}
