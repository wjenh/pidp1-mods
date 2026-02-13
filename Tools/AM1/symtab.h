/* symtab.h - symbol table defines
 *
 * This file defines various things used by the symbol table routines.
 * The symbol table is kept as a simple binary tree.
 *
*/
#ifndef SYM_DATA

// The SYM_DATA macro is used to get at the extra data for a symnode
#define SYM_DATA( nodeP, type ) ( (type *) ((nodeP) + 1) )

#define SYMIFLAG_DELETED    1

typedef struct symnode      // a node in a symbol tree
{
    struct symnode *leftP;  // the left and right links
    struct symnode *rightP;
    char *name;
    unsigned int symflags;  // internal flags
    unsigned int flags;     // for the user
    int bank;               // the bank number this was defined in, if needed
    int value;
    int value2;
    void *ptr;
    struct symnode *symP;
} SymNode, *SymNodeP, **SymNodePP;

void sym_init( SymNodePP );
SymNodeP sym_add( SymNodePP, SymNodeP );
SymNodeP sym_find( SymNodePP, char * );
SymNodeP sym_make( char *, int );
void sym_delete( SymNodeP );
void sym_free( SymNodePP );
#endif
