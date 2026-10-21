/* mapcodegen.c - the origin map, <base>.map
 *
 * One record for each line of the listing that holds a word in the listing's order
 * so a reader can pair the records with those lines by counting.
 * A word assembled from code names the file and line of its statement and,
 * when macros created the statement, each expansion from the macro the program used with the
 * body line it came from. 
 * A constant's or a variable's word gives only its address.
 * Finally, the file each symbol in the .sym file is defined in.
 *
 * The format, one record per line, fields separated by one space:
 *     %%am1 origin map%%
 *     V1
 *     <the source file>
 *     F <n> <file>                 a file's number, before its first use
 *     W <address> <overlay> <file n> <line> [<macro> <file n> <line>]...
 *     P <address>                  a constant or variable
 *     S <symbol> <file n>
 * The address is the listing's, bank then address, six octal digits
 * The overlay is its name, or '-' for the root.
 * A file number of -1 means not known.
 * The .lst and .sym files are unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"
#include "preprocess.h"

#define MAP_LABEL "%%am1 origin map%%"
#define MAP_VERSION "V1"

typedef struct mapfile
{
    const char *nameP;
    int number;
    struct mapfile *nextP;
} MapFile;

static FILE *mapfP;                 // NULL when no map is being written
static MapFile *filesP;
static int nFiles;
static const char *curFileP;        // the file the listing is in
static PNodeP curStmtP;             // the statement being listed
static bool inPool;                 // constants or variables are being listed

extern OverlayContextP overlaysP;

// A file's number, giving it one and writing its F record the first time it is seen.
static int
fileNumber(const char *nameP)
{
MapFile *fP;

    if( !nameP )
    {
        return( -1 );
    }

    for( fP = filesP; fP; fP = fP->nextP )
    {
        if( !strcmp(fP->nameP, nameP) )
        {
            return( fP->number );
        }
    }

    if( !(fP = calloc(1, sizeof(MapFile))) || !(fP->nameP = strdup(nameP)) )
    {
        fprintf(stderr, "am1: out of memory writing the origin map\n");
        exit(1);
    }

    fP->number = nFiles++;
    fP->nextP = filesP;
    filesP = fP;
    fprintf(mapfP, "F %d %s\n", fP->number, nameP);
    return( fP->number );
}

// An overlay's name from its number, '-' for the root.
static const char *
overlayName(int number)
{
OverlayContextP ovP;

    for( ovP = overlaysP; ovP; ovP = ovP->nextP )
    {
        if( ovP->number == number )
        {
            return( ovP->nameP );
        }
    }

    return( "-" );
}

// Start the map on fP, for the program in srcNameP.
void
mapOpen(FILE *fP, const char *srcNameP)
{
    mapfP = fP;
    curFileP = srcNameP;
    fprintf(mapfP, "%s\n%s\n%s\n", MAP_LABEL, MAP_VERSION, srcNameP);
}

// End the map: nothing more is written to it.
void
mapClose(void)
{
    mapfP = NULL;
}

// The listing is now in file nameP, after a File line.
void
mapFile(const char *nameP)
{
    curFileP = nameP;
}

// The listing is now listing statement stmtP.
void
mapStatement(PNodeP stmtP)
{
    curStmtP = stmtP;
}

// The listing is listing constants or variables (on true), or has stopped.
void
mapPool(bool on)
{
    inPool = on;
}

// A word's record for a listing line that holds one, nodeP being the node
// the listing printed it from.
void
mapWord(PNodeP nodeP)
{
const PPOrigin *oP;
int i, file;

    if( !mapfP )
    {
        return;
    }

    if( inPool )
    {
        fprintf(mapfP, "P %02o%04o\n", nodeP->bank, nodeP->pc);
        return;
    }

    file = fileNumber(curFileP);
    oP = (curStmtP)?curStmtP->originP:NULL;

    // the chain's files get their numbers, and F records, first
    for( i = 0; oP && (i < oP->count); ++i )
    {
        fileNumber(oP->levelsP[i].fileP);
    }

    fprintf(mapfP, "W %02o%04o %s %d %d", nodeP->bank, nodeP->pc, overlayName(nodeP->overlay),
        file, nodeP->lineNo);

    for( i = 0; oP && (i < oP->count); ++i )
    {
        fprintf(mapfP, " %s %d %d", oP->levelsP[i].macroP, fileNumber(oP->levelsP[i].fileP),
            oP->levelsP[i].line);
    }

    fputc('\n', mapfP);
}

// The S records of one bank's symbol tree, in the .sym's order.
static void
mapSymbol(SymNodeP symP)
{
int file;

    if( !symP )
    {
        return;
    }

    mapSymbol(symP->leftP);
    file = fileNumber(symP->fileP);
    fprintf(mapfP, "S %s %d\n", symP->name, file);
    mapSymbol(symP->rightP);
}

// The S records: each symbol the .sym lists, with the file it is defined in.
void
mapSymbols(BankContextP banksP)
{
BankContextP bankP;

    if( !mapfP )
    {
        return;
    }

    for( bankP = banksP; bankP; bankP = bankP->nextP )
    {
        mapSymbol(bankP->globalSymP);
    }
}
