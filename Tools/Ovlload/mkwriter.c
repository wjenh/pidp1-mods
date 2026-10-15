/*
 * mkwriter -- make ovlwriter.h, the words of the PDP-1 overlay writer that ovlload -p
 * punches, from two assemblies of ovlwriter.am1 at different origins.
 *
 * ovlload puts the writer wherever bank 0 has room, so it needs to know which of the
 * writer's words hold an address of the writer's own.  A word that does is the one
 * whose two assemblies differ by exactly the difference of their origins; any other
 * difference is an error, since adding the origin would not relocate it.  The first
 * assembly must be at origin 0, so each word's value is what ovlload adds the origin to.
 *
 * The assemblies are am1's -T dumps: one line a word, its address and value in octal,
 * the last line the start or stop directive's, not a word.  The writer's labels come
 * from the symbol table of the first assembly, am1 -s: those ovlload needs, and the
 * first word past the writer's buffer, which am1 does not punch.
 *
 * Built and run only by the Makefile's writer target, so building ovlload needs no am1.
 *
 * Usage: mkwriter first.dmp second.dmp first.sym ovlwriter.h
 * Exit status: 0 written, 1 an error.  The header is opened only once everything is
 * read and checked.
 *
 * 13-Oct-2026 Claude initial version
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#define WORDMASK 0777777
#define BANKWORDS 010000
#define LINESIZE 256

// A label of the writer that ovlload needs, and the define it becomes.
typedef struct
{
    const char *labelP;
    const char *defineP;
    const char *commentP;
    long offset;
} Label;

static Label labels[] =
{
    { "ovwTracks", "OVW_TRACKS", "the survey's two words of track bits", -1 },
    { "ovwCore", "OVW_CORE", "an overlay's parameters: the window's full core address,", -1 },
    { "ovwTrack", "OVW_TRACK", "    its track,", -1 },
    { "ovwDrum", "OVW_DRUM", "    its drum address,", -1 },
    { "ovwCount", "OVW_COUNT", "    the words in its range, 1 to 010000,", -1 },
    { "ovwSum", "OVW_SUM", "    and their sum by add", -1 },
    { "ovwSurvey", "OVW_SURVEY", "the survey entry, once, after the track bits", -1 },
    { "ovwWrite", "OVW_WRITE", "the write entry, after each overlay's parameters", -1 },
    { "ovwEndRun", "OVW_END_RUN", "the end entry when the program follows", -1 },
    { "ovwEnd", "OVW_END", "the end entry when the tape holds the overlays alone", -1 },
    { "ovwBuffer", "OVW_BUFFER", "the buffer, not punched", -1 },
    { "ovwTop", "OVW_SIZE", "the first word past the writer: the core it needs", -1 },
};

#define LABELS ((int)(sizeof(labels) / sizeof(labels[0])))

static int32_t firstWords[BANKWORDS];
static int32_t secondWords[BANKWORDS];

static int readDump(const char *pathP, int32_t *wordsP, long *originP);
static bool readSymbols(const char *pathP);
static bool writeHeader(const char *pathP, int count, const bool *addressP);

// Read both dumps and the symbols, mark each word that holds an address, check
// everything, and only then write the header.  Exits 0, or 1 after saying why.
int
main(int argc, char **argv)
{
int count;
int secondCount;
int i;
int addresses;
long firstOrigin;
long secondOrigin;
long delta;
bool address[BANKWORDS];

    if( argc != 5 )
    {
        fprintf(stderr, "Usage: mkwriter first.dmp second.dmp first.sym ovlwriter.h\n");
        fprintf(stderr, "    Makes ovlwriter.h from two am1 -T dumps of ovlwriter.am1, the first at origin 0,\n");
        fprintf(stderr, "    and the first's symbol table.\n");
        exit(1);
    }

    if( ((count = readDump(argv[1], firstWords, &firstOrigin)) < 0) ||
        ((secondCount = readDump(argv[2], secondWords, &secondOrigin)) < 0) )
    {
        exit(1);
    }

    if( firstOrigin != 0 )
    {
        fprintf(stderr, "mkwriter: %s is assembled at %06lo; the first assembly must be at origin 0.\n", argv[1],
            firstOrigin);
        exit(1);
    }

    if( (secondOrigin == 0) || (secondCount != count) )
    {
        fprintf(stderr, "mkwriter: %s is at origin %06lo with %o words, %s at 0 with %o: not the same program at"
            " another origin.\n", argv[2], secondOrigin, secondCount, argv[1], count);
        exit(1);
    }

    // A word is an address of the writer's when the second assembly holds it moved by the
    // difference of the origins.  Any other difference would not be undone by relocating.
    delta = secondOrigin - firstOrigin;
    addresses = 0;
    for( i = 0; i < count; ++i )
    {
        if( secondWords[i] == firstWords[i] )
        {
            address[i] = false;
        }
        else if( (long)secondWords[i] == ((long)firstWords[i] + delta) )
        {
            address[i] = true;
            ++addresses;
        }
        else
        {
            fprintf(stderr, "mkwriter: word %04o is %06o at origin 0 and %06o at %06lo: it does not move with"
                " the writer as an address does.\n", i, firstWords[i], secondWords[i], secondOrigin);
            exit(1);
        }
    }

    if( !readSymbols(argv[3]) )
    {
        exit(1);
    }

    // Every label but the buffer and the top is a punched word.
    for( i = 0; i < LABELS; ++i )
    {
        if( labels[i].offset < 0 )
        {
            fprintf(stderr, "mkwriter: %s has no label %s.\n", argv[3], labels[i].labelP);
            exit(1);
        }

        if( (strcmp(labels[i].labelP, "ovwBuffer") != 0) && (strcmp(labels[i].labelP, "ovwTop") != 0) &&
            (labels[i].offset >= count) )
        {
            fprintf(stderr, "mkwriter: %s's label %s, %04lo, is past the %04o words punched.\n", argv[3],
                labels[i].labelP, labels[i].offset, count);
            exit(1);
        }
    }

    if( !writeHeader(argv[4], count, address) )
    {
        exit(1);
    }

    printf("mkwriter: %s: %04o words punched, %04lo with the buffer, %d of them addresses.\n", argv[4], count,
        labels[LABELS - 1].offset, addresses);
    exit(0);
}

// Read an am1 -T dump into wordsP, the first word at index 0, and its origin into
// *originP.  The words must be one run, with no gap, in one bank, and name no overlay.
// Returns how many there are, or -1 after saying why.
static int
readDump(const char *pathP, int32_t *wordsP, long *originP)
{
FILE *fP;
char line[LINESIZE];
char rest[LINESIZE];
int lineNo;
int count;
long address;
long word;
long heldAddress;
long heldWord;
bool held;

    if( !(fP = fopen(pathP, "r")) )
    {
        fprintf(stderr, "mkwriter: can't open '%s': %s\n", pathP, strerror(errno));
        return(-1);
    }

    // Each line is held until the next is read, so the last, the start or stop
    // directive's, is never taken as a word.
    lineNo = 0;
    count = 0;
    held = false;
    heldAddress = 0;
    heldWord = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        ++lineNo;
        rest[0] = 0;
        if( (sscanf(line, "%lo %lo %255s", &address, &word, rest) < 2) || (rest[0] != 0) ||
            (address < 0) || (address > 0177777) || (word < 0) || (word > WORDMASK) )
        {
            fprintf(stderr, "mkwriter: %s line %d is not an address and a word, as am1 -T writes them.\n", pathP,
                lineNo);
            fclose(fP);
            return(-1);
        }

        if( held )
        {
            if( count == 0 )
            {
                *originP = heldAddress;
            }

            if( heldAddress != (*originP + count) )
            {
                fprintf(stderr, "mkwriter: %s line %d: the words are not one run from %06lo.\n", pathP,
                    lineNo - 1, *originP);
                fclose(fP);
                return(-1);
            }

            if( (heldAddress > 07777) || (count >= BANKWORDS) )
            {
                fprintf(stderr, "mkwriter: %s line %d: the writer is not in bank 0.\n", pathP, lineNo - 1);
                fclose(fP);
                return(-1);
            }

            wordsP[count++] = (int32_t)heldWord;
        }

        held = true;
        heldAddress = address;
        heldWord = word;
    }

    fclose(fP);
    if( count == 0 )
    {
        fprintf(stderr, "mkwriter: %s holds no words.\n", pathP);
        return(-1);
    }

    return(count);
}

// Read the labels ovlload needs from an am1 symbol table into labels[].  Returns false
// after saying why if the file can't be read or is not a symbol table.
static bool
readSymbols(const char *pathP)
{
FILE *fP;
char line[LINESIZE];
char name[LINESIZE];
char kind[LINESIZE];
int lineNo;
int i;
long address;

    if( !(fP = fopen(pathP, "r")) )
    {
        fprintf(stderr, "mkwriter: can't open '%s': %s\n", pathP, strerror(errno));
        return(false);
    }

    lineNo = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        ++lineNo;
        if( lineNo == 1 )
        {
            if( strncmp(line, "%%am1 symtab file%%", 19) != 0 )
            {
                fprintf(stderr, "mkwriter: %s is not an am1 symbol table.\n", pathP);
                fclose(fP);
                return(false);
            }

            continue;
        }

        // The version and the source's name, then a symbol a line.
        if( (lineNo <= 3) || (sscanf(line, "%lo %255s %255s", &address, kind, name) != 3) )
        {
            continue;
        }

        for( i = 0; i < LABELS; ++i )
        {
            if( strcmp(name, labels[i].labelP) == 0 )
            {
                labels[i].offset = address;
            }
        }
    }

    fclose(fP);
    return(true);
}

// Write the header to pathP: the extents, the labels, and each word with its mark.
// Returns false after saying why if it can't be written.
static bool
writeHeader(const char *pathP, int count, const bool *addressP)
{
FILE *fP;
int i;

    if( !(fP = fopen(pathP, "w")) )
    {
        fprintf(stderr, "mkwriter: can't open '%s' to write it: %s\n", pathP, strerror(errno));
        return(false);
    }

    fprintf(fP, "/*\n");
    fprintf(fP, " * ovlwriter.h -- the words of the PDP-1 overlay writer, ovlwriter.am1, as assembled at\n");
    fprintf(fP, " * origin 0, for ovlload -p.  Made by mkwriter, the Makefile's writer target: do not edit.\n");
    fprintf(fP, " */\n\n");
    fprintf(fP, "#ifndef OVLWRITER_H\n");
    fprintf(fP, "#define OVLWRITER_H\n\n");
    fprintf(fP, "#include <stdint.h>\n\n");
    fprintf(fP, "// The words punched, from the origin.\n");
    fprintf(fP, "#define OVW_WORDS 0%o\n\n", count);
    fprintf(fP, "// The writer's labels, as offsets from its origin.\n");
    for( i = 0; i < LABELS; ++i )
    {
        fprintf(fP, "#define %-12s 0%04lo    // %s\n", labels[i].defineP, labels[i].offset, labels[i].commentP);
    }

    fprintf(fP, "\n// Each word as assembled at origin 0, and whether it holds an address of the writer's,\n");
    fprintf(fP, "// to which ovlload adds the origin it puts the writer at.\n");
    fprintf(fP, "typedef struct\n{\n    uint32_t word;\n    uint8_t address;\n} OvwWord;\n\n");
    fprintf(fP, "static const OvwWord ovwWords[OVW_WORDS] =\n{\n");
    for( i = 0; i < count; ++i )
    {
        fprintf(fP, "    { 0%06o, %d },    // %04o\n", firstWords[i], (addressP[i]) ? 1 : 0, i);
    }

    fprintf(fP, "};\n\n#endif\n");
    if( fclose(fP) != 0 )
    {
        fprintf(stderr, "mkwriter: writing '%s' failed: %s\n", pathP, strerror(errno));
        return(false);
    }

    return(true);
}
