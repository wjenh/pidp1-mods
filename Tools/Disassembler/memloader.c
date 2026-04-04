/*
 * Load from a pidp-1 memory image file.
 * It expects lines of the form:
 * address:value
 *
 * If args are supplied during INIT, they will be interpreted as:
 * start-address
 * end-address
 *
 * The default is 0, 7751 and either none, start-address, or both can be given.
 * They can be given in any base that strtol() recognizes, 0bnnn, 0nnn, nnn, 0xnnn.
 * The addresses are full 16 bit address space values.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "loader.h"

static int state = LOADER_CMD_START;    // what we're doing now
static int curAddr = 0;
static int beginAddr = 0;
static int endAddr = 07750;
static int startAddr = -1;

static bool readMem(FILE *fileP, int start, int end, int *addrP, int *dataP);
static int getNumber(char *strP);

int
memloader(FILE *fP, int directive, int *addressP, int *dataP, char *argsP[])
{
int i;

    if( directive == LOADER_CMD_INIT )
    {
        if( argsP )
        {
            if( argsP[0] )
            {
                if( (i = getNumber(argsP[0])) >= 0 )
                {
                    beginAddr = i;
                }

                if( argsP[1] )
                {
                    if( (i = getNumber(argsP[1])) >= 0 )
                    {
                        endAddr = i;
                    }

                    if( argsP[2] )
                    {
                        if( (i = getNumber(argsP[2])) >= 0 )
                        {
                            startAddr = i;
                        }
                    }
                }
            }
        }

        state = LOADER_CMD_INIT;
        return( LOADER_INIT_NORIM );
    }
    else if( directive == LOADER_CMD_START )
    {
        curAddr = beginAddr;
        state = LOADER_CMD_NEXT;        // ready for data
        return( LOADER_MORE );
    }

    if( state != LOADER_CMD_NEXT )
    {
        return( LOADER_ERROR );
    }

    if( !readMem(fP, curAddr, endAddr, addressP, dataP) )
    {
        if( startAddr == -1 )
        {
            startAddr = beginAddr;
        }

        *addressP = startAddr;
        *dataP = startAddr;

        state = LOADER_CMD_START;
        return( LOADER_DONE );
    }

    return( LOADER_MORE );
}

// Read a line from the input file.
// Parse into address, value.
// If the address is less than start, continue reading until not.
// If the address is greater than end or EOF, return false.
// Otherwise, set the address and value vars and return true.
// We process both the new more readable format and the old lame one.
bool
readMem(FILE *fileP, int start, int end, int *addrP, int *dataP)
{
bool didOne = false;
char *strP;
char buf[128];

    while( strP = fgets(buf, 100, fileP) )
    {
        *addrP = strtol(strP, &strP, 8);
        if( *addrP > end )
        {
            return(false);              // done
        }

        if( *addrP >= start )
        {
            if( !strP || (*strP != ':') )
            {
                continue;  // malformed, skip it
            }

            if( *(strP + 1) != ' ' )
            {
                // assume it's old format and the next line is the data
                // If it's not a valid file, will fail next read after this one also
                fgets(buf, 100, fileP);
                *dataP = strtol(buf, 0, 8);
            }
            else
            {
                // We'll assume the line is OK if we got the firt part
                *dataP = strtol(strP+1, &strP, 8);
            }

            return(true);
        }
    }

    return(false);
}

// Get a valid number in a strtol() compatible base.
// Return -1 if not valid
int
getNumber(char *strP)
{
int num;
char *cP;

    num = strtol(strP, &cP, 0);
    if( !*cP )
    {
        return(num);
    }
    else
    {
        return(-1);
    }
}


