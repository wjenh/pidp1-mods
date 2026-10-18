/*
 * ovlload -- put a program's drum overlays on the Type 23 drum and load the program.
 *
 * am1 assembles each overlay of a program to run in a core window and writes them all
 * to a text file, file.ovl.
 * It contains a heading whose stamp line gives the build stamp and the
 * full address of the program's overlay table.
 * Then for each overlay a line giving its window, bank, first and last address,
 * drum track, length and mode, followed by its code words,
 * the two header words, the overlay stamp and the overlays number included.s
 * All numberrs are octal.
 *
 * Each overlay goes on its track at drum addresses equal to its window's core
 * addresses and the rest of the window's range padded with zeros.
 * The overlay manager in the program loads it with one transfer.
 * Nothing else on the drum is touched.
 *
 * Unless -n is given, the program's tape is then loaded in the way as fastload does.
 * Before anything is written, the tape's* word at the overlay table's address must be the .ovl's stamp.
 * A drum and a program from different builds are never loaded together.
 *
 * A track holding something another tool put there is overwritten and
 * named in a warning.
 * The user is resposible for saving a drum image that is to be preserved.
 *
 * The .ovl is read and checked by ovlfile.c, which drumlist shares.
 * It is what identifies data written by other programs ovlfile recognizes.
 *
 * With -p, a paper tape is punched instead that can be directly read-in loaded.
 * It contains a loader that first loads the drum with the overlay code that is included, then
 * loads the main program and starts it.
 *
 * Usage: ovlload [-i drumfile] [-c] [-n] [-t tape] [-h host[:port]] [-y] [-m] [-M memfile] file.ovl
 *        ovlload -p tapefile [-n] [-t tape] file.ovl
 * Exit status: 0 done, or with -c the drum is current; 1 with -c the drum differs;
 * 2 an error, nothing having been written unless the message says so.
 *
 * 12-Oct-2026 wje/Claude initial version
 * 13-Oct-2026 Claude .ovl reader shared in ovlfile.c; other tools' tracks warned of; -f gone
 * 13-Oct-2026 Claude -p punches a tape the pdp-1 writes its own drum from, by ovlwriter.h
 * 18-Oct-2026 wje for a rim produced using -p, just start the program when it loads, no halting.
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

#include "../AD1/ad1link.h"
#include "../AM1/tapeimage.h"
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

static bool readDrum(const char *pathP);
static void warnOverwritten(void);
static int overlayNames(int track, char *namesP, size_t size);
static int compareDrum(void);
static bool writeDrum(const char *pathP);
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
uint32_t run;
uint32_t pc;
bool compare;
bool drumOnly;
bool useMemFile;
bool autoStart;
bool wasRunning;
bool stillRunning;
bool programOption;
bool drumOption;
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
            "-m and -M are not used.\n");
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

    ovlNameP = argv[optind];
    if( !ovlRead(ovlNameP, &ovl, err) )
    {
        fprintf(stderr, "ovlload: %s.  Nothing was written.\n", err);
        exit(EXIT_ERROR);
    }

    if( compare )
    {
        if( !readDrum(drumNameP) )
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

        if( (address = loadTape(tapeNameP, NULL, false)) == LOADFAILED )
        {
            fprintf(stderr, "ovlload: can't load the tape '%s', is it a valid am1 rim tape?\n", tapeNameP);
            exit(EXIT_ERROR);
        }

        if( !tapeImageWord((uint32_t)ovl.tableAddr, &stampWord) )
        {
            fprintf(stderr, "ovlload: the tape '%s' has no word at the overlay table's address %06o;"
                " it is not this program's tape.\n", tapeNameP, ovl.tableAddr);
            exit(EXIT_ERROR);
        }

        if( (int)stampWord != ovl.stamp )
        {
            fprintf(stderr, "ovlload: the tape's stamp %06o is not the .ovl's %06o: they are from different"
                " builds.  Nothing was written.\n", stampWord, ovl.stamp);
            exit(EXIT_ERROR);
        }
    }

    if( punchNameP )
    {
        exit(punchTape(punchNameP, tapeNameP, drumOnly));
    }

    if( !readDrum(drumNameP) )
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

        if( !(memFilefP = fopen(memFileNameP, "w")) )
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

        // Stopped first, so no running program uses the drum while it changes.
        run = 0;
        if( (status = ad1Stop(&link, &run, &pc)) != AD1P_ST_OK )
        {
            fprintf(stderr, "ovlload: can't stop the pdp-1: %s\n", ad1StatusText(status));
            ad1LinkClose(&link);
            exit(EXIT_ERROR);
        }

        wasRunning = (run != 0);
    }

    if( !writeDrum(drumNameP) )
    {
        if( memFilefP )
        {
            fclose(memFilefP);
            printf("The memory file was not written.\n");
        }
        else if( !drumOnly )
        {
            ad1LinkClose(&link);
            printf("The program was not loaded; the pdp-1 is stopped.\n");
        }

        exit(EXIT_ERROR);
    }

    warnOverwritten();

    if( drumOnly )
    {
        exit(EXIT_DONE);
    }

    // The program, as fastload loads it.
    if( memFilefP )
    {
        if( (address = loadTape(tapeNameP, memFilefP, true)) == LOADFAILED )
        {
            fclose(memFilefP);
            fprintf(stderr, "ovlload: can't load the tape, is it a valid macro or am1 rim tape?\n");
            printf("The drum was written; the memory file is incomplete.\n");
            exit(EXIT_ERROR);
        }

        fclose(memFilefP);
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
    // running is what ad1Stop() said.
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

// Read the drum file into drum[], words past its end zero.  A missing file leaves drum[]
// zero and drumFileWords -1.  Returns false, after saying why, if the file can't be read.
static bool
readDrum(const char *pathP)
{
int fd;
ssize_t got;
struct stat st;

    memset(drum, 0, sizeof(drum));
    if( (fd = open(pathP, O_RDONLY)) < 0 )
    {
        if( errno == ENOENT )
        {
            drumFileWords = -1;
            return(true);
        }

        fprintf(stderr, "ovlload: can't open drum file '%s': %s\n", pathP, strerror(errno));
        return(false);
    }

    if( (fstat(fd, &st) < 0) || ((got = read(fd, drum, sizeof(drum))) < 0) )
    {
        fprintf(stderr, "ovlload: can't read drum file '%s': %s\n", pathP, strerror(errno));
        close(fd);
        return(false);
    }

    drumFileWords = (long)(st.st_size / (off_t)sizeof(int32_t));
    close(fd);
    return(true);
}

// Warn of each track an overlay was written on that held another tool's data: a program
// drumupdater loaded, Adventure's save block, or part of a Z-machine story.  drum[] still
// holds the drum as it was before the write.
static void
warnOverwritten(void)
{
int t;
int zTracks;
char names[1024];

    zTracks = storyTracks(drum);
    for( t = 0; t < OVL_TRACKS; ++t )
    {
        if( !overlayNames(t, names, sizeof(names)) )
        {
            continue;
        }

        if( drumupdaterMarked(drum, t) )
        {
            fprintf(stderr, "ovlload: track %o (%d decimal) held a program drumupdater loaded; it was overwritten"
                " by %s.\n", t, t, names);
        }

        if( (t == ADVENTURE_SAVE_TRACK) && adventureSaved(drum) )
        {
            fprintf(stderr, "ovlload: track %o (%d decimal) held Adventure's save block; it was overwritten"
                " by %s.\n", t, t, names);
        }

        if( t < zTracks )
        {
            fprintf(stderr, "ovlload: track %o (%d decimal) held part of a Z-machine story, tracks 0-%o; it was"
                " overwritten by %s.\n", t, t, zTracks - 1, names);
        }
    }
}

// The overlays written on a track, as "overlay a" or "overlays a, b", in namesP.
// Returns how many there are.
static int
overlayNames(int track, char *namesP, size_t size)
{
int i;
int n;
int listed;
size_t used;

    n = 0;
    for( i = 0; i < ovl.count; ++i )
    {
        if( ovl.overlaysP[i].track == track )
        {
            ++n;
        }
    }

    // snprintf() gives the length it wanted, so a truncated list ends the loop.
    used = (size_t)snprintf(namesP, size, "%s", (n == 1) ? "overlay" : "overlays");
    listed = 0;
    for( i = 0; (i < ovl.count) && (used < size); ++i )
    {
        if( ovl.overlaysP[i].track == track )
        {
            used += (size_t)snprintf(namesP + used, size - used, "%s%s", (listed++ == 0) ? " " : ", ",
                ovl.overlaysP[i].name);
        }
    }

    return(n);
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

// Write each overlay's window range on its track: its words, then zeros to the window's
// end.  Returns false, after saying why and what was written, if a write fails.
static bool
writeDrum(const char *pathP)
{
int i;
int fd;
int span;
size_t bytes;
long end;
int32_t buffer[OVL_TRACKWORDS];
Overlay *ovP;

    if( (fd = open(pathP, O_RDWR | O_CREAT, 0666)) < 0 )
    {
        fprintf(stderr, "ovlload: can't open drum file '%s' to write it: %s\n", pathP, strerror(errno));
        return(false);
    }

    if( drumFileWords < 0 )
    {
        printf("Drum file %s created.\n", pathP);
    }

    end = 0;
    for( i = 0; i < ovl.count; ++i )
    {
        ovP = &ovl.overlaysP[i];
        span = (ovP->last - ovP->first) + 1;
        if( ((long)(ovP->track * OVL_TRACKWORDS) + ovP->last + 1) > end )
        {
            end = (long)(ovP->track * OVL_TRACKWORDS) + ovP->last + 1;
        }
        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer, ovP->wordsP, ovP->length * sizeof(int32_t));
        bytes = span * sizeof(int32_t);

        // Past the file's end, the write extends it and the gap reads as zeros.
        if( pwrite(fd, buffer, bytes, (off_t)((ovP->track * OVL_TRACKWORDS) + ovP->first) * sizeof(int32_t))
            != (ssize_t)bytes )
        {
            fprintf(stderr, "ovlload: writing overlay %s to '%s' failed: %s\n", ovP->name, pathP, strerror(errno));
            fprintf(stderr, "ovlload: %d of %d overlays were written before it.\n", i, ovl.count);
            close(fd);
            return(false);
        }

        printf("overlay %s: track %o, drum %04o-%04o, %o words, %s\n", ovP->name, ovP->track,
            ovP->first, ovP->last, ovP->length, (ovP->persistent) ? "rw" : "ro");
    }

    if( close(fd) != 0 )
    {
        fprintf(stderr, "ovlload: closing '%s' failed: %s\n", pathP, strerror(errno));
        return(false);
    }

    // Only a file that ended before the last overlay's window was extended; the file
    // then ends there, not at a whole drum.
    if( (drumFileWords >= 0) && (drumFileWords < end) )
    {
        printf("Drum file %s was %ld words long; the overlays past its end extended it with zeros.\n",
            pathP, drumFileWords);
    }

    return(true);
}

// The program's tape, beside the .ovl with the same base name: .rim, else .bin, as am1
// writes it.  Returns the name, or NULL after saying neither exists.
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
            fprintf(stderr, "ovlload: the window %s, %04o-%04o, reaches the tape's loader at %04o.  Nothing was"
                " punched.\n", ovl.overlaysP[i].window, ovl.overlaysP[i].first, ovl.overlaysP[i].last,
                LDR_START_ADDR);
            return(EXIT_ERROR);
        }
    }

    if( (origin = placeWriter()) < 0 )
    {
        fprintf(stderr, "ovlload: bank 0 has no %o words free of the windows between %04o and %04o for the"
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
    printf("Tape %s: the writer at %04lo-%04lo, the %s loader.\n", pathP, origin, origin + OVW_SIZE - 1,
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

    // A word is three binary tape charactersframes,the label portion is not marked as binary.
    // The RIM part is pairs of a dio and the word it stores ended by the jmp to the loader.
    // the word at the loader's first address, an eem or nop.
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
    fprintf(stderr, "    The tape's stamp must match the .ovl, or nothing is written.\n");
    fprintf(stderr, "    -i names the drum file, default %s.\n", DEFAULT_DRUM);
    fprintf(stderr, "    -c compares the drum with file.ovl and writes nothing; exit 0 current, 1 not.\n");
    fprintf(stderr, "    -n loads the drum only, not the program.\n");
    fprintf(stderr, "    -t names the tape, the default is file.rim or file.bin.\n");
    fprintf(stderr, "    -h names a pidp-1 on another machine, default localhost:1044.\n");
    fprintf(stderr, "    -y starts the program once it is loaded.\n");
    fprintf(stderr, "    -m loads the program into the coremem file instead, the pidp-1 must not be running.\n");
    fprintf(stderr, "    -M names the coremem file, default %s, and implies -m.\n", DEFAULT_MEMFILE);
    fprintf(stderr, "Usage: ovlload -p tapefile [-n] [-t tape] file.ovl\n");
    fprintf(stderr, "    Punches a tape that a pdp-1 reads in to write the overlays to the drum,\n");
    fprintf(stderr, "    then loads the program and starts it.\n");
    fprintf(stderr, "    -n punches the just the overlays and halts.\n");
    fprintf(stderr, "    -t names the tape, the default is file.rim or file.bin.\n");
    exit(EXIT_ERROR);
}
