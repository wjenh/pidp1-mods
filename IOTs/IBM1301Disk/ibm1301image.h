// The IBM 1301 disk image, one file per module, shared by the IOT_10 plugin and the
// dsk1301 tool.
// It defines the file's layout, reads a cylinder's format record into the map of its areas,
// builds format records, and creates a fresh image.
// Every function is static, so each program that includes it gets a private copy.
//
// The file is little-endian, with one host byte per character (a six-bit or eight-bit value).
// It starts with a header of IBM1301_HEADERSIZE bytes, the magic "IBM1301\0" then 32-bit words
// for the format version, the module number and the slot size.
// Then each of the 250 cylinders has 41 slots of IBM1301_SLOTSIZE bytes, the format slot and
// then tracks 0-39.
// A format slot is a 2-byte length and the format record's characters (BCD 1-4) as written;
// length 0 is a cylinder never formatted.
// A track slot holds the characters of HA2, then each record address and record in track order,
// as the cylinder's format lays them out, with nothing between them.
// HA1, the track number, is fixed on the drive and not stored.
//
// The format record is laid out in IBM's 7631 OEM manual A22-6725.
// It starts with the track identification area, 444 333333333 4 3333333333 4, always
// eight-bit, and HA2's area, n + 4 ones.
// Each record follows as an X gap of 12 zeros, a record address area of n + 4 ones (n >= 6), a
// Y gap (a zero, ten ones, a zero) and a record area of n + 4 ones (n >= 2).
// A zero, gap 3, ends it.
// Ones and zeros are 1 and 2 in a six-bit cylinder, 3 and 4 in an eight-bit one.
//
// 23-Nov-2026 Claude initial verson.
// 23-Nov-2026 wje cleanup.

#ifndef IBM1301IMAGE_H
#define IBM1301IMAGE_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IBM1301_MAGIC "IBM1301"         // 8 bytes with its NUL
#define IBM1301_VERSION 1
#define IBM1301_HEADERSIZE 4096
#define IBM1301_SLOTSIZE 4096
#define IBM1301_CYLINDERS 250
#define IBM1301_HEADS 40
#define IBM1301_TRACKS (IBM1301_CYLINDERS * IBM1301_HEADS)
#define IBM1301_SLOTS (IBM1301_HEADS + 1)
#define IBM1301_FILESIZE ((off_t)IBM1301_HEADERSIZE + \
    ((off_t)IBM1301_CYLINDERS * IBM1301_SLOTS * IBM1301_SLOTSIZE))
#define IBM1301_FORMATMAX (IBM1301_SLOTSIZE - 2)
#define IBM1301_MAXMODULES 10

// The track's capacity, HA2 plus each record's length + record address + 32.
#define IBM1301_CAPACITY6 2840
#define IBM1301_CAPACITY8 2205

// The track identification area's 24 positions, and the fewest a record can take.
#define IBM1301_TIPOS 24
#define IBM1301_MAXRECORDS 72

// A fresh image as the IOTs default and dsk1301 makes, is six-bit, with HA2 of two
// characters and five records of 528 characters.
// This was Stanford's 176-word record with six-character record addresses.
#define IBM1301_DEFHA2 2
#define IBM1301_DEFRA 6
#define IBM1301_DEFREC 528
#define IBM1301_DEFCOUNT 5

// What ibm1301ParseFormat() finds wrong with a format record.
#define IBM1301_FMT_OK 0
#define IBM1301_FMT_BADCHAR 1          // a character other than 1-4
#define IBM1301_FMT_LAYOUT 2           // the areas are not laid out as IBM requires
#define IBM1301_FMT_MIXED 3            // six-bit and eight-bit characters in one record
#define IBM1301_FMT_TOOLONG 4          // more than the track holds

// One area of a track, with where it starts in the format record, counted in positions from
// the first position of the track identification area, how many characters it holds, and
// where they are in the track slot.
// The characters are in the area's positions 1 to n; positions 0, n + 1, n + 2 and n + 3 are
// the area's overhead.
typedef struct
{
    int pos;
    int n;
    int offset;
} Ibm1301Area;

// A cylinder's layout, read from its format record.
typedef struct
{
    bool formatted;
    bool eightBit;
    Ibm1301Area ha2;
    int count;                          // records on each track
    Ibm1301Area ra[IBM1301_MAXRECORDS];
    Ibm1301Area rec[IBM1301_MAXRECORDS];
    int positions;                      // positions the format record takes, gap 3 included
    int trackChars;                     // characters in a track slot
} Ibm1301Map;

// Returns the byte offset of a cylinder's slot in the file, slot 0 the format and 1-40 the
// tracks.
static off_t
ibm1301SlotOffset(int cyl, int slot)
{
    return( (off_t)IBM1301_HEADERSIZE + ((((off_t)cyl * IBM1301_SLOTS) + slot) * IBM1301_SLOTSIZE) );
}

// Stores a 32-bit value little-endian at p.
static void
ibm1301Put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

// Returns the 32-bit little-endian value at p.
static uint32_t
ibm1301Get32(const unsigned char *p)
{
    return( (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) );
}

// Returns a format slot's length, the 2-byte value at its start.
static int
ibm1301FormatLength(const unsigned char *slotP)
{
    return( (int)slotP[0] | ((int)slotP[1] << 8) );
}

// Sets a format slot's length.
static void
ibm1301SetFormatLength(unsigned char *slotP, int len)
{
    slotP[0] = (unsigned char)(len & 0xff);
    slotP[1] = (unsigned char)((len >> 8) & 0xff);
}

// Checks a header for this module.
// Returns NULL if it is good, else a phrase saying what is wrong.
static const char *
ibm1301CheckHeader(const unsigned char *hdrP, int module)
{
    if( memcmp(hdrP, IBM1301_MAGIC, 8) != 0 )
    {
        return( "not an IBM 1301 image" );
    }

    if( ibm1301Get32(hdrP + 8) != IBM1301_VERSION )
    {
        return( "an image of another format version" );
    }

    if( (module >= 0) && (ibm1301Get32(hdrP + 12) != (uint32_t)module) )
    {
        return( "the image of another module" );
    }

    if( ibm1301Get32(hdrP + 16) != IBM1301_SLOTSIZE )
    {
        return( "an image with another slot size" );
    }

    return( NULL );
}

// Counts the characters equal to c from fmtP[at], stopping at len.
static int
ibm1301Run(const unsigned char *fmtP, int len, int at, int c)
{
int n;

    for( n = 0; ((at + n) < len) && (fmtP[at + n] == c); ++n )
    {
        ;
    }

    return( n );
}

// Reads a format record into a map.
// Characters of value 0 after gap 3 are ignored, since they fill out the program's last word.
// Returns IBM1301_FMT_OK, or the IBM1301_FMT_ value saying what is wrong.
static int
ibm1301ParseFormat(const unsigned char *fmtP, int len, Ibm1301Map *mapP)
{
static const char ti[] = "444333333333433333333334";
int at;
int one;
int zero;
int run;
int r;
int used;
int i;

    memset(mapP, 0, sizeof(*mapP));

    // Every character before the padding is 1-4, and none of the other mode's after the
    // track identification area.
    while( (len > 0) && (fmtP[len - 1] == 0) )
    {
        --len;
    }

    for( i = 0; i < len; ++i )
    {
        if( (fmtP[i] < 1) || (fmtP[i] > 4) )
        {
            return( IBM1301_FMT_BADCHAR );
        }
    }

    if( len < (IBM1301_TIPOS + 1) )
    {
        return( IBM1301_FMT_LAYOUT );
    }

    for( i = 0; i < IBM1301_TIPOS; ++i )
    {
        if( fmtP[i] != (unsigned char)(ti[i] - '0') )
        {
            return( IBM1301_FMT_LAYOUT );
        }
    }

    mapP->eightBit = (fmtP[IBM1301_TIPOS] >= 3);
    one = (mapP->eightBit) ? 3 : 1;
    zero = (mapP->eightBit) ? 4 : 2;
    for( i = IBM1301_TIPOS; i < len; ++i )
    {
        if( (fmtP[i] != one) && (fmtP[i] != zero) )
        {
            return( IBM1301_FMT_MIXED );
        }
    }

    // HA2's area.
    at = IBM1301_TIPOS;
    run = ibm1301Run(fmtP, len, at, one);
    if( run < 6 )
    {
        return( IBM1301_FMT_LAYOUT );
    }

    mapP->ha2.pos = at;
    mapP->ha2.n = run - 4;
    mapP->ha2.offset = 0;
    used = mapP->ha2.n;
    at += run;

    // Each record is an X gap, its address area, a Y gap and its record area.
    for( r = 0; ; ++r )
    {
        run = ibm1301Run(fmtP, len, at, zero);
        if( (run == 1) && ((at + 1) == len) && (r > 0) )
        {
            break;                      // gap 3
        }

        if( (run != 12) || (r >= IBM1301_MAXRECORDS) )
        {
            return( IBM1301_FMT_LAYOUT );
        }

        at += run;
        run = ibm1301Run(fmtP, len, at, one);
        if( run < 10 )
        {
            return( IBM1301_FMT_LAYOUT );
        }

        mapP->ra[r].pos = at;
        mapP->ra[r].n = run - 4;
        at += run;

        if( (ibm1301Run(fmtP, len, at, zero) != 1) || (ibm1301Run(fmtP, len, (at + 1), one) != 10) ||
            (ibm1301Run(fmtP, len, (at + 11), zero) != 1) )
        {
            return( IBM1301_FMT_LAYOUT );
        }

        at += 12;
        run = ibm1301Run(fmtP, len, at, one);
        if( run < 6 )
        {
            return( IBM1301_FMT_LAYOUT );
        }

        mapP->rec[r].pos = at;
        mapP->rec[r].n = run - 4;
        at += run;

        mapP->ra[r].offset = used;
        used += mapP->ra[r].n;
        mapP->rec[r].offset = used;
        used += mapP->rec[r].n;
    }

    mapP->count = r;
    mapP->positions = len;
    mapP->trackChars = used;

    // IBM's capacity rule leaves room for the 11 filler positions the 7631 writes itself.
    used = mapP->ha2.n;
    for( r = 0; r < mapP->count; ++r )
    {
        used += mapP->rec[r].n + mapP->ra[r].n + 32;
    }

    if( used > ((mapP->eightBit) ? IBM1301_CAPACITY8 : IBM1301_CAPACITY6) )
    {
        return( IBM1301_FMT_TOOLONG );
    }

    mapP->formatted = true;
    return( IBM1301_FMT_OK );
}

// Builds the format record of count equal records into fmtP, which holds IBM1301_FORMATMAX.
// Returns its length, or 0 if the layout is impossible, with too short an area or too long a
// track.
static int
ibm1301BuildFormat(unsigned char *fmtP, bool eightBit, int ha2, int ra, int rec, int count)
{
static const char ti[] = "444333333333433333333334";
Ibm1301Map map;
int one;
int zero;
int at;
int r;

    if( (ha2 < 2) || (ra < 6) || (rec < 2) || (count < 1) || (count > IBM1301_MAXRECORDS) )
    {
        return( 0 );
    }

    if( (IBM1301_TIPOS + (ha2 + 4) + (count * (ra + rec + 32)) + 1) > IBM1301_FORMATMAX )
    {
        return( 0 );
    }

    one = (eightBit) ? 3 : 1;
    zero = (eightBit) ? 4 : 2;
    for( at = 0; at < IBM1301_TIPOS; ++at )
    {
        fmtP[at] = (unsigned char)(ti[at] - '0');
    }

    memset(fmtP + at, one, (size_t)(ha2 + 4));
    at += ha2 + 4;
    for( r = 0; r < count; ++r )
    {
        memset(fmtP + at, zero, 12);
        at += 12;
        memset(fmtP + at, one, (size_t)(ra + 4));
        at += ra + 4;
        fmtP[at] = (unsigned char)zero;
        memset(fmtP + at + 1, one, 10);
        fmtP[at + 11] = (unsigned char)zero;
        at += 12;
        memset(fmtP + at, one, (size_t)(rec + 4));
        at += rec + 4;
    }

    fmtP[at++] = (unsigned char)zero;
    if( ibm1301ParseFormat(fmtP, at, &map) != IBM1301_FMT_OK )
    {
        return( 0 );
    }

    return( at );
}

// Writes a track's record addresses as a fresh image has them, the four track digits and the
// record number as two digits, 01 up, then zeros to the area's length.
// BCD digits are 0-9.
static void
ibm1301DefaultAddresses(unsigned char *trackP, const Ibm1301Map *mapP, int track)
{
unsigned char *raP;
int r;

    for( r = 0; r < mapP->count; ++r )
    {
        raP = trackP + mapP->ra[r].offset;
        memset(raP, 0, (size_t)mapP->ra[r].n);
        raP[0] = (unsigned char)((track / 1000) % 10);
        raP[1] = (unsigned char)((track / 100) % 10);
        raP[2] = (unsigned char)((track / 10) % 10);
        raP[3] = (unsigned char)(track % 10);
        raP[4] = (unsigned char)(((r + 1) / 10) % 10);
        raP[5] = (unsigned char)((r + 1) % 10);
    }
}

// Writes len bytes at offset, all of them or fail.
// Returns true on success, else false with errno set.
static bool
ibm1301WriteAt(int fd, const void *bufP, size_t len, off_t offset)
{
const char *p;
ssize_t n;

    p = (const char *)bufP;
    while( len > 0 )
    {
        n = pwrite(fd, p, len, offset);
        if( (n < 0) && (errno == EINTR) )
        {
            continue;
        }

        if( n <= 0 )
        {
            if( n == 0 )
            {
                errno = EIO;
            }
            return( false );
        }

        p += n;
        len -= (size_t)n;
        offset += n;
    }

    return( true );
}

// Creates a fresh image for a module at path, formatted as a fresh image is (the defaults
// above), or blank, every cylinder never formatted.
// The file is sparse, its unwritten bytes 0.
// An existing file is replaced only if replace is true.
// Returns true on success, else false with errno set.
static bool
ibm1301CreateImage(const char *pathP, int module, bool formatted, bool replace)
{
unsigned char hdr[IBM1301_HEADERSIZE];
static unsigned char slot[IBM1301_SLOTSIZE];
Ibm1301Map map;
int fd;
int len;
int cyl;
int head;
int err;
bool ok;

    fd = open(pathP, (O_RDWR | O_CREAT | (replace ? O_TRUNC : O_EXCL)), 0666);
    if( fd < 0 )
    {
        return( false );
    }

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, IBM1301_MAGIC, 8);
    ibm1301Put32(hdr + 8, IBM1301_VERSION);
    ibm1301Put32(hdr + 12, (uint32_t)module);
    ibm1301Put32(hdr + 16, IBM1301_SLOTSIZE);

    ok = (ftruncate(fd, IBM1301_FILESIZE) == 0) && ibm1301WriteAt(fd, hdr, sizeof(hdr), 0);

    if( ok && formatted )
    {
        memset(slot, 0, sizeof(slot));
        len = ibm1301BuildFormat(slot + 2, false, IBM1301_DEFHA2, IBM1301_DEFRA, IBM1301_DEFREC,
            IBM1301_DEFCOUNT);
        ibm1301SetFormatLength(slot, len);
        ibm1301ParseFormat(slot + 2, len, &map);
        for( cyl = 0; ok && (cyl < IBM1301_CYLINDERS); ++cyl )
        {
            ok = ibm1301WriteAt(fd, slot, (size_t)(len + 2), ibm1301SlotOffset(cyl, 0));
        }

        // Only the record addresses of each track are not 0.
        memset(slot, 0, sizeof(slot));
        for( cyl = 0; ok && (cyl < IBM1301_CYLINDERS); ++cyl )
        {
            for( head = 0; ok && (head < IBM1301_HEADS); ++head )
            {
                ibm1301DefaultAddresses(slot, &map, ((cyl * IBM1301_HEADS) + head));
                ok = ibm1301WriteAt(fd, slot, (size_t)map.trackChars, ibm1301SlotOffset(cyl, (head + 1)));
            }
        }
    }

    err = errno;
    if( (close(fd) != 0) && ok )
    {
        ok = false;
        err = errno;
    }

    errno = err;
    return( ok );
}

#endif
