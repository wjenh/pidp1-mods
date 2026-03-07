// Functions to provide source-level viewing of a file.
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "ad1.h"

#define MAXLINES    4000
#define MEMBANKS    16
#define MEMSIZE     4096

static int numLines;
static int curLine;
static bool memMapped;
static FILE *fP;

// How we map line numbers into file locations
static long lineMap[MAXLINES];
// And addresses to line numbers
// Since multiple source lines can use the same address, we keep them all in a list
static MapEntryPP memMap[MEMBANKS];  // we don't allocate until we need to

bool isFileMapped(void);
bool isMemMapped(void);
bool loadFileMap(bool isLst, char *filenameP);
void closeListFile(void);
MapEntryP getLinesFromAddress(int addr);
int getLineFromAddress(int addr);
int getLineCount(void);
int getCurrentLineNumber(void);
void printLines(MapEntryP linesP);
bool printLine(int line);
bool printNextLine(void);

// Have we mapped a file and is it still open?
bool
isFileMapped()
{
    return((fP)?true:false);
}

// Is memory also mapped?
bool
isMemMapped()
{
    return(memMapped);
}

// Try to load line mappings from a source file.
// If it's a .lst file, we can construct the memory->line mapping also.
// Otherwise, all we have is the line number mapping.
bool
loadFileMap(bool fromLst, char *filenameP)
{
int i;
int bank;
int address;
long offset;
MapEntryP memP, newP;
MapEntryPP bankP;
char *cP, *cP2;
char tmpbuf[1024];

    if( !(fP = fopen(filenameP, "r")) )
    {
        return(false);
    }
    
    // read through the file keeping track of the offset of the start of each line.
    numLines = 0;
    for( numLines = 0; numLines < MAXLINES; )
    {
        offset = ftell(fP);
        lineMap[numLines++] = offset;

        if( !fgets(tmpbuf, sizeof(tmpbuf), fP) )
        {
            break;
        }

        if( fromLst )
        {
            // Pick up the address for building the memory map.
            // Line will be of the form:
            // [ ]+lineno: address (rest of line)
            // Line numbers are always decimal, addresses octal.
            // Any line not of the above form is ignored.
            // Note that the line number is that of the line in the original source,
            // not in the listing file.
            // So, we ignore it and use the current listing file line number.
            if( (cP = strchr(tmpbuf,':')) )
            {
                *cP++ = NUL;

                // cP now pointing to the space after the :
                address = strtol(cP, &cP2, 8);

                if( *cP2 != ' ' )
                {
                    // Not a memory allocating line, skip
                    continue;
                }

                bank = (address & 0xF000) >> 12;
                address &= 0xFFF;
                if( (bank >= 0) && (bank < MEMBANKS) )
                {
                    if( !(bankP = memMap[bank]) )    // not allocated yet
                    {
                        bankP = (MapEntryPP)calloc(MEMSIZE, sizeof(MapEntryP));
                        memMap[bank] = bankP;
                        memMapped = true;
                    }

                    newP = (MapEntryP)calloc(1, sizeof(MapEntry));
                    newP->lineNo = numLines;

                    if( !bankP[address] )
                    {
                        // First one
                        bankP[address] = newP;    // if muliple users of addr, take only the first
                    }
                    else
                    {
                        // Get to the end of the list
                        for( memP = bankP[address]; memP->nextP; memP = memP->nextP )
                        {
                        }

                        memP->nextP = newP;
                    }
                }
            }
        }
    }

    curLine = -1;
    return(true);
}

// Try to get a line number, return it if found else -1.
int
getLineFromAddress(int address)
{
MapEntryP entryP;

    if( (entryP = getLinesFromAddress(address)) )
    {
        return( entryP->lineNo );
    }
    else
    {
        return(-1);
    }
}

// If we have a mem->line map, get the line number.
// If not, return -1.
// Address is a full 16 bit address.
MapEntryP
getLinesFromAddress(int address)
{
int bank;
MapEntryPP bankP;

    if( !isMemMapped() )
    {
        return( NIL );
    }

    bank = (address & 0170000) >> 12;
    address &= 07777;
    if( (bank >= 0) && (bank < MEMBANKS) )
    {
        if( !(bankP = memMap[bank]) )    // nothing here
        {
            return( NIL );
        }

        return( bankP[address] );
    }
    else
    {
        return( NIL );
    }
}

// How many lines are in the file
int
getLineCount()
{
    return(numLines);
}

// Done with file
void
closeListFile()
{
int i, j;
MapEntryPP bankP;
MapEntryP entryP, nextP;;

    if( fP )
    {
        fclose(fP);
    }

    fP = 0;
    numLines = 0;

    // Clean up memory map if we have one
    if( isMemMapped() )
    {
        memMapped = false;
        for( i = 0; i < MEMBANKS; ++i )
        {
            if( (bankP = memMap[i]) )
            {
                for( entryP  = *bankP, j = 0; j < MEMSIZE; ++j, ++entryP )
                {
                    nextP = entryP->nextP;
                    free(entryP);
                    entryP = nextP;
                }

                free( memMap[i] );
                memMap[i] = NIL;
            }
        }
    }
}

// Print multiple lines from a MapEntry list.
void
printLines(MapEntryP linesP)
{
    while( linesP )
    {
        printLine(linesP->lineNo);
        linesP = linesP->nextP;
    }
}

// Try to print a line given its line number.
// Return true if it was printed, else false.
// If successful, the current line is set to this line.
bool
printLine(int lineno)
{
char linebuf[1024];

    if( !fP || (lineno < 1) || (lineno > numLines) )
    {
        return(false);
    }

    fseek(fP, lineMap[lineno-1], SEEK_SET);
    if( fgets(linebuf, sizeof(linebuf), fP) )
    {
        curLine = lineno;
        // Lines from a .lst file aready have the line number text
        printf("%s", linebuf);
        return(true);
    }
    else
    {
        return(false);
    }
}

// Try to print the next line in the file, initially set by printLine().
// Return true if it was printed, else false.
// If successful, the current line is incremented.
bool
printNextLine(void)
{
    if( printLine(curLine + 1) )
    {
        ++curLine;
        return(true);
    }

    return(false);
}

// What is says.
int
getCurrentLineNumber()
{
    return( curLine );
}
