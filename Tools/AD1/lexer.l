%{
/* lexer.l - lex for ad1 */

#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include "ad1.h"
#include "y.tab.h"

#define FAIL 0		/* bison understands this value */
#define STRBUF 1024	/* maximum literal string length */

int parseAndExecute(char *lineP);

extern int base;
extern DispatchP dispatchTable;

extern DispatchP findCommand(DispatchP tbl, char *strP);
extern void *allocMem(int bytes);
extern void freeMem(void);
%}

/* this is to handle special cases for a few arguments, a hack */
%s CMDARG
%s REGARG
%s BASEARG

%%
^[a-z]+			{
			DispatchP dispP;

			    if( (dispP = findCommand(dispatchTable, yytext)) )
			    {
				// We sometimes need to have a command word as an argument or register,
				// or we want to control how integers are converted.
			    	switch( dispP->token )
				{
				case DELETE:
				case ENABLE:
				case DISABLE:
				    BEGIN CMDARG;
				    break;

				case BASE:
				    BEGIN BASEARG;
				    break;

				case BANK:
				case SHOW:
				case SET:
				    BEGIN REGARG;
				    break;
				}

			        return(dispP->token);
			    }
			    else
			    {
				BEGIN INITIAL;
			    	printf("No such command. Type 'help'.\n");
				return(FAIL);
			    }
			}

\&			{ return(AND); }
\|			{ return(OR); }
\^			{ return(XOR); }
\~			{ return(CMPL); }
\+			{ return(PLUS); }
\-			{ return(MINUS); }
\/			{ return(DIV); }
\*			{ return(MUL); }
\<\<			{ return(LSHIFT); }
\>\>			{ return(RSHIFT); }
\(			{ return(LPAREN); }
\)			{ return(RPAREN); }
\.			{ return(DOT); }
\@			{ return(LINEAT); }

\,[0-9]{1,2}		{
			int ival;

			    ival = strtol(yytext+1, 0, 10);
			    if( (ival < 0) || (ival >= MAXBANKS) )
			    {
				printf("A bank number must be between 0-%d decimal.\n", MAXBANKS - 1);
				return(FAIL);
			    }

			    yylval.ival = ival;
			    return(BREF);
			}

ac			{
			    yylval.ival = ACREG;
			    return( REGISTER );
			}

io			{
			    yylval.ival = IOREG;
			    return( REGISTER );
			}

pf			{
			    yylval.ival = PFREG;
			    return( REGISTER );
			}

pf[1-6]			{
			char *cP;

			    cP = strchr(yytext, 'f') + 1;
			    switch( strtol(cP, NIL, 10) )
			    {
			    case 1:
			        yylval.ival = PF1REG;
				break;
			    case 2:
			        yylval.ival = PF2REG;
				break;
			    case 3:
			        yylval.ival = PF3REG;
				break;
			    case 4:
			        yylval.ival = PF4REG;
				break;
			    case 5:
			        yylval.ival = PF5REG;
				break;
			    case 6:
			        yylval.ival = PF6REG;
				break;
			    }

			    return( REGISTER );
			}

tw			{
			    yylval.ival = TWREG;
			    return( REGISTER );
			}

pc			{
			    yylval.ival = PCREG;
			    return( REGISTER );
			}

as			{
			    yylval.ival = ASREG;
			    return( REGISTER );
			}

ma			{
			    yylval.ival = MAREG;
			    return( REGISTER );
			}

mb			{
			    yylval.ival = MBREG;
			    return( REGISTER );
			}

ss			{
			    yylval.ival = SSREG;
			    return( REGISTER );
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
			    if( YYSTATE == BASEARG )
			    {
			    	// This is always a decimal number.
				yylval.ival = atoi(yytext);
				return(INTEGER);
			    }

			    if( (base == 8) && (strchr(yytext, '8') || strchr(yytext, '9')) )
			    {
				printf("decimal number %s, but in octal mode", yytext);
				return(FAIL);
			    }

			    yylval.ival = (int)strtol(yytext,0,(base)?base:0);
			    return(INTEGER);
			}

<CMDARG>[a-z][a-z]*	{
			DispatchP dispP;

			    BEGIN INITIAL;
			    if( (dispP = findCommand(dispatchTable, yytext)) )
			    {
			        return(dispP->token);
			    }
			    else
			    {
			        yylval.strP = (char *)allocMem(strlen(yytext) + 1);
			        strcpy(yylval.strP, yytext);
			        return(SYMBOL);
			    }
			}

[a-zA-Z_][a-zA-Z0-9_]*	{
			DispatchP dispP;

			    if( YYSTATE == REGARG )
			    {
			    	// We can have a register with the same name as a command
				// that looks like a symbol here.
				// We want to use the length matching for a command,
				// so try to look it up, but only if it's longer than one char,
				// because that might be a base specifier.
				BEGIN INITIAL;
				if( (strlen(yytext) > 1) &&(dispP = findCommand(dispatchTable, yytext)) )
				{
				    switch( dispP->token )
				    {
				    case WATCH:
				    case BREAK:
				        yylval.ival = dispP->token;
				        return( REGISTER );
				    }
			        }
			    }

			    BEGIN INITIAL;
			    yylval.strP = (char *)allocMem(strlen(yytext) + 1);
			    strcpy(yylval.strP, yytext);
			    return(SYMBOL);
			}

".*"			{
			int i;
			char *cP;
				
			    i = strlen(yytext);

			    cP = allocMem(i);
			    strcpy(cP, yytext + 1);
			    *(cP + i - 1) = '\0';
			    yylval.strP = cP;
			    return(STRING);
			}

[ ]+			{ return(SEPARATOR); }
%%
int
parseAndExecute(char *lineP)
{
int status;
YY_BUFFER_STATE lexBuf;

    lexBuf = yy_scan_string(lineP);
    status = yyparse();
    yy_delete_buffer(lexBuf);
    freeMem();          // frees up memory we allocated by allocMem()

    return(status);
}
