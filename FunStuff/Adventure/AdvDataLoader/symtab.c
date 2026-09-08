/* symtab.c - symbol table manipulation routines
 *
 * The symbol table is a simple unbalanced binary tree keyed on the
 * symbol name, see symtab.h for the node layout. Every node carries two
 * fields reserved for the caller: an int, symNodeP->ival, and a
 * (void *) pointer, symNodeP->ptr.
 *
 * A node's name string is owned by the tree once the node is added:
 * symMake keeps the pointer it is handed, and symFree releases it.
 *
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "symtab.h"

// Initializes a new, empty symbol table rooted at *rootPP.
void
symInit(SymNodePP rootPP)
{
    *rootPP = (SymNodeP)0;
}

// Recursively frees a subtree, both the nodes and their name strings.
// Internal to this file; callers use symFree.
static void
realFree(SymNodeP rootP)
{
    if( rootP->rightP )
    {
        realFree(rootP->rightP);
    }

    if( rootP->leftP )
    {
        realFree(rootP->leftP);
    }

    free(rootP->nameP);
    free(rootP);
}

// Frees every node in the table and resets the root to empty.
// Safe to call on an already-empty table.
void
symFree(SymNodePP rootPP)
{
    if( *rootPP )
    {
        realFree(*rootPP);
        *rootPP = (SymNodeP)0;
    }
}

// Allocates and zeroes a new node, keeping nameP as its name.
// The string must be allocated and must not be freed by the caller;
// symFree releases it with the node.
// Returns the new node, or 0 if there was no memory.
SymNodeP
symMake(char *nameP)
{
SymNodeP symP;

    if( !(symP = (SymNodeP)calloc(sizeof(SymNode), sizeof(char))) )
    {
        return((SymNodeP)0);
    }

    symP->nameP = nameP;
    return(symP);
}

// Looks up a name in the table.
// Returns the matching node, or 0 if the name is not present.
SymNodeP
symFind(SymNodePP rootPP, char *nameP)
{
int cmp;                /* result of comparison */
SymNodeP curP;          /* current node we have */

    curP = *rootPP;

    while( curP )
    {
        if( (cmp = strcmp(nameP, curP->nameP)) == 0 )
        {
            return(curP);
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

    return((SymNodeP)0);
}

// Adds a node to the table, keyed on newP->nameP.
// Returns newP on success, or 0 if a node with that name already
// exists, in which case the table is left unchanged and newP is not
// linked in (the caller still owns it).
SymNodeP
symAdd(SymNodePP rootPP, SymNodeP newP)
{
int cmp;            /* result of comparison */
SymNodeP curP;      /* current node we have */

    if( !(curP = *rootPP) )
    {
        *rootPP = newP;
        return(newP); /* first one today */
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

    return(newP);
}
