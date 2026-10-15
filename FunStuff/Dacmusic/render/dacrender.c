// dacrender: plays a tape punched by the D/A converters' music program
// (dacmusic.am1's PUNCH: Samson's Harmony Compiler parts, then the
// orchestra block with each converter's part, voice, octave, volume and
// place) into a wav file, as the player and the device would play it,
// with no PDP-1: 16-bit stereo at 50 kHz, the device's own frame.
//
// It rebuilds the player and the device, not the emulator: the tape as
// READ TAPE takes it, the timeline as pdp1m13 times a score, each
// converter's sequencer as the player latches its entries, and each
// converter one machine cycle at a time as IOT_71.c computes it, with the
// same integer arithmetic.
//
// The instruments are voices, as a voice file holds them
// (../voices/README.md). The renderer places the voices the orchestra
// plays in the converters' memory itself, as the player's loader does;
// where a block lands does not reach the output, only which pitches share
// one. A converter's organ stops are premixed as the player premixes
// them. The nine standard voices are compiled in from dacrender_lib.h;
// --voices=file.lib plays a library of voice files instead.
//
// The device's room (room.h), onset, voicing and céleste (edge.h), and
// its converter for recorded instruments (version 5 voice files) are
// played as the device plays them; README.md here and Implementation.md,
// "The device", describe them.
//
// Needs the C99 library and libm only; the device's gain table is
// compiled in too. One thread: the tape is read, the whole piece
// sequenced into each converter's entries, then the converters run
// cycle by cycle and each 4-cycle frame is mixed and written as soon as
// it is made.
//
// Usage: dacrender tape.bin [out.wav] [--secs=S] [--voices=file.lib] [--room=none|hall|busch|voices] [--onset=MS]
// A flag left out takes the tape's room or onset, if its orchestra block (version 3) sets it.
//
// 07-Oct-2026 Claude: written.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dacrender_lib.h"
#include "room.h"
#include "edge.h"

// The machine and the output.
#define CPS 200000                  // machine cycles a second
#define CYCLES_PER_FRAME 4          // the device's output frame: 50 kHz
#define RATE (CPS / CYCLES_PER_FRAME)
#define NCONV 4
#define WAV_FRAMES 4096             // frames buffered before each write
#define CYCLES_PER_MS 200           // the onset's bound
#define ONSET_SEED 2654435769u      // a converter's sequence starts at its number + 1 times this

// The tape, as the player reads it.
#define WORD_MASK 0777777
#define SIGN_BIT 0400000
#define BAR 0600000                 // a bar line in a part's notes
#define TEMPO_MARK 0700000          // a tempo word: these bits set
#define ORCH_COUNT 0700021          // the orchestra block's count: a mark, and its 17 words
#define ORCH_COUNT0 0700015         // the block from before the volumes: its 13 words
#define ORCH_COUNT2 0700046         // version 2 (README.md): its 38 words
#define ORCH_WORDS 17
#define ORCH_WORDS0 13
#define ORCH_WORDS2 38
#define ORCH_VERSION2 2
#define ORCH_COUNT3 0700050         // version 3: version 2's words, then the room and the onset
#define ORCH_WORDS3 40
#define ORCH_VERSION3 3
#define SETTING_CONFIGURED 0400000  // a block's room or onset: the configuration's (here, the flags')
#define ROOM_CODES 4                // a block's room: 0 none, 1 hall, 2 Busch Hall, 3 the voices'
#define ONSET_UNIT 25               // cycles in a block's onset unit, 1/8 ms
#define LINE2 9                     // a converter's words in version 2
#define STOPS 4
#define PLACE_RIGHT 0400            // a place in the stereo field: 0 left, 0400 right
#define PLACE_STEP 040              // the player's nine places are 040 apart
#define NPLACES 9
#define TAPE_START 020000           // the player keeps the parts in banks 2-3
#define TAPE_END 037300             // to its TEND, below PUNCH
#define PART_HEADER 4               // words the player keeps before each part
#define MAX_PARTS 30
#define NOCT 5                      // the octaves an orchestra block can name
#define VSTEP 24                    // 1/8 dB between a slider's levels
#define VLEVELS 16
#define DEFAULT_PER 3

// The timing (pdp1m13's) and the sequencer's limits.
#define DEFAULT_TEMPO 170
#define PAUSE 400000                // cycles between movements, 2 s
#define MIN_GAP 400                 // cycles: a key off leaving a shorter gap is dropped
#define MAX_EVENT 0377777           // cycles: a length fits a positive word
#define SPLIT 0200000               // a longer entry is latched in pieces of this length
#define PITCH0 48                   // the pitch whose table is latched before the first entry
#define TABLE_LENGTH 256

// The device (IOT 71 and 72).
#define FLOOR 0xFFFFFF              // the attenuation's largest: OFF
#define LEVEL_NONE 0777             // a sustain of none: the decay runs on to the floor
#define MAXLEN 262144               // an event length of 0
#define PHASE_MASK 0xFFFFFFFUL      // 16 integer and 12 fraction bits
#define NO_THRESHOLD 0777
#define SILENT 4096                 // a gain index from here up is silence
#define MUTE 0777                   // a volume of mute
#define ACT_WAVE 1
#define ACT_KEYON 2
#define ACT_KEYOFF 4
#define ACT_PHASE 8
#define ACT_SAMPLE 16
#define ACT_INTERP 32               // dyx's 040: the interpolation, held
#define ACT_PACK 64                 // dyx's 0100: the packing, held (sample mode only)
#define ACT_HELD (ACT_WAVE | ACT_SAMPLE)
#define ACT_KEPT (ACT_WAVE | ACT_SAMPLE | ACT_INTERP | ACT_PACK) // the actions a note leaves latched
#define ACT_MASK 0177               // dyx's actions
#define PACKED_END_MAX 21845        // a packed end, in groups: 65,535 samples
#define NO_SUSTAIN (LEVEL_NONE << 9)
#define MEM_WORDS 0200000
// Where the voices' blocks are placed from: the player's banks 5-15, from
// 050000 to the end of memory, as the player places them.
#define PLACE_START 050000
// A voice whose notes latch each block's own end, loop and stride has
// each block after those three words, as the player's loader puts them.
#define OWN_PREFIX 3

// The voicing (edge.h) and the room it chooses.
#define ROOM_VOICES (-1)            // --room=voices: Busch Hall if a converter's voicing asks for it
#define EDGE_UNIT_D (1.0 / (double)(1 << EDGE_OUT_SHIFT))
#define EDGE_LOWEST (-34)           // a voice file's edge levels, dB, even
#define EDGE_LOUDEST (-22)
#define RISE_PERIODS 10             // the one rise the speech holds

// The converter for recorded instruments (IOT 72's commands 011-016).
#define SLOW_TICK 64                // the slow counter clocks at every 64th cycle from the go
#define SLOW_PHASE 0xFFFFFFL        // its phase, 24 bits
#define SLOW_HOLD 16                // the delay's unit, in clocks
#define SLOW_QUARTERS 0xC00000L     // t2 is read three quarters on from t1
#define PERC_MASK 0377              // the percussion's sine, 256 words
#define PERC_PHASE 0xFFFFFUL        // its phase, 8 integer and 12 fraction bits
#define FRACTION 07777              // a phase's 12 fraction bits
#define RATE_MAX 0777777            // the slow counter's rate, the percussion's decay
#define LEVEL_MAX 0777              // a depth or a level
#define DELAY_MAX 0377              // the vibrato's delay, in 16 clocks
#define K_MAX 7                     // the percussion's multiple
#define PERC_WORDS 256              // the percussion's sine: the device reads 256 words
#define ADDR_MAX 0177777            // an end, a loop or a stride

// Voice files.
#define VOICE_VERSION 5             // the newest; 1 has no stops, 2 no voicing, 3 no céleste, 4 none of 5's
#define NZONES 6                    // a stop's zones, an octave each from pitch 2
#define MAX_ZONES (NZONES + 1)      // and zone 6, a version 5 premix's seventh block
#define ZONE_PITCHES 12
#define ZONE_TOP 73                 // B6: the highest an octave up sounds in a voice without zone 6
#define ZONE6_TOP 75                // C#7: the highest with zone 6
#define FIRST_FIVE 5                // the voice file version that makes a library a version 5 drum
#define MAX_LINE 4096
#define MAX_VOICES 512

enum { STAGE_OFF, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };
enum { EV_STRIKE, EV_SLUR, EV_OFF, EV_MARK };
enum { READ_OK, READ_END, READ_LARGE };
enum { READ_FIRST, READ_SECOND, READ_PERC };

typedef struct
{
    int *notesP;
    int nNotes;
    int *barsP;
    int nBars;
} Part;

// A converter's line of the orchestra: its part in each movement (-1:
// none), its instrument (its first stop), its stops, and its octave's
// number.
typedef struct
{
    int part;
    int inst;
    int stops[STOPS];
    int nStops;
    int octave;
} OrchLine;

typedef struct
{
    int *wordsP;
    long nWords;
    long pos;
    long fp;                        // the player's next word of banks 2-3
} TapeReader;

// One entry of a converter's ring: its length in cycles, and the table,
// step and dyx value it latches, 0 where it latches nothing.
typedef struct
{
    long len;
    long dxa;
    long dyf;
    long dyx;
} Entry;

// A voice, as its file holds it: its blocks are lengthsP[0..nBlocks-1]
// words, one after another in wordsP; ownP holds each block's own end,
// loop and stride, three a block, the end -1 for a block with none (NULL:
// none has).
typedef struct
{
    const char *nameP;
    int n;
    int sample;
    int stop;                       // a stop: played as the player's premix of it
    int speech[MAX_ZONES];          // a stop's speech block for each zone, zone 6's the sixth's without it
    int zone6;                      // zone 6's block; -1: none
    int version;                    // the file's
    long start, peak, attack, decay, sustain, release;
    long stride;
    long thresholds[3];
    long end, loop;
    long steps[LIB_PITCHES];
    int zones[LIB_PITCHES];
    int nBlocks;
    const int *lengthsP;
    const unsigned long *wordsP;
    const long *ownP;
    int voicing;                    // dzv's low bits: the edge's level, the speech, Busch Hall
    int interp;                     // version 5: the interpolation
    int pack;                       // the packing: end, loop and stride in groups
    long vRate, vDepth, vDelay;     // the slow counter's rate, the vibrato's depth and delay
    int vFree;                      // free running
    long lesA, lesP;                // the Leslie's tremolo and place depths
    long pLevel, pK, pDecay;        // the percussion: its level, multiple (0: none) and decay
    int pBlock;                     // its sine's block
} Voice;

// A voice placed in memory: what the sequencer and the converter's
// constants need of it, its zones now addresses, zone 6's in zone6 (0:
// none). With own, each pitch's block has its own end, loop and stride
// (zoneEnd, zoneLoop, zoneStride; zone 6's zone6End, zone6Loop,
// zone6Stride), latched with its table.
typedef struct
{
    const char *nameP;
    int n;
    long zoneMap[LIB_PITCHES];
    long zone6;
    long zone6End, zone6Loop, zone6Stride;
    long steps[LIB_PITCHES];
    long attack, decay, release, start, peak, sustain, held, end, loop, stride;
    long thresholds[3];
    int voicing;
    int hasLatches;                 // version 5's six are given
    long dyv, dyw, dym, dyk, dyj, dyp;
    int own;
    long zoneEnd[LIB_PITCHES];
    long zoneLoop[LIB_PITCHES];
    long zoneStride[LIB_PITCHES];
} Instrument;

typedef struct
{
    int part;
    const Instrument *instP;
    int octave;                     // the shift: -2 to 2
    int volume;                     // 1/8 dB
    long sus;
    long held;
    Entry *entriesP;
    long nEntries;
    long capEntries;
    long pend[3];
    long long last;
    int sounding;
    int pitch;
} Sequencer;

// What an event's end loads besides the event's length: the note.
typedef struct
{
    long addr, length, period, step, attack, decay, release;
    long start, peak, sustain, actions, end, loop, stride, th[3];
    int voicing;
    long vRate, vDepth, vDelay, vFree, tDepth, pDepth, kMul, kLevel, kDecay, kAddr;
} Note;

// The pair of words a read holds with the interpolation, by address, and
// their samples; n of them (0: none yet).
typedef struct
{
    long addr[2];
    long value[2];
    int n;
} Pair;

// One converter: the latches the program writes, the values loaded from
// them at an event's end, the envelope, the clocks, the onset's note,
// its cycles to wait (0: none) and its sequence, and the voicing's state
// and its cycles since the go.
typedef struct
{
    long lAddr, lLength, lPeriod, lStep, lEvlen, lAttack, lDecay, lRelease;
    long lStart, lPeak, lSustain, lActions, lEnd, lLoop, lStride, lTh[3];
    int lVoicing;
    long lVRate, lVDepth, lVDelay, lVFree, lTDepth, lPDepth, lKMul, lKLevel, lKDecay, lKAddr;
    EdgeState edge;
    long ticks;
    Note held;
    long onsetLeft;
    uint32_t onsetRandom;
    int sample;
    long addr, mask, period, step, end, loop, stride, th[3];
    long att, start, peak, sustain, attack, decay, release;
    int stage;
    long vol, volTarget;
    int going;
    unsigned long phase;
    long evLeft, countdown;
    double visible, pending;
    int pendingValid;
    // The céleste: its detune k (0: none), the second phase and its step,
    // whether this sample time is the odd one, each read's word (visible
    // is the two mixed, or the first alone), and the second's to come.
    int celeste;
    unsigned long phase2;
    long step2;
    int odd;
    double first, second, pending2;
    int pending2Valid;
    // Recorded instruments: the interpolation; the slow counter's phase,
    // its delay left and its latches, and the terms formed from it (the
    // vibrato's m, the steps taken, the tremolo, the place's offsets); the
    // percussion's latches, phase, attenuation (FLOOR: none) and word.
    // pairs: each read's held pair, the first's, the céleste's and the
    // percussion's. packed: the first read's and the céleste's from packed
    // words (sample mode only).
    int interp;
    int packed;
    Pair pairs[3];
    long lfo, hold;
    long vRate, vDepth, vDelay, tDepth, pDepth;
    int vFree;
    long long m;
    long trem, panL, panR, stepM, step2M;
    long kMul, kLevel, kDecay, kAddr;
    unsigned long kPhase;
    long kAtt;
    double kValue, pendingK;
    int pendingKValid;
    const Sequencer *seqP;
    long next;                      // the next entry of the stream
} Converter;

typedef struct
{
    long long t16;
    int kind;
    int pitch;
} Event;

typedef struct
{
    Event *evP;
    long n;
    long cap;
} EventList;

typedef struct
{
    int v;
    long o;
    long u;
    int art;
    int pitch;
} BarNote;

typedef struct
{
    long o;
    int v;
    int tempo;
} Change;

typedef struct
{
    long o;
    int tempo;
} Seg;

static Part parts[MAX_PARTS];
static int nParts;
static int per;
static OrchLine orch[NCONV];
static int volumes[NCONV];
static int places[NCONV];           // each converter's place, as the orchestra block holds it
static long placeAtts[NCONV][2];    // its mixer attenuations, 1/64 dB, into the left and right channels
static long memWords[MEM_WORDS];    // -1: no voice's word there
static Voice voices[MAX_VOICES];
static int nVoices;
static int libraryFive;             // a voice file of version 5: the player's drum is version 5
static Instrument placed[NCONV];
static long placeNext;
static Sequencer seqs[NCONV];
static Converter convs[NCONV];
static long long unmappedFetches;
static long stepsBeyond;
static long articulationsHalted;
static Room room;                   // after the mix; its kind ROOM_OFF unless --room= or the tape
static long onsetCycles;            // the onset's bound in cycles; 0: off, unless --onset= or the tape
static int tapeRoom = SETTING_CONFIGURED;   // the orchestra block's room, as the player takes it
static int tapeOnset = SETTING_CONFIGURED;  // and its onset, in 1/8 ms
static const int octaveShifts[NOCT] = { 0, -1, 1, -2, 2 };
static const char *octaveFeet[NOCT] = { "8", "16", "4", "32", "2" };
// The player's places, left to right (menus.ac's plv is k x PLACE_STEP), each
// one's dzp word (its plp: the left's attenuation in bits 0-8 and the right's
// in 9-17, 1/8 dB, MUTE for off, on a constant-power law) and its name.
static const int placeWords[NPLACES] =
{
    0000777, 0001162, 0006103, 0015051, 0030030, 0051015, 0103006, 0162001, 0777000
};
static const char *placeNames[NPLACES] = { "LEFT", "L3", "L2", "L1", "CENTER", "R1", "R2", "R3", "RIGHT" };

// fail: prints the message and exits with status 1.
static void fail(const char *msgP)
{
    fprintf(stderr, "dacrender: %s\n", msgP);
    exit(1);
}

// growOrFail: realloc that exits on failure.
// Returns the new block.
static void *growOrFail(void *ptrP, size_t bytes)
{
    void *newP;

    newP = realloc(ptrP, bytes);
    if( !newP )
    {
        fail("out of memory");
    }
    return newP;
}

// instByName: the library's voice of that name.
// Returns its number, or -1 if the library has none.
static int instByName(const char *nameP)
{
    int k;

    for(k = 0; k < nVoices; k++)
    {
        if( strcmp(voices[k].nameP, nameP) == 0 )
        {
            return k;
        }
    }
    return -1;
}

// The voices.

// compiledVoices: the nine compiled in, as voices.
static void compiledVoices(void)
{
    const LibVoice *lP;
    Voice *vP;
    int k;
    int p;

    libraryFive = 0;
    for(k = 0; k < LIB_VOICES; k++)
    {
        lP = &libVoices[k];
        vP = &voices[k];
        vP->nameP = lP->nameP;
        vP->n = lP->n;
        vP->sample = lP->sample;
        vP->start = lP->start;
        vP->peak = lP->peak;
        vP->attack = lP->attack;
        vP->decay = lP->decay;
        vP->sustain = lP->sustain;
        vP->release = lP->release;
        vP->stride = lP->stride;
        vP->thresholds[0] = lP->thresholds[0];
        vP->thresholds[1] = lP->thresholds[1];
        vP->thresholds[2] = lP->thresholds[2];
        vP->end = lP->end;
        vP->loop = lP->loop;
        for(p = 0; p < LIB_PITCHES; p++)
        {
            vP->steps[p] = lP->steps[p];
            vP->zones[p] = lP->zones[p];
        }
        vP->nBlocks = lP->nBlocks;
        vP->lengthsP = lP->lengthsP;
        vP->wordsP = lP->wordsP;
        vP->voicing = 0;            // the nine have none
        vP->zone6 = lP->zone6;
        vP->version = lP->version;
        libraryFive |= (vP->version >= FIRST_FIVE);
    }
    nVoices = LIB_VOICES;
}

// voiceFail: a voice file's error, at its line, and exit 1.
static void voiceFail(const char *pathP, int line, const char *msgP)
{
    if( line > 0 )
    {
        fprintf(stderr, "dacrender: %s:%d: %s\n", pathP, line, msgP);
    }
    else
    {
        fprintf(stderr, "dacrender: %s: %s\n", pathP, msgP);
    }
    exit(1);
}

// strdupOrFail: a new copy of the string.
static char *strdupOrFail(const char *sP)
{
    char *dP;

    dP = growOrFail(NULL, strlen(sP) + 1);
    strcpy(dP, sP);
    return dP;
}

// voiceNumbers: the line's numbers from pP on into valsP (base 10, or 8
// for a block's words), at most max.
// Returns how many, or -1 for a token that is no number.
static int voiceNumbers(char *pP, long *valsP, int max, int base)
{
    char *tokP;
    char *endP;
    int n;

    n = 0;
    for(tokP = strtok(pP, " \t\r\n"); tokP != NULL; tokP = strtok(NULL, " \t\r\n"))
    {
        if( n == max )
        {
            return (max + 1);
        }
        valsP[n] = strtol(tokP, &endP, base);
        if( (*endP != '\0') || (tokP[0] == '-') || (tokP[0] == '+') )
        {
            return -1;
        }
        n++;
    }
    return n;
}

// celesteOf: the céleste's detune k in half cents from the line's text at
// pP: cents from 0.5 to 7.5 in half cents (2, 2.5, 2.50), nothing after.
// Returns k, 1 to 15, or 0 for anything else.
static int celesteOf(const char *pP)
{
    long whole;
    int half;
    int digits;

    while( (*pP == ' ') || (*pP == '\t') )
    {
        pP++;
    }
    whole = 0;
    digits = 0;
    while( (*pP >= '0') && (*pP <= '9') && (whole < 100) )
    {
        whole = ((whole * 10) + (*pP++ - '0'));
        digits++;
    }
    half = 0;
    if( *pP == '.' )
    {
        pP++;
        if( (*pP == '5') || (*pP == '0') )
        {
            half = (*pP++ == '5');
            digits++;
        }
        else
        {
            return 0;
        }
        while( *pP == '0' )
        {
            pP++;
        }
    }
    while( (*pP == ' ') || (*pP == '\t') || (*pP == '\r') || (*pP == '\n') )
    {
        pP++;
    }
    if( (digits == 0) || (*pP != '\0') || (((whole * 2) + half) < 1) || (((whole * 2) + half) > 15) )
    {
        return 0;
    }
    return (int)((whole * 2) + half);
}

// wholeOf: the token at tokP as a number of decimal digits alone, into *outP.
// Returns 1, or 0 if it is not one.
static int wholeOf(const char *tokP, long *outP)
{
    char *endP;

    if( (tokP[0] < '0') || (tokP[0] > '9') )
    {
        return 0;
    }
    *outP = strtol(tokP, &endP, 10);
    return (*endP == '\0');
}

// readVoice: the voice file at pathP into *vP (README.md, "Voice files");
// a malformed file, or one whose blocks are shorter than the voice
// plays, ends the run.
static void readVoice(const char *pathP, Voice *vP)
{
    enum { F_NONE, F_STEPS, F_ZONES, F_SPEECH, F_BLOCK };
    FILE *fP;
    char line[MAX_LINE];
    char word[32];
    char tok[3][32];
    char *pP;
    char *hashP;
    char *endP;
    char extra;
    long vals[MAX_LINE / 2];
    long db;
    unsigned long *wordsP;
    int *lengthsP;
    long *ownP;
    long nWords;
    long capWords;
    long blockFill;
    int field;
    int nSteps;
    int nZones;
    int nSpeech;
    int version;
    int seenMode;
    int seenEnvelope;
    int seenStride;
    int seenThresholds;
    int seenSample;
    int seenVersion;
    int seenFamily;
    int seenEdge;
    int seenRise;
    int seenRoom;
    int seenCeleste;
    int seenInterp;
    int seenPack;
    int seenVibrato;
    int seenLeslie;
    int seenPercussion;
    int lineNo;
    int n;
    int i;
    int b;
    int need;
    int timbres;
    int k;

    memset(vP, 0, sizeof(*vP));
    vP->zone6 = -1;
    if( !(fP = fopen(pathP, "r")) )
    {
        voiceFail(pathP, 0, "cannot read the voice file");
    }
    wordsP = NULL;
    lengthsP = NULL;
    ownP = NULL;
    nWords = 0;
    capWords = 0;
    blockFill = 0;
    field = F_NONE;
    nSteps = 0;
    nZones = 0;
    nSpeech = 0;
    version = 0;
    seenMode = seenEnvelope = seenStride = seenThresholds = seenSample = seenVersion = seenFamily = 0;
    seenEdge = seenRise = seenRoom = seenCeleste = 0;
    seenInterp = seenPack = seenVibrato = seenLeslie = seenPercussion = 0;
    vP->n = -1;
    lineNo = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        lineNo++;
        if( (strchr(line, '\n') == NULL) && !feof(fP) )
        {
            voiceFail(pathP, lineNo, "a line too long");
        }
        if( (hashP = strchr(line, '#')) != NULL )
        {
            *hashP = '\0';
        }
        for(pP = line; (*pP == ' ') || (*pP == '\t'); pP++)
        {
        }
        if( (*pP == '\0') || (*pP == '\n') || (*pP == '\r') )
        {
            continue;
        }
        // A line that starts with a number continues the field above it.
        if( (*pP >= '0') && (*pP <= '9') )
        {
            if( field == F_STEPS )
            {
                n = voiceNumbers(pP, vals, (LIB_PITCHES - nSteps), 10);
                if( (n < 0) || (n > (LIB_PITCHES - nSteps)) )
                {
                    voiceFail(pathP, lineNo, "steps: 64 numbers");
                }
                for(i = 0; i < n; i++)
                {
                    vP->steps[nSteps + i] = vals[i];
                }
                nSteps += n;
            }
            else if( field == F_ZONES )
            {
                n = voiceNumbers(pP, vals, (LIB_PITCHES - nZones), 10);
                if( (n < 0) || (n > (LIB_PITCHES - nZones)) )
                {
                    voiceFail(pathP, lineNo, "zones: 64 numbers");
                }
                for(i = 0; i < n; i++)
                {
                    if( (vals[i] < 0) || (vals[i] > 255) )
                    {
                        voiceFail(pathP, lineNo, "a zone's block out of range");
                    }
                    vP->zones[nZones + i] = (int)vals[i];
                }
                nZones += n;
            }
            else if( field == F_SPEECH )
            {
                n = voiceNumbers(pP, vals, (MAX_ZONES - nSpeech), 10);
                if( (n < 0) || (n > (MAX_ZONES - nSpeech)) )
                {
                    voiceFail(pathP, lineNo, "speech: 6 numbers, or 7 with zone 6");
                }
                for(i = 0; i < n; i++)
                {
                    if( (vals[i] < 0) || (vals[i] > 255) )
                    {
                        voiceFail(pathP, lineNo, "a speech block out of range");
                    }
                    vP->speech[nSpeech + i] = (int)vals[i];
                }
                nSpeech += n;
            }
            else if( field == F_BLOCK )
            {
                n = voiceNumbers(pP, vals, (int)(lengthsP[vP->nBlocks - 1] - blockFill), 8);
                if( (n < 0) || (n > (lengthsP[vP->nBlocks - 1] - blockFill)) )
                {
                    voiceFail(pathP, lineNo, "a block's words: octal, as many as its length");
                }
                for(i = 0; i < n; i++)
                {
                    if( vals[i] > WORD_MASK )
                    {
                        voiceFail(pathP, lineNo, "a word beyond 18 bits");
                    }
                    wordsP[nWords] = (unsigned long)vals[i];
                    nWords++;
                }
                blockFill += n;
            }
            else
            {
                voiceFail(pathP, lineNo, "numbers with no field above them");
            }
            continue;
        }
        if( (field == F_BLOCK) && (blockFill != lengthsP[vP->nBlocks - 1]) )
        {
            voiceFail(pathP, lineNo, "a block has fewer words than its length");
        }
        field = F_NONE;
        if( sscanf(pP, "%31s", word) != 1 )
        {
            voiceFail(pathP, lineNo, "no field");
        }
        pP += strlen(word);
        if( !seenVersion )
        {
            if( (strcmp(word, "dacvoice") != 0) || (voiceNumbers(pP, vals, 1, 10) != 1) || (vals[0] < 1)
                || (vals[0] > VOICE_VERSION) )
            {
                voiceFail(pathP, lineNo, "not a dacvoice 1 to 5 file");
            }
            version = (int)vals[0];
            seenVersion = 1;
        }
        else if( (strcmp(word, "name") == 0) || (strcmp(word, "family") == 0) )
        {
            // The rest of the line, trimmed.
            while( (*pP == ' ') || (*pP == '\t') )
            {
                pP++;
            }
            n = (int)strlen(pP);
            while( (n > 0) && ((pP[n - 1] == '\n') || (pP[n - 1] == '\r') || (pP[n - 1] == ' ')
                   || (pP[n - 1] == '\t')) )
            {
                n--;
            }
            pP[n] = '\0';
            if( word[0] == 'n' )
            {
                if( (vP->nameP != NULL) || (n == 0) )
                {
                    voiceFail(pathP, lineNo, "one name, not empty");
                }
                vP->nameP = strdupOrFail(pP);
            }
            else
            {
                // The menu's family: checked, and not kept, since nothing
                // here plays by it.
                if( seenFamily || (n == 0) )
                {
                    voiceFail(pathP, lineNo, "one family, not empty");
                }
                seenFamily = 1;
            }
        }
        else if( strcmp(word, "n") == 0 )
        {
            if( (vP->n >= 0) || (voiceNumbers(pP, vals, 1, 10) != 1) || (vals[0] < 1) || (vals[0] > 4095) )
            {
                voiceFail(pathP, lineNo, "n: one sample period, 1 to 4095");
            }
            vP->n = (int)vals[0];
        }
        else if( strcmp(word, "mode") == 0 )
        {
            if( seenMode || (sscanf(pP, "%31s", word) != 1)
                || ((strcmp(word, "table") != 0) && (strcmp(word, "sample") != 0)
                    && ((strcmp(word, "stop") != 0) || (version < 2))) )
            {
                voiceFail(pathP, lineNo, "mode: table or sample, or in version 2 stop");
            }
            vP->sample = (strcmp(word, "sample") == 0);
            vP->stop = (strcmp(word, "stop") == 0);
            seenMode = 1;
        }
        else if( strcmp(word, "envelope") == 0 )
        {
            if( seenEnvelope || (voiceNumbers(pP, vals, 6, 10) != 6) || (vals[0] > 0777) || (vals[1] > 0777)
                || (vals[4] > 0777) || (vals[2] > WORD_MASK) || (vals[3] > WORD_MASK) || (vals[5] > WORD_MASK) )
            {
                voiceFail(pathP, lineNo, "envelope: start, peak, attack, decay, sustain, release");
            }
            vP->start = vals[0];
            vP->peak = vals[1];
            vP->attack = vals[2];
            vP->decay = vals[3];
            vP->sustain = vals[4];
            vP->release = vals[5];
            seenEnvelope = 1;
        }
        else if( strcmp(word, "stride") == 0 )
        {
            if( seenStride || (voiceNumbers(pP, vals, 1, 10) != 1) || (vals[0] > 0177777) )
            {
                voiceFail(pathP, lineNo, "stride: one number");
            }
            vP->stride = vals[0];
            seenStride = 1;
        }
        else if( strcmp(word, "thresholds") == 0 )
        {
            if( seenThresholds || (voiceNumbers(pP, vals, 3, 10) != 3) || (vals[0] > 0777) || (vals[1] > 0777)
                || (vals[2] > 0777) )
            {
                voiceFail(pathP, lineNo, "thresholds: three levels, 511 for none");
            }
            vP->thresholds[0] = vals[0];
            vP->thresholds[1] = vals[1];
            vP->thresholds[2] = vals[2];
            seenThresholds = 1;
        }
        else if( strcmp(word, "sample") == 0 )
        {
            if( seenSample || (voiceNumbers(pP, vals, 2, 10) != 2) || (vals[0] < 1) || (vals[0] > 0177777)
                || (vals[1] > vals[0]) )
            {
                voiceFail(pathP, lineNo, "sample: the end and the loop, in words");
            }
            vP->end = vals[0];
            vP->loop = vals[1];
            seenSample = 1;
        }
        else if( strcmp(word, "steps") == 0 )
        {
            if( nSteps > 0 )
            {
                voiceFail(pathP, lineNo, "steps again");
            }
            field = F_STEPS;
            n = voiceNumbers(pP, vals, LIB_PITCHES, 10);
            if( n < 0 )
            {
                voiceFail(pathP, lineNo, "steps: 64 numbers");
            }
            for(i = 0; i < n; i++)
            {
                vP->steps[i] = vals[i];
            }
            nSteps = n;
        }
        else if( strcmp(word, "zones") == 0 )
        {
            if( nZones > 0 )
            {
                voiceFail(pathP, lineNo, "zones again");
            }
            field = F_ZONES;
            n = voiceNumbers(pP, vals, LIB_PITCHES, 10);
            if( n < 0 )
            {
                voiceFail(pathP, lineNo, "zones: 64 numbers");
            }
            for(i = 0; i < n; i++)
            {
                vP->zones[i] = (int)vals[i];
            }
            nZones = n;
        }
        else if( (strcmp(word, "speech") == 0) && (version >= 2) )
        {
            if( nSpeech > 0 )
            {
                voiceFail(pathP, lineNo, "speech again");
            }
            field = F_SPEECH;
            n = voiceNumbers(pP, vals, MAX_ZONES, 10);
            if( n < 0 )
            {
                voiceFail(pathP, lineNo, "speech: 6 numbers, or 7 with zone 6");
            }
            for(i = 0; i < n; i++)
            {
                vP->speech[i] = (int)vals[i];
            }
            nSpeech = n;
        }
        else if( (strcmp(word, "zone6") == 0) && (version >= 5) )
        {
            // zone6 B: zone 6's block, which only an octave up plays.
            if( (vP->zone6 >= 0) || (voiceNumbers(pP, vals, 1, 10) != 1) || (vals[0] > 255) )
            {
                voiceFail(pathP, lineNo, "zone6: one block number, once");
            }
            vP->zone6 = (int)vals[0];
        }
        else if( (strcmp(word, "edge") == 0) && (version >= 3) )
        {
            // edge DB: the edge tone's level k, DB = -36 + 2k (a level is negative, so it is
            // read here, not by voiceNumbers()).
            db = strtol(pP, &endP, 10);
            while( (*endP == ' ') || (*endP == '\t') || (*endP == '\r') || (*endP == '\n') )
            {
                endP++;
            }
            if( seenEdge || (endP == pP) || (*endP != '\0') || ((db % 2) != 0) || (db < EDGE_LOWEST)
                || (db > EDGE_LOUDEST) )
            {
                voiceFail(pathP, lineNo, "edge: an even level from -34 to -22 dB");
            }
            vP->voicing |= (int)((db - EDGE_DB_BASE) / 2);
            seenEdge = 1;
        }
        else if( (strcmp(word, "rise") == 0) && (version >= 3) )
        {
            if( seenRise || (voiceNumbers(pP, vals, 1, 10) != 1) || (vals[0] != RISE_PERIODS) )
            {
                voiceFail(pathP, lineNo, "rise: the rise is 10 periods");
            }
            vP->voicing |= EDGE_RISE;
            seenRise = 1;
        }
        else if( (strcmp(word, "room") == 0) && (version >= 3) )
        {
            if( seenRoom || (sscanf(pP, "%31s %c", word, &extra) != 1)
                || ((strcmp(word, "busch") != 0) && (strcmp(word, "hall") != 0)) )
            {
                voiceFail(pathP, lineNo, "room: busch or hall");
            }
            if( strcmp(word, "busch") == 0 )
            {
                vP->voicing |= EDGE_BUSCH;
            }
            seenRoom = 1;
        }
        else if( (strcmp(word, "celeste") == 0) && (version >= 4) )
        {
            k = celesteOf(pP);
            if( seenCeleste || (k == 0) )
            {
                voiceFail(pathP, lineNo, "celeste: 0.5 to 7.5 cents, in half cents");
            }
            vP->voicing |= (k << EDGE_CELESTE_SHIFT);
            seenCeleste = 1;
        }
        else if( (strcmp(word, "interpolate") == 0) && (version >= 5) )
        {
            if( seenInterp || (sscanf(pP, " %c", &extra) == 1) )
            {
                voiceFail(pathP, lineNo, "interpolate: no value");
            }
            vP->interp = 1;
            seenInterp = 1;
        }
        else if( (strcmp(word, "pack") == 0) && (version >= 5) )
        {
            if( seenPack || (sscanf(pP, " %c", &extra) == 1) )
            {
                voiceFail(pathP, lineNo, "pack: no value");
            }
            vP->pack = 1;
            seenPack = 1;
        }
        else if( (strcmp(word, "vibrato") == 0) && (version >= 5) )
        {
            // vibrato RATE DEPTH DELAY, or RATE DEPTH free (a word, so not by voiceNumbers()).
            if( seenVibrato || (sscanf(pP, "%31s %31s %31s %c", tok[0], tok[1], tok[2], &extra) != 3)
                || !wholeOf(tok[0], &vals[0]) || !wholeOf(tok[1], &vals[1]) || (vals[0] < 1)
                || (vals[0] > RATE_MAX) || (vals[1] > LEVEL_MAX) )
            {
                voiceFail(pathP, lineNo, "vibrato: the rate (1 to 262143), the depth (0-511), and the delay or free");
            }
            vP->vRate = vals[0];
            vP->vDepth = vals[1];
            vP->vFree = (strcmp(tok[2], "free") == 0);
            if( !vP->vFree && (!wholeOf(tok[2], &vP->vDelay) || (vP->vDelay > DELAY_MAX)) )
            {
                voiceFail(pathP, lineNo, "vibrato: the delay is 0-255, or free");
            }
            seenVibrato = 1;
        }
        else if( (strcmp(word, "leslie") == 0) && (version >= 5) )
        {
            if( seenLeslie || (voiceNumbers(pP, vals, 2, 10) != 2) || (vals[0] > LEVEL_MAX) || (vals[1] > LEVEL_MAX) )
            {
                voiceFail(pathP, lineNo, "leslie: the tremolo's depth and the place's, 0-511 each");
            }
            vP->lesA = vals[0];
            vP->lesP = vals[1];
            seenLeslie = 1;
        }
        else if( (strcmp(word, "percussion") == 0) && (version >= 5) )
        {
            if( seenPercussion || (voiceNumbers(pP, vals, 4, 10) != 4) || (vals[0] > LEVEL_MAX) || (vals[1] < 1)
                || (vals[1] > K_MAX) || (vals[2] < 1) || (vals[2] > RATE_MAX) || (vals[3] > 255) )
            {
                voiceFail(pathP, lineNo, "percussion: its level (0-511), k (1-7), its decay (1 to 262143) and its "
                          "sine's block");
            }
            vP->pLevel = vals[0];
            vP->pK = vals[1];
            vP->pDecay = vals[2];
            vP->pBlock = (int)vals[3];
            seenPercussion = 1;
        }
        else if( strcmp(word, "block") == 0 )
        {
            // Its number and length, and in version 5 its own end, loop and stride.
            n = voiceNumbers(pP, vals, 5, 10);
            if( ((n != 2) && ((n != 5) || (version < 5))) || (vals[0] != vP->nBlocks) || (vals[1] < 1)
                || (vals[1] > 0177777) || (vP->nBlocks == 256) )
            {
                voiceFail(pathP, lineNo, "block: its number, in order from 0 to 255, and its length, and in "
                          "version 5 its own end, loop and stride");
            }
            if( (n == 5) && ((vals[2] < 1) || (vals[2] > ADDR_MAX) || (vals[3] > vals[2]) || (vals[4] > ADDR_MAX)) )
            {
                voiceFail(pathP, lineNo, "block: its own end (1 to 65535), loop (to the end) and stride (to 65535)");
            }
            lengthsP = growOrFail(lengthsP, ((size_t)vP->nBlocks + 1) * sizeof(int));
            lengthsP[vP->nBlocks] = (int)vals[1];
            ownP = growOrFail(ownP, (((size_t)vP->nBlocks + 1) * 3) * sizeof(long));
            ownP[3 * vP->nBlocks] = ((n == 5) ? vals[2] : -1);
            ownP[(3 * vP->nBlocks) + 1] = ((n == 5) ? vals[3] : 0);
            ownP[(3 * vP->nBlocks) + 2] = ((n == 5) ? vals[4] : 0);
            vP->nBlocks++;
            if( (nWords + vals[1]) > capWords )
            {
                capWords = (((nWords + vals[1]) > (2 * capWords)) ? (nWords + vals[1]) : (2 * capWords));
                wordsP = growOrFail(wordsP, (size_t)capWords * sizeof(unsigned long));
            }
            blockFill = 0;
            field = F_BLOCK;
        }
        else
        {
            voiceFail(pathP, lineNo, "no such field");
        }
    }
    fclose(fP);
    if( (field == F_BLOCK) && (blockFill != lengthsP[vP->nBlocks - 1]) )
    {
        voiceFail(pathP, lineNo, "a block has fewer words than its length");
    }
    if( !seenVersion || (vP->nameP == NULL) || (vP->n < 0) || !seenMode || !seenEnvelope || !seenStride
        || !seenThresholds || (nSteps != LIB_PITCHES) || (nZones != LIB_PITCHES) || (vP->nBlocks == 0) )
    {
        voiceFail(pathP, 0, "a field is missing: every voice has its name, n, mode, envelope, stride, "
                  "thresholds, 64 steps, 64 zones and a block");
    }
    if( (vP->sample || vP->stop) != seenSample )
    {
        voiceFail(pathP, 0, "a sample line belongs with mode sample or stop, and only there");
    }
    if( vP->stop != (nSpeech > 0) )
    {
        voiceFail(pathP, 0, "a speech line belongs with mode stop, and only there");
    }
    if( vP->zone6 >= vP->nBlocks )
    {
        voiceFail(pathP, 0, "zone 6 names a block the file does not hold");
    }
    vP->version = version;
    // The edge and the rise follow the note's fundamental, the table's one
    // period; a sample has none.
    if( vP->sample && ((vP->voicing & (EDGE_MASK | EDGE_RISE)) != 0) )
    {
        voiceFail(pathP, 0, "the edge and the rise belong with a table or a stop");
    }
    // A stop: its premix's loop is one table, after the speech; each
    // zone's speech block is the speech and each table TABLE_LENGTH words,
    // as the player reads them into the premix's place. Without zone 6
    // its seventh speech is its sixth, as the drum's header has it.
    if( vP->stop )
    {
        if( (nSpeech != ((vP->zone6 >= 0) ? MAX_ZONES : NZONES)) || (vP->loop != TABLE_LENGTH)
            || (vP->end <= vP->loop) )
        {
            voiceFail(pathP, 0, "a stop: 6 speech blocks, or 7 with zone 6, a loop of 256 words after its speech");
        }
        if( vP->zone6 < 0 )
        {
            vP->speech[NZONES] = vP->speech[NZONES - 1];
        }
        for(i = 0; i < MAX_ZONES; i++)
        {
            if( (vP->speech[i] >= vP->nBlocks) || (lengthsP[vP->speech[i]] != (vP->end - vP->loop)) )
            {
                voiceFail(pathP, 0, "a stop's speech block is not in the file, or not as long as its speech");
            }
        }
        for(i = 0; i <= LIB_PITCHES; i++)
        {
            b = ((i < LIB_PITCHES) ? vP->zones[i] : vP->zone6);
            if( (b >= 0) && (b < vP->nBlocks) && (lengthsP[b] != TABLE_LENGTH) )
            {
                voiceFail(pathP, 0, "a stop's table is not 256 words");
            }
        }
    }
    // Every word the voice can fetch is in its block: a table's timbres
    // and their 256 words, a sample up to its end; a block with its own
    // end and stride, its end and each timbre's.
    need = TABLE_LENGTH;
    timbres = 0;
    for(i = 0; i < 3; i++)
    {
        if( vP->thresholds[i] != NO_THRESHOLD )
        {
            need += (int)vP->stride;
            timbres++;
        }
    }
    if( vP->sample )
    {
        need = (int)vP->end;
    }
    // A packed voice's end, loop and stride are in groups, two words each,
    // and each timbre's end is fetched.
    if( vP->pack )
    {
        if( !vP->sample )
        {
            voiceFail(pathP, 0, "pack belongs with mode sample");
        }
        need = (int)(2 * (vP->end + (timbres * vP->stride)));
    }
    // Zone 6's block, at i == LIB_PITCHES, is checked as the map's are.
    for(i = 0; i <= LIB_PITCHES; i++)
    {
        if( (i == LIB_PITCHES) && (vP->zone6 < 0) )
        {
            break;
        }
        b = ((i < LIB_PITCHES) ? vP->zones[i] : vP->zone6);
        if( b >= vP->nBlocks )
        {
            voiceFail(pathP, 0, "a zone names a block the file does not hold");
        }
        if( vP->pack && (((ownP[3 * b] >= 0) ? ownP[3 * b] : vP->end) > PACKED_END_MAX) )
        {
            voiceFail(pathP, 0, "a packed end is over 21845 groups");
        }
        if( vP->pack && seenPercussion && (b == vP->pBlock) )
        {
            voiceFail(pathP, 0, "a packed voice's zone names the percussion's block, which is not packed");
        }
        if( ownP[3 * b] >= 0 )
        {
            if( lengthsP[b] < ((vP->pack ? 2 : 1) * (ownP[3 * b] + (timbres * ownP[(3 * b) + 2]))) )
            {
                voiceFail(pathP, 0, "a block is shorter than its own end and timbres");
            }
        }
        else if( lengthsP[b] < need )
        {
            voiceFail(pathP, 0, "a block is shorter than the voice plays");
        }
    }
    // A block's own end, loop and stride are a sample's, latched with its zone.
    for(b = 0; b < vP->nBlocks; b++)
    {
        if( ownP[3 * b] >= 0 )
        {
            for(i = 0; (i < LIB_PITCHES) && (vP->zones[i] != b); i++)
            {
            }
            if( !vP->sample || ((i == LIB_PITCHES) && (b != vP->zone6)) )
            {
                voiceFail(pathP, 0, "a block's own end, loop and stride belong with mode sample, on a block a "
                          "zone names");
            }
        }
    }
    if( seenLeslie && !seenVibrato )
    {
        voiceFail(pathP, 0, "a leslie line needs a vibrato line, the counter's rate");
    }
    if( seenPercussion && ((vP->pBlock >= vP->nBlocks) || (lengthsP[vP->pBlock] < PERC_WORDS)) )
    {
        voiceFail(pathP, 0, "the percussion's block is not in the file, or under 256 words");
    }
    vP->lengthsP = lengthsP;
    vP->wordsP = wordsP;
    vP->ownP = ownP;
}

// blockValues: block b's end, loop and stride: its own, or the voice's (a
// compiled voice has none of its own).
static void blockValues(const Voice *vP, int b, long *endP, long *loopP, long *strideP)
{
    if( (vP->ownP != NULL) && (vP->ownP[3 * b] >= 0) )
    {
        *endP = vP->ownP[3 * b];
        *loopP = vP->ownP[(3 * b) + 1];
        *strideP = vP->ownP[(3 * b) + 2];
    }
    else
    {
        *endP = vP->end;
        *loopP = vP->loop;
        *strideP = vP->stride;
    }
}

// ownEnds: whether each note latches its block's end, loop and stride: a
// block a zone names (zone 6's too) has its own, other than the voice's.
static int ownEnds(const Voice *vP)
{
    long e;
    long l;
    long s;
    int p;

    for(p = 0; p <= LIB_PITCHES; p++)
    {
        if( (p == LIB_PITCHES) && (vP->zone6 < 0) )
        {
            break;
        }
        blockValues(vP, ((p < LIB_PITCHES) ? vP->zones[p] : vP->zone6), &e, &l, &s);
        if( (e != vP->end) || (l != vP->loop) || (s != vP->stride) )
        {
            return 1;
        }
    }
    return 0;
}

// voiceLatches: the six latches version 5 adds for voice vP, its
// percussion's sine at kAddr, into *iP; hasLatches when any of the voice's
// five is not 0, as the program writes the six only then.
static void voiceLatches(const Voice *vP, long kAddr, Instrument *iP)
{
    iP->dyv = vP->vRate;
    iP->dyw = ((vP->vDepth << 9) | (vP->vDelay << 1) | (long)vP->vFree);
    iP->dym = ((vP->lesA << 9) | vP->lesP);
    iP->dyk = ((vP->pLevel << 9) | vP->pK);
    iP->dyj = vP->pDecay;
    iP->dyp = kAddr;
    iP->hasLatches = ((iP->dyv | iP->dyw | iP->dym | iP->dyk | iP->dyj) != 0);
}

// readLibrary: the voice files a library lists, one a line, relative to
// the library's own directory; a voice's number is its place in the list.
static void readLibrary(const char *pathP)
{
    FILE *fP;
    char line[MAX_LINE];
    char *pP;
    char *hashP;
    char *fullP;
    const char *slashP;
    size_t dir;
    int n;

    if( !(fP = fopen(pathP, "r")) )
    {
        voiceFail(pathP, 0, "cannot read the library");
    }
    slashP = strrchr(pathP, '/');
    dir = ((slashP != NULL) ? (size_t)((slashP - pathP) + 1) : 0);
    nVoices = 0;
    libraryFive = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        if( (hashP = strchr(line, '#')) != NULL )
        {
            *hashP = '\0';
        }
        for(pP = line; (*pP == ' ') || (*pP == '\t'); pP++)
        {
        }
        n = (int)strlen(pP);
        while( (n > 0) && ((pP[n - 1] == '\n') || (pP[n - 1] == '\r') || (pP[n - 1] == ' ') || (pP[n - 1] == '\t')) )
        {
            n--;
        }
        pP[n] = '\0';
        if( n == 0 )
        {
            continue;
        }
        if( nVoices == MAX_VOICES )
        {
            voiceFail(pathP, 0, "more than 512 voices");
        }
        if( pP[0] == '/' )
        {
            fullP = strdupOrFail(pP);
        }
        else
        {
            fullP = growOrFail(NULL, dir + (size_t)n + 1);
            memcpy(fullP, pathP, dir);
            strcpy(fullP + dir, pP);
        }
        readVoice(fullP, &voices[nVoices]);
        libraryFive |= (voices[nVoices].version >= FIRST_FIVE);
        nVoices++;
        free(fullP);
    }
    fclose(fP);
    if( nVoices == 0 )
    {
        voiceFail(pathP, 0, "the library lists no voice");
    }
}

// placeVoice: voice k's blocks in memory from placeNext on, as the
// player's loader places them, into *iP: with own ends, each block after
// its end, loop and stride (OWN_PREFIX words).
static void placeVoice(int k, Instrument *iP)
{
    const Voice *vP;
    long addr[256];
    long prefix[OWN_PREFIX];
    long w;
    long i;
    long own;
    int b;
    int p;

    vP = &voices[k];
    memset(iP, 0, sizeof(*iP));
    own = (ownEnds(vP) ? OWN_PREFIX : 0);
    w = 0;
    for(b = 0; b < vP->nBlocks; b++)
    {
        if( (placeNext + own + vP->lengthsP[b]) > MEM_WORDS )
        {
            fprintf(stderr, "dacrender: the orchestra's voices do not fit the player's banks 5-15\n");
            exit(1);
        }
        if( own != 0 )
        {
            blockValues(vP, b, &prefix[0], &prefix[1], &prefix[2]);
            for(i = 0; i < OWN_PREFIX; i++)
            {
                memWords[placeNext] = prefix[i];
                placeNext++;
            }
        }
        addr[b] = placeNext;
        for(i = 0; i < vP->lengthsP[b]; i++)
        {
            memWords[placeNext] = (long)vP->wordsP[w];
            placeNext++;
            w++;
        }
    }
    iP->nameP = vP->nameP;
    iP->n = vP->n;
    iP->own = ownEnds(vP);
    for(p = 0; p < LIB_PITCHES; p++)
    {
        iP->zoneMap[p] = addr[vP->zones[p]];
        iP->steps[p] = vP->steps[p];
        blockValues(vP, vP->zones[p], &iP->zoneEnd[p], &iP->zoneLoop[p], &iP->zoneStride[p]);
    }
    if( vP->zone6 >= 0 )
    {
        iP->zone6 = addr[vP->zone6];
        blockValues(vP, vP->zone6, &iP->zone6End, &iP->zone6Loop, &iP->zone6Stride);
    }
    iP->attack = vP->attack;
    iP->decay = vP->decay;
    iP->release = vP->release;
    iP->start = vP->start;
    iP->peak = vP->peak;
    iP->sustain = vP->sustain;
    iP->held = ((vP->sample ? ACT_HELD : ACT_WAVE) | (vP->interp ? ACT_INTERP : 0) | (vP->pack ? ACT_PACK : 0));
    iP->end = vP->end;
    iP->loop = vP->loop;
    iP->stride = vP->stride;
    iP->thresholds[0] = vP->thresholds[0];
    iP->thresholds[1] = vP->thresholds[1];
    iP->thresholds[2] = vP->thresholds[2];
    iP->voicing = vP->voicing;
    voiceLatches(vP, ((vP->pK != 0) ? addr[vP->pBlock] : 0), iP);
}

// zoneEnds: the end, loop and stride the table at addr latches with it,
// for an instrument whose blocks have their own.
static void zoneEnds(const Instrument *iP, long addr, long *endP, long *loopP, long *strideP)
{
    int p;

    if( (iP->zone6 != 0) && (addr == iP->zone6) )
    {
        *endP = iP->zone6End;
        *loopP = iP->zone6Loop;
        *strideP = iP->zone6Stride;
        return;
    }
    for(p = 0; (p < LIB_PITCHES) && (iP->zoneMap[p] != addr); p++)
    {
    }
    if( p == LIB_PITCHES )
    {
        fail("a table latched that no zone names");
    }
    *endP = iP->zoneEnd[p];
    *loopP = iP->zoneLoop[p];
    *strideP = iP->zoneStride[p];
}

// sampleOf: a word's sample, as the converters read bits 0-11.
static long sampleOf(long w)
{
    long v;

    v = ((w >> 6) & 07777);
    return ((v & 04000) ? -((~v) & 07777) : v);
}

// wordOf: a sample as an 18-bit ones' complement word.
static long wordOf(long sample)
{
    long w;

    w = (labs(sample) << 6);
    return ((sample < 0) ? ((~w) & WORD_MASK) : w);
}

// blockOf: the words of voice vP's block b.
static const unsigned long *blockOf(const Voice *vP, int b)
{
    const unsigned long *wP;
    int i;

    wP = vP->wordsP;
    for(i = 0; i < b; i++)
    {
        wP += vP->lengthsP[i];
    }
    return wP;
}

// tableBlock: voice vP's block for zone z, 0-6: the zone map's at the
// zone's first pitch, or zone 6's; zone 5's for zone 6 in a voice without
// one.
static int tableBlock(const Voice *vP, int z)
{
    if( (z == NZONES) && (vP->zone6 >= 0) )
    {
        return vP->zone6;
    }
    return vP->zones[2 + (ZONE_PITCHES * ((z < NZONES) ? z : (NZONES - 1)))];
}

// placeStops: the converter's stops added together in memory from
// placeNext on, as the player's loader adds them: for each of the NZONES
// zones, and on a version 5 drum zone 6 too, a block of the sample's end
// words, word i the sum of every stop's zone table word i mod
// TABLE_LENGTH and, below the speech's end, of the first stop's speech
// word i; the PDP-1's adds give the integer sum, since no partial sum
// passes full scale. Every pitch plays its zone's block; the seventh is
// zone 6 when every stop has one, else the block of pitches 62-63. The
// voicing is the loudest edge, the widest céleste and every other bit any
// stop has. Ends the run if the stops are not alike (their voicing aside),
// as the player refuses them.
static void placeStops(const OrchLine *oP, Instrument *iP)
{
    const Voice *fP;
    const Voice *vP;
    const unsigned long *tabP[STOPS];
    const unsigned long *speechP;
    const unsigned long *sineP;
    long sum;
    long kAddr;
    long i;
    int voicing;
    int nBlocks;
    int mixed;
    int s;
    int z;
    int p;

    memset(iP, 0, sizeof(*iP));
    fP = &voices[oP->stops[0]];
    nBlocks = (libraryFive ? MAX_ZONES : NZONES);
    mixed = 0;
    voicing = 0;
    for(s = 0; s < oP->nStops; s++)
    {
        vP = &voices[oP->stops[s]];
        if( (vP->voicing & EDGE_MASK) > (voicing & EDGE_MASK) )
        {
            voicing = ((voicing & ~EDGE_MASK) | (vP->voicing & EDGE_MASK));
        }
        if( (vP->voicing & EDGE_CELESTE) > (voicing & EDGE_CELESTE) )
        {
            voicing = ((voicing & ~EDGE_CELESTE) | (vP->voicing & EDGE_CELESTE));
        }
        voicing |= (vP->voicing & ~(EDGE_MASK | EDGE_CELESTE) & EDGE_VOICING);
        if( !vP->stop || (vP->n != fP->n) || (vP->start != fP->start) || (vP->peak != fP->peak)
            || (vP->attack != fP->attack) || (vP->decay != fP->decay) || (vP->sustain != fP->sustain)
            || (vP->release != fP->release) || (vP->stride != fP->stride) || (vP->end != fP->end)
            || (vP->loop != fP->loop) || (memcmp(vP->thresholds, fP->thresholds, sizeof(fP->thresholds)) != 0)
            || (memcmp(vP->steps, fP->steps, sizeof(fP->steps)) != 0) || (vP->interp != fP->interp)
            || (vP->vRate != fP->vRate) || (vP->vDepth != fP->vDepth) || (vP->vDelay != fP->vDelay)
            || (vP->vFree != fP->vFree) || (vP->lesA != fP->lesA) || (vP->lesP != fP->lesP)
            || (vP->pLevel != fP->pLevel) || (vP->pK != fP->pK) || (vP->pDecay != fP->pDecay)
            || ((vP->pK != 0) && (memcmp(blockOf(vP, vP->pBlock), blockOf(fP, fP->pBlock),
                                         PERC_WORDS * sizeof(unsigned long)) != 0)) )
        {
            fprintf(stderr, "dacrender: the stops %s and %s do not match, and the player refuses them\n",
                    fP->nameP, vP->nameP);
            exit(1);
        }
        mixed |= (vP->zone6 < 0);
    }
    if( (placeNext + (nBlocks * fP->end) + ((fP->pK != 0) ? fP->lengthsP[fP->pBlock] : 0)) > MEM_WORDS )
    {
        fprintf(stderr, "dacrender: the orchestra's voices do not fit the player's banks 5-15\n");
        exit(1);
    }
    for(z = 0; z < nBlocks; z++)
    {
        for(s = 0; s < oP->nStops; s++)
        {
            vP = &voices[oP->stops[s]];
            tabP[s] = blockOf(vP, tableBlock(vP, z));
        }
        speechP = blockOf(fP, fP->speech[z]);
        for(i = 0; i < fP->end; i++)
        {
            sum = ((i < (fP->end - fP->loop)) ? sampleOf((long)speechP[i]) : 0);
            for(s = 0; s < oP->nStops; s++)
            {
                sum += sampleOf((long)tabP[s][i % TABLE_LENGTH]);
            }
            memWords[placeNext + (z * fP->end) + i] = wordOf(sum);
        }
    }
    iP->nameP = fP->nameP;
    iP->n = fP->n;
    for(p = 0; p < LIB_PITCHES; p++)
    {
        z = (((p > 2) ? (p - 2) : 0) / ZONE_PITCHES);
        if( libraryFive && mixed && (z == (NZONES - 1)) )
        {
            z = NZONES;             // a stop without zone 6: 62-63 play the seventh block
        }
        iP->zoneMap[p] = (placeNext + (z * fP->end));
        iP->steps[p] = fP->steps[p];
    }
    if( libraryFive && !mixed )
    {
        iP->zone6 = (placeNext + (NZONES * fP->end));
    }
    placeNext += (nBlocks * fP->end);
    // The first stop's percussion sine, the whole block, after the premix.
    kAddr = 0;
    if( fP->pK != 0 )
    {
        kAddr = placeNext;
        sineP = blockOf(fP, fP->pBlock);
        for(i = 0; i < fP->lengthsP[fP->pBlock]; i++)
        {
            memWords[placeNext] = (long)sineP[i];
            placeNext++;
        }
    }
    iP->attack = fP->attack;
    iP->decay = fP->decay;
    iP->release = fP->release;
    iP->start = fP->start;
    iP->peak = fP->peak;
    iP->sustain = fP->sustain;
    iP->held = (ACT_HELD | (fP->interp ? ACT_INTERP : 0));
    iP->end = fP->end;
    iP->loop = fP->loop;
    iP->stride = fP->stride;
    iP->thresholds[0] = fP->thresholds[0];
    iP->thresholds[1] = fP->thresholds[1];
    iP->thresholds[2] = fP->thresholds[2];
    iP->voicing = voicing;
    iP->own = 0;
    voiceLatches(fP, kAddr, iP);
}

// The tape.

// readTapeFile: the file's words, three frames with the eighth hole each;
// frames without it (leader) are skipped, as rpb skips them.
// Returns 1, or 0 if the file cannot be read.
static int readTapeFile(const char *pathP, TapeReader *rP)
{
    FILE *fP;
    int c;
    int frame[3];
    int nFrame;
    long cap;

    if( !(fP = fopen(pathP, "rb")) )
    {
        return 0;
    }
    rP->wordsP = NULL;
    rP->nWords = 0;
    rP->pos = 0;
    rP->fp = TAPE_START;
    cap = 0;
    nFrame = 0;
    while( (c = fgetc(fP)) != EOF )
    {
        if( (c & 0200) == 0 )
        {
            continue;
        }
        frame[nFrame] = (c & 077);
        nFrame++;
        if( nFrame == 3 )
        {
            if( rP->nWords == cap )
            {
                cap = ((cap == 0) ? 1024 : (cap * 2));
                rP->wordsP = growOrFail(rP->wordsP, (size_t)cap * sizeof(int));
            }
            rP->wordsP[rP->nWords] = ((frame[0] << 12) | (frame[1] << 6) | frame[2]);
            rP->nWords++;
            nFrame = 0;
        }
    }
    fclose(fP);
    return 1;
}

// nextWord: the tape's next word into *wP.
// Returns 1, or 0 at the tape's end.
static int nextWord(TapeReader *rP, int *wP)
{
    if( rP->pos >= rP->nWords )
    {
        return 0;
    }
    *wP = rP->wordsP[rP->pos];
    rP->pos++;
    return 1;
}

// advance: the player's next word of banks 2-3 (its adv).
// Returns 1, or 0 when that reaches TAPE_END.
static int advance(TapeReader *rP)
{
    rP->fp++;
    return (rP->fp != TAPE_END);
}

// onesAdd: the PDP-1's add, ones' complement with the end-around carry,
// and a minus zero made plus zero, as the player's checksum adds.
static int onesAdd(int a, int b)
{
    long s;

    s = ((long)a + (long)b);
    if( s > WORD_MASK )
    {
        s = ((s + 1) & WORD_MASK);
    }
    return ((s == WORD_MASK) ? 0 : (int)s);
}

// readBlock: count words into a new array at *wordsPP, then the checksum,
// each word taking its place in banks 2-3 as the player's rdb does.
// Returns READ_OK, READ_END (the tape ended in the block) or READ_LARGE
// (past bank 3); *sumOkP says whether the checksum held.
static int readBlock(TapeReader *rP, int count, int **wordsPP, int *sumOkP)
{
    int *wP;
    int i;
    int sum;
    int w;

    wP = growOrFail(NULL, (size_t)count * sizeof(int));
    *wordsPP = wP;
    sum = 0;
    for(i = 0; i < count; i++)
    {
        if( !nextWord(rP, &w) )
        {
            return READ_END;
        }
        wP[i] = w;
        sum = onesAdd(sum, w);
        if( !advance(rP) )
        {
            return READ_LARGE;
        }
    }
    if( !nextWord(rP, &w) )
    {
        return READ_END;
    }
    *sumOkP = (w == sum);
    return READ_OK;
}

// inRange: the player's rng, 0 <= w < limit on an 18-bit word.
static int inRange(int w, int limit)
{
    return (((w & SIGN_BIT) == 0) && (w < limit));
}

// isLevel: whether a volume is one of the slider's levels.
static int isLevel(int w)
{
    int k;

    for(k = 0; k < VLEVELS; k++)
    {
        if( w == (k * VSTEP) )
        {
            return 1;
        }
    }
    return 0;
}

// isPlace: whether a place is one of the player's nine.
static int isPlace(int w)
{
    return (inRange(w, PLACE_RIGHT + 1) && ((w % PLACE_STEP) == 0));
}

// defaultPlace: converter c's place before any is given: 1 and 2 left, 3
// and 4 right.
static int defaultPlace(int c)
{
    return ((c < 2) ? 0 : PLACE_RIGHT);
}

// isOnset: whether a block's onset is one the player's list has: the
// configuration's, off, or 2, 4, 6, 8, 12 or 16 ms.
static int isOnset(int w)
{
    static const int onsets[] = { SETTING_CONFIGURED, 0, 020, 040, 060, 0100, 0140, 0200 };
    size_t k;

    for(k = 0; k < (sizeof(onsets) / sizeof(onsets[0])); k++)
    {
        if( w == onsets[k] )
        {
            return 1;
        }
    }
    return 0;
}

// takeOrchestra2: a version 2 or 3 block's words (n of them), as the
// player's orb takes them: a line has one to four stops, packed from the
// first, no detune, and a place that is one of the nine; version 3's room
// and onset are the configuration's or ones the player's lists have.
// Returns as takeOrchestra; NCONV + 1 also for a version that is not the
// block's, or a room or onset the player does not have.
static int takeOrchestra2(const int *wP, int n)
{
    const int *lP;
    int c;
    int s;

    if( (wP[0] != ((n == ORCH_WORDS3) ? ORCH_VERSION3 : ORCH_VERSION2)) || !inRange(((wP[1] - 1) & WORD_MASK), NCONV) )
    {
        return (NCONV + 1);
    }
    if( (n == ORCH_WORDS3) && (((wP[38] != SETTING_CONFIGURED) && !inRange(wP[38], ROOM_CODES)) || !isOnset(wP[39])) )
    {
        return (NCONV + 1);
    }
    for(c = 0; c < NCONV; c++)
    {
        lP = (wP + 2 + (LINE2 * c));
        if( !inRange(lP[0], NCONV + 1) || !inRange(lP[1], NOCT) || !isLevel(lP[2]) || (lP[3] != 0)
            || !isPlace(lP[4]) || !inRange(((lP[5] - 1) & WORD_MASK), nVoices) )
        {
            return (c + 1);
        }
        for(s = 1; s < STOPS; s++)
        {
            if( (lP[5 + s] != 0) && ((lP[4 + s] == 0) || !inRange(((lP[5 + s] - 1) & WORD_MASK), nVoices)) )
            {
                return (c + 1);
            }
        }
    }
    per = wP[1];
    for(c = 0; c < NCONV; c++)
    {
        lP = (wP + 2 + (LINE2 * c));
        orch[c].part = (lP[0] - 1);
        orch[c].octave = lP[1];
        volumes[c] = lP[2];
        places[c] = lP[4];
        orch[c].inst = (lP[5] - 1);
        orch[c].nStops = 0;
        for(s = 0; (s < STOPS) && (lP[5 + s] != 0); s++)
        {
            orch[c].stops[s] = (lP[5 + s] - 1);
            orch[c].nStops++;
        }
    }
    tapeRoom = ((n == ORCH_WORDS3) ? wP[38] : SETTING_CONFIGURED);
    tapeOnset = ((n == ORCH_WORDS3) ? wP[39] : SETTING_CONFIGURED);
    return 0;
}

// takeOrchestra: the block's words, as the player's orb takes them: every
// number one the player has, or nothing changed. A block from before
// version 2 puts each converter at its own place, and one from before
// version 3 leaves the room and the onset the configuration's.
// Returns 0 if taken, else the converter (1 to NCONV) with a number out of
// range or a volume that is no level, or NCONV + 1 for per.
static int takeOrchestra(const int *wP, int n)
{
    static const int zeros[NCONV] = { 0, 0, 0, 0 };
    const int *volP;
    int c;
    int p;

    if( (n == ORCH_WORDS2) || (n == ORCH_WORDS3) )
    {
        return takeOrchestra2(wP, n);
    }
    volP = ((n == ORCH_WORDS) ? (wP + ORCH_WORDS0) : zeros);
    p = ((wP[0] - 1) & WORD_MASK);
    if( !inRange(p, NCONV) )
    {
        return (NCONV + 1);
    }
    for(c = 0; c < NCONV; c++)
    {
        if( !inRange(wP[1 + (3 * c)], NCONV + 1) || !inRange(wP[2 + (3 * c)], nVoices)
            || !inRange(wP[3 + (3 * c)], NOCT) )
        {
            return (c + 1);
        }
    }
    for(c = 0; c < NCONV; c++)
    {
        if( !isLevel(volP[c]) )
        {
            return (c + 1);
        }
    }
    per = wP[0];
    for(c = 0; c < NCONV; c++)
    {
        orch[c].part = (wP[1 + (3 * c)] - 1);
        orch[c].inst = wP[2 + (3 * c)];
        orch[c].stops[0] = orch[c].inst;
        orch[c].nStops = 1;
        orch[c].octave = wP[3 + (3 * c)];
        volumes[c] = volP[c];
        places[c] = defaultPlace(c);
    }
    tapeRoom = SETTING_CONFIGURED;
    tapeOnset = SETTING_CONFIGURED;
    return 0;
}

// tapeError: READ TAPE's end at an error: the parts read whole before it
// are kept.
static void tapeError(const char *whyP)
{
    printf("warning: TAPE ERROR %s: the %d parts read whole before it are kept\n", whyP, nParts);
}

// loadTape: every part on the tape, and an orchestra block, as READ TAPE
// takes them (dacmusic.am1's loader): a checksum that fails takes the
// block as read; a bad count, too many parts or a tape larger than its
// area ends the read, keeping the parts before it; a block the player
// would refuse leaves the orchestra as it was.
static void loadTape(TapeReader *rP)
{
    long ph;
    int ct;
    int nct;
    int i;
    int status;
    int sumOk;
    int why;
    int *notesP;
    int *barsP;
    int *blockP;

    nParts = 0;
    for(;;)
    {
        ph = rP->fp;
        for(i = 0; i < PART_HEADER; i++)
        {
            if( !advance(rP) )
            {
                tapeError("TAPE TOO LARGE");
                return;
            }
        }
        if( !nextWord(rP, &ct) )
        {
            return;
        }
        if( (ct == ORCH_COUNT) || (ct == ORCH_COUNT0) || (ct == ORCH_COUNT2) || (ct == ORCH_COUNT3) )
        {
            rP->fp = ph;
            status = readBlock(rP, (ct & 0777), &blockP, &sumOk);
            if( status == READ_LARGE )
            {
                free(blockP);
                tapeError("TAPE TOO LARGE");
                return;
            }
            if( status == READ_END )
            {
                free(blockP);
                printf("warning: the tape ends in the orchestra block, which is not taken\n");
                return;
            }
            if( !sumOk )
            {
                printf("warning: the orchestra block's checksum fails (TAPE READ CHECKSUM ERROR); "
                       "taken as read\n");
            }
            if( (why = takeOrchestra(blockP, (ct & 0777))) != 0 )
            {
                if( why == (NCONV + 1) )
                {
                    printf("warning: TAPE READ ORCHESTRA REFUSED: its parts a movement are not 1 to %d, "
                           "its version is not one the player reads, or its room or onset is not one the "
                           "player's lists have; the orchestra is left as it was\n", NCONV);
                }
                else
                {
                    printf("warning: TAPE READ ORCHESTRA REFUSED: converter %d has a part, instrument or "
                           "octave the player does not have, or a volume that is not a slider's level, "
                           "or a stop the library lacks, a gap in the stops, a detune, or a place that is not "
                           "one of the nine; the orchestra is left as it was\n", why);
                }
            }
            free(blockP);
            rP->fp = ph;
            continue;
        }
        if( (ct == 0) || ((ct & SIGN_BIT) != 0) )
        {
            tapeError("BAD COUNT");
            return;
        }
        nct = ct;
        status = readBlock(rP, nct, &notesP, &sumOk);
        if( status != READ_OK )
        {
            free(notesP);
            if( status == READ_LARGE )
            {
                tapeError("TAPE TOO LARGE");
            }
            return;
        }
        if( !sumOk )
        {
            printf("warning: part %d's notes: the checksum fails (TAPE READ CHECKSUM ERROR); taken as read\n",
                   nParts + 1);
        }
        // The bar line the player puts after the notes.
        if( !advance(rP) )
        {
            free(notesP);
            tapeError("TAPE TOO LARGE");
            return;
        }
        if( !nextWord(rP, &ct) )
        {
            free(notesP);
            return;
        }
        if( (ct == 0) || ((ct & SIGN_BIT) != 0) )
        {
            free(notesP);
            tapeError("BAD COUNT");
            return;
        }
        status = readBlock(rP, ct, &barsP, &sumOk);
        if( status != READ_OK )
        {
            free(notesP);
            free(barsP);
            if( status == READ_LARGE )
            {
                tapeError("TAPE TOO LARGE");
            }
            return;
        }
        if( !sumOk )
        {
            printf("warning: part %d's bar pointers: the checksum fails (TAPE READ CHECKSUM ERROR); "
                   "taken as read\n", nParts + 1);
        }
        // The 0 that ends the list, then the bar pointers as read, kept for PUNCH.
        for(i = 0; i <= ct; i++)
        {
            if( !advance(rP) )
            {
                free(notesP);
                free(barsP);
                tapeError("TAPE TOO LARGE");
                return;
            }
        }
        if( nParts == MAX_PARTS )
        {
            free(notesP);
            free(barsP);
            tapeError("TOO MANY PARTS");
            return;
        }
        parts[nParts].notesP = notesP;
        parts[nParts].nNotes = nct;
        parts[nParts].barsP = barsP;
        parts[nParts].nBars = ct;
        nParts++;
    }
}

// The timeline.

// kOf: cycles a unit (1/1536 of a whole note) at tempo t, in 1/16 cycle,
// as pdp1m13's tpo forms it with the test word at 170.
static long long kOf(int tempo)
{
    long long q;
    long long tpm;

    q = (((long long)tempo * DEFAULT_TEMPO) / DEFAULT_TEMPO);
    tpm = ((8 * q * 601) >> 8);
    return ((tpm * 35) >> 4);
}

// atOffset: the time, in 1/16 cycle, of offset o in a bar starting at
// start, where the tempo changes at each segment's offset.
static long long atOffset(long long start, const Seg *segsP, int nSegs, long o)
{
    long long s;
    int i;

    s = start;
    for(i = 0; i < nSegs; i++)
    {
        if( ((i + 1) < nSegs) && (o > segsP[i + 1].o) )
        {
            s += ((long long)(segsP[i + 1].o - segsP[i].o) * kOf(segsP[i].tempo));
        }
        else
        {
            return (s + ((long long)(o - segsP[i].o) * kOf(segsP[i].tempo)));
        }
    }
    return s;
}

// silentUnits: the silent share of a note of u units for its
// articulation (pdp1m13's cxt). One cxt halts on plays as normal, as the
// player's Continue does.
static long silentUnits(int art, long u)
{
    switch( art )
    {
    case 0:
        return (u >> 3);
    case 1:
        return (u >> 2);
    case 2:
        return (u >> 1);
    case 4:
        return (((u >> 2) + u) >> 1);
    case 8:
        return 0;
    default:
        articulationsHalted++;
        return (u >> 3);
    }
}

// addEvent: appends an event to a voice's list.
static void addEvent(EventList *lP, long long t16, int kind, int pitch)
{
    if( lP->n == lP->cap )
    {
        lP->cap = ((lP->cap == 0) ? 64 : (lP->cap * 2));
        lP->evP = growOrFail(lP->evP, (size_t)lP->cap * sizeof(Event));
    }
    lP->evP[lP->n].t16 = t16;
    lP->evP[lP->n].kind = kind;
    lP->evP[lP->n].pitch = pitch;
    lP->n++;
}

// The sequencer.

// seqBoundary: a boundary at cycle t: the pending entry gets its length,
// split SPLIT cycles at a time while longer than MAX_EVENT, and a new one
// begins. An event at the last boundary's time joins its entry.
static void seqBoundary(Sequencer *sP, long long t)
{
    long long n;
    long piece;
    int first;

    if( t == sP->last )
    {
        return;
    }
    n = (t - sP->last);
    if( n < 0 )
    {
        fail("internal error: an event before the last boundary");
    }
    first = 1;
    while( n > 0 )
    {
        piece = ((n <= MAX_EVENT) ? (long)n : SPLIT);
        if( sP->nEntries == sP->capEntries )
        {
            sP->capEntries = ((sP->capEntries == 0) ? 1024 : (sP->capEntries * 2));
            sP->entriesP = growOrFail(sP->entriesP, (size_t)sP->capEntries * sizeof(Entry));
        }
        sP->entriesP[sP->nEntries].len = piece;
        sP->entriesP[sP->nEntries].dxa = (first ? sP->pend[0] : 0);
        sP->entriesP[sP->nEntries].dyf = (first ? sP->pend[1] : 0);
        sP->entriesP[sP->nEntries].dyx = (first ? sP->pend[2] : 0);
        sP->nEntries++;
        first = 0;
        n -= piece;
    }
    sP->pend[0] = 0;
    sP->pend[1] = 0;
    sP->pend[2] = 0;
    sP->last = t;
}

// seqMerge: the latches an event adds to the pending entry; 0 adds none.
static void seqMerge(Sequencer *sP, long dxa, long dyf, long dyx)
{
    if( dxa != 0 )
    {
        sP->pend[0] = dxa;
    }
    if( dyf != 0 )
    {
        sP->pend[1] = dyf;
    }
    if( dyx != 0 )
    {
        sP->pend[2] = dyx;
    }
}

// seqPitch: the pitch a note's step is taken at, and in *octP the
// octaves its step is still shifted by, as the player's pqs takes them.
// Each octave up raises the pitch while it stays in the zone map: a
// zone's harmonics are cut for its own top pitch, so a step shifted up
// past them would alias. An octave left that would sound above the
// voice's top (ZONE6_TOP with zone 6, ZONE_TOP without), or carry the
// step past the converter's 17 bits, is given up, the note broken back.
// So an octave is left only from 52-63, sounding 64-75 on zone 6, or from
// 52-61, sounding 64-73 on the top zone (seqZone). An octave down shifts
// the step alone.
static int seqPitch(const Sequencer *sP, int pitch, int *octP)
{
    int top;
    int o;

    top = ((sP->instP->zone6 != 0) ? ZONE6_TOP : ZONE_TOP);
    o = sP->octave;
    while( (o > 0) && ((pitch + ZONE_PITCHES) < LIB_PITCHES) )
    {
        pitch += ZONE_PITCHES;
        o--;
    }
    while( (o > 0) && ((pitch + (ZONE_PITCHES * o)) > top) )
    {
        o--;
    }
    while( (o > 0) && ((sP->instP->steps[pitch] << o) >= (1L << 17)) )
    {
        o--;
    }
    *octP = o;
    return pitch;
}

// seqZone: the table (or sample) of a pitch at the converter's octave:
// seqPitch's pitch's, or for a note still shifted up zone 6's, or the top
// zone's, cut for ZONE_TOP, in a voice without zone 6.
static long seqZone(const Sequencer *sP, int pitch)
{
    int o;

    pitch = seqPitch(sP, pitch, &o);
    if( o <= 0 )
    {
        return sP->instP->zoneMap[pitch];
    }
    return ((sP->instP->zone6 != 0) ? sP->instP->zone6 : sP->instP->zoneMap[LIB_PITCHES - 1]);
}

// seqStep: the step of a pitch at the converter's octave, taken at
// seqPitch's pitch. The player shifts it by the octaves left: sar to go
// down, ral (an 18-bit rotate) to go up.
static long seqStep(const Sequencer *sP, int pitch)
{
    long s;
    int o;
    int k;

    s = sP->instP->steps[seqPitch(sP, pitch, &o)];
    if( o < 0 )
    {
        s >>= (-o);
    }
    for(k = 0; k < o; k++)
    {
        s = (((s << 1) | (s >> 17)) & WORD_MASK);
    }
    if( (s <= 0) || (s >= (1L << 17)) )
    {
        stepsBeyond++;
    }
    return s;
}

// seqEvent: what the converter makes of an event at cycle t: a strike, a
// slur (the step changes, the envelope carries on), a key off, or a mark
// at a bar's end, which only closes the entry.
static void seqEvent(Sequencer *sP, long long t, int kind, int pitch)
{
    long table;

    if( (kind == EV_OFF) && !sP->sounding )
    {
        return;
    }
    if( kind == EV_SLUR )
    {
        if( !sP->sounding )
        {
            kind = EV_STRIKE;
        }
        else if( pitch == sP->pitch )
        {
            return;
        }
        else if( sP->sus == NO_SUSTAIN )
        {
            // With no sustain level the note would die away under the slur.
            kind = EV_STRIKE;
        }
        else if( sP->instP->own && (seqZone(sP, pitch) != seqZone(sP, sP->pitch)) )
        {
            // Another block's end, loop and stride: latched with its
            // table, they would cut the note sounding, so it is struck.
            kind = EV_STRIKE;
        }
    }
    if( kind == EV_STRIKE )
    {
        sP->pitch = pitch;
        sP->sounding = 1;
        seqBoundary(sP, t);
        seqMerge(sP, seqZone(sP, pitch), seqStep(sP, pitch),
                 (sP->sus | sP->held | ACT_KEYON | ACT_PHASE));
    }
    else if( kind == EV_SLUR )
    {
        table = ((seqZone(sP, pitch) != seqZone(sP, sP->pitch)) ? seqZone(sP, pitch) : 0);
        sP->pitch = pitch;
        seqBoundary(sP, t);
        seqMerge(sP, table, seqStep(sP, pitch), 0);
    }
    else if( kind == EV_OFF )
    {
        sP->sounding = 0;
        seqBoundary(sP, t);
        seqMerge(sP, 0, 0, (sP->sus | sP->held | ACT_KEYOFF));
    }
    else
    {
        seqBoundary(sP, t);
    }
}

// seqFinish: the last entry: silence, repeated for ever.
static void seqFinish(Sequencer *sP)
{
    seqBoundary(sP, sP->last + MAX_EVENT);
}

// feedStep: one step of the timeline (a bar or a pause) to every
// converter: its part's events, then a mark at the step's end.
static void feedStep(EventList *evsP, int nv, long long end16)
{
    Sequencer *sP;
    long i;
    int c;

    for(c = 0; c < NCONV; c++)
    {
        sP = &seqs[c];
        if( (sP->part >= 0) && (sP->part < nv) )
        {
            for(i = 0; i < evsP[sP->part].n; i++)
            {
                seqEvent(sP, (evsP[sP->part].evP[i].t16 >> 4), evsP[sP->part].evP[i].kind,
                         evsP[sP->part].evP[i].pitch);
            }
        }
        seqEvent(sP, (end16 >> 4), EV_MARK, 0);
    }
}

// playedBars: a part's bars in the order of play, each its first note
// word's index; the voice is over at the closing 600000, past the notes,
// or at a pointer to a bar line.
// Returns the count; *startsPP is a new array.
static int playedBars(const Part *pP, int **startsPP)
{
    int *sP;
    int n;
    int i;
    int p;

    sP = growOrFail(NULL, ((size_t)pP->nBars + 1) * sizeof(int));
    n = 0;
    for(i = 0; i < pP->nBars; i++)
    {
        p = pP->barsP[i];
        if( (p == BAR) || (p >= pP->nNotes) || (pP->notesP[p] == BAR) )
        {
            break;
        }
        sP[n] = p;
        n++;
    }
    *startsPP = sP;
    return n;
}

// sequencePiece: the piece's timeline, step by step into the sequencers:
// every per parts are a movement; every bar starts on all voices together
// and lasts as long as its longest; a tempo word changes every voice's
// tempo from the score time where its voice reaches it; the movements are
// PAUSE cycles apart.
// Returns the piece's length in 1/16 cycle, to the end of the last pause.
static long long sequencePiece(int *nMovementsP)
{
    EventList evs[NCONV];
    BarNote *notesP;
    Change *changesP;
    Seg *segsP;
    int *startsP[NCONV];
    int nStarts[NCONV];
    int legato[NCONV];
    long ends[NCONV];
    long capNotes;
    long capChanges;
    long nNotes;
    long nChanges;
    long long t16;
    long o;
    long u;
    long length;
    long silent;
    long sound;
    long i;
    long j;
    int tempo;
    int nMov;
    int m;
    int nv;
    int v;
    int k;
    int nBars;
    int w;
    int art;
    int pitch;
    Change ch;

    memset(evs, 0, sizeof(evs));
    notesP = NULL;
    changesP = NULL;
    segsP = NULL;
    capNotes = 0;
    capChanges = 0;
    tempo = DEFAULT_TEMPO;
    t16 = 0;
    nMov = ((nParts + per - 1) / per);
    for(m = 0; m < nMov; m++)
    {
        nv = (((nParts - (m * per)) < per) ? (nParts - (m * per)) : per);
        nBars = 0;
        for(v = 0; v < nv; v++)
        {
            nStarts[v] = playedBars(&parts[(m * per) + v], &startsP[v]);
            if( nStarts[v] > nBars )
            {
                nBars = nStarts[v];
            }
            legato[v] = 0;
        }
        for(k = 0; k < nBars; k++)
        {
            // The bar's notes and tempo words, voice by voice, at their offsets in units.
            nNotes = 0;
            nChanges = 0;
            for(v = 0; v < nv; v++)
            {
                o = 0;
                if( k < nStarts[v] )
                {
                    for(i = startsP[v][k]; (i < parts[(m * per) + v].nNotes)
                        && (parts[(m * per) + v].notesP[i] != BAR); i++)
                    {
                        w = parts[(m * per) + v].notesP[i];
                        if( (w & TEMPO_MARK) == TEMPO_MARK )
                        {
                            if( nChanges == capChanges )
                            {
                                capChanges = ((capChanges == 0) ? 16 : (capChanges * 2));
                                changesP = growOrFail(changesP, (size_t)capChanges * sizeof(Change));
                                segsP = growOrFail(segsP, ((size_t)capChanges + 1) * sizeof(Seg));
                            }
                            changesP[nChanges].o = o;
                            changesP[nChanges].v = v;
                            changesP[nChanges].tempo = (w & 077777);
                            nChanges++;
                            continue;
                        }
                        u = ((long)(w & 0177) * (((w >> 15) & 1) ? 2 : 3) * 8);
                        if( u <= 0 )
                        {
                            continue;
                        }
                        if( nNotes == capNotes )
                        {
                            capNotes = ((capNotes == 0) ? 64 : (capNotes * 2));
                            notesP = growOrFail(notesP, (size_t)capNotes * sizeof(BarNote));
                        }
                        notesP[nNotes].v = v;
                        notesP[nNotes].o = o;
                        notesP[nNotes].u = u;
                        notesP[nNotes].art = ((((w >> 16) & 3) << 2) | ((w >> 13) & 3));
                        notesP[nNotes].pitch = ((w >> 7) & 077);
                        nNotes++;
                        o += u;
                    }
                }
                ends[v] = o;
            }
            length = 0;
            for(v = 0; v < nv; v++)
            {
                if( ends[v] > length )
                {
                    length = ends[v];
                }
            }
            // The tempo segments: the changes in order of offset, then voice (stable).
            for(i = 1; i < nChanges; i++)
            {
                ch = changesP[i];
                for(j = (i - 1); (j >= 0) && ((changesP[j].o > ch.o)
                    || ((changesP[j].o == ch.o) && (changesP[j].v > ch.v))); j--)
                {
                    changesP[j + 1] = changesP[j];
                }
                changesP[j + 1] = ch;
            }
            if( !segsP )
            {
                segsP = growOrFail(segsP, sizeof(Seg));
            }
            segsP[0].o = 0;
            segsP[0].tempo = tempo;
            for(i = 0; i < nChanges; i++)
            {
                segsP[i + 1].o = changesP[i].o;
                segsP[i + 1].tempo = changesP[i].tempo;
                tempo = changesP[i].tempo;
            }
            // Each note: a strike, or a slur after a legato note; then its key off,
            // unless the silence it leaves is under MIN_GAP cycles.
            for(v = 0; v < nv; v++)
            {
                evs[v].n = 0;
            }
            for(i = 0; i < nNotes; i++)
            {
                v = notesP[i].v;
                o = notesP[i].o;
                u = notesP[i].u;
                art = notesP[i].art;
                pitch = notesP[i].pitch;
                if( pitch <= 1 )
                {
                    addEvent(&evs[v], atOffset(t16, segsP, (int)nChanges + 1, o), EV_OFF, 0);
                    legato[v] = 0;
                    continue;
                }
                silent = silentUnits(art, u);
                sound = (u - silent);
                addEvent(&evs[v], atOffset(t16, segsP, (int)nChanges + 1, o), (legato[v] ? EV_SLUR : EV_STRIKE),
                         pitch);
                legato[v] = (silent == 0);
                if( (silent > 0)
                    && (((atOffset(t16, segsP, (int)nChanges + 1, o + u) >> 4)
                         - (atOffset(t16, segsP, (int)nChanges + 1, o + sound) >> 4)) >= MIN_GAP) )
                {
                    addEvent(&evs[v], atOffset(t16, segsP, (int)nChanges + 1, o + sound), EV_OFF, 0);
                }
            }
            // A voice shorter than the bar rests to its end.
            for(v = 0; v < nv; v++)
            {
                if( ends[v] < length )
                {
                    addEvent(&evs[v], atOffset(t16, segsP, (int)nChanges + 1, ends[v]), EV_OFF, 0);
                    legato[v] = 0;
                }
            }
            t16 = atOffset(t16, segsP, (int)nChanges + 1, length);
            feedStep(evs, nv, t16);
        }
        // The movement's end: every voice off at its last bar's end, then the pause.
        for(v = 0; v < nv; v++)
        {
            evs[v].n = 0;
            addEvent(&evs[v], t16, EV_OFF, 0);
            free(startsP[v]);
        }
        feedStep(evs, nv, t16 + ((long long)PAUSE * 16));
        t16 += ((long long)PAUSE * 16);
    }
    for(v = 0; v < NCONV; v++)
    {
        free(evs[v].evP);
    }
    free(notesP);
    free(changesP);
    free(segsP);
    *nMovementsP = nMov;
    return t16;
}

// The converters.

// volumeOf: the volume, 1/64 dB, that dzg heads for with value.
static long volumeOf(long value)
{
    long v;

    v = (value & 0777);
    return ((v == MUTE) ? SILENT : (v << 3));
}

// gainOf: the gain of an attenuation's integer part plus the volume.
static double gainOf(long index)
{
    return ((index < SILENT) ? libGain[index] : 0.0);
}

// envAdvance: one cycle of the envelope: a stage moves the attenuation by
// its rate, and on reaching its target passes to the next stage.
static void envAdvance(Converter *cP)
{
    long sign;
    long rate;
    long target;
    long dist;
    int next;

    if( cP->stage == STAGE_ATTACK )
    {
        sign = -1;
        rate = cP->attack;
        target = (cP->peak << 15);
        next = STAGE_DECAY;
    }
    else if( cP->stage == STAGE_DECAY )
    {
        sign = 1;
        rate = cP->decay;
        if( cP->sustain == LEVEL_NONE )
        {
            target = FLOOR;
            next = STAGE_OFF;
        }
        else
        {
            target = (cP->sustain << 15);
            next = STAGE_SUSTAIN;
        }
    }
    else if( cP->stage == STAGE_RELEASE )
    {
        sign = 1;
        rate = cP->release;
        target = FLOOR;
        next = STAGE_OFF;
    }
    else
    {
        return;
    }
    dist = ((sign < 0) ? (cP->att - target) : (target - cP->att));
    if( (dist <= 0) || ((rate > 0) && (dist <= rate)) )
    {
        cP->att = target;
        cP->stage = next;
    }
    else
    {
        cP->att += (sign * rate);
    }
}

// convTakeNote: the note the latches hold; the one-shot actions are
// consumed, so only the held ones stay latched.
static void convTakeNote(Converter *cP, Note *nP)
{
    int k;

    nP->addr = cP->lAddr;
    nP->length = cP->lLength;
    nP->period = cP->lPeriod;
    nP->step = cP->lStep;
    nP->attack = cP->lAttack;
    nP->decay = cP->lDecay;
    nP->release = cP->lRelease;
    nP->start = cP->lStart;
    nP->peak = cP->lPeak;
    nP->sustain = cP->lSustain;
    nP->actions = cP->lActions;
    nP->end = cP->lEnd;
    nP->loop = cP->lLoop;
    nP->stride = cP->lStride;
    for(k = 0; k < 3; k++)
    {
        nP->th[k] = cP->lTh[k];
    }
    nP->voicing = cP->lVoicing;
    nP->vRate = cP->lVRate;
    nP->vDepth = cP->lVDepth;
    nP->vDelay = cP->lVDelay;
    nP->vFree = cP->lVFree;
    nP->tDepth = cP->lTDepth;
    nP->pDepth = cP->lPDepth;
    nP->kMul = cP->lKMul;
    nP->kLevel = cP->lKLevel;
    nP->kDecay = cP->lKDecay;
    nP->kAddr = cP->lKAddr;
    cP->lActions = (cP->lActions & ACT_KEPT);
}

// convDrop: both reads' words to 0, and any still to come dropped, with
// the pairs the interpolation holds for them.
static void convDrop(Converter *cP)
{
    cP->visible = 0.0;
    cP->pendingValid = 0;
    cP->first = 0.0;
    cP->second = 0.0;
    cP->pending2Valid = 0;
    cP->pairs[READ_FIRST].n = 0;
    cP->pairs[READ_SECOND].n = 0;
}

// floorShift: x shifted right by n, toward minus infinity (C leaves a
// negative's shift to the compiler).
static long long floorShift(long long x, int n)
{
    return ((x >= 0) ? (x >> n) : -((-x + ((1LL << n) - 1)) >> n));
}

// triangle: a triangle of the slow counter's 24-bit phase p, from -16,384
// to 16,384: 0 at 0, rising to 16,384 at a quarter, -16,384 at three.
static long triangle(long p)
{
    long q;

    q = (p >> 8);
    if( q < 16384 )
    {
        return q;
    }
    if( q < 49152 )
    {
        return (32768 - q);
    }
    return (q - 65536);
}

// convForm: the slow counter's terms, from its phase now: the vibrato's m
// and the steps taken, the tremolo, and the place's offsets.
static void convForm(Converter *cP)
{
    long t1;
    long t2;

    t1 = triangle(cP->lfo);
    t2 = triangle((cP->lfo + SLOW_QUARTERS) & SLOW_PHASE);
    cP->m = floorShift((long long)cP->vDepth * t1, 9);
    cP->trem = (((cP->tDepth << 3) * (16384 + t2)) >> 15);
    cP->panL = (((cP->pDepth << 3) * (16384 + t2)) >> 15);
    cP->panR = (((cP->pDepth << 3) * (16384 - t2)) >> 15);
    cP->stepM = (cP->step + (long)floorShift((long long)cP->step * cP->m, 19));
    cP->step2M = (cP->step2 + (long)floorShift((long long)cP->step2 * cP->m, 19));
}

// convMix: the output's sample: with the céleste the two reads' words at
// equal power, else the first's.
static double convMix(const Converter *cP)
{
    return ((cP->celeste != 0) ? ((cP->first + cP->second) * EDGE_CELESTE_MIX) : cP->first);
}

// convLoadNote: a note loads, and its actions apply (phase reset, key
// on, with the slow counter's restart and the percussion's strike, key
// off), the slow counter's terms are formed, then its voicing: with the
// speech, the attack is held to the rise's rate while it lasts, never
// faster than the note's own.
static void convLoadNote(Converter *cP, const Note *nP)
{
    long a;
    int32_t rise;
    int detached;
    int k;

    cP->sample = ((nP->actions & ACT_SAMPLE) != 0);
    cP->addr = nP->addr;
    cP->period = nP->period;
    cP->step = nP->step;
    cP->mask = (nP->length - 1);
    cP->end = nP->end;
    cP->loop = nP->loop;
    cP->stride = nP->stride;
    for(k = 0; k < 3; k++)
    {
        cP->th[k] = nP->th[k];
    }
    cP->attack = nP->attack;
    cP->decay = nP->decay;
    cP->release = nP->release;
    cP->start = nP->start;
    cP->peak = nP->peak;
    cP->sustain = nP->sustain;
    cP->celeste = edgeCeleste(nP->voicing);
    cP->step2 = edgeCelesteStep(nP->step, cP->celeste);
    cP->interp = ((nP->actions & ACT_INTERP) != 0);
    cP->packed = (cP->sample && ((nP->actions & ACT_PACK) != 0));
    cP->vRate = nP->vRate;
    cP->vDepth = nP->vDepth;
    cP->vDelay = nP->vDelay;
    cP->vFree = (nP->vFree != 0);
    cP->tDepth = nP->tDepth;
    cP->pDepth = nP->pDepth;
    cP->kMul = nP->kMul;
    cP->kLevel = nP->kLevel;
    cP->kDecay = nP->kDecay;
    cP->kAddr = nP->kAddr;
    // Before the key on: the percussion strikes only on a note detached.
    detached = ((cP->stage == STAGE_OFF) || (cP->stage == STAGE_RELEASE));
    a = nP->actions;
    if( (a & ACT_PHASE) != 0 )
    {
        cP->phase = 0;
        cP->phase2 = 0;
    }
    if( (a & ACT_KEYON) != 0 )
    {
        cP->att = (cP->start << 15);
        cP->stage = STAGE_ATTACK;
        // The old note's samples go: until the new note's first words, the output is 0.
        convDrop(cP);
        cP->odd = 0;
        if( !cP->vFree )
        {
            cP->lfo = 0;
            cP->hold = (cP->vDelay * SLOW_HOLD);
        }
        if( (cP->kMul != 0) && detached )
        {
            cP->kPhase = 0;
            cP->kAtt = (cP->kLevel << 15);
            cP->kValue = 0.0;
            cP->pendingKValid = 0;
            cP->pairs[READ_PERC].n = 0;
        }
    }
    if( ((a & ACT_KEYOFF) != 0) && (cP->stage != STAGE_OFF) )
    {
        cP->stage = STAGE_RELEASE;
    }
    convForm(cP);
    cP->visible = convMix(cP);
    rise = edgeLoad(&cP->edge, nP->voicing, nP->step, nP->period, nP->start, nP->peak, nP->attack,
                    ((a & ACT_KEYON) != 0));
    if( (rise > 0) && (cP->stage == STAGE_ATTACK) && (rise < cP->attack) )
    {
        cP->attack = rise;
    }
}

// convLoad: an event's end: the event's length and the note load
// together.
static void convLoad(Converter *cP)
{
    Note note;

    cP->evLeft = ((cP->lEvlen != 0) ? cP->lEvlen : MAXLEN);
    convTakeNote(cP, &note);
    convLoadNote(cP, &note);
}

// convLateEvent: an event's end with the onset: the event's length loads
// now, the note d cycles later (now if d is 0), d the sequence's next
// number mod the bound + 1, never past half the event, as the device's
// lateEvent().
static void convLateEvent(Converter *cP)
{
    uint32_t x;
    long d;

    cP->evLeft = ((cP->lEvlen != 0) ? cP->lEvlen : MAXLEN);
    convTakeNote(cP, &cP->held);
    x = cP->onsetRandom;
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    cP->onsetRandom = x;
    d = (long)(x % (uint32_t)(onsetCycles + 1));
    if( d > (cP->evLeft / 2) )
    {
        d = (cP->evLeft / 2);
    }
    if( (d == 0) || ((cP->held.actions & ACT_WAVE) == 0) )
    {
        convLoadNote(cP, &cP->held);
    }
    else
    {
        cP->onsetLeft = d;
    }
}

// convRunEntry: the stream's next entry, as the player latches it; past
// the last, the last again.
static void convRunEntry(Converter *cP)
{
    const Entry *eP;
    long end;
    long loop;
    long stride;
    long i;

    i = ((cP->next < cP->seqP->nEntries) ? cP->next : (cP->seqP->nEntries - 1));
    eP = &cP->seqP->entriesP[i];
    if( eP->dxa != 0 )
    {
        cP->lAddr = (eP->dxa & 0177777);
        if( cP->seqP->instP->own )
        {
            // The table's block's own end, loop and stride, latched after it.
            zoneEnds(cP->seqP->instP, eP->dxa, &end, &loop, &stride);
            cP->lEnd = (((end & 0177777) != 0) ? (end & 0177777) : 0200000);
            cP->lLoop = (loop & 0177777);
            cP->lStride = (stride & 0177777);
        }
    }
    if( eP->dyf != 0 )
    {
        cP->lStep = (eP->dyf & WORD_MASK);
    }
    if( eP->dyx != 0 )
    {
        cP->lSustain = ((eP->dyx >> 9) & 0777);
        cP->lActions = (eP->dyx & ACT_MASK);
    }
    cP->lEvlen = (((eP->len & WORD_MASK) != 0) ? (eP->len & WORD_MASK) : MAXLEN);
    cP->next = (i + 1);
}

// convRead: the word at the address plus offset plus the phase's integer
// part (masked to the table, except in sample mode): bits 0-11, ones'
// complement, as -1..1.
static double convRead(const Converter *cP, long offset, unsigned long phase)
{
    long index;
    long w;

    index = (long)(phase >> 12);
    if( !cP->sample )
    {
        index &= cP->mask;
    }
    w = memWords[(cP->addr + offset + index) & 0177777];
    if( w < 0 )
    {
        unmappedFetches++;
        w = 0;
    }
    if( (w & SIGN_BIT) != 0 )
    {
        return ((double)(-(long)(((unsigned long)~w >> 6) & 03777)) / 2047.0);
    }
    return ((double)((w >> 6) & 03777) / 2047.0);
}

// convTimbre: the timbre at this sample: the thresholds the level has
// reached (0 with no stride).
static long convTimbre(const Converter *cP)
{
    long level;
    long timbre;
    int k;

    timbre = 0;
    if( cP->stride != 0 )
    {
        level = (cP->att >> 15);
        for(k = 0; k < 3; k++)
        {
            if( (cP->th[k] != NO_THRESHOLD) && (level >= cP->th[k]) )
            {
                timbre++;
            }
        }
    }
    return timbre;
}

// convWordValue: the word at addr as the converter's 12-bit sample: bits
// 0-11, ones' complement, -0 as 0; an address no voice fills reads 0.
static long convWordValue(long addr)
{
    long w;

    w = memWords[addr & 0177777];
    if( w < 0 )
    {
        unmappedFetches++;
        return 0;
    }
    if( (w & SIGN_BIT) != 0 )
    {
        return -(long)(((unsigned long)~w >> 6) & 03777);
    }
    return ((w >> 6) & 03777);
}

// convReadPair: read r's sample with the interpolation (READ_FIRST,
// READ_SECOND the céleste's, READ_PERC the percussion's) at phase: the
// word at its index and the next, each kept from the pair the read holds,
// by address, or fetched; the interpolation's product rounded, its shift
// toward minus infinity.
// Returns the sample, as -1..1.
static double convReadPair(Converter *cP, int r, unsigned long phase)
{
    Pair *pairP;
    Pair now;
    long offset;
    long i;
    long j;
    int h;
    int k;

    if( r == READ_PERC )
    {
        i = (long)((phase >> 12) & PERC_MASK);
        now.addr[0] = ((cP->kAddr + i) & 0177777);
        now.addr[1] = ((cP->kAddr + ((i + 1) & PERC_MASK)) & 0177777);
    }
    else
    {
        offset = (cP->addr + (convTimbre(cP) * cP->stride));
        i = (long)(phase >> 12);
        if( cP->sample )
        {
            j = (((i + 1) >= cP->end) ? ((i + 1) - cP->loop) : (i + 1));
        }
        else
        {
            i &= cP->mask;
            j = ((i + 1) & cP->mask);
        }
        now.addr[0] = ((offset + i) & 0177777);
        now.addr[1] = ((offset + j) & 0177777);
    }
    pairP = &cP->pairs[r];
    for(k = 0; k < 2; k++)
    {
        for(h = 0; (h < pairP->n) && (pairP->addr[h] != now.addr[k]); h++)
        {
        }
        now.value[k] = ((h < pairP->n) ? pairP->value[h] : convWordValue(now.addr[k]));
    }
    now.n = 2;
    *pairP = now;
    return ((double)(now.value[0] + floorShift(((long long)(now.value[1] - now.value[0])
                                                * (long long)(phase & FRACTION)) + 2048, 12)) / 2047.0);
}

// rawWord: the word at addr; an address no voice fills reads 0.
static long rawWord(long addr)
{
    long w;

    w = memWords[addr & 0177777];
    if( w < 0 )
    {
        unmappedFetches++;
        return 0;
    }
    return w;
}

// packedNumber: sample i of the packed samples from base, as the
// converter's 12-bit number (ones' complement, -0 as 0): sample 3g is word
// 2g's bits 0-11, 3g + 1 its bits 12-17 then word 2g + 1's 0-5, 3g + 2
// word 2g + 1's bits 6-17.
static long packedNumber(long base, long i)
{
    long g;
    long x;

    g = (i / 3);
    switch( i % 3 )
    {
    case 0:
        x = (rawWord(base + (2 * g)) >> 6);
        break;
    case 1:
        x = (((rawWord(base + (2 * g)) & 077) << 6) | (rawWord(base + (2 * g) + 1) >> 12));
        break;
    default:
        x = rawWord(base + (2 * g) + 1);
        break;
    }
    x &= 07777;
    return (((x & 04000) != 0) ? -(long)((~x) & 03777) : x);
}

// convReadPacked: a packed read's sample at phase (Implementation.md,
// "Recorded instruments"): the end, the loop and the stride in groups,
// the phase in samples; with the interpolation between sample i and the
// next, as convReadPair().
// Memory does not change while it renders, so it reads both words each
// time, the values a held word would give.
// Returns the sample, as -1..1.
static double convReadPacked(const Converter *cP, unsigned long phase)
{
    long base;
    long i;
    long j;
    long s0;
    long s1;

    base = (cP->addr + (2 * convTimbre(cP) * cP->stride));
    i = (long)(phase >> 12);
    s0 = packedNumber(base, i);
    if( !cP->interp )
    {
        return ((double)s0 / 2047.0);
    }
    j = (((i + 1) >= (3 * cP->end)) ? ((i + 1) - (3 * cP->loop)) : (i + 1));
    s1 = packedNumber(base, j);
    return ((double)(s0 + floorShift(((long long)(s1 - s0) * (long long)(phase & FRACTION)) + 2048, 12)) / 2047.0);
}

// convStep: a phase stepped; in sample mode, at or past the end, back by
// the loop's length (a packed voice's three samples a group).
static unsigned long convStep(const Converter *cP, unsigned long phase, long step)
{
    long end;
    long loop;

    end = (cP->packed ? (3 * cP->end) : cP->end);
    loop = (cP->packed ? (3 * cP->loop) : cP->loop);
    phase = ((phase + (unsigned long)step) & PHASE_MASK);
    if( cP->sample && ((long)(phase >> 12) >= end) )
    {
        phase = ((phase - ((unsigned long)loop << 12)) & PHASE_MASK);
    }
    return phase;
}

// convSampleTime: the sample clock: unless the envelope is OFF, the word
// at the address plus the timbre times the stride plus the phase's integer
// part is fetched, and with the céleste at every other such sample time
// from the key on (with the interpolation at every one) the second
// phase's too, and while the percussion sounds its sine's word; each read
// interpolated with the next word if the note holds the interpolation.
// Then the phases step: the first two by the steps the vibrato takes, the
// percussion's by its multiple of the first's.
static void convSampleTime(Converter *cP)
{
    long timbre;

    cP->countdown = cP->period;
    if( cP->stage != STAGE_OFF )
    {
        timbre = convTimbre(cP);
        if( cP->packed )
        {
            cP->pending = convReadPacked(cP, cP->phase);
        }
        else if( cP->interp )
        {
            cP->pending = convReadPair(cP, READ_FIRST, cP->phase);
        }
        else
        {
            cP->pending = convRead(cP, (timbre * cP->stride), cP->phase);
        }
        cP->pendingValid = 1;
        if( (cP->celeste != 0) && (cP->interp || !cP->odd) )
        {
            if( cP->packed )
            {
                cP->pending2 = convReadPacked(cP, cP->phase2);
            }
            else if( cP->interp )
            {
                cP->pending2 = convReadPair(cP, READ_SECOND, cP->phase2);
            }
            else
            {
                cP->pending2 = convRead(cP, (timbre * cP->stride), cP->phase2);
            }
            cP->pending2Valid = 1;
        }
        if( cP->kAtt < FLOOR )
        {
            if( cP->interp )
            {
                cP->pendingK = convReadPair(cP, READ_PERC, cP->kPhase);
            }
            else
            {
                cP->pendingK = ((double)convWordValue(cP->kAddr + (long)((cP->kPhase >> 12) & PERC_MASK)) / 2047.0);
            }
            cP->pendingKValid = 1;
        }
        cP->odd = !cP->odd;
    }
    cP->phase = convStep(cP, cP->phase, cP->stepM);
    cP->phase2 = convStep(cP, cP->phase2, cP->step2M);
    cP->kPhase = ((cP->kPhase + ((unsigned long)cP->kMul * (unsigned long)cP->stepM)) & PERC_PHASE);
}

// convLevel: the output: the fetched sample times the gain of the
// attenuation plus the volume plus the tremolo, then the edge tone, the
// breath and the percussion, each times that gain and the gain of its own
// level (the breath also that of its decay; the percussion's attenuation
// is its level and decay together), summed in doubles, as a 32-bit float.
static float convLevel(const Converter *cP)
{
    const EdgeState *eP;
    double x;
    long base;

    if( cP->stage == STAGE_OFF )
    {
        return 0.0f;
    }
    eP = &cP->edge;
    base = ((cP->att >> 12) + cP->vol + cP->trem);
    x = (cP->visible * gainOf(base));
    if( (eP->voicing & EDGE_MASK) != 0 )
    {
        x += ((((double)eP->edge.y * EDGE_UNIT_D) * gainOf(base)) * gainOf(eP->edgeLevel));
    }
    if( (eP->voicing & EDGE_RISE) != 0 )
    {
        x += (((((double)eP->breath.y * EDGE_UNIT_D) * gainOf(base)) * gainOf(eP->breathLevel))
              * gainOf(eP->breathAtt >> 12));
    }
    if( cP->kAtt < FLOOR )
    {
        x += ((cP->kValue * gainOf(base)) * gainOf(cP->kAtt >> 12));
    }
    return (float)x;
}

// convGo: the latches set once before the start, in the player's order
// (the volume first; before the go it is there at once), the first entry,
// then dxg: everything loads and the first sample is fetched.
// Returns cycle 0's output.
static float convGo(Converter *cP, const Sequencer *sP)
{
    const Instrument *iP;
    long dyl;
    long dyx;
    long end;
    long loop;
    long stride;

    memset(cP, 0, sizeof(*cP));
    cP->seqP = sP;
    cP->att = FLOOR;
    cP->kAtt = FLOOR;
    cP->stage = STAGE_OFF;
    cP->sustain = LEVEL_NONE;
    iP = sP->instP;
    cP->volTarget = volumeOf(sP->volume);
    cP->vol = cP->volTarget;
    cP->lPeriod = (((iP->n & 07777) != 0) ? (iP->n & 07777) : 4096);
    cP->lAddr = (iP->zoneMap[PITCH0] & 0177777);
    cP->lLength = TABLE_LENGTH;
    cP->lStep = 0;
    cP->lAttack = (iP->attack & WORD_MASK);
    cP->lDecay = (iP->decay & WORD_MASK);
    cP->lRelease = (iP->release & WORD_MASK);
    dyl = (((long)iP->start << 9) | iP->peak);
    cP->lStart = ((dyl >> 9) & 0777);
    cP->lPeak = (dyl & 0777);
    dyx = (((long)iP->sustain << 9) | iP->held);
    cP->lSustain = ((dyx >> 9) & 0777);
    cP->lActions = (dyx & ACT_MASK);
    // With each block's own, the first table's block's.
    end = iP->end;
    loop = iP->loop;
    stride = iP->stride;
    if( iP->own )
    {
        zoneEnds(iP, iP->zoneMap[PITCH0], &end, &loop, &stride);
    }
    cP->lEnd = (((end & 0177777) != 0) ? (end & 0177777) : 0200000);
    cP->lLoop = (loop & 0177777);
    cP->lStride = (stride & 0177777);
    cP->lTh[0] = iP->thresholds[0];
    cP->lTh[1] = iP->thresholds[1];
    cP->lTh[2] = iP->thresholds[2];
    cP->lVoicing = (iP->voicing & EDGE_VOICING);
    // Version 5's six, last; 0 (as the latches start) for a voice without them.
    if( iP->hasLatches )
    {
        cP->lVRate = (iP->dyv & WORD_MASK);
        cP->lVDepth = ((iP->dyw >> 9) & 0777);
        cP->lVDelay = ((iP->dyw >> 1) & 0377);
        cP->lVFree = (iP->dyw & 1);
        cP->lTDepth = ((iP->dym >> 9) & 0777);
        cP->lPDepth = (iP->dym & 0777);
        cP->lKLevel = ((iP->dyk >> 9) & 0777);
        cP->lKMul = (iP->dyk & K_MAX);
        cP->lKDecay = (iP->dyj & WORD_MASK);
        cP->lKAddr = (iP->dyp & 0177777);
    }
    cP->next = 0;
    convRunEntry(cP);
    edgeInit(&cP->edge, (int)(cP - convs));
    cP->ticks = 0;
    convLoad(cP);
    cP->onsetLeft = 0;
    cP->onsetRandom = ((uint32_t)((cP - convs) + 1) * ONSET_SEED);
    cP->going = 1;
    convSampleTime(cP);
    convRunEntry(cP);
    return 0.0f;
}

// convPoll: one machine cycle: the volume moves 1/64 dB toward its
// target, then the envelope, the percussion's decay and the breath, a
// note the onset holds, the event timer, the sample clock, at every fourth
// cycle from the go the noise, at every 64th the slow counter and its
// terms, and the output. A fetched word reaches the output at the next
// cycle.
// Returns the cycle's output.
static float convPoll(Converter *cP)
{
    if( cP->pendingValid || cP->pending2Valid )
    {
        if( cP->pendingValid )
        {
            cP->first = cP->pending;
            cP->pendingValid = 0;
        }
        if( cP->pending2Valid )
        {
            cP->second = cP->pending2;
            cP->pending2Valid = 0;
        }
        cP->visible = convMix(cP);
    }
    if( cP->pendingKValid )
    {
        cP->kValue = cP->pendingK;
        cP->pendingKValid = 0;
    }
    if( cP->volTarget > cP->vol )
    {
        cP->vol++;
    }
    else if( cP->volTarget < cP->vol )
    {
        cP->vol--;
    }
    envAdvance(cP);
    if( cP->kAtt < FLOOR )
    {
        // The percussion's decay: at the floor it is gone.
        if( (cP->kAtt + cP->kDecay) >= FLOOR )
        {
            cP->kAtt = FLOOR;
            cP->kValue = 0.0;
            cP->pendingKValid = 0;
            cP->pairs[READ_PERC].n = 0;
        }
        else
        {
            cP->kAtt += cP->kDecay;
        }
    }
    edgePoll(&cP->edge);
    if( cP->stage == STAGE_OFF )
    {
        convDrop(cP);
    }
    if( (cP->onsetLeft > 0) && (--cP->onsetLeft == 0) )
    {
        convLoadNote(cP, &cP->held);
    }
    cP->evLeft--;
    cP->countdown--;
    if( cP->evLeft == 0 )
    {
        if( onsetCycles > 0 )
        {
            convLateEvent(cP);
        }
        else
        {
            convLoad(cP);
        }
        convRunEntry(cP);
    }
    if( cP->countdown == 0 )
    {
        convSampleTime(cP);
    }
    cP->ticks++;
    if( (cP->ticks % EDGE_TICK) == 0 )
    {
        edgeTick(&cP->edge);
    }
    if( (cP->ticks % SLOW_TICK) == 0 )
    {
        // The slow counter's clock: the delay's first, then the rate.
        if( cP->hold > 0 )
        {
            cP->hold--;
        }
        else
        {
            cP->lfo = ((cP->lfo + cP->vRate) & SLOW_PHASE);
        }
        convForm(cP);
    }
    return convLevel(cP);
}

// The file.

// putLe: n bytes of v, little-endian, at pP.
static void putLe(unsigned char *pP, unsigned long v, int n)
{
    int i;

    for(i = 0; i < n; i++)
    {
        pP[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

// writeHeader: the wav file's header for frames stereo 16-bit frames.
// Returns 1, or 0 on a write error.
static int writeHeader(FILE *fP, long frames)
{
    unsigned char h[44];
    unsigned long body;

    body = ((unsigned long)frames * 4UL);
    memcpy(h, "RIFF", 4);
    putLe(h + 4, (36UL + body), 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    putLe(h + 16, 16, 4);
    putLe(h + 20, 1, 2);
    putLe(h + 22, 2, 2);
    putLe(h + 24, RATE, 4);
    putLe(h + 28, (RATE * 4UL), 4);
    putLe(h + 32, 4, 2);
    putLe(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    putLe(h + 40, body, 4);
    return (fwrite(h, 1, sizeof(h), fP) == sizeof(h));
}

// pcmOf: a channel's sample as 16 bits, rounded half to even and clipped
// at 32,767; *clippedP counts the clipped.
static int pcmOf(float x, long *clippedP)
{
    double r;

    r = nearbyint((double)x * 32767.0);
    if( r > 32767.0 )
    {
        (*clippedP)++;
        return 32767;
    }
    if( r < -32767.0 )
    {
        (*clippedP)++;
        return -32767;
    }
    return (int)r;
}

// pcmHeard: as pcmOf, of a sample heard with the room (1/256 of a step,
// roomHeard()'s).
static int pcmHeard(long long heard, long *clippedP)
{
    double r;

    r = nearbyint((double)heard / 256.0);
    if( r > 32767.0 )
    {
        (*clippedP)++;
        return 32767;
    }
    if( r < -32767.0 )
    {
        (*clippedP)++;
        return -32767;
    }
    return (int)r;
}

// placeAttenuation: the attenuation, 1/64 dB, of one channel's 9 bits of
// a dzp word (1/8 dB), MUTE past the gain table, as the device takes it.
static long placeAttenuation(int value)
{
    return ((value == MUTE) ? SILENT : ((long)value << 3));
}

// setPlaceGains: each converter's mixer attenuations for its place. The
// player gives the dzp before the converters start, so they hold from the
// first cycle and no slew is heard.
static void setPlaceGains(void)
{
    int w;
    int c;

    for(c = 0; c < NCONV; c++)
    {
        w = placeWords[places[c] / PLACE_STEP];
        placeAtts[c][0] = placeAttenuation(w >> 9);
        placeAtts[c][1] = placeAttenuation(w & 0777);
    }
}

// mixChannel: one channel of a frame, summed in doubles: converter by
// converter, its four cycles, each its output times its gain, the gain of
// its place's attenuation plus the Leslie's offset at that cycle (pan, 0
// without one), a gain of 0 left out. At the default places (gains of
// exactly 1 and 0) it is the pair's plain sum.
static float mixChannel(float out[NCONV][CYCLES_PER_FRAME], long pan[NCONV][CYCLES_PER_FRAME][2], int ch)
{
    double sum;
    double g;
    int c;
    int j;

    sum = 0.0;
    for(c = 0; c < NCONV; c++)
    {
        for(j = 0; j < CYCLES_PER_FRAME; j++)
        {
            g = gainOf(placeAtts[c][ch] + pan[c][j][ch]);
            if( g != 0.0 )
            {
                sum += ((double)out[c][j] * g);
            }
        }
    }
    return (float)(sum / 8.0);
}

// render: frames 4-cycle frames of the four converters, mixed as the
// device mixes them, each channel the converters through their places'
// gains, halved and averaged over each frame, then tail frames of
// silence, through the room when there is one, into the file.
// Returns 1, or 0 on a write error; *peakP the largest sample, *clippedP
// the samples clipped.
static int render(FILE *fP, long frames, long tail, double *peakP, long *clippedP)
{
    unsigned char *bufP;
    float out[NCONV][CYCLES_PER_FRAME];
    long pan[NCONV][CYCLES_PER_FRAME][2];
    float left;
    float right;
    double peak;
    int64_t dryL;
    int64_t dryR;
    int64_t wetL;
    int64_t wetR;
    int64_t heardL;
    int64_t heardR;
    long f;
    long nBuf;
    int c;
    int j;

    bufP = growOrFail(NULL, (size_t)WAV_FRAMES * 4);
    peak = 0.0;
    *clippedP = 0;
    nBuf = 0;
    for(f = 0; f < (frames + tail); f++)
    {
        left = 0.0f;
        right = 0.0f;
        if( f < frames )
        {
            for(c = 0; c < NCONV; c++)
            {
                for(j = 0; j < CYCLES_PER_FRAME; j++)
                {
                    if( (f == 0) && (j == 0) )
                    {
                        out[c][j] = convGo(&convs[c], &seqs[c]);
                    }
                    else
                    {
                        out[c][j] = convPoll(&convs[c]);
                    }
                    pan[c][j][0] = convs[c].panL;
                    pan[c][j][1] = convs[c].panR;
                }
            }
            left = mixChannel(out, pan, 0);
            right = mixChannel(out, pan, 1);
        }
        if( room.kind == ROOM_OFF )
        {
            if( fabs(left) > peak )
            {
                peak = fabs(left);
            }
            if( fabs(right) > peak )
            {
                peak = fabs(right);
            }
            putLe(bufP + (4 * nBuf), (unsigned long)(pcmOf(left, clippedP) & 0xFFFF), 2);
            putLe(bufP + (4 * nBuf) + 2, (unsigned long)(pcmOf(right, clippedP) & 0xFFFF), 2);
        }
        else
        {
            // The room takes the mix in 1/256 of a step, as the device gives it its frame,
            // and gives back the frame heard, the dry and the wet cut by its trim.
            dryL = (int64_t)nearbyint((double)left * 8388352.0);
            dryR = (int64_t)nearbyint((double)right * 8388352.0);
            roomFrame(&room, dryL, dryR, &wetL, &wetR);
            heardL = roomHeard(&room, dryL, wetL);
            heardR = roomHeard(&room, dryR, wetR);
            if( fabs((double)heardL / 8388352.0) > peak )
            {
                peak = fabs((double)heardL / 8388352.0);
            }
            if( fabs((double)heardR / 8388352.0) > peak )
            {
                peak = fabs((double)heardR / 8388352.0);
            }
            putLe(bufP + (4 * nBuf), (unsigned long)(pcmHeard(heardL, clippedP) & 0xFFFF), 2);
            putLe(bufP + (4 * nBuf) + 2, (unsigned long)(pcmHeard(heardR, clippedP) & 0xFFFF), 2);
        }
        nBuf++;
        if( (nBuf == WAV_FRAMES) || ((f + 1) == (frames + tail)) )
        {
            if( fwrite(bufP, 4, (size_t)nBuf, fP) != (size_t)nBuf )
            {
                free(bufP);
                return 0;
            }
            nBuf = 0;
        }
    }
    free(bufP);
    *peakP = peak;
    return 1;
}

// The command.

// outName: the tape's name with its extension replaced by .wav.
// Returns a new string.
static char *outName(const char *tapeP)
{
    const char *slashP;
    const char *backP;
    const char *dotP;
    char *nameP;
    size_t stem;

    slashP = strrchr(tapeP, '/');
    backP = strrchr(tapeP, '\\');
    if( (backP != NULL) && ((slashP == NULL) || (backP > slashP)) )
    {
        slashP = backP;
    }
    dotP = strrchr(tapeP, '.');
    stem = (((dotP != NULL) && ((slashP == NULL) || (dotP > slashP))) ? (size_t)(dotP - tapeP) : strlen(tapeP));
    nameP = growOrFail(NULL, stem + 5);
    memcpy(nameP, tapeP, stem);
    strcpy(nameP + stem, ".wav");
    return nameP;
}

// usage: the command's form, and exit 1.
static void usage(void)
{
    fprintf(stderr, "usage: dacrender tape.bin [out.wav] [--secs=S] [--voices=file.lib]"
            " [--room=none|hall|busch|voices] [--onset=MS]\n"
            "  plays a tape punched by the D/A converters' music program, with its\n"
            "  orchestra and volumes, into a 16-bit stereo wav file at 50 kHz;\n"
            "  --secs=S renders only the first S seconds; --voices=file.lib plays\n"
            "  the voices that library lists in place of the nine compiled in;\n"
            "  --room=hall or --room=busch puts the device's room after the mix,\n"
            "  --room=voices the one the voices choose (Busch Hall or the hall),\n"
            "  --room=none none; --onset=MS loads each note up to MS ms late, as\n"
            "  the device's daconset. Without them, the room and onset the tape\n"
            "  sets, if it sets them; else none\n");
    exit(1);
}

// main: reads the tape, sequences the piece, renders it into the file.
int main(int argc, char **argv)
{
    TapeReader reader;
    FILE *fP;
    const char *tapeP;
    const char *libP;
    char *wavP;
    double secs;
    double peak;
    long long total16;
    long long cycles;
    long frames;
    long clipped;
    long tail;
    long i;
    int nMov;
    int organ[NCONV];
    int roomKind;
    int roomGiven;
    int onsetGiven;
    int fromVoices;
    int c;
    int k;
    clock_t t0;

    tapeP = NULL;
    wavP = NULL;
    libP = NULL;
    secs = 0.0;
    roomKind = ROOM_OFF;
    roomGiven = 0;
    onsetGiven = 0;
    for(i = 1; i < argc; i++)
    {
        if( strncmp(argv[i], "--secs=", 7) == 0 )
        {
            secs = strtod(argv[i] + 7, NULL);
            if( secs <= 0.0 )
            {
                usage();
            }
        }
        else if( (strncmp(argv[i], "--voices=", 9) == 0) && (argv[i][9] != '\0') )
        {
            libP = (argv[i] + 9);
        }
        else if( strcmp(argv[i], "--room=none") == 0 )
        {
            roomKind = ROOM_OFF;
            roomGiven = 1;
        }
        else if( strcmp(argv[i], "--room=hall") == 0 )
        {
            roomKind = ROOM_HALL;
            roomGiven = 1;
        }
        else if( strcmp(argv[i], "--room=busch") == 0 )
        {
            roomKind = ROOM_BUSCH;
            roomGiven = 1;
        }
        else if( strcmp(argv[i], "--room=voices") == 0 )
        {
            roomKind = ROOM_VOICES;
            roomGiven = 1;
        }
        else if( strncmp(argv[i], "--onset=", 8) == 0 )
        {
            onsetCycles = lrint(strtod(argv[i] + 8, NULL) * CYCLES_PER_MS);
            onsetGiven = 1;
            if( onsetCycles < 0 )
            {
                usage();
            }
        }
        else if( argv[i][0] == '-' )
        {
            usage();
        }
        else if( !tapeP )
        {
            tapeP = argv[i];
        }
        else if( !wavP )
        {
            wavP = argv[i];
        }
        else
        {
            usage();
        }
    }
    if( !tapeP )
    {
        usage();
    }
    if( !wavP )
    {
        wavP = outName(tapeP);
    }
    t0 = clock();

    if( libP != NULL )
    {
        readLibrary(libP);
        printf("voices: %s, %d voice%s\n", libP, nVoices, ((nVoices == 1) ? "" : "s"));
    }
    else
    {
        compiledVoices();
    }

    // The player's default: the organ, 3 parts a movement, every volume 0 dB.
    // A library without it can play only tapes that name their orchestra.
    organ[0] = instByName("principal");
    organ[1] = instByName("bourdon");
    organ[2] = instByName("gedackt");
    organ[3] = organ[1];
    per = DEFAULT_PER;
    orch[0].part = 0;
    orch[1].part = 2;
    orch[2].part = 1;
    orch[3].part = 2;
    for(c = 0; c < NCONV; c++)
    {
        orch[c].inst = organ[c];
        orch[c].stops[0] = organ[c];
        orch[c].nStops = 1;
        orch[c].octave = 0;
        volumes[c] = 0;
        places[c] = defaultPlace(c);
    }
    orch[3].octave = 1;             // 16 feet

    if( !readTapeFile(tapeP, &reader) )
    {
        fprintf(stderr, "dacrender: cannot read %s\n", tapeP);
        return 1;
    }
    loadTape(&reader);
    if( nParts == 0 )
    {
        fprintf(stderr, "dacrender: no parts on %s\n", tapeP);
        return 1;
    }
    for(c = 0; c < NCONV; c++)
    {
        if( orch[c].inst < 0 )
        {
            fprintf(stderr, "dacrender: the tape names no orchestra the library can play, and the library "
                    "lacks the default organ (principal, gedackt, bourdon)\n");
            return 1;
        }
    }

    // The orchestra's voices, each placed once, where the device fetches them.
    for(i = 0; i < MEM_WORDS; i++)
    {
        memWords[i] = -1;
    }
    placeNext = PLACE_START;
    for(c = 0; c < NCONV; c++)
    {
        for(k = 0; (k < c) && ((orch[k].nStops != orch[c].nStops)
                                || (memcmp(orch[k].stops, orch[c].stops, (size_t)orch[c].nStops * sizeof(int)) != 0));
            k++)
        {
        }
        if( k < c )
        {
            placed[c] = placed[k];
        }
        else if( voices[orch[c].inst].stop )
        {
            placeStops(&orch[c], &placed[c]);
        }
        else if( orch[c].nStops > 1 )
        {
            fprintf(stderr, "dacrender: converter %d names %s and other voices; only stops sound together, and the "
                    "player refuses it\n", c + 1, voices[orch[c].inst].nameP);
            return 1;
        }
        else
        {
            placeVoice(orch[c].inst, &placed[c]);
        }
    }

    for(c = 0; c < NCONV; c++)
    {
        memset(&seqs[c], 0, sizeof(seqs[c]));
        seqs[c].part = orch[c].part;
        seqs[c].instP = &placed[c];
        seqs[c].octave = octaveShifts[orch[c].octave];
        seqs[c].volume = volumes[c];
        seqs[c].sus = ((long)seqs[c].instP->sustain << 9);
        seqs[c].held = seqs[c].instP->held;
        seqs[c].pitch = PITCH0;
    }
    total16 = sequencePiece(&nMov);
    for(c = 0; c < NCONV; c++)
    {
        seqFinish(&seqs[c]);
    }

    printf("%s: %d parts, %d a movement: %d movement%s\n", tapeP, nParts, per, nMov, ((nMov == 1) ? "" : "s"));
    for(c = 0; c < NCONV; c++)
    {
        if( orch[c].part < 0 )
        {
            printf("  converter %d (%s): no part\n", c + 1, placeNames[places[c] / PLACE_STEP]);
        }
        else
        {
            printf("  converter %d (%s): part %d, %s", c + 1, placeNames[places[c] / PLACE_STEP], orch[c].part + 1,
                   voices[orch[c].inst].nameP);
            for(k = 1; k < orch[c].nStops; k++)
            {
                printf(" + %s", voices[orch[c].stops[k]].nameP);
            }
            printf(" at %s feet, volume %s%g dB\n", octaveFeet[orch[c].octave], ((volumes[c] > 0) ? "-" : ""),
                   (volumes[c] / 8.0));
        }
    }
    if( articulationsHalted > 0 )
    {
        printf("warning: %ld notes with an articulation pdp1m13 halts on, played as normal "
               "(the player's Continue)\n", articulationsHalted);
    }
    if( stepsBeyond > 0 )
    {
        printf("warning: %ld notes shifted beyond the step's range; played as the player rotates them\n",
               stepsBeyond);
    }

    cycles = (total16 >> 4);
    printf("the piece: %.1f s (%lld cycles, to the end of the last pause)\n", ((double)cycles / CPS), cycles);
    tail = 0;
    // The tape's room and onset, where no flag gives them: the player gives the device the
    // block's settings at PLAY. Its "the configuration's" is the flags' here, so none.
    if( !roomGiven && (tapeRoom != SETTING_CONFIGURED) )
    {
        static const int roomOfCode[ROOM_CODES] = { ROOM_OFF, ROOM_HALL, ROOM_BUSCH, ROOM_VOICES };

        roomKind = roomOfCode[tapeRoom];
    }
    if( !onsetGiven && (tapeOnset != SETTING_CONFIGURED) )
    {
        onsetCycles = ((long)tapeOnset * ONSET_UNIT);
    }
    // The room from the voices: the player starts all four converters together, so the
    // device's choice at each go comes to Busch Hall if any one's voicing asks for it.
    // (The device may hold the hall for the few cycles between the goes; the mix is silent
    // then.)
    fromVoices = (roomKind == ROOM_VOICES);
    if( fromVoices )
    {
        roomKind = ROOM_HALL;
        for(c = 0; c < NCONV; c++)
        {
            if( (placed[c].voicing & EDGE_BUSCH) != 0 )
            {
                roomKind = ROOM_BUSCH;
            }
        }
    }
    if( roomKind != ROOM_OFF )
    {
        printf("the room: %s%s\n", ((roomKind == ROOM_BUSCH) ? "Busch Hall" : "the hall"),
               (fromVoices ? ", from the voices" : ""));
        tail = ((roomKind == ROOM_BUSCH) ? (4 * RATE) : (2 * RATE));
    }
    if( onsetCycles > 0 )
    {
        printf("the onset: each note up to %.1f ms late\n", ((double)onsetCycles / CYCLES_PER_MS));
    }
    if( (secs > 0.0) && ((long long)(secs * CPS) < cycles) )
    {
        cycles = (long long)(secs * CPS);
        tail = 0;
    }
    frames = (long)(cycles / CYCLES_PER_FRAME);
    setPlaceGains();
    roomInit(&room, roomKind);

    if( !(fP = fopen(wavP, "wb")) )
    {
        fprintf(stderr, "dacrender: cannot write %s\n", wavP);
        return 1;
    }
    if( !writeHeader(fP, (frames + tail)) || !render(fP, frames, tail, &peak, &clipped) )
    {
        fclose(fP);
        fprintf(stderr, "dacrender: write error on %s\n", wavP);
        return 1;
    }
    if( fclose(fP) != 0 )
    {
        fprintf(stderr, "dacrender: write error on %s\n", wavP);
        return 1;
    }
    if( unmappedFetches > 0 )
    {
        printf("warning: %lld fetches outside the library, played as 0\n", unmappedFetches);
    }
    printf("%s: %.1f s, %ld frames, peak %.2f, %ld samples clipped, %.1f s to render\n", wavP,
           ((double)(frames + tail) / RATE), (frames + tail), peak, clipped, ((double)(clock() - t0) / CLOCKS_PER_SEC));
    return 0;
}
