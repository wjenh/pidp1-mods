// Load a binary tape as if it had been done via read-in.
// A tape in bin or am1 format can be loaded.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "ad1.h"

// The kind of tape we are loading
#define BINTAPE 1
#define AM1TAPE 2

int loadTape(char *filenameP);

static int savedWord;

// The words of the tape, block after block, and where each block goes. They are collected while
// the tape is parsed and sent only when it has parsed cleanly, so a bad tape changes nothing.
static uint32_t imageWords[MAXMEM];
static uint32_t nWords;
static Ad1Block blocks[MAXMEM];
static uint32_t nBlocks;

static int getWord(FILE *fP);
static void ungetWord(int word);
static int skipLoader(FILE *fP);
static int loadAm1(FILE *fP);
static int loadBin(FILE *fP);
static bool beginBlock(uint32_t address);
static bool addWord(uint32_t word);
static bool sendImage(void);

// Attempt to load a tape.
// If the file can't be opened, say so and return LOADFAILED.
// If the file isn't a valid binary load tape, say so and return LOADFAILED.
// If it succeeds, return the starting address or if it was an am1 tape with a stop,
// return LOADSTOP.
int
loadTape(char *filenameP)
{
int kind;           // which kind of format we are loading
int addr;
FILE *fP;

    if( !(fP = fopen(filenameP, "r")) )
    {
        printf("Can't open tape file '%s'.\n", filenameP);
        return( LOADFAILED );
    }

    savedWord = -1;             // for ungetWord() pushback
    nWords = 0;
    nBlocks = 0;

    // A tape can be a bin tape, an am1 tape with a loader, or an am1 tape with no loader.
    // Tapes with loaders are loaded by read-in.
    // For these:
    // A bin tape will have the first two words as 0327751, 0730002.
    // An am1 tape with a loader will have 0327751, 0724074 or 0760000.
    // Both of these will end when the first word of subsequent two-word pairs is 0607751.
    // An am1 tape with no loader will have a first word whose high 2 bits are 0.

    if( (kind = skipLoader(fP)) == EOF )
    {
        printf("File '%s' is not a valid macro binary or am1 binary tape.\n", filenameP);
        fclose(fP);
        return(LOADFAILED);
    }

    addr = LOADFAILED;

    // ready to load
    if( kind == AM1TAPE )
    {
        addr = loadAm1(fP);
    }
    else if( kind == BINTAPE )
    {
        addr = loadBin(fP);
    }

    fclose(fP);
    if( addr == LOADFAILED )
    {
        printf("Loading failed, tape is not the correct format.\n");
        return(LOADFAILED);
    }

    if( !sendImage() )
    {
        return(LOADFAILED);
    }

    return(addr);
}

// Read the next binary word, 3 characters.
// If a char isn't marked as a binary char, ignore it and continue reading.
// Returns the next 18 bit word or EOF if at the end of the file.
int
getWord(FILE *fP)
{
int in;
int word;
int count;

    if( savedWord != -1 )
    {
        word = savedWord;
        savedWord = -1;
        return(word);
    }

    for( count = 0, word = 0; count < 3; )
    {
        if( (in = fgetc(fP)) == EOF )
        {
            return(EOF);
        }

        if( in & 0200 )     // binary marker
        {
            word <<= 6;
            word |= (in & 077);
            ++count;
        }
    }

    return(word);
}

// One level only pushback.
void
ungetWord(int word)
{
    savedWord = word;
}

// Read the beginning of a tape, figure out what format it is and skip the loader, if any.
// Returns one of BINTAPE, AM1TAPE, or EOF on error.
int
skipLoader(FILE *fP)
{
int word;
int kind;

    if( (word = getWord(fP)) == EOF )
    {
        return(EOF);
    }

    if( word == 0327751 )       // read-in loader, see which one
    {
        word = getWord(fP);
        if( word == 0730002 )
        {
            kind = BINTAPE;
        }
        else if( (word == 0724074) || (word == 0760000) )
        {
            kind = AM1TAPE;
        }
        else
        {
            return(EOF);
        }
    }
    else if( (word & 0600000) == 0 )
    {
        ungetWord(word);        // am1 tape with no loader, put back the beginning of the am1 data
        return( AM1TAPE );
    }
    else
    {
        return(EOF);            // neither: kind would be used unset below
    }

    // Ok, we have a tape with a read-in loader, skip it
    while( (word = getWord(fP)) != EOF )
    {
        if( (word & 0770000) == 0600000 )
        {
            break;              // end of loader
        }

        getWord(fP);            // skip 2nd word in the pair
    }

    if( word == EOF )
    {
        return(EOF);            // invalid tape
    }

    return(kind);
}

// Load an am1 binary, return the starting address or LOADSTOP, or LOADFAIL for an error.
int
loadAm1(FILE *fP)
{
int word;
int op;
int curAddr = -1;
int endAddr;
bool loading;

    loading = false;            // true if we are loading data, false if looking for a control word.

    while( (word = getWord(fP)) != EOF )
    {
        if( !loading )
        {
            op = word & 0600000;
            if( op == 0 )       // beginning of a block
            {
                curAddr = word & 0177777;
                endAddr = getWord(fP);
                loading = true;
                if( !beginBlock((uint32_t)curAddr) )
                {
                    return(LOADFAILED);
                }
            }
            else if( op == 0400000 )    // starting address, done
            {
                return( word & 0177777 );
            }
            else if( op == 0600000 )    // stop, done
            {
                return(LOADSTOP);
            }
            else
            {
                return(LOADFAILED);
            }
        }
        else
        {
            if( (curAddr < 0) || (curAddr >= (MEMSIZE * MEMBANKS)) )
            {
                return(LOADFAILED);
            }

            if( !addWord((uint32_t)word) )
            {
                return(LOADFAILED);
            }

            ++curAddr;
            if( curAddr == endAddr )
            {
                loading = false;                // end of a data block
            }
        }
    }

    return(LOADFAILED);
}

// Load a macro-style binary, return the starting address or LOADFAIL for an error.
int
loadBin(FILE *fP)
{
int word;
int op;
int curAddr = -1;
int endAddr;
bool loading;

    loading = false;            // true if we are loading data, false if looking for a control word.

    while( (word = getWord(fP)) != EOF )
    {
        if( !loading )
        {
            op = word & 0770000;
            if( op == 0320000 )       // DIO, beginning of a block
            {
                // Only 12 bit addresses for bin loader
                curAddr = ADDRESSOF(word);
                endAddr = ADDRESSOF(getWord(fP));
                loading = true;
                if( !beginBlock((uint32_t)curAddr) )
                {
                    return(LOADFAILED);
                }
            }
            else if( op == 0600000 )    // JMP starting address, done
            {
                return( ADDRESSOF(word) );
            }
            else
            {
                return(LOADFAILED);
            }
        }
        else
        {
            if( (curAddr < 0) || (curAddr >= (MEMSIZE * MEMBANKS)) )
            {
                return(LOADFAILED);
            }

            if( !addWord((uint32_t)word) )
            {
                return(LOADFAILED);
            }

            ++curAddr;
            if( curAddr == endAddr )
            {
                getWord(fP);                    // discard the checksum
                loading = false;                // end of a data block
            }
        }
    }

    return(LOADFAILED);
}

// Start a new block at an address. The words that follow belong to it.
static bool
beginBlock(uint32_t address)
{
    if( nBlocks >= MAXMEM )
    {
        return(false);
    }

    blocks[nBlocks].address = address;
    blocks[nBlocks].count = 0;
    ++nBlocks;
    return(true);
}

// Add a word to the current block. A tape with more words than the machine has memory is not one
// that could have been meant to load.
static bool
addWord(uint32_t word)
{
    if( nWords >= MAXMEM )
    {
        return(false);
    }

    imageWords[nWords++] = word;
    ++blocks[nBlocks - 1].count;
    return(true);
}

// Send the tape to the emulator in one request, stopping it first if it is running. Nothing is
// stored unless all of it can be. Returns true on success.
static bool
sendImage(void)
{
uint32_t i;
uint32_t used;
bool wasRunning;
int status;

    // A block that ended up with no words has nothing to write; drop it, keeping the words of
    // the others in step.
    used = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        if( blocks[i].count > 0 )
        {
            blocks[used++] = blocks[i];
        }
    }

    nBlocks = used;
    if( nBlocks == 0 )
    {
        return(true);               // an empty tape, nothing to store
    }

    status = tgtWriteBlocks(blocks, nBlocks, imageWords, true, &wasRunning);
    if( status != AD1P_ST_OK )
    {
        printf("Can't load the pdp-1: %s.\n", ad1StatusText(status));
        if( status == AD1P_ST_TIMEOUT )
        {
            printf("It did not stop in time, nothing was stored.\n");
        }

        return(false);
    }

    if( wasRunning )
    {
        printf("The pdp-1 was running and has been stopped.\n");
    }

    return(true);
}
