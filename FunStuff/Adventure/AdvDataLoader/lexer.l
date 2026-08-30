%{
// lexer.l - lex for advdataloader

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "symtab.h"
#include "advdataloader.h"
#include "y.tab.h"

typedef enum {NORMAL, INMESSAGE} State;

static State curState = NORMAL;
%}

/* State control, used for message text */
%x DOTEXT
/* State control, used for the verb statement's voc/handler fields -- see
 * the 'voc'/'handler' keyword rules below for why this state exists. */
%x RAWWORD
%option noyywrap
%option yylineno
%option noinput
%option nounput

%%
^#.*$       {}

message     {
                curState = INMESSAGE;
                return(MESSAGE);
            }

<DOTEXT>^.*\n   {
                // A message body line, returned RAW: physical newline
                // stripped, no escape processing, no space insertion --
                // all of that is done in C (advdataloader.c), not here
                // (see SPEC-PHASE1.md "Text pipeline").
                yytext[yyleng - 1] = 0;      // strip the trailing \n

                if( !strcmp(yytext, "@") )
                {
                    BEGIN INITIAL;
                    curState = NORMAL;
                    return(ENDMSG);
                }
                else if( yytext[0] == 0 )
                {
                    // Blank line inside a block: skipped entirely, not
                    // even an empty MSGLINE (matches advtextloader.c's
                    // readSource(), which just `continue`s on one).
                }
                else
                {
                    yylval.strP = malloc(strlen(yytext) + 1);
                    strcpy(yylval.strP, yytext);
                    return(MSGLINE);
                }
            }

yes|true    {
                yylval.yesNo = true;
                return(YESNO);
            }

no|false    {
                yylval.yesNo = false;
                return(YESNO);
            }

exit        { return(EXIT); }
bit         { return(BIT); }
cond        { return(COND); }
msg         { return(MSG); }
rand        { return(RAND); }

flag        { return(FLAG); }
object      { return(OBJECT); }
direction   { return(DIRECTION); }
room        { return(ROOM); }
verb        { return(VERB); }

longmsg     { return(LONGMSG); }
shortmsg    { return(SHORTMSG); }
none        { return(NONE); }
action      { return(ACTION); }
attributes  { return(ATTRIBUTES); }

    /* Verbs section keywords (TASK-VERB-EMISSION.md). 'msgref' (not the
     * already-taken 'msg', used by room exits' COND ... MSG clause) names
     * a doSpeak row's message argument; 'karg' names a doFoo row's small
     * integer k-value; 'voc' labels the vocabulary-word field, 'handler'
     * the dispatch-target field -- same fixed-field-order, keyword-
     * labeled style as the 'object' statement.
     *
     * 'voc'/'handler' switch into the RAWWORD state (see below) for
     * exactly their following field: verbTab's real vocabulary words
     * include plain English verbs that collide with other section's
     * keywords (e.g. voc_exit, the OUT-direction synonym, collides with
     * EXIT -- ROOM's exit-row keyword) -- discovered when 'verb EXIT voc
     * exit move OUT handler doMove' failed to parse, the corpus word
     * 'exit' lexing as EXIT instead of STRING. RAWWORD forces the single
     * word right after 'voc'/'handler' to lex as a plain identifier
     * regardless of any keyword table entry, the same idea as
     * INMESSAGE/DOTEXT already uses to keep message body text from being
     * tokenized against the keyword table. */
voc         { BEGIN RAWWORD; return(VOC); }
move        { return(MOVEKW); }
msgref      { return(MSGREF); }
karg        { return(KARG); }
handler     { BEGIN RAWWORD; return(HANDLERKW); }

    /* Objects section keywords (SPEC-PHASE2.md). treasure was already
     * declared as a %token but, pre-phase-2, never actually returned by
     * the lexer -- "treasure" used to lex as a plain STRING attribute
     * name in the old skeleton object grammar. It's a real keyword now. */
name        { return(NAME); }
loc         { return(LOC); }
take        { return(TAKE); }
invmsg      { return(INVMSG); }
heremsg     { return(HEREMSG); }
treasure    { return(TREASURE); }

end         { return(END); }

0d[0-9]+    {
                // Fixed: was strtol(yytext, NULL, 16), which parsed "0d123"
                // as hex 0xd123 ('d' is a hex digit). Decimal, skip "0d".
                yylval.ival = strtol(yytext + 2, NULL, 10);
                return(INTEGER);
            }

0x[0-9A-Fa-f]+ {
                yylval.ival = strtol(yytext,NULL,16);
                return(INTEGER);
            }

[0-7]+     {
                // The default is octal (am1 convention -- a bare 8 or 9
                // has no rule here and falls through to the catch-all
                // error rule below, exactly as it should).
                yylval.ival = strtol(yytext,NULL,8);
                return(INTEGER);
            }

[a-zA-Z_][a-zA-Z0-9_]*  {
                if( curState == INMESSAGE )
                {
                    BEGIN DOTEXT;
                }

                yylval.strP = malloc(yyleng+1);
                strcpy(yylval.strP,yytext);
                return(STRING);
            }

<RAWWORD>[a-zA-Z_][a-zA-Z0-9_]*  {
                // The single word right after 'voc'/'handler' -- always
                // STRING, even if it happens to spell a keyword
                // elsewhere in the grammar (see the 'voc'/'handler'
                // rules above). One word only: back to INITIAL
                // immediately, so the field *after* this one (argSpec,
                // or the next verb statement) lexes normally again.
                BEGIN INITIAL;
                yylval.strP = malloc(yyleng+1);
                strcpy(yylval.strP,yytext);
                return(STRING);
            }

,           {
                return(COMMA);
            }

<*>[ \t\n\r]+ ;

<RAWWORD>.  {
                // Only reached for a non-whitespace, non-identifier
                // character -- the whitespace-skip rule above (<*>, so
                // it applies to RAWWORD too) and the identifier rule
                // both take priority by flex's longest-match/first-rule
                // tie-breaking, so a single space between 'voc'/
                // 'handler' and its word never lands here.
                verror("Expected a plain word after 'voc'/'handler', got '%c' (0x%02x)",
                    yytext[0], (unsigned char)yytext[0]);
            }

<DOTEXT><<EOF>> {
                verror("Unterminated message block at end of file (missing '@')");
            }

.           {
                verror("Unexpected character '%c' (0x%02x)", yytext[0], (unsigned char)yytext[0]);
            }

%%

void
gdbtarget()         // just so we can get gdb into this file easily
{
    return;
}
