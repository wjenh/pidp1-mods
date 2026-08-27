/*
 * Load a binary tape as if it had been done via read-in.
 * A tape in bin or am1 format can be loaded.
 * The emulator must be running and must be in shared memory mode,
 * the data is directly loaded into working memory.
 * If you're tired of waiting for those simulated tape reads,
 * here's your answer.
 *
 * 26-Aug-2026 wje initial version
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

// pdp1.h needs these defined
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;

// We need this to pick up the definition of the PDP1 data structure.
#define USEAM1
#include "/opt/pidp1-mods/src/blincolnlights/pdp1/pdp1.h"

// The kind of tape we are loading
#define BINTAPE 1
#define AM1TAPE 2

#define MEMSIZE 4096
#define MEMBANKS 16

#define SHM_NAME "/pidp1"

#define NIL 0

#define LOADFAILED  -1
#define LOADSTOP    -2
#define BANKOF(x) (((x) >> 12) & 017)
#define ADDRESSOF(x) ((x) & 07777)
#define FULLADDR(bank, addr) (((bank) << 12) | ADDRESSOF(addr))

PDP1P pdp1P;                    // how we get to pdp-1 memory

static int savedWord;

static bool attachMemory(void);
static int loadTape(char *filenameP);
static int getWord(FILE *fP);
static void ungetWord(int word);
static int skipLoader(FILE *fP);
static int loadAm1(FILE *fP);
static int loadBin(FILE *fP);

int
main(int argc, char **argv)
{
int address;
size_t len;
char *filenameP;
char *lineP;

    if( argc != 2 )
    {
        fprintf(stderr, "Usage: fastload rimfile\n");
        exit(1);
    }

    filenameP = argv[1];
    if( !attachMemory() )
    {
        fprintf(stderr, "Can't attach to the pdp-1, is it running and is shared=yes in the config file?\n");
        exit(1);
    }

    // Ok, be sure pdp-1 is stopped and load it.
    AD1_SET_STOP(pdp1P);
    if( (address = loadTape(filenameP)) == LOADFAILED )
    {
        fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
        exit(1);
    }
    else if( address == LOADSTOP )
    {
        printf("Tape loaded, an am1 program with a stop directive.\n");
        exit(0);
    }

    printf("Tape loaded, start address 0%0o. Start it (y or n/newline)?\n", address);
    if( (getline(&lineP, &len, stdin)) != -1 )
    {
        if( *lineP == 'y' )
        {
            pdp1P->ad1StartAddr = ADDRESSOF(address);
            pdp1P->ad1ExtendedAddr = address & 0170000;    
            AD1_CLEAR_SINGLE(pdp1P);    // shouldn't be set, but be sure
            AD1_SET_START(pdp1P);
        }
    }

    exit(0);
}

// Attempt to load a tape directly into pdp-1 active memory.
// If the file can't be opened, say so and exit.
// If the file isn't a valid binary load tape, say so and exit.
// If the shared memory segment can't be attached, say so and exit.
// Otherwise, stop the pdp-1 and load its memory from the tape.
// If it succeeds, print the starting address, ask if it should be started.
// If so, start the pdp-1 at that address before exiting..

// Do the actual loading.
// Return the starting address or LOADSTOP on success, else LOADFAILED.
int
loadTape(char *filenameP)
{
int kind;           // which kind of format we are loading
int addr;
FILE *fP;

    if( !(fP = fopen(filenameP, "r")) )
    {
        printf("Can't open tape file '%s'.\n", filenameP);
        return( -1 );
    }

    savedWord = -1;             // for ungetWord() pushback

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
            return(LOADFAILED);
        }
    }
    else if( kind == BINTAPE )
    {
        if( (addr = loadBin(fP)) == LOADFAILED )
        {
            printf("Loading failed, tape is not the correct format.\n");
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
// LOADSTOP is returned if the program ended with a stop direcive.
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

            pdp1P->core[curAddr++] = word;      // just put the data into the current addr
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

            pdp1P->core[curAddr++] = word;      // just put the data into the current addr
            if( curAddr == endAddr )
            {
                getWord(fP);                    // discard the checksum
                loading = false;                // end of a data block
            }
        }
    }

    return(LOADFAILED);
}

// Attach the shared memory segment,
// return true for success, else false.
bool
attachMemory(void)
{
int shmFd;

    shmFd = shm_open(SHM_NAME, O_RDWR, 0666);
    if( shmFd < 0 )
    {
        return(false);                          // not found, pdp1 not running or not set up for shared memory
    }
    else
    {
        pdp1P = mmap(NIL, sizeof(PDP1), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
        if( pdp1P == NIL )
        {
            fprintf(stderr, "mmap failed\n");
            close(shmFd);
            exit(1);
        }

        close(shmFd);
    }

    return(true);
}
