// midi2dac makes one or more Standard MIDI Files into a paper tape image
// for the D/A converters' music player (../music/dacmusic.am1), in the
// Harmony Compiler's tape format with the words the player adds to it.
// Each file is a movement of up to four parts, with its notes, tempo
// words, bar lines and dynamics, every bar stored once and pointed at
// again where it recurs. An orchestra block naming each converter's part,
// stops, octave, volume and place may follow.
//
// readSong reads a file. makeGrid and unitOf put its ticks on the
// player's grid of 1/192 of a whole note, exactly, or for a file played
// in by a snap to 64ths. precut cuts a played legato's brief overlaps,
// shares gives the four parts to the file's groups, and reduceGroup and
// splitGroup make each group its parts. noteList gives each note its
// length and articulation from the silence before the next, tempoWords
// and dynamicsOf place the tempo words and dynamics, partWords writes each
// part's bars as words, storeBars stores each bar once, and fitNew and
// fitPlain check the tape against the player's core.
//
// The arithmetic is integer except for the articulations' silent shares,
// computed in doubles in the order written. Built with -ffp-contract=off
// (the Makefile), a tape is the same on every host.
//
// C99 and the C library only, one thread. Most memory is never freed,
// since the program exits after one tape. README.md describes its use.
//
// Usage: midi2dac [options] MIDI... -o TAPE.bin
//
// 22-Oct-2026 Claude: written.
// 23-Oct-2026 Claude: comments cleaned for release.

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The player's limits (dacmusic.am1).
#define NCONV 4                     // converters, so parts a movement
#define MAXP 30                     // parts on a tape
#define MAXSEG 16                   // tempo words in one bar
#define DQROOM 4                    // dynamics one converter takes in one bar
#define TAPE 020000                 // the tape's first word in core
#define TEND 036400                 // the first area's end
#define TAPE2 050000                // a larger tape runs on from here ...
#define VEND 0200000                // ... to here, the voices after it
#define TEND_PLAIN 037300           // the 1.0 player's area's end

// The tape's words.
#define WORD_MASK 0777777
#define BAR 0600000                 // a bar line; 600001-600006 the dynamics
#define TEMPO_MARK 0700000
#define TRIPLET 0100000
#define REST_PITCH 0200             // pitch field 1, a rest
#define KEY_SEMITONE 22             // a note word's pitch = MIDI key - 22 (C1, key 24, is 2)
#define ORCH_COUNT3 0700050         // the version 3 orchestra block's count word, 40 words
#define ORCH_VERSION3 3
#define CONFIGURED 0400000          // a room or onset word taking the emulator's setting
#define STOPS 4
#define LEADER 30                   // blank frames before and after the words

// Time.
#define WHOLE 192                   // units a whole note, 3 a 64th, 2 a triplet 64th
#define BRIEF 6                     // an overlap of a 32nd or less is a played legato
#define LEGATO_FROM 96              // --organ plays legato from a half note up
#define TEMPO_K 12174               // the tempo word N plays 12,174 / N quarter notes a minute
#define MAX_TEMPO 077777

// Keys and levels.
#define LOW_KEY 24
#define HIGH_KEY 85
#define HIGH_PLAIN 84               // pdp1m13's highest
#define PERCUSSION 9                // General MIDI's channel 10
#define LEVEL_F 4                   // f, where each movement starts
#define NLEVELS 6
#define PRINCIPAL 4                 // the voice of the nine a program with none takes

// Controllers kept, by index in a group's cc lists.
#define CC_VOLUME 0                 // CC 7
#define CC_PAN 1                    // CC 10
#define CC_EXPRESSION 2             // CC 11
#define CC_SUSTAIN 3                // CC 64
#define NCC 4

// The pieces of a bar, as partWords lays them out.
#define PIECE_TEMPO 0
#define PIECE_DYNAMIC 1
#define PIECE_NOTE 2
#define PIECE_REST 3

#define DYN_VELOCITY 0
#define DYN_CC7 1
#define DYN_CC11 2
#define DYN_NONE 3

typedef struct
{
    long start;
    long end;
    int key;
    int vel;
} MidiNote;

typedef struct
{
    long tick;
    int value;
} CcEvent;

typedef struct
{
    CcEvent *eventsP;
    int count;
    int cap;
} CcList;

typedef struct
{
    int track;
    int channel;                    // its channel, or 0 for a track's channels merged
    char *nameP;                    // NULL if the track has no name
    unsigned chans;                 // the channels of its events, a bit each
    MidiNote *notesP;
    int nNotes;
    int capNotes;
    CcList cc[NCC];
    int hasProgram;
    long programTick;
    int program;
    long bends;
} Group;

typedef struct
{
    long tick;
    long us;
} Tempo;

typedef struct
{
    long tick;
    int num;
    long long den;
} Meter;

typedef struct
{
    int fmt;
    long div;
    Group **groupsP;
    int nGroups;
    Tempo *temposP;
    int nTempos;
    Meter *metersP;
    int nMeters;
    long unpaired;
} Song;

typedef struct
{
    long tick;
    int vel;
    int ch;
    int key;
} Pending;

typedef struct
{
    long div;
    int played;
    long off;
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
    int vel;
    long s;                         // the onset's tick
    double e;                       // the end's tick, moved where the note is cut
} Note;

typedef struct
{
    Note **notesP;
    int count;
} NoteRun;

typedef struct
{
    long start;
    long length;
    int key;
    char art;
} Listed;

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
    int first;                      // the first piece of its note
} Item;

typedef struct
{
    int kind;
    long value;                     // a tempo word's N, or a dynamic's level
    int key;
    char art;
    int trip;
    int t64;
} Piece;

typedef struct
{
    long *wordsP;
    long *barOffP;                  // nBars + 1
    int nBars;
} PartBars;

typedef struct
{
    long *notesP;
    int nNotes;
    long *ptrP;
    int nPtr;
} Stored;

typedef struct
{
    int *idxP;                      // the groups merged into the entry's parts
    int nIdx;
    long n;                         // its parts
} PlanItem;

typedef struct
{
    PartBars bars;
    Group *groupP;
    long firstTick;
} MovePart;

typedef struct
{
    MovePart *partsP;
    int nParts;
    PartBars restBars;
} Movement;

typedef struct
{
    int part;                       // -1 for none
    int octave;                     // its index in 8, 16, 4, 32, 2 feet
    long volume;                    // 1/8 dB of attenuation
    long place;
    int stops[STOPS];
    int nStops;
    int given;
} OrchLine;

static const char *usageText =
    "usage: midi2dac [--played] [--tempo N] [--transpose N] [--by-channel] [--parts SPEC] "
    "[--organ] [--dynamics=velocity|cc7|cc11|none] [--orchestra FILE|--orchestra=gm] "
    "[--no-share] [--plain] [-q] MIDI... -o TAPE.bin";

// The articulations, by silent share, in the order a tie is settled.
static const char artLetters[5] = { 'l', 'e', 'q', 'h', 's' };
static const double artShares[5] = { 0.0, 0.125, 0.25, 0.5, 0.625 };
static const double nearShare = 0.0625;
static const int dynVelocities[NLEVELS] = { 33, 49, 64, 80, 96, 112 };
static const char *nineNames[9] =
{
    "flute", "reed", "plucked-bass", "marimba", "principal", "gedackt", "trumpet", "bourdon", "harpsichord"
};
// General MIDI's families, [lo, hi), to a voice of the nine.
static const int gmMap[8][3] =
{
    { 0, 8, 8 }, { 8, 16, 3 }, { 16, 24, 4 }, { 24, 32, 8 }, { 32, 40, 2 }, { 56, 64, 6 }, { 64, 72, 1 },
    { 72, 80, 0 }
};
static const char *placeNames[9] = { "left", "l3", "l2", "l1", "center", "r1", "r2", "r3", "right" };
static const long defaultPlaces[NCONV] = { 0, 0, 0400, 0400 };
static const char *feetNames[5] = { "8ft", "16ft", "4ft", "32ft", "2ft" };
// CC 7 at each 3 dB point of the sliders, 127 x 10^(-3k/40).
static const int volumeCc[16] = { 127, 107, 90, 76, 64, 54, 45, 38, 32, 27, 23, 19, 16, 13, 11, 10 };
static const char *roomNames[5] = { "config", "none", "hall", "busch", "voices" };
static const long roomWords[5] = { CONFIGURED, 0, 1, 2, 3 };
static const char *onsetNames[8] = { "config", "off", "2", "4", "6", "8", "12", "16" };
static const long onsetWords[8] = { CONFIGURED, 0, 020, 040, 060, 0100, 0140, 0200 };

// The options.
static int optPlayed;
static int optHasTempo;
static long optTempo;
static long optTranspose;
static int optByChannel;
static const char *optPartsP;
static int optOrgan;
static int optDynamics;
static const char *optOrchestraP;
static int optShare = 1;
static int optPlain;
static int optQuiet;
static const char *optOutP;

// The report, printed after a tape is made.
static char **reportP;
static int nReport;
static int capReport;

static int timesOk[128];            // a note word's written times in 64ths

// realloc that ends the program when memory runs out.
static void *growOrFail(void *ptrP, size_t count, size_t size)
{
    void *newP;

    newP = realloc(ptrP, ((count == 0) ? 1 : count) * size);
    if( newP == NULL )
    {
        printf("midi2dac: out of memory\n");
        exit(1);
    }
    return newP;
}

// A copy of a string, from the heap.
static char *copyString(const char *textP)
{
    char *outP;

    outP = growOrFail(NULL, strlen(textP) + 1, 1);
    strcpy(outP, textP);
    return outP;
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

// Add a line to the report.
static void report(const char *fmtP, ...)
{
    va_list args;

    if( nReport == capReport )
    {
        capReport = ((capReport == 0) ? 16 : (capReport * 2));
        reportP = growOrFail(reportP, (size_t)capReport, sizeof(char *));
    }
    va_start(args, fmtP);
    reportP[nReport++] = formatString(fmtP, args);
    va_end(args);
}

// Print a refusal and end the program, status 1, with nothing written.
static void refuse(const char *fmtP, ...)
{
    va_list args;
    char *textP;

    va_start(args, fmtP);
    textP = formatString(fmtP, args);
    va_end(args);
    printf("midi2dac: refused: %s\n", textP);
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
    printf("midi2dac: %s\n%s\n", textP, usageText);
    exit(2);
}

// The file name less its directories.
static const char *baseName(const char *pathP)
{
    const char *slashP;

    slashP = strrchr(pathP, '/');
    return ((slashP != NULL) ? (slashP + 1) : pathP);
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

// The absolute value of a long.
static long absLong(long x)
{
    return ((x < 0) ? -x : x);
}

// The ones' complement add of a block's words, as the player sums them.
static long checksum(const long *wordsP, int count)
{
    long s;
    int i;

    s = 0;
    for(i = 0; i < count; i++)
    {
        s = (s + wordsP[i]);
        if( s > WORD_MASK )
        {
            s = ((s + 1) & WORD_MASK);
        }
        if( s == WORD_MASK )
        {
            s = 0;
        }
    }
    return s;
}

// A stable merge sort. Equal elements keep their order, which qsort does
// not promise, so ties are settled the same on every host.
static void stableSort(void *baseP, size_t count, size_t size, int (*cmpP)(const void *, const void *))
{
    char *aP;
    char *tmpP;
    size_t width;
    size_t lo;
    size_t mid;
    size_t hi;
    size_t i;
    size_t j;
    size_t k;

    if( count < 2 )
    {
        return;
    }
    aP = baseP;
    tmpP = growOrFail(NULL, count, size);
    for(width = 1; width < count; width = (width * 2))
    {
        for(lo = 0; lo < count; lo = (lo + (2 * width)))
        {
            mid = (((lo + width) < count) ? (lo + width) : count);
            hi = (((lo + (2 * width)) < count) ? (lo + (2 * width)) : count);
            i = lo;
            j = mid;
            k = lo;
            while( (i < mid) && (j < hi) )
            {
                if( cmpP((aP + (j * size)), (aP + (i * size))) < 0 )
                {
                    memcpy((tmpP + (k * size)), (aP + (j * size)), size);
                    j++;
                }
                else
                {
                    memcpy((tmpP + (k * size)), (aP + (i * size)), size);
                    i++;
                }
                k++;
            }
            while( i < mid )
            {
                memcpy((tmpP + (k * size)), (aP + (i * size)), size);
                i++;
                k++;
            }
            while( j < hi )
            {
                memcpy((tmpP + (k * size)), (aP + (j * size)), size);
                j++;
                k++;
            }
        }
        memcpy(aP, tmpP, (count * size));
    }
    free(tmpP);
}

// Compare two numbers. Returns -1, 0 or 1 as a is less, equal or greater.
static int cmpLong(long long a, long long b)
{
    return ((a < b) ? -1 : ((a > b) ? 1 : 0));
}

// MIDI notes by start, end, key and velocity.
static int cmpMidiNote(const void *aP, const void *bP)
{
    const MidiNote *a = aP;
    const MidiNote *b = bP;
    int c;

    if( (c = cmpLong(a->start, b->start)) != 0 )
    {
        return c;
    }
    if( (c = cmpLong(a->end, b->end)) != 0 )
    {
        return c;
    }
    if( (c = cmpLong(a->key, b->key)) != 0 )
    {
        return c;
    }
    return cmpLong(a->vel, b->vel);
}

// Tempo changes by tick, then length.
static int cmpTempo(const void *aP, const void *bP)
{
    const Tempo *a = aP;
    const Tempo *b = bP;
    int c;

    if( (c = cmpLong(a->tick, b->tick)) != 0 )
    {
        return c;
    }
    return cmpLong(a->us, b->us);
}

// Meters by tick, numerator and denominator.
static int cmpMeter(const void *aP, const void *bP)
{
    const Meter *a = aP;
    const Meter *b = bP;
    int c;

    if( (c = cmpLong(a->tick, b->tick)) != 0 )
    {
        return c;
    }
    if( (c = cmpLong(a->num, b->num)) != 0 )
    {
        return c;
    }
    return cmpLong(a->den, b->den);
}

// Notes by onset, end and key.
static int cmpNote(const void *aP, const void *bP)
{
    const Note *a = *(Note * const *)aP;
    const Note *b = *(Note * const *)bP;
    int c;

    if( (c = cmpLong(a->us, b->us)) != 0 )
    {
        return c;
    }
    if( (c = cmpLong(a->ue, b->ue)) != 0 )
    {
        return c;
    }
    return cmpLong(a->key, b->key);
}

// Notes by onset and key.
static int cmpNoteStartKey(const void *aP, const void *bP)
{
    const Note *a = *(Note * const *)aP;
    const Note *b = *(Note * const *)bP;
    int c;

    if( (c = cmpLong(a->us, b->us)) != 0 )
    {
        return c;
    }
    return cmpLong(a->key, b->key);
}

// Mark in timesOk the times, in 64ths, one note word can write. Each is a
// base of 1 to 64 64ths and its dots, each dot half the last, none under
// a 64th, at most 127.
static void makeTimes(void)
{
    int b;
    int t;
    int h;

    for(b = 64; b >= 1; b = (b / 2))
    {
        t = b;
        h = b;
        while( 1 )
        {
            if( t <= 127 )
            {
                timesOk[t] = 1;
            }
            h = (h / 2);
            if( h < 1 )
            {
                break;
            }
            t = (t + h);
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

// A new, empty group, from the heap.
static Group *newGroup(int track, int channel)
{
    Group *gP;

    gP = growOrFail(NULL, 1, sizeof(Group));
    memset(gP, 0, sizeof(Group));
    gP->track = track;
    gP->channel = channel;
    return gP;
}

// Add a controller event to a list.
static void addCc(CcList *listP, long tick, int value)
{
    if( listP->count == listP->cap )
    {
        listP->cap = ((listP->cap == 0) ? 16 : (listP->cap * 2));
        listP->eventsP = growOrFail(listP->eventsP, (size_t)listP->cap, sizeof(CcEvent));
    }
    listP->eventsP[listP->count].tick = tick;
    listP->eventsP[listP->count].value = value;
    listP->count++;
}

// A track's name, printable ASCII only (anything else a space) and
// trimmed of spaces, so the report is the same bytes everywhere.
static char *trackName(const unsigned char *dP, long n)
{
    char *outP;
    long i;
    long a;
    long b;

    outP = growOrFail(NULL, (size_t)(n + 1), 1);
    for(i = 0; i < n; i++)
    {
        outP[i] = (char)(((dP[i] >= 32) && (dP[i] < 127)) ? dP[i] : ' ');
    }
    a = 0;
    b = n;
    while( (a < b) && (outP[a] == ' ') )
    {
        a++;
    }
    while( (b > a) && (outP[b - 1] == ' ') )
    {
        b--;
    }
    memmove(outP, (outP + a), (size_t)(b - a));
    outP[b - a] = '\0';
    return outP;
}

// Read one track. Its notes and controllers go into its groups (one a
// channel, or one for the track), its tempo and meter events into the song's.
static void readTrack(Song *songP, const unsigned char *tP, long len, int track, Group **trackGroupsP,
                      Pending **pendP, int *capPendP, int *capTemposP, int *capMetersP)
{
    long j;
    long tick;
    int run;
    int nPend;
    int haveName;
    char *nameP;
    int st;
    int typ;
    long n;
    int ch;
    int kind;
    int d0;
    int d1;
    int key;
    int k;
    int p;
    Group *gP;

    j = 0;
    tick = 0;
    run = -1;
    nPend = 0;
    haveName = 0;
    nameP = NULL;
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
            if( (j + 1) >= len )
            {
                refuse("the file ends inside an event");
            }
            typ = tP[j + 1];
            j = (j + 2);
            n = readVlq(tP, len, &j);
            if( (j + n) > len )
            {
                refuse("the file ends inside an event");
            }
            if( (typ == 0x51) && (n == 3) )
            {
                if( songP->nTempos == *capTemposP )
                {
                    *capTemposP = (*capTemposP * 2) + 16;
                    songP->temposP = growOrFail(songP->temposP, (size_t)*capTemposP, sizeof(Tempo));
                }
                songP->temposP[songP->nTempos].tick = tick;
                songP->temposP[songP->nTempos].us = bigEndian((tP + j), 3);
                songP->nTempos++;
            }
            else if( (typ == 0x58) && (n >= 2) )
            {
                if( songP->nMeters == *capMetersP )
                {
                    *capMetersP = (*capMetersP * 2) + 16;
                    songP->metersP = growOrFail(songP->metersP, (size_t)*capMetersP, sizeof(Meter));
                }
                songP->metersP[songP->nMeters].tick = tick;
                songP->metersP[songP->nMeters].num = tP[j];
                // The power is capped at 31. A larger one is no meter either, and the cap
                // makes its refusal print the same number on every host.
                songP->metersP[songP->nMeters].den = (1LL << ((tP[j + 1] < 31) ? tP[j + 1] : 31));
                songP->nMeters++;
            }
            else if( (typ == 3) && !haveName )
            {
                nameP = trackName((tP + j), n);
                haveName = 1;
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
        if( (j + n) > len )
        {
            refuse("the file ends inside an event");
        }
        d0 = tP[j];
        d1 = ((n == 2) ? tP[j + 1] : 0);
        j = (j + n);
        ch = (run & 0x0f);
        kind = (run & 0xf0);
        k = (((songP->fmt == 0) || optByChannel) ? ch : 0);
        if( trackGroupsP[k] == NULL )
        {
            trackGroupsP[k] = newGroup(track, k);
        }
        gP = trackGroupsP[k];
        gP->chans = (gP->chans | (1u << ch));
        if( (kind == 0x90) && (d1 != 0) )
        {
            if( nPend == *capPendP )
            {
                *capPendP = (*capPendP * 2) + 64;
                *pendP = growOrFail(*pendP, (size_t)*capPendP, sizeof(Pending));
            }
            (*pendP)[nPend].tick = tick;
            (*pendP)[nPend].vel = d1;
            (*pendP)[nPend].ch = ch;
            (*pendP)[nPend].key = d0;
            nPend++;
        }
        else if( (kind == 0x80) || (kind == 0x90) )
        {
            // The earliest note on of this key on this channel ends here.
            key = d0;
            for(p = 0; p < nPend; p++)
            {
                if( ((*pendP)[p].ch == ch) && ((*pendP)[p].key == key) )
                {
                    break;
                }
            }
            if( p < nPend )
            {
                if( gP->nNotes == gP->capNotes )
                {
                    gP->capNotes = ((gP->capNotes * 2) + 64);
                    gP->notesP = growOrFail(gP->notesP, (size_t)gP->capNotes, sizeof(MidiNote));
                }
                gP->notesP[gP->nNotes].start = (*pendP)[p].tick;
                gP->notesP[gP->nNotes].end = tick;
                gP->notesP[gP->nNotes].key = key;
                gP->notesP[gP->nNotes].vel = (*pendP)[p].vel;
                gP->nNotes++;
                memmove((*pendP + p), (*pendP + p + 1), ((size_t)(nPend - p - 1) * sizeof(Pending)));
                nPend--;
            }
            else
            {
                songP->unpaired++;
            }
        }
        else if( kind == 0xb0 )
        {
            if( d0 == 7 )
            {
                addCc(&gP->cc[CC_VOLUME], tick, d1);
            }
            else if( d0 == 10 )
            {
                addCc(&gP->cc[CC_PAN], tick, d1);
            }
            else if( d0 == 11 )
            {
                addCc(&gP->cc[CC_EXPRESSION], tick, d1);
            }
            else if( d0 == 64 )
            {
                addCc(&gP->cc[CC_SUSTAIN], tick, d1);
            }
        }
        else if( (kind == 0xc0) && !gP->hasProgram )
        {
            gP->hasProgram = 1;
            gP->programTick = tick;
            gP->program = d0;
        }
        else if( kind == 0xe0 )
        {
            gP->bends++;
        }
    }
    songP->unpaired = (songP->unpaired + nPend);
    for(k = 0; k < 16; k++)
    {
        if( trackGroupsP[k] != NULL )
        {
            trackGroupsP[k]->nameP = nameP;
        }
    }
}

// Read a whole file into the heap, with a NUL after it. Returns NULL if it
// cannot be read.
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
    while( (got = (long)fread((dP + *sizeP), 1, (size_t)(cap - *sizeP - 1), fP)) > 0 )
    {
        *sizeP = (*sizeP + got);
        if( (*sizeP + 1) == cap )
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
    dP[*sizeP] = '\0';
    return dP;
}

// Read a MIDI file into its groups with notes, in track and channel order,
// and its tempo map and meters, each sorted with repeats removed. A file
// it cannot take is refused.
static Song *readSong(const char *pathP)
{
    unsigned char *dP;
    long size;
    Song *songP;
    long i;
    long ln;
    int track;
    Group *trackGroups[16];
    Pending *pendP;
    int capPend;
    int capTempos;
    int capMeters;
    int capGroups;
    int k;
    int n;
    int m;

    if( !(dP = readWhole(pathP, &size)) )
    {
        refuse("%s: cannot be read", pathP);
    }
    if( (size < 14) || (memcmp(dP, "MThd", 4) != 0) )
    {
        refuse("not a Standard MIDI File");
    }
    songP = growOrFail(NULL, 1, sizeof(Song));
    memset(songP, 0, sizeof(Song));
    songP->fmt = (int)bigEndian((dP + 8), 2);
    songP->div = bigEndian((dP + 12), 2);
    if( songP->fmt == 2 )
    {
        refuse("format 2 is not taken");
    }
    if( ((songP->div & 0x8000) != 0) || (songP->div == 0) )
    {
        refuse("SMPTE time is not taken");
    }
    pendP = NULL;
    capPend = 0;
    capTempos = 0;
    capMeters = 0;
    capGroups = 0;
    i = (8 + bigEndian((dP + 4), 4));
    track = 0;
    while( (i + 8) <= size )
    {
        ln = bigEndian((dP + i + 4), 4);
        if( memcmp((dP + i), "MTrk", 4) != 0 )
        {
            i = (i + 8 + ln);
            continue;
        }
        if( (i + 8 + ln) > size )
        {
            ln = (size - (i + 8));
        }
        memset(trackGroups, 0, sizeof(trackGroups));
        readTrack(songP, (dP + i + 8), ln, track, trackGroups, &pendP, &capPend, &capTempos, &capMeters);
        for(k = 0; k < 16; k++)
        {
            if( (trackGroups[k] != NULL) && (trackGroups[k]->nNotes > 0) )
            {
                if( songP->nGroups == capGroups )
                {
                    capGroups = ((capGroups * 2) + 8);
                    songP->groupsP = growOrFail(songP->groupsP, (size_t)capGroups, sizeof(Group *));
                }
                stableSort(trackGroups[k]->notesP, (size_t)trackGroups[k]->nNotes, sizeof(MidiNote), cmpMidiNote);
                songP->groupsP[songP->nGroups++] = trackGroups[k];
            }
        }
        i = (i + 8 + ln);
        track++;
    }
    if( songP->nGroups == 0 )
    {
        refuse("no notes");
    }
    if( songP->nTempos == 0 )
    {
        songP->temposP = growOrFail(songP->temposP, 1, sizeof(Tempo));
        songP->temposP[0].tick = 0;
        songP->temposP[0].us = 500000;
        songP->nTempos = 1;
    }
    stableSort(songP->temposP, (size_t)songP->nTempos, sizeof(Tempo), cmpTempo);
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
        songP->metersP = growOrFail(songP->metersP, 1, sizeof(Meter));
        songP->metersP[0].tick = 0;
        songP->metersP[0].num = 4;
        songP->metersP[0].den = 4;
        songP->nMeters = 1;
    }
    stableSort(songP->metersP, (size_t)songP->nMeters, sizeof(Meter), cmpMeter);
    for(n = 0, m = 0; n < songP->nMeters; n++)
    {
        if( (m == 0) || (cmpMeter(&songP->metersP[m - 1], &songP->metersP[n]) != 0) )
        {
            songP->metersP[m++] = songP->metersP[n];
        }
    }
    songP->nMeters = m;
    return songP;
}

// Time.

// Set up the grid. For a file played in, its offset is the one in ticks
// that puts the onsets nearest a 64th grid, the first such on a tie.
static void makeGrid(Grid *gridP, long div, int played, const Song *songP)
{
    long o;
    long long cost;
    long long best;
    long long m;
    int g;
    int n;

    gridP->div = div;
    gridP->played = played;
    gridP->off = 0;
    if( !played )
    {
        return;
    }
    best = -1;
    for(o = 0; o < ((div + 15) / 16); o++)
    {
        cost = 0;
        for(g = 0; g < songP->nGroups; g++)
        {
            for(n = 0; n < songP->groupsP[g]->nNotes; n++)
            {
                m = floorMod((16LL * (songP->groupsP[g]->notesP[n].start - o)), div);
                cost = (cost + ((m < (div - m)) ? m : (div - m)));
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
// played in, more than a tick from the nearest unit).
static int unitOf(const Grid *gridP, long t, long *uP)
{
    long long u;

    if( gridP->played )
    {
        *uP = (long)(3 * roundHalfEven((16LL * (t - gridP->off)), gridP->div));
        return 1;
    }
    u = floorDiv(((96LL * t) + gridP->div), (2LL * gridP->div));
    *uP = (long)u;
    return (((((48LL * t) - (u * gridP->div)) < 0) ? -((48LL * t) - (u * gridP->div)) : ((48LL * t) - (u * gridP->div))) <= 48);
}

// The unit nearest tick t, on the grid or not, to find a note's bar.
static long nearUnit(const Grid *gridP, long t)
{
    long u;

    if( gridP->played )
    {
        unitOf(gridP, t, &u);
        return u;
    }
    return (long)floorDiv(((96LL * t) + gridP->div), (2LL * gridP->div));
}

// A unit's tick, u x div / 48, plus the offset.
static double tickOf(const Grid *gridP, long u)
{
    return (((double)((long long)u * gridP->div) / 48.0) + (double)gridP->off);
}

// The bars from the meters, to the end. A meter that is not a whole number
// of 32nds is refused.
static void makeBars(Bars *barsP, const Song *songP, long end)
{
    long at;
    long ln;
    int cap;
    int k;
    long mt;
    long mt0;
    int num;
    long long den;

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
        den = 1;
        for(k = (songP->nMeters - 1); k >= 0; k--)
        {
            mt = ((songP->metersP[k].tick * 48) / songP->div);
            if( (mt <= at) || (mt == mt0) )
            {
                num = songP->metersP[k].num;
                den = songP->metersP[k].den;
                break;
            }
        }
        ln = (long)((WHOLE * (long long)num) / den);
        if( ((ln * den) != (WHOLE * (long long)num)) || ((ln % 6) != 0) || (ln == 0) )
        {
            refuse("the meter %d/%lld is not a whole number of 32nds", num, den);
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
    while( ((k + 1) < barsP->nBars) && (barsP->startsP[k + 1] <= u) )
    {
        k++;
    }
    return (k + 1);
}

// Notes to parts.

// mostAtOnce's events in order.
static int cmpEvent(const void *aP, const void *bP)
{
    return cmpLong(*(const long *)aP, *(const long *)bP);
}

// The most notes sounding at once. At one unit, ends come before onsets.
static int mostAtOnce(Note **notesP, int count)
{
    long *evP;
    int i;
    int k;
    int best;

    evP = growOrFail(NULL, (size_t)(2 * count), sizeof(long));
    // An event is its unit times 2, plus 1 for an onset, so ends sort first.
    for(i = 0; i < count; i++)
    {
        evP[2 * i] = ((notesP[i]->us * 2) + 1);
        evP[(2 * i) + 1] = (notesP[i]->ue * 2);
    }
    qsort(evP, (size_t)(2 * count), sizeof(long), cmpEvent);
    k = 0;
    best = 0;
    for(i = 0; i < (2 * count); i++)
    {
        k = (k + (((evP[i] & 1) != 0) ? 1 : -1));
        if( k > best )
        {
            best = k;
        }
    }
    free(evP);
    return best;
}

// Cut a note held past the next onset in its group by a 32nd or less at
// that onset, as a played legato. Returns the count of notes cut.
static int precut(Note **notesP, int count, const Grid *gridP)
{
    long *onsetsP;
    int nOn;
    int i;
    int lo;
    int hi;
    int mid;
    int cut;
    long nxt;

    onsetsP = growOrFail(NULL, (size_t)count, sizeof(long));
    nOn = 0;
    for(i = 0; i < count; i++)
    {
        if( (nOn == 0) || (onsetsP[nOn - 1] != notesP[i]->us) )
        {
            onsetsP[nOn++] = notesP[i]->us;
        }
    }
    cut = 0;
    for(i = 0; i < count; i++)
    {
        lo = 0;
        hi = nOn;
        while( lo < hi )
        {
            mid = ((lo + hi) / 2);
            if( onsetsP[mid] > notesP[i]->us )
            {
                hi = mid;
            }
            else
            {
                lo = (mid + 1);
            }
        }
        if( lo < nOn )
        {
            nxt = onsetsP[lo];
            if( (nxt < notesP[i]->ue) && ((notesP[i]->ue - nxt) <= BRIEF) )
            {
                notesP[i]->ue = nxt;
                notesP[i]->e = tickOf(gridP, nxt);
                cut++;
            }
        }
    }
    free(onsetsP);
    return cut;
}

// Give each group one part, then the parts left one at a time to the group
// sounding most at once beyond its share, the first such on a tie.
static void shares(const int *atOnceP, int *shareP, int count)
{
    int i;
    int k;
    int total;

    total = 0;
    for(i = 0; i < count; i++)
    {
        shareP[i] = 1;
        total++;
    }
    while( total < NCONV )
    {
        k = 0;
        for(i = 1; i < count; i++)
        {
            if( (atOnceP[i] - shareP[i]) > (atOnceP[k] - shareP[k]) )
            {
                k = i;
            }
        }
        if( (atOnceP[k] - shareP[k]) <= 0 )
        {
            break;
        }
        shareP[k]++;
        total++;
    }
}

// A candidate at one onset in splitGroup, a held note (voice is its part)
// or a new one (voice -1).
typedef struct
{
    int voice;
    Note *noteP;
} Cand;

static Note *splitVoices[NCONV];
static NoteRun splitOut[NCONV];
static int splitCut;
static const Grid *splitGridP;

// End part v's note at unit at, cut if it would sound past it, and add it
// to the part.
static void splitClose(int v, long at)
{
    Note *xP;

    xP = splitVoices[v];
    if( at < xP->ue )
    {
        splitCut++;
        xP->ue = at;
        xP->e = tickOf(splitGridP, at);
    }
    splitOut[v].notesP = growOrFail(splitOut[v].notesP, (size_t)(splitOut[v].count + 1), sizeof(Note *));
    splitOut[v].notesP[splitOut[v].count++] = xP;
    splitVoices[v] = NULL;
}

// Try every placing of the new notes in the free parts, keeping the best
// in bestPermP. The best has the least disorder of register (part 0 the
// highest), then the smallest leaps, the first tried winning a tie.
static void splitPlace(int n, Note **newP, int nNew, const int *freeP, int nFree, const int *lastP,
                       int depth, int *permP, int *usedP, long *bestP, int *bestPermP, int *haveP)
{
    Note *trial[NCONV];
    long disorder;
    long leap;
    int a;
    int b;
    int i;
    int v;

    if( depth == nNew )
    {
        for(v = 0; v < n; v++)
        {
            trial[v] = splitVoices[v];
        }
        for(i = 0; i < nNew; i++)
        {
            trial[permP[i]] = newP[i];
        }
        disorder = 0;
        for(a = 0; a < n; a++)
        {
            for(b = (a + 1); b < n; b++)
            {
                if( (trial[a] != NULL) && (trial[b] != NULL) && (trial[a]->key < trial[b]->key) )
                {
                    disorder++;
                }
            }
        }
        leap = 0;
        for(i = 0; i < nNew; i++)
        {
            v = permP[i];
            leap = (leap + absLong((long)(newP[i]->key - ((lastP[v] >= 0) ? lastP[v] : (72 - (12 * v))))));
        }
        if( !*haveP || (disorder < bestP[0]) || ((disorder == bestP[0]) && (leap < bestP[1])) )
        {
            *haveP = 1;
            bestP[0] = disorder;
            bestP[1] = leap;
            memcpy(bestPermP, permP, ((size_t)nNew * sizeof(int)));
        }
        return;
    }
    for(i = 0; i < nFree; i++)
    {
        if( !usedP[i] )
        {
            usedP[i] = 1;
            permP[depth] = freeP[i];
            splitPlace(n, newP, nNew, freeP, nFree, lastP, (depth + 1), permP, usedP, bestP, bestPermP, haveP);
            usedP[i] = 0;
        }
    }
}

// Split notes of any number at once into n >= 2 monophonic parts, part 0
// the highest. At each onset a new note at a sounding pitch ends the old
// one. When more than n would sound, the highest and lowest are kept, then
// new notes before held ones, then a pitch class not yet among those kept,
// then the nearest the middle. A held note left out is cut, a new one
// dropped. The parts go in splitOut, the counts in the pointers.
static void splitGroup(Note **notesP, int count, int n, const Grid *gridP, int *cutP, int *droppedP, int *doubledP)
{
    Note **startP;
    Note **newP;
    Cand *candsP;
    int capCands;
    int nCand;
    int lastKey[NCONV];
    int freeV[NCONV];
    int perm[NCONV];
    int used[NCONV];
    int bestPerm[NCONV];
    long best[2];
    int have;
    int nFree;
    int nNew;
    int nStart;
    int i;
    int j;
    int v;
    int k;
    long t;
    int lo;
    int hi;
    int mid2;
    int pick;
    int kept;
    int keepN;
    Cand *keepP;
    int *inRestP;

    splitGridP = gridP;
    splitCut = 0;
    *droppedP = 0;
    *doubledP = 0;
    for(v = 0; v < n; v++)
    {
        splitVoices[v] = NULL;
        splitOut[v].notesP = NULL;
        splitOut[v].count = 0;
        lastKey[v] = -1;
    }
    startP = growOrFail(NULL, (size_t)count, sizeof(Note *));
    newP = growOrFail(NULL, (size_t)count, sizeof(Note *));
    capCands = (count + NCONV);
    candsP = growOrFail(NULL, (size_t)capCands, sizeof(Cand));
    keepP = growOrFail(NULL, (size_t)capCands, sizeof(Cand));
    inRestP = growOrFail(NULL, (size_t)capCands, sizeof(int));
    i = 0;
    while( i < count )
    {
        t = notesP[i]->us;
        // The notes starting here, one a key. Of two at one key, the longer is kept, the first on a tie.
        nStart = 0;
        while( (i < count) && (notesP[i]->us == t) )
        {
            for(j = 0; j < nStart; j++)
            {
                if( startP[j]->key == notesP[i]->key )
                {
                    break;
                }
            }
            if( j < nStart )
            {
                (*doubledP)++;
                if( notesP[i]->ue > startP[j]->ue )
                {
                    startP[j] = notesP[i];
                }
            }
            else
            {
                startP[nStart++] = notesP[i];
            }
            i++;
        }
        for(v = 0; v < n; v++)
        {
            if( (splitVoices[v] != NULL) && (splitVoices[v]->ue <= t) )
            {
                splitClose(v, splitVoices[v]->ue);
            }
        }
        // The new notes, highest first.
        nNew = nStart;
        memcpy(newP, startP, ((size_t)nNew * sizeof(Note *)));
        for(j = 1; j < nNew; j++)
        {
            Note *xP = newP[j];

            k = (j - 1);
            while( (k >= 0) && (newP[k]->key < xP->key) )
            {
                newP[k + 1] = newP[k];
                k--;
            }
            newP[k + 1] = xP;
        }
        for(v = 0; v < n; v++)
        {
            if( splitVoices[v] != NULL )
            {
                for(j = 0; j < nNew; j++)
                {
                    if( newP[j]->key == splitVoices[v]->key )
                    {
                        break;
                    }
                }
                if( j < nNew )
                {
                    splitClose(v, t);
                }
            }
        }
        nCand = 0;
        for(v = 0; v < n; v++)
        {
            if( splitVoices[v] != NULL )
            {
                candsP[nCand].voice = v;
                candsP[nCand].noteP = splitVoices[v];
                nCand++;
            }
        }
        for(j = 0; j < nNew; j++)
        {
            candsP[nCand].voice = -1;
            candsP[nCand].noteP = newP[j];
            nCand++;
        }
        if( nCand > n )
        {
            // The candidates by key, then the highest and the lowest kept.
            for(j = 1; j < nCand; j++)
            {
                Cand c = candsP[j];

                k = (j - 1);
                while( (k >= 0) && (candsP[k].noteP->key > c.noteP->key) )
                {
                    candsP[k + 1] = candsP[k];
                    k--;
                }
                candsP[k + 1] = c;
            }
            keepP[0] = candsP[nCand - 1];
            keepP[1] = candsP[0];
            keepN = 2;
            lo = candsP[0].noteP->key;
            hi = candsP[nCand - 1].noteP->key;
            mid2 = (lo + hi);
            for(j = 0; j < nCand; j++)
            {
                inRestP[j] = ((j > 0) && (j < (nCand - 1)));
            }
            while( keepN < n )
            {
                pick = -1;
                for(j = 0; j < nCand; j++)
                {
                    int better;
                    int pcJ;
                    int pcP;
                    int q;

                    if( !inRestP[j] )
                    {
                        continue;
                    }
                    if( pick < 0 )
                    {
                        pick = j;
                        continue;
                    }
                    pcJ = 0;
                    pcP = 0;
                    for(q = 0; q < keepN; q++)
                    {
                        if( (keepP[q].noteP->key % 12) == (candsP[j].noteP->key % 12) )
                        {
                            pcJ = 1;
                        }
                        if( (keepP[q].noteP->key % 12) == (candsP[pick].noteP->key % 12) )
                        {
                            pcP = 1;
                        }
                    }
                    // The better is the least of (held, its pitch class kept, its distance from the middle, -key).
                    if( (candsP[j].voice >= 0) != (candsP[pick].voice >= 0) )
                    {
                        better = (candsP[j].voice < 0);
                    }
                    else if( pcJ != pcP )
                    {
                        better = !pcJ;
                    }
                    else if( absLong((2 * candsP[j].noteP->key) - mid2) != absLong((2 * candsP[pick].noteP->key) - mid2) )
                    {
                        better = (absLong((2 * candsP[j].noteP->key) - mid2) < absLong((2 * candsP[pick].noteP->key) - mid2));
                    }
                    else
                    {
                        better = (candsP[j].noteP->key > candsP[pick].noteP->key);
                    }
                    if( better )
                    {
                        pick = j;
                    }
                }
                keepP[keepN++] = candsP[pick];
                inRestP[pick] = 0;
            }
            for(j = 0; j < nCand; j++)
            {
                if( inRestP[j] )
                {
                    if( candsP[j].voice >= 0 )
                    {
                        splitClose(candsP[j].voice, t);
                    }
                    else
                    {
                        (*droppedP)++;
                    }
                }
            }
            nNew = 0;
            for(kept = 0; kept < keepN; kept++)
            {
                if( keepP[kept].voice < 0 )
                {
                    newP[nNew++] = keepP[kept].noteP;
                }
            }
        }
        nFree = 0;
        for(v = 0; v < n; v++)
        {
            if( splitVoices[v] == NULL )
            {
                freeV[nFree++] = v;
                used[nFree - 1] = 0;
            }
        }
        have = 0;
        best[0] = 0;
        best[1] = 0;
        if( nNew <= nFree )
        {
            splitPlace(n, newP, nNew, freeV, nFree, lastKey, 0, perm, used, best, bestPerm, &have);
        }
        if( have )
        {
            for(j = 0; j < nNew; j++)
            {
                splitVoices[bestPerm[j]] = newP[j];
                lastKey[bestPerm[j]] = newP[j]->key;
            }
        }
    }
    for(v = 0; v < n; v++)
    {
        if( splitVoices[v] != NULL )
        {
            splitClose(v, splitVoices[v]->ue);
        }
    }
    for(v = 0; v < n; v++)
    {
        stableSort(splitOut[v].notesP, (size_t)splitOut[v].count, sizeof(Note *), cmpNoteStartKey);
    }
    *cutP = splitCut;
    free(startP);
    free(newP);
    free(candsP);
    free(keepP);
    free(inRestP);
}

// Make a group into n parts in splitOut, two or more by splitGroup. One
// part keeps the highest of notes starting together, the first on a tie,
// and each note ends at the next onset (noteList cuts it). For one part,
// *cutP gets the count of notes overlapping the next.
static void reduceGroup(Note **notesP, int count, int n, const Grid *gridP, int *cutP, int *droppedP, int *doubledP)
{
    int i;
    int j;
    Note *bestP;

    if( n >= 2 )
    {
        splitGroup(notesP, count, n, gridP, cutP, droppedP, doubledP);
        return;
    }
    splitOut[0].notesP = growOrFail(NULL, (size_t)count, sizeof(Note *));
    splitOut[0].count = 0;
    *droppedP = 0;
    *doubledP = 0;
    i = 0;
    while( i < count )
    {
        bestP = notesP[i];
        j = (i + 1);
        while( (j < count) && (notesP[j]->us == notesP[i]->us) )
        {
            (*droppedP)++;
            if( notesP[j]->key > bestP->key )
            {
                bestP = notesP[j];
            }
            j++;
        }
        splitOut[0].notesP[splitOut[0].count++] = bestP;
        i = j;
    }
    *cutP = 0;
    for(i = 0; (i + 1) < splitOut[0].count; i++)
    {
        if( splitOut[0].notesP[i]->ue > splitOut[0].notesP[i + 1]->us )
        {
            (*cutP)++;
        }
    }
}

// The articulation nearest a silent share, as an index into artLetters,
// the first on a tie.
static int nearestArt(double silent)
{
    double x;
    int best;
    int k;

    x = ((silent > 0.0) ? silent : 0.0);
    best = 0;
    for(k = 1; k < 5; k++)
    {
        if( absDouble(artShares[k] - x) < absDouble(artShares[best] - x) )
        {
            best = k;
        }
    }
    return best;
}

// One part's notes, each (start, length, key, articulation). A note runs
// to the next onset when the silence before it is within 1/16 of an
// articulation's share, or, played in, a 64th or less. Otherwise it keeps
// its end and a rest follows. With --organ a note from a half note up is
// legato, unless the next note is the same key at once, and others normal.
static Listed *noteList(const NoteRun *partP, const Grid *gridP, const Bars *barsP)
{
    Listed *outP;
    int k;
    long nxt;
    long ue;
    double ticksTo;
    double silent;
    double x;
    int art;
    int same;
    const Note *xP;

    outP = growOrFail(NULL, (size_t)partP->count, sizeof(Listed));
    for(k = 0; k < partP->count; k++)
    {
        xP = partP->notesP[k];
        nxt = (((k + 1) < partP->count) ? partP->notesP[k + 1]->us : barsP->startsP[barsP->nBars]);
        ue = xP->ue;
        if( ue > nxt )
        {
            ue = nxt;
        }
        if( ue <= xP->us )
        {
            ue = (xP->us + 3);
        }
        outP[k].start = xP->us;
        outP[k].key = xP->key;
        if( optOrgan )
        {
            same = (((k + 1) < partP->count) && (partP->notesP[k + 1]->us == ue) && (partP->notesP[k + 1]->key == xP->key));
            if( gridP->played && ((nxt - ue) <= 3) )
            {
                ue = nxt;
            }
            outP[k].length = (ue - xP->us);
            outP[k].art = ((((ue - xP->us) >= LEGATO_FROM) && !same) ? 'l' : 'e');
            continue;
        }
        ticksTo = ((((double)((long long)nxt * gridP->div) / 48.0) + (double)gridP->off) - (double)xP->s);
        silent = (1.0 - ((xP->e - (double)xP->s) / ticksTo));
        art = nearestArt(silent);
        x = ((silent > 0.0) ? silent : 0.0);
        if( (absDouble(artShares[art] - x) <= nearShare) || (gridP->played && ((nxt - ue) <= 3)) )
        {
            outP[k].length = (nxt - xP->us);
            outP[k].art = artLetters[art];
            continue;
        }
        silent = (1.0 - ((xP->e - (double)xP->s) / ((double)((long long)(ue - xP->us) * gridP->div) / 48.0)));
        art = nearestArt(silent);
        outP[k].length = (ue - xP->us);
        outP[k].art = artLetters[art];
    }
    return outP;
}

// Tempo and dynamics.

// The tempo words, each (unit, N), N = TEMPO_K / quarter notes a minute,
// rounded half up, a later one at a unit replacing the earlier. They are
// scaled by --tempo, those at or past the end are dropped, and a bar over
// MAXSEG keeps its first 15 and its last.
static TempoWord *tempoWords(const Song *songP, const Grid *gridP, const Bars *barsP, const char *fnameP, int *countP)
{
    TempoWord *wP;
    TempoWord *outP;
    int nW;
    int nOut;
    int i;
    int b;
    int first;
    int inBar;
    long u;
    long n;
    long w0;

    wP = growOrFail(NULL, (size_t)songP->nTempos, sizeof(TempoWord));
    nW = 0;
    for(i = 0; i < songP->nTempos; i++)
    {
        if( !unitOf(gridP, songP->temposP[i].tick, &u) )
        {
            refuse("bar %d: a tempo change off the grid", barOf(barsP, ((songP->temposP[i].tick * 48) / songP->div)));
        }
        n = (long)((((2LL * TEMPO_K) * songP->temposP[i].us) + 60000000LL) / 120000000LL);
        if( (n < 1) || (n > MAX_TEMPO) )
        {
            refuse("a tempo of %ld us a quarter is outside the tempo word", songP->temposP[i].us);
        }
        if( u < 0 )
        {
            u = 0;
        }
        if( (nW > 0) && (wP[nW - 1].unit == u) )
        {
            nW--;
        }
        wP[nW].unit = u;
        wP[nW].n = n;
        nW++;
    }
    if( optHasTempo )
    {
        w0 = wP[0].n;
        for(i = 0; i < nW; i++)
        {
            wP[i].n = (long)((((2LL * wP[i].n) * optTempo) + w0) / (2LL * w0));
        }
        for(i = 0; i < nW; i++)
        {
            if( (wP[i].n < 1) || (wP[i].n > MAX_TEMPO) )
            {
                refuse("--tempo %ld makes a tempo word of %ld", optTempo, wP[i].n);
            }
        }
    }
    outP = growOrFail(NULL, (size_t)nW, sizeof(TempoWord));
    nOut = 0;
    for(b = 0; b < barsP->nBars; b++)
    {
        first = -1;
        inBar = 0;
        for(i = 0; i < nW; i++)
        {
            if( (wP[i].unit >= barsP->startsP[b]) && (wP[i].unit < barsP->startsP[b + 1]) )
            {
                if( first < 0 )
                {
                    first = i;
                }
                inBar++;
            }
        }
        if( inBar > MAXSEG )
        {
            report("%s: bar %d: %d tempo words, the first %d and the last kept", fnameP, (b + 1), inBar, (MAXSEG - 1));
            memcpy((outP + nOut), (wP + first), ((size_t)(MAXSEG - 1) * sizeof(TempoWord)));
            nOut = (nOut + (MAXSEG - 1));
            outP[nOut++] = wP[first + inBar - 1];
        }
        else if( inBar > 0 )
        {
            memcpy((outP + nOut), (wP + first), ((size_t)inBar * sizeof(TempoWord)));
            nOut = (nOut + inBar);
        }
    }
    free(wP);
    *countP = nOut;
    return outP;
}

// The level nearest a velocity, 0 (pp) to 5 (ff), the softer on a tie.
static int levelOf(int v)
{
    int best;
    int k;

    best = 0;
    for(k = 1; k < NLEVELS; k++)
    {
        if( absLong(dynVelocities[k] - v) < absLong(dynVelocities[best] - v) )
        {
            best = k;
        }
    }
    return best;
}

// Set levelAtP[i] to the dynamic written at note i, or -1 for none. A part
// whose notes all have one level gets none. Otherwise, from f, a dynamic is
// written when the level moves two or more, or stays one away for three notes.
static void dynamicsOf(const int *levelsP, int count, int *levelAtP)
{
    int i;
    int cur;
    int runN;
    int r0;
    int r1;
    int r2;
    int allOne;

    for(i = 0; i < count; i++)
    {
        levelAtP[i] = -1;
    }
    allOne = 1;
    for(i = 1; i < count; i++)
    {
        if( levelsP[i] != levelsP[0] )
        {
            allOne = 0;
        }
    }
    if( allOne )
    {
        return;
    }
    cur = LEVEL_F;
    // The run of levels other than the one in force. Only its last three count.
    runN = 0;
    r0 = -1;
    r1 = -1;
    r2 = -1;
    for(i = 0; i < count; i++)
    {
        if( absLong(levelsP[i] - cur) >= 2 )
        {
            levelAtP[i] = levelsP[i];
            cur = levelsP[i];
            runN = 0;
        }
        else if( levelsP[i] != cur )
        {
            r0 = r1;
            r1 = r2;
            r2 = levelsP[i];
            runN++;
            if( (runN >= 3) && (r0 == r2) && (r1 == r2) )
            {
                levelAtP[i] = r2;
                cur = r2;
                runN = 0;
            }
        }
        else
        {
            runN = 0;
        }
    }
}

// Each note's level, from its velocity, or from the last CC 7 or CC 11 at
// or before its onset (f before the first).
static void noteLevels(const NoteRun *partP, const Group *gP, int *levelsP)
{
    const CcList *listP;
    int k;
    int i;
    int v;

    if( optDynamics == DYN_VELOCITY )
    {
        for(k = 0; k < partP->count; k++)
        {
            levelsP[k] = levelOf(partP->notesP[k]->vel);
        }
        return;
    }
    listP = &gP->cc[(optDynamics == DYN_CC7) ? CC_VOLUME : CC_EXPRESSION];
    for(k = 0; k < partP->count; k++)
    {
        v = -1;
        for(i = 0; i < listP->count; i++)
        {
            if( listP->eventsP[i].tick <= partP->notesP[k]->s )
            {
                v = listP->eventsP[i].value;
            }
        }
        levelsP[k] = ((v >= 0) ? levelOf(v) : LEVEL_F);
    }
}

// Words.

// Split a length in units into written times, largest first, the triplets
// after. A length not a multiple of 3 takes the least triplet part that
// leaves one. Returns the count, or -1 for a length no note word holds.
static int piecesOf(long length, int *tripP, int *t64P, int cap)
{
    long trip;
    long n;
    int count;
    int pass;
    int v;

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
        trip = (((((length - 2) % 3) + 3) % 3 == 0) ? 2 : 4);
        if( trip > length )
        {
            return -1;
        }
    }
    count = 0;
    for(pass = 0; pass < 2; pass++)
    {
        n = ((pass == 0) ? ((length - trip) / 3) : (trip / 2));
        while( n > 0 )
        {
            for(v = ((n < 127) ? (int)n : 127); !timesOk[v]; v--)
            {
            }
            if( count < cap )
            {
                tripP[count] = pass;
                t64P[count] = v;
            }
            count++;
            n = (n - v);
        }
    }
    return count;
}

// Add a word to a growing array.
static void addWord(long **wordsP, int *countP, int *capP, long w)
{
    if( *countP == *capP )
    {
        *capP = ((*capP * 2) + 256);
        *wordsP = growOrFail(*wordsP, (size_t)*capP, sizeof(long));
    }
    (*wordsP)[(*countP)++] = w;
}

// One part's bars as words. The notes and the rests between them are cut
// at the bar lines and (part 1) at the tempo words, each length written as
// times, legato into the next piece of the same note. A tempo word goes
// before the piece at its unit, then a dynamic before the first piece of
// its note. A rest takes the bar's mode, the commonest articulation where
// it leads the last by two, or legato where a rest follows it in the bar.
static PartBars partWords(const Listed *notesP, int nNotes, const TempoWord *tempoP, int nTempo, const Bars *barsP,
                          const int *dynUnitP, const int *dynLevelP, int nDyn, const char *fnameP, int pn)
{
    PartBars out;
    Item *itemsP;
    int nItems;
    int capItems;
    Item *cutP;
    int nCut;
    int capCut;
    Piece *piecesP;
    int nPieces;
    int capPieces;
    long t;
    int i;
    int j;
    int b;
    int k;
    long b1;
    int nW;
    int capW;
    char mode;
    int counts[5];
    int bestA;
    int modeA;
    int a;
    int trips[256];
    int t64s[256];
    int np;
    long edge;
    long segStart;
    long w;

    capItems = ((2 * nNotes) + 2);
    itemsP = growOrFail(NULL, (size_t)capItems, sizeof(Item));
    nItems = 0;
    t = 0;
    for(i = 0; i < nNotes; i++)
    {
        if( notesP[i].start > t )
        {
            itemsP[nItems].s = t;
            itemsP[nItems].ln = (notesP[i].start - t);
            itemsP[nItems].key = -1;
            itemsP[nItems].art = 0;
            nItems++;
        }
        itemsP[nItems].s = notesP[i].start;
        itemsP[nItems].ln = notesP[i].length;
        itemsP[nItems].key = notesP[i].key;
        itemsP[nItems].art = notesP[i].art;
        nItems++;
        t = (notesP[i].start + notesP[i].length);
    }
    if( t < barsP->startsP[barsP->nBars] )
    {
        itemsP[nItems].s = t;
        itemsP[nItems].ln = (barsP->startsP[barsP->nBars] - t);
        itemsP[nItems].key = -1;
        itemsP[nItems].art = 0;
        nItems++;
    }
    // Each item cut at every bar line and tempo word inside it.
    capCut = (nItems + barsP->nBars + nTempo + 4);
    cutP = growOrFail(NULL, (size_t)capCut, sizeof(Item));
    nCut = 0;
    b = 0;
    j = 0;
    for(i = 0; i < nItems; i++)
    {
        segStart = itemsP[i].s;
        while( 1 )
        {
            // The first cut strictly inside what is left of the item.
            edge = (itemsP[i].s + itemsP[i].ln);
            // A note made a 64th long may run past the next onset, so the search starts again.
            if( (b > 0) && (barsP->startsP[b - 1] > segStart) )
            {
                b = 0;
            }
            if( (j > 0) && (tempoP[j - 1].unit > segStart) )
            {
                j = 0;
            }
            while( (b <= barsP->nBars) && (barsP->startsP[b] <= segStart) )
            {
                b++;
            }
            if( (b <= barsP->nBars) && (barsP->startsP[b] < edge) )
            {
                edge = barsP->startsP[b];
            }
            while( (j < nTempo) && (tempoP[j].unit <= segStart) )
            {
                j++;
            }
            if( (j < nTempo) && (tempoP[j].unit < edge) )
            {
                edge = tempoP[j].unit;
            }
            if( nCut == capCut )
            {
                capCut = (capCut * 2);
                cutP = growOrFail(cutP, (size_t)capCut, sizeof(Item));
            }
            cutP[nCut].s = segStart;
            cutP[nCut].ln = (edge - segStart);
            cutP[nCut].key = itemsP[i].key;
            cutP[nCut].art = (((edge == (itemsP[i].s + itemsP[i].ln)) || (itemsP[i].key < 0)) ? itemsP[i].art : 'l');
            cutP[nCut].first = (segStart == itemsP[i].s);
            nCut++;
            if( edge == (itemsP[i].s + itemsP[i].ln) )
            {
                break;
            }
            segStart = edge;
        }
    }
    out.nBars = barsP->nBars;
    out.barOffP = growOrFail(NULL, (size_t)(barsP->nBars + 1), sizeof(long));
    out.wordsP = NULL;
    nW = 0;
    capW = 0;
    capPieces = 64;
    piecesP = growOrFail(NULL, (size_t)capPieces, sizeof(Piece));
    k = 0;
    j = 0;
    mode = 'e';
    for(b = 0; b < barsP->nBars; b++)
    {
        b1 = barsP->startsP[b + 1];
        out.barOffP[b] = nW;
        nPieces = 0;
        while( (k < nCut) && (cutP[k].s < b1) )
        {
            if( (nPieces + 260) > capPieces )
            {
                capPieces = ((capPieces * 2) + 260);
                piecesP = growOrFail(piecesP, (size_t)capPieces, sizeof(Piece));
            }
            for(i = 0; i < nTempo; i++)
            {
                if( tempoP[i].unit == cutP[k].s )
                {
                    piecesP[nPieces].kind = PIECE_TEMPO;
                    piecesP[nPieces].value = tempoP[i].n;
                    nPieces++;
                    break;
                }
            }
            if( (cutP[k].key >= 0) && cutP[k].first )
            {
                for(i = 0; i < nDyn; i++)
                {
                    if( dynUnitP[i] == cutP[k].s )
                    {
                        piecesP[nPieces].kind = PIECE_DYNAMIC;
                        piecesP[nPieces].value = dynLevelP[i];
                        nPieces++;
                        break;
                    }
                }
            }
            np = piecesOf(cutP[k].ln, trips, t64s, 256);
            if( np < 0 )
            {
                refuse("%spart %d, bar %d: %s of %ld unit%s, which no note word holds", fnameP, (pn + 1), (b + 1),
                       ((cutP[k].key < 0) ? "a rest" : "a note"), cutP[k].ln, ((cutP[k].ln == 1) ? "" : "s"));
            }
            if( np > 256 )
            {
                refuse("%spart %d, bar %d: a note of %ld units, too long", fnameP, (pn + 1), (b + 1), cutP[k].ln);
            }
            for(i = 0; i < np; i++)
            {
                piecesP[nPieces].kind = ((cutP[k].key < 0) ? PIECE_REST : PIECE_NOTE);
                piecesP[nPieces].key = cutP[k].key;
                piecesP[nPieces].art = ((((i == (np - 1)) || (cutP[k].key < 0))) ? cutP[k].art : 'l');
                piecesP[nPieces].trip = trips[i];
                piecesP[nPieces].t64 = t64s[i];
                nPieces++;
                if( nPieces >= capPieces )
                {
                    capPieces = (capPieces * 2);
                    piecesP = growOrFail(piecesP, (size_t)capPieces, sizeof(Piece));
                }
            }
            k++;
        }
        // The bar's mode. Of its notes' articulations in letter order, the
        // commonest, the first on a tie, becomes the mode when it leads it by two.
        memset(counts, 0, sizeof(counts));
        for(i = 0; i < nPieces; i++)
        {
            if( piecesP[i].kind == PIECE_NOTE )
            {
                counts[(int)(strchr("ehlqs", piecesP[i].art) - "ehlqs")]++;
            }
        }
        bestA = -1;
        for(a = 0; a < 5; a++)
        {
            if( (counts[a] > 0) && ((bestA < 0) || (counts[a] > counts[bestA])) )
            {
                bestA = a;
            }
        }
        if( bestA >= 0 )
        {
            modeA = (int)(strchr("ehlqs", mode) - "ehlqs");
            if( counts[bestA] > (counts[modeA] + 1) )
            {
                mode = "ehlqs"[bestA];
            }
        }
        for(i = 0; i < nPieces; i++)
        {
            if( piecesP[i].kind == PIECE_TEMPO )
            {
                addWord(&out.wordsP, &nW, &capW, (TEMPO_MARK + piecesP[i].value));
                continue;
            }
            if( piecesP[i].kind == PIECE_DYNAMIC )
            {
                addWord(&out.wordsP, &nW, &capW, (BAR + 1 + piecesP[i].value));
                continue;
            }
            w = (piecesP[i].t64 + (piecesP[i].trip ? TRIPLET : 0));
            if( piecesP[i].kind == PIECE_REST )
            {
                char art = mode;

                for(a = (i + 1); a < nPieces; a++)
                {
                    if( (piecesP[a].kind == PIECE_NOTE) || (piecesP[a].kind == PIECE_REST) )
                    {
                        break;
                    }
                }
                if( (a < nPieces) && (piecesP[a].kind == PIECE_REST) )
                {
                    art = 'l';
                }
                w = (w + REST_PITCH);
                w = (w + ((art == 'l') ? 0400000 : ((art == 's') ? 0200000 : ((art == 'h') ? 040000 : ((art == 'q') ? 020000 : 0)))));
            }
            else
            {
                w =(w + ((long)(piecesP[i].key - KEY_SEMITONE) << 7));
                w = (w + ((piecesP[i].art == 'l') ? 0400000 : ((piecesP[i].art == 's') ? 0200000
                    : ((piecesP[i].art == 'h') ? 040000 : ((piecesP[i].art == 'q') ? 020000 : 0)))));
            }
            addWord(&out.wordsP, &nW, &capW, w);
        }
    }
    out.barOffP[barsP->nBars] = nW;
    free(itemsP);
    free(cutP);
    free(piecesP);
    return out;
}

// A part's notes and bar pointers. Each bar is stored followed by a bar
// line, and with sharing a bar identical to an earlier one is pointed at again.
static Stored storeBars(const PartBars *barsP)
{
    Stored out;
    long *firstP;
    int capN;
    int b;
    int c;
    int i;
    long len;

    out.notesP = NULL;
    out.nNotes = 0;
    capN = 0;
    out.ptrP = growOrFail(NULL, (size_t)(barsP->nBars + 1), sizeof(long));
    out.nPtr = 0;
    firstP = growOrFail(NULL, (size_t)barsP->nBars, sizeof(long));
    for(b = 0; b < barsP->nBars; b++)
    {
        len = (barsP->barOffP[b + 1] - barsP->barOffP[b]);
        for(c = 0; optShare && (c < b); c++)
        {
            if( (firstP[c] >= 0) && ((barsP->barOffP[c + 1] - barsP->barOffP[c]) == len)
                && (memcmp((barsP->wordsP + barsP->barOffP[c]), (barsP->wordsP + barsP->barOffP[b]), ((size_t)len * sizeof(long))) == 0) )
            {
                break;
            }
        }
        if( optShare && (c < b) )
        {
            firstP[b] = -1;
            out.ptrP[out.nPtr++] = firstP[c];
            continue;
        }
        firstP[b] = out.nNotes;
        out.ptrP[out.nPtr++] = out.nNotes;
        for(i = 0; i < len; i++)
        {
            addWord(&out.notesP, &out.nNotes, &capN, barsP->wordsP[barsP->barOffP[b] + i]);
        }
        addWord(&out.notesP, &out.nNotes, &capN, BAR);
    }
    out.ptrP[out.nPtr++] = BAR;
    free(firstP);
    return out;
}

// The orchestra.

// A text file's lines, split at each newline only, a NUL taken as a space,
// each a string in place. Returns NULL if it cannot be read.
static char **textLines(const char *pathP, int *countP)
{
    char *dP;
    char **linesP;
    long size;
    long i;
    int n;

    if( !(dP = (char *)readWhole(pathP, &size)) )
    {
        return NULL;
    }
    n = 1;
    for(i = 0; i < size; i++)
    {
        if( dP[i] == '\0' )
        {
            dP[i] = ' ';
        }
        else if( dP[i] == '\n' )
        {
            n++;
        }
    }
    linesP = growOrFail(NULL, (size_t)n, sizeof(char *));
    n = 0;
    linesP[n++] = dP;
    for(i = 0; i < size; i++)
    {
        if( dP[i] == '\n' )
        {
            dP[i] = '\0';
            linesP[n++] = (dP + i + 1);
        }
    }
    *countP = n;
    return linesP;
}

// Cut a line at its first #, if any.
static void dropComment(char *lineP)
{
    char *hashP;

    if( (hashP = strchr(lineP, '#')) != NULL )
    {
        *hashP = '\0';
    }
}

// The entries of a voice library, numbered as dacdrum numbers them. A #
// starts a comment and blank lines are not counted. Returns NULL if the
// library cannot be read.
static char **readLibrary(const char *pathP, int *countP)
{
    char **linesP;
    char **namesP;
    char *pP;
    int nLines;
    int i;
    size_t n;

    if( !(linesP = textLines(pathP, &nLines)) )
    {
        return NULL;
    }
    namesP = growOrFail(NULL, (size_t)nLines, sizeof(char *));
    *countP = 0;
    for(i = 0; i < nLines; i++)
    {
        // Trimmed as dacdrum trims it.
        dropComment(linesP[i]);
        for(pP = linesP[i]; (*pP == ' ') || (*pP == '\t'); pP++)
        {
        }
        n = strlen(pP);
        while( (n > 0) && ((pP[n - 1] == ' ') || (pP[n - 1] == '\t') || (pP[n - 1] == '\r') || (pP[n - 1] == '\n')) )
        {
            n--;
        }
        pP[n] = '\0';
        if( n > 0 )
        {
            namesP[(*countP)++] = pP;
        }
    }
    return namesP;
}

// Read a voice file's header as dacdrum reads it, whether its first mode is
// stop and its first family (NULL for none). Returns 0 if the file cannot
// be read, else 1.
static int voiceKind(const char *pathP, int *stopP, char **familyPP)
{
    char **linesP;
    char *pP;
    char *endP;
    int nLines;
    int haveMode;
    int i;
    size_t n;

    if( !(linesP = textLines(pathP, &nLines)) )
    {
        return 0;
    }
    *stopP = 0;
    *familyPP = NULL;
    haveMode = 0;
    for(i = 0; i < nLines; i++)
    {
        for(pP = linesP[i]; isspace((unsigned char)*pP); pP++)
        {
        }
        for(endP = pP; (*endP != '\0') && !isspace((unsigned char)*endP); endP++)
        {
        }
        n = (size_t)(endP - pP);
        if( (n == 4) && (strncmp(pP, "mode", 4) == 0) && !haveMode )
        {
            for(pP = endP; isspace((unsigned char)*pP); pP++)
            {
            }
            for(endP = pP; (*endP != '\0') && !isspace((unsigned char)*endP); endP++)
            {
            }
            if( endP > pP )
            {
                *stopP = (((endP - pP) == 4) && (strncmp(pP, "stop", 4) == 0));
                haveMode = 1;
            }
        }
        else if( (n == 6) && (strncmp(pP, "family", 6) == 0) && (*familyPP == NULL) )
        {
            for(pP = endP; (*pP == ' ') || (*pP == '\t'); pP++)
            {
            }
            n = strlen(pP);
            while( (n > 0) && ((pP[n - 1] == ' ') || (pP[n - 1] == '\t') || (pP[n - 1] == '\r') || (pP[n - 1] == '\n')) )
            {
                n--;
            }
            pP[n] = '\0';
            *familyPP = pP;
        }
    }
    return 1;
}

// Are two strings the same, case not counting? Returns 1 if so, else 0.
static int sameNoCase(const char *aP, const char *bP)
{
    while( (*aP != '\0') && (tolower((unsigned char)*aP) == tolower((unsigned char)*bP)) )
    {
        aP++;
        bP++;
    }
    return (tolower((unsigned char)*aP) == tolower((unsigned char)*bP));
}

// A voice's number in the library, found by its entry as written or its
// file name less .voice, case not counting. Returns -1 if it has none.
static int voiceNumber(char **namesP, int count, const char *wantP)
{
    const char *baseP;
    size_t n;
    size_t i;
    int k;

    for(k = 0; k < count; k++)
    {
        baseP = baseName(namesP[k]);
        n = strlen(baseP);
        if( (n >= 6) && sameNoCase((baseP + n - 6), ".voice") )
        {
            n = (n - 6);
        }
        if( sameNoCase(wantP, namesP[k]) )
        {
            return k;
        }
        // The file name less .voice, its first n characters, case not counting.
        for(i = 0; (i < n) && (tolower((unsigned char)wantP[i]) == tolower((unsigned char)baseP[i])); i++)
        {
        }
        if( (i == n) && (wantP[n] == '\0') )
        {
            return k;
        }
    }
    return -1;
}

// Is the whole of textP one of the count names, case not counting? Returns its index, or -1.
static int nameIndex(const char *textP, const char **namesP, int count)
{
    int i;

    for(i = 0; i < count; i++)
    {
        if( sameNoCase(textP, namesP[i]) )
        {
            return i;
        }
    }
    return -1;
}

// Are textP's len characters 1 to 6 digits, after an optional minus when
// allowed? Returns 1 if so, else 0.
static int isNumber(const char *textP, int minus, size_t len)
{
    size_t i;
    size_t d;

    i = 0;
    if( minus && (len > 0) && (textP[0] == '-') )
    {
        i = 1;
    }
    d = (len - i);
    if( (d < 1) || (d > 6) )
    {
        return 0;
    }
    for(; i < len; i++)
    {
        if( !isdigit((unsigned char)textP[i]) )
        {
            return 0;
        }
    }
    return 1;
}

// A version 3 orchestra block's 42 words, count and checksum included,
// into outP.
static void makeBlock(int per, const OrchLine *linesP, long room, long onset, long *outP)
{
    long body[40];
    int c;
    int s;
    int i;

    i = 0;
    body[i++] = ORCH_VERSION3;
    body[i++] = per;
    for(c = 0; c < NCONV; c++)
    {
        body[i++] = ((linesP[c].part >= 0) ? (linesP[c].part + 1) : 0);
        body[i++] = linesP[c].octave;
        body[i++] = (linesP[c].volume & WORD_MASK);
        body[i++] = 0;
        body[i++] = linesP[c].place;
        for(s = 0; s < STOPS; s++)
        {
            body[i++] = ((s < linesP[c].nStops) ? (linesP[c].stops[s] + 1) : 0);
        }
    }
    body[i++] = (room & WORD_MASK);
    body[i++] = onset;
    outP[0] = ORCH_COUNT3;
    memcpy((outP + 1), body, sizeof(body));
    outP[41] = checksum(body, 40);
}

// A converter's line with no part, voice 0 at the converter's default place.
static void emptyLine(OrchLine *lineP, int c)
{
    memset(lineP, 0, sizeof(OrchLine));
    lineP->part = -1;
    lineP->place = defaultPlaces[c];
    lineP->stops[0] = 0;
    lineP->nStops = 1;
}

// Whether a converter's several voices may sound together by the player's
// rule, each a stop and all of one family. None of the nine (no library) is
// a stop. Returns 1 if they may, else 0. An unreadable voice file is refused.
static int stopsOfOneFamily(const char *libDirP, char **namesP, const OrchLine *lnP, const char *whereP, int lineNo)
{
    char *pathP;
    char *familyP[STOPS];
    int stop[STOPS];
    int s;

    if( libDirP == NULL )
    {
        return 0;
    }
    // Every voice is read first, so one that cannot be read is refused before the rule is applied.
    for(s = 0; s < lnP->nStops; s++)
    {
        const char *entryP = namesP[lnP->stops[s]];

        pathP = growOrFail(NULL, (strlen(libDirP) + strlen(entryP) + 1), 1);
        strcpy(pathP, ((entryP[0] == '/') ? "" : libDirP));
        strcat(pathP, entryP);
        if( !voiceKind(pathP, &stop[s], &familyP[s]) )
        {
            refuse("%s:%d: the voice %s cannot be read", whereP, lineNo, entryP);
        }
    }
    for(s = 0; s < lnP->nStops; s++)
    {
        if( !stop[s] || ((familyP[s] == NULL) != (familyP[0] == NULL))
            || ((familyP[s] != NULL) && (strcmp(familyP[s], familyP[0]) != 0)) )
        {
            return 0;
        }
    }
    return 1;
}

// An orchestra block from an orchestra file (README.md). A line it cannot
// take is refused.
static void orchestraFile(const char *pathP, int per, long *outP)
{
    char **textP;
    int nLines;
    char **tok;
    int nTok;
    char *pP;
    char **namesP;
    int nNames;
    long room;
    long onset;
    OrchLine lines[NCONV];
    int lineNo;
    const char *whereP;
    char *libPathP;
    char *libDirP;
    const char *slashP;
    int c;
    int t;
    int k;
    int v;
    int nVoices;
    long a;
    size_t len;
    OrchLine *lnP;

    namesP = (char **)nineNames;
    nNames = 9;
    libDirP = NULL;
    room = CONFIGURED;
    onset = CONFIGURED;
    for(c = 0; c < NCONV; c++)
    {
        emptyLine(&lines[c], c);
    }
    whereP = baseName(pathP);
    if( !(textP = textLines(pathP, &nLines)) )
    {
        refuse("%s: cannot be read", pathP);
    }
    for(lineNo = 1; lineNo <= nLines; lineNo++)
    {
        dropComment(textP[lineNo - 1]);
        // At most a token for every two characters.
        tok = growOrFail(NULL, ((strlen(textP[lineNo - 1]) / 2) + 1), sizeof(char *));
        nTok = 0;
        pP = textP[lineNo - 1];
        while( 1 )
        {
            while( isspace((unsigned char)*pP) )
            {
                pP++;
            }
            if( *pP == '\0' )
            {
                break;
            }
            tok[nTok++] = pP;
            while( (*pP != '\0') && !isspace((unsigned char)*pP) )
            {
                pP++;
            }
            if( *pP != '\0' )
            {
                *pP++ = '\0';
            }
        }
        if( nTok == 0 )
        {
            continue;
        }
        if( sameNoCase(tok[0], "library") && (nTok == 2) )
        {
            // Relative to the orchestra file's directory.
            slashP = strrchr(pathP, '/');
            libPathP = growOrFail(NULL, (strlen(pathP) + strlen(tok[1]) + 1), 1);
            if( (tok[1][0] == '/') || (slashP == NULL) )
            {
                strcpy(libPathP, tok[1]);
            }
            else
            {
                memcpy(libPathP, pathP, (size_t)((slashP - pathP) + 1));
                strcpy((libPathP + (slashP - pathP) + 1), tok[1]);
            }
            if( !(namesP = readLibrary(libPathP, &nNames)) )
            {
                refuse("%s:%d: the library %s cannot be read", whereP, lineNo, tok[1]);
            }
            // Its voices' files are relative to its directory.
            slashP = strrchr(libPathP, '/');
            libDirP = copyString(libPathP);
            libDirP[(slashP != NULL) ? ((slashP - libPathP) + 1) : 0] = '\0';
            continue;
        }
        if( sameNoCase(tok[0], "room") && (nTok == 2) && ((k = nameIndex(tok[1], roomNames, 5)) >= 0) )
        {
            room = roomWords[k];
            continue;
        }
        if( sameNoCase(tok[0], "onset") && (nTok == 2) && ((k = nameIndex(tok[1], onsetNames, 8)) >= 0) )
        {
            onset = onsetWords[k];
            continue;
        }
        if( (strlen(tok[0]) == 1) && (tok[0][0] >= '1') && (tok[0][0] <= '4') && (nTok >= 2)
            && (sameNoCase(tok[1], "part") || sameNoCase(tok[1], "none")) )
        {
            c = (tok[0][0] - '1');
            lnP = &lines[c];
            if( lnP->given )
            {
                refuse("%s:%d: converter %d given twice", whereP, lineNo, (c + 1));
            }
            t = 2;
            lnP->part = -1;
            if( sameNoCase(tok[1], "part") )
            {
                if( (nTok < 3) || !isNumber(tok[2], 0, strlen(tok[2])) || (atol(tok[2]) < 1) || (atol(tok[2]) > per) )
                {
                    refuse("%s:%d: no part %s (1 to %d)", whereP, lineNo, ((nTok < 3) ? "given" : tok[2]), per);
                }
                lnP->part = (int)(atol(tok[2]) - 1);
                t = 3;
            }
            lnP->nStops = 0;
            nVoices = 0;
            for(; t < nTok; t++)
            {
                len = strlen(tok[t]);
                if( (k = nameIndex(tok[t], feetNames, 5)) >= 0 )
                {
                    lnP->octave = k;
                }
                else if( (len >= 3) && sameNoCase((tok[t] + len - 2), "db") && isNumber(tok[t], 1, (len - 2)) )
                {
                    a = -atol(tok[t]);
                    if( ((a % 3) != 0) || (a < 0) || (a > 45) )
                    {
                        refuse("%s:%d: %s is not a slider's level (0dB to -45dB, 3dB a point)", whereP, lineNo, tok[t]);
                    }
                    lnP->volume = (8 * a);
                }
                else if( (k = nameIndex(tok[t], placeNames, 9)) >= 0 )
                {
                    lnP->place = (k * 040);
                }
                else
                {
                    if( (v = voiceNumber(namesP, nNames, tok[t])) < 0 )
                    {
                        refuse("%s:%d: no voice %s in the library", whereP, lineNo, tok[t]);
                    }
                    // Too many are refused once the whole line is read.
                    if( nVoices < STOPS )
                    {
                        lnP->stops[lnP->nStops++] = v;
                    }
                    nVoices++;
                }
            }
            if( nVoices > STOPS )
            {
                refuse("%s:%d: more than %d stops", whereP, lineNo, STOPS);
            }
            if( (lnP->nStops > 1) && !stopsOfOneFamily(libDirP, namesP, lnP, whereP, lineNo) )
            {
                refuse("%s:%d: converter %d's voices are not stops of one family, and only those sound together",
                       whereP, lineNo, (c + 1));
            }
            if( (lnP->part >= 0) && (lnP->nStops == 0) )
            {
                refuse("%s:%d: a part needs a voice", whereP, lineNo);
            }
            if( lnP->nStops == 0 )
            {
                lnP->stops[0] = 0;
                lnP->nStops = 1;
            }
            lnP->given = 1;
            continue;
        }
        refuse("%s:%d: not understood", whereP, lineNo);
    }
    makeBlock(per, lines, room, onset, outP);
}

// An orchestra block from the first movement. Converter c plays part c, on
// the voice of the nine its group's first program names, at the place of its
// first pan and the level of its first volume at or before the part's
// first note.
static void orchestraGm(int per, const Movement *moveP, const char *fnameP, long *outP)
{
    OrchLine lines[NCONV];
    const Group *gP;
    long first;
    int c;
    int prog;
    int v;
    int i;
    int k;
    int best;
    int val;

    for(c = 0; c < NCONV; c++)
    {
        emptyLine(&lines[c], c);
    }
    for(c = 0; (c < per) && (c < moveP->nParts); c++)
    {
        gP = moveP->partsP[c].groupP;
        first = moveP->partsP[c].firstTick;
        prog = (gP->hasProgram ? gP->program : 0);
        v = -1;
        for(i = 0; i < 8; i++)
        {
            if( (prog >= gmMap[i][0]) && (prog < gmMap[i][1]) )
            {
                v = gmMap[i][2];
                break;
            }
        }
        if( v < 0 )
        {
            report("%s: part %d: program %d has no voice among the nine, the principal", fnameP, (c + 1), prog);
            v = PRINCIPAL;
        }
        lines[c].part = c;
        lines[c].stops[0] = v;
        lines[c].nStops = 1;
        for(i = 0; i < gP->cc[CC_PAN].count; i++)
        {
            if( gP->cc[CC_PAN].eventsP[i].tick <= first )
            {
                lines[c].place = (((gP->cc[CC_PAN].eventsP[i].value * 8) + 63) / 127) * 040;
                break;
            }
        }
        for(i = 0; i < gP->cc[CC_VOLUME].count; i++)
        {
            if( gP->cc[CC_VOLUME].eventsP[i].tick <= first )
            {
                val = gP->cc[CC_VOLUME].eventsP[i].value;
                best = 0;
                for(k = 1; k < 16; k++)
                {
                    if( absLong(volumeCc[k] - val) < absLong(volumeCc[best] - val) )
                    {
                        best = k;
                    }
                }
                lines[c].volume = (24 * best);
                break;
            }
        }
    }
    makeBlock(per, lines, CONFIGURED, CONFIGURED, outP);
}

// The tape.

// Place the tape as the player's READ TAPE does, each part's notes and bar
// line with its four words, then its pointers and end mark, each piece
// below TEND or else from TAPE2 on to VEND. Returns 0 if a piece fits
// neither. Else returns 1, *belowP set if the tape is all below TEND and
// *voicesP the words left for the voices.
static int fitNew(const Stored *partsP, int nParts, int hasOrch, int *belowP, long *voicesP)
{
    long fp;
    long aend;
    long size;
    long vbase;
    int p;
    int piece;

    fp = TAPE;
    aend = TEND;
    for(p = 0; p < nParts; p++)
    {
        for(piece = 0; piece < 2; piece++)
        {
            size = ((piece == 0) ? (partsP[p].nNotes + 5) : (partsP[p].nPtr + 1));
            if( (fp + size) >= aend )
            {
                if( aend != TEND )
                {
                    return 0;
                }
                fp = TAPE2;
                aend = VEND;
                if( (fp + size) >= aend )
                {
                    return 0;
                }
            }
            fp = (fp + size);
        }
    }
    *belowP = (aend == TEND);
    vbase = (*belowP ? TAPE2 : fp);
    if( hasOrch && ((fp + 40) >= aend) )
    {
        if( (aend != TEND) || ((TAPE2 + 40) >= VEND) )
        {
            return 0;
        }
    }
    *voicesP = (VEND - vbase);
    return 1;
}

// Does the tape fit the 1.0 player, each part taking 4 words, its notes, a
// bar line, its pointers, a 0 and its pointers again, then four words of
// look-ahead, all below its TEND? Returns 1 if so, else 0.
static int fitPlain(const Stored *partsP, int nParts)
{
    long total;
    int p;

    total = 0;
    for(p = 0; p < nParts; p++)
    {
        total = (total + partsP[p].nNotes + (2 * partsP[p].nPtr) + 6);
    }
    return ((total + 4) < (TEND_PLAIN - TAPE));
}

// One movement.

// The group's label in the report.
static char *groupLabel(const Group *gP, int k)
{
    char buf[64];
    char *outP;

    if( (gP->nameP != NULL) && (gP->nameP[0] != '\0') )
    {
        outP = growOrFail(NULL, (strlen(gP->nameP) + 64), 1);
        sprintf(outP, "group %d (%s)", (k + 1), gP->nameP);
        return outP;
    }
    sprintf(buf, "group %d", (k + 1));
    return copyString(buf);
}

// Does an item of --parts have its form, numbers of 1 to 6 digits joined by
// '+', then perhaps a ':' and one more? Returns 1 if so, else 0.
static int partItemOk(const char *itemP, size_t len)
{
    size_t i;
    int digits;
    int colon;

    i = 0;
    colon = 0;
    while( 1 )
    {
        digits = 0;
        while( (i < len) && isdigit((unsigned char)itemP[i]) )
        {
            digits++;
            i++;
        }
        if( (digits < 1) || (digits > 6) )
        {
            return 0;
        }
        if( i == len )
        {
            return 1;
        }
        if( colon || ((itemP[i] != '+') && (itemP[i] != ':')) )
        {
            return 0;
        }
        colon = (itemP[i] == ':');
        i++;
    }
}

// Parse --parts G[+G...][:N],... into its entries, each groups and a count.
// A group named twice in one entry is merged twice. A spec naming no such
// group, a group in two entries, or more than four parts is refused.
static PlanItem *parseParts(const char *specP, int nGroups, int *nPlanP)
{
    PlanItem *planP;
    const char *pP;
    const char *itemP;
    char *usedP;
    int nPlan;
    int cap;
    long total;
    long v;
    size_t len;
    size_t i;
    int k;
    int g;

    usedP = growOrFail(NULL, (size_t)nGroups, 1);
    memset(usedP, 0, (size_t)nGroups);
    planP = NULL;
    cap = 0;
    nPlan = 0;
    total = 0;
    pP = specP;
    while( 1 )
    {
        itemP = pP;
        while( (*pP != '\0') && (*pP != ',') )
        {
            pP++;
        }
        len = (size_t)(pP - itemP);
        if( !partItemOk(itemP, len) )
        {
            usage("--parts %s: not understood", specP);
        }
        if( nPlan == cap )
        {
            cap = ((cap * 2) + 4);
            planP = growOrFail(planP, (size_t)cap, sizeof(PlanItem));
        }
        planP[nPlan].idxP = growOrFail(NULL, ((len / 2) + 1), sizeof(int));
        planP[nPlan].nIdx = 0;
        planP[nPlan].n = 1;
        i = 0;
        while( i < len )
        {
            v = 0;
            while( (i < len) && isdigit((unsigned char)itemP[i]) )
            {
                v = ((v * 10) + (itemP[i] - '0'));
                i++;
            }
            planP[nPlan].idxP[planP[nPlan].nIdx++] = (int)(v - 1);
            if( (i < len) && (itemP[i] == ':') )
            {
                v = 0;
                for(i++; i < len; i++)
                {
                    v = ((v * 10) + (itemP[i] - '0'));
                }
                planP[nPlan].n = v;
            }
            else if( i < len )
            {
                i++;
            }
        }
        // Checked against the entries before this one only.
        if( planP[nPlan].n < 1 )
        {
            refuse("--parts %s: no such group, or one named twice", specP);
        }
        for(k = 0; k < planP[nPlan].nIdx; k++)
        {
            g = planP[nPlan].idxP[k];
            if( (g < 0) || (g >= nGroups) || usedP[g] )
            {
                refuse("--parts %s: no such group, or one named twice", specP);
            }
        }
        for(k = 0; k < planP[nPlan].nIdx; k++)
        {
            usedP[planP[nPlan].idxP[k]] = 1;
        }
        total = (total + planP[nPlan].n);
        nPlan++;
        if( *pP == '\0' )
        {
            break;
        }
        pP++;
    }
    if( total > NCONV )
    {
        refuse("--parts %s: more than %d parts", specP, NCONV);
    }
    free(usedP);
    *nPlanP = nPlan;
    return planP;
}

// One file made into a movement's parts.
static Movement makeMovement(const char *pathP)
{
    Movement mv;
    const char *fnameP;
    Song *songP;
    char **labelP;
    Grid grid;
    Bars bars;
    Note ***gnotesP;
    int *gcountP;
    long end;
    long us;
    long ue;
    long e;
    int key;
    int k;
    int i;
    int j;
    int g;
    double moved;
    double m;
    int haveMoved;
    PlanItem *planP;
    int nPlan;
    char *namedP;
    Note **mergedP[16];
    int mergedN[16];
    int atOnce[16];
    int shareOf[16];
    char *nameOf[16];
    int n;
    int c;
    int cut;
    int dropped;
    int doubled;
    NoteRun outRuns[NCONV];
    Group *outGroup[NCONV];
    int nOut;
    TempoWord *tempoP;
    int nTempo;
    Listed *listP;
    int *levelsP;
    int *levelAtP;
    int *dynUnitP;
    int *dynLevelP;
    int nDyn;
    int b;
    int inBar;
    int v;
    size_t len;

    fnameP = baseName(pathP);
    songP = readSong(pathP);
    labelP = growOrFail(NULL, (size_t)songP->nGroups, sizeof(char *));
    for(k = 0; k < songP->nGroups; k++)
    {
        labelP[k] = groupLabel(songP->groupsP[k], k);
    }
    if( songP->unpaired > 0 )
    {
        report("%s: %ld unpaired note events passed over", fnameP, songP->unpaired);
    }
    for(k = 0; k < songP->nGroups; k++)
    {
        if( songP->groupsP[k]->bends > 0 )
        {
            report("%s: %s: %ld pitch bends dropped", fnameP, labelP[k], songP->groupsP[k]->bends);
        }
        if( songP->groupsP[k]->cc[CC_SUSTAIN].count > 0 )
        {
            report("%s: %s: the sustain pedal passed over", fnameP, labelP[k]);
        }
    }
    makeGrid(&grid, songP->div, optPlayed, songP);
    end = 0;
    for(g = 0; g < songP->nGroups; g++)
    {
        for(i = 0; i < songP->groupsP[g]->nNotes; i++)
        {
            // The end is past every note's end, and every onset by a 64th, so a
            // last note too short to reach a unit still has one before the bars' end.
            e = nearUnit(&grid, songP->groupsP[g]->notesP[i].end);
            if( (nearUnit(&grid, songP->groupsP[g]->notesP[i].start) + 3) > e )
            {
                e = (nearUnit(&grid, songP->groupsP[g]->notesP[i].start) + 3);
            }
            if( ((g == 0) && (i == 0)) || (e > end) )
            {
                end = e;
            }
        }
    }
    makeBars(&bars, songP, end);
    gnotesP = growOrFail(NULL, (size_t)songP->nGroups, sizeof(Note **));
    gcountP = growOrFail(NULL, (size_t)songP->nGroups, sizeof(int));
    moved = 0.0;
    haveMoved = 0;
    for(k = 0; k < songP->nGroups; k++)
    {
        Group *gP = songP->groupsP[k];

        gnotesP[k] = growOrFail(NULL, (size_t)gP->nNotes, sizeof(Note *));
        gcountP[k] = gP->nNotes;
        for(i = 0; i < gP->nNotes; i++)
        {
            Note *xP;

            if( !unitOf(&grid, gP->notesP[i].start, &us) || !unitOf(&grid, gP->notesP[i].end, &ue) )
            {
                refuse("%s: %s, bar %d: a note off the grid of 64ths and triplet 64ths (--played snaps it)",
                       fnameP, labelP[k], barOf(&bars, nearUnit(&grid, gP->notesP[i].start)));
            }
            key = (int)(gP->notesP[i].key + optTranspose);
            if( (key < LOW_KEY) || (key > (optPlain ? HIGH_PLAIN : HIGH_KEY)) )
            {
                refuse("%s: %s, bar %d: key %d is outside %d-%d", fnameP, labelP[k], barOf(&bars, us), key, LOW_KEY,
                       (optPlain ? HIGH_PLAIN : HIGH_KEY));
            }
            if( optPlayed )
            {
                m = absDouble(((double)(gP->notesP[i].start - grid.off)) - ((double)((long long)us * grid.div) / 48.0));
                if( !haveMoved || (m > moved) )
                {
                    moved = m;
                    haveMoved = 1;
                }
            }
            xP = growOrFail(NULL, 1, sizeof(Note));
            xP->us = us;
            xP->ue = ue;
            xP->key = key;
            xP->vel = gP->notesP[i].vel;
            xP->s = gP->notesP[i].start;
            xP->e = (double)gP->notesP[i].end;
            gnotesP[k][i] = xP;
        }
        stableSort(gnotesP[k], (size_t)gcountP[k], sizeof(Note *), cmpNote);
    }
    if( optPlayed && haveMoved )
    {
        report("%s: snapped to 64ths at an offset of %ld ticks, notes moved up to %.1f ticks", fnameP, grid.off, moved);
    }
    // Which groups make which parts.
    if( optPartsP != NULL )
    {
        planP = parseParts(optPartsP, songP->nGroups, &nPlan);
        namedP = growOrFail(NULL, (size_t)songP->nGroups, 1);
        memset(namedP, 0, (size_t)songP->nGroups);
        for(i = 0; i < nPlan; i++)
        {
            for(j = 0; j < planP[i].nIdx; j++)
            {
                namedP[planP[i].idxP[j]] = 1;
            }
        }
        for(k = 0; k < songP->nGroups; k++)
        {
            if( !namedP[k] )
            {
                report("%s: %s left out (--parts)", fnameP, labelP[k]);
            }
        }
    }
    else
    {
        nPlan = 0;
        planP = growOrFail(NULL, (size_t)songP->nGroups, sizeof(PlanItem));
        for(k = 0; k < songP->nGroups; k++)
        {
            if( songP->groupsP[k]->chans == (1u << PERCUSSION) )
            {
                report("%s: %s: channel 10, percussion, left out", fnameP, labelP[k]);
            }
            else
            {
                planP[nPlan].idxP = growOrFail(NULL, 1, sizeof(int));
                planP[nPlan].idxP[0] = k;
                planP[nPlan].nIdx = 1;
                planP[nPlan].n = 0;
                nPlan++;
            }
        }
        if( nPlan > NCONV )
        {
            refuse("%s: %d groups, more than %d parts (--parts chooses)", fnameP, nPlan, NCONV);
        }
        if( nPlan == 0 )
        {
            refuse("%s: no notes but percussion", fnameP);
        }
    }
    for(i = 0; i < nPlan; i++)
    {
        mergedN[i] = 0;
        for(j = 0; j < planP[i].nIdx; j++)
        {
            mergedN[i] = (mergedN[i] + gcountP[planP[i].idxP[j]]);
        }
        mergedP[i] = growOrFail(NULL, (size_t)mergedN[i], sizeof(Note *));
        n = 0;
        for(j = 0; j < planP[i].nIdx; j++)
        {
            memcpy((mergedP[i] + n), gnotesP[planP[i].idxP[j]], ((size_t)gcountP[planP[i].idxP[j]] * sizeof(Note *)));
            n = (n + gcountP[planP[i].idxP[j]]);
        }
        stableSort(mergedP[i], (size_t)mergedN[i], sizeof(Note *), cmpNote);
        c = precut(mergedP[i], mergedN[i], &grid);
        len = 1;
        for(j = 0; j < planP[i].nIdx; j++)
        {
            len = (len + strlen(labelP[planP[i].idxP[j]]) + 1);
        }
        nameOf[i] = growOrFail(NULL, len, 1);
        nameOf[i][0] = '\0';
        for(j = 0; j < planP[i].nIdx; j++)
        {
            if( j > 0 )
            {
                strcat(nameOf[i], "+");
            }
            strcat(nameOf[i], labelP[planP[i].idxP[j]]);
        }
        if( c > 0 )
        {
            report("%s: %s: %d brief overlaps cut at the next onset", fnameP, nameOf[i], c);
        }
        atOnce[i] = mostAtOnce(mergedP[i], mergedN[i]);
    }
    if( optPartsP == NULL )
    {
        shares(atOnce, shareOf, nPlan);
    }
    else
    {
        for(i = 0; i < nPlan; i++)
        {
            shareOf[i] = (int)planP[i].n;
        }
    }
    nOut = 0;
    for(i = 0; i < nPlan; i++)
    {
        n = ((shareOf[i] < atOnce[i]) ? shareOf[i] : atOnce[i]);
        reduceGroup(mergedP[i], mergedN[i], n, &grid, &cut, &dropped, &doubled);
        if( atOnce[i] > 1 )
        {
            if( doubled > 0 )
            {
                report("%s: %s: sounds %d at once, into %d part%s: %d cut, %d dropped, %d doubled", fnameP, nameOf[i],
                       atOnce[i], n, ((n == 1) ? "" : "s"), cut, dropped, doubled);
            }
            else
            {
                report("%s: %s: sounds %d at once, into %d part%s: %d cut, %d dropped", fnameP, nameOf[i],
                       atOnce[i], n, ((n == 1) ? "" : "s"), cut, dropped);
            }
        }
        for(j = 0; j < n; j++)
        {
            if( splitOut[j].count > 0 )
            {
                outRuns[nOut] = splitOut[j];
                outGroup[nOut] = songP->groupsP[planP[i].idxP[0]];
                nOut++;
            }
        }
    }
    tempoP = tempoWords(songP, &grid, &bars, fnameP, &nTempo);
    mv.nParts = nOut;
    mv.partsP = growOrFail(NULL, (size_t)nOut, sizeof(MovePart));
    for(c = 0; c < nOut; c++)
    {
        listP = noteList(&outRuns[c], &grid, &bars);
        levelsP = growOrFail(NULL, (size_t)outRuns[c].count, sizeof(int));
        levelAtP = growOrFail(NULL, (size_t)outRuns[c].count, sizeof(int));
        dynUnitP = growOrFail(NULL, (size_t)outRuns[c].count, sizeof(int));
        dynLevelP = growOrFail(NULL, (size_t)outRuns[c].count, sizeof(int));
        nDyn = 0;
        if( (optDynamics != DYN_NONE) && !optPlain )
        {
            noteLevels(&outRuns[c], outGroup[c], levelsP);
            dynamicsOf(levelsP, outRuns[c].count, levelAtP);
            for(i = 0; i < outRuns[c].count; i++)
            {
                if( levelAtP[i] >= 0 )
                {
                    dynUnitP[nDyn] = (int)outRuns[c].notesP[i]->us;
                    dynLevelP[nDyn] = levelAtP[i];
                    nDyn++;
                }
            }
            // A bar over the player's DQROOM keeps its last four.
            for(b = 0; b < bars.nBars; b++)
            {
                inBar = 0;
                for(i = 0; i < nDyn; i++)
                {
                    if( (dynUnitP[i] >= bars.startsP[b]) && (dynUnitP[i] < bars.startsP[b + 1]) )
                    {
                        inBar++;
                    }
                }
                if( inBar > DQROOM )
                {
                    report("%s: part %d, bar %d: %d dynamics, the last %d kept", fnameP, (c + 1), (b + 1), inBar, DQROOM);
                    v = 0;
                    for(i = 0, j = 0; i < nDyn; i++)
                    {
                        if( (dynUnitP[i] >= bars.startsP[b]) && (dynUnitP[i] < bars.startsP[b + 1]) )
                        {
                            v++;
                            if( v <= (inBar - DQROOM) )
                            {
                                continue;
                            }
                        }
                        dynUnitP[j] = dynUnitP[i];
                        dynLevelP[j] = dynLevelP[i];
                        j++;
                    }
                    nDyn = j;
                }
            }
        }
        {
            char where[1100];

            snprintf(where, sizeof(where), "%s: ", fnameP);
            mv.partsP[c].bars = partWords(listP, outRuns[c].count, tempoP, ((c == 0) ? nTempo : 0), &bars,
                                          dynUnitP, dynLevelP, nDyn, where, c);
        }
        mv.partsP[c].groupP = outGroup[c];
        mv.partsP[c].firstTick = outRuns[c].notesP[0]->s;
        free(listP);
        free(levelsP);
        free(levelAtP);
        free(dynUnitP);
        free(dynLevelP);
    }
    mv.restBars = partWords(NULL, 0, NULL, 0, &bars, NULL, NULL, 0, "", 0);
    return mv;
}

// The program.

// Does textP start with prefixP? Returns 1 if so, else 0.
static int startsWith(const char *textP, const char *prefixP)
{
    return (strncmp(textP, prefixP, strlen(prefixP)) == 0);
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

// Read the options and the MIDI files, write the tape, and print the
// reports and the summary. Returns 0 with a tape written, else 1.
int main(int argc, char **argv)
{
    const char **filesP;
    int nFiles;
    int i;
    int f;
    int p;
    int per;
    const char *vP;
    const char *nameP;
    long n;
    Movement *movesP;
    Stored *partsP;
    int nParts;
    long orch[42];
    int hasOrch;
    long *wordsP;
    int nWords;
    int capWords;
    const char *outNameP;
    int below = 0;
    long voices = 0;
    FILE *fP;
    unsigned char *bytesP;
    long nBytes;
    int lacking;

    makeTimes();
    filesP = growOrFail(NULL, (size_t)argc, sizeof(char *));
    nFiles = 0;
    optDynamics = DYN_VELOCITY;
    for(i = 1; i < argc; i++)
    {
        if( strcmp(argv[i], "--played") == 0 )
        {
            optPlayed = 1;
        }
        else if( strcmp(argv[i], "--by-channel") == 0 )
        {
            optByChannel = 1;
        }
        else if( strcmp(argv[i], "--organ") == 0 )
        {
            optOrgan = 1;
        }
        else if( strcmp(argv[i], "--no-share") == 0 )
        {
            optShare = 0;
        }
        else if( strcmp(argv[i], "--plain") == 0 )
        {
            optPlain = 1;
        }
        else if( strcmp(argv[i], "-q") == 0 )
        {
            optQuiet = 1;
        }
        else if( (strcmp(argv[i], "-o") == 0) || startsWith(argv[i], "-o=") )
        {
            optOutP = optionValue(argc, argv, &i, "-o");
        }
        else if( (strcmp(argv[i], "--tempo") == 0) || (strcmp(argv[i], "--transpose") == 0)
                 || startsWith(argv[i], "--tempo=") || startsWith(argv[i], "--transpose=") )
        {
            nameP = (startsWith(argv[i], "--tempo") ? "--tempo" : "--transpose");
            vP = optionValue(argc, argv, &i, nameP);
            if( !isNumber(vP, 1, strlen(vP)) )
            {
                usage("%s %s: not a number", nameP, vP);
            }
            n = atol(vP);
            if( strcmp(nameP, "--tempo") == 0 )
            {
                if( (n < 1) || (n > MAX_TEMPO) )
                {
                    usage("--tempo %s: not 1 to 32767", vP);
                }
                optHasTempo = 1;
                optTempo = n;
            }
            else
            {
                optTranspose = n;
            }
        }
        else if( (strcmp(argv[i], "--parts") == 0) || startsWith(argv[i], "--parts=") )
        {
            optPartsP = optionValue(argc, argv, &i, "--parts");
        }
        else if( startsWith(argv[i], "--dynamics=") )
        {
            vP = (argv[i] + strlen("--dynamics="));
            if( strcmp(vP, "velocity") == 0 )
            {
                optDynamics = DYN_VELOCITY;
            }
            else if( strcmp(vP, "cc7") == 0 )
            {
                optDynamics = DYN_CC7;
            }
            else if( strcmp(vP, "cc11") == 0 )
            {
                optDynamics = DYN_CC11;
            }
            else if( strcmp(vP, "none") == 0 )
            {
                optDynamics = DYN_NONE;
            }
            else
            {
                usage("--dynamics=%s: not velocity, cc7, cc11 or none", vP);
            }
        }
        else if( (strcmp(argv[i], "--orchestra") == 0) || startsWith(argv[i], "--orchestra=") )
        {
            optOrchestraP = optionValue(argc, argv, &i, "--orchestra");
        }
        else if( (argv[i][0] == '-') && (strcmp(argv[i], "-") != 0) )
        {
            usage("%s: no such option", argv[i]);
        }
        else
        {
            filesP[nFiles++] = argv[i];
        }
    }
    if( (nFiles == 0) || (optOutP == NULL) || (optOutP[0] == '\0') )
    {
        usage("a MIDI file and -o TAPE.bin are needed");
    }
    if( optPlain && (optOrchestraP != NULL) && (optOrchestraP[0] != '\0') )
    {
        usage("--plain makes no orchestra block");
    }
    // An empty --parts asks for nothing, so the groups make the parts.
    if( (optPartsP != NULL) && (optPartsP[0] == '\0') )
    {
        optPartsP = NULL;
    }
    movesP = growOrFail(NULL, (size_t)nFiles, sizeof(Movement));
    per = 0;
    for(f = 0; f < nFiles; f++)
    {
        movesP[f] = makeMovement(filesP[f]);
        if( movesP[f].nParts > per )
        {
            per = movesP[f].nParts;
        }
    }
    partsP = growOrFail(NULL, (size_t)(nFiles * NCONV), sizeof(Stored));
    nParts = 0;
    for(f = 0; f < nFiles; f++)
    {
        lacking = (per - movesP[f].nParts);
        if( lacking > 0 )
        {
            report("%s: %d part%s of rests added, for %d parts a movement", baseName(filesP[f]), lacking,
                   ((lacking == 1) ? "" : "s"), per);
        }
        for(p = 0; p < movesP[f].nParts; p++)
        {
            partsP[nParts++] = storeBars(&movesP[f].partsP[p].bars);
        }
        for(p = 0; p < lacking; p++)
        {
            partsP[nParts++] = storeBars(&movesP[f].restBars);
        }
    }
    if( nParts > MAXP )
    {
        refuse("%d parts, more than the %d a tape holds", nParts, MAXP);
    }
    hasOrch = 0;
    if( (optOrchestraP != NULL) && (strcmp(optOrchestraP, "gm") == 0) )
    {
        orchestraGm(per, &movesP[0], baseName(filesP[0]), orch);
        hasOrch = 1;
    }
    else if( (optOrchestraP != NULL) && (optOrchestraP[0] != '\0') )
    {
        orchestraFile(optOrchestraP, per, orch);
        hasOrch = 1;
    }
    wordsP = NULL;
    nWords = 0;
    capWords = 0;
    for(p = 0; p < nParts; p++)
    {
        addWord(&wordsP, &nWords, &capWords, partsP[p].nNotes);
        for(i = 0; i < partsP[p].nNotes; i++)
        {
            addWord(&wordsP, &nWords, &capWords, partsP[p].notesP[i]);
        }
        addWord(&wordsP, &nWords, &capWords, checksum(partsP[p].notesP, partsP[p].nNotes));
        addWord(&wordsP, &nWords, &capWords, partsP[p].nPtr);
        for(i = 0; i < partsP[p].nPtr; i++)
        {
            addWord(&wordsP, &nWords, &capWords, partsP[p].ptrP[i]);
        }
        addWord(&wordsP, &nWords, &capWords, checksum(partsP[p].ptrP, partsP[p].nPtr));
    }
    if( hasOrch )
    {
        for(i = 0; i < 42; i++)
        {
            addWord(&wordsP, &nWords, &capWords, orch[i]);
        }
    }
    outNameP = baseName(optOutP);
    if( optPlain )
    {
        if( !fitPlain(partsP, nParts) )
        {
            refuse("%d words: too large for the 1.0 player (--plain)", nWords);
        }
    }
    else if( !fitNew(partsP, nParts, hasOrch, &below, &voices) )
    {
        refuse("%d words: too large for the player's core", nWords);
    }
    if( !optQuiet )
    {
        for(i = 0; i < nReport; i++)
        {
            printf("%s\n", reportP[i]);
        }
    }
    nBytes = ((2 * LEADER) + (3 * (long)nWords));
    bytesP = growOrFail(NULL, (size_t)nBytes, 1);
    memset(bytesP, 0, (size_t)nBytes);
    for(i = 0; i < nWords; i++)
    {
        bytesP[LEADER + (3 * i)] = (unsigned char)(0200 | ((wordsP[i] >> 12) & 077));
        bytesP[LEADER + (3 * i) + 1] = (unsigned char)(0200 | ((wordsP[i] >> 6) & 077));
        bytesP[LEADER + (3 * i) + 2] = (unsigned char)(0200 | (wordsP[i] & 077));
    }
    if( !(fP = fopen(optOutP, "wb")) || (fwrite(bytesP, 1, (size_t)nBytes, fP) != (size_t)nBytes) )
    {
        printf("midi2dac: cannot write %s\n", optOutP);
        if( fP != NULL )
        {
            fclose(fP);
        }
        return 1;
    }
    if( fclose(fP) != 0 )
    {
        printf("midi2dac: cannot write %s\n", optOutP);
        return 1;
    }
    printf("%s: %d part%s, %d movement%s of %d, %d words", outNameP, nParts, ((nParts == 1) ? "" : "s"), nFiles,
           ((nFiles == 1) ? "" : "s"), per, nWords);
    if( optPlain )
    {
        printf(": plain, for pdp1m13 and the 1.0 player\n");
    }
    else
    {
        printf(": %s, %ld words for the voices\n", (below ? "below TEND" : "runs on from 050000"), voices);
    }
    return 0;
}
