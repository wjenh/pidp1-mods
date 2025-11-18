%{
/*AB parser.l - lex for the PDP-1 macro preprocessor
AE*/


#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

#define MAXDEPTH 128	// include file nesting, should be enough for anyone
#define STRBUF 1024	// maximum literal string length
#define DEFAULT_SYSDIR "/opt/pidp1/MacroIncludes/"

void checkFirst(char *);
void checkComplete();
void printLoc(char *, int);
int processEscape(char);
void setIndent(char *);
void doIndent();
void processInclude(char *);
void yyerror(char *, char *);
void usage();

int indent;
int ival;
bool decimal = false;
bool saw_constant = false;
bool saw_extra = false;
bool saw_start = false;
char ch;

FILE *errfP;

YY_BUFFER_STATE inclStack[MAXDEPTH];
int inclStackIdx = 0;
char sysIncludeDir[512];

%}

%x FIRST ASCII DONE

%%

%{
    BEGIN FIRST;
%}

<FIRST>^.*$		{
			    BEGIN INITIAL;
			    checkFirst(yytext);
			    fprintf(yyout, "%s\n", yytext);
			}

^[ \t]*			{
			    setIndent(yytext);
			    fprintf(yyout, "%s", yytext);
			}

^#include[ \t]+\<.+\>.*$	{
			    processInclude(yytext);
			}

^#include[ \t]+\".+\".*$	{
			    processInclude(yytext);
			}

decimal			{
			    decimal = true;
			    fprintf(yyout, "%s", yytext);
			}

octal			{
			    decimal = false;
			    fprintf(yyout, "%s", yytext);
			}

0x[0-9A-Fa-f]+		{
			    ival = strtol(yytext,0,16);
			    fprintf(yyout, decimal?"%d":"%o", ival);
			}

ascii[ \t]+		{
			    BEGIN ASCII;
			    fprintf(yyout, "/ ascii\n");
	    		}

^[a-zA-z0-9\+\-]+\/[ \t]+ {
			    printLoc(yytext, yyleng);
			}

[a-zA-Z0-9]\t[^ \/]	{
			    ch = (yytext[yyleng-1]);
			    yytext[yyleng-2] = '\0';
			    yyleng -= 2;
			    unput(ch);
			    fprintf(yyout, "%s\n", yytext);
			    doIndent();
			}

[^ \t][\t]+\/		{
			    fprintf(yyout, "%c\n/", yytext[0]);
			}

\(			{
			    saw_constant = true;
			    fputc('(', yyout);
			}

constants		{
			    saw_constant = false;
			    fprintf(yyout, "%s", yytext);
			}

start			{
			    BEGIN DONE;
			    saw_start = true;
			    fprintf(yyout, "%s", yytext);
			}

<DONE>^.*$		{
			    saw_extra = true;
			    fprintf(yyout, "%s\n", yytext);
			}

\'			{
			int ch;

			    if( (ch = input()) == '\\' )
			    {
				ch = input();
			 	ch = processEscape(ch);
			    }

			    if( ch == EOF )
			    {
				yyerror("unterminated single-quote", 0);
			    }
			    else if( ch )
			    {
				fprintf(yyout, decimal?"%d":"%o", ch);
			    }
			    
			    input();	// get rid of trailing quote
			}

<ASCII>\"		{
			int tmpc;
			char *cP;
			char string[STRBUF];

			    BEGIN INITIAL;
			    cP = string;

			    while( (tmpc = input()) != '"' )
			    {
				if( cP >= (string+STRBUF) )
				{
				    yyerror("string too long", 0);
				}

				if( tmpc == EOF )
				{
				}

				if( tmpc == '\\' )
				{
				    if( tmpc = processEscape(tmpc) && tmpc )
				    {
				        *cP++ = (char)tmpc;
				    }
				}
				else if( tmpc )
				{
				    *cP++ = (char)tmpc;
				}
			    }

			    *cP = '\0';

			    doIndent();

			    for( tmpc = 0, cP = string; *cP != 0; tmpc ^= 1 )
			    {
				fprintf(yyout, decimal?"%03d":"%03o", *cP++);
				if( tmpc )
				{
				    fprintf(yyout, "\n");
				    doIndent();
				}
			    }

			    if( tmpc )
			    {
			        fprintf(yyout, "000\n");
			    }
			    else
			    {
			        fprintf(yyout, "000000\n");
			    }
			}

%%

/*
 * Given a charater that immediately followed a backslash, process to complete the
 * escaped char or chars, return the result or EOF.
 * A 0 return means ignore and continue reading.
 */
int
processEscape( char ch)
{
int number;
int ctr;

    switch( ch )
    {
    case '\n':	/* \<newline> ignored */
	ch = '\0';
	break;

    case 'e':
	ch = '\033';	/* an escape */
	break;

    case 'b':
	ch = '\b';	/* a backspace */
	break;

    case '^':		/* ^char is control char */
	ch = input() & 037;
	break;

    case 'f':
	ch= '\f';	/* formfeed */
	break;

    case 'n':		/* a newline */
	ch= '\n';
	break;

    case 'r':
	ch = '\r';	/* return */
	break;

    case 't':		/* a tab */
	ch = '\t';
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
	number = ch - '0';
	ctr = 1;

	ch = input();

	while( isdigit(ch) && (ctr < 3) )
	{
	    number = number*8 + (ch - '0');
	    ++ctr;
	    ch = input();
	}

	unput(ch);
	ch = number;
	break;

    default:		/* just char as is */
	break;
    }

    return( ch );
}

/*
 * Scan the passed string to guess if it is code instead of a program label.
 * If it starts with digits followed by a slash or letters followed by a comma,
 * we assume it's code and give a warning.
 */
void
checkFirst(char *cP)
{
    if( isdigit(*cP) )
    {
        while( *cP && isdigit(*cP) )
        {
    	    ++cP;
        }

	if( *cP == '/' )
	{
	    fprintf(errfP, "Warning: the first line looks like a location, not a program label.\n");
	}
    }
    else
    {
        while( *cP && isalpha(*cP) )
        {
	    ++cP;
        }

	if( *cP == ',' )
	{
	    fprintf(errfP, "Warning: the first line looks like a storage label, not a program label.\n");
	}
    }
}

/*
 * This splits a line of the form 'xxx/ yyy' into two lines,
 * used to fix a location followed by text.
 */
void
printLoc(char *strP, int len)
{
char *cP;

    for( cP = strP + len - 1; cP >= strP; --cP )
    {
        if( (*cP != ' ') && (*cP != '\t' ) )
        {
	    break;
        }
    }

    fprintf(yyout, "%s\n    ", strP);
}

/*
 * Scan the passed line, counting spaces and tabs,
 * and compute the indent.
 */
void
setIndent(char *cP)
{
int i;

    for( indent = 0; *cP; ++cP  )
    {
    	if( *cP == ' ' )
	{
	    ++indent;
	}
	else
	{
	    i = (indent % 8);
	    indent += i;
	    if( i == 0 )
	    {
	    	indent += 8;
	    }
	}
    }
}

/*
 * Indent by as many spaces as the current indent level.
 */
void doIndent()
{
int i;

    for( i = 0; i++ < indent; )
    {
    	fputc(' ', yyout);
    }
}

/*
 * After the code has been processed, make sure there is a start.
 * If any constants have been seen, be sure a constants statement followed.
 */
void
checkComplete()
{
    if( !saw_start )
    {
    	fprintf(errfP,"Missing a start statement!\n");
    }
    
    if( saw_extra )
    {
    	fprintf(errfP,"The start statement is not the last line, the assembler will ignore the rest!\n");
    }

    if( saw_constant )
    {
    	fprintf(errfP,"Missing a constants statement!\n");
    }
}

/*
 * The entire line 'include....' is passed.
 * Figure out the file, open it.
 * If successful, the current yyin is pushed onto the file stack
 * and the named file becomes the new yyin.
 * When eof occurs, the stack is popped and the previous yyin restored.
 */
void
processInclude(char *cP)
{
bool sysInc = false;
char *cP2;
char *nameP;
char filename[512];
FILE *fP;

    // The lexer has guaranteed we will have matched "" or <>
    if( (cP2 = strchr(cP,'"')) )
    {
    	nameP = cP2 + 1;
	cP2 = strchr(nameP,'"');
	*cP2++ = 0;
    }
    else
    {
	sysInc = true;
    	nameP = strchr(cP,'<') + 1;
	cP2 = strchr(nameP,'>');
	*cP2++ = 0;
    }

    if( *cP2 )			// has trailing text, make it a comment
    {
    	fprintf(yyout,"/%s\n", cP2);
    }

    sprintf(filename,"%s%s%s",sysInc?sysIncludeDir:"", sysInc?"/":"", nameP);

    if( !(fP = fopen(nameP,"r")) )
    {
    	yyerror("can't open include file ", nameP);
    }
    
    inclStack[inclStackIdx++] = YY_CURRENT_BUFFER;
    yy_switch_to_buffer( yy_create_buffer(fP, YY_BUF_SIZE) );
}

void
yyerror(char *cP, char *cP2)
{
    fprintf(errfP,"Fatal error: %s%s\n", cP, cP2?cP2:"");
    exit(1);
}

int
yywrap()
{
    if( inclStackIdx > 0 )
    {
        fclose(yyin);
        yy_delete_buffer( YY_CURRENT_BUFFER );
        yy_switch_to_buffer(inclStack[--inclStackIdx]);
	return(0);
    }
    else
    {
        checkComplete();
        return(1);
    }
}

int
main(int argc, char **argv)
{
int i;
char opt;

    errfP = stderr;
    strcpy(sysIncludeDir, DEFAULT_SYSDIR);

    while( (opt = getopt(argc, argv, "S:o:e:")) != -1 )
    {
        switch( opt )
        {
        case 'S':
           strcpy(sysIncludeDir, optarg);
           break;

        case 'o':
           if( (yyout = fopen(optarg,"w")) == 0 )
	   {
               fprintf(stderr, "Can't open output file '%s'\n", optarg);
	       return(1);
	   }
           break;

        case 'e':
           if( (errfP = fopen(optarg,"w")) == 0 )
	   {
               fprintf(stderr, "m1pp can't open error file '%s'\n", optarg);
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

void
usage()
{
   fprintf(stderr, "Usage: m1pp [-o outfile] [-S sysincdir] [-e errfile] [infile]\n");
   exit(1);
}
