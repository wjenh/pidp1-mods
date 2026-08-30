/* symtab.c - symbol table manipulation routines
 *
 * The symbol table routines use binary trees, see symtab.h for
 * more information. Every symbol node has a long int reserved for
 * use by the caller, symNodeP->ival, and a (void *)pointer, symNodeP->ptr.
 *
 * The following routines are available:
 *
 * void symInit(SymNodePP rootPP)
 *	Initializes a new symbol table.
 * void symFree(SymNodePP rootPP)
 *	Frees up all allocated storage for a tree.
 * SymNodeP symMake(char *nameP)
 *	Allocates and initializes a new SymNode.
 *  The passed nameP is kept as the name, be sure it's
 *  an allocated string, and not freed later.
 * SymNodeP symFind(SymNodePP rootPP, char *nameP)
 *	Searches for a given name in the table. If found, a pointer to
 *	the SymNode is returned, else 0.
 * void symAdd(SymNodePP rootPP, SymNodeP newP)
 *	Adds the new data to the table.
 *	NewP is returned, if a node already exists, null is returned.
 *
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "symtab.h"

void
symInit( SymNodePP rootPP )   /* Initialize a root node */
{
    *rootPP = ( SymNodeP ) 0;   /* real tough */
}

static void
real_free( SymNodeP rootP )     /* Free up a table */
{
    if( rootP->rightP )
    {
        real_free( rootP->rightP );
    }

    if( rootP->leftP )
    {
        real_free( rootP->leftP );
    }

    free( rootP->nameP );
    free( rootP );
}

void
symFree( SymNodePP rootPP )           /* User perceived free */
{
    if( *rootPP )
    {
        real_free( *rootPP );
        *rootPP = ( SymNodeP ) 0;
    }
}

SymNodeP
symMake(char *nameP)           /* Create a new node */
{
SymNodeP symP;

    if( !(symP = (SymNodeP) calloc(sizeof(SymNode), sizeof(char))) )
    {
        return( (SymNodeP)0 ); /* sorry, no memory */
    }

     symP->nameP = nameP;
    return( symP );
}

SymNodeP
symFind(SymNodePP rootPP, char *nameP)  /* Look up a name in the table */
{
int cmp;                /* result of comparison */
SymNodeP curP;          /* current node we have */

    curP = *rootPP;

    while( curP )
    {
        if( (cmp = strcmp(nameP, curP->nameP)) == 0 )
        {
            return( curP ); /* found it */
        }
        else if( cmp < 0 )
        {
            curP = curP->leftP;
        }
        else
        {
            curP = curP->rightP;
        }
    }

    return(0); /* not found */
}

SymNodeP
symAdd(SymNodePP rootPP, SymNodeP newP) /* Add a node to a table */
{
int cmp;            /* result of comparison */
SymNodeP curP;      /* current node we have */

    if( !(curP = *rootPP) )
    {
        *rootPP = newP;
        return( newP ); /* first one today */
    }

    for( ;; )
    {
        if( (cmp = strcmp(newP->nameP, curP->nameP)) == 0 )
        {
            return((SymNodeP)0);        // already exists, fail
        }
        else if( cmp < 0 )
        {
            if( curP->leftP )
            {
                curP = curP->leftP;     /* keep looking */
            }
            else                        /* it goes here */
            {
                curP->leftP = newP;
                break;
            }
        }
        else
        {
            if( curP->rightP )
            {
                curP = curP->rightP;    /* keep looking */
            }
            else                        /* it goes here */
            {
                curP->rightP = newP;
                break;
            }
        }
    }

    return( newP );
}
