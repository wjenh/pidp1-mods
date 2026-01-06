/*
 * listSymtab - output a symbol table dump
 *
 * The first line will be the filename.
 * Each subsequent line is one symbol in the form:
 * aaaaaa symbol-name
 * where aaaaaa is the 16-bit address of the symbol's location in memory
 * and nnnn is the line number in the source file where the symbol was resolved.
 *
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"

void printSymbol(FILE *outfP, SymNodeP symP);

// Globals will be in each bank's context.
// The global syms from the last bank switched to added after the last switch are in  globalSymP.
void
listSymtab(FILE *outfP, char* filenameP, BankContextP banksP)
{
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
    if( !symP )
    {
        return;
    }

    // This time we infix walk to get syms in nice alphabetic order
    printSymbol(outfP, symP->leftP);
    fprintf(outfP, "%06o %s\n", (symP->bank << 12) + symP->value, symP->name);
    printSymbol(outfP, symP->rightP);
}
