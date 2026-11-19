/*
 * fstool: manage a file system on a Type 550 tape image or a Type 23 drum file.
 *
 * Usage: fstool [-D first] -l image
 *        fstool -D all -l drumfile
 *        fstool [-D first] -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F]
 *             -s source image
 *        fstool [-D first] -d -n name [-F] image
 *        fstool [-D first] -x -n name [-f bytes|text|words] -o output image
 *        fstool [-D first [-b blocks]] -i [-F] image
 *        fstool [-D first] -p image
 * where:
 * -D first - the image is a drum file and the volume starts at the given track.
 *            Its size is read from its directory, except by format.
 *            With -D the image may be left out, and is then the emulator's drum file.
 *            Without -D the image is a tape
 * -D all     - with -l only: list every volume on the drum file
 * -l         - list the directory
 * -a         - add the host file source to the tape or volume as -n name
 * -d         - delete -n name
 * -x         - extract -n name to the output file
 * -i         - format: write an empty directory, creating the image if it is missing
 * -p         - pack: move the files down to close the gaps between them
 * -n name    - the file's name: 1 to 9 characters, no control character, no leading blank,
 *               matched without regard to case and kept in upper case
 * -t type    - the file's type, octal 0-7777, decimal 0-4095, default 0
 * -r         - mark the file read-only
 * -b blocks  - add: the blocks to allocate, at least the data's size and at most 511, octal 0777;
 *              defaults to the data's size, one block for an empty file
 *              format, drum only: the volume's blocks, whole tracks of 16 within the drum;
 *              defaults to the size of the volume already at that track, else one track, 16
 *              A tape is always 01100 blocks.
 * -f form    - the host file's format:
 *                bytes (the default), two 8-bit bytes a word, the first in
 *                  the high half, as am1's ascii produces
 *                text, as bytes with each line end stored as CR (015), extracted back as LF
 *                words, each 18-bit word in a little-endian 32-bit word matching the PDP-1
 * -F         - force: add replaces a file of the same name, delete removes a read-only file,
 *                format overwrites another program's data in blocks 1 and 2
 *
 * Numeric values are decimal, or octal with a leading 0, hexadecimal with 0x, binary with 0b.
 *
 * The file system is the am1 library's in Am1Includes/FILESYSTEM, with the tape's layer in
 * Am1Includes/MICROTAPE and the drum's in Am1Includes/DRUM, see their README.md files.
 * Block 0 unused, the directory is in block 1 and a copy in block 2, the files from block 3,
 * each a run of contiguous blocks.
 * A tape or a drum volume this tool writes works with the library routines and vice-versa.
 *
 * A drum volume is a run of tracks, 16 blocks of 256, octal 0400, words a track with no checksum,
 * skewed by one block a track.
 * Its directory records its first track and a volume whose
 * directory records a different one is refused, as the library's drMount refuses it.
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
 * See Docs/UsingFstool.md for more information.
 *
 * The image's words are in one struct image, and the tape or drum volume a command works on is
 * one struct volume, sized and named before the command runs; both are passed by pointer.
 * What differs between a tape and a drum file is the functions of a struct device, chosen once
 * by -D.
 *
 * Revision history:
 *
 * 11-Nov-2026 Claude/wje initial version
 * 13-Nov-2026 Claude file system volumes on a Type 23 drum file, -D
 * 13-Nov-2026 wje rename, reorganize
 * 13-Nov-2026 wje values can now be decimal, octal, hex, or binary.
 * 18-Nov-2026 wje require at least one block when formatting a drum volume.
 * 19-Nov-2026 Claude restructured at wje's request to be better organized.
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
#ifndef DRUMFILE
#define DRUMFILE       "/opt/pidp1-mods/pdp23drum"
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

// How a volume's size was found, for format's note on it.
enum sizing {
    SIZE_READ,                  // a tape's, or a drum volume's from its directory
    SIZE_GIVEN,                 // format's -b
    SIZE_KEPT,                  // format: the size of the volume already at that track
    SIZE_DEFAULT                // format: one track, as no volume starts there
};

struct image;
struct volume;

// What differs between a tape and a drum file: the image's form, a block's place and checksum,
// and the messages that name the device.
struct device {
    const char *nounP;          // the contents, in messages: "tape" or "volume"
    const char *fileP;          // the image, in a failed write: "image" or "drum file"
    const char *lockedP;        // why a write-locked image is not written
    bool firstInDir;            // the directory records the volume's first track, in DDEV
    // Read the image into memory, a missing one blank when mayBeMissing; false with a message.
    bool (*read)(struct image *imgP, bool mayBeMissing);
    // Write the image back over its file; false with a message.
    bool (*write)(struct image *imgP);
    // A block's 0400 data words.
    uint32_t *(*data)(const struct volume *vP, int blk);
    // Whether a block's checksum is good.
    bool (*good)(const struct volume *vP, int blk);
    // Write a block's data, with its checksums.
    void (*put)(struct volume *vP, int blk, const uint32_t *dataP);
    // Copy a block whole.
    void (*copy)(struct volume *vP, int to, int from);
    // Before a format: what the user is to be told about the volume.
    void (*formatting)(const struct volume *vP);
    // Before a write: warn when a running program may hold the image mounted.
    void (*warnMounted)(const struct volume *vP);
};

// The image file's words, read completely, changed in memory and written back whole.
struct image {
    const struct device *devP;
    const char *pathP;          // the image as named, for messages about the file
    bool changed;               // the directory was written, so the image is to be
    union {
        struct {
            uint32_t blk[BLKS][BLKW];       // every block past the file's end blank
            int fileBlks;                   // blocks in the file as read
            int topBlks;                    // blocks the file will hold when written
        } tape;
        struct {
            uint32_t word[DR_WORDS];        // zero past the file's end
            int fileWords;                  // words in the file as read
            bool trackWritten[DR_TRACKS];
        } drum;
    };
};

// The tape, or the drum volume, a command works on. openVolume sizes and names it before the
// command runs, and only format chooses its size.
struct volume {
    struct image *imgP;
    int first;                  // a drum volume's first track, 0 on a tape
    int tracks;                 // a drum volume's tracks, 0 on a tape
    int blocks;                 // a tape's, or 16 a drum track; 0 when no directory gave them
    enum sizing sizing;
    const char *nameP;          // the tape or the volume, for messages about its contents
    char nameBuf[PATH_MAX + 32];
    uint32_t *dirP;             // the directory's data words, in block DIRBLK, once mounted
    int dirFrom;                // the block the directory was read from
};

// The command line.
struct options {
    char cmd;                   // l a d x i p
    const char *imageP;
    const char *nameP;
    const char *sourceP;
    const char *outputP;
    int type;
    int blocks;                 // -1 when -b is not given: add and format each have a default
    bool readOnly;
    bool force;
    enum form f;
    bool onDrum;
    bool allTracks;             // -D all
    int first;                  // -D first
    char seen[32];              // each option given, once, for each command's check
    int nSeen;
};

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

static void parseOptions(int argc, char **argv, struct options *oP);
static void checkOptions(const struct options *oP);
static void parseVolume(const char *argP, struct options *oP);
static int numericArg(const char *argP, char opt);
static void usage(void);
static void openVolume(struct volume *vP, struct image *imgP, const struct options *oP);
static void sizeVolume(struct volume *vP, int first, int blocks);
static int formatBlocks(const struct image *imgP, int first, int given, enum sizing *howP);
static int volumeAt(const struct image *imgP, int t, int *blkP);
static void maskVolume(struct volume *vP);
static int onesAdd(int a, int b);
static int blockTotal(const uint32_t *blkP);
static bool blockBlank(const struct image *imgP, int blk);
static uint32_t *blockData(const struct volume *vP, int blk);
static bool blockOk(const struct volume *vP, int blk);
static void putBlock(struct volume *vP, int blk, const uint32_t *dataP);
static void copyBlock(struct volume *vP, int to, int from);
static bool tapeRead(struct image *imgP, bool mayBeMissing);
static bool tapeWrite(struct image *imgP);
static uint32_t *tapeData(const struct volume *vP, int blk);
static bool tapeGood(const struct volume *vP, int blk);
static void tapePut(struct volume *vP, int blk, const uint32_t *dataP);
static void tapeCopy(struct volume *vP, int to, int from);
static void tapeFormatting(const struct volume *vP);
static void tapeWarnMounted(const struct volume *vP);
static bool drumRead(struct image *imgP, bool mayBeMissing);
static bool drumWrite(struct image *imgP);
static uint32_t *drumData(const struct volume *vP, int blk);
static bool drumGood(const struct volume *vP, int blk);
static void drumPut(struct volume *vP, int blk, const uint32_t *dataP);
static void drumCopy(struct volume *vP, int to, int from);
static void drumFormatting(const struct volume *vP);
static void drumWarnMounted(const struct volume *vP);
static bool replaceFile(const struct image *imgP, const uint8_t *bytesP, size_t len);
static bool dirOk(const struct volume *vP, const uint32_t *dataP);
static bool mountDir(struct volume *vP);
static void writeDir(struct volume *vP);
static void warnOverlap(const struct volume *vP);
static uint32_t *entry(const struct volume *vP, int slot);
static int packName(const char *nameP, uint32_t *nmP);
static void unpackName(const uint32_t *nmP, char *outP);
static int lookup(const struct volume *vP, const uint32_t *nmP, int *slotP, int *freeP);
static void freeSpace(const struct volume *vP, int need, int *totalP, int *largestP, int *fitP);
static uint8_t *readHost(const char *pathP, size_t *lenP);
static uint32_t *sourceWords(const struct volume *vP, const char *pathP, enum form f, int *countP);
static bool writeHost(const char *pathP, const uint32_t *wordsP, int count, enum form f);
static bool canonPath(const char *pathP, char *outP, size_t size);
static void warnIfListed(const char *pathP);
static void warnIfDrum(const struct volume *vP);
static int libError(const struct volume *vP, int code, const char *nameP, const char *detailP);
static int statusError(const char *whereP, const char *nounP, int code, const char *nameP,
    const char *detailP);
static int cmdList(struct volume *vP);
static int cmdScan(struct image *imgP);
static int cmdAdd(struct volume *vP, const struct options *oP);
static int cmdDelete(struct volume *vP, const char *nameP, bool force);
static int cmdExtract(struct volume *vP, const char *nameP, enum form f, const char *outputP);
static int cmdFormat(struct volume *vP, bool force, bool existed);
static int cmdPack(struct volume *vP);

static const struct device tapeDevice = {
    .nounP = "tape",
    .fileP = "image",
    .lockedP = "the image file is read-only",
    .firstInDir = false,
    .read = tapeRead,
    .write = tapeWrite,
    .data = tapeData,
    .good = tapeGood,
    .put = tapePut,
    .copy = tapeCopy,
    .formatting = tapeFormatting,
    .warnMounted = tapeWarnMounted
};

static const struct device drumDevice = {
    .nounP = "volume",
    .fileP = "drum file",
    .lockedP = "the drum file is read-only",
    .firstInDir = true,
    .read = drumRead,
    .write = drumWrite,
    .data = drumData,
    .good = drumGood,
    .put = drumPut,
    .copy = drumCopy,
    .formatting = drumFormatting,
    .warnMounted = drumWarnMounted
};

int
main(int argc, char **argv)
{
struct options opt;
struct image *imgP;
struct volume vol;
int status;
bool writes;
bool existed;

    parseOptions(argc, argv, &opt);
    checkOptions(&opt);

    if( !(imgP = calloc(1, sizeof(*imgP))) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(1);
    }
    imgP->devP = (opt.onDrum ? &drumDevice : &tapeDevice);
    imgP->pathP = opt.imageP;

    // The image: a missing one is a blank tape or drum only to format.
    writes = (strchr("adip", opt.cmd) != NULL);
    existed = (access(imgP->pathP, F_OK) == 0);
    if( !imgP->devP->read(imgP, (opt.cmd == 'i')) )
    {
        return(1);
    }

    if( opt.allTracks )
    {
        status = cmdScan(imgP);
        free(imgP);
        return(status);
    }

    openVolume(&vol, imgP, &opt);
    if( writes && existed && (access(imgP->pathP, W_OK) != 0) )
    {
        return(libError(&vol, E_LOCKED, NULL, imgP->devP->lockedP));
    }

    switch( opt.cmd )
    {
    case 'l':
        status = cmdList(&vol);
        break;

    case 'a':
        status = cmdAdd(&vol, &opt);
        break;

    case 'd':
        status = cmdDelete(&vol, opt.nameP, opt.force);
        break;

    case 'x':
        status = cmdExtract(&vol, opt.nameP, opt.f, opt.outputP);
        break;

    case 'i':
        imgP->devP->formatting(&vol);
        status = cmdFormat(&vol, opt.force, existed);
        break;

    default:
        status = cmdPack(&vol);
        break;
    }

    if( (status == 0) && writes && imgP->changed )
    {
        imgP->devP->warnMounted(&vol);
        if( !imgP->devP->write(imgP) )
        {
            status = 1;
        }
    }
    free(imgP);

    return(status);
}

// Read the command line into *oP: the command, its options and the image.
// Exits through usage on an unknown or malformed option, a second command or a wrong count of
// images, and with a message on a bad value.
static void
parseOptions(int argc, char **argv, struct options *oP)
{
int opt;

    memset(oP, 0, sizeof(*oP));
    oP->f = FORM_BYTES;
    oP->blocks = -1;
    while( (opt = getopt(argc, argv, "ladxipn:t:rb:f:Fs:o:D:")) != -1 )
    {
        if( opt == '?' )
        {
            usage();
        }

        if( (oP->nSeen < ((int) sizeof(oP->seen) - 1)) && !strchr(oP->seen, opt) )
        {
            oP->seen[oP->nSeen++] = (char) opt;
        }
        oP->seen[oP->nSeen] = '\0';

        switch( opt )
        {
        case 'l':
        case 'a':
        case 'd':
        case 'x':
        case 'i':
        case 'p':
            if( oP->cmd && (oP->cmd != opt) )
            {
                fprintf(stderr, "fstool: only one of -l -a -d -x -i -p\n");
                usage();
            }
            oP->cmd = (char) opt;
            break;

        case 'n':
            oP->nameP = optarg;
            break;

        case 't':
            oP->type = numericArg(optarg, 't');
            if( oP->type > ITYPE )
            {
                fprintf(stderr, "fstool: -t %s: the type is octal 0-7777\n", optarg);
                exit(1);
            }
            break;

        case 'r':
            oP->readOnly = true;
            break;

        case 'b':
            // Anything past 0777 is refused as RANGE later.
            oP->blocks = numericArg(optarg, 'b');
            break;

        case 'f':
            if( strcmp(optarg, "bytes") == 0 )
            {
                oP->f = FORM_BYTES;
            }
            else if( strcmp(optarg, "text") == 0 )
            {
                oP->f = FORM_TEXT;
            }
            else if( strcmp(optarg, "words") == 0 )
            {
                oP->f = FORM_WORDS;
            }
            else
            {
                fprintf(stderr, "fstool: -f %s: the form is bytes, text or words\n", optarg);
                exit(1);
            }
            break;

        case 'F':
            oP->force = true;
            break;

        case 's':
            oP->sourceP = optarg;
            break;

        case 'o':
            oP->outputP = optarg;
            break;

        case 'D':
            oP->onDrum = true;
            parseVolume(optarg, oP);
            break;

        default:
            usage();
        }
    }

    if( !oP->cmd )
    {
        usage();
    }

    // If using the drum, the filename is not required.
    // It then defaults to DRUMFILE.
    if( optind != (argc - 1) )
    {
        if( !oP->onDrum || (optind != argc) )
        {
            usage();
        }

        oP->imageP = DRUMFILE;
    }
    else
    {
        oP->imageP = argv[optind];
    }
}

// Check the options against the command: each command takes only its own options, and its
// required ones; -D goes with any. Format's -b sizes a drum volume, whole tracks within the
// drum; a tape has one size.
// Exits through usage, or with a message, on a mismatch.
static void
checkOptions(const struct options *oP)
{
const char *allowedP;
int i;

    switch( oP->cmd )
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
        allowedP = "ibFD";
        break;

    default:
        allowedP = (oP->cmd == 'l') ? "lD" : "pD";
        break;
    }

    for( i = 0; i < oP->nSeen; i++ )
    {
        if( !strchr(allowedP, oP->seen[i]) )
        {
            fprintf(stderr, "fstool: -%c cannot be used with -%c\n", oP->seen[i], oP->cmd);
            usage();
        }
    }

    if( (strchr("adx", oP->cmd) && !oP->nameP) || ((oP->cmd == 'a') && !oP->sourceP) ||
        ((oP->cmd == 'x') && !oP->outputP) )
    {
        usage();
    }

    if( oP->allTracks && (oP->cmd != 'l') )
    {
        fprintf(stderr, "fstool: -D all is only for -l; the other commands name a volume, -D first\n");
        usage();
    }

    if( (oP->cmd == 'i') && (oP->blocks >= 0) )
    {
        if( !oP->onDrum )
        {
            fprintf(stderr, "fstool: -b with -i is for a drum volume; a tape is always %d blocks\n", BLKS);
            exit(1);
        }
        if( (oP->blocks == 0) || ((oP->blocks % DR_BPT) != 0)
            || ((oP->blocks / DR_BPT) > (DR_TRACKS - oP->first)) )
        {
            fprintf(stderr, "fstool: -b %d: NOUNIT, a drum volume is whole tracks of %d blocks within "
                "the drum; starting at track %d it is %d to %d blocks\n", oP->blocks, DR_BPT, oP->first,
                DR_BPT, ((DR_TRACKS - oP->first) * DR_BPT));
            exit(1);
        }
    }
}

// -D's value: first, a volume of tracks on the drum, the number in any form numericArg
// accepts, or all, every volume.
// Exits on a usage error.
static void
parseVolume(const char *argP, struct options *oP)
{
int first;

    if( strcmp(argP, "all") == 0)
    {
        oP->allTracks = true;
        return;
    }

    first = numericArg(argP, 'D');

    // The tracks are not known until the directory is read, so only the first is checked here.
    if( first >= DR_TRACKS )
    {
        fprintf(stderr,
            "fstool: -D %s: NOUNIT, the volume's initial track exceeds the size of the drum, 32 tracks\n", argP);
        exit(1);
    }

    oP->allTracks = false;
    oP->first = first;
}

// A numeric option value: decimal, or with a leading 0 octal, 0x hexadecimal, 0b binary,
// e.g., 10, 012, 0b1101, 0x3F.
// Exits through usage on anything else, including a value past int's range, which strtol()'s
// long would hold but an int would not.
static int
numericArg(const char *argP, char opt)
{
const char *digitsP;
char *endP;
int base;
long v;

    // strtol() reads 0b as binary only in a C23 library, so binary's prefix is taken here and
    // its digits checked; strtol() with a base of 0 reads the other forms.
    digitsP = argP;
    base = 0;
    if( (argP[0] == '0') && ((argP[1] == 'b') || (argP[1] == 'B')) )
    {
        digitsP = &argP[2];
        base = 2;
        if( strspn(digitsP, "01") != strlen(digitsP) )
        {
            digitsP = "";
        }
    }

    errno = 0;
    v = strtol(digitsP, &endP, base);
    if( (*digitsP == '\0') || (*endP != '\0') || (errno != 0) || (v < 0) || (v > INT_MAX)
        || (*digitsP == '-') || (*digitsP == '+') )
    {
        fprintf(stderr, "fstool: -%c %s: not a valid number\n", opt, argP);
        usage();
    }

    return((int) v);
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: fstool [-D first] -l image\n"
        "       fstool -D all -l drumfile\n"
        "       fstool [-D first] -a -n name [-t type] [-r] [-b blocks] [-f bytes|text|words] [-F]\n"
        "            -s source image\n"
        "       fstool [-D first] -d -n name [-F] image\n"
        "       fstool [-D first] -x -n name [-f bytes|text|words] -o output image\n"
        "       fstool [-D first [-b blocks]] -i [-F] image\n"
        "       fstool [-D first] -p image\n"
        "  Manage the file system on a Type 550 tape image, or with -D on a Type 23 drum file.\n"
        "  -D first: the image is a drum file, the volume starting at track first, its size read\n"
        "     from its directory; with -D the image may be left out, the emulator's drum file.\n"
        "  -D all: with -l, list every volume on the drum file.\n"
        "  -l lists the directory.\n"
        "  -a adds source as name.\n"
        "  -d deletes name.\n"
        "  -x extracts name to output.\n"
        "  -i formats an empty directory, a missing image is created. A drum volume is -b blocks,\n"
        "     whole tracks of 16; without -b, the volume already at that track keeps its size,\n"
        "     else it is one track, 16 blocks. A tape is always %d blocks.\n"
        "  -p packs the files to close any gaps.\n"
        "  -n name: 1 to 9 characters, any case, no control characters or a leading blank.\n"
        "  -t type: decimal 0-4095, octal 0-7777, default 0.\n"
        "  -r: the file is read-only.\n"
        "  -b blocks: with -a, at least as big as the data, at most 511, octal 0777, default is the\n"
        "     data's size; with -i, see above.\n"
        "  -f form:\n"
        "     bytes (default), two 8-bit bytes a word;\n"
        "     text, as bytes with line terminators stored as CR;\n"
        "     words, each word a little-endian 32-bit word.\n"
        "  -F: force.\n"
        "    add replaces a file of that name;\n"
        "    delete removes a read-only file;\n"
        "    format overwrites another program's data.\n"
        "  Numbers are decimal unless they start with a leading 0, 0x, or 0b.\n"
        "  Change a tape or a drum volume only when not being used by a program.\n"
        "    A program that has it mounted can overwrite the change.\n"
        "    A tape in %s, or the drum %s, gives a warning.\n",
        BLKS, MTFS_LIST, DRUMFILE);

    exit(1);
}

// Make the volume a command works on, sized and named, from the image read: a tape is the
// whole image; a drum volume's size is its directory's, or format's choice.
// A drum volume's words are masked to 18 bits, as only they are the file system's; another
// program's tracks are kept as they are, whatever they hold.
static void
openVolume(struct volume *vP, struct image *imgP, const struct options *oP)
{
    memset(vP, 0, sizeof(*vP));
    vP->imgP = imgP;
    vP->sizing = SIZE_READ;
    if( !oP->onDrum )
    {
        vP->blocks = BLKS;
        vP->nameP = imgP->pathP;
        return;
    }

    if( oP->cmd == 'i' )
    {
        sizeVolume(vP, oP->first, formatBlocks(imgP, oP->first, oP->blocks, &vP->sizing));
    }
    else
    {
        // 0 blocks when the track starts no volume; mountDir then reports NOFS.
        sizeVolume(vP, oP->first, volumeAt(imgP, oP->first, NULL));
    }
    maskVolume(vP);
}

// Set a drum volume's first track and blocks, a multiple of 16 or 0 when no directory gave
// them, and its name for messages.
static void
sizeVolume(struct volume *vP, int first, int blocks)
{
    vP->first = first;
    vP->blocks = blocks;
    vP->tracks = (blocks / DR_BPT);
    if( vP->tracks <= 1 )
    {
        snprintf(vP->nameBuf, sizeof(vP->nameBuf), "%s track %3d", vP->imgP->pathP, first);
    }
    else
    {
        snprintf(vP->nameBuf, sizeof(vP->nameBuf), "%s tracks %3d-%3d", vP->imgP->pathP, first,
            ((first + vP->tracks) - 1));
    }
    vP->nameP = vP->nameBuf;
}

// Format's blocks for the drum volume at track first: -b's when given. Without it, a volume
// keeps the size of one already starting at its track, so a reformat leaves no tracks behind
// it; on a track that starts none, it is one track.
// Returns the blocks, with *howP saying which.
static int
formatBlocks(const struct image *imgP, int first, int given, enum sizing *howP)
{
int blocks;

    if( given >= 0 )
    {
        *howP = SIZE_GIVEN;
        return(given);
    }
    if( (blocks = volumeAt(imgP, first, NULL)) > 0 )
    {
        *howP = SIZE_KEPT;
        return(blocks);
    }
    *howP = SIZE_DEFAULT;

    return(DR_BPT);
}

// The blocks of the volume starting at drum track t: its directory, block 1 or else block 2,
// is this file system's, records t as its first track, and is for whole tracks within the
// drum. Blocks 1 and 2 are always on a volume's first track, unskewed.
// Returns the blocks, with *blkP the directory block when blkP is not NULL, or 0 when track t
// starts no volume.
static int
volumeAt(const struct image *imgP, int t, int *blkP)
{
const uint32_t *dP;
int blk;

    for( blk = DIRBLK; blk <= DIRBLK2; blk++ )
    {
        dP = &imgP->drum.word[(t * DR_TRACKW) + (blk * WDS)];
        if( (dP[DMAGIC] == MAGIC) && (dP[DVER] == VERSION) && (dP[DDEV] == (uint32_t) t)
            && (dP[DBLKS] != 0) && ((dP[DBLKS] % DR_BPT) == 0)
            && ((dP[DBLKS] / DR_BPT) <= (uint32_t) (DR_TRACKS - t)) )
        {
            if( blkP )
            {
                *blkP = blk;
            }
            return((int) dP[DBLKS]);
        }
    }

    return(0);
}

// Mask a drum volume's words to 18 bits.
static void
maskVolume(struct volume *vP)
{
int i;
int end;

    end = ((vP->first + vP->tracks) * DR_TRACKW);
    for( i = (vP->first * DR_TRACKW); i < end; i++ )
    {
        vP->imgP->drum.word[i] &= MASK;
    }
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

// Whether a tape block is blank as the plugin keeps one: the leading checksum -0, the rest zero.
static bool
blockBlank(const struct image *imgP, int blk)
{
int i;

    if( imgP->tape.blk[blk][0] != BLANKCK )
    {
        return(false);
    }
    for( i = 1; i < BLKW; i++ )
    {
        if( imgP->tape.blk[blk][i] != 0 )
        {
            return(false);
        }
    }

    return(true);
}

// A volume block's 0400 data words, wherever the device keeps them.
static uint32_t *
blockData(const struct volume *vP, int blk)
{
    return(vP->imgP->devP->data(vP, blk));
}

// Whether a block's checksum is good, as the control checks it on a read.
static bool
blockOk(const struct volume *vP, int blk)
{
    return(vP->imgP->devP->good(vP, blk));
}

// Write a block's data as the device's control writes it.
static void
putBlock(struct volume *vP, int blk, const uint32_t *dataP)
{
    vP->imgP->devP->put(vP, blk, dataP);
}

// Copy a block whole, as pack moves a file.
static void
copyBlock(struct volume *vP, int to, int from)
{
    vP->imgP->devP->copy(vP, to, from);
}

// Read the tape image into memory, every block past its end blank.
// A missing image is a blank tape when mayBeMissing is true.
// Returns false, with a message, when it cannot be read or is no tape image.
static bool
tapeRead(struct image *imgP, bool mayBeMissing)
{
uint8_t buf[BLKW * 4];
struct stat st;
FILE *fP;
int blk;
int i;

    for( blk = 0; blk < BLKS; blk++ )
    {
        imgP->tape.blk[blk][0] = BLANKCK;
        memset(&imgP->tape.blk[blk][1], 0, ((BLKW - 1) * sizeof(uint32_t)));
    }
    imgP->tape.fileBlks = imgP->tape.topBlks = 0;

    if( !(fP = fopen(imgP->pathP, "rb")) )
    {
        if( (errno == ENOENT) && mayBeMissing )
        {
            return(true);
        }
        fprintf(stderr, "fstool: %s: %s\n", imgP->pathP, strerror(errno));
        return(false);
    }

    // The plugin writes whole blocks only, and no more than a tape's.
    if( (fstat(fileno(fP), &st) != 0) || !S_ISREG(st.st_mode) || ((st.st_size % sizeof(buf)) != 0) ||
        ((st.st_size / (off_t) sizeof(buf)) > BLKS) )
    {
        fprintf(stderr, "fstool: %s: not a tape image, a whole number of %d-byte blocks, at most %d\n",
            imgP->pathP, (int) sizeof(buf), BLKS);
        fclose(fP);
        return(false);
    }

    imgP->tape.fileBlks = (int) (st.st_size / (off_t) sizeof(buf));
    for( blk = 0; blk < imgP->tape.fileBlks; blk++ )
    {
        if( fread(buf, 1, sizeof(buf), fP) != sizeof(buf) )
        {
            fprintf(stderr, "fstool: %s: %s\n", imgP->pathP, ferror(fP) ? strerror(errno) : "short read");
            fclose(fP);
            return(false);
        }
        for( i = 0; i < BLKW; i++ )
        {
            imgP->tape.blk[blk][i] = ((uint32_t) buf[4 * i] | ((uint32_t) buf[(4 * i) + 1] << 8)
                | ((uint32_t) buf[(4 * i) + 2] << 16) | ((uint32_t) buf[(4 * i) + 3] << 24));
            imgP->tape.blk[blk][i] &= MASK;      // just in case
        }
    }

    fclose(fP);
    imgP->tape.topBlks = imgP->tape.fileBlks;

    return(true);
}

// Write the image's topBlks blocks over the image file.
// Returns false with a message on any failure.
static bool
tapeWrite(struct image *imgP)
{
uint8_t *bufP;
size_t n;
int blk;
int i;
bool ok;

    if( !(bufP = malloc(((size_t) imgP->tape.topBlks * BLKW * 4) + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(false);
    }
    n = 0;
    for( blk = 0; blk < imgP->tape.topBlks; blk++ )
    {
        for( i = 0; i < BLKW; i++ )
        {
            bufP[n++] = (uint8_t) (imgP->tape.blk[blk][i] & 0377);
            bufP[n++] = (uint8_t) ((imgP->tape.blk[blk][i] >> 8) & 0377);
            bufP[n++] = (uint8_t) ((imgP->tape.blk[blk][i] >> 16) & 0377);
            bufP[n++] = 0;
        }
    }
    ok = replaceFile(imgP, bufP, n);
    free(bufP);

    return(ok);
}

// A tape block's data words, past its leading checksum.
static uint32_t *
tapeData(const struct volume *vP, int blk)
{
    return(&vP->imgP->tape.blk[blk][1]);
}

// Whether a tape block totals zero, +0 or -0, as the control checks it on a read.
static bool
tapeGood(const struct volume *vP, int blk)
{
int s;

    s = blockTotal(vP->imgP->tape.blk[blk]);

    return((s == 0) || (s == MASK));
}

// Write a tape block as the control writes one forward: -0 leading, the data, then the
// complement of their sum, so the block totals zero.
static void
tapePut(struct volume *vP, int blk, const uint32_t *dataP)
{
uint32_t *bP;

    bP = vP->imgP->tape.blk[blk];
    bP[0] = BLANKCK;
    memcpy(&bP[1], dataP, (WDS * sizeof(uint32_t)));
    bP[BLKW - 1] = 0;
    bP[BLKW - 1] = (((uint32_t) blockTotal(bP)) ^ MASK);
    if( blk >= vP->imgP->tape.topBlks )
    {
        vP->imgP->tape.topBlks = (blk + 1);
    }
}

// Copy a tape block whole, its checksums with it. A blank block past the image's end stays out
// of the image, as an unwritten one reads the same.
static void
tapeCopy(struct volume *vP, int to, int from)
{
struct image *imgP;

    imgP = vP->imgP;
    memmove(imgP->tape.blk[to], imgP->tape.blk[from], sizeof(imgP->tape.blk[0]));
    if( (to >= imgP->tape.topBlks) && !blockBlank(imgP, to) )
    {
        imgP->tape.topBlks = (to + 1);
    }
}

// A tape has one size and shares nothing, so its format has nothing to tell.
static void
tapeFormatting(const struct volume *vP)
{
    (void) vP;
}

// Warn when the tape image is named in the emulator's list of mounted tapes.
static void
tapeWarnMounted(const struct volume *vP)
{
    warnIfListed(vP->imgP->pathP);
}

// Read the drum file into memory, every word past its end zero.
// A missing file is a blank drum when mayBeMissing is true.
// Returns false, with a message, when it cannot be read or there is no drum file.
// A volume's words of more than 18 bits are masked to 18 bits by openVolume, once its tracks
// are known.
static bool
drumRead(struct image *imgP, bool mayBeMissing)
{
uint8_t *bufP;
struct stat st;
FILE *fP;
int i;

    memset(imgP->drum.word, 0, sizeof(imgP->drum.word));
    imgP->drum.fileWords = 0;

    if( !(fP = fopen(imgP->pathP, "rb")) )
    {
        if( (errno == ENOENT) && mayBeMissing )
        {
            return(true);
        }
        fprintf(stderr, "fstool: %s: %s\n", imgP->pathP, strerror(errno));
        return(false);
    }

    // The emulator writes whole words, and no more than the drum's.
    if( (fstat(fileno(fP), &st) != 0) || !S_ISREG(st.st_mode) || ((st.st_size % 4) != 0) ||
        ((st.st_size / 4) > DR_WORDS) )
    {
        fprintf(stderr, "fstool: %s: not a drum file, a whole number of 4-byte words, at most %d\n",
            imgP->pathP, DR_WORDS);
        fclose(fP);
        return(false);
    }

    imgP->drum.fileWords = st.st_size / 4;
    if( !(bufP = malloc((size_t) st.st_size + 1)) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        fclose(fP);
        return(false);
    }
    if( fread(bufP, 1, (size_t) st.st_size, fP) != (size_t) st.st_size )
    {
        fprintf(stderr, "fstool: %s: %s\n", imgP->pathP, ferror(fP) ? strerror(errno) : "short read");
        free(bufP);
        fclose(fP);
        return(false);
    }
    fclose(fP);
    for( i = 0; i < imgP->drum.fileWords; i++ )
    {
        imgP->drum.word[i] = ((uint32_t) bufP[4 * i] | ((uint32_t) bufP[(4 * i) + 1] << 8)
            | ((uint32_t) bufP[(4 * i) + 2] << 16) | ((uint32_t) bufP[(4 * i) + 3] << 24));
    }
    free(bufP);

    return(true);
}

// Write the drum over the drum file: every word as read, the volume's changed, the file grown
// only to the end of the last track written.
// Returns false with a message on any failure.
static bool
drumWrite(struct image *imgP)
{
uint8_t *bufP;
uint32_t w;
int words;
int i;
int t;
bool ok;

    words = imgP->drum.fileWords;
    for( t = 0; t < DR_TRACKS; t++ )
    {
        if( imgP->drum.trackWritten[t] && (((t + 1) * DR_TRACKW) > words) )
        {
            words = ((t + 1) * DR_TRACKW);
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
        w = imgP->drum.word[i];
        bufP[4 * i] = (uint8_t) (w & 0377);
        bufP[(4 * i) + 1] = (uint8_t) ((w >> 8) & 0377);
        bufP[(4 * i) + 2] = (uint8_t) ((w >> 16) & 0377);
        bufP[(4 * i) + 3] = (uint8_t) ((w >> 24) & 0377);
    }
    ok = replaceFile(imgP, bufP, ((size_t) words * 4));
    free(bufP);

    return(ok);
}

// A drum volume block's data words, where the skewed layout puts it.
static uint32_t *
drumData(const struct volume *vP, int blk)
{
int k;

    k = (blk / DR_BPT);

    return(&vP->imgP->drum.word[((vP->first + k) * DR_TRACKW) + ((((blk % DR_BPT) + k) % DR_BPT) * WDS)]);
}

// A drum block has no checksum and is always good.
static bool
drumGood(const struct volume *vP, int blk)
{
    (void) vP;
    (void) blk;

    return(true);
}

// Write a drum block, its data alone, and mark its track to be written back.
static void
drumPut(struct volume *vP, int blk, const uint32_t *dataP)
{
    memcpy(drumData(vP, blk), dataP, (WDS * sizeof(uint32_t)));
    vP->imgP->drum.trackWritten[vP->first + (blk / DR_BPT)] = true;
}

// Copy a drum block, its data alone.
static void
drumCopy(struct volume *vP, int to, int from)
{
    drumPut(vP, to, drumData(vP, from));
}

// Before a drum volume is formatted: how its size was chosen when -b did not give it, and any
// volume sharing its tracks.
static void
drumFormatting(const struct volume *vP)
{
    if( vP->sizing == SIZE_KEPT )
    {
        fprintf(stderr, "fstool: %s: the volume keeps its %d blocks\n", vP->imgP->pathP, vP->blocks);
    }
    else if( vP->sizing == SIZE_DEFAULT )
    {
        fprintf(stderr, "fstool: %s: the volume is the default of %d blocks, one track\n",
            vP->imgP->pathP, vP->blocks);
    }
    warnOverlap(vP);
}

// Warn when the drum file is the emulator's.
static void
drumWarnMounted(const struct volume *vP)
{
    warnIfDrum(vP);
}

// Write len bytes to a temporary file in the image's directory and rename it to the image's
// name, keeping its mode.
// A failure leaves the old file intact.
// Returns false with a message on any failure.
static bool
replaceFile(const struct image *imgP, const uint8_t *bytesP, size_t len)
{
char tmp[PATH_MAX + 16];
struct stat st;
mode_t mode;
mode_t mask;
FILE *fP;
int fd;
bool ok;

    if( stat(imgP->pathP, &st) == 0 )
    {
        mode = (st.st_mode & 07777);
    }
    else
    {
        mask = umask(0);
        umask(mask);
        mode = (0666 & ~mask);
    }

    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", imgP->pathP);
    if( (fd = mkstemp(tmp)) < 0 )
    {
        fprintf(stderr, "fstool: %s: can't create temporary file: %s\n", imgP->pathP, strerror(errno));
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

    if( !ok || (rename(tmp, imgP->pathP) != 0) )
    {
        fprintf(stderr, "fstool: %s: can't write the %s: %s\n", imgP->pathP, imgP->devP->fileP,
            strerror(errno));
        unlink(tmp);
        return(false);
    }

    return(true);
}

// Whether a directory block's data is this file system's: the magic, the version and the
// volume's blocks, as the library's fsDirOk checks them.
static bool
dirOk(const struct volume *vP, const uint32_t *dataP)
{
    return((dataP[DMAGIC] == MAGIC) && (dataP[DVER] == VERSION) && (dataP[DBLKS] == (uint32_t) vP->blocks));
}

// Find the directory as the library's mount does: block 1 if it totals zero and is the file
// system's, else block 2, copied into block 1's place in memory.
// On the drum, a directory whose recorded first track is not the volume's is refused, as drMount refuses it.
// Returns true, or false after reporting NOFS.
static bool
mountDir(struct volume *vP)
{
char detail[160];
uint32_t *dP;
int blk;

    // A drum volume of no blocks is one no directory sized; dirOk would take a count of 0.
    for( blk = DIRBLK; (blk <= DIRBLK2) && (vP->blocks > 0); blk++ )
    {
        if( blockOk(vP, blk) && dirOk(vP, blockData(vP, blk)) )
        {
            vP->dirFrom = blk;
            if( blk != DIRBLK )
            {
                // The first directory block was invalid, but the second is ok,
                // copy it into the first in memory.
                memcpy(blockData(vP, DIRBLK), blockData(vP, blk), (WDS * sizeof(uint32_t)));
            }

            vP->dirP = blockData(vP, DIRBLK);

            if( vP->imgP->devP->firstInDir && (vP->dirP[DDEV] != (uint32_t) vP->first) )
            {
                snprintf(detail, sizeof(detail), "the directory records the volume's first track as "
                    "%d, not %d", vP->dirP[DDEV], vP->first);
                libError(vP, E_NOFS, NULL, detail);
                return(false);
            }

            return(true);
        }
    }

    // On the drum, a directory that is the file system's but was refused says why.
    for( blk = DIRBLK; vP->imgP->devP->firstInDir && (blk <= DIRBLK2); blk++ )
    {
        dP = blockData(vP, blk);
        if( (dP[DMAGIC] != MAGIC) || (dP[DVER] != VERSION) )
        {
            continue;
        }
        if( dP[DDEV] != (uint32_t) vP->first )
        {
            snprintf(detail, sizeof(detail), "the directory in block %d records the volume's first "
                "track as %d, not %d", blk, dP[DDEV], vP->first);
        }
        else
        {
            snprintf(detail, sizeof(detail), "the directory in block %d is for %d blocks, not whole "
                "tracks of %d within the drum", blk, dP[DBLKS], DR_BPT);
        }
        libError(vP, E_NOFS, NULL, detail);
        return(false);
    }
    libError(vP, E_NOFS, NULL, NULL);

    return(false);
}

// Write the directory as both copies, blocks 1 then 2, increasing the generation, as the
// library's fsWrDir does.
static void
writeDir(struct volume *vP)
{
uint32_t data[WDS];

    vP->dirP[DGEN] = ((vP->dirP[DGEN] + 1) & GENMASK);
    memcpy(data, vP->dirP, sizeof(data));
    putBlock(vP, DIRBLK, data);
    putBlock(vP, DIRBLK2, data);
    vP->dirP = blockData(vP, DIRBLK);
    vP->imgP->changed = true;
}

// Warn when the drum volume about to be formatted shares tracks with another volume: a file
// added to either would overwrite the other's blocks.
static void
warnOverlap(const struct volume *vP)
{
int t;
int n;
int end;

    end = (vP->first + vP->tracks);
    for( t = 0; t < DR_TRACKS; t++ )
    {
        if( (t == vP->first) || ((n = (volumeAt(vP->imgP, t, NULL) / DR_BPT)) == 0) )
        {
            continue;
        }
        if( (t < end) && ((t + n) > vP->first) )
        {
            fprintf(stderr, "fstool: %s: warning: the volume at tracks %d-%d shares tracks with this "
                "one; fstool -D all -l lists the volumes\n", vP->nameP, t, ((t + n) - 1));
        }
    }
}

// The directory entry of a slot, 0 to NENT-1.
static uint32_t *
entry(const struct volume *vP, int slot)
{
    return(&vP->dirP[DENT0 + (ENTW * slot)]);
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
lookup(const struct volume *vP, const uint32_t *nmP, int *slotP, int *freeP)
{
uint32_t *eP;
int slot;

    *freeP = -1;
    for( slot = 0; slot < NENT; slot++ )
    {
        eP = entry(vP, slot);
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
freeSpace(const struct volume *vP, int need, int *totalP, int *largestP, int *fitP)
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
        first = vP->blocks;
        alloc = 0;
        for( slot = 0; slot < NENT; slot++ )
        {
            eP = entry(vP, slot);
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
        if( first == vP->blocks )
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
// *countP their count, or NULL with a message. A file too large for a file on the volume is
// RANGE.
static uint32_t *
sourceWords(const struct volume *vP, const char *pathP, enum form f, int *countP)
{
uint8_t *bytesP;
uint32_t *wordsP;
size_t len;
size_t i;
size_t n;
int count;
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
        count = len / 4;
    }
    else
    {
        count = (len + 1) / 2;
    }
    if( count > (MAXBLKS * WDS) )
    {
        libError(vP, E_RANGE, NULL, "the source is larger than a file's 0777 maximum blocks");
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
                fprintf(stderr, "fstool: %s: word %d is octal 0%o, larger than 18 bits\n", pathP, (int)i, w);
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
writeHost(const char *pathP, const uint32_t *wordsP, int count, enum form f)
{
uint8_t *bufP;
FILE *fP;
size_t n;
int i;
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
            fprintf(stderr, "fstool: %s: warning: the tape is mounted on drive %d in %s.\n"
                "A loaded program that has it mounted can overwrite this change.\n",
                pathP, drive, MTFS_LIST);
            break;
        }
    }
    fclose(fP);
}

// Warn when the drum file is the emulator's: a program holding the volume mounted keeps its
// own directory, and its next directory write would undo this change.
static void
warnIfDrum(const struct volume *vP)
{
char mine[PATH_MAX + 2];
char theirs[PATH_MAX + 2];

    if( canonPath(vP->imgP->pathP, mine, sizeof(mine)) && canonPath(DRUMFILE, theirs, sizeof(theirs))
        && (strcmp(mine, theirs) == 0) )
    {
        fprintf(stderr, "fstool: %s: warning: this is the emulator's drum file.\n"
            "A loaded program that has the volume mounted can overwrite this change.\n", vP->nameP);
    }
}

// Report one of the library's status codes for the volume, and the file named if any, with
// its meaning or the detail given.
// Returns 1, the exit status.
static int
libError(const struct volume *vP, int code, const char *nameP, const char *detailP)
{
    return(statusError(vP->nameP, vP->imgP->devP->nounP, code, nameP, detailP));
}

// Report a status code for whereP, the volume or the image, and the file named if any, with
// its meaning, naming the device by nounP, or the detail given.
// Returns 1, the exit status.
static int
statusError(const char *whereP, const char *nounP, int code, const char *nameP,
    const char *detailP)
{
char meaning[128];

    fprintf(stderr, "fstool: %s: ", whereP);
    if( nameP )
    {
        fprintf(stderr, "%s: ", nameP);
    }
    snprintf(meaning, sizeof(meaning), errMeaning[code], nounP);
    fprintf(stderr, "%s, %s\n", errName[code], (detailP ? detailP : meaning));

    return(1);
}

// List the directory with warnings for what the library routines report as warnings.
static int
cmdList(struct volume *vP)
{
char name[NAMELEN + 1];
uint32_t *eP;
int total;
int largest;
int fit;
int slot;
int blk;
int last;

    if( !mountDir(vP) )
    {
        return(1);
    }
    if( vP->dirFrom != DIRBLK )
    {
        fprintf(stderr, "fstool: %s: warning: directory block 1 is bad; the copy in block 2 was "
            "read\n", vP->nameP);
    }

    freeSpace(vP, 0, &total, &largest, &fit);
    printf("%s: generation %d, %d blocks, %d free, largest gap %d\n", vP->nameP,
        vP->dirP[DGEN], vP->dirP[DBLKS], total, largest);
    printf("slot  name       type  app  ro  first  blocks    words\n");
    for( slot = 0; slot < NENT; slot++ )
    {
        eP = entry(vP, slot);
        if( eP[0] == 0 )
        {
            continue;
        }
        unpackName(eP, name);
        printf("%4d  %-9s  %4d  0%02o  %-2s  %5d  %6d  %7d\n", slot, name,
            (eP[DEINFO] & ITYPE), ((eP[DEINFO] & IAPP) >> IAPPSHIFT),
            ((eP[DEINFO] & IRO) ? "ro" : ""), eP[DEFIRST], eP[DEALLOC], eP[DELEN]);

        if( eP[DEMOVE] != 0 )
        {
            fprintf(stderr, "fstool: %s: %s: warning: marked as moving to block %d; a pack was "
                "stopped while moving it\n", vP->nameP, name, eP[DEMOVE]);
        }
        last = (int) (eP[DEFIRST] + ((eP[DELEN] + WDS - 1) / WDS));
        for( blk = (int) eP[DEFIRST]; (blk < last) && (blk < vP->blocks); blk++ )
        {
            if( !blockOk(vP, blk) )
            {
                fprintf(stderr, "fstool: %s: %s: warning: block %d does not total zero, a checksum error\n",
                    vP->nameP, name, blk);
            }
        }
    }

    return(0);
}

// Add a host file as -n name: a free slot and the first gap large enough, as the library's
// mtfCreate finds them, then its words and the directory.
// With -F a file of that name is replaced.
// Returns the exit status.
static int
cmdAdd(struct volume *vP, const struct options *oP)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
uint32_t data[WDS];
uint32_t *wordsP;
uint32_t *eP;
const char *nameP;
int blocks;
int count;
int done;
int need;
int slot;
int freeSlot;
int total;
int largest;
int fit;
int blk;
int status;

    if( (status = packName(oP->nameP, nm)) != E_OK )
    {
        return(libError(vP, status, oP->nameP, NULL));
    }
    // Messages name the file as it is saved, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir(vP) )
    {
        return(1);
    }
    if( !(wordsP = sourceWords(vP, oP->sourceP, oP->f, &count)) )
    {
        return(1);
    }

    // The checks in the library's order: the name, a slot, the size, then the room.
    if( lookup(vP, nm, &slot, &freeSlot) == E_OK )
    {
        if( !oP->force )
        {
            free(wordsP);
            return(libError(vP, E_EXISTS, nameP, NULL));
        }
        memset(entry(vP, slot), 0, (ENTW * sizeof(uint32_t)));
        lookup(vP, nm, &slot, &freeSlot);
    }
    if( freeSlot < 0 )
    {
        free(wordsP);
        return(libError(vP, E_DIRFULL, nameP, NULL));
    }
    need = (int) ((count + WDS - 1) / WDS);
    if( need == 0 )
    {
        need = 1;
    }
    blocks = ((oP->blocks < 0) ? need : oP->blocks);
    if( (blocks == 0) || (blocks > MAXBLKS) )
    {
        free(wordsP);
        return(libError(vP, E_RANGE, nameP, "a file is 1 to 0777 blocks"));
    }
    if( blocks < need )
    {
        free(wordsP);
        return(libError(vP, E_RANGE, nameP, "-b gives fewer blocks than the data needs"));
    }
    freeSpace(vP, blocks, &total, &largest, &fit);
    if( fit < 0 )
    {
        free(wordsP);
        return(libError(vP, E_NOSPACE, nameP, NULL));
    }

    eP = entry(vP, freeSlot);
    eP[0] = nm[0];
    eP[1] = nm[1];
    eP[2] = nm[2];
    eP[DEFIRST] = (uint32_t) fit;
    eP[DEALLOC] = (uint32_t) blocks;
    eP[DELEN] = (uint32_t) count;
    eP[DEINFO] = ((uint32_t) oP->type | (oP->readOnly ? IRO : 0));
    eP[DEMOVE] = 0;

    // Only the blocks that hold data are written, the rest of the allocation is unchanged.
    for( blk = fit, done = 0; done < count; blk++, done += WDS )
    {
        memset(data, 0, sizeof(data));
        memcpy(data, &wordsP[done], ((size_t) (((count - done) < WDS) ? (count - done) : WDS)
            * sizeof(uint32_t)));
        putBlock(vP, blk, data);
    }
    free(wordsP);
    writeDir(vP);

    return(0);
}

// Delete name: its entry cleared and the directory written; its blocks are not erased.
// A read-only file needs force.
// Returns the exit status.
static int
cmdDelete(struct volume *vP, const char *nameP, bool force)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
int slot;
int freeSlot;
int status;

    if( (status = packName(nameP, nm)) != E_OK )
    {
        return(libError(vP, status, nameP, NULL));
    }
    // Messages name the file as it is stored, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir(vP) )
    {
        return(1);
    }
    if( lookup(vP, nm, &slot, &freeSlot) != E_OK )
    {
        return(libError(vP, E_NOTFOUND, nameP, NULL));
    }
    if( (entry(vP, slot)[DEINFO] & IRO) && !force )
    {
        return(libError(vP, E_READONLY, nameP, NULL));
    }
    memset(entry(vP, slot), 0, (ENTW * sizeof(uint32_t)));
    writeDir(vP);

    return(0);
}

// Extract name's words bounded by its length to a host file.
// A tape block that does not total zero is BADBLOCK; a drum block has no checksum.
// Returns the exit status.
static int
cmdExtract(struct volume *vP, const char *nameP, enum form f, const char *outputP)
{
uint32_t nm[3];
char shown[NAMELEN + 1];
uint32_t *eP;
uint32_t *wordsP;
int count;
int i;
int slot;
int freeSlot;
int blk;
int status;
char detail[128];

    if( (status = packName(nameP, nm)) != E_OK )
    {
        return(libError(vP, status, nameP, NULL));
    }
    // Messages name the file as it is kept, in upper case.
    unpackName(nm, shown);
    nameP = shown;
    if( !mountDir(vP) )
    {
        return(1);
    }
    if( lookup(vP, nm, &slot, &freeSlot) != E_OK )
    {
        return(libError(vP, E_NOTFOUND, nameP, NULL));
    }
    eP = entry(vP, slot);
    count = (int) eP[DELEN];
    if( (count > (int) (eP[DEALLOC] * WDS)) || ((int) (eP[DEFIRST] + eP[DEALLOC]) > vP->blocks) )
    {
        snprintf(detail, sizeof(detail), "the entry's length or blocks are past its allocation "
            "or the end of the %s", vP->imgP->devP->nounP);
        return(libError(vP, E_RANGE, nameP, detail));
    }

    if( !(wordsP = calloc((size_t) (count + 1), sizeof(uint32_t))) )
    {
        fprintf(stderr, "fstool: out of memory\n");
        return(1);
    }
    for( i = 0; i < count; i++ )
    {
        blk = (int) (eP[DEFIRST] + (uint32_t) (i / WDS));
        if( ((i % WDS) == 0) && !blockOk(vP, blk) )
        {
            snprintf(detail, sizeof(detail), "block %d does not total zero, a checksum error", blk);
            free(wordsP);
            return(libError(vP, E_BADBLOCK, nameP, detail));
        }
        wordsP[i] = blockData(vP, blk)[i % WDS];
    }
    status = (writeHost(outputP, wordsP, count, f) ? 0 : 1);
    free(wordsP);

    return(status);
}

// Format: an empty directory of the volume's blocks at generation 1, with the volume's first
// track, 0 on a tape.
// Unless forced, refused with FOREIGN when blocks 1 and 2 hold another program's data.
// One must be this file system's, of the volume's blocks or on the drum recording its first
// track, or both total zero with only zero data, blank blocks; never-written drum tracks read
// as zeros.
// File blocks are not touched.
// Returns the exit status.
static int
cmdFormat(struct volume *vP, bool force, bool existed)
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
            if( !blockOk(vP, blk) )
            {
                continue;
            }

            // On the drum, a directory of this volume is ours whatever its size, so a volume
            // is reformatted to another size without -F.
            bP = blockData(vP, blk);
            if( dirOk(vP, bP) || (vP->imgP->devP->firstInDir && (bP[DMAGIC] == MAGIC)
                && (bP[DVER] == VERSION) && (bP[DDEV] == (uint32_t) vP->first)) )
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
            return(libError(vP, E_FOREIGN, NULL, "blocks 1 and 2 hold another program's data; -F "
                "formats anyway"));
        }
    }

    memset(data, 0, sizeof(data));
    data[DMAGIC] = MAGIC;
    data[DVER] = VERSION;
    data[DGEN] = 0;
    data[DBLKS] = (uint32_t) vP->blocks;
    data[DDEV] = (uint32_t) vP->first;
    memcpy(blockData(vP, DIRBLK), data, sizeof(data));
    vP->dirP = blockData(vP, DIRBLK);
    writeDir(vP);

    return(0);
}

// Pack: each file in order of first block is moved down to the
// lowest free block, a file marked as already moving left where it is.
// All in memory, so the directory is written once and no file is ever marked.
// Returns the exit status.
static int
cmdPack(struct volume *vP)
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

    if( !mountDir(vP) )
    {
        return(1);
    }

    moved = 0;
    pos = FIRSTBLK;
    for( iter = 0; iter <= NENT; iter++ )
    {
        selP = NULL;
        first = vP->blocks;
        for( slot = 0; slot < NENT; slot++ )
        {
            eP = entry(vP, slot);
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
            for( i = 0; (i < alloc) && ((first + i) < vP->blocks); i++ )
            {
                copyBlock(vP, (pos + i), (first + i));
            }
            selP[DEFIRST] = (uint32_t) pos;
            first = pos;
            moved++;
        }
        pos = (first + alloc);
    }

    if( moved == 0 )
    {
        printf("%s: no gaps to close\n", vP->nameP);
        return(0);
    }
    writeDir(vP);
    printf("%s: %d file%s moved\n", vP->nameP, moved, ((moved == 1) ? "" : "s"));

    return(0);
}

// List every volume on the drum file, each track whose block 1 or 2 holds a file
// system directory recording that track as its first, with blocks for whole tracks within
// the drum. Each is a volume of its own, its directory read where it was found; a listing
// changes nothing, so the words are left as they are.
// A volume overlapping one listed before it, which indicates a directory left from an earlier format, gives a warning.
// Returns the exit status, NOFS when no track starts a volume.
static int
cmdScan(struct image *imgP)
{
struct volume vol;
int blocks;
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
        blk = DIRBLK;
        if( (blocks = volumeAt(imgP, t, &blk)) == 0 )
        {
            continue;
        }

        memset(&vol, 0, sizeof(vol));
        vol.imgP = imgP;
        sizeVolume(&vol, t, blocks);
        vol.dirFrom = blk;
        vol.dirP = blockData(&vol, blk);
        freeSpace(&vol, 0, &total, &largest, &fit);
        files = 0;
        for( slot = 0; slot < NENT; slot++ )
        {
            if( entry(&vol, slot)[0] != 0 )
            {
                files++;
            }
        }
        if( vol.tracks == 1 )
        {
            printf("%s: track %3d, ", imgP->pathP, t);
        }
        else
        {
            printf("%s: tracks %3d-%3d, ", imgP->pathP, t, ((t + vol.tracks) - 1));
        }
        printf("generation %d, %d blocks, %d file%s, %d free, largest gap %d\n", vol.dirP[DGEN],
            vol.blocks, files, ((files == 1) ? "" : "s"), total, largest);
        if( blk != DIRBLK )
        {
            fprintf(stderr, "fstool: %s: track %3d: warning: directory block 1 is not the file "
                "system's; the copy in block 2 was read\n", imgP->pathP, t);
        }
        if( t < lastEnd )
        {
            fprintf(stderr, "fstool: %s: track %3d: warning: inside the volume listed before it, "
                "which ends at track %3d; one of the two is left from an earlier format\n",
                imgP->pathP, t, (lastEnd - 1));
        }
        if( (t + vol.tracks) > lastEnd )
        {
            lastEnd = (t + vol.tracks);
        }
        found++;
    }

    if( found == 0 )
    {
        return(statusError(imgP->pathP, imgP->devP->nounP, E_NOFS, NULL,
            "no track starts a file system volume"));
    }

    return(0);
}
