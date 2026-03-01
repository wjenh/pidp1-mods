// Functions to provide source-level viewing of a file.
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define MAXLINES    4000
#define MEMBANKS    16
#define MEMSIZE     4096

// Define here so we don't need to drag in ad1.h
#define NIL (void *)0
#define NUL '\0'

static int numLines;
static int curLine;
static bool memMapped;
static FILE *fP;

// How we map line numbers into file locations
static long lineMap[MAXLINES];
static long *memMap[MEMBANKS];  // we don't allocate until we need to

bool isFileMapped(void);
bool isMemMapped(void);
bool loadFileMap(bool isLst, char *filenameP);
void closeListFile(void);
int getLineFromAddress(int addr);
int getLineCount(void);
int getCurrentLineNumber(void);
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
int lineno;
int bank;
int address;
long offset;
long *memP;
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
            // Pick up the line number and address for building the memory map.
            // Line will be of the form:
            // [ ]+lineno: address (rest of line)
            // Line numbers are always decimal, addresses octal.
            // Any line not of the above form is ignored.
            if( (cP = strchr(tmpbuf,':')) )
            {
                *cP++ = NUL;
                lineno = strtol(tmpbuf, &cP2, 10);

                // cP now pointing to the space after the :
                address = strtol(cP, &cP2, 8);

                if( *cP2 != ' ' )
                {
                    // Not a memory allocating line, skip
                    continue;
                }

                bank = address & 0xF000;
                address &= 0xFFF;
                if( (bank >= 0) && (bank < MEMBANKS) )
                {
                    if( !(memP = memMap[bank]) )    // not allocated yet
                    {
                        memP = (long *)calloc(MEMSIZE, sizeof(long));
                        memMap[bank] = memP;
                        memMapped = true;
                    }

                    if( !memP[address] )
                    {
                        memP[address] = lineno;    // if muliple users of addr, take only the first
                    }
                }
            }
        }
    }

    curLine = -1;
    return(true);
}

// If we have a mem->line map, get the line number.
// If not, return -1.
// Address is a full 16 bit address.
int
getLineFromAddress(int address)
{
int bank;
long *memP;

    if( !isMemMapped() )
    {
        return( -1 );
    }
    bank = address & 0xF000;
    address &= 0xFFF;
    if( (bank >= 0) && (bank < MEMBANKS) )
    {
        if( !(memP = memMap[bank]) )    // nothing here
        {
            return( -1 );
        }

        return( memP[address] );
    }
    else
    {
        return( -1 );
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
int i;

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
            if( memMap[i] )
            {
                free( memMap[i] );
                memMap[i] = NIL;
            }
        }
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
