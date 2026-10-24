// midi2hc makes a Standard MIDI File into source text for the Harmony
// Compiler (../harmony/harmony.am1), in the language of Peter Samson's
// hc1d, ASCII, one part to a track (to a channel in a type 0 file). The
// text names every bar, gives each note its pitch with no key signature,
// its written length (ties and triplets where needed) and its
// articulation, and carries the file's tempo map in part 1. A file the
// compiler's words cannot hold is refused, with its part and bar.
//
// readSong reads the file. noteList puts each note on the grid of 1/192
// of a whole note (for a file played in, by a snap to 64ths) and gives it
// a length and articulation from the silence before the next onset;
// tempoWords makes the tempo map hc1d's numbers. writePart writes a
// part's text, and padFills keeps the compiler's measure buffer from
// filling inside a bar. Before anything is written, readBack reads the
// text again as the compiler reads it, written apart from the writer, and
// the text is refused unless it gives back the same notes and tempos.
//
// The arithmetic is integer except for the articulations' silent shares,
// computed in doubles in the order written. Built with -ffp-contract=off
// (the Makefile), a text is the same on every host.
//
// C99 and the C library only, one thread. Most memory is never freed,
// since the program exits after one text. README.md describes its use.
//
// Usage: midi2hc [--played] [--title T] [--tempo N] MIDI OUT.txt
//
// 24-Oct-2026 Claude: written.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHOLE 192                   // units a whole note: 3 a 64th, 2 a triplet 64th
#define KEY_SEMITONE 22             // hc1d's semitone = MIDI key - 22 (C1, key 24, is 2)
#define LOW_KEY 24                  // C1, position 0
#define HIGH_KEY 84                 // C6, position 35
#define NPOS 36
#define TEMPO_K 12174               // hc1d's tempo N plays 12,174 / N quarter notes a minute
#define MAX_TEMPO 077777
#define STAFF 12                    // positions an `a' or `b' moves a note
#define DEFAULT_US 500000           // a file with no tempo: 120 a minute

// hc1d's measure buffer: a slash ends a fill from about 256 characters on
// (255-257, as the compiler counts), and 384 overflow it. A bar that
// would carry a fill to FILL_MARGIN starts a fill of its own.
#define FILL_SOFT 256
#define FILL_MARGIN 380

#define NART 5
// The written pieces of one length: a bar of 255/1 is 16,320 64ths, 129
// words of at most 127 64ths, and a triplet's two.
#define MAX_PIECES 160

typedef struct
{
    long start;
    long end;
    int key;
} MidiNote;

typedef struct
{
    MidiNote *notesP;
    int count;
    int cap;
} Group;

// A tempo's microseconds a quarter are kept as the event's bytes, leading
// zeros dropped, since an event may carry any number of them.
typedef struct
{
    long tick;
    unsigned char *bytesP;
    int nBytes;
} Tempo;

typedef struct
{
    long tick;
    int num;
    int power;                      // the denominator is 2 to this
} Meter;

typedef struct
{
    int fmt;
    long div;
    Group *groupsP;
    int nGroups;
    Tempo *temposP;
    int nTempos;
    Meter *metersP;
    int nMeters;
} Song;

typedef struct
{
    long div;
    long g;                         // ticks a 64th
    int played;
    long off;                       // the snap's offset in ticks
} Grid;

typedef struct
{
    long *startsP;                  // nBars + 1 of them, bar k+1 starting at startsP[k]
    long *unitsP;
    int nBars;
} Bars;

typedef struct
{
    long us;
    long ue;
    int key;
    long s;
    long e;
} Work;

typedef struct
{
    long start;
    long length;
    int key;
    char art;
} Listed;

typedef struct
{
    Listed *notesP;
    int count;
} Part;

typedef struct
{
    long unit;
    long n;
} TempoWord;

typedef struct
{
    long s;
    long ln;
    int key;                        // -1 for a rest
    char art;
} Item;

typedef struct
{
    int isTempo;
    long tempo;
    int key;                        // -1 for a rest
    char art;
    int trip;
    int t64;
} Piece;

typedef struct
{
    char *textP;
    size_t len;
    size_t cap;
} Text;

typedef struct
{
    char **linesP;
    int count;
    int cap;
} Lines;

static const char *usageText = "usage: midi2hc [--played] [--title T] [--tempo N] MIDI OUT.txt";

// The articulations by silent share, legato first: the first on a tie.
static const char artLetters[NART] = { 'l', 'e', 'q', 'h', 's' };
static const double artShares[NART] = { 0.0, 0.125, 0.25, 0.5, 0.625 };
static const double nearShare = 0.0625;
// A position's natural semitone in its octave, from C.
static const int steps[7] = { 0, 2, 4, 5, 7, 9, 11 };
// A pitch class written as a letter from C and its sharp (1) or flat (-1).
static const int spellLetter[12] = { 0, 0, 1, 2, 2, 3, 3, 4, 4, 5, 6, 6 };
static const int spellAlter[12] = { 0, 1, 0, -1, 0, 0, 1, 0, 1, 0, -1, 0 };
// The clefs, each with the position of its bottom line, in the order a tie
// is settled.
static const char *clefNames[4] = { "treble", "alto", "tenor", "bass" };
static const int clefOffsets[4] = { 22, 16, 14, 10 };

static int timesN[128];             // a written time in 64ths: its note value, 0 for none
static int timesDots[128];          // and its dots

// realloc that ends the program when memory runs out.
static void *growOrFail(void *ptrP, size_t count, size_t size)
{
    void *newP;

    newP = realloc(ptrP, ((count == 0) ? 1 : count) * size);
    if( newP == NULL )
    {
        printf("midi2hc: out of memory\n");
        exit(1);
    }
    return newP;
}

// A formatted string, from the heap.
static char *formatString(const char *fmtP, va_list args)
{
    va_list again;
    int n;
    char *outP;

    va_copy(again, args);
    n = vsnprintf(NULL, 0, fmtP, again);
    va_end(again);
    outP = growOrFail(NULL, (size_t)(n + 1), 1);
    vsnprintf(outP, (size_t)(n + 1), fmtP, args);
    return outP;
}

// A formatted string, from the heap.
static char *format(const char *fmtP, ...)
{
    va_list args;
    char *textP;

    va_start(args, fmtP);
    textP = formatString(fmtP, args);
    va_end(args);
    return textP;
}

// Print a refusal and end the program, status 1, with nothing written.
static void refuse(const char *fmtP, ...)
{
    va_list args;
    char *textP;

    va_start(args, fmtP);
    textP = formatString(fmtP, args);
    va_end(args);
    printf("refused: %s\n", textP);
    exit(1);
}

// Print what is wrong with the command line and the usage, and end the
// program, status 2.
static void usage(const char *fmtP, ...)
{
    va_list args;
    char *textP;

    va_start(args, fmtP);
    textP = formatString(fmtP, args);
    va_end(args);
    printf("midi2hc: %s\n%s\n", textP, usageText);
    exit(2);
}

// Add formatted text to the end of a text.
static void textAdd(Text *tP, const char *fmtP, ...)
{
    va_list args;
    char *addP;
    size_t n;

    va_start(args, fmtP);
    addP = formatString(fmtP, args);
    va_end(args);
    n = strlen(addP);
    if( (tP->len + n + 1) > tP->cap )
    {
        tP->cap = (((tP->len + n + 1) * 2) + 64);
        tP->textP = growOrFail(tP->textP, tP->cap, 1);
    }
    memcpy((tP->textP + tP->len), addP, (n + 1));
    tP->len = (tP->len + n);
    free(addP);
}

// Add a line, from the heap, to a list of lines.
static void linesAdd(Lines *lP, char *lineP)
{
    if( lP->count == lP->cap )
    {
        lP->cap = ((lP->cap * 2) + 32);
        lP->linesP = growOrFail(lP->linesP, (size_t)lP->cap, sizeof(char *));
    }
    lP->linesP[lP->count++] = lineP;
}

// Floor division, for a positive divisor.
static long long floorDiv(long long num, long long den)
{
    long long q;

    q = (num / den);
    if( ((num % den) != 0) && (num < 0) )
    {
        q--;
    }
    return q;
}

// The remainder of floorDiv, never negative.
static long long floorMod(long long num, long long den)
{
    return (num - (floorDiv(num, den) * den));
}

// num / den to the nearest integer, a half to the even one.
static long long roundHalfEven(long long num, long long den)
{
    long long q;
    long long r;

    q = floorDiv(num, den);
    r = (num - (q * den));
    if( ((2 * r) > den) || (((2 * r) == den) && ((q & 1) != 0)) )
    {
        q++;
    }
    return q;
}

// The absolute value of a double.
static double absDouble(double x)
{
    return ((x < 0.0) ? -x : x);
}

// A white-space character, as the compiler's reader here takes one: the
// ASCII spaces and the four separators 034-037.
static int isSpace(int c)
{
    return ((c == ' ') || ((c >= '\t') && (c <= '\r')) || ((c >= 034) && (c <= 037)));
}

// A string of decimal digits, at least one.
static int allDigits(const char *textP, size_t len)
{
    size_t i;

    if( len == 0 )
    {
        return 0;
    }
    for(i = 0; i < len; i++)
    {
        if( (textP[i] < '0') || (textP[i] > '9') )
        {
            return 0;
        }
    }
    return 1;
}

// The value of len decimal digits, held at a billion: only compared with
// small numbers.
static long digitsValue(const char *textP, size_t len)
{
    long v;
    size_t i;

    v = 0;
    for(i = 0; i < len; i++)
    {
        v = ((v * 10) + (textP[i] - '0'));
        if( v > 1000000000L )
        {
            v = 1000000000L;
        }
    }
    return v;
}

// The decimal text of an unsigned big-endian number of n bytes, from the
// heap, so that a refusal prints any number a file holds.
static char *bigDecimal(const unsigned char *bytesP, int n)
{
    unsigned char *workP;
    char *outP;
    int nOut;
    int i;
    int nonzero;
    unsigned rem;
    unsigned cur;
    char c;

    workP = growOrFail(NULL, (size_t)n, 1);
    memcpy(workP, bytesP, (size_t)n);
    outP = growOrFail(NULL, ((size_t)n * 3) + 2, 1);
    nOut = 0;
    do
    {
        // Divide the number by 10 in place; the remainder is the next digit.
        rem = 0;
        nonzero = 0;
        for(i = 0; i < n; i++)
        {
            cur = ((rem << 8) | workP[i]);
            workP[i] = (unsigned char)(cur / 10);
            rem = (cur % 10);
            nonzero = (nonzero || (workP[i] != 0));
        }
        outP[nOut++] = (char)('0' + rem);
    } while( nonzero );
    for(i = 0; i < (nOut / 2); i++)
    {
        c = outP[i];
        outP[i] = outP[nOut - 1 - i];
        outP[nOut - 1 - i] = c;
    }
    outP[nOut] = '\0';
    free(workP);
    return outP;
}

// The decimal text of 2 to the power p, from the heap.
static char *powerText(int p)
{
    unsigned char bytes[33];

    memset(bytes, 0, sizeof(bytes));
    bytes[32 - (p / 8)] = (unsigned char)(1u << (p % 8));
    return bigDecimal(bytes, 33);
}

// Mark the times, in 64ths, one note word can write, with their note
// values and dots: a base of 1 to 64 64ths and its dots, each dot half
// the last, none under a 64th, at most 127 (the word's seven bits).
static void makeTimes(void)
{
    int b;
    int t;
    int h;
    int d;

    for(b = 64; b >= 1; b = (b / 2))
    {
        t = b;
        h = b;
        d = 0;
        while( 1 )
        {
            if( (t <= 127) && (timesN[t] == 0) )
            {
                timesN[t] = (64 / b);
                timesDots[t] = d;
            }
            h = (h / 2);
            if( h < 1 )
            {
                break;
            }
            t = (t + h);
            d++;
        }
    }
}

// Reading the file.

// The n-byte big-endian number at pP.
static long bigEndian(const unsigned char *pP, int n)
{
    long v;
    int i;

    v = 0;
    for(i = 0; i < n; i++)
    {
        v = ((v << 8) | pP[i]);
    }
    return v;
}

// The variable-length number at *iP in a track of len bytes, *iP moved past
// it. A track that ends inside it is refused.
static long readVlq(const unsigned char *tP, long len, long *iP)
{
    long v;
    int b;

    v = 0;
    while( 1 )
    {
        if( *iP >= len )
        {
            refuse("the file ends inside an event");
        }
        b = tP[(*iP)++];
        v = ((v << 7) | (b & 0x7f));
        if( (b & 0x80) == 0 )
        {
            return v;
        }
    }
}

// Tempos by tick, then by microseconds.
static int cmpTempo(const Tempo *aP, const Tempo *bP)
{
    int c;

    if( aP->tick != bP->tick )
    {
        return ((aP->tick < bP->tick) ? -1 : 1);
    }
    if( aP->nBytes != bP->nBytes )
    {
        return ((aP->nBytes < bP->nBytes) ? -1 : 1);
    }
    c = memcmp(aP->bytesP, bP->bytesP, (size_t)aP->nBytes);
    return ((c < 0) ? -1 : ((c > 0) ? 1 : 0));
}

// Meters by tick, numerator and denominator.
static int cmpMeter(const Meter *aP, const Meter *bP)
{
    if( aP->tick != bP->tick )
    {
        return ((aP->tick < bP->tick) ? -1 : 1);
    }
    if( aP->num != bP->num )
    {
        return ((aP->num < bP->num) ? -1 : 1);
    }
    if( aP->power != bP->power )
    {
        return ((aP->power < bP->power) ? -1 : 1);
    }
    return 0;
}

// MIDI notes by start, end and key.
static int cmpMidiNote(const void *aP, const void *bP)
{
    const MidiNote *a = aP;
    const MidiNote *b = bP;

    if( a->start != b->start )
    {
        return ((a->start < b->start) ? -1 : 1);
    }
    if( a->end != b->end )
    {
        return ((a->end < b->end) ? -1 : 1);
    }
    return ((a->key < b->key) ? -1 : ((a->key > b->key) ? 1 : 0));
}

// Notes on the grid by onset, end, key, then their ticks.
static int cmpWork(const void *aP, const void *bP)
{
    const Work *a = aP;
    const Work *b = bP;

    if( a->us != b->us )
    {
        return ((a->us < b->us) ? -1 : 1);
    }
    if( a->ue != b->ue )
    {
        return ((a->ue < b->ue) ? -1 : 1);
    }
    if( a->key != b->key )
    {
        return ((a->key < b->key) ? -1 : 1);
    }
    if( a->s != b->s )
    {
        return ((a->s < b->s) ? -1 : 1);
    }
    return ((a->e < b->e) ? -1 : ((a->e > b->e) ? 1 : 0));
}

// Add a note to a group.
static void addNote(Group *gP, long start, long end, int key)
{
    if( gP->count == gP->cap )
    {
        gP->cap = ((gP->cap * 2) + 64);
        gP->notesP = growOrFail(gP->notesP, (size_t)gP->cap, sizeof(MidiNote));
    }
    gP->notesP[gP->count].start = start;
    gP->notesP[gP->count].end = end;
    gP->notesP[gP->count].key = key;
    gP->count++;
}

// Read one track. Its notes go into its groups (one a channel in a type 0
// file, else one for the track), its tempo and meter events into the
// song's. A note on is ended by the next note off of its key and channel,
// the earliest pending first.
static void readTrack(Song *songP, const unsigned char *tP, long len, Group *groupsP,
                      int *capTemposP, int *capMetersP)
{
    long j;
    long tick;
    int run;
    int st;
    int typ;
    long n;
    long have;
    long k;
    int ch;
    int kind;
    int key;
    long *pendTickP;
    int *pendKeyP;
    int nPend;
    int capPend;
    int p;
    Tempo *newP;

    j = 0;
    tick = 0;
    run = -1;
    pendTickP = NULL;
    pendKeyP = NULL;
    nPend = 0;
    capPend = 0;
    while( j < len )
    {
        tick = (tick + readVlq(tP, len, &j));
        if( j >= len )
        {
            refuse("the file ends inside an event");
        }
        st = tP[j];
        if( st == 0xff )
        {
            // A meta event's data runs on past a track's end only as far as the track.
            if( (j + 1) >= len )
            {
                refuse("the file ends inside an event");
            }
            typ = tP[j + 1];
            j = (j + 2);
            n = readVlq(tP, len, &j);
            have = (((j + n) <= len) ? n : (len - j));
            if( typ == 0x51 )
            {
                if( songP->nTempos == *capTemposP )
                {
                    *capTemposP = ((*capTemposP * 2) + 16);
                    songP->temposP = growOrFail(songP->temposP, (size_t)*capTemposP, sizeof(Tempo));
                }
                newP = &songP->temposP[songP->nTempos++];
                newP->tick = tick;
                for(k = 0; (k < have) && (tP[j + k] == 0); k++)
                {
                }
                newP->nBytes = (int)(have - k);
                newP->bytesP = growOrFail(NULL, (size_t)newP->nBytes, 1);
                memcpy(newP->bytesP, (tP + j + k), (size_t)newP->nBytes);
            }
            else if( typ == 0x58 )
            {
                if( have < 2 )
                {
                    refuse("the file ends inside an event");
                }
                if( songP->nMeters == *capMetersP )
                {
                    *capMetersP = ((*capMetersP * 2) + 16);
                    songP->metersP = growOrFail(songP->metersP, (size_t)*capMetersP, sizeof(Meter));
                }
                songP->metersP[songP->nMeters].tick = tick;
                songP->metersP[songP->nMeters].num = tP[j];
                songP->metersP[songP->nMeters].power = tP[j + 1];
                songP->nMeters++;
            }
            j = (j + n);
            continue;
        }
        if( (st == 0xf0) || (st == 0xf7) )
        {
            j = (j + 1);
            n = readVlq(tP, len, &j);
            j = (j + n);
            continue;
        }
        if( (st & 0x80) != 0 )
        {
            run = st;
            j++;
        }
        if( run < 0 )
        {
            refuse("a data byte with no status");
        }
        n = ((((run & 0xf0) == 0xc0) || ((run & 0xf0) == 0xd0)) ? 1 : 2);
        ch = (run & 0x0f);
        kind = (run & 0xf0);
        if( (kind == 0x80) || (kind == 0x90) )
        {
            // A note off needs only its key; a note on its velocity too.
            if( (j + ((kind == 0x90) ? 2 : 1)) > len )
            {
                refuse("the file ends inside an event");
            }
            key = tP[j];
            if( (kind == 0x90) && (tP[j + 1] != 0) )
            {
                if( nPend == capPend )
                {
                    capPend = ((capPend * 2) + 64);
                    pendTickP = growOrFail(pendTickP, (size_t)capPend, sizeof(long));
                    pendKeyP = growOrFail(pendKeyP, (size_t)capPend, sizeof(int));
                }
                pendTickP[nPend] = tick;
                pendKeyP[nPend] = ((ch << 8) | key);
                nPend++;
            }
            else
            {
                for(p = 0; p < nPend; p++)
                {
                    if( pendKeyP[p] == ((ch << 8) | key) )
                    {
                        break;
                    }
                }
                if( p < nPend )
                {
                    addNote(&groupsP[(songP->fmt == 0) ? ch : 0], pendTickP[p], tick, key);
                    memmove((pendTickP + p), (pendTickP + p + 1), ((size_t)(nPend - p - 1) * sizeof(long)));
                    memmove((pendKeyP + p), (pendKeyP + p + 1), ((size_t)(nPend - p - 1) * sizeof(int)));
                    nPend--;
                }
            }
        }
        j = (j + n);
    }
    free(pendTickP);
    free(pendKeyP);
}

// Read a whole file into the heap. Returns NULL if it cannot be read.
static unsigned char *readWhole(const char *pathP, long *sizeP)
{
    FILE *fP;
    unsigned char *dP;
    long cap;
    long got;
    int bad;

    if( !(fP = fopen(pathP, "rb")) )
    {
        return NULL;
    }
    cap = 65536;
    *sizeP = 0;
    dP = growOrFail(NULL, (size_t)cap, 1);
    while( (got = (long)fread((dP + *sizeP), 1, (size_t)(cap - *sizeP), fP)) > 0 )
    {
        *sizeP = (*sizeP + got);
        if( *sizeP == cap )
        {
            cap = (cap * 2);
            dP = growOrFail(dP, (size_t)cap, 1);
        }
    }
    // A directory opens, then fails to read.
    bad = ferror(fP);
    fclose(fP);
    if( bad )
    {
        free(dP);
        return NULL;
    }
    return dP;
}

// Read a MIDI file into its groups with notes, in track and channel order,
// each sorted, and its tempo map and meters, each sorted with repeats
// removed. A file it cannot take is refused.
static void readSong(const char *pathP, Song *songP)
{
    unsigned char *dP;
    long size;
    long i;
    long ln;
    Group trackGroups[16];
    int capTempos;
    int capMeters;
    int k;
    int n;
    int m;
    Tempo t;
    Meter mt;

    if( !(dP = readWhole(pathP, &size)) )
    {
        refuse("%s cannot be read", pathP);
    }
    if( (size < 4) || (memcmp(dP, "MThd", 4) != 0) )
    {
        refuse("not a Standard MIDI File");
    }
    if( size < 14 )
    {
        refuse("the file ends inside its header");
    }
    memset(songP, 0, sizeof(Song));
    songP->fmt = (int)bigEndian((dP + 8), 2);
    songP->div = bigEndian((dP + 12), 2);
    if( (songP->div & 0x8000) != 0 )
    {
        refuse("SMPTE time is not taken");
    }
    capTempos = 0;
    capMeters = 0;
    songP->groupsP = growOrFail(NULL, 16, sizeof(Group));
    i = (8 + bigEndian((dP + 4), 4));
    while( (i + 8) <= size )
    {
        ln = bigEndian((dP + i + 4), 4);
        if( memcmp((dP + i), "MTrk", 4) == 0 )
        {
            memset(trackGroups, 0, sizeof(trackGroups));
            readTrack(songP, (dP + i + 8), (((i + 8 + ln) <= size) ? ln : (size - (i + 8))), trackGroups,
                      &capTempos, &capMeters);
            for(k = 0; k < 16; k++)
            {
                if( trackGroups[k].count > 0 )
                {
                    songP->groupsP = growOrFail(songP->groupsP, (size_t)(songP->nGroups + 1), sizeof(Group));
                    qsort(trackGroups[k].notesP, (size_t)trackGroups[k].count, sizeof(MidiNote), cmpMidiNote);
                    songP->groupsP[songP->nGroups++] = trackGroups[k];
                }
            }
        }
        i = (i + 8 + ln);
    }
    if( songP->nGroups == 0 )
    {
        refuse("no notes");
    }
    if( songP->nTempos == 0 )
    {
        songP->temposP = growOrFail(NULL, 1, sizeof(Tempo));
        songP->temposP[0].tick = 0;
        songP->temposP[0].bytesP = growOrFail(NULL, 3, 1);
        songP->temposP[0].bytesP[0] = ((DEFAULT_US >> 16) & 0xff);
        songP->temposP[0].bytesP[1] = ((DEFAULT_US >> 8) & 0xff);
        songP->temposP[0].bytesP[2] = (DEFAULT_US & 0xff);
        songP->temposP[0].nBytes = 3;
        songP->nTempos = 1;
    }
    // An insertion sort, a few events at most, then the repeats dropped.
    for(n = 1; n < songP->nTempos; n++)
    {
        t = songP->temposP[n];
        for(m = n; (m > 0) && (cmpTempo(&songP->temposP[m - 1], &t) > 0); m--)
        {
            songP->temposP[m] = songP->temposP[m - 1];
        }
        songP->temposP[m] = t;
    }
    for(n = 0, m = 0; n < songP->nTempos; n++)
    {
        if( (m == 0) || (cmpTempo(&songP->temposP[m - 1], &songP->temposP[n]) != 0) )
        {
            songP->temposP[m++] = songP->temposP[n];
        }
    }
    songP->nTempos = m;
    if( songP->nMeters == 0 )
    {
        songP->metersP = growOrFail(NULL, 1, sizeof(Meter));
        songP->metersP[0].tick = 0;
        songP->metersP[0].num = 4;
        songP->metersP[0].power = 2;
        songP->nMeters = 1;
    }
    for(n = 1; n < songP->nMeters; n++)
    {
        mt = songP->metersP[n];
        for(m = n; (m > 0) && (cmpMeter(&songP->metersP[m - 1], &mt) > 0); m--)
        {
            songP->metersP[m] = songP->metersP[m - 1];
        }
        songP->metersP[m] = mt;
    }
    for(n = 0, m = 0; n < songP->nMeters; n++)
    {
        if( (m == 0) || (cmpMeter(&songP->metersP[m - 1], &songP->metersP[n]) != 0) )
        {
            songP->metersP[m++] = songP->metersP[n];
        }
    }
    songP->nMeters = m;
}

// Time.

// Set up the grid. For a file played in, the offset is the one in ticks
// that puts the onsets nearest a 64th grid, the smallest on a tie.
static void makeGrid(Grid *gridP, const Song *songP, int played)
{
    long o;
    long long cost;
    long long best;
    long long m;
    int k;
    int n;

    if( songP->div == 0 )
    {
        refuse("a division of 0 ticks is not taken");
    }
    if( (songP->div % 48) != 0 )
    {
        refuse("a division of %ld ticks is not a multiple of 48", songP->div);
    }
    gridP->div = songP->div;
    gridP->g = (songP->div / 16);
    gridP->played = played;
    gridP->off = 0;
    if( !played )
    {
        return;
    }
    best = -1;
    for(o = 0; o < gridP->g; o++)
    {
        cost = 0;
        for(k = 0; k < songP->nGroups; k++)
        {
            for(n = 0; n < songP->groupsP[k].count; n++)
            {
                m = floorMod((songP->groupsP[k].notesP[n].start - o), gridP->g);
                cost = (cost + ((m < (gridP->g - m)) ? m : (gridP->g - m)));
            }
        }
        if( (best < 0) || (cost < best) )
        {
            best = cost;
            gridP->off = o;
        }
    }
}

// The unit of tick t in *uP. Returns 1, or 0 off the grid (for a file not
// played in, not a whole unit).
static int unitOf(const Grid *gridP, long t, long *uP)
{
    if( gridP->played )
    {
        *uP = (long)(3 * roundHalfEven((t - gridP->off), gridP->g));
        return 1;
    }
    if( ((t * 48) % gridP->div) != 0 )
    {
        return 0;
    }
    *uP = ((t * 48) / gridP->div);
    return 1;
}

// The bars from the meters, to the end unit. The meter in force at a
// bar's start is the latest at or before it, or before the first meter
// the first. A meter that is not a whole number of 32nds is refused.
static void makeBars(Bars *barsP, const Song *songP, long end)
{
    long at;
    long ln;
    int cap;
    int k;
    long mt;
    long mt0;
    int num;
    int power;

    cap = 64;
    barsP->startsP = growOrFail(NULL, (size_t)(cap + 1), sizeof(long));
    barsP->unitsP = growOrFail(NULL, (size_t)cap, sizeof(long));
    barsP->nBars = 0;
    barsP->startsP[0] = 0;
    mt0 = ((songP->metersP[0].tick * 48) / songP->div);
    while( 1 )
    {
        at = barsP->startsP[barsP->nBars];
        num = 0;
        power = 0;
        for(k = (songP->nMeters - 1); k >= 0; k--)
        {
            mt = ((songP->metersP[k].tick * 48) / songP->div);
            if( (mt <= at) || (mt == mt0) )
            {
                num = songP->metersP[k].num;
                power = songP->metersP[k].power;
                break;
            }
        }
        // A bar of no length is refused too, since it never reaches the end.
        ln = ((power <= 40) ? ((WHOLE * (long)num) >> power) : 0);
        if( (power > 40) || ((ln << power) != (WHOLE * (long)num)) || ((ln % 6) != 0) || (ln == 0) )
        {
            refuse("the meter %d/%s is not a whole number of 32nds", num, powerText(power));
        }
        if( barsP->nBars == cap )
        {
            cap = (cap * 2);
            barsP->startsP = growOrFail(barsP->startsP, (size_t)(cap + 1), sizeof(long));
            barsP->unitsP = growOrFail(barsP->unitsP, (size_t)cap, sizeof(long));
        }
        barsP->unitsP[barsP->nBars] = ln;
        barsP->startsP[barsP->nBars + 1] = (at + ln);
        barsP->nBars++;
        if( barsP->startsP[barsP->nBars] >= end )
        {
            break;
        }
    }
}

// The bar, from 1, that unit u falls in.
static int barOf(const Bars *barsP, long u)
{
    int k;

    k = 0;
    while( ((k + 1) < (barsP->nBars + 1)) && (barsP->startsP[k + 1] <= u) )
    {
        k++;
    }
    return (k + 1);
}

// The notes.

// The articulation nearest a silent share, as an index into artLetters,
// the first on a tie.
static int nearestArt(double silent)
{
    double x;
    int best;
    int k;

    x = ((silent > 0.0) ? silent : 0.0);
    best = 0;
    for(k = 1; k < NART; k++)
    {
        if( absDouble(artShares[k] - x) < absDouble(artShares[best] - x) )
        {
            best = k;
        }
    }
    return best;
}

// Each part's notes, (start, length, key, articulation) in units. A note
// runs to the next onset when the silence before it is within 1/16 of an
// articulation's share, or, played in, a 64th or less; otherwise it keeps
// its own end, its articulation by its own silence, and a rest follows.
// For a file played in, *movedMaxP, *movedOverP and *nMovedP report the
// snap: the most an onset moved in ticks, how many moved more than 10.
static Part *noteList(const Song *songP, const Grid *gridP, const Bars *barsP,
                      long *movedMaxP, long *movedOverP, long *nMovedP)
{
    Part *partsP;
    Work *wP;
    const MidiNote *mP;
    int pn;
    int k;
    int n;
    long us;
    long ue;
    long nxt;
    long moved;
    double ticksTo;
    double silent;
    int a;

    partsP = growOrFail(NULL, (size_t)songP->nGroups, sizeof(Part));
    *movedMaxP = 0;
    *movedOverP = 0;
    *nMovedP = 0;
    for(pn = 0; pn < songP->nGroups; pn++)
    {
        n = songP->groupsP[pn].count;
        wP = growOrFail(NULL, (size_t)n, sizeof(Work));
        for(k = 0; k < n; k++)
        {
            mP = &songP->groupsP[pn].notesP[k];
            if( !unitOf(gridP, mP->start, &us) || !unitOf(gridP, mP->end, &ue) )
            {
                refuse("part %d, bar %d: a note off the grid of 64ths and triplet 64ths "
                       "(shorter than a 64th, or a tuplet hc1d has not)",
                       (pn + 1), barOf(barsP, (unitOf(gridP, mP->start, &us) ? us : ((mP->start * 48) / gridP->div))));
            }
            if( gridP->played )
            {
                moved = (mP->start - gridP->off - ((us * gridP->g) / 3));
                moved = ((moved < 0) ? -moved : moved);
                *movedMaxP = ((moved > *movedMaxP) ? moved : *movedMaxP);
                *movedOverP = (*movedOverP + (moved > 10));
                (*nMovedP)++;
            }
            if( (mP->key < LOW_KEY) || (mP->key > HIGH_KEY) )
            {
                refuse("part %d, bar %d: key %d is outside C1-C6", (pn + 1), barOf(barsP, us), mP->key);
            }
            wP[k].us = us;
            wP[k].ue = ue;
            wP[k].key = mP->key;
            wP[k].s = mP->start;
            wP[k].e = mP->end;
        }
        qsort(wP, (size_t)n, sizeof(Work), cmpWork);
        for(k = 1; k < n; k++)
        {
            if( (wP[k].us == wP[k - 1].us) || ((wP[k].us < wP[k - 1].ue) && !gridP->played) )
            {
                refuse("part %d, bar %d: two notes at once", (pn + 1), barOf(barsP, wP[k].us));
            }
        }
        partsP[pn].notesP = growOrFail(NULL, (size_t)n, sizeof(Listed));
        partsP[pn].count = n;
        for(k = 0; k < n; k++)
        {
            us = wP[k].us;
            ue = wP[k].ue;
            nxt = (((k + 1) < n) ? wP[k + 1].us : barsP->startsP[barsP->nBars]);
            if( ue > nxt )
            {
                ue = nxt;               // played: the next onset cuts it
            }
            if( ue <= us )
            {
                ue = (us + 3);
            }
            ticksTo = (double)((((nxt * gridP->g) / 3) + gridP->off) - wP[k].s);
            silent = (1.0 - ((double)(wP[k].e - wP[k].s) / ticksTo));
            a = nearestArt(silent);
            partsP[pn].notesP[k].start = us;
            partsP[pn].notesP[k].key = wP[k].key;
            // Played in, a silence of a 64th or less on the grid is the hand's, not a rest.
            if( (absDouble(artShares[a] - ((silent > 0.0) ? silent : 0.0)) <= nearShare)
                || (gridP->played && ((nxt - ue) <= 3)) )
            {
                partsP[pn].notesP[k].length = (nxt - us);
                partsP[pn].notesP[k].art = artLetters[a];
                continue;
            }
            silent = (1.0 - ((double)(wP[k].e - wP[k].s) / (double)(((ue - us) * gridP->g) / 3)));
            partsP[pn].notesP[k].length = (ue - us);
            partsP[pn].notesP[k].art = artLetters[nearestArt(silent)];
        }
        free(wP);
    }
    return partsP;
}

// hc1d's tempo words, (unit, N), from the tempo map; a later change at the
// same unit replaces an earlier one. Refused off the grid, or outside
// hc1d's numbers. Returns the words, their count in *countP.
static TempoWord *tempoWords(const Song *songP, const Grid *gridP, const Bars *barsP, int *countP)
{
    TempoWord *wordsP;
    int nWords;
    int k;
    int j;
    long u;
    long long us;
    long long n;
    const Tempo *tP;

    wordsP = growOrFail(NULL, (size_t)songP->nTempos, sizeof(TempoWord));
    nWords = 0;
    for(k = 0; k < songP->nTempos; k++)
    {
        tP = &songP->temposP[k];
        if( !unitOf(gridP, tP->tick, &u) )
        {
            refuse("a tempo change off the grid at bar %d", barOf(barsP, ((tP->tick * 48) / gridP->div)));
        }
        // From 2^40 microseconds on, N is far past hc1d's largest.
        if( tP->nBytes > 5 )
        {
            refuse("tempo %s us is outside hc1d's numbers", bigDecimal(tP->bytesP, tP->nBytes));
        }
        us = bigEndian(tP->bytesP, tP->nBytes);
        // K x us / 60,000,000, a half up.
        n = (((2LL * TEMPO_K * us) + 60000000LL) / 120000000LL);
        if( (n < 1) || (n > MAX_TEMPO) )
        {
            refuse("tempo %lld us is outside hc1d's numbers", us);
        }
        u = ((u > 0) ? u : 0);
        for(j = 0; j < nWords; j++)
        {
            if( wordsP[j].unit == u )
            {
                memmove((wordsP + j), (wordsP + j + 1), ((size_t)(nWords - j - 1) * sizeof(TempoWord)));
                nWords--;
                break;
            }
        }
        wordsP[nWords].unit = u;
        wordsP[nWords].n = (long)n;
        nWords++;
    }
    *countP = nWords;
    return wordsP;
}

// The tempo words with the first made n and each later one in the same
// proportion, a half up: the piece at another speed, its tempo changes
// kept. Refused if a word falls outside hc1d's numbers.
static void scaleTempos(TempoWord *wordsP, int count, long n)
{
    long long first;
    long long w;
    int k;

    first = wordsP[0].n;
    for(k = 0; k < count; k++)
    {
        w = floorDiv(((2LL * wordsP[k].n * n) + first), (2LL * first));
        if( (w < 1) || (w > MAX_TEMPO) )
        {
            refuse("tempo %ld makes a tempo word of %lld, outside hc1d's numbers", n, w);
        }
    }
    for(k = 0; k < count; k++)
    {
        wordsP[k].n = (long)floorDiv(((2LL * wordsP[k].n * n) + first), (2LL * first));
    }
}

// A part's notes with a legato note followed at once by its own pitch
// joined to it: a tie, or a repeat the player holds as one.
static Part joined(const Part *pP)
{
    Part out;
    Listed *lastP;
    int k;

    out.notesP = growOrFail(NULL, (size_t)pP->count, sizeof(Listed));
    out.count = 0;
    for(k = 0; k < pP->count; k++)
    {
        lastP = ((out.count > 0) ? &out.notesP[out.count - 1] : NULL);
        if( (lastP != NULL) && (lastP->art == 'l') && (lastP->key == pP->notesP[k].key)
            && ((lastP->start + lastP->length) == pP->notesP[k].start) )
        {
            lastP->length = (lastP->length + pP->notesP[k].length);
            lastP->art = pP->notesP[k].art;
        }
        else
        {
            out.notesP[out.count++] = pP->notesP[k];
        }
    }
    return out;
}

// The text.

// A position's natural semitone.
static int natural(int i)
{
    return (2 + (12 * (i / 7)) + steps[i % 7]);
}

// The position from C1 of a MIDI key, its sharp or flat in *alterP. A
// black key is spelled sharp or flat by spellAlter, unless that position
// is one avoid marks and the other spelling's is not: then the letter
// below, sharp, or above, flat.
static int indexOf(int key, const int *avoidP, int *alterP)
{
    int letter;
    int alter;
    int i;
    int j;
    int d;

    letter = spellLetter[key % 12];
    alter = spellAlter[key % 12];
    i = ((7 * ((key / 12) - 2)) + letter);
    if( (alter != 0) && (avoidP != NULL) && (i >= 0) && (i < NPOS) && avoidP[i] )
    {
        j = ((alter < 0) ? (i - 1) : (i + 1));
        if( (j >= 0) && (j < NPOS) && !avoidP[j] )
        {
            d = ((key - KEY_SEMITONE) - natural(j));
            if( (d == 1) || (d == -1) )
            {
                *alterP = d;
                return j;
            }
        }
    }
    *alterP = alter;
    return i;
}

// A length in units as written pieces, largest first, each a note value's
// time in 64ths (*t64P) and whether it is triplets (*tripP): triplets
// where the length is not a multiple of 3 (all of it if it is one of 2,
// else the least that leaves a multiple of 3). Returns the count.
static int piecesOf(long length, int *tripP, int *t64P, int cap)
{
    long trip;
    long n;
    int v;
    int count;
    int pass;

    if( (length % 3) == 0 )
    {
        trip = 0;
    }
    else if( (length % 2) == 0 )
    {
        trip = length;
    }
    else
    {
        trip = ((((length - 2) % 3) == 0) ? 2 : 4);
        if( trip > length )
        {
            refuse("a length of %ld units", length);
        }
    }
    count = 0;
    for(pass = 0; pass < 2; pass++)
    {
        n = ((pass == 0) ? ((length - trip) / 3) : (trip / 2));
        while( n > 0 )
        {
            for(v = ((n < 127) ? (int)n : 127); timesN[v] == 0; v--)
            {
            }
            if( count == cap )
            {
                return -1;
            }
            tripP[count] = pass;
            t64P[count] = v;
            count++;
            n = (n - v);
        }
    }
    return count;
}

// Add an item, splitting it at each cut inside it; every piece but its
// last is legato, a rest's pieces rests.
static void addCutItem(Item **itemsP, int *countP, int *capP, const Item *iP, const long *cutsP, int nCuts)
{
    long a;
    long b;
    int k;

    a = iP->s;
    for(k = 0; k <= nCuts; k++)
    {
        if( (k < nCuts) && ((cutsP[k] <= iP->s) || (cutsP[k] >= (iP->s + iP->ln))) )
        {
            continue;
        }
        b = ((k < nCuts) ? cutsP[k] : (iP->s + iP->ln));
        if( *countP == *capP )
        {
            *capP = ((*capP * 2) + 64);
            *itemsP = growOrFail(*itemsP, (size_t)*capP, sizeof(Item));
        }
        (*itemsP)[*countP].s = a;
        (*itemsP)[*countP].ln = (b - a);
        (*itemsP)[*countP].key = iP->key;
        (*itemsP)[*countP].art = (((b == (iP->s + iP->ln)) || (iP->key < 0)) ? iP->art : 'l');
        (*countP)++;
        a = b;
    }
}

// Add a piece to a bar's list.
static void addPiece(Piece **piecesP, int *countP, int *capP, const Piece *pP)
{
    if( *countP == *capP )
    {
        *capP = ((*capP * 2) + 64);
        *piecesP = growOrFail(*piecesP, (size_t)*capP, sizeof(Piece));
    }
    (*piecesP)[(*countP)++] = *pP;
}

// The tempo word at unit u, or -1.
static long tempoAt(const TempoWord *wordsP, int count, long u)
{
    int k;

    for(k = 0; k < count; k++)
    {
        if( wordsP[k].unit == u )
        {
            return wordsP[k].n;
        }
    }
    return -1;
}

// A part's lines, with spaces before a bar's slash where the bar after it
// would carry a fill of hc1d's measure buffer past FILL_MARGIN: the fill
// then ends at that slash (past the threshold, whichever of 255-257 it
// is) and the long bar starts the next. A run of spaces is no word to the
// compiler. A bar too long for the buffer even then is refused.
static void padFills(Lines *lP, int pn)
{
    long n;
    int k;
    size_t len;
    size_t nextLen;
    char *lineP;
    long pad;
    const char *spaceP;

    n = 0;
    for(k = 1; k < lP->count; k++)
    {
        len = strlen(lP->linesP[k]);
        n = (n + 1 + (long)len);        // the line end before it, as a space, and the line
        if( (len == 0) || (lP->linesP[k][len - 1] != '/') )
        {
            continue;
        }
        nextLen = (((k + 1) < lP->count) ? strlen(lP->linesP[k + 1]) : 0);
        if( n > (FILL_SOFT + 1) )
        {
            n = 0;                      // the fill ends here at any of the thresholds
        }
        else if( (n >= FILL_SOFT) || (((k + 1) < lP->count) && ((n + 1 + (long)nextLen) >= FILL_MARGIN)) )
        {
            if( ((k + 1) < lP->count) && ((1 + (long)nextLen) >= FILL_MARGIN) )
            {
                spaceP = strchr(lP->linesP[k + 1], ' ');
                refuse("part %d, bar %.*s: too many characters for hc1d's measure buffer", pn,
                       (int)((spaceP != NULL) ? (size_t)(spaceP - lP->linesP[k + 1]) : nextLen),
                       lP->linesP[k + 1]);
            }
            pad = ((FILL_SOFT + 2) - n);
            lineP = growOrFail(NULL, (len + (size_t)pad + 1), 1);
            memcpy(lineP, lP->linesP[k], (len - 1));
            memset((lineP + len - 1), ' ', (size_t)pad);
            lineP[len - 1 + (size_t)pad] = '/';
            lineP[len + (size_t)pad] = '\0';
            lP->linesP[k] = lineP;
            n = 0;
        }
    }
}

// Write one part's text, from its title line to `end', onto outP: its
// clef (the one whose middle line is nearest the part's median note), its
// units at bar 1 and where the meter changes, its tempo words in part 1,
// and a line per bar.
static void writePart(Text *outP, const Part *partP, int pn, int nParts, const TempoWord *tempoP, int nTempo,
                      const Bars *barsP, const char *titleP)
{
    Lines lines;
    int *idxP;
    int alter;
    int k;
    int j;
    int m;
    int b;
    int med;
    int clef;
    int clefOff;
    long *cutsP;
    int nCuts;
    Item *cutP;
    int nCut;
    int capCut;
    Item it;
    long t;
    long b1;
    char mode;
    Text words;
    Piece *piecesP;
    int nPieces;
    int capPieces;
    Piece pc;
    int trips[MAX_PIECES];
    int t64s[MAX_PIECES];
    int np;
    long tw;
    int artCount[128];
    char best;
    const char *orderP;
    int white[NPOS];
    int acc[NPOS];
    int i;
    int want;
    int pos;
    int nb;
    const Piece *nxtP;
    char sign[8];
    char letter[2];
    char dots[16];
    char letters[48];
    char *p;

    // The clef.
    idxP = growOrFail(NULL, (size_t)(partP->count + 1), sizeof(int));
    for(k = 0; k < partP->count; k++)
    {
        idxP[k] = indexOf(partP->notesP[k].key, NULL, &alter);
    }
    for(k = 1; k < partP->count; k++)
    {
        med = idxP[k];
        for(j = k; (j > 0) && (idxP[j - 1] > med); j--)
        {
            idxP[j] = idxP[j - 1];
        }
        idxP[j] = med;
    }
    med = ((partP->count > 0) ? idxP[partP->count / 2] : 27);
    clef = 0;
    for(k = 1; k < 4; k++)
    {
        if( abs(clefOffsets[k] + 5 - med) < abs(clefOffsets[clef] + 5 - med) )
        {
            clef = k;
        }
    }
    clefOff = clefOffsets[clef];
    free(idxP);
    memset(&lines, 0, sizeof(lines));
    linesAdd(&lines, format("%s, part %d of %d/", titleP, (pn + 1), nParts));
    linesAdd(&lines, format("%s", clefNames[clef]));

    // The cuts: the bar lines and, in part 1, the tempo changes, sorted.
    cutsP = growOrFail(NULL, (size_t)(barsP->nBars + 1 + nTempo), sizeof(long));
    nCuts = 0;
    for(k = 0; k <= barsP->nBars; k++)
    {
        cutsP[nCuts++] = barsP->startsP[k];
    }
    for(k = 0; (pn == 0) && (k < nTempo); k++)
    {
        cutsP[nCuts++] = tempoP[k].unit;
    }
    for(k = 1; k < nCuts; k++)
    {
        t = cutsP[k];
        for(j = k; (j > 0) && (cutsP[j - 1] > t); j--)
        {
            cutsP[j] = cutsP[j - 1];
        }
        cutsP[j] = t;
    }
    for(k = 0, m = 0; k < nCuts; k++)
    {
        if( (m == 0) || (cutsP[m - 1] != cutsP[k]) )
        {
            cutsP[m++] = cutsP[k];
        }
    }
    nCuts = m;

    // The items, notes and rests, then cut at the bar lines and tempo changes.
    cutP = NULL;
    nCut = 0;
    capCut = 0;
    t = 0;
    for(k = 0; k <= partP->count; k++)
    {
        b1 = ((k < partP->count) ? partP->notesP[k].start : barsP->startsP[barsP->nBars]);
        if( b1 > t )
        {
            it.s = t;
            it.ln = (b1 - t);
            it.key = -1;
            it.art = 0;
            addCutItem(&cutP, &nCut, &capCut, &it, cutsP, nCuts);
        }
        if( k < partP->count )
        {
            it.s = partP->notesP[k].start;
            it.ln = partP->notesP[k].length;
            it.key = partP->notesP[k].key;
            it.art = partP->notesP[k].art;
            addCutItem(&cutP, &nCut, &capCut, &it, cutsP, nCuts);
            t = (it.s + it.ln);
        }
    }

    mode = 'e';
    k = 0;
    piecesP = NULL;
    capPieces = 0;
    for(b = 0; b < barsP->nBars; b++)
    {
        b1 = barsP->startsP[b + 1];
        if( (b == 0) || (barsP->unitsP[b] != barsP->unitsP[b - 1]) )
        {
            linesAdd(&lines, format("units %ld", (barsP->unitsP[b] / 6)));
        }
        if( (b == 0) && (pn == 0) && ((tw = tempoAt(tempoP, nTempo, 0)) >= 0) )
        {
            linesAdd(&lines, format("tempo %ld", tw));
        }
        memset(&words, 0, sizeof(words));
        textAdd(&words, "%d", (b + 1));
        for(i = 0; i < NPOS; i++)
        {
            acc[i] = -1;
            white[i] = 0;
        }
        nPieces = 0;
        while( (k < nCut) && (cutP[k].s < b1) )
        {
            if( (pn == 0) && (cutP[k].s > 0) && ((tw = tempoAt(tempoP, nTempo, cutP[k].s)) >= 0) )
            {
                if( cutP[k].key < 0 )
                {
                    // hc1d's legato before a rest marks the word before the rest.
                    refuse("part 1, bar %d: a tempo change on a rest", (b + 1));
                }
                memset(&pc, 0, sizeof(pc));
                pc.isTempo = 1;
                pc.tempo = tw;
                addPiece(&piecesP, &nPieces, &capPieces, &pc);
            }
            np = piecesOf(cutP[k].ln, trips, t64s, MAX_PIECES);
            if( np < 0 )
            {
                refuse("part %d, bar %d: a length of %ld units", (pn + 1), (b + 1), cutP[k].ln);
            }
            for(j = 0; j < np; j++)
            {
                memset(&pc, 0, sizeof(pc));
                pc.key = cutP[k].key;
                pc.art = ((((j + 1) == np) || (cutP[k].key < 0)) ? cutP[k].art : 'l');
                pc.trip = trips[j];
                pc.t64 = t64s[j];
                addPiece(&piecesP, &nPieces, &capPieces, &pc);
            }
            k++;
        }

        // The bar's mode: its notes' commonest articulation (the first in
        // alphabetical order on a tie), written as a word of its own where it
        // changes, so those notes need no letter.
        memset(artCount, 0, sizeof(artCount));
        for(j = 0; j < nPieces; j++)
        {
            if( !piecesP[j].isTempo && (piecesP[j].key >= 0) )
            {
                artCount[(unsigned char)piecesP[j].art]++;
            }
        }
        best = 0;
        for(orderP = "ehlqs"; *orderP != '\0'; orderP++)
        {
            if( (artCount[(unsigned char)*orderP] > 0)
                && ((best == 0) || (artCount[(unsigned char)*orderP] > artCount[(unsigned char)best])) )
            {
                best = *orderP;
            }
        }
        if( (best != 0) && (artCount[(unsigned char)best] > (artCount[(unsigned char)mode] + 1)) )
        {
            mode = best;
            textAdd(&words, " %c", mode);
        }

        // A black key is spelled away from a letter the bar uses natural.
        for(j = 0; j < nPieces; j++)
        {
            if( !piecesP[j].isTempo && (piecesP[j].key >= 0) && (spellAlter[piecesP[j].key % 12] == 0) )
            {
                white[indexOf(piecesP[j].key, NULL, &alter)] = 1;
            }
        }
        for(j = 0; j < nPieces; j++)
        {
            pc = piecesP[j];
            if( pc.isTempo )
            {
                textAdd(&words, " tempo %ld", pc.tempo);
                continue;
            }
            p = dots;
            if( pc.trip )
            {
                *p++ = 'c';
            }
            for(nb = 0; nb < timesDots[pc.t64]; nb++)
            {
                *p++ = '.';
            }
            *p = '\0';
            if( pc.key < 0 )
            {
                textAdd(&words, " r%s%d", dots, timesN[pc.t64]);
                continue;
            }
            i = indexOf(pc.key, white, &alter);
            want = (pc.key - KEY_SEMITONE);
            // hc1d holds an accidental to the bar's end, so a natural is written
            // where an earlier one would otherwise hold.
            p = sign;
            if( ((acc[i] >= 0) ? acc[i] : natural(i)) == want )
            {
            }
            else if( want == natural(i) )
            {
                *p++ = ')';
            }
            else
            {
                for(nb = 0; nb < abs(want - natural(i)); nb++)
                {
                    *p++ = ((want > natural(i)) ? '(' : '-');
                }
            }
            *p = '\0';
            acc[i] = want;
            pos = (i - clefOff);
            nb = 0;
            while( pos < 0 )
            {
                pos = (pos + STAFF);
                nb++;
            }
            nxtP = NULL;
            for(m = (j + 1); m < nPieces; m++)
            {
                if( !piecesP[m].isTempo )
                {
                    nxtP = &piecesP[m];
                    break;
                }
            }
            // A letter where the mode is not the note's articulation, and before
            // a rest (hc1d makes a note with no letter legato when a rest follows it).
            letter[0] = '\0';
            letter[1] = '\0';
            if( (pc.art != mode) || ((nxtP != NULL) && (nxtP->key < 0) && (pc.art != 'l')) )
            {
                letter[0] = pc.art;
            }
            p = letters;
            for(m = 0; m < nb; m++)
            {
                *p++ = 'b';
            }
            *p = '\0';
            strcat(letters, sign);
            strcat(letters, letter);
            strcat(letters, dots);
            textAdd(&words, " %d%s%d", pos, ((letters[0] != '\0') ? letters : "t"), timesN[pc.t64]);
        }
        textAdd(&words, "/");
        linesAdd(&lines, words.textP);
    }
    linesAdd(&lines, format("end"));
    padFills(&lines, (pn + 1));
    for(k = 0; k < lines.count; k++)
    {
        textAdd(outP, "%s\n", lines.linesP[k]);
    }
    free(cutsP);
    free(cutP);
    free(piecesP);
}

// The measure buffer.

// The longest fill of hc1d's measure buffer in the part whose text
// (after its title) is bodyP, len characters, the slash's threshold taken
// at 255, 256 and 257: below it a slash does not end a fill, from it on the
// first does. The longest in *nP, the bar it ends in in *barP.
static void longestFill(const char *bodyP, size_t len, long *nP, long *barP)
{
    long bestN;
    long bestBar;
    long n;
    long bar;
    int soft;
    size_t i;

    bestN = 0;
    bestBar = 0;
    for(soft = (FILL_SOFT - 1); soft <= (FILL_SOFT + 1); soft++)
    {
        n = 0;
        bar = 1;
        for(i = 0; i < len; i++)
        {
            n++;
            if( bodyP[i] == '/' )
            {
                bar++;
                if( n > soft )
                {
                    if( (n > bestN) || ((n == bestN) && ((bar - 1) > bestBar)) )
                    {
                        bestN = n;
                        bestBar = (bar - 1);
                    }
                    n = 0;
                }
            }
        }
        if( (n > bestN) || ((n == bestN) && (bar > bestBar)) )
        {
            bestN = n;
            bestBar = bar;
        }
    }
    *nP = bestN;
    *barP = bestBar;
}

// The text read back, as the compiler reads the words the writer uses,
// written from the language alone, apart from the writer.

typedef struct
{
    Part *partsP;                   // each part's notes, joined
    TempoWord **temposP;            // each part's tempo words
    int *nTemposP;
    int nParts;
} ReadBack;

// Add a note to a list.
static void addListed(Part *pP, int *capP, long start, long length, int key, char art)
{
    if( pP->count == *capP )
    {
        *capP = ((*capP * 2) + 64);
        pP->notesP = growOrFail(pP->notesP, (size_t)*capP, sizeof(Listed));
    }
    pP->notesP[pP->count].start = start;
    pP->notesP[pP->count].length = length;
    pP->notesP[pP->count].key = key;
    pP->notesP[pP->count].art = art;
    pP->count++;
}

// Read one part's body, len characters at bodyP: its notes (rests
// included, key -1) in *outP and its tempo words in *tempoPP, *nTempoP.
// Returns NULL, or what is wrong, from the heap.
static char *readBody(const char *bodyP, size_t len, int pn, Part *outP, TempoWord **tempoPP, int *nTempoP)
{
    size_t i;
    size_t a;
    size_t tl;
    char tok[512];
    int clef;
    long units;
    char mode;
    long count;
    long barU;
    long t;
    int acc[NPOS];
    int op;                         // 0 none, 1 units, 2 tempo
    int prevNote;                   // -1 none, else the index of the word before
    int prevIsNote;
    int prevHasArt;
    int cap;
    int capTempo;
    int k;
    size_t g1;
    size_t g3;
    const char *midP;
    size_t midLen;
    long n;
    long base;
    long half;
    long time;
    int trip;
    char art;
    long ln;
    long pos;
    int semi;
    int nA;
    int nB;
    int nUp;
    int nDown;
    int nat;
    int hasNat;
    char where[600];

    clef = 22;
    units = -1;
    mode = 'e';
    count = 1;
    barU = 0;
    t = 0;
    op = 0;
    prevNote = -1;
    prevIsNote = 0;
    prevHasArt = 0;
    for(k = 0; k < NPOS; k++)
    {
        acc[k] = -1;
    }
    outP->notesP = NULL;
    outP->count = 0;
    cap = 0;
    *tempoPP = NULL;
    *nTempoP = 0;
    capTempo = 0;
    i = 0;
    while( 1 )
    {
        // The next token: a slash, or a run of characters that are neither space nor slash.
        while( (i < len) && isSpace((unsigned char)bodyP[i]) )
        {
            i++;
        }
        if( i >= len )
        {
            break;
        }
        a = i;
        if( bodyP[i] == '/' )
        {
            i++;
        }
        else
        {
            while( (i < len) && !isSpace((unsigned char)bodyP[i]) && (bodyP[i] != '/') )
            {
                i++;
            }
        }
        tl = (i - a);
        if( tl >= sizeof(tok) )
        {
            tl = (sizeof(tok) - 1);
        }
        memcpy(tok, (bodyP + a), tl);
        tok[tl] = '\0';
        snprintf(where, sizeof(where), "part %d, bar %ld (%s)", pn, count, tok);
        if( op != 0 )
        {
            if( !allDigits(tok, tl) )
            {
                return format("%s: %s with no number", where, ((op == 1) ? "units" : "tempo"));
            }
            if( op == 1 )
            {
                units = digitsValue(tok, tl);
            }
            else
            {
                if( *nTempoP == capTempo )
                {
                    capTempo = ((capTempo * 2) + 16);
                    *tempoPP = growOrFail(*tempoPP, (size_t)capTempo, sizeof(TempoWord));
                }
                (*tempoPP)[*nTempoP].unit = t;
                (*tempoPP)[*nTempoP].n = digitsValue(tok, tl);
                (*nTempoP)++;
            }
            op = 0;
            continue;
        }
        if( strcmp(tok, "/") == 0 )
        {
            if( (units < 0) || (barU != (6 * units)) )
            {
                if( units < 0 )
                {
                    return format("part %d, bar %ld: %ld units, not None", pn, count, barU);
                }
                return format("part %d, bar %ld: %ld units, not %ld", pn, count, barU, (6 * units));
            }
            count++;
            barU = 0;
            for(k = 0; k < NPOS; k++)
            {
                acc[k] = -1;
            }
            prevNote = -1;
            continue;
        }
        for(k = 0; k < 4; k++)
        {
            if( strcmp(tok, clefNames[k]) == 0 )
            {
                break;
            }
        }
        if( k < 4 )
        {
            clef = clefOffsets[k];
            continue;
        }
        if( (strcmp(tok, "units") == 0) || (strcmp(tok, "tempo") == 0) )
        {
            op = ((tok[0] == 'u') ? 1 : 2);
            continue;
        }
        if( (tl == 1) && (strchr("slehq", tok[0]) != NULL) )
        {
            mode = tok[0];
            continue;
        }
        if( allDigits(tok, tl) )
        {
            if( digitsValue(tok, tl) != count )
            {
                return format("%s: bar number %s, the count %ld", where, tok, count);
            }
            continue;
        }

        // A note or rest word: a position or r, letters, a note value.
        g1 = 0;
        if( (tok[0] >= '0') && (tok[0] <= '9') )
        {
            while( (g1 < tl) && (tok[g1] >= '0') && (tok[g1] <= '9') )
            {
                g1++;
            }
        }
        else if( tok[0] == 'r' )
        {
            g1 = 1;
        }
        else
        {
            return format("%s: not a word this reader knows", where);
        }
        g3 = g1;
        while( (g3 < tl) && ((tok[g3] < '0') || (tok[g3] > '9')) )
        {
            if( ((tok[g3] < 'a') || (tok[g3] > 'z')) && (strchr("().-", tok[g3]) == NULL) )
            {
                return format("%s: not a word this reader knows", where);
            }
            g3++;
        }
        if( (g3 == tl) || !allDigits((tok + g3), (tl - g3)) )
        {
            return format("%s: not a word this reader knows", where);
        }
        midP = (tok + g1);
        midLen = (g3 - g1);
        nA = 0;
        nB = 0;
        nUp = 0;
        nDown = 0;
        hasNat = 0;
        trip = 0;
        art = 0;
        half = 0;
        for(k = 0; k < (int)midLen; k++)
        {
            if( midP[k] == 't' )
            {
                continue;
            }
            if( strchr("abc().-slehq", midP[k]) == NULL )
            {
                return format("%s: a letter this reader does not take", where);
            }
        }
        n = digitsValue((tok + g3), (tl - g3));
        if( (n != 1) && (n != 2) && (n != 4) && (n != 8) && (n != 16) && (n != 32) && (n != 64) )
        {
            return format("%s: duration %ld", where, n);
        }
        base = (64 / n);
        half = base;
        time = base;
        for(k = 0; k < (int)midLen; k++)
        {
            switch( midP[k] )
            {
            case '.':
                half = (half / 2);
                if( half < 1 )
                {
                    return format("%s: a dot under a 64th", where);
                }
                time = (time + half);
                break;
            case 'a':
                nA++;
                break;
            case 'b':
                nB++;
                break;
            case 'c':
                trip = 1;
                break;
            case '(':
                nUp++;
                break;
            case '-':
                nDown++;
                break;
            case ')':
                hasNat = 1;
                break;
            case 't':
                break;
            default:
                art = midP[k];          // the last articulation letter given
                break;
            }
        }
        ln = (time * (trip ? 2 : 3));
        if( tok[0] == 'r' )
        {
            // A note with no articulation letter before a rest is legato.
            if( (prevNote >= 0) && prevIsNote && !prevHasArt )
            {
                outP->notesP[prevNote].art = 'l';
            }
            addListed(outP, &cap, t, ln, -1, 0);
            prevNote = (outP->count - 1);
            prevIsNote = 0;
            prevHasArt = 0;
        }
        else
        {
            pos = (digitsValue(tok, g1) + (STAFF * (nA - nB)) + clef);
            if( (pos < 0) || (pos >= NPOS) )
            {
                return format("%s: position %ld", where, pos);
            }
            nat = natural((int)pos);
            if( hasNat )
            {
                acc[pos] = nat;
            }
            else if( (nUp > 0) || (nDown > 0) )
            {
                acc[pos] = (nat + nUp - nDown);
            }
            semi = ((acc[pos] >= 0) ? acc[pos] : nat);
            if( (semi < 2) || (semi > 076) )
            {
                return format("%s: semitone %d", where, semi);
            }
            addListed(outP, &cap, t, ln, (semi + KEY_SEMITONE), ((art != 0) ? art : mode));
            prevNote = (outP->count - 1);
            prevIsNote = 1;
            prevHasArt = (art != 0);
        }
        t = (t + ln);
        barU = (barU + ln);
    }
    if( (op != 0) || (barU != 0) )
    {
        return format("part %d ends inside a bar or a pseudo-op", pn);
    }
    // The rests dropped.
    for(k = 0, i = 0; k < outP->count; k++)
    {
        if( outP->notesP[k].key >= 0 )
        {
            outP->notesP[i++] = outP->notesP[k];
        }
    }
    outP->count = (int)i;
    return NULL;
}

// Read the text back: per part its notes, joined, and its tempo words.
// Returns NULL, or what is wrong, from the heap.
static char *readBack(const char *textP, ReadBack *rbP)
{
    const char *restP;
    const char *slashP;
    size_t len;
    size_t p;
    size_t start;
    size_t end;
    int found;
    char *errP;
    Part raw;
    int k;

    memset(rbP, 0, sizeof(*rbP));
    restP = textP;
    while( 1 )
    {
        // Stop where only white space is left.
        len = strlen(restP);
        for(p = 0; (p < len) && isSpace((unsigned char)restP[p]); p++)
        {
        }
        if( p == len )
        {
            break;
        }
        if( !(slashP = strchr(restP, '/')) )
        {
            return format("a part with no title");
        }
        restP = (slashP + 1);
        len = strlen(restP);
        k = (rbP->nParts + 1);
        rbP->partsP = growOrFail(rbP->partsP, (size_t)k, sizeof(Part));
        rbP->temposP = growOrFail(rbP->temposP, (size_t)k, sizeof(TempoWord *));
        rbP->nTemposP = growOrFail(rbP->nTemposP, (size_t)k, sizeof(int));
        rbP->nParts = k;
        // The first `end' with white space or the text's start before it, and
        // white space or the text's end after it.
        found = 0;
        start = 0;
        end = 0;
        for(p = 0; (p + 3) <= len; p++)
        {
            if( (strncmp((restP + p), "end", 3) == 0) && ((p == 0) || isSpace((unsigned char)restP[p - 1]))
                && (((p + 3) == len) || isSpace((unsigned char)restP[p + 3])) )
            {
                found = 1;
                start = ((p == 0) ? 0 : (p - 1));
                end = (((p + 3) == len) ? (p + 3) : (p + 4));
                break;
            }
        }
        if( !found )
        {
            return format("part %d has no end", k);
        }
        if( (errP = readBody(restP, start, k, &raw, &rbP->temposP[k - 1], &rbP->nTemposP[k - 1])) != NULL )
        {
            return errP;
        }
        rbP->partsP[k - 1] = joined(&raw);
        restP = (restP + end);
    }
    return NULL;
}

// A note as Python writes the tuple, onto tP; NULL as None.
static void noteRepr(Text *tP, const Listed *lP)
{
    if( lP == NULL )
    {
        textAdd(tP, "None");
        return;
    }
    textAdd(tP, "(%ld, %ld, %d, '%c')", lP->start, lP->length, lP->key, lP->art);
}

// Tempo words as Python writes the list, onto tP.
static void tempoRepr(Text *tP, const TempoWord *wP, int count)
{
    int k;

    textAdd(tP, "[");
    for(k = 0; k < count; k++)
    {
        textAdd(tP, "%s(%ld, %ld)", ((k > 0) ? ", " : ""), wP[k].unit, wP[k].n);
    }
    textAdd(tP, "]");
}

// Whether the text reads back as the parts' notes, joined, and the tempo
// words, all in part 1. Returns NULL, or the first difference or the
// reader's complaint, from the heap.
static char *roundTrip(const char *textP, const Part *partsP, int nParts, const TempoWord *tempoP, int nTempo)
{
    ReadBack rb;
    char *errP;
    Text diff;
    Part want;
    int pn;
    int k;
    int n;
    const Listed *uP;
    const Listed *vP;
    int same;

    if( (errP = readBack(textP, &rb)) != NULL )
    {
        return format("read back: %s", errP);
    }
    if( rb.nParts != nParts )
    {
        return format("%d parts against %d", nParts, rb.nParts);
    }
    for(pn = 0; pn < nParts; pn++)
    {
        want = joined(&partsP[pn]);
        n = ((want.count > rb.partsP[pn].count) ? want.count : rb.partsP[pn].count);
        for(k = 0; k < n; k++)
        {
            uP = ((k < want.count) ? &want.notesP[k] : NULL);
            vP = ((k < rb.partsP[pn].count) ? &rb.partsP[pn].notesP[k] : NULL);
            same = ((uP != NULL) && (vP != NULL) && (uP->start == vP->start) && (uP->length == vP->length)
                    && (uP->key == vP->key) && (uP->art == vP->art));
            if( !same )
            {
                memset(&diff, 0, sizeof(diff));
                textAdd(&diff, "part %d, note %d: ", (pn + 1), (k + 1));
                noteRepr(&diff, uP);
                textAdd(&diff, " against ");
                noteRepr(&diff, vP);
                return diff.textP;
            }
        }
    }
    same = 1;
    for(pn = 0; pn < nParts; pn++)
    {
        if( rb.nTemposP[pn] != ((pn == 0) ? nTempo : 0) )
        {
            same = 0;
        }
        for(k = 0; same && (pn == 0) && (k < nTempo); k++)
        {
            same = ((rb.temposP[0][k].unit == tempoP[k].unit) && (rb.temposP[0][k].n == tempoP[k].n));
        }
    }
    if( !same )
    {
        memset(&diff, 0, sizeof(diff));
        textAdd(&diff, "tempos [");
        for(pn = 0; pn < nParts; pn++)
        {
            textAdd(&diff, "%s", ((pn > 0) ? ", " : ""));
            tempoRepr(&diff, rb.temposP[pn], rb.nTemposP[pn]);
        }
        textAdd(&diff, "] against [");
        for(pn = 0; pn < nParts; pn++)
        {
            textAdd(&diff, "%s", ((pn > 0) ? ", " : ""));
            tempoRepr(&diff, tempoP, ((pn == 0) ? nTempo : 0));
        }
        textAdd(&diff, "]");
        return diff.textP;
    }
    return NULL;
}

// The default title: the file's name less its directories and extension.
static char *defaultTitle(const char *pathP)
{
    const char *baseP;
    const char *dotP;
    const char *p;
    char *outP;
    size_t n;

    baseP = strrchr(pathP, '/');
    baseP = ((baseP != NULL) ? (baseP + 1) : pathP);
    dotP = strrchr(baseP, '.');
    n = strlen(baseP);
    // A name of dots before its last one, such as ".mid", has no extension.
    if( dotP != NULL )
    {
        for(p = baseP; (p < dotP) && (*p == '.'); p++)
        {
        }
        if( p < dotP )
        {
            n = (size_t)(dotP - baseP);
        }
    }
    outP = growOrFail(NULL, (n + 1), 1);
    memcpy(outP, baseP, n);
    outP[n] = '\0';
    return outP;
}

// An option's value, after its = or else the next argument, *iP moved past it.
static const char *optionValue(int argc, char **argv, int *iP, const char *nameP)
{
    size_t n;

    n = strlen(nameP);
    if( (strncmp(argv[*iP], nameP, n) == 0) && (argv[*iP][n] == '=') )
    {
        return (argv[*iP] + n + 1);
    }
    (*iP)++;
    if( *iP >= argc )
    {
        usage("%s needs a value", nameP);
    }
    return argv[*iP];
}

// Read the options and the MIDI file, write the text, and print the
// summary. Returns 0 with the text written; 1 refused, or the text not
// reading back; 2 for a usage error.
int main(int argc, char **argv)
{
    const char *files[2];
    int nFiles;
    int played;
    const char *titleP;
    int hasTempo;
    long tempo;
    const char *vP;
    const char *p;
    int i;
    int k;
    Song song;
    Grid grid;
    Bars bars;
    Part *partsP;
    TempoWord *wordsP;
    int nWords;
    long end;
    long u;
    long movedMax;
    long movedOver;
    long nMoved;
    Text text;
    Text line;
    char *errP;
    FILE *fP;
    int counts[NART];
    int a;
    const char *bodyP;
    const char *partEndP;
    const char *nextP;
    long fillN;
    long fillBar;
    int pn;

    makeTimes();
    nFiles = 0;
    played = 0;
    titleP = NULL;
    hasTempo = 0;
    tempo = 0;
    for(i = 1; i < argc; i++)
    {
        if( strcmp(argv[i], "--played") == 0 )
        {
            played = 1;
        }
        else if( (strcmp(argv[i], "--title") == 0) || (strncmp(argv[i], "--title=", 8) == 0) )
        {
            titleP = optionValue(argc, argv, &i, "--title");
        }
        else if( (strcmp(argv[i], "--tempo") == 0) || (strncmp(argv[i], "--tempo=", 8) == 0) )
        {
            vP = optionValue(argc, argv, &i, "--tempo");
            p = (((vP[0] == '-') || (vP[0] == '+')) ? (vP + 1) : vP);
            if( !allDigits(p, strlen(p)) || (strlen(p) > 12) )
            {
                usage("--tempo takes a whole number, not %s", vP);
            }
            tempo = strtol(vP, NULL, 10);
            hasTempo = 1;
        }
        else if( (argv[i][0] == '-') && (argv[i][1] != '\0') )
        {
            usage("%s is not an option", argv[i]);
        }
        else if( nFiles < 2 )
        {
            files[nFiles++] = argv[i];
        }
        else
        {
            usage("one MIDI file and one output file");
        }
    }
    if( nFiles != 2 )
    {
        usage("one MIDI file and one output file");
    }

    readSong(files[0], &song);
    makeGrid(&grid, &song, played);
    end = 0;
    for(k = 0; k < song.nGroups; k++)
    {
        for(i = 0; i < song.groupsP[k].count; i++)
        {
            end = ((song.groupsP[k].notesP[i].end > end) ? song.groupsP[k].notesP[i].end : end);
        }
    }
    makeBars(&bars, &song, (unitOf(&grid, end, &u) ? u : (((end * 48) / grid.div) + 1)));
    partsP = noteList(&song, &grid, &bars, &movedMax, &movedOver, &nMoved);
    wordsP = tempoWords(&song, &grid, &bars, &nWords);
    if( hasTempo )
    {
        scaleTempos(wordsP, nWords, tempo);
    }
    if( titleP == NULL )
    {
        titleP = defaultTitle(files[0]);
    }

    // The parts, a blank line between them.
    memset(&text, 0, sizeof(text));
    textAdd(&text, "");
    for(pn = 0; pn < song.nGroups; pn++)
    {
        if( pn > 0 )
        {
            textAdd(&text, "\n");
        }
        writePart(&text, &partsP[pn], pn, song.nGroups, wordsP, nWords, &bars, titleP);
    }
    if( (errP = roundTrip(text.textP, partsP, song.nGroups, wordsP, nWords)) != NULL )
    {
        printf("the text does not read back: %s\n", errP);
        return 1;
    }
    if( !(fP = fopen(files[1], "wb")) || (fwrite(text.textP, 1, text.len, fP) != text.len) || (fclose(fP) != 0) )
    {
        printf("midi2hc: %s cannot be written\n", files[1]);
        return 1;
    }

    // The summary: the parts, each part's notes by articulation, its
    // longest fill of the measure buffer, and the snap.
    memset(&line, 0, sizeof(line));
    tempoRepr(&line, wordsP, nWords);
    printf("%s: %d parts, %d bars, tempo %s\n", files[1], song.nGroups, bars.nBars, line.textP);
    for(pn = 0; pn < song.nGroups; pn++)
    {
        memset(counts, 0, sizeof(counts));
        for(i = 0; i < partsP[pn].count; i++)
        {
            for(a = 0; a < NART; a++)
            {
                counts[a] = (counts[a] + (partsP[pn].notesP[i].art == artLetters[a]));
            }
        }
        memset(&line, 0, sizeof(line));
        textAdd(&line, "");
        for(a = 0; a < NART; a++)
        {
            if( counts[a] > 0 )
            {
                textAdd(&line, "%s%c %d", ((line.len > 0) ? ", " : ""), artLetters[a], counts[a]);
            }
        }
        printf("  part %d: %d notes, %s\n", (pn + 1), partsP[pn].count, line.textP);
    }
    // Each part's text runs to "\nend\n"; its body starts after its title's slash.
    p = text.textP;
    pn = 0;
    while( *p != '\0' )
    {
        partEndP = strstr(p, "\nend\n");
        nextP = ((partEndP != NULL) ? (partEndP + 5) : (p + strlen(p)));
        partEndP = ((partEndP != NULL) ? partEndP : nextP);
        pn++;
        bodyP = memchr(p, '/', (size_t)(partEndP - p));
        if( bodyP != NULL )
        {
            longestFill((bodyP + 1), (size_t)(partEndP - (bodyP + 1)), &fillN, &fillBar);
            printf("  part %d: the longest fill %ld characters, in bar %ld\n", pn, fillN, fillBar);
        }
        p = nextP;
    }
    if( played )
    {
        printf("  the snap: offset %ld ticks; onsets moved at most %ld.0 ticks, %ld of %ld more than 10\n",
               grid.off, movedMax, movedOver, nMoved);
    }
    return 0;
}
