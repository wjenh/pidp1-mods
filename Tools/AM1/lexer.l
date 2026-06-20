%{
/* am1.l - lex for am1 */

#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"
#include "y.tab.h"
#include "type340chars.h"

#define STRBUF 1024		/* maximum literal string length */
#define MAXCCOMMENT 4096	/* maximum c-style comment length */

static bool decimalMode = false;
static bool inConstant = false;
static bool sawSemi = false;
static bool noIncrement = false;

static char *commentP;
static char commentBuf[MAXCCOMMENT];

extern int lineno;
extern char *filenameP;
extern SymNodeP permSymP;
extern SymNodeP globalSymP;

int countNLs(char *strP);
SymNodeP resolveLocalSymbol(char *nameP);

void gdbtarget(void);		// this is a do-nothng just to add as a gdb breakpoint target

extern int processEscape(char);
extern int type340Shift(char);
extern char processType340Escape(char ch);
extern char asciiToFlexo(char, int *);	// in parsefns.c
extern char asciiToType340(char);
extern void reset_line(void);
extern void verror(char *msgP, ...);
extern void vwarn(int errtype, char *msgP, ...);

#define YY_USER_INIT BEGIN FIRST

%}

%s BODY
%x FIRST DOASCII DOTEXT CONST CCOMMENT T340STR

%%

<FIRST>^[^#].*$		{
			    BEGIN BODY;
			    lineno = 1;			// always our first line
			    yylval.strP = malloc(strlen(yytext) + 1);
			    strcpy(yylval.strP, yytext);
			    return(HEADER);
			}

^[ \t]*"/*"		{
			    BEGIN(CCOMMENT);
			    strcpy(commentBuf, yytext);
			    commentP = commentBuf + strlen(commentBuf);
			    *commentP = '\0';
			    yylval.ival = lineno;
			    return(CSSTART);
			}

<CCOMMENT>"*/"		{
			    BEGIN BODY;
			    *commentP = '\0';
			    yylval.strP = malloc(commentP - commentBuf + 1);
			    strcpy(yylval.strP, commentBuf);
			    unput('\n');	// sometimes lookahead screws up start-of-line
			    return(CSCOMMENT);
			}

<CCOMMENT>\n		{
			    ++lineno;

			    if( commentP >= &commentBuf[MAXCCOMMENT] )
			    {
			    	verror("C-style comment exceeds %d characters", MAXCCOMMENT - 1);
			    }

			    *commentP++ = yytext[0];
			}

<CCOMMENT>[^*\n]*	{
			int len;

			    len = strlen(yytext);
			    if( (commentP + len) >= &commentBuf[MAXCCOMMENT] )
			    {
			    	verror("C-style comment exceeds %d characters", MAXCCOMMENT - 1);
			    }

			    strcpy(commentP, yytext);
			    commentP += len;
			}

<CCOMMENT>"*"+[^*/\n]*	{
			int len;

			    len = strlen(yytext);
			    if( (commentP + len) >= &commentBuf[MAXCCOMMENT] )
			    {
			    	verror("C-style comment exceeds %d characters", MAXCCOMMENT - 1);
			    }

			    strcpy(commentP, yytext);
			    commentP += len;
			}

<CCOMMENT><<EOF>>	{
			    verror("unterminated C-style comment, missing */", yytext);
			}

[ \t]*"//".*\n		{
			char *cP;

			    cP = strchr(yytext, '/') + 2;
			    yylval.strP = malloc(strlen(cP) + 1);
			    strcpy(yylval.strP, cP);
			    *strchr(yylval.strP, '\n') = 0;

			    if( inConstant )
			    {
			    	noIncrement = true;	// because we're going to push back a newline
			    }

			    if( !noIncrement && !sawSemi )
			    {
			        ++lineno;
			    }
			    noIncrement = sawSemi = false;

			    if( inConstant )
			    {
			    	unput('\n');
			    }
			    inConstant = false;

			    return(COMMENT);
			}

\%forcelocal		{ return(FORCELOC); }

loc(al)?[ \t]*		{ return(LOCAL); }
addloc(al)?[ \t]*	{ return(ADDLOCAL); }
endloc(al)?[ \t]*	{ return(ENDLOC); }

var[ \t]*		{ return(VAR); }
table[ \t]*		{ return(TABLE); }
bank[ \t]*		{ return(BANK); }

\:			{ return(BREF); }
^[ \t]*start[ \t]*	{ return(START); }
^[ \t]*stop[ \t]*	{ return(STOP); }
^[ \t]*constants[ \t]*	{ return(CONSTANTS); }
^[ \t]*variables[ \t]*	{ return(VARS); }
^[ \t]*import[ \t]*	{ return(IMPORT); }
^[ \t]*export[ \t]*	{ return(EXPORT); }

ascii[ \t]+		{ return(ASCII); }
text[ \t]+		{ BEGIN DOTEXT; }

type340[ \t]+		{
			    BEGIN T340STR;
			    return(TYPE340);
			}

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

				if( *cP == '\\' )		// special directive, red, black, newline
				{
				    ++cP;
				    ++i;

				    if( nchars <= 0 )
				    {
				        verror("invalid \\ in flexo, no directive character");
				    }
				    else if( *cP == 'B' )
				    {
				    	ch = BLACK;
				    }
				    else if( *cP == 'R' )
				    {
				    	ch = RED;
				    }
				    else if( *cP == 'n' )
				    {
				    	ch = FLEX_CR;
				    }
				    else
				    {
				        verror("\\%c in text is not valid", *cP);
				    }
				}
				else
				{
				    ch = asciiToFlexo(*cP, &shift);
				}

				if( ch == NONE )
				{
				    vwarn(WARN_FLEX,
				        "The character '%c' is not a Flexo character, replaced with space", *cP);
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
			    	vwarn(WARN_FLEX,
				    "only the first %d characters used because shift/unshift codes needed",
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
			    	vwarn(WARN_FLEX, "The character '%c' is not a Flexo character, replaced with space",
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
[ ]*%%[ ]*		{ return(MOD); }
[ ]*<<[ ]*		{ return(LSHIFT); }
[ ]*>>[ ]*		{ return(RSHIFT); }
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

0b[0-1]+		{
			    yylval.ival = (int)strtol(yytext+2,0,2);
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

\/[ \t]*[^a-zA-Z0-9\~\%\[\/] {
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
			    return( EMPTYLINE );
			}

[ \t]*(\n|\;[ \t]*)	{
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

				yylval.strP = 0;
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

\"			{
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

			    yylval.strP = (char *)malloc(strlen(string) + 1);
			    strcpy(yylval.strP, string);
			    return( STRING );
			}

<T340STR>\"		{
			int i;
			int tmpc;
			int charCount;
			bool sawEnd;
			bool sawForcedShift;
			char *cP;
			bool inUpper;		// current shift state, false if lower
			char string[STRBUF];

			    cP = string;
			    charCount = 0;
			    sawEnd = false;
			    sawForcedShift = false;
			    inUpper = true;	// we always start in upper for the type 340

			    while( (tmpc = input()) != '"' )
			    {
				if( cP >= (string+STRBUF) )
				{
				    verror("string too long", 0);
				}

				if( (tmpc == EOF) || (tmpc == '\n') )
				{
				    verror("unterminated quote in type340 string");
				}

				if( tmpc == '\\' )
				{
				    tmpc = processType340Escape(input());
				    if( !tmpc )
				    {
				        continue;		// meant a line continuation
				    }

				    if( tmpc == TYPE340UPPER )
				    {
				        inUpper = true;
				        sawForcedShift = true;
				    }
				    else if( tmpc == TYPE340LOWER )
				    {
				        inUpper = false;
				        sawForcedShift = true;
				    }
				    else if( tmpc == TYPE340AUTO )
				    {
				        sawForcedShift = false;
				        continue;
				    }
				    else if( tmpc == TYPE340NOEND )
				    {
				        sawEnd = true;		// fake that we had one
				        continue;
				    }
				    else if( tmpc == TYPE340NL )	// special case, convert
				    {
				        ++charCount;
				        *cP++ = TYPE340CR;
				        tmpc = TYPE340LF;
				    }
				    else if( tmpc == TYPE340END )
				    {
				        sawEnd = true;
				    }
				}
				else
				{
			            i = type340Shift(tmpc);
			            if( !sawForcedShift && (i > 0) && !inUpper )
			            {
				        inUpper = true;
				        *cP++ = TYPE340UPPER;
				        ++charCount;
			            }
			            else if( !sawForcedShift && (i < 0) && inUpper )
			            {
				        inUpper = false;
				        *cP++ = TYPE340LOWER;
				        ++charCount;
			            }

			            tmpc = asciiToType340(tmpc);
				}

			        ++charCount;
			        *cP++ = (char)tmpc;
			    }

			    if( !sawEnd )
			    {
			        *cP++ = TYPE340END;
			        ++charCount;
			    }

			    // We reuse the Flex struct, this is also a counted string
			    BEGIN BODY;
			    yylval.flexText.nchars = charCount;
			    yylval.flexText.bufP  = (char *)malloc(charCount);
			    memcpy(yylval.flexText.bufP, string, charCount);

			    return( T340STRING );
			}

\<[a-zA-Z\./_0-9]+\>	{
			    yylval.strP = malloc(strlen(yytext) + 1);
			    strcpy(yylval.strP, yytext);
			    return(LIBFILE);
			}
			
<DOTEXT>.		{
			int tmpc, rslt;
			int count;
			int shift;
			char marker;
			char *cP;
			char string[STRBUF];

			    marker = yytext[0];

			    cP = string;
			    count = shift = 0;

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

				if( tmpc == '\\' )		// special directive, red or black
				{
				    if( (tmpc = input()) == marker )
				    {
				        verror("invalid \\ in text, no directive character");
				    }
				    switch(tmpc)
				    {
				    case 'B':
				    	rslt = BLACK;
					break;

				    case 'R':
				    	rslt = RED;
					break;

				    case 'n':
				    	rslt = FLEX_CR;
					break;

				    default:
				        verror("\\%c in text is not valid", tmpc);
				    }
				}
				else
				{
				    rslt = asciiToFlexo(tmpc, &shift);
				}

				if( rslt == NONE )
				{
				    vwarn(WARN_FLEX,
				        "The character '%c' is not a Flexo character, replaced with space", tmpc);
				    rslt = FLEX_SPACE;
				}

				if( (rslt == CSHIFT) || (rslt == CUNSHIFT) )
				{
				    *cP++ = rslt;
				    ++count;
				    rslt = asciiToFlexo(tmpc, &shift);	// get the real char
				}

			        *cP++ = (char)rslt;
				++count;
			    }

			    BEGIN BODY;
			    yylval.flexText.nchars = count;
			    yylval.flexText.bufP  = (char *)malloc(count);
			    memcpy(yylval.flexText.bufP, string, count);
			    return( TEXT );
			}

.			{
			    if( yytext[0] != '\r')	// in case we get cr-nl or nl-cr, drop the cr
			    {
			        return(yytext[0]);
			    }
			}

%%
void
gdbtarget()
{
    return;
}

#include "lexfuncs.c"
