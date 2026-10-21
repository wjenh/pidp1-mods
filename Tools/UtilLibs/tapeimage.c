/*
 * Read a binary tape as read-in would, for fastload and ovlload.
 * A tape in macro bin or am1 format is checked and collected as an image of blocks
 * which sendImage() stores in the running pdp-1 over the debugger link in one step, or that
 * tapeWriteMemFile() writes to the core-memory file.
 * Nothing is written until the entire tape has been read, a bad tape changes nothing.
 * Depends on ad1's link client, ad1link.c, which the program links in.
 *
 * 21-Oct-2026 Claude each tape starts a fresh image, a bad tape's file is closed, and the
 *   memory file is written from the image
 * 21-Oct-2026 wje cleanup
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "ad1link.h"
#include "tapeimage.h"

// The kind of tape we are loading
#define BINTAPE 1
#define AM1TAPE 2

#define MEMSIZE 4096
#define MEMBANKS 16

#define BANKOF(x) (((x) >> 12) & 017)
#define ADDRESSOF(x) ((x) & 07777)
#define FULLADDR(bank, addr) (((bank) << 12) | ADDRESSOF(addr))

static int savedWord;

// The words of the tape, block after block, and where each block goes.
// They are collected while the tape is parsed and sent only when it has parsed cleanly.
static uint32_t *imageP;
static uint32_t imageWords;
static uint32_t imageCap;
static Ad1Block *blocksP;
static uint32_t nBlocks;
static uint32_t blocksCap;

static int getWord(FILE *fP);
static int skipLoader(FILE *fP);
static int loadAm1(FILE *fP);
static int loadBin(FILE *fP);
static bool beginBlock(uint32_t address);
static bool addWord(uint32_t word);
static void ungetWord(int word);

// Read and check a tape for sendImage() or tapeWriteMemFile().
// Return the starting address or LOADSTOP on success, else LOADFAILED.
int
loadTape(char *filenameP)
{
int kind;           // which kind of format we are loading
int addr;
FILE *fP;

    // The buffers are kept for reuse; only what they hold is dropped.
    imageWords = 0;
    nBlocks = 0;
    savedWord = -1;             // for ungetWord() pushback

    if( !(fP = fopen(filenameP, "r")) )
    {
        printf("Can't open tape file '%s'.\n", filenameP);
        return( -1 );
    }

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
        return(-1);
    }

    addr = LOADFAILED;

    // ready to load
    if( kind == AM1TAPE )
    {
        if( (addr = loadAm1(fP)) == LOADFAILED )
        {
            printf("Loading failed, tape is not the correct format.\n");
            fclose(fP);
            return(LOADFAILED);
        }
    }
    else if( kind == BINTAPE )
    {
        if( (addr = loadBin(fP)) == LOADFAILED )
        {
            printf("Loading failed, tape is not the correct format.\n");
            fclose(fP);
            return(LOADFAILED);
        }
    }

    fclose(fP);
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
// LOADSTOP is returned if the program ended with a stop directive.
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

// Start a new block of the image at address. Returns false if out of memory.
bool
beginBlock(uint32_t address)
{
    if( nBlocks == blocksCap )
    {
        Ad1Block *newP;

        blocksCap = (blocksCap == 0) ? 16 : (blocksCap * 2);
        if( !(newP = realloc(blocksP, blocksCap * sizeof(Ad1Block))) )
        {
            fprintf(stderr, "Out of memory.\n");
            return(false);
        }

        blocksP = newP;
    }

    blocksP[nBlocks].address = address;
    blocksP[nBlocks].count = 0;
    ++nBlocks;
    return(true);
}

// Add one word to the current block. Returns false if out of memory.
bool
addWord(uint32_t word)
{
    if( imageWords == imageCap )
    {
        uint32_t *newP;

        imageCap = (imageCap == 0) ? 4096 : (imageCap * 2);
        if( !(newP = realloc(imageP, imageCap * sizeof(uint32_t))) )
        {
            fprintf(stderr, "Out of memory.\n");
            return(false);
        }

        imageP = newP;
    }

    imageP[imageWords++] = word;
    ++blocksP[nBlocks - 1].count;
    return(true);
}

// Stop the pdp-1 if it is running and write the collected blocks, all or not at all.
// Sets *wasRunningP.
// Returns false, after saying why on an error.
bool
sendImage(Ad1Link *linkP, bool *wasRunningP)
{
uint32_t i;
uint32_t used;
uint32_t wasRunning;
int status;

    // A block that ended up with no words has nothing to write.
    // Drop it, keeping the words of the others in step.
    used = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        if( blocksP[i].count > 0 )
        {
            blocksP[used++] = blocksP[i];
        }
    }

    nBlocks = used;
    if( nBlocks == 0 )
    {
        return(true);               // an empty tape, nothing to store
    }

    wasRunning = 0;
    status = ad1WriteBlocks(linkP, blocksP, nBlocks, imageP, 1, &wasRunning, NULL);
    *wasRunningP = (wasRunning != 0);
    if( status != AD1P_ST_OK )
    {
        fprintf(stderr, "Can't load the pdp-1: %s\n", ad1StatusText(status));
        if( status == AD1P_ST_TIMEOUT )
        {
            fprintf(stderr, "It did not stop in time, nothing was stored.\n");
        }

        return(false);
    }

    return(true);
}

// Find the word the collected image puts at address, the last one given when blocks overlap,
// as read-in would leave it.
// Returns true with *wordP set, or false if no block covers address.
bool
tapeImageWord(uint32_t address, uint32_t *wordP)
{
uint32_t i;
uint32_t first;
bool found;

    found = false;
    first = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        if( (address >= blocksP[i].address) && (address < (blocksP[i].address + blocksP[i].count)) )
        {
            *wordP = imageP[first + (address - blocksP[i].address)];
            found = true;
        }

        first += blocksP[i].count;
    }

    return(found);
}

// Whether the memory file can be written, asked without changing it.
// If it did not exist before is removed again.
bool
tapeMemFileWritable(const char *nameP)
{
struct stat st;
bool existed;
FILE *fP;

    // lstat, so a symlink to a file not yet made counts as there and is not removed.
    existed = (lstat(nameP, &st) == 0);
    if( !(fP = fopen(nameP, "a")) )
    {
        return(false);
    }

    fclose(fP);
    if( !existed )
    {
        unlink(nameP);
    }

    return(true);
}

// Write the collected image to an opened core-memory file, one line per word in tape order, as
// the pidp-1 reads it at its start.
// Returns false if a write failed; the file may be partial then.
bool
tapeWriteMemFile(FILE *memFileFp)
{
uint32_t i;
uint32_t j;
uint32_t first;

    first = 0;
    for( i = 0; i < nBlocks; ++i )
    {
        for( j = 0; j < blocksP[i].count; ++j )
        {
            if( fprintf(memFileFp, "%06o: %06o\n", (blocksP[i].address + j), imageP[first + j]) < 0 )
            {
                return(false);
            }
        }

        first += blocksP[i].count;
    }

    return(fflush(memFileFp) == 0);
}
