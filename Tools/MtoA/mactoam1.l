%{
/*
  Lex for the PDP-1 macro to am1 processor
 *
 * Original author: Bill Ezell (wje) pdp1@quackers.net
 * Free to use for any purpose as long as attribution is kept.
 *
 * Version 1.1
 *
 * Revision history:
 *
 * 15-Dec-2025 - initial version
 * 5-Jan-2026 - fix bug terminating a #define macro with no endloc
 *
*/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

#include "symtab.h"

#define eatLine() {while( (ch = input()) != '\n' ) ; unput(ch);}

void yyerror(char *, char *);
void usage();

int argCount;
bool needSemi;
bool hasParams;
char ch;
char *cP;

char str[256];		// scratch string

typedef struct
    {
        int state;
        SymNodeP symP;
    } State, *StateP;

int stackCnt;		// stack 'ptr'
State stack[256];

SymNodeP defsP;		// for tracking define names
SymNodePP defsPP = &defsP;
SymNodeP curSymP;

FILE *errfP;
%}

%s BODY
%x FIRST DEFINE PARAMS ARGS

%%

%{
    BEGIN FIRST;
%}

<FIRST>^.*$		{
			    BEGIN BODY;
			    fprintf(yyout, "%s", yytext);
			}

[\t]*define[ ]+[a-z0-9]+	{
			    // We need to get the name
			    cP = strchr(yytext, ' ');
			    while( isspace(*cP) )
			    {
			    	++cP;
			    }

			    fprintf(yyout, "#define %s", cP);
			    while( ((ch = input()) != '\n') && isspace(ch) )
			    	;

			    unput(ch);

			    // do we have parameters?
			    if( isalpha(ch) || isdigit(ch) )
			    {
			    	// Yes
			        // Remember this for expansion later
			        curSymP = sym_make(cP,0);
			        sym_add(defsPP, curSymP);

				fprintf(yyout, "(");
				hasParams = true;
				BEGIN PARAMS;
			    }
			    else
			    {
				hasParams = false;
			    	BEGIN DEFINE;
			    }
			}

<PARAMS>[a-zA-Z][a-zA-Z0-9]*[,]{0,1}	{
			    // doing an argument list, format for cpp
			    if( yytext[yyleng-1] != ',' )
			    {
				fprintf(yyout, "%s)\\\n", yytext);
				fprintf(yyout, "local;\\\n");
				fprintf(yyout, "%%forcelocal;\\\n");
				needSemi = false;
				// Discard in case there is a comment following
				eatLine();
				BEGIN DEFINE;
			    }
			    else
			    {
			    	fprintf(yyout, "%s", yytext);
			    }

			    // count the params we have
			    curSymP->value++;
			}

<DEFINE>[+-]{0,1}R	{
			    // now is unnecessary
			}

<DEFINE>,		{
			    fputc(',', yyout);
			    needSemi = false;
			}

<DEFINE>\n		{
			    fprintf(yyout, "%s\\\n", needSemi?";":" ");
			    needSemi = true;
			}

<DEFINE>[ \t]terminate	{
			    BEGIN BODY;
			    if( hasParams )
			    {
			        fprintf(yyout, "endloc\n");
			    }
			    else
			    {
			    	fprintf(yyout, "\n");
			    }
			    fprintf(yyout, "// end of define conversion\n");
			}

\!			{
			    fprintf(yyout, " | ");
			}

^\/.*\n			{
			    // comment on a line by itself
			    if( YYSTATE != DEFINE )
			    {
			        fprintf(yyout, "/%s", yytext);
			    }
			}

[\t]+\/.*\n		{
			    // trailing comment
			    if( YYSTATE == ARGS )
			    {
			    	// we didn't finsh them
				while( argCount < curSymP->value )
				{
				    fprintf(yyout,"%s0", (argCount++ > 0)?",":"");
				}

				fputc(')', yyout);
				curSymP = stack[--stackCnt].symP;
				BEGIN stack[stackCnt].state;
			    }

			    cP = strchr(yytext, '/');
			    fprintf(yyout, " /%s", cP);
			}

\(			{
			    fprintf(yyout, "[");
			}

\)			{
			    fprintf(yyout, "]");
			}

^\t+			{
			    	// eat leading tabs
			}

\t+		{
			    fprintf(yyout, ";");
			}

\,		{
			    fprintf(yyout, ", ");
			}

<BODY,DEFINE>[a-z][a-z0-9]*		{
			SymNodeP symP2;

			    // Figure out if this is a define invocation
			    fprintf(yyout, "%s", yytext);
			    if( (symP2 = sym_find(defsPP, yytext)) )
			    {
			         // yes, invoking a define
				 fputc('(', yyout);
				 argCount = 0;

			         stack[stackCnt].state = YYSTATE;
			         stack[stackCnt++].symP = curSymP;
				 curSymP = symP2;
			         BEGIN ARGS;
			    }
			}

<ARGS>[a-zA-Z0-9]+[,]{0,1}	{
			int ch;
			    // doing an argument list, format for cpp
			    ch = yytext[yyleng-1];
			    if( ch == ',' )
			    {
			    	// more args
			    	fprintf(yyout, "%s", yytext);
			    	++argCount;
			    }
			    else
			    {
				fprintf(yyout, "%s", yytext);
				while( ++argCount < curSymP->value )
				{
				    fprintf(yyout,"%s0", (argCount > 1)?",":"");
				}

				fputc(')', yyout);
				curSymP = stack[--stackCnt].symP;
				BEGIN stack[stackCnt].state;
			    }
			}

[ \t]*[a-z0-9]+=	{
			    cP = yytext;
			    while( isspace(*cP) )
			    {
			    	++cP;
			    }

			    strcpy(str, cP);
			    str[strlen(str) - 1] = '\0';	// drop the =
			    fprintf(yyout,"#define %s ", str);
			}

<DEFINE>\n[ \t]*\n		{
			    // eat empty lines
			    unput('\n');
			}

<ARGS>\n		{
			    // be sure we finished the arg list
			    if( argCount < curSymP->value )
			    {
			        while( argCount < curSymP->value )
			        {
			            fprintf(yyout,"%s0", (argCount++ > 0)?",":"");
			        }

			        fprintf(yyout, ")");
			    }

			     unput('\n');
			    curSymP = stack[--stackCnt].symP;
			    BEGIN stack[stackCnt].state;
		        }
%%

void
yyerror(char *cP, char *cP2)
{
    fprintf(errfP,"Fatal error: %s %s\n", cP, cP2?cP2:"");
    exit(1);
}

int
main(int argc, char **argv)
{
int i;
char opt;
char *cP;

    errfP = stderr;
    sym_init(defsPP);

    while( (opt = getopt(argc, argv, "o:")) != -1 )
    {
        switch( opt )
        {
        case 'o':
           if( (yyout = fopen(optarg,"w")) == 0 )
	   {
               fprintf(stderr, "Can't open output file '%s'\n", optarg);
	       return(1);
	   }
           break;

        default: /* '?' */
	   usage();
        }
    }

    if( optind >= argc )
    {
        yylex();			// no input file, uses stdin
    }
    else if( optind == (argc - 1) )
    {
    	if( !(yyin = fopen(argv[optind], "r")) )
	{
	    fprintf(stderr, "m1pp can't open input file '%s'\n", argv[optind]);
	}

	yylex();
    }
    else
    {
    	usage();			// too many files
    }

    if( yyin && (yyin != stdin) )
    {
    	fclose(yyin);
    }

    if( yyout && (yyout != stdout) )
    {
    	fclose(yyout);
    }

    exit( 0 );
}

int
yywrap()
{
    return(1);
}

void
usage()
{
   fprintf(stderr, "Usage: mtoa [-o outfile] [infile]\n");
   fprintf(stderr, "-o outfile write converted source to outfile instaed of stdout\n");
   fprintf(stderr, "If infile is not given, use stdin\n");
   exit(1);
}
