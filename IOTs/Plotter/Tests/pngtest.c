/*
 * pngtest.c -- encodes known bitmaps with the plotter's PNG encoder (../pngWrite.c), for
 * checkpng.py to check.
 *
 * Each case is written as NAME.png with the bitmap it came from as NAME.pbm (plain PBM, 1 for
 * ink), and listed in OUTDIR/manifest.txt as "png pbm pixelsPerMeter". The cases: a blank
 * sheet, one pixel, a diagonal, a width that is not a multiple of 8, the 560's and 563's
 * widths, a long 563 sheet of many deflate blocks, and a sheet of noise that deflate cannot
 * shrink. It also checks here that the line-art sheets are compressed, at most an eighth of
 * their raw image data, and that pngEncode() refuses a zero width, a zero height and a file
 * over PNG_MAX_BYTES.
 *
 * Usage: pngtest OUTDIR. Exit status 0 if every file was written and every refusal came.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pngWrite.h"

#define PPM 3937        // 100 dots an inch

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint8_t *inkP;      // width * height, 1 for ink
} Bitmap;

static const char *outDirP;
static FILE *manifestP;
static int failures;

// Returns a blank bitmap of the given size; exits if memory runs out.
static Bitmap
newBitmap(uint32_t width, uint32_t height)
{
Bitmap b;

    b.width = width;
    b.height = height;
    b.inkP = calloc((size_t)width * height, 1);
    if( !b.inkP )
    {
        fprintf(stderr, "pngtest: out of memory\n");
        exit(2);
    }

    return(b);
}

// The encoder's row callback: packs a bitmap row, ink as 0 (black), padding bits white.
static void
bitmapRow(void *ctxP, uint32_t row, uint8_t *rowP)
{
Bitmap *bP;
uint32_t x;

    bP = ctxP;
    memset(rowP, 0xff, ((size_t)bP->width + 7) / 8);
    for( x = 0; x < bP->width; ++x )
    {
        if( bP->inkP[((size_t)row * bP->width) + x] )
        {
            rowP[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
}

// Encodes b as NAME.png, writes NAME.pbm, and lists the pair. Counts a failure if anything
// could not be written. Returns the PNG's size, 0 if there is none.
static size_t
writeCase(const char *nameP, Bitmap *bP)
{
char path[512];
uint8_t *pngP;
size_t size;
FILE *fP;
size_t i;

    if( !(pngP = pngEncode(bP->width, bP->height, PPM, bitmapRow, bP, &size)) )
    {
        printf("FAIL %s: pngEncode() refused it\n", nameP);
        ++failures;
        return(0);
    }

    snprintf(path, sizeof(path), "%s/%s.png", outDirP, nameP);
    if( !(fP = fopen(path, "wb")) || (fwrite(pngP, 1, size, fP) != size) || fclose(fP) )
    {
        printf("FAIL %s: cannot write %s\n", nameP, path);
        ++failures;
        free(pngP);
        return(0);
    }

    free(pngP);

    snprintf(path, sizeof(path), "%s/%s.pbm", outDirP, nameP);
    if( !(fP = fopen(path, "w")) )
    {
        printf("FAIL %s: cannot write %s\n", nameP, path);
        ++failures;
        return(0);
    }

    fprintf(fP, "P1\n%u %u\n", bP->width, bP->height);
    for( i = 0; i < ((size_t)bP->width * bP->height); ++i )
    {
        fputc(bP->inkP[i] ? '1' : '0', fP);
        if( ((i + 1) % 70) == 0 )
        {
            fputc('\n', fP);
        }
    }

    fputc('\n', fP);
    fclose(fP);
    fprintf(manifestP, "%s.png %s.pbm %d\n", nameP, nameP, PPM);
    printf("wrote %s (%u x %u, %zu bytes)\n", nameP, bP->width, bP->height, size);
    return(size);
}

// Checks that a line-art case was compressed: its file at most an eighth of its raw image data,
// a filter byte and the packed pixels a row. A stored, uncompressed, file is larger than that
// data, so this fails against an encoder that does not compress.
static void
expectCompressed(const char *nameP, Bitmap *bP, size_t size)
{
size_t raw;

    raw = (size_t)bP->height * ((((size_t)bP->width + 7) / 8) + 1);
    if( (size == 0) || (size > (raw / 8)) )
    {
        printf("FAIL size %s: %zu bytes for %zu of image data, more than an eighth\n", nameP, size, raw);
        ++failures;
        return;
    }

    printf("PASS size %s: %zu bytes for %zu of image data\n", nameP, size, raw);
}

// Checks that pngEncode() refuses a size. Counts a failure if it does not.
static void
expectRefusal(const char *whatP, uint32_t width, uint32_t height)
{
Bitmap b;
uint8_t *pngP;
size_t size;

    b.width = width;
    b.height = height;
    b.inkP = NULL;      // the callback must never be reached
    pngP = pngEncode(width, height, PPM, bitmapRow, &b, &size);
    if( pngP )
    {
        printf("FAIL refusal of %s: pngEncode() made a %zu byte file\n", whatP, size);
        ++failures;
        free(pngP);
        return;
    }

    printf("PASS refusal of %s\n", whatP);
}

// Writes the cases into argv[1]. Returns 0 if all were written and every refusal came.
int
main(int argc, char **argv)
{
char path[512];
Bitmap b;
uint32_t x;
uint32_t y;
uint32_t seed;
size_t size;
size_t i;

    if( argc != 2 )
    {
        fprintf(stderr, "usage: pngtest OUTDIR\n");
        return(2);
    }

    outDirP = argv[1];
    mkdir(outDirP, 0777);
    snprintf(path, sizeof(path), "%s/manifest.txt", outDirP);
    if( !(manifestP = fopen(path, "w")) )
    {
        fprintf(stderr, "pngtest: cannot write %s\n", path);
        return(2);
    }

    b = newBitmap(8, 3);
    writeCase("blank", &b);

    b = newBitmap(16, 16);
    b.inkP[(7 * 16) + 5] = 1;
    writeCase("onepixel", &b);

    b = newBitmap(40, 40);
    for( x = 0; x < 40; ++x )
    {
        b.inkP[(x * 40) + x] = 1;
    }
    writeCase("diagonal", &b);

    // Width 13: each row's second byte holds 3 pixels and 5 padding bits.
    b = newBitmap(13, 5);
    for( y = 0; y < 5; ++y )
    {
        for( x = 0; x < 13; ++x )
        {
            b.inkP[(y * 13) + x] = (uint8_t)(((x * 7) + (y * 3)) % 5 == 0);
        }
    }
    b.inkP[12] = 1;
    b.inkP[(4 * 13) + 12] = 1;
    writeCase("width13", &b);

    // The 560's sheet width, 600 rows: 83,400 bytes of image data.
    b = newBitmap(1100, 600);
    for( y = 0; y < 600; ++y )
    {
        b.inkP[((size_t)y * 1100) + ((y * 3) % 1100)] = 1;
        b.inkP[((size_t)y * 1100) + 1099 - (y % 1100)] = 1;
        if( (y % 50) == 0 )
        {
            for( x = 0; x < 1100; ++x )
            {
                b.inkP[((size_t)y * 1100) + x] = 1;
            }
        }
    }
    size = writeCase("sheet560", &b);
    expectCompressed("sheet560", &b, size);

    // The 563's width, 2950 (369 bytes a row, 6 padding bits), 500 rows.
    b = newBitmap(2950, 500);
    for( y = 0; y < 500; ++y )
    {
        b.inkP[((size_t)y * 2950) + ((y * 5) % 2950)] = 1;
        b.inkP[((size_t)y * 2950) + 2949] = (uint8_t)(y & 1);
    }
    size = writeCase("sheet563", &b);
    expectCompressed("sheet563", &b, size);

    // A long 563 sheet of line art, 740,000 bytes of image data: many deflate blocks. A grid
    // every 100 steps, a diagonal, and a ray at a slope of 1/3.
    b = newBitmap(2950, 2000);
    for( y = 0; y < 2000; ++y )
    {
        for( x = 0; x < 2950; ++x )
        {
            if( ((y % 100) == 0) || ((x % 100) == 0) || (x == y) || (x == (3 * y) % 2950) )
            {
                b.inkP[((size_t)y * 2950) + x] = 1;
            }
        }
    }
    size = writeCase("long563", &b);
    expectCompressed("long563", &b, size);
    free(b.inkP);

    // Noise, which deflate cannot shrink: the file is at its largest for its size, so this
    // proves the encoder's buffer holds the worst case. A fixed LCG, so every run is the same.
    b = newBitmap(1100, 400);
    seed = 12345;
    for( i = 0; i < ((size_t)1100 * 400); ++i )
    {
        seed = (seed * 1103515245U) + 12345U;
        b.inkP[i] = (uint8_t)((seed >> 16) & 1);
    }
    writeCase("dense", &b);
    free(b.inkP);

    fclose(manifestP);

    expectRefusal("width 0", 0, 10);
    expectRefusal("height 0", 10, 0);
    expectRefusal("a file over PNG_MAX_BYTES", 2950, 3000000);

    return((failures == 0) ? 0 : 1);
}
