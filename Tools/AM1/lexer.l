%{
/* am1.l - lex for am1 */

#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"
#include "y.tab.h"

#define STRBUF 1024	/* maximum literal string length */

bool decimalMode = false;
bool inConstant = false;
bool sawSemi = false;
bool noIncrement = false;

extern int lineno;
extern char *filenameP;

extern SymNodeP permSymP;
extern SymNodeP globalSymP;

SymNodeP resolveLocalSymbol(char *nameP);
extern void verror(char *msgP, ...);
extern void vwarn(char *msgP, ...);
extern char asciiToFlexo(char, int *);	// in parsefns.c

int processEscape(char);
void reset_line(void);

#define YY_USER_INIT BEGIN FIRST

%}

%s BODY
%x FIRST DOASCII DOTEXT

%%

<FIRST>^[^#].*$		{
			    BEGIN BODY;
			    lineno = 1;			// always our first line
			    yylval.strP = malloc(strlen(yytext) + 1);
			    strcpy(yylval.strP, yytext);
			    return(HEADER);
			}

\%forcelocal		{
			    return(FORCELOC);
			}

local[ \t]*		{ return(LOCAL); }
endloc[ \t]*		{ return(ENDLOC); }
var[ \t]*		{ return(VAR); }
bank[ \t]*		{ return(BANK); }
\:			{ return(BREF); }
^[ \t]*start[ \t]*	{ return(START); }
^[ \t]*constants[ \t]*	{ return(CONSTANTS); }
^[ \t]*variables[ \t]*	{ return(VARS); }

ascii[ \t]+		{ BEGIN DOASCII; }
text[ \t]+		{ BEGIN DOTEXT; }

flexo[ \t][^\n;]{1,3}	{
			int i, ch, shift, nshifts, nchars;
			char *cP;
			int rslt;

			    cP = yytext;
			    // skip to the argument
			    while( *cP && !isspace(*cP) )
			    {
			        ++cP;
			    }

			    while( isspace(*cP) )
			    {
			    	++cP;
			    }

			    nchars = strlen(cP);
			    for( nshifts = shift = i = rslt = 0; i < nchars; ++i )
			    {
				ch = asciiToFlexo(*cP, &shift);
				if( ch == NONE )
				{
				    vwarn("The character '%c' is not a Flexo character, replaced with space",
				 	*cP);
				    ch = FLEX_SPACE;
				}

			    	rslt <<= 6;
				rslt |= ch;

				if( (ch != CSHIFT) && (ch != CUNSHIFT) )
				{
				    ++cP;
				}
				else
				{
				    ++nshifts;
				}
			    }

			    if( (nchars + nshifts) > 3 )
			    {
			    	vwarn("only the first %d characters used because shift/unshift codes needed",
				    nchars - nshifts);
			    }

			    while( nchars++ < 3 )
			    {
			    	rslt <<= 6;
			    }

			    yylval.ival = rslt;
			    return(FLEXO);
		        }

char[ \t]+.[lmr]	{
			int rslt;
			int shift;
			char *cP;

			    cP = yytext;
			    // skip to the argument
			    while( *cP && !isspace(*cP) )
			    {
			        ++cP;
			    }

			    while( isspace(*cP) )
			    {
			    	++cP;
			    }

			    rslt = *cP++ ;
			    shift = 0;
			    rslt = asciiToFlexo(rslt, &shift);
			    if( rslt == CSHIFT )
			    {
			    	verror("The character '%c' is a shifted Flexo character, not allowed",
				    *(cP - 1));
			    }
			    else if( rslt == NONE )
			    {
			    	vwarn("The character '%c' is not a Flexo character, replaced with space",
				    *(cP - 1));
				rslt = FLEX_SPACE;
			    }

			    switch( *cP )
			    {
			    case 'l':
				rslt <<= 12;
			    	break;
			    case 'm':
				rslt <<= 6;
			    	break;
			    case 'r':
			    	break;
			    }

			    yylval.ival = rslt;
			    return(CHAR);
			}

decimal			{ decimalMode = true; }
octal			{ decimalMode = false; }

[ ]*\&[ ]*		{ return(AND); }
[ ]*\|[ ]*		{ return(OR); }
[ ]*\^[ ]*		{ return(XOR); }
[ ]*\~[ ]*		{ return(CMPL); }
[ ]*\+[ ]*		{ return(PLUS); }
[ ]*\-[ ]*		{ return(MINUS); }
[ ]*\/[ ]*		{ return(DIV); }
[ ]*\*[ ]*		{ return(MUL); }
\.			{ return(DOT); }
\,[ \t]*		{ return(LOCATION); }

\[			{
			    inConstant = true;
			    return(CONSTANT);
			}

\]			{
			    inConstant = false;
			    return(ENDCONST);
			}

[ \t]*\/\/.*\n		{
			char *cP;

			    cP = strchr(yytext, '/') + 2;
			    yylval.strP = malloc(strlen(cP) + 1);
			    strcpy(yylval.strP, cP);
			    *strchr(yylval.strP, '\n') = 0;
			    if( !noIncrement && !sawSemi )
			    {
			        ++lineno;
			    }
			    noIncrement = sawSemi = false;
			    return(COMMENT);
			}

0x[0-9A-Fa-f]+		{
			    yylval.ival = (int)strtol(yytext,0,16);
			    return(INTEGER);
			}

0d[0-9]+		{
			    yylval.ival = (int)strtol(yytext+2,0,10);
			    return(INTEGER);
			}

0o[0-7]+		{
			    yylval.ival = (int)strtol(yytext+2,0,8);
			    return(INTEGER);
			}

[0-9]+			{
			    if( !decimalMode && (strchr(yytext, '8') || strchr(yytext, '9')) )
			    {
				verror("decimal number %s, but in octal mode", yytext);
				return(BAD);
			    }

			    yylval.ival = (int)strtol(yytext,0,(decimalMode)?10:8);
			    return(INTEGER);
			}

[1-9]s			{
			SymNodeP symP;

			    if( (symP = sym_find((SymNodeP *)&permSymP,yytext)) )
			    {
			        yylval.symP = symP;
			        return(VALUESPEC);
			    }
			    else
			    {
				verror("can't find %s in permanent symbols", yytext);
			    }
			}

[a-zA-Z_][a-zA-Z0-9_]*	{
			SymNodeP symP;

			    if( (symP = sym_find((SymNodeP *)&permSymP,yytext)) )
			    {
				switch( symP->flags & SYM_MASK )
				{
				case SYM_VALUE:
				    yylval.symP = symP;
				    return(VALUESPEC);

				case SYM_OPCODE:
				    yylval.symP = symP;
				    return(OPCODE);

				case SYM_OPADDR:
				    yylval.symP = symP;
				    return(OPADDR);

				case SYM_OPORABLE:
				    yylval.symP = symP;
				    return(OPORABLE);
				}

				verror("inernal error in perm sym lookup");
			    }
			    else if( (symP = resolveLocalSymbol(yytext)) )
			    {
				yylval.symP = symP;
				return(LCLADDR);
			    }
			    else if( (symP = sym_find(&globalSymP,yytext)) )
			    {
				yylval.symP = symP;
				return(ADDR);
			    }
			    else
			    {
				yylval.strP = malloc(yyleng+1);
				strcpy(yylval.strP,yytext);
				return(NAME);
			    }
			}

%[a-zA-Z_][a-zA-Z0-9_]*	{
			SymNodeP nodeP;

			    if( (nodeP = resolveLocalSymbol(yytext)) )
			    {
				yylval.symP = nodeP;
				return(LCLADDR);
			    }
			    else
			    {
				yylval.strP = malloc(yyleng+1);
				strcpy(yylval.strP,yytext);
				return(LCLNAME);
			    }
			}

^[\t ]+			{ }

[\t ]+			{
			    return(SEPARATOR);
			}

\/[ \t]*[^a-zA-Z0-9\~\%\[] {
			    int ch;

			    ch = yytext[yyleng-1];
			    if( ch == '\n' )
			    {
			    	++lineno;
				noIncrement = true;	// we don't want terminator to inc again
			    }

			    unput( ch );

			    if( ch == '/' )
			    {
			    	unput(' ');	// preserve a space between origin and comment
			    }
			    return(ORIGIN);
			}

^\n			{
			    if( !noIncrement )
			    {
			        ++lineno;
			    }
			    noIncrement = sawSemi = false;
			    return( TERMINATOR );
			 }

[ \t]*(\n|(\;[ \t]*))	{
			    if( inConstant )
			    {
				inConstant = false;
				if( strchr(yytext,';') )
				{
				    unput(';');
				}
				else
				{
				    unput('\n');
				}
			        return(ENDCONST);
			    }
			    else
			    {
				if( strchr(yytext,';') )
				{
				    if( !sawSemi )
				    {
				        ++lineno;		// only once until real terminator
				    }

				    sawSemi = true;
				    return(SEMI);
				}
				else
				{
				    // not semi terminated
				    if( sawSemi )
				    {
				        sawSemi = false;	// we don't incr here, all one line
				    }
				    else if( !noIncrement )
				    {
					++lineno;
				    }

				    noIncrement = false;
				    return(TERMINATOR);
				}
			    }
			}

<FIRST,BODY>^\#		{
			    // Handle cpp line number resets
			    reset_line();
			    yylval.strP = malloc(strlen(filenameP) + 1);
			    strcpy(yylval.strP, filenameP);
			    return( FILENAME );
			}

\'			{
			int tmpc;
			int number;
			int ctr;

			    tmpc = input();

			    if( tmpc == '\\' )
			    {
				switch( (tmpc = input()) ) /* process \ */
				{
				case '\n':	/* \<newline> not allowed */
				    return(BAD);
				    break;

				case 'e':
				    tmpc = '\033';	/* an escape */
				    break;

				case 'b':
				    tmpc = '\b';	/* a backspace */
				    break;

				case '^':		/* ^char */
				    tmpc = input() & 037;
				    break;

				case 'f':
				    tmpc= '\f';	/* formfeed */
				    break;

				case 'n':		/* a newline */
				    tmpc= '\n';
				    break;

				case 'r':
				    tmpc = '\r';	/* return */
				    break;

				case 't':		/* a tab */
				    tmpc = '\t';
				    break;

				case '\\':		/* an esc'd \ */
				    break;

				case '0':		/* numeric escape */
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				    number = tmpc - '0';
				    ctr = 1;

				    tmpc = input();

				    while( isdigit(tmpc) && (ctr < 3) )
				    {
					number = number*8 + (tmpc - '0');
					++ctr;
					tmpc = input();
				    }

				    unput(tmpc);
				    tmpc = number;
				    break;

				default:		/* just char as is */
				    break;
				}
			    }

			    yylval.ival = tmpc;		/* in case is a lit */

			    if( (tmpc = input()) == '\'' )
			    {
				return(LITCHAR);
			    }
			    else
			    {
				return(BAD);
			    }
			}

<DOASCII>\"		{
			int tmpc;
			char *cP;
			char string[STRBUF];

			    cP = string;

			    while( (tmpc = input()) != '"' )
			    {
				if( cP >= (string+STRBUF) )
				{
				    verror("string too long", 0);
				}

				if( (tmpc == EOF) || (tmpc == '\n') )
				{
					verror("unterminated quote in ascii string");
				}

				if( tmpc == '\\' )
				{
				    tmpc = processEscape(input());
				    if( !tmpc )
				    {
				    	continue;		// meant a line continuation
				    }
				}

			        *cP++ = (char)tmpc;
			    }

			    *cP = '\0';

			    BEGIN BODY;
			    yylval.strP = malloc(strlen(string) + 1);
			    strcpy(yylval.strP, string);
			    return( ASCII );
			}

<DOTEXT>.		{
			int tmpc, rslt;
			int shift;
			char marker;
			char *cP;
			char string[STRBUF];

			    marker = yytext[0];

			    cP = string;
			    shift = 0;

			    while( (tmpc = input()) != marker )
			    {
				if( cP >= (string+STRBUF) )
				{
				    verror("string too long", 0);
				}

				if( (tmpc == EOF) || (tmpc == '\n') )
				{
					verror("unterminated %c in text", marker);
				}

				rslt = asciiToFlexo(tmpc, &shift);

				if( rslt == NONE )
				{
				    vwarn("The character '%c' is not a Flexo character, replaced with space",
				 	tmpc);
				    rslt = FLEX_SPACE;
				}

				if( (rslt == CSHIFT) || (rslt == CUNSHIFT) )
				{
				    *cP++ = rslt;
				    rslt = asciiToFlexo(tmpc, &shift);	// get the real char
				}

			        *cP++ = (char)rslt;
			    }

			    *cP = '\0';

			    BEGIN BODY;
			    yylval.strP = malloc(strlen(string) + 1);
			    strcpy(yylval.strP, string);
			    return( TEXT );
			}

.			{
			    if( yytext[0] != '\r')	// in case we get cr-nl or nl-cr, drop the cr
			    {
			        return(yytext[0]);
			    }
			}

%%
#include "lexfuncs.c"
