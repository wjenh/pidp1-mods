// Functions to provide source-level viewing of a file.
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "ad1.h"

int curLine;
int curFileNo;

extern int numFiles;
// Since multiple source lines can use the same address, we keep them all in a list
extern MapEntryPP memMap[MEMBANKS];
extern FileInfoP files[];

MapEntryP getMapForFileNo(MapEntryP mapP, int fileNo);
MapEntryP getLinesFromAddress(int addr);
int getLineFromAddress(int addr, int fileno);
int getLineCount(int fileno);
int getCurrentLineNumber(void);
void setCurrentLineNumber(int lineNo);
void printLines(MapEntryP linesP);
bool printLine(int fileno, int line);
bool printNextLine(void);

extern bool isMemMapped(void);

// Try to get a line number, return it if found else -1.
// Set the current file to the file it was found in, or the first file if not found in that file.
int
getLineFromAddress(int address, int fileno)
{
MapEntryP entryP;

    if( fileno == NOARG )
    {
        fileno = curFileNo;
    }

    if( (entryP = getLinesFromAddress(address)) )
    {
        if( !getMapForFileNo(entryP, fileno) )
        {
            // Just use the first one.
            curFileNo = entryP->fileNo;
        }
        else
        {
            curFileNo = fileno;
        }

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
MapEntryP entryP;

    if( !isMemMapped() )
    {
        return( NIL );
    }

    bank = BANKOF(address);
    address = ADDRESSOF(address);

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

// See if there is an entry in the map list that matches the given file number.
// Return it if so, else return NIL.
MapEntryP
getMapForFileNo(MapEntryP mapP, int fileNo)
{
    while( mapP )
    {
        if( mapP->fileNo == fileNo )
        {
            return( mapP );
        }
        
        mapP = mapP->nextP;
    }

    return(NIL);
}

// How many lines are in the file
int
getLineCount(int fileNo)
{
    if( (fileNo >= 0) && (fileNo <= numFiles) )
    {
        return( files[fileNo]->numLines );
    }
    else
    {
        return(0);
    }
}

// Print multiple lines from a MapEntry list,
// but only if they are in the current file.
void
printLines(MapEntryP linesP)
{
    while( linesP )
    {
        if( linesP->fileNo == curFileNo )
        {
            printLine(linesP->fileNo, linesP->lineNo);
        }
        linesP = linesP->nextP;
    }
}

// Try to print a line given its file and line number.
// Return true if it was printed, else false.
// If successful, the current line is set to this line.
bool
printLine(int fileno, int lineno)
{
FileInfoP infoP;
char linebuf[1024];

    if( (fileno < 0) || (fileno >= numFiles) )
    {
        return(false);
    }

    infoP = files[fileno];

    if( !infoP->fP || (lineno < 1) || (lineno > infoP->numLines) )
    {
        return(false);
    }

    fseek(infoP->fP, infoP->lineMap[lineno-1], SEEK_SET);
    if( fgets(linebuf, sizeof(linebuf), infoP->fP) )
    {
        curLine = lineno;
        // Lines from a .lst file aready have the line number text
        // but they doesn't match the actual line numbers in the file, they're from the original file.
        // So, also show the line number in the listing file.
        printf("%-4d %s", lineno, linebuf);
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
    if( printLine(curFileNo, curLine + 1) )
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

// This too.
void
setCurrentLineNumber(int lineNo)
{
    curLine = lineNo;
}
