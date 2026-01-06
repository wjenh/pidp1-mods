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

// If any banks have been used, globals will be in each bank's context.
// The global syms from the last bank switched to added after the last switch are in  globalSymP.
void
listSymtab(FILE *outfP, char* filenameP, BankContextP banksP, SymNodeP globalSymP)
{
    fprintf(outfP, "%s\n", filenameP);
    printSymbol(outfP, globalSymP);
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

    fprintf(outfP, "%06o %s\n", (symP->bank << 12) + symP->value, symP->name);
    printSymbol(outfP, symP->leftP);
    printSymbol(outfP, symP->rightP);
}
