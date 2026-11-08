// The core image file save/restore.
// readmem() loads working memory from it at start-up and dumpmem() saves
// working memory to it at exit, one "address: word" line in octal per nonzero word.

#include "common.h"
#include "pdp1.h"
#include "coreImage.h"

#define NIL 0

// Read a memory file produced by dumpmem() into working memory.
// Reworked by wje to keep the address and data on one line, much more readable.
void
readmem(const char *filenameP, Word *mem, Word size)
{
int val;
Word addr;
Word word;
char *strP;
FILE *fileP;
char buf[100];

    if( (fileP = fopen(filenameP, "r")) == NIL )
    {
        return;
    }

    // Handle both old and new file formats.
    // New is addr: word, old has each on a separate line.
    addr = 0;
    while( (strP = fgets(buf, 100, fileP)) )
    {
        val = strtol(strP, &strP, 8);
        if( strP && (*strP == ':') )
        {
            addr = val;         // address part
            if( addr >= size )
            {
                fprintf(stderr, "Address out of range: %o\n", addr);
                break;
            }
        }

        if( !strP || (*strP == '\n') )
        {
            // must be old format memory
            if( addr < size)
            {
                mem[addr] = val;
            }
        }
        else
        {
            // We'll assume the line is OK if we got the first part
            word = strtol(strP+1, &strP, 8);
            if( addr < size)
            {
                mem[addr] = word;
            }
        }
    }

    fclose(fileP);
}

// Dump working memory to the memory image file.
// Reworked by wje to keep the address and data on one line, much more readable.
void
dumpmem(const char *fileNameP, Word *mem, Word size)
{
FILE *fP;
Word i;

    if( !(fP = fopen(fileNameP, "w")) )
    {
        return;
    }

    for(i = 0; i < size; i++)
    {
        if( mem[i] != 0 )
        {
            // Just put it on one line, jeez
            // Support for the old format removed by wje 7-Oct-2026.
            fprintf(fP, "%06o: %06o\n", i, mem[i]);
        }
    }

    fclose(fP);
}
