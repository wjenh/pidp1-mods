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

int yyerror(const char *errstrP);

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
extern void addRoomExitCondSilent(char *dirNameP, char *destNameP, char *condNameP);
extern void addRoomExitCondMsg(char *dirNameP, char *condNameP, char *msgNameP);
extern void addRoomExitRand(char *dirNameP, char *destNameP, int percent);
extern void addRoomExitMsg(char *dirNameP, char *msgNameP);
extern void addRoomExitRandMsg(char *dirNameP, int percent, char *msgNameP);
extern void addObjectDef(char *nameP, char *vocSymP, char *locTextP, bool take,
    char *invMsgNameP, char *hereMsgNameP, char *treasureTextP);
extern void addVerbDef(char *nameP, char *vocWordP, int vocBank, VerbArgP argP, char *handlerP);
extern void addAliasDef(char *vocWordP, char *objNameP);
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
%token ALIAS

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
%token BANKKW
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

definitions : messages movement flags actions rooms objects aliases verbs
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
            /* The same gate with no message -- a failed condition falls
             * through to the next
             * entry for this direction without printing, which is what
             * adven.f4 label 12 does. LALR(1)-clean: after the condition
             * name the parser shifts on MSG and reduces on anything
             * else, and MSG starts no other roomspec. */
            | EXIT STRING STRING COND STRING
            {
                addRoomExitCondSilent($2, $3, $5);
            }
            | EXIT STRING STRING RAND INTEGER
            {
                addRoomExitRand($2, $3, $5);
            }
            /* Message-only rows: NONE in the destination
             * slot means "print and stay put". Distinguished from the
             * three forms above at token 3, so still LALR(1)-clean. */
            | EXIT STRING NONE MSG STRING
            {
                addRoomExitMsg($2, $5);
            }
            | EXIT STRING NONE RAND INTEGER MSG STRING
            {
                addRoomExitRandMsg($2, $5, $7);
            }
            /* The gated message-only row -- if the condition holds the
             * row's own action runs (print and stay); if it fails the
             * scan silently tries the next entry. adven.dat's conditional
             * N>500 rows. Note the sense:
             * on a NONE row `msg` is the ACTION, not a refusal, exactly
             * as it is on the two unconditional NONE forms above. */
            | EXIT STRING NONE COND STRING MSG STRING
            {
                addRoomExitCondMsg($2, $5, $7);
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

aliases     : /* empty */
            | aliases alias
            ;

/* A second vocabulary word for an object that
 * already has an 'object' row. findObj walks objNames/objLoc/objTake in
 * lockstep on one index, so a synonym cannot be a second objNames entry
 * without breaking that correspondence -- it becomes a row in the
 * separate two-column objAlias table instead, which findObj scans only
 * after its own walk has missed. 'voc' is reused here for the same
 * reason the verbs section uses it: it switches the lexer into RAWWORD,
 * so alias words that collide with keywords (box, key, name, ...) still
 * lex as plain identifiers. The object must already be defined -- the
 * aliases section sits after 'objects' in 'definitions' so there are no
 * forward references, exactly as objects and verbs are placed. */
alias       : ALIAS VOC STRING OBJECT STRING
            {
                addAliasDef($3, $5);
            }
            ;

verbs       : /* empty */
            | verbs verb
            ;

/* Two forms, differing only in the optional "bank <n>" that names the
 * memory bank holding this row's voc_* string. Without it the string is
 * assumed to be in bank 2, where most of them live; the motion words are
 * in bank 3 and say so. LALR-safe: after VOC STRING the lookahead is
 * either BANKKW or one of argSpec's four distinct leading tokens. */
verb        : VERB STRING VOC STRING argSpec HANDLERKW STRING
            {
                addVerbDef($2, $4, 2, $5, $7);
            }
            | VERB STRING VOC STRING BANKKW INTEGER argSpec HANDLERKW STRING
            {
                addVerbDef($2, $4, $6, $7, $9);
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
// Reports a printf-style error against the line the lexer is currently
// on, then calls fail(). Does not return.
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

// bison's own error entry point: reports the parser's message against
// the current line, then calls fail(). Does not return; the return(0)
// only satisfies the declared int result.
int
yyerror(const char *errstrP)
{
    fprintf(stderr,"advdataloader: %s at line %d\n", errstrP, yylineno);
    fail();
    return(0);
}
