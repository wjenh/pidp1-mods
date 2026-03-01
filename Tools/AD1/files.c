// Process various combinations of filenames and try to set up the list/source and symbol data.

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define NIL (void *)0
#define NUL '\0'

char *am1NameP;
char *lstNameP;
char *symNameP;

// Try various ways to open the necessary files.
// A filename could have been given as:
// filename
// filename.am1
// filename.sym
// filename.lst
// Infer the ohter names from whatever is given.
// The names will be malloced, so clean up if needed.
// Returns true for all cases other than a nil or empty nameP.
bool
resolveFiles(char *nameP)
{
char *extP;
char *cP;
char am1str[256];
char lststr[256];
char symstr[256];

    if( !nameP || (*nameP == NUL) )
    {
        return(false);
    }

    if( !(extP = strchr(nameP, '.')) )
    {
        extP = nameP + strlen(nameP);
        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".am1") )
    {
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".lst") )
    {
        strcpy(lststr, nameP);

        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".sym");
        strcpy(symstr, nameP);
    }
    else if( !strcmp(extP, ".sym") )
    {
        strcpy(symstr, nameP);

        strcpy(extP, ".am1");
        strcpy(am1str, nameP);

        strcpy(extP, ".lst");
        strcpy(lststr, nameP);
    }

    am1NameP = (char *)malloc(strlen(am1str)+1);
    strcpy(am1NameP, am1str);
    symNameP = (char *)malloc(strlen(symstr)+1);
    strcpy(symNameP, symstr);
    lstNameP = (char *)malloc(strlen(lststr)+1);
    strcpy(lstNameP, lststr);

    return( true );
}

// Free the space allocated for the file names,
// reset the pointers to nil.
void
clearFiles()
{
    if( am1NameP )
    {
        free( am1NameP );
        am1NameP = NIL;
    }

    if( lstNameP )
    {
        free( lstNameP );
        lstNameP = NIL;
    }

    if( symNameP )
    {
        free( symNameP );
        symNameP = NIL;
    }
}
