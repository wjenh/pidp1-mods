// pngWrite.c -- a minimal PNG encoder for 1-bit grayscale images, no library needed.
//
// It writes the chunks IHDR, pHYs (optional), one IDAT and IEND.
// The IDAT holds a zlib stream/ of stored deflate blocks (RFC 1950 and 1951), no compression,
// so no zlib is needed to build/ or run it.
// Every row has filter type 0.
// Single-threaded and reentrant, it keeps no state between calls.

#include <stdlib.h>
#include <string.h>

#include "pngWrite.h"

// A stored deflate block carries at most 65535 bytes (RFC 1951 3.2.4).
#define STORED_MAX 65535

static uint32_t crcTable[256];
static int crcTableMade;

static void makeCrcTable(void);
static uint32_t crcUpdate(uint32_t crc, const uint8_t *dataP, size_t len);
static uint8_t *putBe32(uint8_t *outP, uint32_t value);
static uint8_t *putChunk(uint8_t *outP, const char *typeP, const uint8_t *dataP, uint32_t len);

uint8_t *
pngEncode(uint32_t width, uint32_t height, uint32_t pixelsPerMeter, PngRowFn rowFn, void *ctxP, size_t *sizeP)
{
uint8_t ihdr[13];
uint8_t phys[9];
uint8_t *fileP;
uint8_t *outP;
uint8_t *idatP;
uint8_t *rowP;
size_t rowBytes;
size_t rawLen;
size_t blocks;
size_t zlibLen;
size_t fileLen;
size_t done;
size_t left;
size_t blockLen;
size_t rowAt;
uint32_t row;
uint32_t adlerA;
uint32_t adlerB;
uint32_t crc;
size_t take;
size_t i;
const uint8_t *srcP;
uint8_t filter;
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
    blocks = (rawLen + STORED_MAX - 1) / STORED_MAX;
    zlibLen = 2 + rawLen + (blocks * 5) + 4;
    fileLen = 8 + (12 + 13) + ((pixelsPerMeter) ? (12 + 9) : 0) + (12 + zlibLen) + 12;
    if( fileLen > PNG_MAX_BYTES )
    {
        return(NULL);
    }

    if( !(fileP = malloc(fileLen)) )
    {
        return(NULL);
    }

    if( !(rowP = malloc(rowBytes)) )
    {
        free(fileP);
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

    // IDAT is built in place: its length, its type, then the zlib stream, which is filled from
    // the rows block by block. Its CRC covers the type and the data, so it comes last.
    outP = putBe32(outP, (uint32_t)zlibLen);
    memcpy(outP, "IDAT", 4);
    idatP = outP;
    outP += 4;

    // zlib header: deflate with a 32K window, no dictionary, check bits making 0x7801 a
    // multiple of 31 (RFC 1950 2.2).
    *outP++ = 0x78;
    *outP++ = 0x01;

    adlerA = 1;
    adlerB = 0;
    done = 0;
    row = 0;
    rowAt = 0;
    while( done < rawLen )
    {
        left = rawLen - done;
        blockLen = (left > STORED_MAX) ? STORED_MAX : left;

        // The stored block header: BFINAL on the last, BTYPE 00, then LEN and its complement,
        // little-endian. A stored block's header ends on a byte boundary by itself.
        *outP++ = ((done + blockLen) == rawLen) ? 1 : 0;
        *outP++ = (uint8_t)(blockLen & 0xff);
        *outP++ = (uint8_t)((blockLen >> 8) & 0xff);
        *outP++ = (uint8_t)(~blockLen & 0xff);
        *outP++ = (uint8_t)((~blockLen >> 8) & 0xff);

        // The raw stream is a run of rows, each a filter byte then rowBytes; a block boundary
        // may fall anywhere in a row, so the row is fetched once and copied out in pieces.
        left = blockLen;
        while( left )
        {
            if( rowAt == 0 )
            {
                filter = 0;
                srcP = &filter;
                take = 1;
                rowFn(ctxP, row, rowP);
            }
            else
            {
                take = rowBytes - (rowAt - 1);
                srcP = rowP + (rowAt - 1);
            }

            if( take > left )
            {
                take = left;
            }

            memcpy(outP, srcP, take);

            // Adler-32 (RFC 1950 8.2) over the uncompressed bytes.
            for( i = 0; i < take; ++i )
            {
                adlerA = (adlerA + srcP[i]) % 65521;
                adlerB = (adlerB + adlerA) % 65521;
            }

            outP += take;
            left -= take;
            rowAt += take;
            if( rowAt == (rowBytes + 1) )
            {
                rowAt = 0;
                ++row;
            }
        }

        done += blockLen;
    }

    outP = putBe32(outP, ((adlerB << 16) | adlerA));
    crc = crcUpdate(0xffffffffU, idatP, (size_t)(outP - idatP)) ^ 0xffffffffU;
    outP = putBe32(outP, crc);

    outP = putChunk(outP, "IEND", NULL, 0);

    free(rowP);
    *sizeP = (size_t)(outP - fileP);
    return(fileP);
}

// Builds the CRC-32 table for the polynomial PNG uses (ISO 3309, PNG spec annex D).
static void
makeCrcTable(void)
{
uint32_t c;
int n;
int k;

    for( n = 0; n < 256; ++n )
    {
        c = (uint32_t)n;
        for( k = 0; k < 8; ++k )
        {
            c = (c & 1) ? (0xedb88320U ^ (c >> 1)) : (c >> 1);
        }

        crcTable[n] = c;
    }

    crcTableMade = 1;
}

// Returns crc updated with len bytes at dataP; start with 0xffffffff and invert the result.
static uint32_t
crcUpdate(uint32_t crc, const uint8_t *dataP, size_t len)
{
size_t i;

    if( !crcTableMade )
    {
        makeCrcTable();
    }

    for( i = 0; i < len; ++i )
    {
        crc = crcTable[(crc ^ dataP[i]) & 0xff] ^ (crc >> 8);
    }

    return(crc);
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

    crc = crcUpdate(0xffffffffU, typeAtP, (size_t)(outP - typeAtP)) ^ 0xffffffffU;
    return(putBe32(outP, crc));
}
