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
#define MAXKNOWN 128	// the maximum number of included files we remember

void checkFirst(char *);
bool checkFlexo(char *);
void checkComplete();
void printLoc(char *, int);
int processEscape(char);
void setIndent(char *);
void doIndent();
void processInclude(char *, bool);
bool addInc(char *);
bool findInc(char *);
void yyerror(char *, char *);
void usage();

int indent;
int ival;
bool decimal = false;
bool saw_constant = false;
bool saw_extra = false;
bool saw_start = false;
char ch;
char *cP;

FILE *errfP;

char sysIncludeDir[128];

char knownIncs[MAXKNOWN][128];
int knownCount;

%}

%s BODY
%x FIRST ASCII DONE

%%

%{
    BEGIN FIRST;
%}

<FIRST>^.*$		{
			    BEGIN BODY;
			    checkFirst(yytext);
			    fprintf(yyout, "%s", yytext);
			}

^[ \t]*			{
			    setIndent(yytext);
			    fprintf(yyout, "%s", yytext);
			}

^#include[ \t]+\<.+\>.*$	{
			    processInclude(yytext, false);
			}

^#include[ \t]+\".+\".*$	{
			    processInclude(yytext, false);
			}

^#includeif[ \t]+\<.+\>.*$	{
			    processInclude(yytext, true);
			}

^#includeif[ \t]+\".+\".*$	{
			    processInclude(yytext, true);
			}

decimal			{
			    decimal = true;
			    fprintf(yyout, "%s", yytext);
			}

octal			{
			    decimal = false;
			    fprintf(yyout, "%s", yytext);
			}

flexo[ ]+[^\n]{1,3}	{
			    if( checkFlexo(yytext) )
			    {
			        fprintf(errfP, "Warning: shifted characters in %s\n", yytext);
			    }

			    fprintf(yyout, "%s", yytext);
			}

[+-][ \t]+		{
			    fprintf(errfP, "Illegal, correcting + or - followed by spaces or tabs in '%s'\n", yytext);
			    fputc(yytext[0], yyout);
			}

[ \t]+[+-][ \t]+	{
			    fprintf(errfP, "Illegal, correcting + or - surrounded by spaces or tabs in '%s'\n", yytext);
			    cP = yytext;
			    while( isspace(*cP) )
			    {
			    	++cP;
			    }

			    fputc(*cP, yyout);
			}

0x[0-9A-Fa-f]+		{
			    ival = strtol(yytext,0,16);
			    fprintf(yyout, decimal?"%d":"%o", ival);
			}

0d[0-9]+		{
			    ival = strtol(yytext+2,0,10);
			    fprintf(yyout, decimal?"%d":"%o", ival);
			}

ascii[ \t]+		{
			    BEGIN ASCII;
			    fprintf(yyout, "/ ascii\n");
	    		}

^[a-zA-z0-9\+\-]+\/[ \t]+ {
			    printLoc(yytext, yyleng);
			}

[^\t$][ ]+\/			{
			    fputc( yytext[0], yyout);
			    fputc('\n', yyout);
			    fputc('/', yyout);
			}

^\/.*$			{
			    fprintf(yyout, "%s", yytext);
			}

\(			{
			    saw_constant = true;
			    fputc('(', yyout);
			}

constants		{
			    saw_constant = false;
			    fprintf(yyout, "%s", yytext);
			}

^[ \t]*start.*$			{
			    BEGIN DONE;
			    saw_start = true;
			    fprintf(yyout, "%s", yytext);
			}

<DONE>^.*$		{
			    saw_extra = true;
			    fprintf(yyout, "%s", yytext);
			}

<BODY>\'\\?.\'		{
			int ch;

			    if( yytext[1] == '\\' )
			    {
			        ch = yytext[2];
			        ch = processEscape(ch);
			    }
			    else
			    {
			    	ch = yytext[1];
			    }

		            if( ch == EOF )
		            {
			        yyerror("unterminated single-quote", 0);
		            }
		            else if( ch )
		            {
			        fprintf(yyout, decimal?"%d":"%o", ch);
		            }
			}

<ASCII>\"		{
			int tmpc;
			char *cP;
			char string[STRBUF];

			    cP = string;

			    while( (tmpc = input()) != '"' )
			    {
				if( cP >= (string+STRBUF) )
				{
				    yyerror("string too long", 0);
				}

				if( tmpc == EOF )
				{
					yyerror("unterminated quote in ascii string", 0);
				}

				if( tmpc == '\\' )
				{
				    tmpc = processEscape(input());
				}

				if( tmpc == EOF )
				{
					yyerror("unterminated escape in ascii string", 0);
				}
				else
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

			    BEGIN BODY;
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
 * Scan the passed string which will be of the form 'flexo x', 'xy', or 'xyz' to
 * see if there is a flexo shifted character.
 * If so, return true, else false.
 * Uses the flex/ascii mapping:
 *
 */
bool
checkFlexo(char *cP)
{
int i;

    for( i = 0, cP += strlen(cP) - 1; i++ < 3; --cP )
    {
    	if( isspace(*cP) )
	{
	    return( false );		// is ok
	}

	if( isupper(*cP) || strchr("#!&^\\*", *cP) )
	{
	    return(true);		// shifted
	}
    }

    return( false );			// is ok
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
void
doIndent()
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
 * Figure out the file.
 * If ifnew is true, only load the file if it hasn't already been loaded, skip otherwise.
 * if isnew is false or the file hasn't been loaded, load it.
 * If successful, the current yyin is pushed onto the file stack
 * and the named file becomes the new yyin.
 * When eof occurs, the stack is popped and the previous yyin restored.
 * 
 */
void
processInclude(char *lineP, bool ifNew)
{
bool sysInc = false;
char *cP2, *cP3;
char *nameP;
char *filenameP;
char filename[512];
char basename[64];
FILE *fP;

    // The lexer has guaranteed we will have matched "" or <>.
    // We keep the first char of filename as " or < for searching.
    if( (nameP = strchr(lineP,'"')) )
    {
	cP2 = strchr(nameP + 1,'"');
    	*stpncpy(filename, nameP + 1, cP2 - nameP - 1) = 0;
	nameP = filename;
	filenameP = nameP;
    }
    else
    {
	sysInc = true;
	basename[0] = '<';
	cP3 = basename + 1;

    	nameP = strchr(lineP,'<') + 1;
	cP2 = strchr(nameP,'>');
    	*stpncpy(cP3, nameP, cP2 - nameP) = 0;
	nameP = basename;
	sprintf(filename, "%s/%s", sysIncludeDir, nameP + 1);
	filenameP = filename;
    }

    ++cP2;	// now point to the char after the closing delimiter

    if( ifNew && findInc(basename) )
    {
    	return;			// do nothing
    }

    if( *cP2 )			// has trailing text, make it a comment
    {
    	fprintf(yyout,"\n/%s", cP2);
    }

    if( !(fP = fopen(filenameP,"r")) )
    {
    	yyerror("can't open include file", filenameP);
    }
    
    addInc(basename);	// keep the opening < or " to differentiate include source
    yypush_buffer_state( yy_create_buffer(fP, YY_BUF_SIZE) );
}

/*
 * Search the list of included files to see if it has already been loaded.
 * If so, return true else false.
 */
bool
findInc(char *nameP)
{
int i;

    for( i = 0; i < knownCount; ++i )
    {
    	if( !strcmp(nameP, knownIncs[i]) )
	{
	    return(true);
	}
    }

    return(false);
}

/*
 * Add an include name to the known list.
 * If the list of known names is exhausted, return false else true.
 */
bool
addInc(char *nameP)
{
    if( knownCount >= MAXKNOWN )
    {
    	return(false);
    }

    strcpy( knownIncs[knownCount++], nameP );
    return(true);
}

void
yyerror(char *cP, char *cP2)
{
    fprintf(errfP,"Fatal error: %s %s\n", cP, cP2?cP2:"");
    exit(1);
}

int
yywrap()
{
FILE *fP;

    fP = yyin;			// in case we pop state
    yypop_buffer_state();

    if( YY_CURRENT_BUFFER )
    {
        fclose(fP);
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
char *cP;

    errfP = stderr;

    if( (cP = getenv("M1SYSINC")) )
    {
    	strcpy(sysIncludeDir, cP);
    }
    else
    {
        strcpy(sysIncludeDir, DEFAULT_SYSDIR);
    }

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
