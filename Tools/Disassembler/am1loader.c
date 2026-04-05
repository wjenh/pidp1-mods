/*
 * Am1 uses its own loader to deal with extended memory:
 * It expects tapes formatted with the following word sequence:
 * 0 - start adodress for the data, only one
 * 1 - end address + 1 for the data
 * 2-n data words
 * n+1 - if bit 0 is set, if bit 1 is 0, bits 2-17 are the start address, else stop
 * 
 * The loader code is:
 *
 * 7751: 724074          eem            if extended memory was not used, am1 will replace this with a nop
 * 7752: 730002      loop, rpb          no checksum is done, we aren't reading from a pysical tape reader
 * 7753: 327773          dio addr       word is the address to begin storing data or negative if done
 * 7754: 642000          spi
 * 7755: 607766          jmp done
 * 7756: 730002          rpb            word is the ending address + 1
 * 7757: 327774          dio end
 * 7760: 730002      load, rpb          read data words and store until end is reached
 * 7761: 337773          dio i addr
 * 7762: 447773          idx addr
 * 7763: 527774          sas end
 * 7764: 607760          jmp load
 * 7765: 607752          jmp loop
 * 7766: 662001      done, ril 1s       last addr bit 0 was set, check bit 1
 * 7767: 652000          spi i          if bit 1 was 0, start prog, bits 2-17 are address
 * 7770: 617773          jmp i addr     start prog
 * 7771: 760400          hlt            nostart, just halt
 * 7772: 607752          jmp loop       and go again
 * 7773: 000000      addr, 0
 * 7774: 000000      end, 0
 */

#include <stdio.h>

#include "loader.h"

static int state = LOADER_CMD_START;    // what we're doing now
static int curAddr;                 // next address to store in
static int endAddr;                 // terminating condition

extern int tape_loc;

extern int nextWord(FILE *fP);

int
am1loader(FILE *fP, int directive, int *addressP, int *dataP, char *argsP[])
{
int word;

    if( directive == LOADER_CMD_INIT )
    {
        state = LOADER_CMD_INIT;
        return( LOADER_INIT_NONE );
    }
    else if( directive == LOADER_CMD_START )
    {
        state = LOADER_CMD_START;
    }

    if( state == LOADER_CMD_START )
    {
        // Expect a block start addr or a terminating start/stop.
        if( (curAddr = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        *addressP = curAddr & 0xFFFF;
        *dataP = curAddr;
        
        if( curAddr & 0400000 )
        {
            return( (curAddr & 0200000)?LOADER_STOP:LOADER_DONE );
        }

        if( (endAddr = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        state = LOADER_CMD_NEXT;        // ready for data
        return( LOADER_AGAIN );
    }

    if( state != LOADER_CMD_NEXT )
    {
        return( LOADER_ERROR );
    }

    // Reading data
    if( curAddr == endAddr )
    {
        // End of block, next one or a start/stop
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
