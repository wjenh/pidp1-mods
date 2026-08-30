/* symtab.h - symbol table defines
 *
 * This file defines various things used by the symbol table routines.
 * The symbol table is kept as a simple binary tree.
 *
*/
#ifndef SYMTAB_H
#define SYMTAB_H

typedef struct symnode      // a node in a symbol tree
{
    struct symnode *leftP;  // the left and right links
    struct symnode *rightP;
    char *nameP;
    unsigned int flags;     // for the user
    int ival;
    void *ptr;
} SymNode, *SymNodeP, **SymNodePP;

void symInit(SymNodePP rootPP);
SymNodeP symAdd(SymNodePP rootPP, SymNodeP symP);
SymNodeP symFind(SymNodePP, char *nameP);
SymNodeP symMake(char *nameP);
void symFree(SymNodePP rootPP);
#endif
