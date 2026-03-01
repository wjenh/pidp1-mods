/*
 * listSymtab - output a symbol table dump
 *
 * The first line is a symtab identification string.
 * The second line is a symtab file version.
 * The third line will be the filename.
 * Each subsequent line is one symbol in the form:
 * aaaaaa t symbol-name nnnn
 * where aaaaaa is the 16-bit address of the symbol's location in memory.
 * t is one of G, X, I for global, exported, or imported.
 * and nnnn is the line number in the source file where the symbol was resolved.
 *
 * Note that the same symbol name can occur in different banks.
 * The full address is used to resolve which symbol is where.
 *
 * wje 24-Feb-26 Add line number and versioning to symbol file, used by ad1.
 *
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"

#define SYMTAB_LABEL  "%%am1 symtab file%%"
#define SYMTAB_VERSION  "V2"

void printSymbol(FILE *outfP, SymNodeP symP);

// Globals will be in each bank's context.
// The global syms from the last bank switched to added after the last switch are in  globalSymP.
void
listSymtab(FILE *outfP, char* filenameP, BankContextP banksP)
{
    fprintf(outfP, "%s\n", SYMTAB_LABEL);
    fprintf(outfP, "%s\n", SYMTAB_VERSION);
    fprintf(outfP, "%s\n", filenameP);
    if( banksP )
    {
        for( BankContextP bankP = banksP; bankP; bankP = bankP->nextP )
        {
            printSymbol(outfP, bankP->globalSymP);
        }
    }
}

void
printSymbol(FILE *outfP, SymNodeP symP)
{
char *typeP;

    if( !symP )
    {
        return;
    }

    // This time we infix walk to get syms in nice alphabetic order
    printSymbol(outfP, symP->leftP);

    if( symP->flags & SYMF_IMPORTED )
    {
        typeP = "I";
    }
    else if( symP->flags & SYMF_EXPORTED )
    {
        typeP = "X";
    }
    else
    {
        typeP = "G";
    }

    fprintf(outfP, "%06o %s %s %d\n", (symP->bank << 12) + symP->value, typeP, symP->name, symP->lineno);

    printSymbol(outfP, symP->rightP);
}
