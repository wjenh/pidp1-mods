/*
 * fstool: manage a file system on a Type 550 tape image or a Type 23 drum file.
 *
 * Usage: fstool [-D first,tracks] -l image
 *        fstool -D all -l drumfile
 *        fstool [-D first,tracks] -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F]
 *             -s source image
 *        fstool [-D first,tracks] -d -n name [-F] image
 *        fstool [-D first,tracks] -x -n name [-f bytes|text|words] -o output image
 *        fstool [-D first,tracks] -i [-F] image
 *        fstool [-D first,tracks] -p image
 * where:
 * -D first,tracks - the image is a drum file and the volume is its tracks first to
 *              first+tracks-1, both octal; without -D the image is a tape
 * -D all     - with -l only: list every volume on the drum file
 * -l         - list the directory
 * -a         - add the host file source to the tape as -n name
 * -d         - delete -n name
 * -x         - extract -n name to the output file
 * -i         - format: write an empty directory, creating the image if it is missing
 * -p         - pack: move the files down to close the gaps between them
 * -n name    - the file's name: 1 to 9 characters, no control character, no leading blank,
 *               matched without regard to case and kept in upper case
 * -t type    - the file's type, octal 0-7777, default 0
 * -r         - mark the file read-only
 * -b blocks  - the blocks to allocate, octal, at least the data's and at most 0777;
 *              defaults to the data's size,  one block for an empty file
 * -f form    - the host file's format:
 *                bytes (the default), two 8-bit bytes a word, the first in
 *                  the high half, as am1's ascii produces
 *                text, as bytes with each line end stored as CR (015), extracted back as LF
 *                words, each 18-bit word in a little-endian 32-bit word matching the PDP-1
 * -F         - force: add replaces a file of the same name, delete removes a read-only file,
 *                format overwrites another program's data in blocks 1 and 2
 *
 * The file system is the am1 library's in Am1Includes/FILESYSTEM, with the tape's layer in
 * Am1Includes/MICROTAPE and the drum's in Am1Includes/DRUM, see their README.md files.
 * Block 0 unused, the directory is in block 1 and a copy in block 2, the files from block 3,
 * each a run of contiguous blocks.
 * A tape or a drum volume this tool writes works with the library routines and vice-versa.
 *
 * A drum volume is a run of whole tracks, 16 blocks of 0400 words a track with no checksum,
 * skewed by one block a track; its directory records its first track, and a volume whose
 * directory records another is refused, as the library's drMount refuses it.
 *
 * The image is read completely, changed in memory, and written to a temporary file in its own
 * directory which is then renamed, so a failure leaves an existing file as it was.
 * A tape image grows only to the last block written.
 * A drum file keeps every word outside the volume's tracks and grows only to the end of the
 * last track written.
 * A read-only image is not written.
 *
 * The Microtape IOT mounts an image again when the file changes, and the drum reads its file
 * again at the first transfer after the file is replaced, but a program that has the tape or
 * the volume mounted keeps its own copy of the directory and its next directory write undoes
 * this tool's change.
 * A command that writes an image named in microtapes.txt, or the emulator's drum file, gives
 * a warning but continues.
 *
 * Exit status is 0 on success, 1 on any error.
 * An error gives the library's status code.
 * See Docs/UsingMtfs.md for more information.
 *
 * Revision history:
 *
 * 11-Nov-2026 Claude/wje initial version
 * 13-Nov-2026 Claude file system volumes on a Type 23 drum file, -D
 * 13-Nov-2026 wje rename, reorganize
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

// The emulator's list of mounted tape.
#ifndef MTFS_LIST
#define MTFS_LIST       "/opt/pidp1-mods/microtapes.txt"
#endif
#ifndef MTFS_TAPEDIR
#define MTFS_TAPEDIR    "/opt/pidp1-mods/Microtapes"    // where a relative name in the list is
#endif
// The emulator's drum file.
#ifndef MTFS_DRUM
#define MTFS_DRUM       "/opt/pidp1-mods/pdp23drum"
#endif

// Each tape block has a leading checksum, the data, and the trailing checksum, each word
// in a little-endian 32-bit word.
#define WDS             0400        // data words a block
#define BLKW            (WDS + 2)
#define BLKS            01100       // blocks on a tape
#define MASK            0777777
#define BLANKCK         MASK        // the leading checksum of a block, and of a blank one

// The drum file: 040 tracks of 010000 words, each in a little-endian 32-bit word, a file
// shorter than the drum reading as zeros past its end. A volume's track k (0 its first) holds
// its block j at ((j + k) mod 16) x 0400 (Am1Includes/DRUM/drum.ah).
#define DR_TRACKS       040
#define DR_TRACKW       010000
#define DR_WORDS        (DR_TRACKS * DR_TRACKW)
#define DR_BPT          16

// The file system's layout (Am1Includes/FILESYSTEM/fs.ah).
#define DIRBLK          1
#define DIRBLK2         2
#define FIRSTBLK        3
#define DMAGIC          0
#define DVER            1
#define DGEN            2
#define DBLKS           3
#define DDEV            4           // the device's words, the drum's first track and 3 zeros
#define DDEVW           4
#define MAGIC           0556446     // MTF in sixbit
#define VERSION         1
#define GENMASK         0377777     // the generation is kept positive
#define DENT0           010
#define ENTW            010
#define NENT            037
#define DEFIRST         3
#define DEALLOC         4
#define DELEN           5
#define DEINFO          6
#define DEMOVE          7
#define IRO             0400000
#define ITYPE           07777
#define IAPP            0370000
#define IAPPSHIFT       12
#define MAXBLKS         0777
#define NAMELEN         9

// The library's status codes the tool can duplicate.
#define E_OK            0
#define E_NOUNIT        1
#define E_LOCKED        3
#define E_BADBLOCK      5
#define E_NOFS          7
#define E_FOREIGN       010
#define E_NAME          011
#define E_EXISTS        012
#define E_NOTFOUND      013
#define E_DIRFULL       014
#define E_NOSPACE       015
#define E_RANGE         016
#define E_READONLY      017
#define E_LAST          023

#define SOURCE_MAX      (16 * 1024 * 1024)  // a host file larger than any tape can hold

enum form { FORM_BYTES, FORM_TEXT, FORM_WORDS };

static const char *errName[E_LAST + 1] = {
    "OK", "NOUNIT", "NOMEDIUM", "LOCKED", "HALTED", "BADBLOCK", "ENDZONE", "NOFS",
    "FOREIGN", "NAME", "EXISTS", "NOTFOUND", "DIRFULL", "NOSPACE", "RANGE", "READONLY",
    "BADFD", "SMALLBUF", "TIMEOUT", "XFER"
};
// Each %s is the device's noun, "tape" or "volume".
static const char *errMeaning[E_LAST + 1] = {
    "success",
    "the volume's tracks are not on the drum",
    "no tape on the drive",
    "the %s is write-locked",
    "a halt stopped the tape",
    "a block checksum failed",
    "the tape ran into an end zone",
    "the %s has no file system directory",
    "blocks 1 and 2 hold another program's data",
    "a name must be 1 to 9 characters with no control characters and no leading blank",
    "a file of that name is already on the %s",
    "no file of that name is on the %s",
    "every directory slot is in use",
    "no gap on the %s is large enough",
    "past the end of the %s or of the file's allocation",
    "the file is marked read-only",
    "the descriptor is not open",
    "the buffer is smaller than one block",
    "a search found no block mark",
    "a drum transfer was cut short"
};

static uint32_t image[BLKS][BLKW];
static uint32_t drum[DR_WORDS];     // the drum file's words, zero past its end
static bool trackWritten[DR_TRACKS];
static long drumWords;              // words in the drum file as read
static int imageBlks;               // blocks in the image file as read
static int topBlks;                 // blocks the image file will hold when written
static bool onDrum;                 // the image is a drum file
static bool drumScan;               // -D all: every volume on the drum file
static int drFirst;                 // the drum volume's first track
static int drTracks;                // its tracks
static int volBlks;                 // the volume's blocks, a tape's or 16 a drum track
static uint32_t *dirP;              // the directory's data words, in block DIRBLK
static int dirFrom;                 // the block the directory was read from
static const char *imageNameP;      // the image as named, for messages about the file
static const char *volNameP;        // the tape or the volume, for messages about its contents
static char volName[PATH_MAX + 32];
static const char *devNounP;        // "tape" or "volume"
static bool changed;                // the directory was written, so the image is to be

static int onesAdd(int a, int b);
static int blockTotal(const uint32_t *blkP);
static uint32_t *blockData(int blk);
static bool blockOk(int blk);
static bool blockBlank(int blk);
static void putBlock(int blk, const uint32_t *dataP);
static void copyBlock(int to, int from);
static bool dirOk(const uint32_t *dataP);
static bool readImage(const char *pathP, bool mayBeMissing);
static bool readDrum(const char *pathP, bool mayBeMissing);
static bool writeImage(const char *pathP);
static bool writeDrum(const char *pathP);
static bool replaceFile(const char *pathP, const uint8_t *bytesP, size_t len);
static bool mountDir(void);
static void writeDir(void);
static uint32_t *entry(int slot);
static int packName(const char *nameP, uint32_t *nmP);
static void unpackName(const uint32_t *nmP, char *outP);
static int lookup(const uint32_t *nmP, int *slotP, int *freeP);
static void freeSpace(int need, int *totalP, int *largestP, int *fitP);
static uint8_t *readHost(const char *pathP, size_t *lenP);
static uint32_t *sourceWords(const char *pathP, enum form f, long *countP);
static bool writeHost(const char *pathP, const uint32_t *wordsP, long count, enum form f);
static bool canonPath(const char *pathP, char *outP, size_t size);
static void warnIfListed(const char *pathP);
static void warnIfDrum(const char *pathP);
static int libError(int code, const char *nameP, const char *detailP);
static void parseVolume(const char *argP);
static int cmdList(void);
static int cmdScan(void);
static int cmdAdd(const char *nameP, int type, bool readOnly, int blocks, enum form f,
    bool force, const char *sourceP);
static int cmdDelete(const char *nameP, bool force);
static int cmdExtract(const char *nameP, enum form f, const char *outputP);
static int cmdFormat(bool force, bool existed);
static int cmdPack(void);
static long octalArg(const char *argP, char opt);
static void usage(void);

int
main(int argc, char **argv)
{
const char *nameP;
const char *sourceP;
const char *outputP;
const char *allowedP;
char seen[32];
char cmd;
enum form f;
long type;
long blocks;
bool readOnly;
bool force;
bool writes;
bool existed;
int status;
int opt;
int nSeen;
int i;

    cmd = 0;
    nameP = sourceP = outputP = NULL;
    f = FORM_BYTES;
    type = 0;
    blocks = -1;
    readOnly = force = false;
    nSeen = 0;
    seen[0] = '\0';
    while( (opt = getopt(argc, argv, "ladxipn:t:rb:f:Fs:o:D:")) != -1 )
    {
        if( opt == '?' )
        {
            usage();
        }

        if( (nSeen < ((int) sizeof(seen) - 1)) && !strchr(seen, opt) )
        {
            seen[nSeen++] = (char) opt;
        }
        seen[nSeen] = '\0';

        switch( opt )
        {
        case 'l':
        case 'a':
        case 'd':
        case 'x':
        case 'i':
        case 'p':
            if( cmd && (cmd != opt) )
            {
                fprintf(stderr, "fstool: only one of -l -a -d -x -i -p\n");
                usage();
            }
            cmd = (char) opt;
            break;

        case 'n':
            nameP = optarg;
            break;

        case 't':
            type = octalArg(optarg, 't');
            if( type > ITYPE )
            {
                fprintf(stderr, "fstool: -t %s: the type is octal 0-7777\n", optarg);
                exit(1);
            }
            break;

        case 'r':
            readOnly = true;
            break;

        case 'b':
            // Held below int's range; anything past 0777 is refused as RANGE later.
            blocks = octalArg(optarg, 'b');
            if( blocks > 07777777 )
            {
                blocks = 07777777;
            }
            break;

        case 'f':
            if( strcmp(optarg, "bytes") == 0 )
            {
                f = FORM_BYTES;
            }
            else if( strcmp(optarg, "text") == 0 )
            {
                f = FORM_TEXT;
            }
            else if( strcmp(optarg, "words") == 0 )
            {
                f = FORM_WORDS;
            }
            else
            {
                fprintf(stderr, "fstool: -f %s: the form is bytes, text or words\n", optarg);
                exit(1);
            }
            break;

        case 'F':
            force = true;
            break;

        case 's':
            sourceP = optarg;
            break;

        case 'o':
            outputP = optarg;
            break;

        case 'D':
            parseVolume(optarg);
            break;

        default:
            usage();
        }
    }

    if( !cmd || (optind != (argc - 1)) )
    {
        usage();
    }
    imageNameP = argv[optind];
    volNameP = imageNameP;
    devNounP = "tape";
    volBlks = BLKS;
    if( onDrum && !drumScan )
    {
        devNounP = "volume";
        volBlks = (drTracks * DR_BPT);
        if( drTracks == 1 )
        {
            snprintf(volName, sizeof(volName), "%s track %03o", imageNameP, drFirst);
        }
        else
        {
            snprintf(volName, sizeof(volName), "%s tracks %03o-%03o", imageNameP, drFirst,
                ((drFirst + drTracks) - 1));
        }
        volNameP = volName;
    }

    // Each command takes only its own options, and its required ones; -D goes with any.
    switch( cmd )
    {
    case 'a':
        allowedP = "antrbfFsD";
        break;

    case 'd':
        allowedP = "dnFD";
        break;

    case 'x':
        allowedP = "xnfoD";
        break;

    case 'i':
        allowedP = "iFD";
        break;

    default:
        allowedP = (cmd == 'l') ? "lD" : "pD";
        break;
    }
    for( i = 0; i < nSeen; i++ )
    {
        if( !strchr(allowedP, seen[i]) )
        {
            fprintf(stderr, "fstool: -%c cannot be used with -%c\n", seen[i], cmd);
            usage();
        }
    }
    if( (strchr("adx", cmd) && !nameP) || ((cmd == 'a') && !sourceP)
        || ((cmd == 'x') && !outputP) )
    {
        usage();
    }
    if( drumScan && (cmd != 'l') )
    {
        fprintf(stderr, "fstool: -D all is only for -l; the other commands name a volume, -D first,tracks\n");
        usage();
    }

    // The image: a missing one is a blank tape or drum only to format.
    writes = (strchr("adip", cmd) != NULL);
    existed = (access(imageNameP, F_OK) == 0);
    if( !(onDrum ? readDrum(imageNameP, (cmd == 'i')) : readImage(imageNameP, (cmd == 'i'))) )
    {
        return(1);
    }
    if( writes && existed && (access(imageNameP, W_OK) != 0) )
    {
        return(libError(E_LOCKED, NULL, (onDrum ? "the drum file is read-only"
            : "the image file is read-only")));
    }

    switch( cmd )
    {
    case 'l':
        status = (drumScan ? cmdScan() : cmdList());
        break;

    case 'a':
        status = cmdAdd(nameP, (int) type, readOnly, (int) blocks, f, force, sourceP);
        break;

    case 'd':
        status = cmdDelete(nameP, force);
        break;

    case 'x':
        status = cmdExtract(nameP, f, outputP);
        break;

    case 'i':
        status = cmdFormat(force, existed);
        break;

    default:
        status = cmdPack();
        break;
    }

    if( (status == 0) && writes && changed )
    {
        if( onDrum )
        {
            warnIfDrum(imageNameP);
        }
        else
        {
            warnIfListed(imageNameP);
        }
        if( !(onDrum ? writeDrum(imageNameP) : writeImage(imageNameP)) )
        {
            status = 1;
        }
    }

    return(status);
}

// 18-bit ones' complement addition, the end-around carry kept, as the control sums a block.
static int
onesAdd(int a, int b)
{
int s;

    s = (a + b);
    if( s > MASK )
    {
        s = ((s & MASK) + 1);
    }

    return(s);
}

// The ones' complement sum of a block's words, checksums included.
static int
blockTotal(const uint32_t *blkP)
{
int s;
int i;

    s = 0;
    for( i = 0; i < BLKW; i++ )
    {
        s = onesAdd(s, (int) blkP[i]);
    }

    return(s);
}

// A volume block's 0400 data words: on a tape past its leading checksum, on the drum where
// the skewed layout puts it.
static uint32_t *
blockData(int blk)
{
int k;

    if( !onDrum )
    {
        return(&image[blk][1]);
    }
    k = (blk / DR_BPT);

    return(&drum[((drFirst + k) * DR_TRACKW) + ((((blk % DR_BPT) + k) % DR_BPT) * WDS)]);
}

// Whether a block totals zero, +0 or -0, as the control checks it on a read.
// A drum block has no checksum and is always good.
static bool
blockOk(int blk)
{
int s;

    if( onDrum )
    {
        return(true);
    }
    s = blockTotal(image[blk]);

    return((s == 0) || (s == MASK));
}

// Whether a block is blank as the plugin keeps one: the leading checksum -0, the rest zero.
static bool
blockBlank(int blk)
{
int i;

    if( image[blk][0] != BLANKCK )
    {
        return(false);
    }
    for( i = 1; i < BLKW; i++ )
    {
        if( image[blk][i] != 0 )
        {
            return(false);
        }
    }

    return(true);
}

// Write a block as the control writes one forward: -0 leading, the data, then the complement
// of their sum, so the block totals zero. A drum block is its data alone.
static void
putBlock(int blk, const uint32_t *dataP)
{
    if( onDrum )
    {
        memcpy(blockData(blk), dataP, (WDS * sizeof(uint32_t)));
        trackWritten[drFirst + (blk / DR_BPT)] = true;
        return;
    }
    image[blk][0] = BLANKCK;
    memcpy(&image[blk][1], dataP, (WDS * sizeof(uint32_t)));
    image[blk][BLKW - 1] = 0;
    image[blk][BLKW - 1] = (((uint32_t) blockTotal(image[blk])) ^ MASK);
    if( blk >= topBlks )
    {
        topBlks = (blk + 1);
    }
}

// Copy a block whole, its checksums with it. A blank block past the image's end stays out of
// the image, as an unwritten one reads the same.
static void
copyBlock(int to, int from)
{
    if( onDrum )
    {
        putBlock(to, blockData(from));
        return;
    }
    memmove(image[to], image[from], sizeof(image[0]));
    if( (to >= topBlks) && !blockBlank(to) )
    {
        topBlks = (to + 1);
    }
}

// Whether a directory block's data is this file system's: the magic, the version and the
// volume's blocks, as the library's fsDirOk checks them.
static bool
dirOk(const uint32_t *dataP)
{
    return((dataP[DMAGIC] == MAGIC) && (dataP[DVER] == VERSION) && (dataP[DBLKS] == (uint32_t) volBlks));
}

// Read the image into memory, every block past its end blank. A missing image is a blank tape
// when mayBeMissing. Returns false, with a message, when it cannot be read or is no tape image.
static bool
readImage(const char *pathP, bool mayBeMissing)
{
uint8_t buf[BLKW * 4];
struct stat st;
FILE *fP;
int blk;
int i;

    for( blk = 0; blk < BLKS; blk++ )
    {
        image[blk][0] = BLANKCK;
        memset(&image[blk][1], 0, ((BLKW - 1) * sizeof(uint32_t)));
    }
    imageBlks = topBlks = 0;

    if( !(fP = fopen(pathP, "rb")) )
    {
        if( (errno == ENOENT) && mayBeMissing )
        {
            return(true);
        }
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
        return(false);
    }

    // The plugin writes whole blocks only, and no more than a tape's.
    if( (fstat(fileno(fP), &st) != 0) || !S_ISREG(st.st_mode) || ((st.st_size % sizeof(buf)) != 0) ||
        ((st.st_size / (off_t) sizeof(buf)) > BLKS) )
    {
        fprintf(stderr, "fstool: %s: not a tape image, a whole number of %d-byte blocks, at most %#o\n",
            pathP, (int) sizeof(buf), BLKS);
        fclose(fP);
        return(false);
    }

    imageBlks = (int) (st.st_size / (off_t) sizeof(buf));
    for( blk = 0; blk < imageBlks; blk++ )
    {
        if( fread(buf, 1, sizeof(buf), fP) != sizeof(buf) )
        {
            fprintf(stderr, "fstool: %s: %s\n", pathP, ferror(fP) ? strerror(errno) : "short read");
            fclose(fP);
            return(false);
        }
        for( i = 0; i < BLKW; i++ )
        {
            image[blk][i] = ((uint32_t) buf[4 * i] | ((uint32_t) buf[(4 * i) + 1] << 8)
                | ((uint32_t) buf[(4 * i) + 2] << 16) | ((uint32_t) buf[(4 * i) + 3] << 24));
            if( image[blk][i] > MASK )
            {
                fprintf(stderr,
                    "fstool: %s: not a tape image (block %#o word %#o is %#o, more than 18 bits)\n",
                    pathP, blk, i, image[blk][i]);
                fclose(fP);
                return(false);
            }
        }
    }
    fclose(fP);
    topBlks = imageBlks;

    return(true);
}

// Read the drum file into memory, every word past its end zero. A missing file is a blank drum
// when mayBeMissing. Returns false, with a message, when it cannot be read, is no drum file, or
// holds a word of more than 18 bits on the volume's tracks.
static bool
readDrum(const char *pathP, bool mayBeMissing)
{
uint8_t *bufP;
struct stat st;
FILE *fP;
long i;
long first;
long end;

    memset(drum, 0, sizeof(drum));
    drumWords = 0;

    if( !(fP = fopen(pathP, "rb")) )
    {
        if( (errno == ENOENT) && mayBeMissing )
        {
            return(true);
        }
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
        return(false);
    }

    // The emulator writes whole words, and no more than the drum's.
    if( (fstat(fileno(fP), &st) != 0) || !S_ISREG(st.st_mode) || ((st.st_size % 4) != 0) ||
        ((st.st_size / 4) > DR_WORDS) )
    {
        fprintf(stderr, "fstool: %s: not a drum file, a whole number of 4-byte words, at most %#o\n",
            pathP, DR_WORDS);
        fclose(fP);
        return(false);
    }

    drumWords = (long) (st.st_size / 4);
    if( !(bufP = malloc((size_t) st.st_size + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        fclose(fP);
        return(false);
    }
    if( fread(bufP, 1, (size_t) st.st_size, fP) != (size_t) st.st_size )
    {
        fprintf(stderr, "fstool: %s: %s\n", pathP, ferror(fP) ? strerror(errno) : "short read");
        free(bufP);
        fclose(fP);
        return(false);
    }
    fclose(fP);
    for( i = 0; i < drumWords; i++ )
    {
        drum[i] = ((uint32_t) bufP[4 * i] | ((uint32_t) bufP[(4 * i) + 1] << 8)
            | ((uint32_t) bufP[(4 * i) + 2] << 16) | ((uint32_t) bufP[(4 * i) + 3] << 24));
    }
    free(bufP);

    // Another program's tracks are kept as they are, whatever they hold; only the volume's
    // words are this file system's.
    if( !drumScan )
    {
        first = ((long) drFirst * DR_TRACKW);
        end = ((long) (drFirst + drTracks) * DR_TRACKW);
        for( i = first; i < end; i++ )
        {
            if( drum[i] > MASK )
            {
                fprintf(stderr,
                    "fstool: %s: not a drum file (track %#o word %#o is %#o, more than 18 bits)\n",
                    pathP, (int) (i / DR_TRACKW), (int) (i % DR_TRACKW), drum[i]);
                return(false);
            }
        }
    }

    return(true);
}

// Write the image's topBlks blocks over the image file.
// Returns false with a message on any failure.
static bool
writeImage(const char *pathP)
{
uint8_t *bufP;
size_t n;
int blk;
int i;
bool ok;

    if( !(bufP = malloc(((size_t) topBlks * BLKW * 4) + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(false);
    }
    n = 0;
    for( blk = 0; blk < topBlks; blk++ )
    {
        for( i = 0; i < BLKW; i++ )
        {
            bufP[n++] = (uint8_t) (image[blk][i] & 0377);
            bufP[n++] = (uint8_t) ((image[blk][i] >> 8) & 0377);
            bufP[n++] = (uint8_t) ((image[blk][i] >> 16) & 0377);
            bufP[n++] = 0;
        }
    }
    ok = replaceFile(pathP, bufP, n);
    free(bufP);

    return(ok);
}

// Write the drum over the drum file: every word as read, the volume's changed, the file grown
// only to the end of the last track written.
// Returns false with a message on any failure.
static bool
writeDrum(const char *pathP)
{
uint8_t *bufP;
long words;
long i;
int t;
bool ok;

    words = drumWords;
    for( t = 0; t < DR_TRACKS; t++ )
    {
        if( trackWritten[t] && (((long) (t + 1) * DR_TRACKW) > words) )
        {
            words = ((long) (t + 1) * DR_TRACKW);
        }
    }
    if( !(bufP = malloc(((size_t) words * 4) + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(false);
    }

    // All 32 bits, so a word another program left wider than 18 bits is written back as it was.
    for( i = 0; i < words; i++ )
    {
        bufP[4 * i] = (uint8_t) (drum[i] & 0377);
        bufP[(4 * i) + 1] = (uint8_t) ((drum[i] >> 8) & 0377);
        bufP[(4 * i) + 2] = (uint8_t) ((drum[i] >> 16) & 0377);
        bufP[(4 * i) + 3] = (uint8_t) ((drum[i] >> 24) & 0377);
    }
    ok = replaceFile(pathP, bufP, ((size_t) words * 4));
    free(bufP);

    return(ok);
}

// Write len bytes to a temporary file in the file's directory and rename it to the given name,
// keeping its mode.
// A failure leaves the old file intact.
// Returns false with a message on any failure.
static bool
replaceFile(const char *pathP, const uint8_t *bytesP, size_t len)
{
char tmp[PATH_MAX + 16];
struct stat st;
mode_t mode;
mode_t mask;
FILE *fP;
int fd;
bool ok;

    if( stat(pathP, &st) == 0 )
    {
        mode = (st.st_mode & 07777);
    }
    else
    {
        mask = umask(0);
        umask(mask);
        mode = (0666 & ~mask);
    }

    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", pathP);
    if( (fd = mkstemp(tmp)) < 0 )
    {
        fprintf(stderr, "fstool: %s: can't create temporary file: %s\n", pathP, strerror(errno));
        return(false);
    }
    if( !(fP = fdopen(fd, "wb")) )
    {
        fprintf(stderr, "fstool: %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return(false);
    }

    ok = (fwrite(bytesP, 1, len, fP) == len);
    ok = (ok && (fflush(fP) == 0) && (fchmod(fd, mode) == 0) && (fsync(fd) == 0));
    ok = ((fclose(fP) == 0) && ok);

    if( !ok || (rename(tmp, pathP) != 0) )
    {
        fprintf(stderr, "fstool: %s: can't write %s %s\n", pathP, (onDrum ? "drum file" : "image"),
            strerror(errno));
        unlink(tmp);
        return(false);
    }

    return(true);
}

// Find the directory as the library's mount does: block 1 if it totals zero and is the file
// system's, else block 2, copied into block 1's place in memory. On the drum, a directory
// whose recorded first track is not the volume's is refused, as drMount refuses it.
// Returns true, or false after reporting NOFS.
static bool
mountDir(void)
{
char detail[160];
uint32_t *dP;
int blk;

    for( blk = DIRBLK; blk <= DIRBLK2; blk++ )
    {
        if( blockOk(blk) && dirOk(blockData(blk)) )
        {
            dirFrom = blk;
            if( blk != DIRBLK )
            {
                memcpy(blockData(DIRBLK), blockData(blk), (WDS * sizeof(uint32_t)));
            }
            dirP = blockData(DIRBLK);
            if( onDrum && (dirP[DDEV] != (uint32_t) drFirst) )
            {
                snprintf(detail, sizeof(detail), "the directory records the volume's first track as "
                    "%#o, not %#o", dirP[DDEV], drFirst);
                libError(E_NOFS, NULL, detail);
                return(false);
            }
            return(true);
        }
    }

    // On the drum, a directory of another size says which tracks to name.
    dP = blockData(DIRBLK);
    if( onDrum && (dP[DMAGIC] == MAGIC) && (dP[DVER] == VERSION) && (dP[DBLKS] != 0)
        && ((dP[DBLKS] % DR_BPT) == 0) )
    {
        snprintf(detail, sizeof(detail), "the directory in block 1 is for %#o blocks, %#o tracks, "
            "not %#o", dP[DBLKS], (dP[DBLKS] / DR_BPT), drTracks);
        libError(E_NOFS, NULL, detail);
        return(false);
    }
    libError(E_NOFS, NULL, NULL);

    return(false);
}

// Write the directory as both copies, blocks 1 then 2, increasing the generation, as the
// library's fsWrDir does.
static void
writeDir(void)
{
uint32_t data[WDS];

    dirP[DGEN] = ((dirP[DGEN] + 1) & GENMASK);
    memcpy(data, dirP, sizeof(data));
    putBlock(DIRBLK, data);
    putBlock(DIRBLK2, data);
    dirP = blockData(DIRBLK);
    changed = true;
}

// The directory entry of a slot, 0 to NENT-1.
static uint32_t *
entry(int slot)
{
    return(&dirP[DENT0 + (ENTW * slot)]);
}

// Pack a name into 3 sixbit words, blank-padded, as the library's am1ToSixbit and nmPack do:
// 0140-0177 by clearing the 040 bit, control characters refused, no leading blank.
// Returns E_OK or E_NAME.
static int
packName(const char *nameP, uint32_t *nmP)
{
size_t len;
size_t i;
int c;
int six[NAMELEN];

    len = strlen(nameP);
    if( (len == 0) || (len > NAMELEN) )
    {
        return(E_NAME);
    }
    for( i = 0; i < NAMELEN; i++ )
    {
        six[i] = 0;
        if( i < len )
        {
            c = (unsigned char) nameP[i];
            if( (c >= 0140) && (c <= 0177) )
            {
                c &= ~040;
            }
            if( (c < 040) || (c >= 0140) )
            {
                return(E_NAME);
            }
            six[i] = (c - 040);
        }
    }
    if( six[0] == 0 )
    {
        return(E_NAME);
    }
    for( i = 0; i < 3; i++ )
    {
        nmP[i] = (uint32_t) ((six[3 * i] << 12) | (six[(3 * i) + 1] << 6) | six[(3 * i) + 2]);
    }

    return(E_OK);
}

// Unpack 3 sixbit words to a string of at most 9 characters, trailing blanks dropped.
static void
unpackName(const uint32_t *nmP, char *outP)
{
int i;
int n;

    for( i = 0; i < NAMELEN; i++ )
    {
        outP[i] = (char) (((nmP[i / 3] >> (12 - (6 * (i % 3)))) & 077) + 040);
    }
    n = NAMELEN;
    while( (n > 0) && (outP[n - 1] == ' ') )
    {
        n--;
    }
    outP[n] = '\0';
}

// Look a packed name up, as the library's mtfLookup does: a slot whose first name word is 0
// is free. Returns E_OK with *slotP the slot, or E_NOTFOUND with *freeP the first free slot,
// -1 when there is none.
static int
lookup(const uint32_t *nmP, int *slotP, int *freeP)
{
uint32_t *eP;
int slot;

    *freeP = -1;
    for( slot = 0; slot < NENT; slot++ )
    {
        eP = entry(slot);
        if( eP[0] == 0 )
        {
            if( *freeP < 0 )
            {
                *freeP = slot;
            }
        }
        else if( (eP[0] == nmP[0]) && (eP[1] == nmP[1]) && (eP[2] == nmP[2]) )
        {
            *slotP = slot;
            return(E_OK);
        }
    }

    return(E_NOTFOUND);
}

// The free blocks as the library's mtfFree walks them: the gaps from block 3 between the
// files in order of first block. Sets the total, the largest gap and the first gap of need
// blocks, -1 when none is large enough or need is 0.
static void
freeSpace(int need, int *totalP, int *largestP, int *fitP)
{
uint32_t *eP;
int pos;
int first;
int alloc;
int gap;
int iter;
int slot;

    *totalP = *largestP = 0;
    *fitP = -1;
    pos = FIRSTBLK;
    for( iter = 0; iter <= NENT; iter++ )
    {
        // The file with the lowest first block at or past pos.
        first = volBlks;
        alloc = 0;
        for( slot = 0; slot < NENT; slot++ )
        {
            eP = entry(slot);
            if( (eP[0] != 0) && ((int) eP[DEFIRST] >= pos) && ((int) eP[DEFIRST] < first) )
            {
                first = (int) eP[DEFIRST];
                alloc = (int) eP[DEALLOC];
            }
        }

        gap = (first - pos);
        *totalP += gap;
        if( gap > *largestP )
        {
            *largestP = gap;
        }
        if( (*fitP < 0) && (need > 0) && (gap >= need) )
        {
            *fitP = pos;
        }
        if( first == volBlks )
        {
            break;
        }
        pos = (first + alloc);
    }
}

// Read a host file completely.
// Returns its bytes, which the caller frees, with *lenP their count, or NULL with a message.
static uint8_t *
readHost(const char *pathP, size_t *lenP)
{
uint8_t *bufP;
FILE *fP;
size_t n;

    if( !(fP = fopen(pathP, "rb")) )
    {
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
        return(NULL);
    }
    if( !(bufP = malloc(SOURCE_MAX + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        fclose(fP);
        return(NULL);
    }
    n = fread(bufP, 1, (SOURCE_MAX + 1), fP);
    if( ferror(fP) )
    {
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
        free(bufP);
        fclose(fP);
        return(NULL);
    }
    fclose(fP);
    *lenP = n;

    return(bufP);
}

// The source file's words in the form given. Returns them, which the caller frees, with
// *countP their count, or NULL with a message. A file too large for a file on tape is RANGE.
static uint32_t *
sourceWords(const char *pathP, enum form f, long *countP)
{
uint8_t *bytesP;
uint32_t *wordsP;
size_t len;
size_t i;
size_t n;
long count;
uint32_t w;

    if( !(bytesP = readHost(pathP, &len)) )
    {
        return(NULL);
    }

    // A line end is the PDP-1's CR; a host CR LF is one line end.
    if( f == FORM_TEXT )
    {
        for( i = n = 0; i < len; i++ )
        {
            if( (bytesP[i] == '\r') && ((i + 1) < len) && (bytesP[i + 1] == '\n') )
            {
                continue;
            }
            bytesP[n++] = ((bytesP[i] == '\n') ? 015 : bytesP[i]);
        }
        len = n;
    }

    if( f == FORM_WORDS )
    {
        if( (len % 4) != 0 )
        {
            fprintf(stderr, "fstool: %s: a word file must be a whole number of 4-byte words\n", pathP);
            free(bytesP);
            return(NULL);
        }
        count = (long) (len / 4);
    }
    else
    {
        count = (long) ((len + 1) / 2);
    }
    if( count > ((long) MAXBLKS * WDS) )
    {
        libError(E_RANGE, NULL, "the source is larger than a file's 0777 maximum blocks");
        free(bytesP);
        return(NULL);
    }

    if( !(wordsP = calloc((size_t) (count + 1), sizeof(uint32_t))) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        free(bytesP);
        return(NULL);
    }
    for( i = 0; i < (size_t) count; i++ )
    {
        if( f == FORM_WORDS )
        {
            w = ((uint32_t) bytesP[4 * i] | ((uint32_t) bytesP[(4 * i) + 1] << 8)
                | ((uint32_t) bytesP[(4 * i) + 2] << 16) | ((uint32_t) bytesP[(4 * i) + 3] << 24));
            if( w > MASK )
            {
                fprintf(stderr, "fstool: %s: word %#lo is %#o, larger than 18 bits\n", pathP,
                    (unsigned long) i, w);
                free(bytesP);
                free(wordsP);
                return(NULL);
            }
            wordsP[i] = w;
        }
        else
        {
            // The first byte in the high half, as am1's ascii packs characters.
            w = ((uint32_t) bytesP[2 * i] << 9);
            if( ((2 * i) + 1) < len )
            {
                w |= bytesP[(2 * i) + 1];
            }
            wordsP[i] = w;
        }
    }
    free(bytesP);
    *countP = count;

    return(wordsP);
}

// Write a file's words to a host file in the form given. Returns false, with a message, on a
// failure.
static bool
writeHost(const char *pathP, const uint32_t *wordsP, long count, enum form f)
{
uint8_t *bufP;
FILE *fP;
size_t n;
long i;
int half;
int h;
int last;
bool wide;
bool ok;

    if( !(bufP = malloc((size_t) ((count * 4) + 1))) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(false);
    }

    n = 0;
    wide = false;
    if( f == FORM_WORDS )
    {
        for( i = 0; i < count; i++ )
        {
            bufP[n++] = (uint8_t) (wordsP[i] & 0377);
            bufP[n++] = (uint8_t) ((wordsP[i] >> 8) & 0377);
            bufP[n++] = (uint8_t) ((wordsP[i] >> 16) & 0377);
            bufP[n++] = 0;
        }
    }
    else
    {
        // Text stops at its trailing zero halves: an odd last character's pad.
        last = (int) (count * 2);
        if( f == FORM_TEXT )
        {
            while( (last > 0) && (((wordsP[(last - 1) / 2] >> ((last & 1) ? 9 : 0)) & 0777) == 0) )
            {
                last--;
            }
        }
        for( h = 0; h < last; h++ )
        {
            half = (int) ((wordsP[h / 2] >> ((h & 1) ? 0 : 9)) & 0777);
            if( half > 0377 )
            {
                wide = true;
            }
            half &= 0377;
            if( (f == FORM_TEXT) && (half == 015) )
            {
                half = '\n';
            }
            bufP[n++] = (uint8_t) half;
        }
    }
    if( wide )
    {
        fprintf(stderr, "fstool: %s: warning: a half word greater than 8 bits was written as its low 8 "
            "bits; -f words keeps all bits\n", pathP);
    }

    if( !(fP = fopen(pathP, "wb")) )
    {
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
        free(bufP);
        return(false);
    }
    ok = (fwrite(bufP, 1, n, fP) == n);
    ok = ((fclose(fP) == 0) && ok);
    free(bufP);
    if( !ok )
    {
        fprintf(stderr, "fstool: %s: %s\n", pathP, strerror(errno));
    }

    return(ok);
}

// A path made absolute with its directory's links resolved, for comparing two names of one
// file; the file itself need not exist.
// Returns false when its directory cannot be resolved.
static bool
canonPath(const char *pathP, char *outP, size_t size)
{
char dirCopy[PATH_MAX];
char baseCopy[PATH_MAX];
char dirReal[PATH_MAX];

    if( realpath(pathP, dirReal) )
    {
        snprintf(outP, size, "%s", dirReal);
        return(true);
    }
    snprintf(dirCopy, sizeof(dirCopy), "%s", pathP);
    snprintf(baseCopy, sizeof(baseCopy), "%s", pathP);
    if( !realpath(dirname(dirCopy), dirReal) )
    {
        return(false);
    }
    snprintf(outP, size, "%s/%s", dirReal, basename(baseCopy));

    return(true);
}

// Warn when the image is named in the emulator's list of mounted tapes: a program holding it
// mounted keeps its own directory, and its next directory write would undo this change.
static void
warnIfListed(const char *pathP)
{
char line[PATH_MAX + 64];
char listed[PATH_MAX + 64];
char mine[(2 * PATH_MAX) + 2];
char theirs[(2 * PATH_MAX) + 2];
const char *suffix;
char *cP;
FILE *fP;
size_t len;
int drive;

    if( !canonPath(pathP, mine, sizeof(mine)) || !(fP = fopen(MTFS_LIST, "r")) )
    {
        return;
    }

    suffix = ",locked";
    while( fgets(line, sizeof(line), fP) )
    {
        // A line is the drive, blanks, then the image's path to the end of the line.
        cP = line;
        while( (*cP == ' ') || (*cP == '\t') )
        {
            cP++;
        }
        if( (*cP < '1') || (*cP > '8') )
        {
            continue;
        }
        drive = (*cP++ - '0');
        if( (*cP != ' ') && (*cP != '\t') )
        {
            continue;
        }
        while( (*cP == ' ') || (*cP == '\t') )
        {
            cP++;
        }
        len = strlen(cP);
        while( (len > 0) && ((cP[len - 1] == '\n') || (cP[len - 1] == '\r')
            || (cP[len - 1] == ' ') || (cP[len - 1] == '\t')) )
        {
            cP[--len] = '\0';
        }
        if( (len > strlen(suffix)) && (strcmp(&cP[len - strlen(suffix)], suffix) == 0) )
        {
            cP[len - strlen(suffix)] = '\0';
        }
        if( *cP == '\0' )
        {
            continue;
        }

        if( *cP == '/' )
        {
            snprintf(listed, sizeof(listed), "%s", cP);
        }
        else
        {
            snprintf(listed, sizeof(listed), "%s/%s", MTFS_TAPEDIR, cP);
        }
        if( canonPath(listed, theirs, sizeof(theirs)) && (strcmp(mine, theirs) == 0) )
        {
            fprintf(stderr, "fstool: %s: warning: the tape is on drive %d in %s; a loaded program that "
                "has it mounted keeps its own directory, and its next directory write will "
                "undo this change\n", pathP, drive, MTFS_LIST);
            break;
        }
    }
    fclose(fP);
}

// Warn when the drum file is the emulator's: a program holding the volume mounted keeps its
// own directory, and its next directory write would undo this change.
static void
warnIfDrum(const char *pathP)
{
char mine[PATH_MAX + 2];
char theirs[PATH_MAX + 2];

    if( canonPath(pathP, mine, sizeof(mine)) && canonPath(MTFS_DRUM, theirs, sizeof(theirs))
        && (strcmp(mine, theirs) == 0) )
    {
        fprintf(stderr, "fstool: %s: warning: this is the emulator's drum; a loaded program that has "
            "the volume mounted keeps its own directory, and its next directory write will undo "
            "this change\n", volNameP);
    }
}

// Report one of the library's status codes for the file named if any with its meaning or
// the detail given.
// Returns 1, the exit status.
static int
libError(int code, const char *nameP, const char *detailP)
{
char meaning[128];

    fprintf(stderr, "fstool: %s: ", volNameP);
    if( nameP )
    {
        fprintf(stderr, "%s: ", nameP);
    }
    snprintf(meaning, sizeof(meaning), errMeaning[code], devNounP);
    fprintf(stderr, "%s, %s\n", errName[code], (detailP ? detailP : meaning));

    return(1);
}

// -D's value: first,tracks in octal, a volume of whole tracks on the drum, or all, every
// volume, for -l. A volume not on the drum is NOUNIT, as the library's drMount gives it.
// Exits on any fault.
static void
parseVolume(const char *argP)
{
const char *cP;
char *endP;
long first;
long tracks;

    onDrum = true;
    drumScan = (strcmp(argP, "all") == 0);
    if( drumScan )
    {
        return;
    }

    errno = 0;
    cP = argP;
    first = -1;
    tracks = -1;
    if( (*cP >= '0') && (*cP <= '7') )
    {
        first = strtol(cP, &endP, 8);
        if( (*endP == ',') && (endP[1] >= '0') && (endP[1] <= '7') )
        {
            tracks = strtol(&endP[1], &endP, 8);
        }
        if( (*endP != '\0') || (errno != 0) )
        {
            tracks = -1;
        }
    }
    if( tracks < 0 )
    {
        fprintf(stderr, "fstool: -D %s: a volume is first,tracks, both octal, or all\n", argP);
        usage();
    }
    if( (first >= DR_TRACKS) || (tracks < 1) || ((first + tracks) > DR_TRACKS) )
    {
        fprintf(stderr, "fstool: -D %s: NOUNIT, the volume's tracks are not on the drum: the first "
            "track is 0-037, with at least one track, the last within 037\n", argP);
        exit(1);
    }
    drFirst = (int) first;
    drTracks = (int) tracks;
}

// List the directory, in octal, with warnings for what the library routines report as warnings.
static int
cmdList(void)
{
char name[NAMELEN + 1];
uint32_t *eP;
int total;
int largest;
int fit;
int slot;
int blk;
int last;

    if( !mountDir() )
    {
        return(1);
    }
    if( dirFrom != DIRBLK )
    {
        fprintf(stderr, "fstool: %s: warning: directory block 1 is bad; the copy in block 2 was "
            "read\n", volNameP);
    }

    freeSpace(0, &total, &largest, &fit);
    printf("%s: generation %o, %#o blocks, %#o free, largest gap %#o\n", volNameP,
        dirP[DGEN], dirP[DBLKS], total, largest);
    printf("slot  name       type  app  ro  first  blocks    words\n");
    for( slot = 0; slot < NENT; slot++ )
    {
        eP = entry(slot);
        if( eP[0] == 0 )
        {
            continue;
        }
        unpackName(eP, name);
        printf("%#4o  %-9s  %04o  %3.2o  %-2s  %#5o  %#6o  %#7o\n", slot, name,
            (eP[DEINFO] & ITYPE), ((eP[DEINFO] & IAPP) >> IAPPSHIFT),
            ((eP[DEINFO] & IRO) ? "ro" : ""), eP[DEFIRST], eP[DEALLOC], eP[DELEN]);

        if( eP[DEMOVE] != 0 )
        {
            fprintf(stderr, "fstool: %s: %s: warning: marked as moving to block %#o; a pack was "
                "stopped while moving it\n", volNameP, name, eP[DEMOVE]);
        }
        last = (int) (eP[DEFIRST] + ((eP[DELEN] + WDS - 1) / WDS));
        for( blk = (int) eP[DEFIRST]; (blk < last) && (blk < volBlks); blk++ )
        {
            if( !blockOk(blk) )
            {
                fprintf(stderr, "fstool: %s: %s: warning: block %#o does not total zero, a checksum error\n",
                    volNameP, name, blk);
            }
        }
    }

    return(0);
}

// Add a host file as name: a free slot and the first gap large enough, as the library's
// mtfCreate finds them, then its words and the directory.
// With force a file of that name is replaced.
// Returns the exit status.
static int
cmdAdd(const char *nameP, int type, bool readOnly, int blocks, enum form f, bool force,
    const char *sourceP)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
uint32_t data[WDS];
uint32_t *wordsP;
uint32_t *eP;
long count;
long done;
int need;
int slot;
int freeSlot;
int total;
int largest;
int fit;
int blk;
int status;

    if( (status = packName(nameP, nm)) != E_OK )
    {
        return(libError(status, nameP, NULL));
    }
    // Messages name the file as it is saved, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir() )
    {
        return(1);
    }
    if( !(wordsP = sourceWords(sourceP, f, &count)) )
    {
        return(1);
    }

    // The checks in the library's order: the name, a slot, the size, then the room.
    if( lookup(nm, &slot, &freeSlot) == E_OK )
    {
        if( !force )
        {
            free(wordsP);
            return(libError(E_EXISTS, nameP, NULL));
        }
        memset(entry(slot), 0, (ENTW * sizeof(uint32_t)));
        lookup(nm, &slot, &freeSlot);
    }
    if( freeSlot < 0 )
    {
        free(wordsP);
        return(libError(E_DIRFULL, nameP, NULL));
    }
    need = (int) ((count + WDS - 1) / WDS);
    if( need == 0 )
    {
        need = 1;
    }
    if( blocks < 0 )
    {
        blocks = need;
    }
    if( (blocks == 0) || (blocks > MAXBLKS) )
    {
        free(wordsP);
        return(libError(E_RANGE, nameP, "a file is 1 to 0777 blocks"));
    }
    if( blocks < need )
    {
        free(wordsP);
        return(libError(E_RANGE, nameP, "-b gives fewer blocks than the data needs"));
    }
    freeSpace(blocks, &total, &largest, &fit);
    if( fit < 0 )
    {
        free(wordsP);
        return(libError(E_NOSPACE, nameP, NULL));
    }

    eP = entry(freeSlot);
    eP[0] = nm[0];
    eP[1] = nm[1];
    eP[2] = nm[2];
    eP[DEFIRST] = (uint32_t) fit;
    eP[DEALLOC] = (uint32_t) blocks;
    eP[DELEN] = (uint32_t) count;
    eP[DEINFO] = ((uint32_t) type | (readOnly ? IRO : 0));
    eP[DEMOVE] = 0;

    // Only the blocks that hold data are written, the rest of the allocation is unchanged.
    for( blk = fit, done = 0; done < count; blk++, done += WDS )
    {
        memset(data, 0, sizeof(data));
        memcpy(data, &wordsP[done], ((size_t) (((count - done) < WDS) ? (count - done) : WDS)
            * sizeof(uint32_t)));
        putBlock(blk, data);
    }
    free(wordsP);
    writeDir();

    return(0);
}

// Delete name: its entry cleared and the directory written; its blocks are not erased.
// A read-only file needs force.
// Returns the exit status.
static int
cmdDelete(const char *nameP, bool force)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
int slot;
int freeSlot;
int status;

    if( (status = packName(nameP, nm)) != E_OK )
    {
        return(libError(status, nameP, NULL));
    }
    // Messages name the file as it is stored, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir() )
    {
        return(1);
    }
    if( lookup(nm, &slot, &freeSlot) != E_OK )
    {
        return(libError(E_NOTFOUND, nameP, NULL));
    }
    if( (entry(slot)[DEINFO] & IRO) && !force )
    {
        return(libError(E_READONLY, nameP, NULL));
    }
    memset(entry(slot), 0, (ENTW * sizeof(uint32_t)));
    writeDir();

    return(0);
}

// Extract name's words bounded by its length to a host file.
// A tape block that does not total zero is BADBLOCK; a drum block has no checksum.
// Returns the exit status.
static int
cmdExtract(const char *nameP, enum form f, const char *outputP)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
uint32_t *eP;
uint32_t *wordsP;
long count;
long i;
int slot;
int freeSlot;
int blk;
int status;
char detail[128];

    if( (status = packName(nameP, nm)) != E_OK )
    {
        return(libError(status, nameP, NULL));
    }
    // Messages name the file as it is kept, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir() )
    {
        return(1);
    }
    if( lookup(nm, &slot, &freeSlot) != E_OK )
    {
        return(libError(E_NOTFOUND, nameP, NULL));
    }
    eP = entry(slot);
    count = (long) eP[DELEN];
    if( (count > ((long) eP[DEALLOC] * WDS)) || (((long) eP[DEFIRST] + eP[DEALLOC]) > volBlks) )
    {
        snprintf(detail, sizeof(detail), "the entry's length or blocks are past its allocation "
            "or the end of the %s", devNounP);
        return(libError(E_RANGE, nameP, detail));
    }

    if( !(wordsP = calloc((size_t) (count + 1), sizeof(uint32_t))) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(1);
    }
    for( i = 0; i < count; i++ )
    {
        blk = (int) (eP[DEFIRST] + (uint32_t) (i / WDS));
        if( ((i % WDS) == 0) && !blockOk(blk) )
        {
            snprintf(detail, sizeof(detail), "block %#o does not total zero, a checksum error", blk);
            free(wordsP);
            return(libError(E_BADBLOCK, nameP, detail));
        }
        wordsP[i] = blockData(blk)[i % WDS];
    }
    status = (writeHost(outputP, wordsP, count, f) ? 0 : 1);
    free(wordsP);

    return(status);
}

// Format: an empty directory at generation 1, on the drum with the volume's first track.
// Unless forced, refused with FOREIGN when blocks 1 and 2 hold another program's data.
// One must be this file system's, of the volume's blocks, or both total zero with only zero
// data, blank blocks; never-written drum tracks read as zeros.
// File blocks are not touched.
// Returns the exit status.
static int
cmdFormat(bool force, bool existed)
{
uint32_t data[WDS];
uint32_t *bP;
int ours;
int zeros;
int blk;
int i;

    if( existed && !force )
    {
        ours = zeros = 0;
        for( blk = DIRBLK; blk <= DIRBLK2; blk++ )
        {
            if( !blockOk(blk) )
            {
                continue;
            }
            bP = blockData(blk);
            if( dirOk(bP) )
            {
                ours++;
                continue;
            }
            for( i = 0; (i < WDS) && (bP[i] == 0); i++ )
            {
            }
            if( i == WDS )
            {
                zeros++;
            }
        }
        if( (ours == 0) && (zeros != 2) )
        {
            return(libError(E_FOREIGN, NULL, "blocks 1 and 2 hold another program's data; -F "
                "formats anyway"));
        }
    }

    memset(data, 0, sizeof(data));
    data[DMAGIC] = MAGIC;
    data[DVER] = VERSION;
    data[DGEN] = 0;
    data[DBLKS] = (uint32_t) volBlks;
    if( onDrum )
    {
        data[DDEV] = (uint32_t) drFirst;
    }
    memcpy(blockData(DIRBLK), data, sizeof(data));
    dirP = blockData(DIRBLK);
    writeDir();

    return(0);
}

// Pack: each file in order of first block is moved down to the
// lowest free block, a file marked as already moving left where it is.
// All in memory, so the directory is written once and no file is ever marked.
// Returns the exit status.
static int
cmdPack(void)
{
uint32_t *eP;
uint32_t *selP;
int pos;
int first;
int alloc;
int moved;
int iter;
int slot;
int i;

    if( !mountDir() )
    {
        return(1);
    }

    moved = 0;
    pos = FIRSTBLK;
    for( iter = 0; iter <= NENT; iter++ )
    {
        selP = NULL;
        first = volBlks;
        for( slot = 0; slot < NENT; slot++ )
        {
            eP = entry(slot);
            if( (eP[0] != 0) && ((int) eP[DEFIRST] >= pos) && ((int) eP[DEFIRST] < first) )
            {
                first = (int) eP[DEFIRST];
                selP = eP;
            }
        }
        if( !selP )
        {
            break;
        }
        alloc = (int) selP[DEALLOC];

        // Lowest block first, which is safe as the new place is below the old.
        if( (selP[DEMOVE] == 0) && (first != pos) )
        {
            for( i = 0; (i < alloc) && ((first + i) < volBlks); i++ )
            {
                copyBlock((pos + i), (first + i));
            }
            selP[DEFIRST] = (uint32_t) pos;
            first = pos;
            moved++;
        }
        pos = (first + alloc);
    }

    if( moved == 0 )
    {
        printf("%s: no gaps to close\n", volNameP);
        return(0);
    }
    writeDir();
    printf("%s: %d file%s moved\n", volNameP, moved, ((moved == 1) ? "" : "s"));

    return(0);
}

// List every volume on the drum file: each track whose block 1, or else 2, holds this file
// system's directory recording that track as its first, with blocks for whole tracks within
// the drum. A volume overlapping one listed before it, a directory left from an earlier
// format, gives a warning.
// Returns the exit status: NOFS when no track starts a volume.
static int
cmdScan(void)
{
uint32_t *dP;
int found;
int lastEnd;
int total;
int largest;
int fit;
int files;
int slot;
int blk;
int t;

    found = 0;
    lastEnd = 0;
    for( t = 0; t < DR_TRACKS; t++ )
    {
        // blockData() and freeSpace() take the volume from these.
        drFirst = t;
        dP = NULL;
        for( blk = DIRBLK; blk <= DIRBLK2; blk++ )
        {
            dP = blockData(blk);
            if( (dP[DMAGIC] == MAGIC) && (dP[DVER] == VERSION) && (dP[DBLKS] != 0)
                && ((dP[DBLKS] % DR_BPT) == 0) && ((t + (dP[DBLKS] / DR_BPT)) <= DR_TRACKS)
                && (dP[DDEV] == (uint32_t) t) )
            {
                break;
            }
            dP = NULL;
        }
        if( !dP )
        {
            continue;
        }

        drTracks = (int) (dP[DBLKS] / DR_BPT);
        volBlks = (int) dP[DBLKS];
        dirP = dP;
        freeSpace(0, &total, &largest, &fit);
        files = 0;
        for( slot = 0; slot < NENT; slot++ )
        {
            if( entry(slot)[0] != 0 )
            {
                files++;
            }
        }
        if( drTracks == 1 )
        {
            printf("%s: track %03o, ", imageNameP, t);
        }
        else
        {
            printf("%s: tracks %03o-%03o, ", imageNameP, t, ((t + drTracks) - 1));
        }
        printf("generation %o, %#o blocks, %d file%s, %#o free, largest gap %#o\n", dP[DGEN],
            volBlks, files, ((files == 1) ? "" : "s"), total, largest);
        if( blk != DIRBLK )
        {
            fprintf(stderr, "fstool: %s: track %03o: warning: directory block 1 is not the file "
                "system's; the copy in block 2 was read\n", imageNameP, t);
        }
        if( t < lastEnd )
        {
            fprintf(stderr, "fstool: %s: track %03o: warning: inside the volume listed before it, "
                "which ends at track %03o; one of the two is left from an earlier format\n",
                imageNameP, t, (lastEnd - 1));
        }
        if( (t + drTracks) > lastEnd )
        {
            lastEnd = (t + drTracks);
        }
        found++;
    }

    if( found == 0 )
    {
        return(libError(E_NOFS, NULL, "no track starts a file system volume"));
    }

    return(0);
}

// An octal option value, digits 0-7 only.
// Exits through usage on anything else.
static long
octalArg(const char *argP, char opt)
{
char *endP;
long v;

    errno = 0;
    v = strtol(argP, &endP, 8);
    if( (*argP == '\0') || (*endP != '\0') || (errno != 0) || (v < 0) || (*argP == '-')
        || (*argP == '+') )
    {
        fprintf(stderr, "fstool: -%c %s: not an octal number\n", opt, argP);
        usage();
    }

    return(v);
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: fstool [-D first,tracks] -l image\n"
        "       fstool -D all -l drumfile\n"
        "       fstool [-D first,tracks] -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F]\n"
        "            -s source image\n"
        "       fstool [-D first,tracks] -d -n name [-F] image\n"
        "       fstool [-D first,tracks] -x -n name [-f bytes|text|words] -o output image\n"
        "       fstool [-D first,tracks] -i [-F] image\n"
        "       fstool [-D first,tracks] -p image\n"
        "  Manage the file system on a Type 550 tape image, or with -D on a Type 23 drum file.\n"
        "  -D first,tracks: the image is a drum file, the volume its tracks first to first+tracks-1.\n"
        "  -D all: with -l, list every volume on the drum file.\n"
        "  -l lists the directory, -a adds source as name, -d deletes name.\n"
        "  -x extracts name to output, -i formats an empty directory, a missing image is created.\n"
        "  -p packs the files to close any gaps.\n"
        "  -n name: 1 to 9 characters, any case, no control characters or a leading blank.\n"
        "  -t type: octal 0-7777, default 0.\n"
        "  -r: the file is read-only.\n"
        "  -b blocks: octal, at least as big as the data, at most 0777; default is the data's size.\n"
        "  -f form:\n"
        "     bytes (default), two 8-bit bytes a word;\n"
        "     text, as bytes with line terminators stored as CR;\n"
        "     words, each word a little-endian 32-bit word.\n"
        "  -F: force.\n"
        "    add replaces a file of that name;\n"
        "    delete removes a read-only file;\n"
        "    format overwrites another program's data.\n"
        "  Numbers are octal.\n"
        "  Change a tape or a drum volume only when not being used by a program\n"
        "    A program that has it mounted would undo the change.\n"
        "    A tape in %s, or the drum %s, gives a warning.\n",
        MTFS_LIST, MTFS_DRUM);
    exit(1);
}
