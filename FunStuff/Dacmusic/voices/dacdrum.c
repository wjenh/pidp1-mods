// dacdrum: writes a library of voice files to the Type 23 drum file, for
// the D/A converters' music program (../music/dacmusic.am1), which loads
// the voices an orchestra names from it. README.md, "dacdrum", lays out
// the directory and the headers word by word.
//
// The whole drum is written: the directory in field 0, the voices in
// fields 1-31, and zeros everywhere else, so nothing already on the drum
// survives. The file is written beside the drum file under another name
// and renamed onto it, so a running emulator takes it whole at its next
// transfer. Each word is a 32-bit int in the host's order, the 18-bit
// value in its low bits, as the drum's plugin holds it.
//
// The directory is version 3; version 4 when any voice has a voicing (its
// header's word 12, bits 9-17, dzv's IO); version 5 when a voice file is version
// 5 or --version=5 asks. Version 5's directory is version 4's; its voice
// header adds the converter's latches, each block's own end, loop and
// stride, the steps as a block of their own, and zone 6's word. A stop's
// record is the one the player's premix of it makes: sample mode, its end
// and loop.
//
// The header and each block go whole into one field, first-fit in fields
// 1-31, in the library's order, so each is one transfer. The player reads
// a header as its whole slot, HEADER_MAX words, so a header is placed only
// where that read stays in the field, though it takes only its own words.
// A block whose words an earlier block already has is not written again
// but named at that block's place, so a division's speech is on the drum
// once. On version 5 a block of more than a field runs from its place to
// its field's end and on from word 0 of each empty field after it.
//
// The voice files are read as dacrender reads them: readVoice and
// readLibrary are ../render/dacrender.c's, and a change to one is made to
// the other. A voice the player could never load, or a family of stops
// that differ in what their premix must share, is refused, and nothing is
// written unless the whole library is good.
//
// Usage: dacdrum [--version=5] file.lib drumfile
//
// 07-Oct-2026 Claude: written.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777
#define PITCHES 64
#define TABLE_LENGTH 256
#define NO_THRESHOLD 0777
#define ACT_WAVE 1
#define ACT_SAMPLE 16
#define ACT_INTERP 32
#define ACT_PACK 64                 // the packing (sample mode only)
#define PACKED_END_MAX 21845        // a packed end, in groups: 65,535 samples

// The drum.
#define FIELDS 32
#define FIELD_WORDS 4096
#define DRUM_WORDS (FIELDS * FIELD_WORDS)

// The directory and the headers.
#define MAGIC 0414243
#define VERSION 3
#define VERSION_VOICED 4            // a voice has a voicing
#define VERSION_FIVE 5              // recorded instruments' voices
#define DIR_HEAD 6                  // magic, version, serial, V, sum, F
#define MAX_FAMILIES 16
#define FAMILY_WORDS 4              // a family's name: 11 characters and the end
#define ENTRY_BASE (DIR_HEAD + (MAX_FAMILIES * FAMILY_WORDS))
#define NAME_WORDS 6
#define ENTRY_WORDS (NAME_WORDS + 2) // the name, the header's place, the family
#define MAX_SUBLIST 22              // the rows a sublist of the menu shows
#define OTHER "OTHER"
#define RECORD 13                   // the voice's record: the words the player latches from
#define MAX_BLOCKS 16               // the player's slot holds this many
#define CORE_WORDS 45056            // banks 5-15, where the player places the blocks
#define T340_END 037
#define T340_SLASH 057
#define STOP_FLAG 0400000           // an entry's family word: the voice is a stop
#define NZONES 6                    // a stop's zones, an octave each from pitch 2
#define MAX_ZONES (NZONES + 1)      // and zone 6, pitches 64-75, a version 5 voice's
#define HEADER_MAX (RECORD + 1 + (3 * MAX_BLOCKS) + (2 * PITCHES)) // the player's slot, 190 words

// A version 5 header: the record, its six latches,
// B, the steps' place and sum, then BLOCK5 words a block.
#define LATCHES5 6                  // dyv, dyw, dym, dyk, dyj, the percussion's block
#define AT_B5 (RECORD + LATCHES5)
#define AT_STEPS5 (AT_B5 + 1)
#define AT_BLOCKS5 (AT_STEPS5 + 2)
#define BLOCK5 6                    // where, length, sum, end, loop, stride
#define OWN_FLAG 0400000            // B's sign bit: each note latches its block's end, loop and stride

// Version 5's latches (IOT 72's commands 011-016; ../Implementation.md, "The instructions").
#define RATE_MAX 0777777            // the slow counter's rate, the percussion's decay
#define LEVEL_MAX 0777              // a depth or a level, 1/8 dB or the vibrato's unit
#define DELAY_MAX 0377              // the vibrato's delay, in 16 clocks
#define K_MAX 7                     // the percussion's multiple
#define PERC_WORDS 256              // the percussion's sine: the device reads 256 words
#define ADDR_MAX 0177777            // an end, a loop or a stride

// The voicing (IOTs/DAC/edge.h's bits, dzv's IO bits 9-17).
#define EDGE_MASK 07                // the edge's level k: -36 + 2k dB
#define EDGE_RISE 010               // the speech
#define EDGE_BUSCH 020              // Busch Hall
#define EDGE_CELESTE_SHIFT 5        // bits 9-12, the céleste's detune in half cents
#define EDGE_DB_BASE (-36)
#define EDGE_LOWEST (-34)           // a voice file's edge levels, dB, even
#define EDGE_LOUDEST (-22)
#define RISE_PERIODS 10             // the one rise a voice file holds

// Voice files.
#define VOICE_VERSION 5             // the newest; 1 has no stops, 2 no voicing, 3 no céleste, 4 none of 5's
#define MAX_LINE 4096
#define MAX_VOICES 64

// A voice, as its file holds it: its blocks are lengthsP[0..nBlocks-1]
// words, one after another in wordsP; ownP holds each block's own end,
// loop and stride, three a block, the end -1 for a block with none.
typedef struct
{
    const char *nameP;
    const char *familyP;            // NULL: none
    int version;
    int n;
    int sample;
    int stop;                       // a stop: its record sample mode, its blocks tables and speech
    int speech[MAX_ZONES];          // a stop's speech block for each zone, zone 6's the 6th's without it
    long start, peak, attack, decay, sustain, release;
    long stride;
    long thresholds[3];
    long end, loop;
    long steps[PITCHES];
    int zones[PITCHES];
    int zone6;                      // zone 6's block; -1: none
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

static Voice voices[MAX_VOICES];
static int nVoices;
static long drum[DRUM_WORDS];
static int fieldUsed[FIELDS];       // words taken from each field's start
static int drumVersion;             // VERSION, VERSION_VOICED or VERSION_FIVE

// fail: prints the message and exits with status 1.
static void fail(const char *msgP)
{
    fprintf(stderr, "dacdrum: %s\n", msgP);
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

// The voice files (dacrender.c's).

// voiceFail: a voice file's error, at its line, and exit 1.
static void voiceFail(const char *pathP, int line, const char *msgP)
{
    if( line > 0 )
    {
        fprintf(stderr, "dacdrum: %s:%d: %s\n", pathP, line, msgP);
    }
    else
    {
        fprintf(stderr, "dacdrum: %s: %s\n", pathP, msgP);
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
    int seenEdge;
    int seenRise;
    int seenRoom;
    int seenCeleste;
    int seenInterp;
    int seenPack;
    int seenVibrato;
    int seenLeslie;
    int seenPercussion;
    int ck;
    int lineNo;
    int n;
    int i;
    int b;
    int need;
    int timbres;

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
    seenMode = seenEnvelope = seenStride = seenThresholds = seenSample = seenVersion = 0;
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
                n = voiceNumbers(pP, vals, (PITCHES - nSteps), 10);
                if( (n < 0) || (n > (PITCHES - nSteps)) )
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
                n = voiceNumbers(pP, vals, (PITCHES - nZones), 10);
                if( (n < 0) || (n > (PITCHES - nZones)) )
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
            vP->version = version;
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
                if( (vP->familyP != NULL) || (n == 0) )
                {
                    voiceFail(pathP, lineNo, "one family, not empty");
                }
                vP->familyP = strdupOrFail(pP);
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
            n = voiceNumbers(pP, vals, PITCHES, 10);
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
            n = voiceNumbers(pP, vals, PITCHES, 10);
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
            // zone6 B: zone 6's block, pitches 64-75, which only an octave up sounds.
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
            ck = celesteOf(pP);
            if( seenCeleste || (ck == 0) )
            {
                voiceFail(pathP, lineNo, "celeste: 0.5 to 7.5 cents, in half cents");
            }
            vP->voicing |= (ck << EDGE_CELESTE_SHIFT);
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
        || !seenThresholds || (nSteps != PITCHES) || (nZones != PITCHES) || (vP->nBlocks == 0) )
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
    // The edge and the rise follow the note's fundamental, the table's one
    // period; a sample has none.
    if( vP->sample && ((vP->voicing & (EDGE_MASK | EDGE_RISE)) != 0) )
    {
        voiceFail(pathP, 0, "the edge and the rise belong with a table or a stop");
    }
    // A stop: its premix's loop is one table, after the speech; each
    // zone's speech block is the speech and each table TABLE_LENGTH words,
    // as the player reads them into the premix's place.
    if( (vP->zone6 >= 0) && (vP->zone6 >= vP->nBlocks) )
    {
        voiceFail(pathP, 0, "zone 6 names a block the file does not hold");
    }
    if( vP->stop )
    {
        if( (nSpeech != ((vP->zone6 >= 0) ? MAX_ZONES : NZONES)) || (vP->loop != TABLE_LENGTH)
            || (vP->end <= vP->loop) )
        {
            voiceFail(pathP, 0, "a stop: 6 speech blocks, or 7 with zone 6, a loop of 256 words after its speech");
        }
        if( vP->zone6 < 0 )
        {
            vP->speech[NZONES] = vP->speech[NZONES - 1];    // zone 6's premix block, zone 5's speech
        }
        for(i = 0; i < MAX_ZONES; i++)
        {
            if( (vP->speech[i] >= vP->nBlocks) || (lengthsP[vP->speech[i]] != (vP->end - vP->loop)) )
            {
                voiceFail(pathP, 0, "a stop's speech block is not in the file, or not as long as its speech");
            }
        }
        for(i = 0; i < PITCHES; i++)
        {
            if( (vP->zones[i] < vP->nBlocks) && (lengthsP[vP->zones[i]] != TABLE_LENGTH) )
            {
                voiceFail(pathP, 0, "a stop's table is not 256 words");
            }
        }
        if( (vP->zone6 >= 0) && (lengthsP[vP->zone6] != TABLE_LENGTH) )
        {
            voiceFail(pathP, 0, "a stop's table is not 256 words");
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
    for(i = 0; i <= PITCHES; i++)
    {
        // Each pitch's block, then zone 6's.
        b = ((i < PITCHES) ? vP->zones[i] : vP->zone6);
        if( b < 0 )
        {
            continue;
        }
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
            for(i = 0; (i < PITCHES) && (vP->zones[i] != b); i++)
            {
            }
            if( !vP->sample || ((i == PITCHES) && (b != vP->zone6)) )
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
            voiceFail(pathP, 0, "more than 64 voices");
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
        nVoices++;
        free(fullP);
    }
    fclose(fP);
    if( nVoices == 0 )
    {
        voiceFail(pathP, 0, "the library lists no voice");
    }
}

// The drum.

// onesAdd: the PDP-1's add, ones' complement with the end-around carry,
// and a minus zero made plus zero.
static long onesAdd(long a, long b)
{
    long s;

    s = (a + b);
    if( s > WORD_MASK )
    {
        s = ((s + 1) & WORD_MASK);
    }
    return ((s == WORD_MASK) ? 0 : s);
}

// sumOf: the ones' complement sum of n words of the drum from at.
static long sumOf(long at, long n)
{
    long s;
    long i;

    s = 0;
    for(i = 0; i < n; i++)
    {
        s = onesAdd(s, drum[at + i]);
    }
    return s;
}

// pack340: the name as 340 text into outP's nWords words (at most
// NAME_WORDS), as am1's type340 packs an upper case string: three 6-bit
// codes a word, left first, the 340 starting in its upper case set,
// letters 1-26, digits 060-071, space 040, slash 057, then the end code;
// the words after it are 0.
// Returns 1, or 0 if a character is not a letter, digit, space or slash,
// or the name and its end do not fit.
static int pack340(const char *nameP, long *outP, int nWords)
{
    int codes[NAME_WORDS * 3];
    int n;
    int c;
    int i;

    n = 0;
    for(; *nameP != '\0'; nameP++)
    {
        c = (unsigned char)*nameP;
        if( (c >= 'a') && (c <= 'z') )
        {
            c = (c - 'a' + 'A');
        }
        if( n == ((nWords * 3) - 1) )
        {
            return 0;
        }
        if( (c >= 'A') && (c <= 'Z') )
        {
            codes[n] = (c - '@');
        }
        else if( ((c >= '0') && (c <= '9')) || (c == ' ') )
        {
            codes[n] = c;
        }
        else if( c == '/' )
        {
            codes[n] = T340_SLASH;
        }
        else
        {
            return 0;
        }
        n++;
    }
    codes[n] = T340_END;
    n++;
    while( n < (nWords * 3) )
    {
        codes[n] = 0;
        n++;
    }
    for(i = 0; i < nWords; i++)
    {
        outP[i] = ((codes[3 * i] << 12) | (codes[(3 * i) + 1] << 6) | codes[(3 * i) + 2]);
    }
    return 1;
}

// place: n words, first-fit in fields 1-31, where the reach words the
// player reads from there (n or more) stay in the field; only n are taken,
// so the next place may follow at once.
// Returns where they go, field << 12 | address, or -1 if no field has room.
static long place(long n, long reach)
{
    int f;
    long at;

    for(f = 1; f < FIELDS; f++)
    {
        if( (fieldUsed[f] + reach) <= FIELD_WORDS )
        {
            at = (((long)f << 12) | fieldUsed[f]);
            fieldUsed[f] += (int)n;
            return at;
        }
    }
    return -1;
}

// placeLong: a block of n words, more than a field, from the free end of
// the first field whose free end, with the empty fields after it, holds
// it, on from word 0 of each field after it.
// Returns where it starts, field << 12 | address, or -1 if no run of fields has room.
static long placeLong(long n)
{
    long left;
    long at;
    int f;
    int g;

    for(f = 1; f < FIELDS; f++)
    {
        if( fieldUsed[f] == FIELD_WORDS )
        {
            continue;
        }
        left = (n - (FIELD_WORDS - fieldUsed[f]));
        for(g = (f + 1); (left > 0) && (g < FIELDS) && (fieldUsed[g] == 0); g++)
        {
            left -= FIELD_WORDS;
        }
        if( left <= 0 )
        {
            // It takes fields f to g - 1; the last keeps what the block leaves of it.
            at = (((long)f << 12) | fieldUsed[f]);
            n -= (FIELD_WORDS - fieldUsed[f]);
            fieldUsed[f] = FIELD_WORDS;
            for(f++; f < g; f++)
            {
                fieldUsed[f] = (int)((n < FIELD_WORDS) ? n : FIELD_WORDS);
                n -= fieldUsed[f];
            }
            return at;
        }
    }
    return -1;
}

// headerWords: the words of the voice's header on the drum.
static long headerWords(const Voice *vP)
{
    if( drumVersion == VERSION_FIVE )
    {
        return (AT_BLOCKS5 + ((long)BLOCK5 * vP->nBlocks) + PITCHES + 1 + (vP->stop ? MAX_ZONES : 0));
    }
    return (RECORD + 1 + (3L * vP->nBlocks) + (2 * PITCHES) + (vP->stop ? NZONES : 0));
}

// blockValues: block b's end, loop and stride: its own, or the voice's (a
// table's end and loop 0, as its record has them).
static void blockValues(const Voice *vP, int b, long *endP, long *loopP, long *strideP)
{
    if( vP->ownP[3 * b] >= 0 )
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
// block a zone names has its own, other than the voice's.
static int ownEnds(const Voice *vP)
{
    long e;
    long l;
    long s;
    int p;

    for(p = 0; p <= PITCHES; p++)
    {
        if( (p == PITCHES) && (vP->zone6 < 0) )
        {
            break;
        }
        blockValues(vP, ((p < PITCHES) ? vP->zones[p] : vP->zone6), &e, &l, &s);
        if( (e != vP->end) || (l != vP->loop) || (s != vP->stride) )
        {
            return 1;
        }
    }
    return 0;
}

// checkVoice: voice k's limits as the player has them; ends the run if one
// is passed.
static void checkVoice(int k)
{
    const Voice *vP;
    char msg[256];
    long total;
    int b;
    int p;

    vP = &voices[k];
    total = 0;
    for(b = 0; b < vP->nBlocks; b++)
    {
        if( (vP->lengthsP[b] > FIELD_WORDS) && (drumVersion != VERSION_FIVE) )
        {
            snprintf(msg, sizeof(msg), "voice %d (%s): block %d is longer than a field, %d words (version 5 "
                     "takes it)", k, vP->nameP, b, FIELD_WORDS);
            fail(msg);
        }
        total += vP->lengthsP[b];
    }
    if( vP->nBlocks > MAX_BLOCKS )
    {
        snprintf(msg, sizeof(msg), "voice %d (%s): %d blocks; the player holds %d", k, vP->nameP, vP->nBlocks,
                 MAX_BLOCKS);
        fail(msg);
    }
    if( headerWords(vP) > HEADER_MAX )
    {
        snprintf(msg, sizeof(msg), "voice %d (%s): a header of %ld words; the player's slot holds %d", k,
                 vP->nameP, headerWords(vP), HEADER_MAX);
        fail(msg);
    }
    if( vP->stop )
    {
        // The premix: a block of end words a zone. The player reads
        // another stop's header and its table into a zone's speech words
        // before the speech, so they must hold both.
        // A percussion's sine follows the premix. On version 5 the premix
        // has zone 6's block too.
        total = ((((drumVersion == VERSION_FIVE) ? MAX_ZONES : NZONES) * vP->end)
                 + ((vP->pK != 0) ? PERC_WORDS : 0));
        if( (vP->end - vP->loop) < (HEADER_MAX + vP->loop) )
        {
            snprintf(msg, sizeof(msg), "voice %d (%s): a speech of %ld words; the player needs %ld", k, vP->nameP,
                     vP->end - vP->loop, HEADER_MAX + vP->loop);
            fail(msg);
        }
    }
    if( total > CORE_WORDS )
    {
        snprintf(msg, sizeof(msg), "voice %d (%s): %ld words; the player's banks 5-15 hold %d", k, vP->nameP,
                 total, CORE_WORDS);
        fail(msg);
    }
    for(p = 0; p < PITCHES; p++)
    {
        if( vP->steps[p] > WORD_MASK )
        {
            snprintf(msg, sizeof(msg), "voice %d (%s): a step beyond 18 bits", k, vP->nameP);
            fail(msg);
        }
    }
}

// The blocks written so far, for writeVoice to find a block's words again
// (a voice's blocks and, in version 5, its steps).
static long writtenAt[MAX_VOICES * (MAX_BLOCKS + 1)];
static long writtenLength[MAX_VOICES * (MAX_BLOCKS + 1)];
static int nWritten;

// writtenBefore: an earlier block with the n words at wordsP.
// Returns its index into the drum, or -1.
static long writtenBefore(const unsigned long *wordsP, long n)
{
    long i;
    int j;

    for(j = 0; j < nWritten; j++)
    {
        if( writtenLength[j] == n )
        {
            for(i = 0; (i < n) && (drum[writtenAt[j] + i] == (long)wordsP[i]); i++)
            {
            }
            if( i == n )
            {
                return writtenAt[j];
            }
        }
    }
    return -1;
}

// putBlock: the n words at wordsP into the drum for voice k, or the place
// of an earlier block with the same words; a block of more than a field
// runs on into the fields after it.
// Returns its index into the drum.
static long putBlock(const unsigned long *wordsP, long n, int k)
{
    char msg[256];
    long at;
    long loc;
    long i;

    if( (at = writtenBefore(wordsP, n)) >= 0 )
    {
        return at;
    }
    loc = ((n > FIELD_WORDS) ? placeLong(n) : place(n, n));
    if( loc < 0 )
    {
        snprintf(msg, sizeof(msg), "the library does not fit the drum's 31 fields (at voice %d, %s)", k,
                 voices[k].nameP);
        fail(msg);
    }
    at = (((loc >> 12) * FIELD_WORDS) + (loc & 07777));
    for(i = 0; i < n; i++)
    {
        drum[at + i] = (long)wordsP[i];
    }
    writtenAt[nWritten] = at;
    writtenLength[nWritten] = n;
    nWritten++;
    return at;
}

// whereOf: a drum index as field << 12 | address.
static long whereOf(long at)
{
    return (((at / FIELD_WORDS) << 12) | (at % FIELD_WORDS));
}

// writeVoice: voice k's header and blocks into the drum, a block whose
// words are there already named at their place.
// Returns where its header is, field << 12 | address.
static long writeVoice(int k)
{
    const Voice *vP;
    char msg[256];
    unsigned long stepWords[PITCHES];
    long hdr;
    long at;
    long w;
    long n;
    long loc;
    long e;
    long l;
    long s;
    int b;
    int p;

    vP = &voices[k];
    // The player reads every header as its whole slot, HEADER_MAX words.
    n = headerWords(vP);
    if( (loc = place(n, HEADER_MAX)) < 0 )
    {
        snprintf(msg, sizeof(msg), "the library does not fit the drum's 31 fields (at voice %d, %s)", k,
                 vP->nameP);
        fail(msg);
    }
    hdr = (((loc >> 12) * FIELD_WORDS) + (loc & 07777));
    drum[hdr + 0] = vP->n;
    drum[hdr + 1] = 0;                  // the zone map's address: the player's
    drum[hdr + 2] = 0;                  // the steps' address: the player's
    drum[hdr + 3] = vP->attack;
    drum[hdr + 4] = vP->decay;
    drum[hdr + 5] = vP->release;
    drum[hdr + 6] = ((vP->start << 9) | vP->peak);
    drum[hdr + 7] = ((vP->sustain << 9) | ((vP->sample || vP->stop) ? (ACT_WAVE | ACT_SAMPLE) : ACT_WAVE)
                     | (vP->interp ? ACT_INTERP : 0) | (vP->pack ? ACT_PACK : 0));
    drum[hdr + 8] = vP->end;
    drum[hdr + 9] = vP->loop;
    drum[hdr + 10] = vP->stride;
    drum[hdr + 11] = ((vP->thresholds[0] << 9) | vP->thresholds[1]);
    drum[hdr + 12] = ((vP->thresholds[2] << 9) | vP->voicing);
    w = 0;
    if( drumVersion == VERSION_FIVE )
    {
        drum[hdr + RECORD + 0] = vP->vRate;
        drum[hdr + RECORD + 1] = ((vP->vDepth << 9) | (vP->vDelay << 1) | vP->vFree);
        drum[hdr + RECORD + 2] = ((vP->lesA << 9) | vP->lesP);
        drum[hdr + RECORD + 3] = ((vP->pLevel << 9) | vP->pK);
        drum[hdr + RECORD + 4] = vP->pDecay;
        drum[hdr + RECORD + 5] = vP->pBlock;
        drum[hdr + AT_B5] = (vP->nBlocks | (ownEnds(vP) ? OWN_FLAG : 0));
        for(b = 0; b < vP->nBlocks; b++)
        {
            at = putBlock(&vP->wordsP[w], vP->lengthsP[b], k);
            w += vP->lengthsP[b];
            blockValues(vP, b, &e, &l, &s);
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 0] = whereOf(at);
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 1] = vP->lengthsP[b];
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 2] = sumOf(at, vP->lengthsP[b]);
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 3] = e;
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 4] = l;
            drum[hdr + AT_BLOCKS5 + (BLOCK5 * b) + 5] = s;
        }
        // The steps, a block of their own after the voice's.
        for(p = 0; p < PITCHES; p++)
        {
            stepWords[p] = (unsigned long)vP->steps[p];
        }
        at = putBlock(stepWords, PITCHES, k);
        drum[hdr + AT_STEPS5] = whereOf(at);
        drum[hdr + AT_STEPS5 + 1] = sumOf(at, PITCHES);
        at = (hdr + AT_BLOCKS5 + (BLOCK5 * vP->nBlocks));
        for(p = 0; p < PITCHES; p++)
        {
            drum[at + p] = vP->zones[p];
        }
        // Zone 6's word: its block plus 1, 0 for none.
        drum[at + PITCHES] = (vP->zone6 + 1);
        for(p = 0; vP->stop && (p < MAX_ZONES); p++)
        {
            drum[at + PITCHES + 1 + p] = vP->speech[p];
        }
        return whereOf(hdr);
    }
    drum[hdr + RECORD] = vP->nBlocks;
    for(b = 0; b < vP->nBlocks; b++)
    {
        at = putBlock(&vP->wordsP[w], vP->lengthsP[b], k);
        w += vP->lengthsP[b];
        drum[hdr + RECORD + 1 + (3 * b)] = whereOf(at);
        drum[hdr + RECORD + 2 + (3 * b)] = vP->lengthsP[b];
        drum[hdr + RECORD + 3 + (3 * b)] = sumOf(at, vP->lengthsP[b]);
    }
    at = (hdr + RECORD + 1 + (3 * vP->nBlocks));
    for(p = 0; p < PITCHES; p++)
    {
        drum[at + p] = vP->zones[p];
        drum[at + PITCHES + p] = vP->steps[p];
    }
    for(p = 0; vP->stop && (p < NZONES); p++)
    {
        drum[at + (2 * PITCHES) + p] = vP->speech[p];
    }
    return whereOf(hdr);
}

// blockWords: the words of voice vP's block b.
static const unsigned long *blockWords(const Voice *vP, int b)
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

// checkStops: voice k, a stop, against the first stop of its family, j:
// the player adds a converter's stops together only if they are alike in
// N, the record but its addresses and the voicing (which it combines),
// and the steps, and plays the first one's speech, so a family's stops
// must have the same speech too; and so in version 5's lines, the
// percussion's sine with them, the first one's too. A family whose stops
// do not all have zone 6 is alike in zones 0-4's speech only: the player's
// premix of it has no zone 6, and its top block, which pitches 62-63 and
// the notes an octave up play, takes the first stop's 7th speech entry
// (zone 6's, cut for C#7, or zone 5's own without one).
// Ends the run if they differ.
static void checkStops(int j, int k)
{
    const Voice *aP;
    const Voice *bP;
    char msg[256];
    int same;
    int n;
    int i;

    aP = &voices[j];
    bP = &voices[k];
    same = ((aP->n == bP->n) && (aP->start == bP->start) && (aP->peak == bP->peak) && (aP->attack == bP->attack)
            && (aP->decay == bP->decay) && (aP->sustain == bP->sustain) && (aP->release == bP->release)
            && (aP->stride == bP->stride) && (aP->thresholds[0] == bP->thresholds[0])
            && (aP->thresholds[1] == bP->thresholds[1]) && (aP->thresholds[2] == bP->thresholds[2])
            && (aP->end == bP->end) && (aP->loop == bP->loop)
            && (memcmp(aP->steps, bP->steps, sizeof(aP->steps)) == 0)
            && (aP->interp == bP->interp) && (aP->vRate == bP->vRate) && (aP->vDepth == bP->vDepth)
            && (aP->vDelay == bP->vDelay) && (aP->vFree == bP->vFree) && (aP->lesA == bP->lesA)
            && (aP->lesP == bP->lesP) && (aP->pLevel == bP->pLevel) && (aP->pK == bP->pK)
            && (aP->pDecay == bP->pDecay));
    n = (((aP->zone6 >= 0) == (bP->zone6 >= 0)) ? ((aP->zone6 >= 0) ? MAX_ZONES : NZONES) : (NZONES - 1));
    for(i = 0; same && (i < n); i++)
    {
        same = (memcmp(blockWords(aP, aP->speech[i]), blockWords(bP, bP->speech[i]),
                       (size_t)(aP->end - aP->loop) * sizeof(unsigned long)) == 0);
    }
    if( same && (aP->pK != 0) )
    {
        same = (memcmp(blockWords(aP, aP->pBlock), blockWords(bP, bP->pBlock),
                       PERC_WORDS * sizeof(unsigned long)) == 0);
    }
    if( !same )
    {
        snprintf(msg, sizeof(msg), "voice %d (%s) is not alike with voice %d (%s), the first stop of its family:"
                 " n, envelope, sample, thresholds, steps, speech and version 5's lines must be the same", k,
                 bP->nameP, j, aP->nameP);
        fail(msg);
    }
}

// writeFile: the drum to pathP, through a file beside it renamed onto it.
static void writeFile(const char *pathP)
{
    FILE *fP;
    char *tmpP;
    int field[FIELD_WORDS];
    int f;
    int i;

    tmpP = growOrFail(NULL, strlen(pathP) + 16);
    sprintf(tmpP, "%s.dacdrum-new", pathP);
    if( !(fP = fopen(tmpP, "wb")) )
    {
        fprintf(stderr, "dacdrum: cannot write %s\n", tmpP);
        exit(1);
    }
    for(f = 0; f < FIELDS; f++)
    {
        for(i = 0; i < FIELD_WORDS; i++)
        {
            field[i] = (int)drum[((long)f * FIELD_WORDS) + i];
        }
        if( fwrite(field, sizeof(int), FIELD_WORDS, fP) != FIELD_WORDS )
        {
            fclose(fP);
            remove(tmpP);
            fprintf(stderr, "dacdrum: cannot write %s\n", tmpP);
            exit(1);
        }
    }
    if( fclose(fP) != 0 )
    {
        remove(tmpP);
        fprintf(stderr, "dacdrum: cannot write %s\n", tmpP);
        exit(1);
    }
    if( rename(tmpP, pathP) != 0 )
    {
        remove(tmpP);
        fprintf(stderr, "dacdrum: cannot rename %s onto %s\n", tmpP, pathP);
        exit(1);
    }
    free(tmpP);
}

// usage: the command's form, and exit 2.
static void usage(void)
{
    fprintf(stderr, "usage: dacdrum [--version=5] file.lib drumfile\n");
    exit(2);
}

// main: reads and checks the library, then writes the whole drum.
int main(int argc, char **argv)
{
    long names[MAX_VOICES][NAME_WORDS];
    long families[MAX_FAMILIES][FAMILY_WORDS];
    long packed[FAMILY_WORDS];
    long where[MAX_VOICES];
    long serial;
    long dirLen;
    char msg[256];
    const char *familyNames[MAX_FAMILIES];
    int familyOf[MAX_VOICES];
    int members[MAX_FAMILIES];
    int firstStop[MAX_FAMILIES];
    int nFamilies;
    int askFive;
    int k;
    int i;
    int f;
    int last;

    askFive = 0;
    if( (argc == 4) && (strcmp(argv[1], "--version=5") == 0) )
    {
        askFive = 1;
        argv++;
        argc--;
    }
    if( argc != 3 )
    {
        usage();
    }
    if( sizeof(int) != 4 )
    {
        fail("an int is not 32 bits here, and the drum file's words are");
    }
    readLibrary(argv[1]);
    // The lowest version that holds the library, or 5 when asked.
    drumVersion = VERSION;
    for(k = 0; k < nVoices; k++)
    {
        if( voices[k].voicing != 0 )
        {
            drumVersion = VERSION_VOICED;
        }
    }
    for(k = 0; k < nVoices; k++)
    {
        if( askFive || (voices[k].version == VOICE_VERSION) )
        {
            drumVersion = VERSION_FIVE;
        }
    }
    nFamilies = 0;
    for(k = 0; k < nVoices; k++)
    {
        checkVoice(k);
        if( !pack340(voices[k].nameP, names[k], NAME_WORDS) )
        {
            snprintf(msg, sizeof(msg), "voice %d: the name \"%s\" is not 1 to %d letters, digits, spaces and /", k,
                     voices[k].nameP, (NAME_WORDS * 3) - 1);
            fail(msg);
        }
        // Families are told apart by their 340 text, so case does not count.
        if( !pack340(((voices[k].familyP != NULL) ? voices[k].familyP : OTHER), packed, FAMILY_WORDS) )
        {
            snprintf(msg, sizeof(msg), "voice %d: the family \"%s\" is not 1 to %d letters, digits, spaces and /",
                     k, voices[k].familyP, (FAMILY_WORDS * 3) - 1);
            fail(msg);
        }
        for(f = 0; (f < nFamilies) && (memcmp(families[f], packed, sizeof(packed)) != 0); f++)
        {
        }
        if( f == nFamilies )
        {
            if( nFamilies == MAX_FAMILIES )
            {
                snprintf(msg, sizeof(msg), "more than %d families (at voice %d, %s)", MAX_FAMILIES, k,
                         voices[k].nameP);
                fail(msg);
            }
            memcpy(families[f], packed, sizeof(packed));
            familyNames[f] = ((voices[k].familyP != NULL) ? voices[k].familyP : OTHER);
            members[f] = 0;
            firstStop[f] = -1;
            nFamilies++;
        }
        familyOf[k] = f;
        members[f]++;
        if( voices[k].stop )
        {
            if( firstStop[f] < 0 )
            {
                firstStop[f] = k;
            }
            else
            {
                checkStops(firstStop[f], k);
            }
        }
        if( members[f] > MAX_SUBLIST )
        {
            snprintf(msg, sizeof(msg), "the family %s has more than %d voices, the rows a sublist shows",
                     familyNames[f], MAX_SUBLIST);
            fail(msg);
        }
    }

    memset(drum, 0, sizeof(drum));
    memset(fieldUsed, 0, sizeof(fieldUsed));
    nWritten = 0;
    for(k = 0; k < nVoices; k++)
    {
        where[k] = writeVoice(k);
    }
    serial = sumOf(FIELD_WORDS, (long)(FIELDS - 1) * FIELD_WORDS);

    drum[0] = MAGIC;
    drum[1] = drumVersion;
    drum[2] = serial;
    drum[3] = nVoices;
    drum[5] = nFamilies;
    for(f = 0; f < nFamilies; f++)
    {
        for(i = 0; i < FAMILY_WORDS; i++)
        {
            drum[DIR_HEAD + (f * FAMILY_WORDS) + i] = families[f][i];
        }
    }
    for(k = 0; k < nVoices; k++)
    {
        for(i = 0; i < NAME_WORDS; i++)
        {
            drum[ENTRY_BASE + (k * ENTRY_WORDS) + i] = names[k][i];
        }
        drum[ENTRY_BASE + (k * ENTRY_WORDS) + NAME_WORDS] = where[k];
        drum[ENTRY_BASE + (k * ENTRY_WORDS) + NAME_WORDS + 1] = (familyOf[k] | (voices[k].stop ? STOP_FLAG : 0));
    }
    dirLen = (ENTRY_BASE + ((long)nVoices * ENTRY_WORDS));
    drum[4] = onesAdd(sumOf(0, 4), sumOf(5, dirLen - 5));

    writeFile(argv[2]);

    last = 0;
    for(f = 1; f < FIELDS; f++)
    {
        if( fieldUsed[f] > 0 )
        {
            last = f;
        }
    }
    printf("dacdrum: %s -> %s: version %d, %d voices in %d families, serial %06lo, directory %ld words, fields 1-%d\n",
           argv[1], argv[2], drumVersion, nVoices, nFamilies, serial, dirLen, last);
    for(k = 0; k < nVoices; k++)
    {
        printf("  %2d %-20s %-12s header %02lo:%04lo, %d blocks\n", k, voices[k].nameP, familyNames[familyOf[k]],
               (where[k] >> 12), (where[k] & 07777), voices[k].nBlocks);
    }
    return 0;
}
