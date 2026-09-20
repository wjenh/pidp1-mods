/*
 * Load a binary tape as if it had been done via read-in.
 * A tape in bin or am1 format can be loaded.
 * There are two modes.
 * If the emulator is running, the tape can be loaded directly into the
 * pdp-1 live memory over the emulator's debugger link (TCP, the ad1port
 * setting in pidp1.config; -h names a host on another machine).
 * If not, the core-image save file, coremem, can be overwritten
 * with the tape so that on the next pdp-1 boot the program will
 * be memory resident.
 *
 * If you're tired of waiting for those simulated tape reads,
 * here's your answer.
 *
 * 26-Aug-2026 wje initial version
 * 28-Aug-2026 wje add load-to-memory-file
 * 12-Sep-2026 wje add more detail to usage
 * 20-Sep-2026 Claude replace the shared-memory link with the network link; the tape is parsed
 *    completely first and then written in one request, so a bad tape changes nothing.
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../AD1/ad1link.h"

#define DEFAULT_MEMFILE "/opt/pidp1-mods/coremem"

// The kind of tape we are loading
#define BINTAPE 1
#define AM1TAPE 2

#define MEMSIZE 4096
#define MEMBANKS 16

#define LOADFAILED  -1
#define LOADSTOP    -2
#define BANKOF(x) (((x) >> 12) & 017)
#define ADDRESSOF(x) ((x) & 07777)
#define FULLADDR(bank, addr) (((bank) << 12) | ADDRESSOF(addr))

static int savedWord;

// The words of the tape, block after block, and where each block goes. They are collected while
// the tape is parsed and sent only when it has parsed cleanly.
static uint32_t *imageP;
static uint32_t imageWords;
static uint32_t imageCap;
static Ad1Block *blocksP;
static uint32_t nBlocks;
static uint32_t blocksCap;

static int loadTape(char *filenameP, FILE *memFileFp, bool toFile);
static int getWord(FILE *fP);
static int skipLoader(FILE *fP);
static int loadAm1(FILE *fP, FILE *memFilefP, bool toFile);
static int loadBin(FILE *fP, FILE *memFilefP, bool toFile);
static bool beginBlock(uint32_t address);
static bool addWord(uint32_t word);
static bool sendImage(Ad1Link *linkP, bool *wasRunningP);
static void ungetWord(int word);
static void usage(void);

int
main(int argc, char **argv)
{
int address;
bool useMemFile;
size_t len;
char *cP;
char *filenameP;
char *lineP;
char *memFileNameP;
char *hostP;
FILE *memFilefP;
Ad1Link link;
bool wasRunning;
int status;

    memFileNameP = DEFAULT_MEMFILE;
    hostP = NULL;
    useMemFile = false;

    // do the command line processing
    ++argv;
    --argc;

    while(argc && (**argv == '-'))                        /* look for directives */
    {
        for(cP = *argv + 1; *cP;)
        {
            switch(*cP++)
            {
            case 'm':
                useMemFile = true;
                break;

            case 'f':               // memfile to use if not the default
                --argc;
                ++argv;
                memFileNameP = *argv;
                break;

            case 'h':               // host[:port] of the emulator, if not this machine
                --argc;
                ++argv;
                if( argc < 1 )
                {
                    usage();
                }

                hostP = *argv;
                break;

            default:
                usage();
                break;
            }
        }

        --argc;
        ++argv;
    }

    if(argc != 1)
    {
        usage();
    }

    filenameP = *argv;

    if( useMemFile )
    {
        printf("Loading into memory file, be sure the pdp-1 isn't running!\n");
        printf("If it is, this load will be overwritten when it exits.\n");
        printf("Continue? (y/n) ");
        if( (getline(&lineP, &len, stdin)) != -1 )
        {
            if( *lineP != 'y' )
            {
                exit(0);
            }
        }

        if( !(memFilefP = fopen(memFileNameP, "w")) )
        {
            fprintf(stderr, "Can't open memory file '%s', be sure the directory and file have write permission.\n",
                memFileNameP);
            exit(1);
        }

        if( (address = loadTape(filenameP, memFilefP, true)) == LOADFAILED )
        {
            fclose(memFilefP);
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }
        else if( address == LOADSTOP )
        {
            fclose(memFilefP);
            printf("Tape loaded, an am1 program with a stop directive.\n");
            exit(0);
        }

        printf("Tape loaded, start address 0%0o.\n", address);
        fclose(memFilefP);
    }
    else
    {
        // Parse the whole tape before touching the emulator, so a bad tape changes nothing.
        if( (address = loadTape(filenameP, 0, false)) == LOADFAILED )
        {
            fprintf(stderr, "Can't load the tape, is it a valid macro or am1 rim tape?\n");
            exit(1);
        }

        if( ad1LinkOpen(&link, hostP, 0, "fastload") != 0 )
        {
            fprintf(stderr, "%s\n", ad1LinkError());
            exit(1);
        }

        // Stops the pdp-1 if it is running, then stores every block in one step.
        wasRunning = false;
        if( !sendImage(&link, &wasRunning) )
        {
            ad1LinkClose(&link);
            exit(1);
        }

        if( wasRunning )
        {
            printf("The pdp-1 was running and has been stopped.\n");
        }

        if( address == LOADSTOP )
        {
            printf("Tape loaded, an am1 program with a stop directive.\n");
            ad1LinkClose(&link);
            exit(0);
        }

        printf("Tape loaded, start address 0%0o. Start it (y or n/newline)?\n", address);
        if( (getline(&lineP, &len, stdin)) != -1 )
        {
            if( *lineP == 'y' )
            {
                if( (status = ad1Start(&link, (uint32_t)address, NULL, NULL)) != AD1P_ST_OK )
                {
                    fprintf(stderr, "Can't start the pdp-1: %s\n", ad1StatusText(status));
                    ad1LinkClose(&link);
                    exit(1);
                }
            }
        }

        ad1LinkClose(&link);
    }

    exit(0);
}

// Read and check a tape. To a file, the words are written as they are read. Otherwise they are
// only collected, for sendImage() to send once the whole tape has been read.
// Return the starting address or LOADSTOP on success, else LOADFAILED.
int
loadTape(char *filenameP, FILE *memFileFp, bool toFile)
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
        if( (addr = loadAm1(fP, memFileFp, toFile)) == LOADFAILED )
        {
            printf("Loading failed, tape is not the correct format.\n");
            return(LOADFAILED);
        }
    }
    else if( kind == BINTAPE )
    {
        if( (addr = loadBin(fP, memFileFp, toFile)) == LOADFAILED )
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
// If toFile is true, write to the memory file, else collect the words for sendImage().
int
loadAm1(FILE *fP, FILE *memFilefP, bool toFile)
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
                if( !toFile && !beginBlock((uint32_t)curAddr) )
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

            if( toFile )
            {
                fprintf(memFilefP,"%06o: %06o\n", curAddr++, word);
            }
            else
            {
                if( !addWord((uint32_t)word) )
                {
                    return(LOADFAILED);
                }

                ++curAddr;
            }

            if( curAddr == endAddr )
            {
                loading = false;                // end of a data block
            }
        }
    }

    return(LOADFAILED);
}

// Load a macro-style binary, return the starting address or LOADFAIL for an error.
// If toFile is true, write to the memory file, else collect the words for sendImage(). (A bin
// tape with -m used to store through a null pointer, since there is no live memory to store to.)
int
loadBin(FILE *fP, FILE *memFilefP, bool toFile)
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
                if( !toFile && !beginBlock((uint32_t)curAddr) )
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

            if( toFile )
            {
                fprintf(memFilefP,"%06o: %06o\n", curAddr++, word);
            }
            else
            {
                if( !addWord((uint32_t)word) )
                {
                    return(LOADFAILED);
                }

                ++curAddr;
            }

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

// Stop the pdp-1 if it is running and write the collected blocks, all together or not at all.
// Sets *wasRunningP. Returns false, after saying why, if it could not be done.
bool
sendImage(Ad1Link *linkP, bool *wasRunningP)
{
uint32_t i;
uint32_t used;
uint32_t wasRunning;
int status;

    // A block that ended up with no words has nothing to write; drop it, keeping the words of
    // the others in step.
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

void
usage(void)
{
    fprintf(stderr, "Usage: fastload [-h host[:port]] [-m] [-f memfilename] rimfile\n");
    fprintf(stderr, "    By default, this will load directly into active memory, stopping the pdp-1\n");
    fprintf(stderr, "    if it is running. The pidp-1 must be running with its debugger port enabled\n");
    fprintf(stderr, "    (ad1port in pidp1.config, on by default). -h names a pidp-1 on another machine,\n");
    fprintf(stderr, "    default localhost:1044; a host other than localhost with no port gets 1045.\n");
    fprintf(stderr, "    Otherwise, -m will update the coremem file, the program must be manually started.\n");
    fprintf(stderr, "    If -f is not used, the default is /opt/pidp1-mods/coremem\n");
    fprintf(stderr, "    Don't use -m if the pidp-1 is running, the memory file will be overwritten by it.\n");
    exit(1);
}
