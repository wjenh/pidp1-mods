// This implements the loader used by Kalah.
// It expects tapes formatted with the following word sequence:
// 0 - start adodress for this block, or if the high bit is set, execute this word (should be a jmp)
// 1 - word count for this block
// 2 - checksum, sum of 0 and 1
// 3-n data words
// Repeat.

#include <stdio.h>

#include "loader.h"

static int state = LOADER_CMD_START;    // what we're doing now
static int curAddr;                 // next address to store in
static int endAddr;                 // terminating condition

extern int tape_loc;

extern int nextWord(FILE *fP);

int
kloader(FILE *fP, int directive, int *addressP, int *dataP, char *argsP[])
{
int word;

    if( directive == LOADER_CMD_INIT )
    {
        state = LOADER_CMD_INIT;
        return( LOADER_INIT_NONE );
    }

    if( (directive == LOADER_CMD_START) || state == LOADER_CMD_START )
    {
        // Expect 2 words, block start addr, block end addrm, or a run, pdp-1 bit 0 set
        if( (curAddr = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        if( curAddr & 0400000 )
        {
            // This is a start, endAddr is the checksum for this, ignore checksum
            *addressP = curAddr & 07777;
            state = LOADER_DONE;
            return( LOADER_DONE );
        }

        // next is a word count to load
        if( (endAddr = curAddr + nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        // checksum of start and count, ignore it
        if( (word = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        state = LOADER_CMD_NEXT;        // ready for data
        return( LOADER_MORE );
    }

    if( state != LOADER_CMD_NEXT )
    {
        return( LOADER_ERROR );
    }

    // Reading data
    if( curAddr == endAddr )
    {
        // Now expect a checksum, which we also ignore
        if( (word = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        // Back to start condition and keep going
        state = LOADER_CMD_START;
    }
    else
    {
        if( (word = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        *addressP = curAddr++;
        *dataP = word;
        return( LOADER_MORE );
    }
}
