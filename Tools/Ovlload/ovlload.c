/*
 * ovlload -- put a program's drum overlays on the Type 23 drum and load the program.
 *
 * am1 assembles each overlay of a program to run in a core window and writes them all
 * to a text file, file.ovl.
 * It contains a heading whose stamp line gives the build stamp and the
 * full address of the program's overlay table,
 * then for each overlay a line giving its window, bank, first and last address,
 * drum track, length and mode, followed by its code words,
 * the two header words, the overlay stamp and the overlay's number.
 * All numbers are octal.
 *
 * Each overlay goes on its track at drum addresses equal to its window's core
 * addresses and the rest of the window's range padded with zeros.
 * The overlay manager in the program loads it with one transfer.
 * Nothing else on the drum is touched.
 *
 * Unless -n is given, the program's tape is then loaded in the same way as fastload does.
 * Without -n, before anything is written, the tape's word at the overlay table's address
 * must be the .ovl's stamp, so ovlload never loads a drum and a program from different builds.
 * With -n no tape is read and nothing is compared: a program from another build, loaded
 * later, is caught by the overlay manager's stamp check at its first overlay load.
 *
 * A track holding something another program put there is overwritten and named in a warning.
 * The user is responsible for saving a drum image that is to be preserved.
 *
 * The .ovl is read and checked by ovlfile.c, which drumlist shares.
 * It is what identifies data written by other programs ovlfile recognizes.
 *
 * With -p, a paper tape is punched instead that can be directly read-in loaded.
 * It contains a loader that first loads the drum with the overlay code that is included, then
 * loads the main program and starts it.
 *
 * Usage: ovlload [-i drumfile] [-c] [-n] [-t tape] [-h host[:port]] [-y] [-m] [-M memfile] file.ovl
 *        ovlload -p tapefile [-n | -t tape] file.ovl
 * Exit status: 0 done, or with -c the drum is current; 1 with -c the drum differs;
 * 2 an error, nothing having been written unless the message says so.
 *
 * 12-Oct-2026 wje/Claude initial version
 * 13-Oct-2026 Claude .ovl reader shared in ovlfile.c; other tools' tracks warned of; -f gone
 * 13-Oct-2026 Claude -p punches a tape the pdp-1 writes its own drum from, by ovlwriter.h
 * 18-Oct-2026 wje for a rim produced using -p, just start the program when it loads, no halting.
 * 21-Oct-2026 Claude the drum writer moved to ovlfile.c for ad1 and fastload; says when it
 *   halted a running pdp-1
 * 21-Oct-2026 Claude a failed -m load leaves the memory file as it was
 * 21-Oct-2026 wje cleanup
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>

#include "ad1link.h"
#include "tapeimage.h"
#include "../AM1/xldr.h"
#include "../AM1/font5x7.h"
#include "ovlfile.h"
#include "ovlwriter.h"

#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"
#define DEFAULT_MEMFILE "/opt/pidp1-mods/coremem"

#define EXIT_DONE 0
#define EXIT_DIFFERS 1
#define EXIT_ERROR 2

// am1's tape words: a RIM pair's dio, the jmp ending the RIM part, a block's control word
// of 0 in bits 0-1, a start word of 10, a stop word of 11 (Tools/AM1/xldr.src).
#define TAPE_DIO 0320000
#define TAPE_JMP 0600000
#define TAPE_CONTROL 0600000
#define TAPE_START 0400000
#define TAPE_STOP 0600000
#define TAPE_ADDRESS 0177777
#define LOADER_NOP 0760000          // in place of the loader's eem, as am1 punches it

// Where the writer can go, words 0-077 are the 16-channel break system's and the loader
// is at LDR_START_ADDR.
#define WRITER_LOWEST 0100
#define WRITER_HIGHEST (LDR_START_ADDR - 1)

// The writer reads its parameters as runs of words.
#if (OVW_SUM != (OVW_CORE + 4)) || (OVW_COUNT != (OVW_CORE + 3)) || (OVW_DRUM != (OVW_CORE + 2)) || \
    (OVW_TRACK != (OVW_CORE + 1))
#error "ovlwriter.h: the overlay parameters are not the run ovlload punches"
#endif

// A block of the program's tape, for -p.
typedef struct
{
    uint32_t address;
    uint32_t count;
    uint32_t *wordsP;
} TapeBlock;

static OvlFile ovl;
static const char *ovlNameP;

static int32_t drum[OVL_DRUMWORDS];
static long drumFileWords;          // words the drum file held, -1 if it did not exist

// The program's tape as -p punches it again: its blocks, its last word, and whether it
// needs the eem loader.
static TapeBlock *rootBlocksP;
static int rootBlockCount;
static uint32_t rootEndWord;
static bool rootNeedsEem;

// The tape -p punches, collected before it is written.
static unsigned char *punchP;
static size_t punchLength;
static size_t punchCap;
static bool punchFailed;            // a frame could not be added, so the tape is not written

static int compareDrum(void);
static char *tapeName(const char *ovlPathP);
static int punchTape(const char *pathP, const char *rootTapeP, bool drumOnly);
static bool readRootTape(const char *pathP);
static long placeWriter(void);
static int32_t onesAdd(int32_t a, int32_t b);
static bool punchByte(int byte);
static bool punchWord(uint32_t word);
static bool punchBlank(int count);
static bool punchLabel(const char *textP);
static bool punchBlock(uint32_t address, const uint32_t *wordsP, uint32_t count);
static void usage(void);

// Read the options and the .ovl, check everything, write the drum, then load the
// program.  Exits with EXIT_DONE, EXIT_DIFFERS (-c only) or EXIT_ERROR.
int
main(int argc, char **argv)
{
int opt;
int address;
int status;
uint32_t stampWord;
uint32_t state[AD1P_STATE_WORDS];
uint32_t run;
uint32_t pc;
bool compare;
bool drumOnly;
bool useMemFile;
bool autoStart;
bool wasRunning;
bool stillRunning;
bool haveStamp;
bool programOption;
bool drumOption;
bool written;
char *drumNameP;
char *punchNameP;
char *tapeNameP;
char *hostP;
char *memFileNameP;
char *lineP;
char err[OVL_ERRSIZE];
size_t len;
FILE *memFilefP;
Ad1Link link;

    address = 0;
    drumNameP = DEFAULT_DRUM;
    memFileNameP = DEFAULT_MEMFILE;
    tapeNameP = NULL;
    hostP = NULL;
    compare = false;
    drumOnly = false;
    useMemFile = false;
    autoStart = false;
    programOption = false;
    drumOption = false;
    punchNameP = NULL;
    memFilefP = NULL;

    while( (opt = getopt(argc, argv, "i:cnt:h:ymM:p:")) != -1 )
    {
        switch( opt )
        {
        case 'i':
            drumNameP = optarg;
            drumOption = true;
            break;

        case 'p':
            punchNameP = optarg;
            break;

        case 'c':
            compare = true;
            break;

        case 'n':
            drumOnly = true;
            break;

        case 't':
            tapeNameP = optarg;
            programOption = true;
            break;

        case 'h':
            hostP = optarg;
            programOption = true;
            break;

        case 'y':
            autoStart = true;
            programOption = true;
            break;

        case 'm':
            useMemFile = true;
            programOption = true;
            break;

        case 'M':
            memFileNameP = optarg;
            useMemFile = true;
            programOption = true;
            break;

        default:
            usage();
            break;
        }
    }

    if( (argc - optind) != 1 )
    {
        usage();
    }

    // -p writes no drum file and loads nothing, only -n and -t say anything about its tape.
    if( punchNameP && (compare || drumOption || hostP || autoStart || useMemFile) )
    {
        fprintf(stderr, "ovlload: -p punches a tape and writes no drum or pdp-1; -i, -c, -h, -y,"
            " -m and -M are not used.\n");
        exit(EXIT_ERROR);
    }

    // -c and -n load no program, so an option about the program means a mistaken command.
    if( (compare || drumOnly) && programOption )
    {
        fprintf(stderr, "ovlload: -c and -n load no program; -t, -h, -y, -m and -M are not used.\n");
        exit(EXIT_ERROR);
    }

    if( useMemFile && hostP )
    {
        fprintf(stderr, "ovlload: -m loads the memory file, -h a running pdp-1; give only one.\n");
        exit(EXIT_ERROR);
    }

    ovlSetProgram("ovlload", stderr);
    ovlNameP = argv[optind];
    if( !ovlRead(ovlNameP, &ovl, err) )
    {
        fprintf(stderr, "ovlload: %s.  Nothing was written.\n", err);
        exit(EXIT_ERROR);
    }

    if( compare )
    {
        if( !ovlReadDrum(drumNameP, drum, &drumFileWords) )
        {
            exit(EXIT_ERROR);
        }

        if( drumFileWords < 0 )
        {
            fprintf(stderr, "ovlload: there is no drum file '%s'.\n", drumNameP);
            exit(EXIT_ERROR);
        }

        exit(compareDrum());
    }

    // Everything is read and checked before anything is written.
    if( !drumOnly )
    {
        if( !tapeNameP && !(tapeNameP = tapeName(ovlNameP)) )
        {
            exit(EXIT_ERROR);
        }

        if( (address = loadTape(tapeNameP)) == LOADFAILED )
        {
            fprintf(stderr, "ovlload: can't load the tape '%s', is it a valid am1 rim tape?\n", tapeNameP);
            exit(EXIT_ERROR);
        }

        stampWord = 0;
        haveStamp = tapeImageWord((uint32_t)ovl.tableAddr, &stampWord);
        if( !ovlStampMatches(&ovl, tapeNameP, haveStamp, stampWord) )
        {
            exit(EXIT_ERROR);
        }
    }

    if( punchNameP )
    {
        exit(punchTape(punchNameP, tapeNameP, drumOnly));
    }

    if( !ovlReadDrum(drumNameP, drum, &drumFileWords) )
    {
        exit(EXIT_ERROR);
    }

    // Reach the program's destination before the drum is written, so a pdp-1 that
    // can't be reached, or a memory file that can't be opened, changes nothing.
    wasRunning = false;
    if( !drumOnly && useMemFile )
    {
        printf("Loading into memory file, be sure the pdp-1 isn't running!\n");
        printf("If it is, this load will be overwritten when it exits.\n");
        printf("Continue? (y/n) ");
        fflush(stdout);
        lineP = 0;
        len = 0;
        if( (getline(&lineP, &len, stdin) == -1) || (*lineP != 'y') )
        {
            printf("Nothing was written.\n");
            exit(EXIT_DONE);
        }

        // Asked without emptying the file: it is emptied and written only once the drum is,
        // so a load that fails leaves it as it was.
        if( !tapeMemFileWritable(memFileNameP) )
        {
            fprintf(stderr, "ovlload: can't open memory file '%s', be sure the directory and file have"
                " write permission.\n", memFileNameP);
            exit(EXIT_ERROR);
        }
    }
    else if( !drumOnly )
    {
        if( ad1LinkOpen(&link, hostP, 0, "ovlload") != 0 )
        {
            fprintf(stderr, "ovlload: %s\n", ad1LinkError());
            exit(EXIT_ERROR);
        }

        // Stopped first, so no running program uses the drum while it changes. STOP's reply
        // gives the state after the stop, so whether it was running is asked first.
        if( (status = ad1GetState(&link, state)) != AD1P_ST_OK )
        {
            fprintf(stderr, "ovlload: can't read the pdp-1's state: %s\n", ad1StatusText(status));
            ad1LinkClose(&link);
            exit(EXIT_ERROR);
        }

        wasRunning = (state[AD1P_STATE_RUN] != 0);
        run = 0;
        if( (status = ad1Stop(&link, &run, &pc)) != AD1P_ST_OK )
        {
            fprintf(stderr, "ovlload: can't stop the pdp-1: %s\n", ad1StatusText(status));
            ad1LinkClose(&link);
            exit(EXIT_ERROR);
        }
    }

    if( !ovlWriteDrum(drumNameP, &ovl, drumFileWords, true) )
    {
        if( useMemFile )
        {
            printf("The memory file was not written.\n");
        }
        else if( !drumOnly )
        {
            ad1LinkClose(&link);
            printf("The program was not loaded; the pdp-1 is stopped.\n");
        }

        exit(EXIT_ERROR);
    }

    ovlWarnOverwritten(&ovl, drum);

    if( drumOnly )
    {
        exit(EXIT_DONE);
    }

    // The program, as fastload loads it, from the tape parsed above.
    if( useMemFile )
    {
        if( !(memFilefP = fopen(memFileNameP, "w")) )
        {
            fprintf(stderr, "ovlload: can't open memory file '%s', be sure the directory and file have"
                " write permission.\n", memFileNameP);
            printf("The drum was written; the memory file was not.\n");
            exit(EXIT_ERROR);
        }

        written = tapeWriteMemFile(memFilefP);
        if( (fclose(memFilefP) != 0) || !written )
        {
            fprintf(stderr, "ovlload: can't write memory file '%s'.\n", memFileNameP);
            printf("The drum was written; the memory file is incomplete.\n");
            exit(EXIT_ERROR);
        }

        if( address == LOADSTOP )
        {
            printf("Tape loaded, an am1 program with a stop directive.\n");
        }
        else
        {
            printf("Tape loaded, start address 0%0o.\n", address);
        }

        exit(EXIT_DONE);
    }

    // The pdp-1 is stopped already, so sendImage() finds it halted; whether it was
    // running is what ad1GetState() said before the stop.
    if( !sendImage(&link, &stillRunning) )
    {
        ad1LinkClose(&link);
        printf("The drum was written; the program was not loaded.\n");
        exit(EXIT_ERROR);
    }

    if( wasRunning )
    {
        printf("The pdp-1 was running and has been halted.\n");
    }

    if( address == LOADSTOP )
    {
        printf("Tape loaded, an am1 program with a stop directive.\n");
        ad1LinkClose(&link);
        exit(EXIT_DONE);
    }

    if( !autoStart )
    {
        printf("Tape loaded, start address 0%0o. Start it (y or n/newline)?\n", address);
    }

    lineP = 0;
    len = 0;
    if( autoStart || ((getline(&lineP, &len, stdin)) != -1) )
    {
        if( autoStart || (*lineP == 'y') )
        {
            if( (status = ad1Start(&link, (uint32_t)address, NULL, NULL)) != AD1P_ST_OK )
            {
                fprintf(stderr, "ovlload: can't start the pdp-1: %s\n", ad1StatusText(status));
                ad1LinkClose(&link);
                exit(EXIT_ERROR);
            }

            printf("The pdp-1 program has been started.\n");
        }
    }

    ad1LinkClose(&link);
    exit(EXIT_DONE);
}

// Compare the drum with the .ovl: a read-only overlay word for word over its window,
// the padding included; a persistent one by its header only, since the overlay manager
// writes its words back.  Returns EXIT_DONE if all agree, else EXIT_DIFFERS.
static int
compareDrum(void)
{
int i;
int addr;
int32_t want;
int32_t got;
bool differs;
Overlay *ovP;

    differs = false;
    for( i = 0; i < ovl.count; ++i )
    {
        ovP = &ovl.overlaysP[i];
        if( ovlCompare(ovP, drum, &addr, &got, &want) != OVL_CURRENT )
        {
            printf("overlay %s differs: track %o, drum address %04o holds %06o, the file %06o\n",
                ovP->name, ovP->track, addr, got & OVL_WORDMASK, want);
            differs = true;
        }
        else
        {
            printf("overlay %s is current: track %o, drum %04o-%04o, %s\n", ovP->name, ovP->track,
                ovP->first, (ovP->persistent) ? (ovP->first + 1) : ovP->last,
                (ovP->persistent) ? "rw, header only" : "ro");
        }
    }

    return( (differs) ? EXIT_DIFFERS : EXIT_DONE );
}

// The program's tape, the .ovl with the same base name: .rim or .bin.
// Returns the name or NULL after saying neither exists.
static char *
tapeName(const char *ovlPathP)
{
char *nameP;
size_t base;

    base = strlen(ovlPathP);
    if( (base > 4) && (strcmp(ovlPathP + base - 4, ".ovl") == 0) )
    {
        base -= 4;
    }

    if( !(nameP = malloc(base + 5)) )
    {
        fprintf(stderr, "ovlload: out of memory.\n");
        return(NULL);
    }

    memcpy(nameP, ovlPathP, base);
    strcpy(nameP + base, ".rim");
    if( access(nameP, R_OK) == 0 )
    {
        return(nameP);
    }

    strcpy(nameP + base, ".bin");
    if( access(nameP, R_OK) == 0 )
    {
        return(nameP);
    }

    nameP[base] = 0;
    fprintf(stderr, "ovlload: there is no tape %s.rim or %s.bin; -t names the tape, -n loads the drum only.\n",
        nameP, nameP);
    free(nameP);
    return(NULL);
}

// Punch the tape for -p to pathP.
// It punches the leader and label, am1's loader in RIM format, the drum writer, the survey's track bits,
// each overlay's window and parameters, the end, and unless drumOnly the program's blocks from rootTapeP.
// Returns EXIT_DONE, or EXIT_ERROR after saying why.
static int
punchTape(const char *pathP, const char *rootTapeP, bool drumOnly)
{
int i;
int a;
int span;
long origin;
bool eem;
char label[OVL_NAMESIZE + 32];
const char *baseP;
size_t baseLen;
uint32_t words[OVL_TRACKWORDS];
uint32_t trackBits[2];
uint32_t params[5];
int32_t sum;
FILE *fP;
Overlay *ovP;

    if( !drumOnly && !readRootTape(rootTapeP) )
    {
        return(EXIT_ERROR);
    }

    // A bank-0 window over the loader's words would be loaded over the loader as it runs.
    for( i = 0; i < ovl.count; ++i )
    {
        if( (ovl.overlaysP[i].bank == 0) && (ovl.overlaysP[i].last >= LDR_START_ADDR) )
        {
            fprintf(stderr, "ovlload: the window %s, %04o-%04o, owrwrites the loader at %04o.  Nothing was"
                " punched.\n", ovl.overlaysP[i].window, ovl.overlaysP[i].first, ovl.overlaysP[i].last,
                LDR_START_ADDR);
            return(EXIT_ERROR);
        }
    }

    if( (origin = placeWriter()) < 0 )
    {
        fprintf(stderr, "ovlload: bank 0 has less than %o words free of the windows between %04o and %04o for the"
            " writer.  Nothing was punched.\n", OVW_SIZE, WRITER_LOWEST, WRITER_HIGHEST);
        return(EXIT_ERROR);
    }

    // The eem loader when anything loads above bank 0, as am1 chooses it for a program.
    eem = (!drumOnly && rootNeedsEem);
    for( i = 0; i < ovl.count; ++i )
    {
        if( ovl.overlaysP[i].bank != 0 )
        {
            eem = true;
        }
    }

    // The label is the .ovl's name, without its directory or .ovl.
    baseP = (strrchr(ovlNameP, '/')) ? (strrchr(ovlNameP, '/') + 1) : ovlNameP;
    baseLen = strlen(baseP);
    if( (baseLen > 4) && (strcmp(baseP + baseLen - 4, ".ovl") == 0) )
    {
        baseLen -= 4;
    }

    if( baseLen > OVL_NAMESIZE )
    {
        baseLen = OVL_NAMESIZE;
    }

    snprintf(label, sizeof(label), "%.*s overlays", (int)baseLen, baseP);

    punchBlank(5);
    punchLabel(label);
    punchBlank(5);
    for( i = 0; i < (int)(sizeof(xloader) / sizeof(xloader[0])); ++i )
    {
        punchWord(TAPE_DIO | (LDR_START_ADDR + i));
        punchWord(((i == 0) && !eem) ? LOADER_NOP : xloader[i]);
    }

    punchWord(TAPE_JMP | LDR_START_ADDR);
    punchBlank(2);

    // The writer, each word holding its address with it.
    for( i = 0; i < OVW_WORDS; ++i )
    {
        words[i] = ovwWords[i].word + ((ovwWords[i].address) ? (uint32_t)origin : 0);
    }

    punchBlock((uint32_t)origin, words, OVW_WORDS);

    // The survey: track n is bit 1 << n of the first word for tracks 0-17, of the second for 20-37.
    trackBits[0] = 0;
    trackBits[1] = 0;
    for( i = 0; i < ovl.count; ++i )
    {
        trackBits[ovl.overlaysP[i].track / 16] |= (1u << (ovl.overlaysP[i].track % 16));
    }

    punchBlock((uint32_t)(origin + OVW_TRACKS), trackBits, 2);
    punchWord(TAPE_START | (uint32_t)(origin + OVW_SURVEY));
    punchBlank(2);

    // Each overlay's window range, its words then zeros as on the drum, and its parameters,
    // the checksum the sum the writer makes with add.
    printf("Tape %s: the writer is at %04lo-%04lo, the %s loader.\n", pathP, origin, origin + OVW_SIZE - 1,
        (eem) ? "eem" : "nop");
    for( i = 0; i < ovl.count; ++i )
    {
        ovP = &ovl.overlaysP[i];
        span = (ovP->last - ovP->first) + 1;
        memset(words, 0, sizeof(words));
        memcpy(words, ovP->wordsP, ovP->length * sizeof(int32_t));
        sum = 0;
        for( a = 0; a < span; ++a )
        {
            sum = onesAdd(sum, (int32_t)words[a]);
        }

        params[0] = (uint32_t)((ovP->bank << 12) | ovP->first);
        params[1] = (uint32_t)ovP->track;
        params[2] = (uint32_t)ovP->first;
        params[3] = (uint32_t)span;
        params[4] = (uint32_t)sum;
        punchBlock(params[0], words, (uint32_t)span);
        punchBlock((uint32_t)(origin + OVW_CORE), params, 5);
        punchWord(TAPE_START | (uint32_t)(origin + OVW_WRITE));
        punchBlank(2);
        printf("overlay %s: track %o, drum %04o-%04o, %o words, %s, checksum %06o\n", ovP->name, ovP->track,
            ovP->first, ovP->last, ovP->length, (ovP->persistent) ? "rw" : "ro", (unsigned int)sum);
    }

    punchWord(TAPE_START | (uint32_t)(origin + ((drumOnly) ? OVW_END : OVW_END_RUN)));
    punchBlank(2);

    // The program, as its own tape loads it.
    if( !drumOnly )
    {
        for( i = 0; i < rootBlockCount; ++i )
        {
            punchBlock(rootBlocksP[i].address, rootBlocksP[i].wordsP, rootBlocksP[i].count);
        }

        punchWord(rootEndWord);
        punchBlank(2);
    }

    punchLabel("DONE");
    punchBlank(5);
    if( punchFailed )
    {
        fprintf(stderr, "ovlload: out of memory.  Nothing was punched.\n");
        return(EXIT_ERROR);
    }

    if( !(fP = fopen(pathP, "wb")) )
    {
        fprintf(stderr, "ovlload: can't open tape file '%s' to punch it: %s\n", pathP, strerror(errno));
        return(EXIT_ERROR);
    }

    if( (fwrite(punchP, 1, punchLength, fP) != punchLength) || (fclose(fP) != 0) )
    {
        fprintf(stderr, "ovlload: writing tape file '%s' failed: %s\n", pathP, strerror(errno));
        return(EXIT_ERROR);
    }

    if( drumOnly )
    {
        printf("Punched, read it in to load the drum, halts with AC 0 when the drum is loaded.\n");
    }
    else
    {
        printf("Punched, read it in and it will start the program when the drum is loaded.\n");
    }

    return(EXIT_DONE);
}

// Read the program's tape for -p into rootBlocksP: an am1 tape, with am1's loader or none,
// its blocks, and its start or stop word.
// Sets rootNeedsEem when the tape's loader is the eem one or a block lies above bank 0.
// Returns false for any error after printing an error message.
static bool
readRootTape(const char *pathP)
{
FILE *fP;
int in;
int frames;
uint32_t acc;
uint32_t word;
uint32_t rimAddress;
uint32_t got;
bool first;
bool inLoader;
bool rimData;
TapeBlock *grownP;
TapeBlock *blockP;

    if( !(fP = fopen(pathP, "rb")) )
    {
        fprintf(stderr, "ovlload: can't open the tape '%s': %s\n", pathP, strerror(errno));
        return(false);
    }

    // A word is three binary tape frames; the label's frames are not marked as binary.
    // The RIM part is pairs of a dio and the word it stores, ended by the jmp to the loader.
    // The word stored at the loader's first address is an eem or a nop, which says which
    // loader it is.
    rootNeedsEem = false;
    first = true;
    inLoader = false;
    rimData = false;
    rimAddress = 0;
    blockP = NULL;
    got = 0;
    acc = 0;
    frames = 0;
    while( (in = fgetc(fP)) != EOF )
    {
        if( !(in & 0200) )
        {
            continue;
        }

        acc = (acc << 6) | (uint32_t)(in & 077);
        if( ++frames < 3 )
        {
            continue;
        }

        word = acc & OVL_WORDMASK;
        acc = 0;
        frames = 0;
        if( first )
        {
            first = false;
            inLoader = (word == (TAPE_DIO | LDR_START_ADDR));
        }

        if( inLoader )
        {
            if( rimData )
            {
                if( rimAddress == LDR_START_ADDR )
                {
                    if( word == xloader[0] )
                    {
                        rootNeedsEem = true;
                    }
                    else if( word != LOADER_NOP )
                    {
                        break;      // not am1's loader
                    }
                }

                rimData = false;
            }
            else if( (word & 0770000) == TAPE_DIO )
            {
                rimAddress = word & 07777;
                rimData = true;
            }
            else if( word == (TAPE_JMP | LDR_START_ADDR) )
            {
                inLoader = false;
            }
            else
            {
                break;
            }

            continue;
        }

        // A block: the word past its end, then its words.
        if( blockP )
        {
            if( got == 0 )
            {
                if( (word <= blockP->address) || ((word - blockP->address) > (OVL_TRACKWORDS * 16)) )
                {
                    break;
                }

                blockP->count = word - blockP->address;
                if( !(blockP->wordsP = malloc(blockP->count * sizeof(uint32_t))) )
                {
                    fprintf(stderr, "ovlload: out of memory.\n");
                    fclose(fP);
                    return(false);
                }

                if( ((blockP->address | (word - 1)) & ~(uint32_t)07777) != 0 )
                {
                    rootNeedsEem = true;
                }
            }
            else
            {
                blockP->wordsP[got - 1] = word;
            }

            if( got++ == blockP->count )
            {
                blockP = NULL;
            }

            continue;
        }

        // A control word: a block's address, or the start or stop word that ends the tape.
        if( (word & TAPE_CONTROL) == 0 )
        {
            if( !(grownP = realloc(rootBlocksP, (rootBlockCount + 1) * sizeof(TapeBlock))) )
            {
                fprintf(stderr, "ovlload: out of memory.\n");
                fclose(fP);
                return(false);
            }

            rootBlocksP = grownP;
            blockP = &rootBlocksP[rootBlockCount++];
            blockP->address = word & TAPE_ADDRESS;
            blockP->count = 0;
            blockP->wordsP = NULL;
            got = 0;
            continue;
        }

        if( ((word & TAPE_CONTROL) == TAPE_START) || ((word & TAPE_CONTROL) == TAPE_STOP) )
        {
            rootEndWord = word;
            fclose(fP);
            return(true);
        }

        break;
    }

    fclose(fP);
    fprintf(stderr, "ovlload: the tape '%s' is not an am1 tape that ends with a start or stop; -p punches"
        " only am1's.  Nothing was punched.\n", pathP);
    return(false);
}

// The highest origin in bank 0, WRITER_LOWEST to WRITER_HIGHEST, where the writer covers no
// bank-0 window.
// Returns the origin, or -1 when no free run is long enough.
static long
placeWriter(void)
{
bool used[OVL_TRACKWORDS];
long origin;
long a;
int i;

    memset(used, 0, sizeof(used));
    for( i = 0; i < ovl.count; ++i )
    {
        if( ovl.overlaysP[i].bank == 0 )
        {
            for( a = ovl.overlaysP[i].first; a <= ovl.overlaysP[i].last; ++a )
            {
                used[a] = true;
            }
        }
    }

    for( origin = (WRITER_HIGHEST + 1) - OVW_SIZE; origin >= WRITER_LOWEST; --origin )
    {
        for( a = origin; (a < (origin + OVW_SIZE)) && !used[a]; ++a )
        {
        }

        if( a == (origin + OVW_SIZE) )
        {
            return(origin);
        }

        // The run from here holds a used word at a; no origin above it, down to it, fits.
        origin = a - OVW_SIZE + 1;
    }

    return(-1);
}

// a + b as the pdp-1's add makes it: ones' complement, the carry out of bit 0 added back in,
// and a result of minus zero made plus zero.
static int32_t
onesAdd(int32_t a, int32_t b)
{
int32_t sum;

    sum = (a & OVL_WORDMASK) + (b & OVL_WORDMASK);
    if( sum > OVL_WORDMASK )
    {
        sum = (sum & OVL_WORDMASK) + 1;
    }

    return( (sum == OVL_WORDMASK) ? 0 : sum );
}

// Add one frame to the tape.
// Returns false if out of memory, punchFailed is then set so
// that a tape left short is never written.
static bool
punchByte(int byte)
{
unsigned char *grownP;

    if( punchLength == punchCap )
    {
        if( !(grownP = realloc(punchP, (punchCap == 0) ? 65536 : (punchCap * 2))) )
        {
            punchFailed = true;
            return(false);
        }

        punchP = grownP;
        punchCap = (punchCap == 0) ? 65536 : (punchCap * 2);
    }

    punchP[punchLength++] = (unsigned char)byte;
    return(true);
}

// Punch a word as three binary frames, high six bits first, each with the binary marker bit set.
static bool
punchWord(uint32_t word)
{
    return( punchByte((int)(((word >> 12) & 077) | 0200)) && punchByte((int)(((word >> 6) & 077) | 0200)) &&
        punchByte((int)((word & 077) | 0200)) );
}

// Punch count blank frames.
static bool
punchBlank(int count)
{
    while( count-- > 0 )
    {
        if( !punchByte(0) )
        {
            return(false);
        }
    }

    return(true);
}

// Punch text readable on the tape as am1 punches its labels, each character five columns
// of its 5x7 font and a blank.
// A character outside the font is punched as '?'.
static bool
punchLabel(const char *textP)
{
int i;
int ch;

    for( ; *textP; ++textP )
    {
        ch = (unsigned char)*textP;
        if( (ch < 0x20) || (ch > 0x7f) )
        {
            ch = '?';
        }

        for( i = 0; i < 5; ++i )
        {
            if( !punchByte(Font5x7[((ch - 0x20) * 5) + i]) )
            {
                return(false);
            }
        }

        if( !punchByte(0) )
        {
            return(false);
        }
    }

    return(true);
}

// Punch one of the loader's blocks: the address, the address past the last word, the words,
// and two blank frames, as am1 does.
static bool
punchBlock(uint32_t address, const uint32_t *wordsP, uint32_t count)
{
uint32_t i;

    if( !punchWord(address) || !punchWord(address + count) )
    {
        return(false);
    }

    for( i = 0; i < count; ++i )
    {
        if( !punchWord(wordsP[i]) )
        {
            return(false);
        }
    }

    return(punchBlank(2));
}

// Print the usage text and exit with EXIT_ERROR.
static void
usage(void)
{
    fprintf(stderr, "Usage: ovlload [-i drumfile] [-c] [-n] [-t tape] [-h host[:port]]\n");
    fprintf(stderr, "    [-y] [-m] [-M memfile] file.ovl\n");
    fprintf(stderr, "    Puts the overlays of file.ovl on the drum, each on its track,\n");
    fprintf(stderr, "    then loads the program's tape as fastload does.\n");
    fprintf(stderr, "    The tape's stamp must match the .ovl, or nothing is written;\n");
    fprintf(stderr, "    with -n no tape is read, and the program checks the stamp when it loads an overlay.\n");
    fprintf(stderr, "    -i names the drum file, default %s.\n", DEFAULT_DRUM);
    fprintf(stderr, "    -c compares the drum with file.ovl and writes nothing; exit 0 current, 1 not.\n");
    fprintf(stderr, "    -n loads the drum only, not the program.\n");
    fprintf(stderr, "    -t names the tape, the default is file.rim or file.bin.\n");
    fprintf(stderr, "    -h names a pidp-1 on another machine, default localhost:1044.\n");
    fprintf(stderr, "    -y starts the program once it is loaded.\n");
    fprintf(stderr, "    -m loads the program into the coremem file instead, the pidp-1 must not be running.\n");
    fprintf(stderr, "    -M names the coremem file, default %s, and implies -m.\n", DEFAULT_MEMFILE);
    fprintf(stderr, "Usage: ovlload -p tapefile [-n | -t tape] file.ovl\n");
    fprintf(stderr, "    Punches a tape that a pdp-1 reads in to write the overlays to the drum,\n");
    fprintf(stderr, "    then loads the program and starts it.\n");
    fprintf(stderr, "    -n punches just the overlays and halts.\n");
    fprintf(stderr, "    -t names the tape, the default is file.rim or file.bin.\n");
    exit(EXIT_ERROR);
}
