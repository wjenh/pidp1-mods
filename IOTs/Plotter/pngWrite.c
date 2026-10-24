// pngWrite.c -- a minimal PNG encoder for 1-bit grayscale images, using zlib.
//
// It writes the chunks IHDR, pHYs (optional), one IDAT and IEND.
// The IDAT holds a zlib stream deflated by zlib, zlib also creates the
// Adler-32 and the chunk CRCs.
// Every row has filter type 0.
// Single-threaded and reentrant, it keeps no state between calls.

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "pngWrite.h"

// Level 1 is the cheapest level that still finds a row repeating the one above it, which is
// most of a plot; Z_RLE would not.
// The encoding runs on the emulator's thread.
#ifndef PNG_LEVEL
#define PNG_LEVEL 1
#endif
#ifndef PNG_STRATEGY
#define PNG_STRATEGY Z_DEFAULT_STRATEGY
#endif

static uint8_t *putBe32(uint8_t *outP, uint32_t value);
static uint8_t *putChunk(uint8_t *outP, const char *typeP, const uint8_t *dataP, uint32_t len);

uint8_t *
pngEncode(uint32_t width, uint32_t height, uint32_t pixelsPerMeter, PngRowFn rowFn, void *ctxP, size_t *sizeP)
{
uint8_t ihdr[13];
uint8_t phys[9];
uint8_t *fileP;
uint8_t *shrunkP;
uint8_t *outP;
uint8_t *idatLenP;
uint8_t *rowP;
size_t rowBytes;
size_t rawLen;
size_t bound;
size_t fileLen;
size_t zlibLen;
size_t size;
uint32_t row;
uint32_t crc;
z_stream z;
int status;
static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

    if( (width == 0) || (height == 0) || !rowFn )
    {
        return(NULL);
    }

    // Each row is a filter byte and its pixels. Sizes are checked before they are multiplied out,
    // so a huge height cannot wrap.
    rowBytes = ((size_t)width + 7) / 8;
    if( (rowBytes + 1) > (PNG_MAX_BYTES / height) )
    {
        return(NULL);
    }

    rawLen = (size_t)height * (rowBytes + 1);

    memset(&z, 0, sizeof(z));
    if( deflateInit2(&z, PNG_LEVEL, Z_DEFLATED, 15, 8, PNG_STRATEGY) != Z_OK )
    {
        return(NULL);
    }

    // The buffer holds the worst case, an incompressible sheet: zlib guarantees the stream fits
    // deflateBound() when only Z_NO_FLUSH and Z_FINISH are used, so it never has to grow.
    bound = deflateBound(&z, (uLong)rawLen);
    fileLen = 8 + (12 + 13) + ((pixelsPerMeter) ? (12 + 9) : 0) + (12 + bound) + 12;
    if( fileLen > PNG_MAX_BYTES )
    {
        deflateEnd(&z);
        return(NULL);
    }

    fileP = malloc(fileLen);
    rowP = malloc(rowBytes + 1);
    if( !fileP || !rowP )
    {
        free(fileP);
        free(rowP);
        deflateEnd(&z);
        return(NULL);
    }

    outP = fileP;
    memcpy(outP, signature, sizeof(signature));
    outP += sizeof(signature);

    // IHDR: bit depth 1, color type 0 (grayscale), deflate, filter method 0, no interlace.
    putBe32(ihdr, width);
    putBe32(ihdr + 4, height);
    ihdr[8] = 1;
    ihdr[9] = 0;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    outP = putChunk(outP, "IHDR", ihdr, sizeof(ihdr));

    // pHYs: the same density on both axes, unit 1 = the meter.
    if( pixelsPerMeter )
    {
        putBe32(phys, pixelsPerMeter);
        putBe32(phys + 4, pixelsPerMeter);
        phys[8] = 1;
        outP = putChunk(outP, "pHYs", phys, sizeof(phys));
    }

    // IDAT is built in place: its length is known only once the stream is done, and its CRC,
    // over the type and the data, comes last.
    idatLenP = outP;
    outP += 4;
    memcpy(outP, "IDAT", 4);
    outP += 4;

    z.next_out = outP;
    z.avail_out = (uInt)bound;

    // The rows go in one at a time, so the whole raw image is never held. With room for the
    // worst case, each call takes all of its row; anything else is a failure.
    status = Z_OK;
    for( row = 0; (row < height) && (status == Z_OK); ++row )
    {
        rowP[0] = 0;
        rowFn(ctxP, row, rowP + 1);
        z.next_in = rowP;
        z.avail_in = (uInt)(rowBytes + 1);
        status = deflate(&z, ((row + 1) == height) ? Z_FINISH : Z_NO_FLUSH);
        if( z.avail_in != 0 )
        {
            status = Z_BUF_ERROR;
        }
    }

    zlibLen = bound - z.avail_out;
    deflateEnd(&z);
    free(rowP);

    if( status != Z_STREAM_END )
    {
        free(fileP);
        return(NULL);
    }

    putBe32(idatLenP, (uint32_t)zlibLen);
    outP += zlibLen;
    crc = (uint32_t)crc32(0L, idatLenP + 4, (uInt)(4 + zlibLen));
    outP = putBe32(outP, crc);

    outP = putChunk(outP, "IEND", NULL, 0);

    // The buffer was sized for the worst case and is held until the file is written; give
    // back the rest.
    size = (size_t)(outP - fileP);
    if( (shrunkP = realloc(fileP, size)) )
    {
        fileP = shrunkP;
    }

    *sizeP = size;
    return(fileP);
}

// Stores value big-endian at outP. Returns the byte after it.
static uint8_t *
putBe32(uint8_t *outP, uint32_t value)
{
    outP[0] = (uint8_t)((value >> 24) & 0xff);
    outP[1] = (uint8_t)((value >> 16) & 0xff);
    outP[2] = (uint8_t)((value >> 8) & 0xff);
    outP[3] = (uint8_t)(value & 0xff);
    return(outP + 4);
}

// Writes a whole chunk at outP: length, type, len bytes of data and the CRC of type and data.
// Returns the byte after it.
static uint8_t *
putChunk(uint8_t *outP, const char *typeP, const uint8_t *dataP, uint32_t len)
{
uint8_t *typeAtP;
uint32_t crc;

    outP = putBe32(outP, len);
    typeAtP = outP;
    memcpy(outP, typeP, 4);
    outP += 4;
    if( len )
    {
        memcpy(outP, dataP, len);
        outP += len;
    }

    crc = (uint32_t)crc32(0L, typeAtP, (uInt)(outP - typeAtP));
    return(putBe32(outP, crc));
}
