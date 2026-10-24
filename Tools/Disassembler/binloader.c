 /* Binloader is the most common loader, also known as the DDT loader, digital-1-3-s-mb_ddt.bin.
 *
 * DDT BIN format
 *
 * DIO startaddr, 32ssss
 * DIO endaddr + 1, 32eeee
 * data
 * ...
 * checksum
 * JMP aaaa, 60aaaa, or another DIO dio block
 *
 *
 * 7751: 730002		rpb          read tape
 * 7752: 327760		dio 7760     will be a 'dio startaddr' or a 'jmp progstart', deposit to 7760
 * 7753: 107760		xct 7760     execute what we just read; if a jmp, we're done otherwise it's meaningless
 * 7754: 327776		dio 7776     initialize checksum
 * 7755: 730002		rpb          read tape
 * 7756: 327777		dio 7777     deposit to 7777, will be 'dio endaddr + 1'
 * 7757: 730002		rpb          read tape, top of loading loop
 * 7760: 60aaaa		dio cur_addr put word in current pc location
 * 7761: 217760		lac i 7760   add the word we stored to the checksum
 * 7762: 407776		add 7776     add to checksum
 * 7763: 247776		dac 7776     update checksum
 * 7764: 447760		idx 7760     7760++, makes the dio point to the next adress to store in
 * 7765: 527777		sas 7777     skip if AC == 'dio endaddr + 1'
 * 7766: 607757		jmp 7757     not done, loop
 * 7767: 207776		lac 7776     add 'dio endaddr + 1' to checksum
 * 7770: 407777		add 7777     the computed checksum is is now in the AC
 * 7771: 730002		rpb          read tape, is checksum from tape
 * 7772: 327776		dio 7776     deposit to 7776
 * 7773: 527776		sas 7776     skip if AC == 7776
 * 7774: 760400		hlt          bad checksum
 * 7775: 607751		jmp 7751     ready for another block or a jmp, back to top
 * 7776: checksum
 * 7777: 32aaaa     dio endaddr + 1
 *
*/

#include <stdio.h>

#include "loader.h"

static int state = LOADER_CMD_START;    // what we're doing now
static int curAddr;                 // next address to store in
static int endAddr;                 // terminating condition
static int blockStart;              // first address of the current block, for messages
static int checksum;                // running checksum of the current block
static int passes;                  // LOADER_CMD_STARTs seen, checksum errors are reported on the first only

extern int tape_loc;

extern int nextWord(FILE *fP);

static int onesAdd(int a, int b);

// The DDT BIN loader, see above.
// Returns LOADER_MORE with a loaded word, LOADER_AGAIN between blocks, LOADER_DONE with the start
// address from the closing jmp, LOADER_STOP if the tape ends with no jmp, or LOADER_ERROR.
int
binloader(FILE *fP, int directive, int *addressP, int *dataP, char *argsP[])
{
int word;

    if( directive == LOADER_CMD_INIT )
    {
        state = LOADER_CMD_INIT;
        return( LOADER_INIT_NONE );
    }
    else if( (directive == LOADER_CMD_START) || (state == LOADER_CMD_START) )
    {
        if( directive == LOADER_CMD_START )
        {
            ++passes;
        }

        state = LOADER_CMD_START;

        // Expect a block start addr, DIO xxxx or a terminating JMP xxxx
        // If none, the tape has no start address.
        if( (curAddr = nextWord(fP)) < 0 )
        {
            state = LOADER_DONE;
            *addressP = *dataP = 0;
            return( LOADER_STOP );
        }

        *addressP = curAddr & 07777;
        *dataP = curAddr;
        
        if( (curAddr & 0760000) == 0600000 )    // JMP
        {
            return( LOADER_DONE );
        }

        if( (curAddr & 0760000) != 0320000 )    // DIO
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        curAddr &= 07777;                       // bin loader only deals with bank 0

        if( (endAddr = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        if( (endAddr & 0760000) != 0320000 )    // DIO
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        // The loader's checksum starts with both dio words, 7754 and 7767 above
        checksum = onesAdd(*dataP, endAddr);
        blockStart = curAddr;

        endAddr &= 07777;                       // bin loader only deals with bank 0
        state = LOADER_CMD_NEXT;

        return( LOADER_AGAIN );
    }
    else if( directive == LOADER_CMD_NEXT )
    {
        state = LOADER_CMD_NEXT;
    }

    if( state != LOADER_CMD_NEXT )
    {
        return( LOADER_ERROR );
    }

    // Reading data
    if( curAddr == endAddr )
    {
        // End of block: the real loader halts on a bad checksum, we warn and go on, so the rest
        // of a damaged tape can still be seen.
        word = nextWord(fP);
        if( (word != checksum) && (passes == 1) )
        {
            fprintf(stderr, "Checksum error in the block at %04o-%04o, tape %06o, computed %06o, "
                "before tape position %d.\n", blockStart, (endAddr - 1), word, checksum, tape_loc);
        }

        state = LOADER_CMD_START;
        return( LOADER_AGAIN );
    }
    else
    {
        if( (word = nextWord(fP)) < 0 )
        {
            state = LOADER_ERROR;
            return( LOADER_ERROR );
        }

        checksum = onesAdd(checksum, word);
        *addressP = curAddr++;
        *dataP = word;
        return( LOADER_MORE );
    }
}

// 18-bit one's complement addition, as the add instruction the loader uses does it.
static int
onesAdd(int a, int b)
{
int sum;

    sum = (a + b);
    if( sum > 0777777 )
    {
        sum = ((sum + 1) & 0777777);       // end-around carry
    }

    return( sum );
}
