// dsk1301 is the host tool for IBM 1301 disk images, the files the IOT_10 plugin keeps one per
// module.
// It makes an image, formats cylinders, sets HA2 (the customer engineer's job on the real
// drive), dumps a cylinder or a track, and copies a record to or from a host file.
// It works on the file in place, so a change it makes while the emulator runs is seen at once
// through the plugin's shared mapping.
// The layout is ibm1301image.h's.
//
// 23-Nov-2026 Claude written.
// 23-Nov-2026 Claude comments cleaned for release.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ibm1301image.h"

// The 7090's BCD characters by six-bit value, '~' where the code has no character.
static const char bcdText[65] =
    "0123456789~#@~~~&ABCDEFGHI?.)~~~-JKLMNOPQR!$*~~~ /STUVWXYZ~,(~~~";

static const char *progP = "dsk1301";

static void usage(void);
static int openImage(const char *pathP, int flags, unsigned char *hdrP);
static bool readSlot(int fd, int cyl, int slot, unsigned char *bufP);
static bool readMap(int fd, int cyl, Ibm1301Map *mapP, unsigned char *fmtP, int *lenP);
static bool parseRange(const char *argP, int max, int *firstP, int *lastP);
static bool parseInt(const char *argP, int lo, int hi, int *valP);
static void dumpChars(const char *labelP, const unsigned char *p, int n);
static int cmdMake(int argc, char **argv);
static int cmdFormat(int argc, char **argv);
static int cmdHa2(int argc, char **argv);
static int cmdDump(int argc, char **argv);
static int cmdPutGet(int argc, char **argv, bool put);

// Runs the command named by the first argument.
// Returns 0 on success, 1 on any failure, 2 for a usage error.
int
main(int argc, char **argv)
{
    if( argc < 2 )
    {
        usage();
        return( 2 );
    }

    if( strcmp(argv[1], "make") == 0 )
    {
        return( cmdMake(argc - 1, argv + 1) );
    }

    if( strcmp(argv[1], "format") == 0 )
    {
        return( cmdFormat(argc - 1, argv + 1) );
    }

    if( strcmp(argv[1], "ha2") == 0 )
    {
        return( cmdHa2(argc - 1, argv + 1) );
    }

    if( strcmp(argv[1], "dump") == 0 )
    {
        return( cmdDump(argc - 1, argv + 1) );
    }

    if( strcmp(argv[1], "put") == 0 )
    {
        return( cmdPutGet(argc - 1, argv + 1, true) );
    }

    if( strcmp(argv[1], "get") == 0 )
    {
        return( cmdPutGet(argc - 1, argv + 1, false) );
    }

    usage();
    return( 2 );
}

// Prints the usage summary to stderr.
static void
usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  %s make [-b] [-f] FILE MODULE\n"
        "      a fresh image of module 0-9: every cylinder six-bit, HA2 00 00, five records\n"
        "      of 528 characters with addresses tttt0r; -b leaves it blank, unformatted;\n"
        "      -f replaces an existing file\n"
        "  %s format FILE CYL[-CYL] 6|8 HA2 RA RECORD COUNT\n"
        "      formats cylinders: the mode, HA2's length, each record address's and record's\n"
        "      length, and the records on a track; HA2 and the records are set to 0, the\n"
        "      addresses to tttt0r as a fresh image has them\n"
        "  %s ha2 FILE TRACK[-TRACK] C,C,...\n"
        "      sets HA2 on tracks, one octal character value for each of HA2's characters\n"
        "  %s dump FILE CYL [TRACK]\n"
        "      the cylinder's format, or a track's areas, in octal and as 7090 BCD\n"
        "  %s put FILE TRACK RECORD HOSTFILE\n"
        "  %s get FILE TRACK RECORD HOSTFILE\n"
        "      copy record 1 up of a track from or to a host file, one byte per character;\n"
        "      put fills a short record out with 0\n",
        progP, progP, progP, progP, progP, progP);
}

// Opens an image and checks its header and size, any module.
// Returns the descriptor, or -1 after saying why on stderr.
static int
openImage(const char *pathP, int flags, unsigned char *hdrP)
{
struct stat st;
const char *whyP;
ssize_t n;
int fd;

    if( (fd = open(pathP, flags)) < 0 )
    {
        fprintf(stderr, "%s: %s: %s\n", progP, pathP, strerror(errno));
        return( -1 );
    }

    n = pread(fd, hdrP, IBM1301_HEADERSIZE, 0);
    whyP = NULL;
    if( n != IBM1301_HEADERSIZE )
    {
        whyP = "too short to be an IBM 1301 image";
    }
    else if( (whyP = ibm1301CheckHeader(hdrP, -1)) == NULL )
    {
        if( (fstat(fd, &st) != 0) || (st.st_size != IBM1301_FILESIZE) )
        {
            whyP = "an IBM 1301 image of the wrong size";
        }
        else if( ibm1301Get32(hdrP + 12) >= IBM1301_MAXMODULES )
        {
            whyP = "an image whose module number is out of range";
        }
    }

    if( whyP )
    {
        fprintf(stderr, "%s: %s: %s, refused\n", progP, pathP, whyP);
        close(fd);
        return( -1 );
    }

    return( fd );
}

// Reads one slot of a cylinder into bufP, IBM1301_SLOTSIZE bytes.
// Returns true on success, else false after saying why.
static bool
readSlot(int fd, int cyl, int slot, unsigned char *bufP)
{
    if( pread(fd, bufP, IBM1301_SLOTSIZE, ibm1301SlotOffset(cyl, slot)) != IBM1301_SLOTSIZE )
    {
        fprintf(stderr, "%s: reading cylinder %d: %s\n", progP, cyl, strerror(errno));
        return( false );
    }

    return( true );
}

// Reads a cylinder's format record and its map; fmtP gets the record, *lenP its length.
// A cylinder never formatted gives a map with formatted false.
// Returns false if the slot cannot be read or holds a record the plugin would refuse.
static bool
readMap(int fd, int cyl, Ibm1301Map *mapP, unsigned char *fmtP, int *lenP)
{
unsigned char slot[IBM1301_SLOTSIZE];
int len;

    if( !readSlot(fd, cyl, 0, slot) )
    {
        return( false );
    }

    len = ibm1301FormatLength(slot);
    if( len > IBM1301_FORMATMAX )
    {
        fprintf(stderr, "%s: cylinder %d: format length %d is damaged\n", progP, cyl, len);
        return( false );
    }

    memcpy(fmtP, slot + 2, (size_t)len);
    *lenP = len;
    memset(mapP, 0, sizeof(*mapP));
    if( len == 0 )
    {
        return( true );
    }

    if( ibm1301ParseFormat(fmtP, len, mapP) != IBM1301_FMT_OK )
    {
        fprintf(stderr, "%s: cylinder %d: the format record is damaged\n", progP, cyl);
        return( false );
    }

    return( true );
}

// Reads "N" or "N-M", decimal, within 0..max.
// Returns true and the range, else false.
static bool
parseRange(const char *argP, int max, int *firstP, int *lastP)
{
char *endP;
long a;
long b;

    errno = 0;
    a = strtol(argP, &endP, 10);
    b = a;
    if( (endP != argP) && (*endP == '-') )
    {
        argP = endP + 1;
        b = strtol(argP, &endP, 10);
    }

    if( (errno != 0) || (endP == argP) || (*endP != '\0') || (a < 0) || (b < a) || (b > max) )
    {
        return( false );
    }

    *firstP = (int)a;
    *lastP = (int)b;
    return( true );
}

// Reads a decimal integer within lo..hi.
// Returns true and the value, else false.
static bool
parseInt(const char *argP, int lo, int hi, int *valP)
{
int a;
int b;

    if( !parseRange(argP, hi, &a, &b) || (a != b) || (a < lo) )
    {
        return( false );
    }

    *valP = a;
    return( true );
}

// Prints n characters under a label, 16 to a line, in octal and as 7090 BCD.
static void
dumpChars(const char *labelP, const unsigned char *p, int n)
{
char text[17];
int i;
int j;

    printf("%s, %d characters\n", labelP, n);
    for( i = 0; i < n; i += 16 )
    {
        printf("  %4d:", i);
        for( j = 0; j < 16; ++j )
        {
            if( (i + j) < n )
            {
                printf(" %03o", p[i + j]);
                text[j] = (p[i + j] < 64) ? bcdText[p[i + j]] : '~';
            }
            else
            {
                printf("    ");
                text[j] = '\0';
            }
        }

        text[16] = '\0';
        printf("  |%s|\n", text);
    }
}

// dsk1301 make [-b] [-f] FILE MODULE creates a module's image, fresh or blank.
// Returns 0 on success, 1 on a failure, 2 for a usage error.
static int
cmdMake(int argc, char **argv)
{
bool blank;
bool replace;
int module;
int i;

    blank = false;
    replace = false;
    for( i = 1; (i < argc) && (argv[i][0] == '-'); ++i )
    {
        if( strcmp(argv[i], "-b") == 0 )
        {
            blank = true;
        }
        else if( strcmp(argv[i], "-f") == 0 )
        {
            replace = true;
        }
        else
        {
            usage();
            return( 2 );
        }
    }

    if( ((argc - i) != 2) || !parseInt(argv[i + 1], 0, (IBM1301_MAXMODULES - 1), &module) )
    {
        usage();
        return( 2 );
    }

    if( !ibm1301CreateImage(argv[i], module, !blank, replace) )
    {
        fprintf(stderr, "%s: %s: %s%s\n", progP, argv[i], strerror(errno),
            (errno == EEXIST) ? " (-f replaces it)" : "");
        return( 1 );
    }

    return( 0 );
}

// dsk1301 format FILE CYL[-CYL] 6|8 HA2 RA RECORD COUNT formats cylinders with equal records.
// Returns 0 on success, 1 on a failure, 2 for a usage error.
static int
cmdFormat(int argc, char **argv)
{
unsigned char hdr[IBM1301_HEADERSIZE];
unsigned char slot[IBM1301_SLOTSIZE];
Ibm1301Map map;
int first;
int last;
int mode;
int ha2;
int ra;
int rec;
int count;
int len;
int cyl;
int head;
int fd;

    if( (argc != 8) || !parseRange(argv[2], (IBM1301_CYLINDERS - 1), &first, &last) ||
        !parseInt(argv[3], 6, 8, &mode) || (mode == 7) || !parseInt(argv[4], 2, 4000, &ha2) ||
        !parseInt(argv[5], 6, 4000, &ra) || !parseInt(argv[6], 2, 4000, &rec) ||
        !parseInt(argv[7], 1, IBM1301_MAXRECORDS, &count) )
    {
        usage();
        return( 2 );
    }

    memset(slot, 0, sizeof(slot));
    len = ibm1301BuildFormat(slot + 2, (mode == 8), ha2, ra, rec, count);
    if( len == 0 )
    {
        fprintf(stderr, "%s: that layout does not fit a %d-bit track\n", progP, mode);
        return( 1 );
    }

    ibm1301SetFormatLength(slot, len);
    ibm1301ParseFormat(slot + 2, len, &map);
    if( (fd = openImage(argv[1], O_RDWR, hdr)) < 0 )
    {
        return( 1 );
    }

    for( cyl = first; cyl <= last; ++cyl )
    {
        if( !ibm1301WriteAt(fd, slot, IBM1301_SLOTSIZE, ibm1301SlotOffset(cyl, 0)) )
        {
            fprintf(stderr, "%s: writing cylinder %d: %s\n", progP, cyl, strerror(errno));
            close(fd);
            return( 1 );
        }
    }

    // The format slot is done; each track gets HA2 and records of 0 and the fresh addresses.
    memset(slot, 0, sizeof(slot));
    for( cyl = first; cyl <= last; ++cyl )
    {
        for( head = 0; head < IBM1301_HEADS; ++head )
        {
            ibm1301DefaultAddresses(slot, &map, ((cyl * IBM1301_HEADS) + head));
            if( !ibm1301WriteAt(fd, slot, IBM1301_SLOTSIZE, ibm1301SlotOffset(cyl, (head + 1))) )
            {
                fprintf(stderr, "%s: writing cylinder %d: %s\n", progP, cyl, strerror(errno));
                close(fd);
                return( 1 );
            }
        }
    }

    close(fd);
    return( 0 );
}

// dsk1301 ha2 FILE TRACK[-TRACK] C,C,... sets the tracks' HA2 characters.
// Returns 0 on success, 1 on a failure, 2 for a usage error.
static int
cmdHa2(int argc, char **argv)
{
unsigned char hdr[IBM1301_HEADERSIZE];
unsigned char fmt[IBM1301_SLOTSIZE];
unsigned char chars[IBM1301_SLOTSIZE];
Ibm1301Map map;
const char *p;
char *endP;
long v;
int n;
int len;
int first;
int last;
int track;
int fd;
int cylRead;

    if( (argc != 4) || !parseRange(argv[2], (IBM1301_TRACKS - 1), &first, &last) )
    {
        usage();
        return( 2 );
    }

    for( n = 0, p = argv[3]; *p; ++n )
    {
        errno = 0;
        v = strtol(p, &endP, 8);
        if( (errno != 0) || (endP == p) || (v < 0) || (v > 0377) || (n >= IBM1301_SLOTSIZE) ||
            ((*endP != ',') && (*endP != '\0')) )
        {
            fprintf(stderr, "%s: HA2 is octal character values separated by commas\n", progP);
            return( 2 );
        }

        chars[n] = (unsigned char)v;
        p = (*endP == ',') ? (endP + 1) : endP;
    }

    if( (fd = openImage(argv[1], O_RDWR, hdr)) < 0 )
    {
        return( 1 );
    }

    cylRead = -1;
    for( track = first; track <= last; ++track )
    {
        if( (track / IBM1301_HEADS) != cylRead )
        {
            cylRead = track / IBM1301_HEADS;
            if( !readMap(fd, cylRead, &map, fmt, &len) )
            {
                close(fd);
                return( 1 );
            }
        }

        if( !map.formatted || (map.ha2.n != n) )
        {
            fprintf(stderr, "%s: track %d: its cylinder's HA2 is %d characters, not %d\n",
                progP, track, (map.formatted ? map.ha2.n : 0), n);
            close(fd);
            return( 1 );
        }

        if( !ibm1301WriteAt(fd, chars, (size_t)n,
            ibm1301SlotOffset(cylRead, ((track % IBM1301_HEADS) + 1))) )
        {
            fprintf(stderr, "%s: writing track %d: %s\n", progP, track, strerror(errno));
            close(fd);
            return( 1 );
        }
    }

    close(fd);
    return( 0 );
}

// dsk1301 dump FILE CYL [TRACK] prints a cylinder's format, or a track's areas.
// Returns 0 on success, 1 on a failure, 2 for a usage error.
static int
cmdDump(int argc, char **argv)
{
unsigned char hdr[IBM1301_HEADERSIZE];
unsigned char fmt[IBM1301_SLOTSIZE];
unsigned char trk[IBM1301_SLOTSIZE];
char label[64];
Ibm1301Map map;
int cyl;
int track;
int len;
int r;
int fd;

    if( ((argc != 3) && (argc != 4)) || !parseInt(argv[2], 0, (IBM1301_CYLINDERS - 1), &cyl) )
    {
        usage();
        return( 2 );
    }

    track = -1;
    if( (argc == 4) && (!parseInt(argv[3], 0, (IBM1301_TRACKS - 1), &track) ||
        ((track / IBM1301_HEADS) != cyl)) )
    {
        fprintf(stderr, "%s: track %s is not in cylinder %d\n", progP, argv[3], cyl);
        return( 2 );
    }

    if( (fd = openImage(argv[1], O_RDONLY, hdr)) < 0 )
    {
        return( 1 );
    }

    if( !readMap(fd, cyl, &map, fmt, &len) )
    {
        close(fd);
        return( 1 );
    }

    printf("module %u, cylinder %d, tracks %d-%d\n", ibm1301Get32(hdr + 12), cyl,
        (cyl * IBM1301_HEADS), ((cyl * IBM1301_HEADS) + IBM1301_HEADS - 1));
    if( !map.formatted )
    {
        printf("never formatted\n");
        close(fd);
        return( 0 );
    }

    printf("%s, HA2 %d, %d records:", (map.eightBit ? "eight-bit" : "six-bit"), map.ha2.n, map.count);
    for( r = 0; r < map.count; ++r )
    {
        printf(" %d/%d", map.ra[r].n, map.rec[r].n);
    }
    printf(" (address/record)\n");

    if( track < 0 )
    {
        dumpChars("format record", fmt, len);
        close(fd);
        return( 0 );
    }

    if( pread(fd, trk, IBM1301_SLOTSIZE, ibm1301SlotOffset(cyl, ((track % IBM1301_HEADS) + 1))) !=
        IBM1301_SLOTSIZE )
    {
        fprintf(stderr, "%s: reading track %d: %s\n", progP, track, strerror(errno));
        close(fd);
        return( 1 );
    }

    printf("track %d\n", track);
    dumpChars("HA2", trk, map.ha2.n);
    for( r = 0; r < map.count; ++r )
    {
        snprintf(label, sizeof(label), "record %d address", (r + 1));
        dumpChars(label, (trk + map.ra[r].offset), map.ra[r].n);
        snprintf(label, sizeof(label), "record %d", (r + 1));
        dumpChars(label, (trk + map.rec[r].offset), map.rec[r].n);
    }

    close(fd);
    return( 0 );
}

// dsk1301 put|get FILE TRACK RECORD HOSTFILE copies a record from or to a host file.
// Returns 0 on success, 1 on a failure, 2 for a usage error.
static int
cmdPutGet(int argc, char **argv, bool put)
{
unsigned char hdr[IBM1301_HEADERSIZE];
unsigned char fmt[IBM1301_SLOTSIZE];
unsigned char buf[IBM1301_SLOTSIZE + 1];
Ibm1301Map map;
FILE *hostP;
size_t n;
off_t at;
int track;
int rec;
int len;
int fd;

    if( (argc != 5) || !parseInt(argv[2], 0, (IBM1301_TRACKS - 1), &track) ||
        !parseInt(argv[3], 1, IBM1301_MAXRECORDS, &rec) )
    {
        usage();
        return( 2 );
    }

    if( (fd = openImage(argv[1], (put ? O_RDWR : O_RDONLY), hdr)) < 0 )
    {
        return( 1 );
    }

    if( !readMap(fd, (track / IBM1301_HEADS), &map, fmt, &len) )
    {
        close(fd);
        return( 1 );
    }

    if( !map.formatted || (rec > map.count) )
    {
        fprintf(stderr, "%s: track %d has no record %d\n", progP, track, rec);
        close(fd);
        return( 1 );
    }

    at = ibm1301SlotOffset((track / IBM1301_HEADS), ((track % IBM1301_HEADS) + 1)) +
        map.rec[rec - 1].offset;
    len = map.rec[rec - 1].n;

    if( !(hostP = fopen(argv[4], (put ? "rb" : "wb"))) )
    {
        fprintf(stderr, "%s: %s: %s\n", progP, argv[4], strerror(errno));
        close(fd);
        return( 1 );
    }

    if( put )
    {
        memset(buf, 0, sizeof(buf));
        n = fread(buf, 1, (size_t)(len + 1), hostP);
        if( n > (size_t)len )
        {
            fprintf(stderr, "%s: %s is longer than the record's %d characters\n", progP, argv[4], len);
            fclose(hostP);
            close(fd);
            return( 1 );
        }

        if( !ibm1301WriteAt(fd, buf, (size_t)len, at) )
        {
            fprintf(stderr, "%s: writing track %d: %s\n", progP, track, strerror(errno));
            fclose(hostP);
            close(fd);
            return( 1 );
        }
    }
    else
    {
        if( (pread(fd, buf, (size_t)len, at) != len) || (fwrite(buf, 1, (size_t)len, hostP) != (size_t)len) )
        {
            fprintf(stderr, "%s: copying record %d of track %d: %s\n", progP, rec, track, strerror(errno));
            fclose(hostP);
            close(fd);
            return( 1 );
        }
    }

    if( fclose(hostP) != 0 )
    {
        fprintf(stderr, "%s: %s: %s\n", progP, argv[4], strerror(errno));
        close(fd);
        return( 1 );
    }

    close(fd);
    return( 0 );
}
