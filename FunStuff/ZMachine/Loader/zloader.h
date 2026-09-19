/*
 * zloader.h -- shared constants for the Z-machine drum writer.
 *
 * The drum geometry and the few layout facts zloader must agree with the
 * interpreter about.
 */

#ifndef ZLOADER_H
#define ZLOADER_H

#include <stdint.h>

// ---- Drum geometry (Docs/UsingType23Drum.md) ---------------------------
// 32 tracks (fields 0-37 octal), 4096 words each. The drum image file is
// one 4-byte native `int` per drum word, tracks stored back to back, the
// format the Type 23 drum plugin reads.
#define ZL_TRACK_SIZE   4096u   // words per drum track
#define ZL_MAX_TRACKS   32u     // tracks 0..31

// ---- PDP-1 memory bank geometry ---------------------------------------
// A PDP-1(D) memory bank is 4096 words (addresses 0-7777 octal), the same
// number as ZL_TRACK_SIZE but an independent hardware fact, so it has its
// own constant.
#define ZL_BANK_SIZE    4096u   // words per PDP-1 memory bank
#define ZL_MAX_BANKS    16u     // banks 0..17 octal (PDP-1D 4-bit field)

// ---- The one fact this tool MUST agree with the interpreter about ----
// Duplicated here rather than shared, because there is no header format
// both C and am1 can read:
//
//   ZL_MAX_DYN_BANKS      == the interpreter's dynamic-memory bank budget
//                            (banks 1..12; 13/14/15 hold zStack, the cache
//                            slots, and the leaf fragments) -- zmemcore.am1's
//                            Z_MAX_DYN_BANKS, whose comment names this one
//
// ---- The story's extent ------------------------------------------------
// zloader.c installs the story up to the header's DECLARED length, and
// zboot.ac derives storyTracks from the same length, since boot sees only
// a drum. Padding a file carries past its declared end is dropped, so it
// never reaches the drum. A length field of 0 installs the whole file
// (zloader.c's readHeader()).
#define ZL_MAX_DYN_BANKS        12u

// ---- Z-machine header field byte offsets (Standard 1.1, the layout V1-V5
// share for the fields this loader needs) ------------------------------
#define ZH_VERSION          0x00   // 1 byte
#define ZH_FLAGS1           0x01   // 1 byte
#define ZH_RELEASE          0x02   // 2 bytes
#define ZH_HIGHMEM_BASE     0x04   // 2 bytes
#define ZH_INITIAL_PC       0x06   // 2 bytes
#define ZH_DICT_ADDR        0x08   // 2 bytes
#define ZH_OBJTABLE_ADDR    0x0A   // 2 bytes
#define ZH_GLOBALS_ADDR     0x0C   // 2 bytes
#define ZH_STATIC_BASE      0x0E   // 2 bytes -- also = dynamic memory size
#define ZH_FLAGS2           0x10   // 2 bytes
#define ZH_SERIAL           0x12   // 6 bytes, ASCII
#define ZH_ABBREV_ADDR      0x18   // 2 bytes
#define ZH_FILE_LENGTH      0x1A   // 2 bytes, must be multiplied by a
                                   // version-dependent factor: 2 for V1-V3,
                                   // 4 for V4-V5, 8 for V6-V8 (S11.1.6).
                                   // zloader.c applies it; zboot.ac applies
                                   // the same rule in units of WORDS rather
                                   // than bytes, because a V4/V5 byte count
                                   // does not fit in an 18-bit PDP-1 word
#define ZH_CHECKSUM         0x1C   // 2 bytes
#define ZH_HEADER_BYTES     0x40   // the header's own documented size

// ---- Word packing convention ---------------------------------------
// Two consecutive story-file bytes pack into one PDP-1 word as
// (byte0 << 8) | byte1, the same big-endian order the Z-machine uses for
// its own 16-bit fields. For any EVEN Z-machine byte address, "read the
// PDP-1 word at (address/2)" and "read the Z-machine word at address" are
// then the same operation. Odd-address byte fetches need a shift/mask
// (high byte = word >> 8, low byte = word & 0377). With the whole file on
// the drum from byte 0, drum word index IS (byteAddress >> 1) for every
// address in the story.

// Pack two story bytes into one PDP-1 word. Returns (hi << 8) | lo.
static inline uint32_t
zl_pack_word(uint8_t hi, uint8_t lo)
{
    return ((uint32_t)hi << 8) | (uint32_t)lo;
}

#endif // ZLOADER_H
