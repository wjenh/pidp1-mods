/*
 * zloader.c -- copies a Z-machine story file onto the PDP-1 drum.
 *
 * Purpose, architectural scope, and dependencies:
 *   Reads a Z-machine V3, V4 or V5 story file (a raw .z3/.z4/.z5) and
 *   writes the story, verbatim, onto the live drum image, zeroing the
 *   rest of the drum. The story is the file up to its header's DECLARED
 *   length; any padding after that is not part of it and is dropped.
 *   Constants shared with the interpreter are in zloader.h.
 *
 *   It computes no layout for the interpreter. The interpreter parses the
 *   story header off the drum at boot and works out its dynamic-memory
 *   size, bank count and page addressing from it, so one assembled
 *   interpreter runs any story with no reassembly.
 *
 *   One interpreter image, zmachine.rim, runs V3, V4 and V5. Boot reads
 *   the version byte and refuses any other version itself (zboot.ac's
 *   section 2), since a file can reach the drum by other means. This
 *   program refuses the same versions, V1, V2 and V6-V8, so that the
 *   refusal lands where a person can read it, and prints the version it
 *   accepted in its report.
 *
 * Drum layout written here:
 *   track 0 upward         the story, to its declared length, packed two
 *                          bytes per word, big-endian, byte 0 at word 0
 *                          -- so drum word index == (Z-machine byte
 *                          address >> 1) for every address in the story
 *   everything after it    zeroed, padding included
 *
 *   There is no save area: saves go to the Type 550 microtape the player
 *   mounts on drive 2, so the story may have the whole drum.
 *
 *   The whole drum belongs to the installed story. Every one of the 32
 *   tracks is written on every run, and nothing is preserved -- not
 *   another application's data, and not the tail of a larger story this
 *   one replaces.
 *
 * Execution model: single-shot batch tool, run offline before the emulator
 * is started. Not part of the emulator or any IOT plugin.
 *
 * 19-Sep-2026 wje/Claude initial release
 * 21-Sep-2026 wje shorten the load report, too verbose
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "zloader.h"

// The live drum the Type 23 plugin pair reads.
// The -i option overrides it.
#define DEFAULT_DRUM "/opt/pidp1-mods/pdp23drum"

static uint8_t  *g_storyP;              // the whole story file in memory
static long      g_storyLen;            // its actual size in bytes, padding included

static uint8_t   g_version;
static uint16_t  g_release;
static uint16_t  g_highMemBase;
static uint16_t  g_initialPC;
static uint16_t  g_dictAddr;
static uint16_t  g_objTableAddr;
static uint16_t  g_globalsAddr;
static uint16_t  g_staticBase;
static uint16_t  g_abbrevAddr;
static uint16_t  g_headerChecksum;
static uint32_t  g_fileLengthField;     // header's claim, already scaled by
                                        //   S11.1.6's version-dependent unit
static uint32_t  g_lengthScale;         // that unit: 2 for V3, 4 for V4/V5
static uint32_t  g_installLen;          // the bytes put on the drum: the
                                        //   declared length, or the whole
                                        //   file when the field is 0
static uint16_t  g_storySum;            // the story's checksum, bytes
                                        //   0x40 .. g_installLen-1, mod 2^16
static char      g_serial[7];

static uint32_t  g_storyWords;          // ceil(g_installLen / 2)
static uint32_t  g_storyTracks;         // ceil(g_storyWords / ZL_TRACK_SIZE)
static uint32_t  g_dynWords;            // ceil(g_staticBase / 2)
static uint32_t  g_dynBanks;            // ceil(g_dynWords / ZL_BANK_SIZE)

static void      usage(const char *progNameP);
static uint16_t  be16(const uint8_t *pP);
static void      loadStoryFile(const char *pathP);
static void      readHeader(void);
static uint16_t  sumStory(void);
static void      computeLayout(void);
static void      writeDrum(const char *pathP);
static void      report(const char *storyPathP, const char *drumPathP);

// Print the usage text to stderr.
static void
usage(const char *progNameP)
{
    fprintf(stderr,
        "Usage: %s [-i path-to-live-drum-image] story.z3|.z4|.z5\n"
        "  Copies a V3, V4 or V5 story file onto the drum. The whole drum\n"
        "  is rewritten; anything already on it is lost. (Saved games are\n"
        "  not on the drum: they go to the tape on drive 2.)\n"
        "  One interpreter image, zmachine.rim, runs V3, V4 and V5 stories.\n"
        "  If no -i path is given, the default is '%s'.\n",
        progNameP, DEFAULT_DRUM);
}

// Read a big-endian 16-bit field, the Z-machine' word order.
// Returns the field's value.
static uint16_t
be16(const uint8_t *pP)
{
    return (uint16_t)(((uint16_t)pP[0] << 8) | (uint16_t)pP[1]);
}

// Read the whole story file into g_storyP and set g_storyLen.
// Exits with status 1 if the file cannot be read or is too small to hold
// a header.
static void
loadStoryFile(const char *pathP)
{
FILE *fP;
long  sizeBytes;

    if( !(fP = fopen(pathP, "rb")) )
    {
        fprintf(stderr, "zloader: can't open story file '%s'.\n", pathP);
        exit(1);
    }

    if( (fseek(fP, 0, SEEK_END) != 0) || ((sizeBytes = ftell(fP)) < 0) )
    {
        fprintf(stderr, "zloader: can't size story file '%s'.\n", pathP);
        fclose(fP);
        exit(1);
    }
    rewind(fP);

    if( sizeBytes < (long)ZH_HEADER_BYTES )
    {
        fprintf(stderr,
            "zloader: '%s' is only %ld bytes, too small to hold a Z-machine "
            "header.\n", pathP, sizeBytes);
        fclose(fP);
        exit(1);
    }

    if( !(g_storyP = (uint8_t *)malloc((size_t)sizeBytes)) )
    {
        fprintf(stderr, "zloader: out of memory reading '%s' (%ld bytes).\n",
            pathP, sizeBytes);
        fclose(fP);
        exit(1);
    }

    if( fread(g_storyP, 1, (size_t)sizeBytes, fP) != (size_t)sizeBytes )
    {
        fprintf(stderr, "zloader: short read on '%s'.\n", pathP);
        fclose(fP);
        exit(1);
    }

    fclose(fP);
    g_storyLen = sizeBytes;
}

// Parse the header into the globals, set g_installLen, and refuse a
// version the interpreter does not run or a header that cannot be a
// usable story. Exits with status 1 on a refusal.
static void
readHeader(void)
{
    g_version        = g_storyP[ZH_VERSION];
    g_release        = be16(g_storyP + ZH_RELEASE);
    g_highMemBase    = be16(g_storyP + ZH_HIGHMEM_BASE);
    g_initialPC      = be16(g_storyP + ZH_INITIAL_PC);
    g_dictAddr       = be16(g_storyP + ZH_DICT_ADDR);
    g_objTableAddr   = be16(g_storyP + ZH_OBJTABLE_ADDR);
    g_globalsAddr    = be16(g_storyP + ZH_GLOBALS_ADDR);
    g_staticBase     = be16(g_storyP + ZH_STATIC_BASE);
    g_abbrevAddr     = be16(g_storyP + ZH_ABBREV_ADDR);
    g_headerChecksum = be16(g_storyP + ZH_CHECKSUM);

    memcpy(g_serial, g_storyP + ZH_SERIAL, 6);
    g_serial[6] = '\0';

    // The version refusal comes before the file-length scale, because the
    // scale is a function of the version.
    //
    // Only V3, V4 and V5 are supported: V1, V2 and V6-V8 are refused.
    if( (g_version < 3u) || (g_version > 5u) )
    {
        fprintf(stderr,
            "zloader: story file is version %u; only versions 3, 4 and 5 "
            "are supported.\n", g_version);
        exit(1);
    }

    // The file-length field is stored divided by a version-dependent unit
    // (standard 1.1 S11.1.6): 2 for V1-V3, 4 for V4-V5, 8 for V6-V8.
    // V6-V8 have already been refused above.
    g_lengthScale     = (g_version <= 3u) ? 2u : 4u;
    g_fileLengthField = (uint32_t)be16(g_storyP + ZH_FILE_LENGTH)
                            * g_lengthScale;

    // Structural sanity, in the order that gives the most useful message:
    // a truncated file, a file that is not a story file at all, a story
    // file whose header disagrees with itself.
    if( (g_staticBase < ZH_HEADER_BYTES) || ((long)g_staticBase > g_storyLen) )
    {
        fprintf(stderr,
            "zloader: static memory base is 0x%04X, which is not inside a "
            "%ld-byte file. This is not a usable story file.\n",
            g_staticBase, g_storyLen);
        exit(1);
    }

    if( g_highMemBase < g_staticBase )
    {
        fprintf(stderr,
            "zloader: high memory starts at 0x%04X, below static memory at "
            "0x%04X. The header contradicts itself.\n",
            g_highMemBase, g_staticBase);
        exit(1);
    }

    // A file SHORTER than its header claims is truncated. A file LONGER is
    // fine and common -- real Infocom files carry padding past their
    // declared length -- so that direction is not an error.
    if( g_fileLengthField > (uint32_t)g_storyLen )
    {
        fprintf(stderr,
            "zloader: header claims %u bytes but the file is only %ld. "
            "It is truncated.\n", g_fileLengthField, g_storyLen);
        exit(1);
    }

    // The story is the file up to its DECLARED length; the bytes after it
    // are padding, which the header's checksum already leaves out
    // (S11.1.6's `verify` sums 0x40 to the declared end).
    //
    // A field of 0 installs the whole file. The standard marks the field
    // "3+" because some early V3 files carry 0 there, and zboot.ac's
    // section 8 keeps only dynamic memory resident for such a story.
    //
    // The truncation refusal above means g_installLen never exceeds the
    // file, so every byte writeDrum() copies exists.
    if( g_fileLengthField != 0u )
    {
        g_installLen = g_fileLengthField;
    }
    else
    {
        g_installLen = (uint32_t)g_storyLen;
    }

    // This test is against what is installed, dynamic memory is loaded from the drum at boot,
    // so a static base past the declared end would have boot load zeros as game state.
    if( (uint32_t)g_staticBase > g_installLen )
    {
        fprintf(stderr,
            "zloader: static memory base is 0x%04X, past the story's "
            "declared end of %u bytes. The header contradicts itself.\n",
            g_staticBase, g_installLen);
        exit(1);
    }
}

// Sum the story the way the `verify` opcode does (standard 1.1 S11.1.6's
// checksum): every byte from 0x40 up to the end of what is installed,
// added unsigned, modulo 0x10000.
//
// Precondition: readHeader() has set g_installLen, which is at least ZH_HEADER_BYTES.
//
// Returns the 16-bit sum. The caller reports a mismatch with the header's
// field but still installs the story.
static uint16_t
sumStory(void)
{
uint32_t byteIx;
uint32_t sum;

    sum = 0u;
    for( byteIx = ZH_HEADER_BYTES; byteIx < g_installLen; byteIx++ )
    {
        sum = ((sum + (uint32_t)g_storyP[byteIx]) & 0xFFFFu);
    }
    return (uint16_t)sum;
}

// Size the installed story in drum words and tracks, and dynamic memory in
// words and banks, from g_installLen and the static base; compute the
// story's checksum for the report; and refuse a story that cannot fit the
// interpreter's dynamic-memory budget or the drum.
//
// Precondition: readHeader() has run. Exits with status 1 on a refusal.
static void
computeLayout(void)
{
    // Round up on both halves of the word count. An odd static base is
    // legal, and its final word then holds one byte of dynamic memory and
    // one byte of static; the interpreter loads that word whole. The
    // installed length is odd only for a whole file installed under a
    // length field of 0; a declared length is a multiple of 2 (V3) or 4
    // (V4/V5).
    g_storyWords  = (g_installLen + 1u) / 2u;
    g_storyTracks = (g_storyWords + ZL_TRACK_SIZE - 1u) / ZL_TRACK_SIZE;
    g_storySum    = sumStory();

    g_dynWords = ((uint32_t)g_staticBase + 1u) / 2u;
    g_dynBanks = (g_dynWords + ZL_BANK_SIZE - 1u) / ZL_BANK_SIZE;

    // This cannot fire for a well-formed file: the static-memory base is a
    // 2-byte header field, so it caps at 65535 bytes = 32768 words = 8
    // banks, inside the interpreter's 12. The guard stays because the
    // budget is a number in a different file that could change.
    if( g_dynBanks > ZL_MAX_DYN_BANKS )
    {
        fprintf(stderr,
            "zloader: dynamic memory is %u words (%u banks); the "
            "interpreter has room for %u. This story cannot run here.\n",
            g_dynWords, g_dynBanks, ZL_MAX_DYN_BANKS);
        exit(1);
    }

    // Only a story that does not fit on the drum at all is refused.
    //
    // This is reachable only through a length field of 0. The field is
    // 16 bits, so a V4/V5 story declares at most 65535 x 4 = 262140 bytes
    // = 131070 words, which is inside 32 tracks (131072 words), and a V3
    // story at most 65535 x 2 bytes, 16 tracks. Only a file installed
    // whole can be too big.
    if( g_storyTracks > ZL_MAX_TRACKS )
    {
        fprintf(stderr,
            "zloader: the story needs %u drum tracks and the drum has %u. "
            "It cannot be installed here.%s\n",
            g_storyTracks, ZL_MAX_TRACKS,
            (g_fileLengthField == 0u)
                ? " (Its header's length field is 0, so the whole file was "
                  "measured.)"
                : "");
        exit(1);
    }
}

// Write the whole drum: the story from track 0 (g_storyWords words, the
// installed length), and zeros after it, so the padding a file carries
// past its declared length never reaches the drum.
//
// The drum file is one native `int` per word, tracks back to back. All 32
// tracks are written every run, so the file is always exactly the right
// size and nothing from a previous story or a different application
// survives on it. Exits with status 1 on an I/O error.
static void
writeDrum(const char *pathP)
{
FILE *fP;
int32_t *trackBufP;
uint32_t track;
uint32_t word;
uint32_t storyWordIx;
uint32_t byteOff;
uint8_t hi, lo;

    if( !(trackBufP = (int32_t *)malloc(ZL_TRACK_SIZE * sizeof(int32_t))) )
    {
        fprintf(stderr, "zloader: out of memory allocating track buffer.\n");
        exit(1);
    }

    if( !(fP = fopen(pathP, "wb")) )
    {
        fprintf(stderr, "zloader: can't open drum image '%s' for writing.\n",
            pathP);
        free(trackBufP);
        exit(1);
    }

    for( track = 0u; track < ZL_MAX_TRACKS; track++ )
    {
        memset(trackBufP, 0, ZL_TRACK_SIZE * sizeof(int32_t));

        // Tracks past the story stay zero.
        if( track < g_storyTracks )
        {
            for( word = 0u; word < ZL_TRACK_SIZE; word++ )
            {
                storyWordIx = track * ZL_TRACK_SIZE + word;
                if( storyWordIx >= g_storyWords )
                {
                    break;      // trailing words of the last track stay zero
                }

                byteOff = storyWordIx * 2u;
                hi = g_storyP[byteOff];
                // Guard the last byte of an odd installed length, which
                // only a whole file under a length field of 0 can have.
                // The guard is on what is installed, not on the file: a
                // byte of padding must not ride into the last word.
                lo = ((byteOff + 1u) < g_installLen)
                        ? g_storyP[byteOff + 1u] : 0u;
                trackBufP[word] = (int32_t)zl_pack_word(hi, lo);
            }
        }

        if( fwrite(trackBufP, sizeof(int32_t), ZL_TRACK_SIZE, fP) != ZL_TRACK_SIZE )
        {
            fprintf(stderr, "zloader: write failed on track %u of '%s'.\n",
                track, pathP);
            fclose(fP);
            free(trackBufP);
            exit(1);
        }
    }

    if( fclose(fP) != 0 )
    {
        fprintf(stderr, "zloader: error closing '%s'; the drum may be "
            "incomplete.\n", pathP);
        free(trackBufP);
        exit(1);
    }

    free(trackBufP);
}

// Print what was installed.
static void
report(const char *storyPathP, const char *drumPathP)
{
    printf("zloader: '%s' -> '%s'\n", storyPathP, drumPathP);
    printf("  Story             : V%u, release %u, serial %s, %ld bytes.\n",
        g_version, g_release, g_serial, g_storyLen);
    printf("  Header            : static 0x%04X, high 0x%04X, initial PC 0x%04X,\n",
        g_staticBase, g_highMemBase, g_initialPC);
    printf("                      dict 0x%04X, objects 0x%04X, globals 0x%04X, abbrev 0x%04X,\n",
        g_dictAddr, g_objTableAddr, g_globalsAddr, g_abbrevAddr);

    // The checksum, over the same bytes the `verify` opcode sums. A
    // mismatch is reported and the story installed anyway.
    if( g_storySum == g_headerChecksum )
    {
        printf("                      checksum valid.\n");
    }
    else
    {
        printf("                      checksum INVALID."
               "                      Installed anyway. the 'verify' command will report it.\n");
    }

    // What was installed, and what was dropped.
    if( g_fileLengthField != 0u )
    {
        printf("Declared length     : %u bytes.\n", g_fileLengthField);
    }
    else
    {
        printf("Declared length     : none (the header's field is 0); the "
            "whole %ld-byte file is installed.\n", g_storyLen);
    }
    printf("Boot set up for V%u.\n",
        g_version);
    printf("Story on drum       : %u words = tracks 0..%u.\n",
        g_storyWords, g_storyTracks - 1u);
    printf("Dynamic memory      : %u words = %u bank(s), loaded at boot into "
        "banks 1..%u.\n", g_dynWords, g_dynBanks, g_dynBanks);
    printf("%u drum tracks unused and zeroed.\n", ZL_MAX_TRACKS - g_storyTracks);

    // Saves go to whatever tape the player mounted on drive 2.
    printf("Remember, saves go to the tape mounted on drive 2.\n");
    printf("Have fun!\n");
}

// Parse the command line, then load the story, check it, write the drum
// and report. Returns 0 on success; every failure exits with status 1.
int
main(int argc, char **argv)
{
const char *storyPathP;
const char *drumPathP;
int         argIx;

    drumPathP  = DEFAULT_DRUM;
    storyPathP = NULL;

    for( argIx = 1; argIx < argc; argIx++ )
    {
        if( strcmp(argv[argIx], "-i") == 0 )
        {
            if( ++argIx >= argc )
            {
                fprintf(stderr, "zloader: -i needs a path.\n");
                usage(argv[0]);
                exit(1);
            }
            drumPathP = argv[argIx];
        }
        else if( storyPathP )
        {
            fprintf(stderr, "zloader: more than one story file given.\n");
            usage(argv[0]);
            exit(1);
        }
        else
        {
            storyPathP = argv[argIx];
        }
    }

    if( !storyPathP )
    {
        usage(argv[0]);
        exit(1);
    }

    loadStoryFile(storyPathP);
    readHeader();
    computeLayout();
    writeDrum(drumPathP);
    report(storyPathP, drumPathP);

    free(g_storyP);
    return 0;
}
