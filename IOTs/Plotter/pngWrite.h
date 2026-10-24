// pngWrite.h -- a minimal PNG encoder for 1-bit grayscale images, using zlib (link with -lz).
//
// The image goes into a buffer in memory, deflated by zlib.

#ifndef PNGWRITE_H
#define PNGWRITE_H

#include <stddef.h>
#include <stdint.h>

// Fills rowP with image row number row (0 is the top) in PNG's 1-bit grayscale form: the
// leftmost pixel in the high bit of the first byte, 1 for white and 0 for black, and
// (width + 7) / 8 bytes in all. ctxP is the pngEncode() caller's.
typedef void (*PngRowFn)(void *ctxP, uint32_t row, uint8_t *rowP);

// Encodes a width by height 1-bit grayscale image, its rows from rowFn, with a pHYs chunk of
// pixelsPerMeter (0 leaves pHYs out). Returns a malloc'd buffer holding the whole file and sets
// *sizeP to its length; the caller frees it. Returns NULL if width or height is 0, the file
// could be larger than PNG_MAX_BYTES (judged by the worst case, an image that does not
// compress), or memory runs out.
uint8_t *pngEncode(uint32_t width, uint32_t height, uint32_t pixelsPerMeter, PngRowFn rowFn, void *ctxP, size_t *sizeP);

// The largest file pngEncode() makes: a PNG chunk's length is limited to 2^31 - 1, and a sheet
// this large is a runaway program, not a drawing.
#define PNG_MAX_BYTES ((size_t)1 << 30)

#endif
