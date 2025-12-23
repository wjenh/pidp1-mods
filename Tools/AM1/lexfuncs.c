/* lexfuncs.c - lexical support routines, called from lex
 *
 * This file MUST BE included by the lex.
 *
*/
#include <ctype.h>
#include <stdbool.h>

extern int lineno;
extern int localDepth;
extern int maxLocalDepth;                   // the deepest nexting we've seen
extern LocalContextP localContextP;	    // used while a local scope is enabled
extern LocalContextP localStack[];
extern char *filenameP;                     // current input file name
static char filename[1024];

void
reset_line()
{
int i, tmpc;
char *cP, *cP2;
char tmpstr[1024];

    cP = tmpstr;

    while( (tmpc = input()) != '\n' )
    {
        if( tmpc == EOF )
        {
            verror("premature end of file");
        }

        if( cP >= (tmpstr+STRBUF) )
        {
            verror("internal # line too long");
        }

        *cP++ = tmpc;
    }

    *cP = '\0';
    cP2 = tmpstr;

    while( *cP2 && !isdigit(*cP2) )
    {
        ++cP2;
    }

    i = atoi(cP2);
    if( i > 0 )
    {
        lineno = i;     // adjust for yacc nl processing
    }

    while( *cP2 && (*cP2 != '"') )
    {
        ++cP2;
    }

    cP = filename;
    ++cP2;

    while( *cP2 && (*cP2 != '"') )
    {
        *cP++ = *cP2++;
    }

    *cP = '\0';
    filenameP = filename;
}

/*
 * Given a charater that immediately followed a backslash, process to complete the
 * escaped char or chars, return the result or EOF.
 * A 0 return means ignore and continue reading.
 */
int
processEscape(char ch)
{
int number;
int ctr;

    switch( ch )
    {
    case '\n':  	// \<newline> ignored
	ch = '\0';
	break;

    case 'e':
	ch = '\033';	// an escape
	break;

    case 'b':
	ch = '\b';	// a backspace
	break;

    case '^':		// ^char is control char
	ch = input() & 037;
	break;

    case 'f':
	ch= '\f';	// formfeed
	break;

    case 'n':		// a newline
	ch= '\n';
	break;

    case 'r':
	ch = '\r';	// return
	break;

    case 't':		// a tab
	ch = '\t';
	break;

    case '\\':		// an esc'd backslash
	break;

    case '0':		// numeric escape
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

    default:		// just char as is
	break;
    }

    return( ch );
}

// Search for a symbol in the current and any parent local scopes
// If anyContext is true, then a search will be made through ALL
// contexts that have been defined even if there is no current local context.
SymNodeP
resolveLocalSymbol(char *nameP)
{
int i;
SymNodeP symP;

    if( !localContextP )
    {
        return( 0 );            // there is no local scope!
    }

    if( !(symP = sym_find(&(localContextP->symRootP), nameP)) )
    {
        // the 0th entry will always be the 'no local scope' value, 0
        for( int i = localDepth - 1; i > 0; --i )
        {
            if( symP = sym_find(&(localStack[i]->symRootP), nameP) )
            {
                break;
            }
        }
    }

    return( symP );
}
