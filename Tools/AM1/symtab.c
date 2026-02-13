/* symtab.c - symbol table manipulation routines
 *
 * The symbol table routines use binary trees, see symtab.h for
 * more information. Every symbol node has a long int reserved for
 * use by the caller, symNodeP->value and an unsigned char flag byte,
 * symNodeP->flags.
 * If more space is needed, reserve it in the sym_make() call.
 *
 * The following routines are available:
 *
 * void sym_init(SymNodePP rootPP)
 *	Initializes a new symbol table.
 * void sym_free(SymNodePP rootPP)
 *	Frees up all allocated storage for a tree.
 * SymNodeP sym_make(char *nameP, int extra)
 *	Allocates and initializes a new SymNode, and adds 'extra' bytes
 *	of additional storage to the node. The extra data can be accessed
 *	via the SYM_DATA((SymNodeP)nodeP) macro.
 * SymNodeP sym_find(SymNodePP rootPP, char *nameP)
 *	Searches for a given name in the table. If found, a pointer to
 *	the SymNode is returned, else 0.
 * void sym_add(SymNodePP rootPP, SymNodeP newP)
 *	Adds the new data to the table.
 *	NewP is returned, if a node already exists, it is freed and
 *	replaced by NewP.
 * void sym_delete(SymNodeP newP)
 *	Deletes the SymNode.
 *	The node is not actually deleted, but is marked as deleted.
 *	It will not be found by sym_find(), but if sym_add() finds it,
 *      the new node will replace it and it will become undeleted.
 *
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "symtab.h"

void
sym_init( SymNodePP rootPP )   /* Initialize a root node */
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

    free( rootP->name );
    free( rootP );
}

void
sym_free( SymNodePP rootPP )           /* User perceived free */
{
    if( *rootPP )
    {
        real_free( *rootPP );
        *rootPP = ( SymNodeP ) 0;
    }
}

SymNodeP
sym_make( char *nameP, int extra ) /* Create a new node */
{
    SymNodeP symP;

    if( !(symP = (SymNodeP) calloc(sizeof(SymNode) + extra, sizeof(char))) )
    {
        return( (SymNodeP) 0 ); /* sorry, no memory */
    }

    if( !(symP->name = malloc(strlen(nameP) + 1)) )
    {
        return( (SymNodeP) 0 ); /* sorry, no memory */
    }

    strcpy( symP->name, nameP );
    return( symP );
}

SymNodeP
sym_find( SymNodePP rootPP, char *nameP )  /* Look up a name in the table */
{
    int cmp;                /* result of comparison */
    SymNodeP curP;          /* current node we have */

    curP = *rootPP;

    while( curP )
    {
        if( (cmp = strcmp(nameP, curP->name)) == 0 )
        {
            return( (curP->symflags & SYMIFLAG_DELETED)?0:curP ); /* found it */
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
sym_add( SymNodePP rootPP, SymNodeP newP ) /* Add a node to a table */
{
    int cmp;            /* result of comparison */
    SymNodeP curP;      /* current node we have */
    SymNodePP lastPP;   /* ptr to parent of curP */

    if( !(curP = *rootPP) )
    {
        *rootPP = newP;
        return( newP ); /* first one today */
    }

    lastPP = rootPP;    /* original parent */

    for( ;; )
    {
        if( (cmp = strcmp(newP->name, curP->name)) == 0 )
        {
            newP->leftP = curP->leftP;  /* already here, replace curP */
            newP->rightP = curP->rightP;
            free( curP );
            *lastPP = newP;
            break;
        }
        else if( cmp < 0 )
        {
            if( curP->leftP )
            {
                lastPP = &(curP->leftP);
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
                lastPP = &(curP->rightP);
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

void
sym_delete(SymNodeP symP)
{
    symP->symflags = SYMIFLAG_DELETED;
}
