/*
 * mkmicrotape.c -- create, check and convert Type 550 Microtape image files.
 *
 * It can be used to create, verify or convert to/from simh representation.
 *
 *   mkmicrotape [-f] blank  <image>             a blank tape: an empty file
 *   mkmicrotape [-f] import <simh-file> <image> from simh; checksums computed, as if each
 *                                               block had been written forward; trailing
 *                                               blank blocks are left off the file
 *   mkmicrotape [-f] export <image> <simh-file> to simh; data words only, all 578 blocks
 *   mkmicrotape      check  <image>             list the blocks whose checksum fails
 *
 * -f allows an existing output file to be overwritten; without it the tool refuses.
 *
 * 11-Sep-2026 wjeClaude - initial version
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "transport555.h"

#define SIMH_BLOCKS     578                 // simh's D18_TSIZE
#define SIMH_WORDS      (SIMH_BLOCKS * MT_DATA_WORDS)
#define SIMH_BYTES      (SIMH_WORDS * 4)

#define EXIT_OK         0
#define EXIT_BADBLOCKS  1
#define EXIT_ERROR      2

static bool force;                          // -f: overwrite an existing output file
static uint32_t imageWords[MT_IMAGE_WORDS];
static uint32_t simhWords[SIMH_WORDS];

// Prints the usage summary to stderr. No return value.
static void
usage(void)
{
    fprintf(stderr,
        "usage: mkmicrotape [-f] blank  <image>\n"
        "       mkmicrotape [-f] import <simh-18b-file> <image>\n"
        "       mkmicrotape [-f] export <image> <simh-18b-file>\n"
        "       mkmicrotape      check  <image>\n"
        "A Type 550 image holds up to %d blocks of %d words (leading checksum, 256 data,\n"
        "trailing checksum), 18-bit words in 32-bit host-order containers, %d bytes a block.\n"
        "Blocks past the end of the file are blank, so an empty file is a blank tape.\n"
        "-f overwrites an existing output file.\n",
        MT_BLOCKS, MT_STORED_WORDS, MT_BLOCK_BYTES);
}

// Returns a pointer to the 258 stored words of block in imageWords.
static uint32_t *
blockP(int block)
{
    return( &imageWords[block * MT_STORED_WORDS] );
}

// Sets the two checksums of block from its data, as the control and a program writing it
// forward would: -0 in the leading slot, and in the trailing slot the complement of the ring
// sum of the -0 and the data, so that the whole block totals -0. No return value.
static void
checksumBlock(int block)
{
uint32_t *bP;
uint32_t sum;
int k;

    bP = blockP(block);
    bP[0] = MT_MINUS_ZERO;
    sum = MT_MINUS_ZERO;
    for( k = 1; k <= MT_DATA_WORDS; ++k )
    {
        sum = mt555RingAdd(sum, bP[k]);
    }

    bP[MT_STORED_WORDS - 1] = ((~sum) & MT_WORDMASK);
}

// Returns true if block of imageWords is exactly a blank block.
static bool
isBlank(int block)
{
uint32_t blank[MT_STORED_WORDS];

    mt555BlankBlock(blank);
    return( memcmp(blockP(block), blank, sizeof(blank)) == 0 );
}

// Opens pathP for writing, refusing an existing file unless -f was given.
// Returns the stream, or NULL after printing why.
static FILE *
openOutput(const char *pathP)
{
FILE *fP;

    if( !force && (access(pathP, F_OK) == 0) )
    {
        fprintf(stderr, "mkmicrotape: %s exists (use -f to overwrite)\n", pathP);
        return(NULL);
    }

    if( !(fP = fopen(pathP, "wb")) )
    {
        perror(pathP);
    }

    return(fP);
}

// Writes the first blocks blocks of imageWords to pathP as a native image (0 makes an empty
// file, a blank tape). Returns true on success.
static bool
writeImage(const char *pathP, int blocks)
{
FILE *fP;
size_t words;
bool ok;

    if( !(fP = openOutput(pathP)) )
    {
        return(false);
    }

    words = ((size_t)blocks * MT_STORED_WORDS);
    ok = (fwrite(imageWords, sizeof(uint32_t), words, fP) == words);
    ok = ((fclose(fP) == 0) && ok);
    if( !ok )
    {
        fprintf(stderr, "mkmicrotape: write to %s failed\n", pathP);
    }

    return(ok);
}

// Reads the native image pathP into imageWords: a whole number of blocks, at most a full
// tape, with every block past the end of the file made blank. Bits above 17 are dropped.
// Puts the number of blocks the file holds in *storedP.
// Returns true on success.
static bool
readImage(const char *pathP, int *storedP)
{
FILE *fP;
long size;
size_t want;
size_t got;
int block;
int i;

    if( !(fP = fopen(pathP, "rb")) )
    {
        perror(pathP);
        return(false);
    }

    // Size it first: fread() would silently drop a partial word at the end.
    size = -1;
    if( fseek(fP, 0L, SEEK_END) == 0 )
    {
        size = ftell(fP);
        rewind(fP);
    }

    if( (size < 0) || ((size % MT_BLOCK_BYTES) != 0) || (size > (long)MT_IMAGE_BYTES) )
    {
        fclose(fP);
        fprintf(stderr, "mkmicrotape: %s is not a Type 550 image (must be a whole number of %d-byte blocks,"
            " at most %d bytes)\n", pathP, MT_BLOCK_BYTES, MT_IMAGE_BYTES);
        return(false);
    }

    want = ((size_t)size / sizeof(uint32_t));
    got = fread(imageWords, sizeof(uint32_t), want, fP);
    fclose(fP);

    if( got != want )
    {
        fprintf(stderr, "mkmicrotape: read of %s failed\n", pathP);
        return(false);
    }

    for( i = 0; i < (int)got; ++i )
    {
        imageWords[i] = (imageWords[i] & MT_WORDMASK);
    }

    *storedP = (int)(got / MT_STORED_WORDS);
    for( block = *storedP; block < MT_BLOCKS; ++block )
    {
        mt555BlankBlock(blockP(block));
    }

    return(true);
}

// blank: a blank tape, which with deferred formatting is an empty file; every block reads
// with data 0 and valid checksums (leading -0, trailing 0). Returns the exit status.
static int
doBlank(const char *imageP)
{
    return( writeImage(imageP, 0) ? EXIT_OK : EXIT_ERROR );
}

// import: reads a simh PDP-1 18b file (578 x 256 little-endian 32-bit words; simh accepts a
// short file and treats the rest as zero, and so does this), keeps blocks 0-575 and computes
// their checksums. Blocks 576-577 do not exist on a Type 550 tape; if they hold anything, a
// warning says so. Trailing blocks that come out blank (all data zero) are left off the file.
// Returns the exit status.
static int
doImport(const char *simhP, const char *imageP)
{
FILE *fP;
unsigned char bytes[4];
long words;
int nonzero;
int block;
int blocks;
int k;
int i;

    if( !(fP = fopen(simhP, "rb")) )
    {
        perror(simhP);
        return(EXIT_ERROR);
    }

    memset(simhWords, 0, sizeof(simhWords));
    words = 0;
    while( (words < SIMH_WORDS) && (fread(bytes, 1, 4, fP) == 4) )
    {
        simhWords[words] = ((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16)
            | ((uint32_t)bytes[3] << 24));
        ++words;
    }

    i = fgetc(fP);
    fclose(fP);
    if( i != EOF )
    {
        fprintf(stderr, "mkmicrotape: %s is longer than a simh 18b tape (%d bytes)\n", simhP, SIMH_BYTES);
        return(EXIT_ERROR);
    }

    if( words < SIMH_WORDS )
    {
        fprintf(stderr, "mkmicrotape: note: %s has %ld of %d words; the rest are zero\n", simhP, words, SIMH_WORDS);
    }

    nonzero = 0;
    for( i = (MT_BLOCKS * MT_DATA_WORDS); i < SIMH_WORDS; ++i )
    {
        nonzero = (nonzero + ((simhWords[i] & MT_WORDMASK) != 0));
    }

    if( nonzero )
    {
        fprintf(stderr, "mkmicrotape: warning: %d nonzero words in blocks 576-577 (1100-1101 octal) dropped;"
            " a Type 550 tape has 576 blocks\n", nonzero);
    }

    for( block = 0; block < MT_BLOCKS; ++block )
    {
        for( k = 0; k < MT_DATA_WORDS; ++k )
        {
            blockP(block)[1 + k] = (simhWords[(block * MT_DATA_WORDS) + k] & MT_WORDMASK);
        }

        checksumBlock(block);
    }

    for( blocks = MT_BLOCKS; (blocks > 0) && isBlank(blocks - 1); --blocks )
    {
        // leave the trailing blank blocks off the file: they read blank anyway
    }

    return( writeImage(imageP, blocks) ? EXIT_OK : EXIT_ERROR );
}

// export: writes the data words of every block (the file's, and blank ones past its end) as a
// simh PDP-1 18b file, little-endian, with blocks 576-577 zero. The checksums are not carried
// (simh has no place for them); a note says how many blocks did not check. Returns the exit
// status.
static int
doExport(const char *imageP, const char *simhP)
{
FILE *fP;
unsigned char bytes[4];
uint32_t word;
int stored;
int bad;
int block;
int i;
bool ok;

    if( !readImage(imageP, &stored) )
    {
        return(EXIT_ERROR);
    }

    bad = 0;
    memset(simhWords, 0, sizeof(simhWords));
    for( block = 0; block < MT_BLOCKS; ++block )
    {
        memcpy(&simhWords[block * MT_DATA_WORDS], &blockP(block)[1], (MT_DATA_WORDS * sizeof(uint32_t)));
        bad = (bad + (mt555BlockSum(blockP(block)) != MT_MINUS_ZERO));
    }

    if( bad )
    {
        fprintf(stderr, "mkmicrotape: note: %d blocks of %s do not check; their data is exported as stored\n",
            bad, imageP);
    }

    if( !(fP = openOutput(simhP)) )
    {
        return(EXIT_ERROR);
    }

    ok = true;
    for( i = 0; (i < SIMH_WORDS) && ok; ++i )
    {
        word = simhWords[i];
        bytes[0] = (unsigned char)(word & 0377);
        bytes[1] = (unsigned char)((word >> 8) & 0377);
        bytes[2] = (unsigned char)((word >> 16) & 0377);
        bytes[3] = (unsigned char)((word >> 24) & 0377);
        ok = (fwrite(bytes, 1, 4, fP) == 4);
    }

    ok = ((fclose(fP) == 0) && ok);
    if( !ok )
    {
        fprintf(stderr, "mkmicrotape: write to %s failed\n", simhP);
        return(EXIT_ERROR);
    }

    return(EXIT_OK);
}

// check: lists every block whose 258 words do not total -0 (the blank blocks past the end of
// the file always check). Returns EXIT_OK if all check, EXIT_BADBLOCKS if any fail, EXIT_ERROR
// if the image cannot be read.
static int
doCheck(const char *imageP)
{
int stored;
int bad;
int block;
uint32_t sum;

    if( !readImage(imageP, &stored) )
    {
        return(EXIT_ERROR);
    }

    bad = 0;
    for( block = 0; block < MT_BLOCKS; ++block )
    {
        sum = mt555BlockSum(blockP(block));
        if( sum != MT_MINUS_ZERO )
        {
            printf("block %04o: checksum total %06o, not -0\n", block, sum);
            ++bad;
        }
    }

    printf("%s: %d blocks, %d in the file, %d fail their checksum\n", imageP, MT_BLOCKS, stored, bad);
    return( bad ? EXIT_BADBLOCKS : EXIT_OK );
}

// Parses the command line and runs the command. Returns the exit status described in the
// file header.
int
main(int argc, char **argv)
{
int argi;
const char *cmdP;

    argi = 1;
    force = false;
    if( (argi < argc) && (strcmp(argv[argi], "-f") == 0) )
    {
        force = true;
        ++argi;
    }

    if( argi >= argc )
    {
        usage();
        return(EXIT_ERROR);
    }

    cmdP = argv[argi++];
    if( (strcmp(cmdP, "blank") == 0) && ((argc - argi) == 1) )
    {
        return( doBlank(argv[argi]) );
    }

    if( (strcmp(cmdP, "import") == 0) && ((argc - argi) == 2) )
    {
        return( doImport(argv[argi], argv[argi + 1]) );
    }

    if( (strcmp(cmdP, "export") == 0) && ((argc - argi) == 2) )
    {
        return( doExport(argv[argi], argv[argi + 1]) );
    }

    if( (strcmp(cmdP, "check") == 0) && ((argc - argi) == 1) )
    {
        return( doCheck(argv[argi]) );
    }

    usage();
    return(EXIT_ERROR);
}
