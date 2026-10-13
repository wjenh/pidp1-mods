// dacvoice: makes a table voice or an organ stop for the D/A converters'
// music program from a definition (the harmonics, how a softer timbre
// darkens, the timbres' levels, the sample period and the envelope, in
// seconds and dB) and writes it as a voice file. README.md has both
// formats and what a voice holds.
//
// A table voice is one 256-word period for each timbre, its pitches cut
// into zones of an octave whose tables hold only the harmonics below 0.45
// of the sample rate at the zone's top pitch, so no note aliases. Zone 6,
// pitches 64-75 cut for C#7, is past the tape's pitches: only a note an
// octave up (the player's pqs) sounds there, and zone 5 is cut for C#6.
// "zones 6" leaves zone 6 out and cuts zone 5 for B6 (pitch 73).
//
// The arithmetic is in doubles, its sums compensated as Python's sum()
// forms them (Neumaier's), so the library's five table voices are remade
// word for word. C99 and libm.
//
// A definition with a frame is an organ stop (README.md, "Stops"). The
// voicing (README.md, "The voicing") makes the file version 3, the céleste
// version 4; the converter for recorded instruments (README.md, "Recorded
// instruments") or zone 6 makes it version 5.
//
// Usage: dacvoice def-file [out.voice]
//
// 07-Oct-2026 Claude: written.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.141592653589793        // the double nearest pi
#define CPS 200000                  // machine cycles a second
#define LENGTH 256                  // words in a table
#define PITCHES 64                  // the player's 6-bit pitch; 0 and 1 are rests
#define PITCH_A4 47                 // 440 Hz; pitch 2 is C1
#define ZONE_PITCHES 12
#define NZONES (((PITCHES - 1 - 2) / ZONE_PITCHES) + 1) // the zones the zone map names, 0-5
#define MAX_ZONES (NZONES + 1)      // and zone 6, which only an octave up reaches
#define ZONE6_LOW 64                // D6: zone 6's pitches, past the tape's
#define ZONE6_TOP 75                // C#7: its top, the highest an octave up sounds with it
#define ZONE5_TOP 63                // C#6: zone 5's top in a voice with zone 6
#define ZONE5_TOP_ALONE 73          // B6: zone 5's top in a voice without one, the highest an octave up sounds there
#define MAX_HARMONICS 256
#define MAX_TIMBRES 4               // the device has three thresholds
#define FULL 2047                   // a converter's full scale
#define PEAK 0.95                   // the loudest sample at full scale
#define LEVEL_NONE 511              // a sustain of none, a threshold of none
#define WORD_MASK 0777777
#define MAX_LINE 4096
#define PER_LINE 8
#define NUMS_PER_LINE 16

// Stops.
#define QUARTER (PEAK * (double)FULL / 4.0) // a stop's loudest sample at level 0
#define SPEECH 1024                 // the speech block's words, before the premix's loop
#define MAX_RANKS 8
#define CHIFF_PARTIALS 64           // the chiff's sinusoids, each at a random frequency in its band
#define CHIFF_RISE 32               // words: the chiff's raised cosine rise
#define CHIFF_FADE 256              // words: its fade to 0 at the speech block's end
#define CHIFF_SEED 1958u            // the Flentrop's year; the same in every zone

// The voicing.
#define EDGE_LOWEST (-34)           // the edge's levels, dB, even
#define EDGE_LOUDEST (-22)
#define RISE_PERIODS 10             // the one rise the speech holds

// The converter for recorded instruments.
#define CLOCK_HZ 3125.0             // the slow counter's clock: every 64th cycle
#define PHASE_ONE 16777216.0        // the counter's 24-bit phase
#define DEPTH_ONE 16384.0           // a depth of this would swing the step by all of it
#define DELAY_CLOCKS 16.0           // the delay's unit, in clocks: 5.12 ms
#define RATE_MAX 0777777L           // dyv's and dyj's 18 bits
#define DELAY_MAX 0377L
#define K_MAX 7                     // the percussion's multiple, 3 bits

// What a definition sets.
typedef struct
{
    char name[128];
    char family[128];               // empty: none, which the drum calls OTHER
    int n;
    int nHarm;
    int harm[MAX_HARMONICS];
    double amp[MAX_HARMONICS];
    double bright;
    int nTimbres;
    double timbres[MAX_TIMBRES];
    double startDb;
    double attackS;
    double peakDb;
    double decayDbs;
    int hasSustain;
    double sustainDb;
    double releaseS;
    int frame;                      // a stop's frame, 8 or 16 feet; 0: not a stop
    int nRanks;
    int rankH[MAX_RANKS];           // the frame harmonic of the rank's pipe in zone 0
    double rankLevel[MAX_RANKS];
    unsigned rankBreaks[MAX_RANKS]; // bit z: the rank breaks back an octave at zone z
    int rankNyquist[MAX_RANKS];     // the rank's own pitch kept to half the sample rate, not 0.45
    double levelDb;                 // under QUARTER
    double chiffS;                  // the chiff's decay to 1/e at its zone's lowest pitch
    double chiffDb;                 // its peak under QUARTER
    double chiffLo;                 // its band, in the frame's harmonics
    double chiffHi;
    int edgeDb;                     // the edge tone's level; 0: none
    int rise;                       // the speech's rise in periods; 0: none
    int busch;                      // room busch
    int celeste;                    // the céleste's detune, half cents; 0: none
    double vibratoHz;               // the slow counter's rate
    double vibratoCents;            // the vibrato's depth at its peak
    double vibratoDelayS;
    int vibratoFree;                // the counter runs on from note to note
    double leslieDb;                // the tremolo's depth
    double placeDb;                 // the place's depth
    double percDb;                  // the percussion's level under the note
    double percK;                   // its multiple of the step
    double percDecayDbs;
    long vibRate;                   // the latch values, from the above (checkConverter)
    long vibDepth;
    long vibDelay;
    long leslieA;
    long leslieP;
    long percLevel;
    long percDecay;
    int nZones;                     // MAX_ZONES, or NZONES for a voice without zone 6
    unsigned seen;
} Definition;

// A zone's harmonics: their indexes into the definition's.
typedef struct
{
    int nParts;
    int part[MAX_HARMONICS];
} Zone;

// The definition's fields, each a bit of Definition.seen. A table voice
// needs the first eleven; a stop needs them but bright and timbres, which
// it may not have, and frame and chiff; family, rank and level are
// optional, and frame, rank, level and chiff a stop's alone; edge, rise,
// room and celeste, the voicing, are optional for either, as are the
// converter's four, interpolate, vibrato, leslie and percussion, and
// zones, 6 for a voice without zone 6 (7, the default, with it).
static const char *fieldNames[] =
{
    "name", "n", "harmonics", "bright", "timbres", "start", "attack", "peak", "decay", "sustain", "release",
    "family", "frame", "rank", "level", "chiff", "edge", "rise", "room", "celeste",
    "interpolate", "vibrato", "leslie", "percussion", "zones"
};

#define NFIELDS ((int)(sizeof(fieldNames) / sizeof(fieldNames[0])))
#define F_BRIGHT (1u << 3)
#define F_TIMBRES (1u << 4)
#define F_FRAME (1u << 12)
#define F_CHIFF (1u << 15)
#define F_CELESTE (1u << 19)
#define VOICING ((1u << 16) | (1u << 17) | (1u << 18) | F_CELESTE)
#define F_INTERP (1u << 20)
#define F_VIBRATO (1u << 21)
#define F_LESLIE (1u << 22)
#define F_PERC (1u << 23)
#define CONVERTER (F_INTERP | F_VIBRATO | F_LESLIE | F_PERC)
#define TABLE_REQUIRED 03777u
#define STOP_REQUIRED ((TABLE_REQUIRED & ~(F_BRIGHT | F_TIMBRES)) | F_FRAME | F_CHIFF)
#define STOP_ONLY (F_FRAME | (1u << 13) | (1u << 14) | F_CHIFF)

static const char *defPathP;
static int defLine;

// fail: the message, at the definition's line when there is one, and exit 1.
static void fail(const char *msgP, const char *argP)
{
    if( defLine > 0 )
    {
        fprintf(stderr, "dacvoice: %s:%d: ", defPathP, defLine);
    }
    else
    {
        fprintf(stderr, "dacvoice: ");
    }
    fprintf(stderr, msgP, argP);
    fprintf(stderr, "\n");
    exit(1);
}

// pySum: the sum of n doubles as Python's sum() forms it from a start of
// 0: the first term exactly, then Neumaier's compensated sum, the
// compensation added at the end.
static double pySum(const double *xP, int n)
{
    double s;
    double c;
    double t;
    int i;

    if( n == 0 )
    {
        return 0.0;
    }
    s = (0.0 + xP[0]);
    c = 0.0;
    for(i = 1; i < n; i++)
    {
        t = (s + xP[i]);
        if( fabs(s) >= fabs(xP[i]) )
        {
            c += ((s - t) + xP[i]);
        }
        else
        {
            c += ((xP[i] - t) + s);
        }
        s = t;
    }
    if( (c != 0.0) && isfinite(c) )
    {
        s += c;
    }
    return s;
}

// roundInt: rounded half to even, as Python's round() rounds.
static long roundInt(double x)
{
    return (long)nearbyint(x);
}

// freq: equal temperament, A4 at 440 Hz.
static double freq(int p)
{
    return (440.0 * pow(2.0, ((double)(p - PITCH_A4) / 12.0)));
}

// zoneOf: a pitch's zone, an octave from pitch 2.
static int zoneOf(int p)
{
    return (((p > 2) ? (p - 2) : 0) / ZONE_PITCHES);
}

// zoneTop: the pitch zone z's harmonics are cut for, its octave's top:
// zone 6's C#7, and zone 5's C#6 with it, B6 without it.
static int zoneTop(int z, int nZones)
{
    int t;
    int top;

    if( z == NZONES )
    {
        return ZONE6_TOP;
    }
    top = ((nZones == MAX_ZONES) ? ZONE5_TOP : ZONE5_TOP_ALONE);
    t = (2 + (ZONE_PITCHES * z) + (ZONE_PITCHES - 1));
    return ((t < top) ? t : top);
}

// zoneLow: zone z's lowest pitch.
static int zoneLow(int z)
{
    return ((z == NZONES) ? ZONE6_LOW : (2 + (ZONE_PITCHES * z)));
}

// levelOf: a 9-bit level, 1/8 dB, for db of attenuation.
static long levelOf(double db)
{
    return roundInt(db * 8.0);
}

// rateOf: the envelope's rate units for db_per_s.
static long rateOf(double dbPerS)
{
    return roundInt(((dbPerS / (double)CPS) * 262144.0));
}

// wordOf: a sample, -FULL..FULL, as the converters read it: bits 0-11 of
// an 18-bit ones' complement word.
static long wordOf(long sample)
{
    long w;

    w = (labs(sample) << 6);
    return ((sample < 0) ? ((~w) & WORD_MASK) : w);
}

// The definition.

// numberOf: the token as a double, or fail.
static double numberOf(const char *tokP)
{
    char *endP;
    double v;

    v = strtod(tokP, &endP);
    if( (endP == tokP) || (*endP != '\0') || !isfinite(v) )
    {
        fail("not a number: %s", tokP);
    }
    return v;
}

// unitIs: whether the next token, if any, is the unit the field takes;
// a field may leave its unit out.
static void unitIs(const char *tokP, const char *unitP)
{
    if( (tokP != NULL) && (strcmp(tokP, unitP) != 0) )
    {
        fail("the unit here is %s", unitP);
    }
}

// skipUnit: past toks[*kP] if it is the unit; inside a line a unit may be
// left out, and what follows is then the next value.
static void skipUnit(char **toks, int nToks, int *kP, const char *unitP)
{
    if( (*kP < nToks) && (strcmp(toks[*kP], unitP) == 0) )
    {
        (*kP)++;
    }
}

// valueAt: toks[*kP] as a number, *kP past it; fails with formP when the
// line has ended.
static double valueAt(char **toks, int nToks, int *kP, const char *formP)
{
    if( *kP >= nToks )
    {
        fail("%s", formP);
    }
    (*kP)++;
    return numberOf(toks[*kP - 1]);
}

// celesteOf: the céleste's detune k in half cents from pP: cents from
// 0.5 to 7.5 in half cents (2, 2.5, 2.50), nothing after.
// Returns k, 1 to 15, or 0 for anything else.
static int celesteOf(const char *pP)
{
    long whole;
    int half;
    int digits;

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
        if( (*pP != '5') && (*pP != '0') )
        {
            return 0;
        }
        half = (*pP++ == '5');
        while( *pP == '0' )
        {
            pP++;
        }
    }
    if( (digits == 0) || (*pP != '\0') || (((whole * 2) + half) < 1) || (((whole * 2) + half) > 15) )
    {
        return 0;
    }
    return (int)((whole * 2) + half);
}

// readDefinition: the definition at pathP into *dP; fails on a field it
// does not know, a required field missing, or a value out of range.
static void readDefinition(const char *pathP, Definition *dP)
{
    FILE *fP;
    char line[MAX_LINE];
    char *toks[MAX_LINE / 2];
    char *pP;
    char *hashP;
    char *outP;
    int nToks;
    int f;
    int i;
    int k;
    double h;

    memset(dP, 0, sizeof(*dP));
    dP->nZones = MAX_ZONES;
    defPathP = pathP;
    if( !(fP = fopen(pathP, "r")) )
    {
        fail("cannot read %s", pathP);
    }
    defLine = 0;
    while( fgets(line, sizeof(line), fP) )
    {
        defLine++;
        if( (strchr(line, '\n') == NULL) && !feof(fP) )
        {
            fail("a line longer than %s characters", "4095");
        }
        if( (hashP = strchr(line, '#')) != NULL )
        {
            *hashP = '\0';
        }
        nToks = 0;
        for(pP = strtok(line, " \t\r\n"); pP != NULL; pP = strtok(NULL, " \t\r\n"))
        {
            toks[nToks] = pP;
            nToks++;
        }
        if( nToks == 0 )
        {
            continue;
        }
        for(f = 0; (f < NFIELDS) && (strcmp(toks[0], fieldNames[f]) != 0); f++)
        {
        }
        if( f == NFIELDS )
        {
            fail("no such field: %s", toks[0]);
        }
        // harmonics and rank may take several lines; every other field one.
        if( ((dP->seen & (1u << f)) != 0) && (strcmp(toks[0], "harmonics") != 0) && (strcmp(toks[0], "rank") != 0) )
        {
            fail("%s again", toks[0]);
        }
        dP->seen |= (1u << f);
        if( strcmp(toks[0], "interpolate") == 0 )
        {
            if( nToks != 1 )
            {
                fail("interpolate takes no value%s", "");
            }
            continue;
        }
        if( nToks < 2 )
        {
            fail("%s takes a value", toks[0]);
        }
        if( (strcmp(toks[0], "name") == 0) || (strcmp(toks[0], "family") == 0) )
        {
            // A name or a family is the rest of the line, its words joined by
            // one space.
            outP = ((toks[0][0] == 'n') ? dP->name : dP->family);
            outP[0] = '\0';
            for(i = 1; i < nToks; i++)
            {
                if( (strlen(outP) + strlen(toks[i]) + 2) > sizeof(dP->name) )
                {
                    fail("a name or family longer than %s characters", "126");
                }
                if( i > 1 )
                {
                    strcat(outP, " ");
                }
                strcat(outP, toks[i]);
            }
        }
        else if( strcmp(toks[0], "n") == 0 )
        {
            h = numberOf(toks[1]);
            if( (nToks != 2) || (h != floor(h)) || (h < 1.0) || (h > 4095.0) )
            {
                fail("n is a sample period of 1 to 4095 cycles%s", "");
            }
            dP->n = (int)h;
        }
        else if( strcmp(toks[0], "harmonics") == 0 )
        {
            if( ((nToks - 1) % 2) != 0 )
            {
                fail("harmonics are pairs: a harmonic's number, its amplitude%s", "");
            }
            for(i = 1; i < nToks; i += 2)
            {
                if( dP->nHarm == MAX_HARMONICS )
                {
                    fail("more than %s harmonics", "256");
                }
                h = numberOf(toks[i]);
                if( (h != floor(h)) || (h < 1.0) || (h > (double)(LENGTH / 2)) )
                {
                    fail("a harmonic's number is 1 to 128: %s", toks[i]);
                }
                for(k = 0; k < dP->nHarm; k++)
                {
                    if( dP->harm[k] == (int)h )
                    {
                        fail("harmonic %s again", toks[i]);
                    }
                }
                dP->harm[dP->nHarm] = (int)h;
                dP->amp[dP->nHarm] = numberOf(toks[i + 1]);
                dP->nHarm++;
            }
        }
        else if( strcmp(toks[0], "bright") == 0 )
        {
            dP->bright = numberOf(toks[1]);
        }
        else if( strcmp(toks[0], "timbres") == 0 )
        {
            if( (nToks - 1) > MAX_TIMBRES )
            {
                fail("at most %s timbres", "4");
            }
            for(i = 1; i < nToks; i++)
            {
                dP->timbres[i - 1] = numberOf(toks[i]);
            }
            dP->nTimbres = (nToks - 1);
        }
        else if( strcmp(toks[0], "start") == 0 )
        {
            dP->startDb = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "dB");
        }
        else if( strcmp(toks[0], "attack") == 0 )
        {
            dP->attackS = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "s");
        }
        else if( strcmp(toks[0], "peak") == 0 )
        {
            dP->peakDb = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "dB");
        }
        else if( strcmp(toks[0], "decay") == 0 )
        {
            dP->decayDbs = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "dB/s");
        }
        else if( strcmp(toks[0], "sustain") == 0 )
        {
            if( strcmp(toks[1], "none") == 0 )
            {
                dP->hasSustain = 0;
            }
            else
            {
                dP->hasSustain = 1;
                dP->sustainDb = numberOf(toks[1]);
                unitIs((nToks > 2) ? toks[2] : NULL, "dB");
            }
        }
        else if( strcmp(toks[0], "frame") == 0 )
        {
            h = numberOf(toks[1]);
            if( (nToks != 2) || ((h != 8.0) && (h != 16.0)) )
            {
                fail("a frame is 8 or 16 feet%s", "");
            }
            dP->frame = (int)h;
        }
        else if( strcmp(toks[0], "rank") == 0 )
        {
            // rank H LEVEL [nyquist] [break Z ...]
            if( dP->nRanks == MAX_RANKS )
            {
                fail("more than %s ranks", "8");
            }
            h = numberOf(toks[1]);
            if( (nToks < 3) || (h != floor(h)) || (h < 1.0) || (h > (double)(LENGTH / 2)) )
            {
                fail("a rank is its frame harmonic, 1 to 128, and its level%s", "");
            }
            dP->rankH[dP->nRanks] = (int)h;
            dP->rankLevel[dP->nRanks] = numberOf(toks[2]);
            dP->rankBreaks[dP->nRanks] = 0;
            dP->rankNyquist[dP->nRanks] = 0;
            k = 3;
            if( (nToks > k) && (strcmp(toks[k], "nyquist") == 0) )
            {
                dP->rankNyquist[dP->nRanks] = 1;
                k++;
            }
            if( nToks > k )
            {
                if( (strcmp(toks[k], "break") != 0) || (nToks == (k + 1)) )
                {
                    fail("a rank's breaks follow the word break%s", "");
                }
                for(i = (k + 1); i < nToks; i++)
                {
                    h = numberOf(toks[i]);
                    if( (h != floor(h)) || (h < 1.0) || (h >= (double)MAX_ZONES) )
                    {
                        fail("a break is a zone, 1 to 6: %s", toks[i]);
                    }
                    dP->rankBreaks[dP->nRanks] |= (1u << (int)h);
                }
            }
            dP->nRanks++;
        }
        else if( strcmp(toks[0], "level") == 0 )
        {
            dP->levelDb = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "dB");
            if( dP->levelDb < 0.0 )
            {
                fail("a level is dB under the quarter scale, 0 or more%s", "");
            }
        }
        else if( strcmp(toks[0], "edge") == 0 )
        {
            // edge LEVEL [dB]: the edge tone under a stop at C4.
            h = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "dB");
            if( (h != floor(h)) || (((int)h % 2) != 0) || (h < EDGE_LOWEST) || (h > EDGE_LOUDEST) )
            {
                fail("an edge level is even, -34 to -22 dB: %s", toks[1]);
            }
            dP->edgeDb = (int)h;
        }
        else if( strcmp(toks[0], "rise") == 0 )
        {
            if( (nToks != 2) || (numberOf(toks[1]) != RISE_PERIODS) )
            {
                fail("the rise is 10 periods: %s", toks[1]);
            }
            dP->rise = RISE_PERIODS;
        }
        else if( strcmp(toks[0], "room") == 0 )
        {
            if( (nToks != 2) || ((strcmp(toks[1], "busch") != 0) && (strcmp(toks[1], "hall") != 0)) )
            {
                fail("a room is busch or hall: %s", toks[1]);
            }
            dP->busch = (strcmp(toks[1], "busch") == 0);
        }
        else if( strcmp(toks[0], "celeste") == 0 )
        {
            // celeste CENTS: a second read of the same words that much sharp.
            if( (nToks != 2) || ((dP->celeste = celesteOf(toks[1])) == 0) )
            {
                fail("a céleste is 0.5 to 7.5 cents, in half cents: %s", toks[1]);
            }
        }
        else if( strcmp(toks[0], "vibrato") == 0 )
        {
            // vibrato RATE [Hz] DEPTH [cents] [delay TIME [s] | free]
            static const char form[] = "vibrato takes its rate, its depth, and a delay or free:"
                                       " vibrato 5.5 Hz 20 cents delay 0.3 s";

            k = 1;
            dP->vibratoHz = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "Hz");
            dP->vibratoCents = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "cents");
            if( (k < nToks) && (strcmp(toks[k], "free") == 0) )
            {
                dP->vibratoFree = 1;
                k++;
            }
            else if( (k < nToks) && (strcmp(toks[k], "delay") == 0) )
            {
                k++;
                dP->vibratoDelayS = valueAt(toks, nToks, &k, form);
                skipUnit(toks, nToks, &k, "s");
            }
            if( k != nToks )
            {
                fail("%s", form);
            }
        }
        else if( strcmp(toks[0], "leslie") == 0 )
        {
            // leslie A [dB] P [dB]
            static const char form[] = "leslie takes the tremolo's depth and the place's: leslie 3 dB 6 dB";

            k = 1;
            dP->leslieDb = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "dB");
            dP->placeDb = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "dB");
            if( k != nToks )
            {
                fail("%s", form);
            }
        }
        else if( strcmp(toks[0], "percussion") == 0 )
        {
            // percussion LEVEL [dB] K decay RATE [dB/s]
            static const char form[] = "percussion takes its level, its multiple and its decay:"
                                       " percussion 6 dB 4 decay 12 dB/s";

            k = 1;
            dP->percDb = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "dB");
            dP->percK = valueAt(toks, nToks, &k, form);
            if( (k >= nToks) || (strcmp(toks[k], "decay") != 0) )
            {
                fail("%s", form);
            }
            k++;
            dP->percDecayDbs = valueAt(toks, nToks, &k, form);
            skipUnit(toks, nToks, &k, "dB/s");
            if( k != nToks )
            {
                fail("%s", form);
            }
        }
        else if( strcmp(toks[0], "zones") == 0 )
        {
            // zones 6: no zone 6, zone 5 cut for B6; zones 7, the default.
            h = numberOf(toks[1]);
            if( (nToks != 2) || ((h != (double)NZONES) && (h != (double)MAX_ZONES)) )
            {
                fail("zones is 6 or 7: %s", toks[1]);
            }
            dP->nZones = (int)h;
        }
        else if( strcmp(toks[0], "chiff") == 0 )
        {
            // chiff TIME [s] LEVEL [dB] LO HI
            if( (nToks != 5) && (nToks != 7) )
            {
                fail("chiff takes its time, its level and its band: 0.012 s 16 dB 2 12%s", "");
            }
            dP->chiffS = numberOf(toks[1]);
            k = 2;
            if( nToks == 7 )
            {
                unitIs(toks[k], "s");
                k++;
            }
            dP->chiffDb = numberOf(toks[k]);
            k++;
            if( nToks == 7 )
            {
                unitIs(toks[k], "dB");
                k++;
            }
            dP->chiffLo = numberOf(toks[k]);
            dP->chiffHi = numberOf(toks[k + 1]);
            if( (dP->chiffS <= 0.0) || (dP->chiffDb < 0.0) || (dP->chiffLo <= 0.0) || (dP->chiffHi <= dP->chiffLo) )
            {
                fail("a chiff's time above 0, its level 0 dB or more, its band low to high%s", "");
            }
        }
        else
        {
            dP->releaseS = numberOf(toks[1]);
            unitIs((nToks > 2) ? toks[2] : NULL, "s");
        }
    }
    fclose(fP);
    defLine = 0;
    if( (dP->seen & STOP_ONLY) != 0 )
    {
        if( (dP->seen & F_FRAME) == 0 )
        {
            fail("rank, level and chiff are a stop's: the definition has no frame%s", "");
        }
        if( (dP->seen & (F_BRIGHT | F_TIMBRES)) != 0 )
        {
            fail("a stop has one timbre: no bright, no timbres%s", "");
        }
        for(f = 0; f < NFIELDS; f++)
        {
            if( ((STOP_REQUIRED & (1u << f)) != 0) && ((dP->seen & (1u << f)) == 0) )
            {
                fail("the stop has no %s", fieldNames[f]);
            }
        }
        if( dP->nRanks == 0 )
        {
            // A stop with no rank line is its pipe at the frame's pitch.
            dP->rankH[0] = 1;
            dP->rankLevel[0] = 1.0;
            dP->nRanks = 1;
        }
        dP->nTimbres = 1;
        dP->timbres[0] = 0.0;
        for(i = 0; i < dP->nRanks; i++)
        {
            if( (dP->rankBreaks[i] >> dP->nZones) != 0 )
            {
                fail("a break at zone 6 in a voice without one (zones 6)%s", "");
            }
        }
    }
    for(f = 0; f < NFIELDS; f++)
    {
        if( ((TABLE_REQUIRED & (1u << f)) != 0) && ((dP->seen & (1u << f)) == 0) && (dP->frame == 0) )
        {
            fail("the definition has no %s", fieldNames[f]);
        }
    }
    if( (dP->attackS <= 0.0) || (dP->releaseS <= 0.0) )
    {
        fail("attack and release take a time above 0%s", "");
    }
    if( dP->nTimbres < 1 )
    {
        fail("at least one timbre%s", "");
    }
    for(i = 1; i < dP->nTimbres; i++)
    {
        if( dP->timbres[i] <= dP->timbres[i - 1] )
        {
            fail("the timbres' levels go from loud to soft, each further down%s", "");
        }
    }
}

// The voice.

// wave: zone's waveform at gain into outP, LENGTH samples: each harmonic
// at its amplitude, falling as gain^(1 + bright (h - 1)) so that a softer
// timbre is darker, its phase spread by its number so the peaks do not
// pile up.
static void wave(const Definition *dP, const Zone *zP, int hMax, double gain, double *outP)
{
    static double terms[MAX_HARMONICS];
    double phase;
    int h;
    int i;
    int j;

    for(i = 0; i < LENGTH; i++)
    {
        for(j = 0; j < zP->nParts; j++)
        {
            h = dP->harm[zP->part[j]];
            phase = (((PI * (double)h) * (double)(h - 1)) / (double)hMax);
            terms[j] = ((dP->amp[zP->part[j]] * pow(gain, (1.0 + (dP->bright * (double)(h - 1)))))
                        * sin(((((2.0 * PI) * (double)h) * (double)i) / (double)LENGTH) + phase));
        }
        outP[i] = pySum(terms, zP->nParts);
    }
}

// writeRows: values in rows of perLine, as fmtP prints each.
static void writeRows(FILE *fP, const long *vP, int n, const char *fmtP, int perLine)
{
    int i;

    for(i = 0; i < n; i++)
    {
        fputs((((i % perLine) == 0) ? "    " : " "), fP);
        fprintf(fP, fmtP, vP[i]);
        if( (((i + 1) % perLine) == 0) || ((i + 1) == n) )
        {
            fputc('\n', fP);
        }
    }
}

// writeVoicing: the definition's voicing lines, if it has any.
static void writeVoicing(const Definition *dP, FILE *fP)
{
    if( dP->edgeDb != 0 )
    {
        fprintf(fP, "edge %d\n", dP->edgeDb);
    }
    if( dP->rise != 0 )
    {
        fprintf(fP, "rise %d\n", dP->rise);
    }
    if( dP->busch )
    {
        fprintf(fP, "room busch\n");
    }
    if( dP->celeste != 0 )
    {
        fprintf(fP, (((dP->celeste % 2) == 0) ? "celeste %d\n" : "celeste %d.5\n"), (dP->celeste / 2));
    }
}

// writeConverter: the converter's lines, if the definition has any, as
// latch values; percBlock is the block that holds the percussion's sine.
static void writeConverter(const Definition *dP, int percBlock, FILE *fP)
{
    if( (dP->seen & F_INTERP) != 0 )
    {
        fprintf(fP, "interpolate\n");
    }
    if( (dP->seen & F_VIBRATO) != 0 )
    {
        fprintf(fP, "vibrato %ld %ld ", dP->vibRate, dP->vibDepth);
        if( dP->vibratoFree )
        {
            fprintf(fP, "free\n");
        }
        else
        {
            fprintf(fP, "%ld\n", dP->vibDelay);
        }
    }
    if( (dP->seen & F_LESLIE) != 0 )
    {
        fprintf(fP, "leslie %ld %ld\n", dP->leslieA, dP->leslieP);
    }
    if( (dP->seen & F_PERC) != 0 )
    {
        fprintf(fP, "percussion %ld %d %ld %d\n", dP->percLevel, (int)dP->percK, dP->percDecay, percBlock);
    }
}

// sineWords: the percussion's sine, one LENGTH-word period, its peak at
// amp, into outP.
static void sineWords(double amp, long *outP)
{
    int i;

    for(i = 0; i < LENGTH; i++)
    {
        outP[i] = wordOf(roundInt((amp * sin((((2.0 * PI) * (double)i) / (double)LENGTH)))));
    }
}

// fileVersion: the voice file's version: 5 with the converter's lines or
// zone 6, 4 with the céleste, 3 with the rest of the voicing, else plain
// (1 for a table voice, 2 for a stop).
static int fileVersion(const Definition *dP, int plain)
{
    if( ((dP->seen & CONVERTER) != 0) || (dP->nZones == MAX_ZONES) )
    {
        return 5;
    }
    if( (dP->seen & F_CELESTE) != 0 )
    {
        return 4;
    }
    return (((dP->seen & VOICING) != 0) ? 3 : plain);
}

// writeEnvelope: the voice file's envelope line, the device's latch values.
static void writeEnvelope(const Definition *dP, FILE *fP)
{
    fprintf(fP, "envelope %ld %ld %ld %ld %ld %ld\n", levelOf(dP->startDb), levelOf(dP->peakDb),
            rateOf(((dP->startDb - dP->peakDb) / dP->attackS)), rateOf(dP->decayDbs),
            (dP->hasSustain ? levelOf(dP->sustainDb) : (long)LEVEL_NONE), rateOf((30.0 / dP->releaseS)));
}

// makeVoice: the voice the definition describes, written to fP.
static void makeVoice(const Definition *dP, const char *defNameP, FILE *fP)
{
    static double w[LENGTH];
    static long blockWords[MAX_ZONES][MAX_TIMBRES * LENGTH];
    static long sine[LENGTH];
    Zone zones[MAX_ZONES];
    int zoneBlock[MAX_ZONES];
    int nBlocks;
    long steps[PITCHES];
    long zoneOfPitch[PITCHES];
    long th[3];
    long s;
    double limit;
    double top;
    double big;
    double scale;
    double g;
    int hMax;
    int z;
    int y;
    int i;
    int j;
    int t;

    // Each zone's harmonics: those below 0.45 of the sample rate at its top pitch.
    hMax = 0;
    for(j = 0; j < dP->nHarm; j++)
    {
        if( dP->harm[j] > hMax )
        {
            hMax = dP->harm[j];
        }
    }
    limit = ((0.45 * (double)CPS) / (double)dP->n);
    for(z = 0; z < dP->nZones; z++)
    {
        top = freq(zoneTop(z, dP->nZones));
        zones[z].nParts = 0;
        for(j = 0; j < dP->nHarm; j++)
        {
            if( ((double)dP->harm[j] * top) < limit )
            {
                zones[z].part[zones[z].nParts] = j;
                zones[z].nParts++;
            }
        }
        if( zones[z].nParts == 0 )
        {
            fail("no harmonic of %s is below 0.45 of the sample rate in the top zone; a smaller n", dP->name);
        }
    }

    // One scale for every zone: the loudest zone's peak at PEAK of full scale.
    big = 0.0;
    for(z = 0; z < dP->nZones; z++)
    {
        wave(dP, &zones[z], hMax, 1.0, w);
        for(i = 0; i < LENGTH; i++)
        {
            if( fabs(w[i]) > big )
            {
                big = fabs(w[i]);
            }
        }
    }
    scale = ((PEAK * (double)FULL) / big);

    // The blocks: a zone's timbres one after another; a zone with the
    // same harmonics as an earlier one shares its block.
    nBlocks = 0;
    for(z = 0; z < dP->nZones; z++)
    {
        for(y = 0; y < z; y++)
        {
            if( (zones[y].nParts == zones[z].nParts)
                && (memcmp(zones[y].part, zones[z].part, (size_t)zones[z].nParts * sizeof(int)) == 0) )
            {
                break;
            }
        }
        if( y < z )
        {
            zoneBlock[z] = zoneBlock[y];
            continue;
        }
        zoneBlock[z] = nBlocks;
        for(t = 0; t < dP->nTimbres; t++)
        {
            // At attenuation timbres[t] the table plays exactly that level: divided by its gain.
            g = pow(10.0, ((-dP->timbres[t]) / 20.0));
            wave(dP, &zones[z], hMax, g, w);
            for(i = 0; i < LENGTH; i++)
            {
                s = roundInt(((scale * w[i]) / g));
                if( labs(s) > FULL )
                {
                    fail("%s clips: a softer timbre louder than the scale allows; a smaller bright", dP->name);
                }
                blockWords[nBlocks][(t * LENGTH) + i] = wordOf(s);
            }
        }
        nBlocks++;
    }

    steps[0] = 0;
    steps[1] = 0;
    for(i = 2; i < PITCHES; i++)
    {
        steps[i] = roundInt(((((freq(i) * (double)LENGTH) * (double)dP->n) / (double)CPS) * 4096.0));
        if( steps[i] >= (64L * 4096L) )
        {
            fail("%s: a step beyond the device's range; a smaller n", dP->name);
        }
    }
    for(i = 0; i < PITCHES; i++)
    {
        zoneOfPitch[i] = zoneBlock[zoneOf(i)];
    }
    // The thresholds: halfway between each timbre's level and the next.
    for(t = 0; t < 3; t++)
    {
        th[t] = LEVEL_NONE;
        if( (t + 1) < dP->nTimbres )
        {
            th[t] = roundInt((((dP->timbres[t] + dP->timbres[t + 1]) / 2.0) * 8.0));
            if( (th[t] < 0) || (th[t] >= LEVEL_NONE) )
            {
                fail("%s: a threshold beyond the device's 0 to 63 dB", dP->name);
            }
        }
    }

    fprintf(fP, "dacvoice %d\n# Made by dacvoice from %s.\n", fileVersion(dP, 1), defNameP);
    fprintf(fP, "name %s\n", dP->name);
    if( dP->family[0] != '\0' )
    {
        fprintf(fP, "family %s\n", dP->family);
    }
    fprintf(fP, "n %d\nmode table\n", dP->n);
    writeVoicing(dP, fP);
    writeConverter(dP, nBlocks, fP);
    writeEnvelope(dP, fP);
    fprintf(fP, "stride %d\n", ((dP->nTimbres > 1) ? LENGTH : 0));
    fprintf(fP, "thresholds %ld %ld %ld\n", th[0], th[1], th[2]);
    fprintf(fP, "steps\n");
    writeRows(fP, steps, PITCHES, "%ld", NUMS_PER_LINE);
    fprintf(fP, "zones\n");
    writeRows(fP, zoneOfPitch, PITCHES, "%ld", NUMS_PER_LINE);
    if( dP->nZones == MAX_ZONES )
    {
        fprintf(fP, "zone6 %d\n", zoneBlock[NZONES]);
    }
    for(j = 0; j < nBlocks; j++)
    {
        fprintf(fP, "block %d %d\n", j, (dP->nTimbres * LENGTH));
        writeRows(fP, blockWords[j], (dP->nTimbres * LENGTH), "%06lo", PER_LINE);
    }
    if( (dP->seen & F_PERC) != 0 )
    {
        // The percussion's sine at the voice's scale: its peak at the loudest zone's.
        sineWords((PEAK * (double)FULL), sine);
        fprintf(fP, "block %d %d\n", nBlocks, LENGTH);
        writeRows(fP, sine, LENGTH, "%06lo", PER_LINE);
    }
    fprintf(stderr, "dacvoice: %s, N %d: %d timbre%s, %d block%s (zones 0-%d), %d words%s\n", dP->name, dP->n,
            dP->nTimbres, ((dP->nTimbres == 1) ? "" : "s"), nBlocks, ((nBlocks == 1) ? "" : "s"), (dP->nZones - 1),
            (nBlocks * dP->nTimbres * LENGTH), (((dP->seen & F_PERC) != 0) ? ", and the percussion's sine" : ""));
}

// Stops.

// random01: the next of a fixed sequence in [0, 1), the same on every
// host: a 32-bit linear congruential generator (Numerical Recipes').
static double random01(unsigned long *stateP)
{
    *stateP = (((*stateP * 1664525ul) + 1013904223ul) & 0xfffffffful);
    return ((double)*stateP / 4294967296.0);
}

// makeStop: the stop the definition describes, written to fP. Harmonic k
// of a rank at frame harmonic H is the frame's harmonic k H, at the rank's
// level times the pipe's amplitude; a rank breaks back an octave (H
// halved) at each zone its breaks name. A rank whose own pitch is not
// below its zone's 0.45 limit fails: the definition must break it back.
// A nyquist rank (the Terts, an odd harmonic, cannot break back) keeps its
// own pitch while under half the sample rate, its harmonics still cut at
// 0.45. Each zone's speech is a chiff (README.md, "Stops"), its peak
// QUARTER lowered by the chiff's level, which must leave four stops' peaks
// room.
static void makeStop(const Definition *dP, const char *defNameP, FILE *fP)
{
    static double amp[MAX_ZONES][LENGTH / 2];
    static double w[MAX_ZONES][LENGTH];
    static double c[SPEECH];
    static double fr[CHIFF_PARTIALS];
    static double ph[CHIFF_PARTIALS];
    static long words[2 * MAX_ZONES][SPEECH];
    static long sine[LENGTH];
    double terms[LENGTH / 2];
    int blockLen[2 * MAX_ZONES];
    int zoneBlock[MAX_ZONES];
    int speechBlock[MAX_ZONES];
    long steps[PITCHES];
    long zoneOfPitch[PITCHES];
    long stopMax;
    long chiffMax;
    long s;
    unsigned long seed;
    double ratio;
    double limit;
    double nyquist;
    double rankLimit;
    double top;
    double bottom;
    double big;
    double scale;
    double hi;
    double tau;
    double e;
    double v;
    int nBlocks;
    int nTables;
    int hMax;
    int hz;
    int h;
    int n;
    int r;
    int z;
    int y;
    int i;
    int j;

    ratio = ((double)dP->frame / 8.0);  // a 16-foot frame sounds an octave down
    limit = ((0.45 * (double)CPS) / (double)dP->n);
    nyquist = ((0.5 * (double)CPS) / (double)dP->n);

    // Each zone's spectrum in the frame's harmonics.
    hMax = 0;
    memset(amp, 0, sizeof(amp));
    for(z = 0; z < dP->nZones; z++)
    {
        top = (freq(zoneTop(z, dP->nZones)) / ratio);
        for(r = 0; r < dP->nRanks; r++)
        {
            hz = dP->rankH[r];
            for(y = 1; y <= z; y++)
            {
                if( (dP->rankBreaks[r] & (1u << y)) != 0 )
                {
                    if( (hz % 2) != 0 )
                    {
                        fail("%s: a rank breaks back from an odd harmonic", dP->name);
                    }
                    hz /= 2;
                }
            }
            rankLimit = (dP->rankNyquist[r] ? nyquist : limit);
            if( !(((double)hz * top) < rankLimit) )
            {
                fprintf(stderr, "dacvoice: %s: rank %d, at harmonic %d in zone %d, is above the zone's top"
                        " harmonic; break it back\n", dP->name, (r + 1), hz, z);
                exit(1);
            }
            for(j = 0; j < dP->nHarm; j++)
            {
                h = (hz * dP->harm[j]);
                if( (h < (LENGTH / 2)) && (((double)h * top) < ((dP->harm[j] == 1) ? rankLimit : limit)) )
                {
                    amp[z][h] += (dP->rankLevel[r] * dP->amp[j]);
                    if( h > hMax )
                    {
                        hMax = h;
                    }
                }
            }
        }
    }

    // The tables: each zone's waveform, its phases spread as a table
    // voice's, then one scale for all.
    big = 0.0;
    for(z = 0; z < dP->nZones; z++)
    {
        for(i = 0; i < LENGTH; i++)
        {
            n = 0;
            for(h = 1; h < (LENGTH / 2); h++)
            {
                if( amp[z][h] != 0.0 )
                {
                    terms[n] = (amp[z][h] * sin(((((2.0 * PI) * (double)h) * (double)i) / (double)LENGTH)
                                               + (((PI * (double)h) * (double)(h - 1)) / (double)hMax)));
                    n++;
                }
            }
            w[z][i] = pySum(terms, n);
            if( fabs(w[z][i]) > big )
            {
                big = fabs(w[z][i]);
            }
        }
    }
    if( big == 0.0 )
    {
        fail("%s has no harmonic below the zones' top", dP->name);
    }
    scale = ((QUARTER * pow(10.0, ((-dP->levelDb) / 20.0))) / big);
    nBlocks = 0;
    for(z = 0; z < dP->nZones; z++)
    {
        for(i = 0; i < LENGTH; i++)
        {
            words[nBlocks][i] = wordOf(roundInt((scale * w[z][i])));
        }
        for(y = 0; y < nBlocks; y++)
        {
            if( memcmp(words[y], words[nBlocks], (size_t)LENGTH * sizeof(long)) == 0 )
            {
                break;
            }
        }
        zoneBlock[z] = y;
        if( y == nBlocks )
        {
            blockLen[nBlocks] = LENGTH;
            nBlocks++;
        }
    }
    nTables = nBlocks;

    // The speech: a chiff a zone, zones with the same words sharing a block.
    stopMax = roundInt(QUARTER);
    chiffMax = (FULL - (4 * stopMax));
    for(z = 0; z < dP->nZones; z++)
    {
        top = (freq(zoneTop(z, dP->nZones)) / ratio);
        bottom = (freq(zoneLow(z)) / ratio);
        hi = (limit / top);
        hi = ((dP->chiffHi < hi) ? dP->chiffHi : hi);
        hi = (((double)(LENGTH / 2) < hi) ? (double)(LENGTH / 2) : hi);
        if( hi <= dP->chiffLo )
        {
            fail("%s: the chiff's band is above a zone's top harmonic", dP->name);
        }
        seed = CHIFF_SEED;
        for(j = 0; j < CHIFF_PARTIALS; j++)
        {
            fr[j] = (dP->chiffLo + ((hi - dP->chiffLo) * random01(&seed)));
            ph[j] = ((2.0 * PI) * random01(&seed));
        }
        // Words a second at the zone's lowest pitch: a period is LENGTH words.
        tau = ((dP->chiffS * (double)LENGTH) * bottom);
        big = 0.0;
        for(i = 0; i < SPEECH; i++)
        {
            e = exp(((-(double)i) / tau));
            if( i < CHIFF_RISE )
            {
                e *= (0.5 * (1.0 - cos(((PI * (double)i) / (double)CHIFF_RISE))));
            }
            if( i >= (SPEECH - CHIFF_FADE) )
            {
                e *= (0.5 * (1.0 + cos(((PI * (double)(i - (SPEECH - CHIFF_FADE))) / (double)CHIFF_FADE))));
            }
            for(j = 0; j < CHIFF_PARTIALS; j++)
            {
                terms[j] = sin((((((2.0 * PI) * fr[j]) * (double)i) / (double)LENGTH) + ph[j]));
            }
            v = pySum(terms, CHIFF_PARTIALS);
            c[i] = (e * v);
            if( fabs(c[i]) > big )
            {
                big = fabs(c[i]);
            }
        }
        scale = ((QUARTER * pow(10.0, ((-dP->chiffDb) / 20.0))) / big);
        for(i = 0; i < SPEECH; i++)
        {
            s = roundInt((scale * c[i]));
            if( labs(s) > chiffMax )
            {
                fprintf(stderr, "dacvoice: %s: the chiff's peak, %ld, leaves four stops' %ld no room under %d;"
                        " a larger chiff level\n", dP->name, labs(s), stopMax, FULL);
                exit(1);
            }
            words[nBlocks][i] = wordOf(s);
        }
        for(y = nTables; y < nBlocks; y++)
        {
            if( memcmp(words[y], words[nBlocks], (size_t)SPEECH * sizeof(long)) == 0 )
            {
                break;
            }
        }
        speechBlock[z] = y;
        if( y == nBlocks )
        {
            blockLen[nBlocks] = SPEECH;
            nBlocks++;
        }
    }

    steps[0] = 0;
    steps[1] = 0;
    for(i = 2; i < PITCHES; i++)
    {
        steps[i] = roundInt((((((freq(i) / ratio) * (double)LENGTH) * (double)dP->n) / (double)CPS) * 4096.0));
        if( steps[i] >= (64L * 4096L) )
        {
            fail("%s: a step beyond the device's range; a smaller n", dP->name);
        }
    }
    for(i = 0; i < PITCHES; i++)
    {
        zoneOfPitch[i] = zoneBlock[zoneOf(i)];
    }

    fprintf(fP, "dacvoice %d\n# Made by dacvoice from %s.\n", fileVersion(dP, 2), defNameP);
    fprintf(fP, "name %s\n", dP->name);
    if( dP->family[0] != '\0' )
    {
        fprintf(fP, "family %s\n", dP->family);
    }
    fprintf(fP, "n %d\nmode stop\n", dP->n);
    writeVoicing(dP, fP);
    writeConverter(dP, nBlocks, fP);
    writeEnvelope(dP, fP);
    fprintf(fP, "stride 0\nthresholds %d %d %d\n", LEVEL_NONE, LEVEL_NONE, LEVEL_NONE);
    fprintf(fP, "sample %d %d\n", (SPEECH + LENGTH), LENGTH);
    fprintf(fP, "steps\n");
    writeRows(fP, steps, PITCHES, "%ld", NUMS_PER_LINE);
    fprintf(fP, "zones\n");
    writeRows(fP, zoneOfPitch, PITCHES, "%ld", NUMS_PER_LINE);
    if( dP->nZones == MAX_ZONES )
    {
        fprintf(fP, "zone6 %d\n", zoneBlock[NZONES]);
    }
    fprintf(fP, "speech\n");
    for(z = 0; z < dP->nZones; z++)
    {
        zoneOfPitch[z] = speechBlock[z];
    }
    writeRows(fP, zoneOfPitch, dP->nZones, "%ld", NUMS_PER_LINE);
    for(j = 0; j < nBlocks; j++)
    {
        fprintf(fP, "block %d %d\n", j, blockLen[j]);
        writeRows(fP, words[j], blockLen[j], "%06lo", PER_LINE);
    }
    if( (dP->seen & F_PERC) != 0 )
    {
        // The percussion's sine at the stop's scale: a quarter of full scale, less its level.
        sineWords((QUARTER * pow(10.0, ((-dP->levelDb) / 20.0))), sine);
        fprintf(fP, "block %d %d\n", nBlocks, LENGTH);
        writeRows(fP, sine, LENGTH, "%06lo", PER_LINE);
    }
    fprintf(stderr, "dacvoice: %s, N %d, frame %d: %d table%s, %d speech block%s, %d words%s\n", dP->name, dP->n,
            dP->frame, nTables, ((nTables == 1) ? "" : "s"), (nBlocks - nTables),
            (((nBlocks - nTables) == 1) ? "" : "s"), ((nTables * LENGTH) + ((nBlocks - nTables) * SPEECH)),
            (((dP->seen & F_PERC) != 0) ? ", and the percussion's sine" : ""));
}

// checkEnvelope: the envelope's latch values in the device's ranges.
static void checkEnvelope(const Definition *dP)
{
    long v[4];
    int i;

    v[0] = levelOf(dP->startDb);
    v[1] = levelOf(dP->peakDb);
    v[2] = (dP->hasSustain ? levelOf(dP->sustainDb) : 0);
    v[3] = 0;
    for(i = 0; i < 3; i++)
    {
        if( (v[i] < 0) || (v[i] >= LEVEL_NONE) )
        {
            fail("%s: start, peak and sustain are 0 to 63 dB", dP->name);
        }
    }
    if( v[0] < v[1] )
    {
        fail("%s: the start is no louder than the peak (it is attenuation)", dP->name);
    }
    if( (rateOf(((dP->startDb - dP->peakDb) / dP->attackS)) > WORD_MASK) || (rateOf(dP->decayDbs) > WORD_MASK)
        || (rateOf(dP->decayDbs) < 0) || (rateOf((30.0 / dP->releaseS)) > WORD_MASK) )
    {
        fail("%s: a rate beyond the device's latch", dP->name);
    }
    // A rate under half a unit (about 0.38 dB/s) rounds to 0, and the device
    // holds a stage whose rate is 0: refused where the stage must move.
    if( (v[0] > v[1]) && (rateOf(((dP->startDb - dP->peakDb) / dP->attackS)) == 0) )
    {
        fail("%s: the attack's rate rounds to 0, so the start never reaches the peak", dP->name);
    }
    if( (dP->decayDbs > 0.0) && (!dP->hasSustain || (v[2] > v[1])) && (rateOf(dP->decayDbs) == 0) )
    {
        fail("%s: the decay's rate rounds to 0, so the peak never falls", dP->name);
    }
    if( rateOf((30.0 / dP->releaseS)) == 0 )
    {
        fail("%s: the release's rate rounds to 0, so the release never ends", dP->name);
    }
}

// checkConverter: the converter's latch values from the definition's
// units, each in the device's range; a rate that rounds to 0 is refused,
// since the counter or the percussion would never move.
static void checkConverter(Definition *dP)
{
    double k;

    if( (dP->seen & F_VIBRATO) != 0 )
    {
        dP->vibRate = roundInt(((dP->vibratoHz * PHASE_ONE) / CLOCK_HZ));
        if( (dP->vibratoHz <= 0.0) || (dP->vibRate < 1) || (dP->vibRate > RATE_MAX) )
        {
            fail("%s: a vibrato's rate is 0.0002 to 48.8 Hz (it rounds to 0 below)", dP->name);
        }
        dP->vibDepth = roundInt((DEPTH_ONE * (pow(2.0, (dP->vibratoCents / 1200.0)) - 1.0)));
        if( (dP->vibratoCents < 0.0) || (dP->vibDepth > LEVEL_NONE) )
        {
            fail("%s: a vibrato's depth is 0 to 53 cents", dP->name);
        }
        dP->vibDelay = roundInt(((dP->vibratoDelayS * CLOCK_HZ) / DELAY_CLOCKS));
        if( (dP->vibratoDelayS < 0.0) || (dP->vibDelay > DELAY_MAX) )
        {
            fail("%s: a vibrato's delay is 0 to 1.3 s", dP->name);
        }
    }
    if( (dP->seen & F_LESLIE) != 0 )
    {
        if( (dP->seen & F_VIBRATO) == 0 )
        {
            fail("%s: a leslie needs a vibrato line, the counter's rate (its depth may be 0)", dP->name);
        }
        dP->leslieA = levelOf(dP->leslieDb);
        dP->leslieP = levelOf(dP->placeDb);
        if( (dP->leslieA < 0) || (dP->leslieA > LEVEL_NONE) || (dP->leslieP < 0) || (dP->leslieP > LEVEL_NONE) )
        {
            fail("%s: a leslie's depths are 0 to 63.875 dB", dP->name);
        }
    }
    if( (dP->seen & F_PERC) != 0 )
    {
        k = dP->percK;
        if( (k != floor(k)) || (k < 1.0) || (k > (double)K_MAX) )
        {
            fail("%s: the percussion's multiple is 1 to 7", dP->name);
        }
        dP->percLevel = levelOf(dP->percDb);
        if( (dP->percLevel < 0) || (dP->percLevel > LEVEL_NONE) )
        {
            fail("%s: the percussion's level is 0 to 63.875 dB under the note", dP->name);
        }
        dP->percDecay = rateOf(dP->percDecayDbs);
        if( (dP->percDecay < 1) || (dP->percDecay > RATE_MAX) )
        {
            fail("%s: the percussion's decay is 0.38 to 200,000 dB/s (it rounds to 0 below)", dP->name);
        }
    }
}

// usage: the command's form, and exit 1.
static void usage(void)
{
    fprintf(stderr, "usage: dacvoice def-file [out.voice]\n"
            "  makes a table voice or an organ stop for the D/A converters' music\n"
            "  program from a definition; writes the voice file to out.voice, or to\n"
            "  standard output\n");
    exit(1);
}

// main: reads the definition, writes the voice.
int main(int argc, char **argv)
{
    static Definition def;
    FILE *fP;
    const char *baseP;

    if( (argc < 2) || (argc > 3) || (argv[1][0] == '-') )
    {
        usage();
    }
    readDefinition(argv[1], &def);
    checkEnvelope(&def);
    checkConverter(&def);
    baseP = strrchr(argv[1], '/');
    baseP = ((baseP != NULL) ? (baseP + 1) : argv[1]);
    if( argc == 3 )
    {
        if( !(fP = fopen(argv[2], "w")) )
        {
            fail("cannot write %s", argv[2]);
        }
    }
    else
    {
        fP = stdout;
    }
    if( def.frame != 0 )
    {
        makeStop(&def, baseP, fP);
    }
    else
    {
        makeVoice(&def, baseP, fP);
    }
    if( (fP != stdout) && (fclose(fP) != 0) )
    {
        fail("write error on %s", argv[2]);
    }
    return 0;
}
