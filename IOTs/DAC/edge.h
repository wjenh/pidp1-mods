// The organ voicing in each D/A converter, from dzv's bits 9-17 (IO & 0777):
// bits 15-17 the edge tone's level k (0 off, -36 + 2k dB under a stop at
// C4), bit 14 the speech, bit 13 Busch Hall (used by the mixer's room, not
// here), bits 9-12 the céleste's detune k in half cents (0 none).
// Implementation.md, "The voicing", describes what each does.
//   - The edge tone: noise through a state-variable band-pass of quality 3
//     at the fundamental f0, 4 dB softer each octave under C4.
//   - The speech: the attack's rise over 10 of the note's periods, never
//     faster than the voice's own, with breath through it: noise about
//     2 f0 (quality 1) 24 dB under a stop, dying away e^-1 over the rise.
//   - The céleste: only the detuned step is here, the first's plus
//     (step D_k + 2^23) >> 24; the second read is the converter's.
//
// Included by IOT_71.c (the device) and render/dacrender.c (the
// renderer), so both compute the same integers. The noise is a 32-bit
// xorshift (13, 17, 5) a converter, seeded at its go and stepped at every
// fourth cycle from it (50 kHz) while the note has the edge or the speech;
// its top 24 bits, signed, are the filters' input. A band's output y is
// band/q, a band-pass of peak gain 1, and the converter adds y * 2^-19
// times the gain of the envelope's attenuation plus the volume, times the
// gain of the level here (and the breath times that of its decay), each in
// 1/64 dB: a gain of its own, as a band's level alone takes up to 50 dB of
// the table's 64. Every coefficient comes from the step and the period in
// integers, a Taylor series for the band's sine and an integer log2 for
// the levels, with the constants below computed once, so no host's sin(),
// pow() or log() can move a bit. A product is rounded by adding 2^15 and
// shifting right 16 (gcc's shift of a negative signed value is
// arithmetic); a filter's state is held to +-2^30.

#ifndef EDGE_H
#define EDGE_H

#include <stdint.h>
#include <string.h>

#define EDGE_MASK 07                // the voicing's bits: the edge's level
#define EDGE_RISE 010               // the speech
#define EDGE_BUSCH 020              // Busch Hall, for the mixer's room
#define EDGE_CELESTE 0740           // the céleste's detune k, half cents; 0: none
#define EDGE_CELESTE_SHIFT 5
#define EDGE_CELESTE_MIX 0.7071067811865476 // 1/sqrt(2): the two reads at equal power
#define EDGE_VOICING 0777
#define EDGE_DB_BASE (-36)          // level k is -36 + 2k dB
#define EDGE_TICK 4                 // cycles from one noise sample to the next
#define EDGE_OUT_SHIFT 19           // a band's y adds y * 2^-19
#define EDGE_LIMIT 0x3FFFFFFF       // a filter's state word's largest magnitude
#define EDGE_BREATH_DONE (4096 << 12) // the breath's attenuation where it is silent

// Constants for the noise, the bands, the levels and the céleste's detunes, computed once.
#define EDGE_NOISE_SEED 2246822507LL
#define EDGE_DAMP 21845LL
#define EDGE_BREATH_DAMP 65536LL
#define EDGE_PI_Q30 3373259426LL
#define EDGE_A_Q16 -269776379LL
#define EDGE_S_Q16 29403329LL
#define EDGE_BREATH_A_Q16 38372791LL
#define EDGE_BREATH_S_Q16 12626113LL
#define EDGE_BREATH_K 14231LL
#define EDGE_RISE_DIV 320LL
#define EDGE_BREATH_TOTAL 2276954LL
#define EDGE_DETUNE_1 4846LL
#define EDGE_DETUNE_2 9694LL
#define EDGE_DETUNE_3 14543LL
#define EDGE_DETUNE_4 19393LL
#define EDGE_DETUNE_5 24245LL
#define EDGE_DETUNE_6 29098LL
#define EDGE_DETUNE_7 33952LL
#define EDGE_DETUNE_8 38808LL
#define EDGE_DETUNE_9 43666LL
#define EDGE_DETUNE_10 48525LL
#define EDGE_DETUNE_11 53385LL
#define EDGE_DETUNE_12 58246LL
#define EDGE_DETUNE_13 63109LL
#define EDGE_DETUNE_14 67974LL
#define EDGE_DETUNE_15 72839LL

// The céleste's detunes, k half cents sharp, as a fraction of the step in 1/2^24.
static const int64_t edgeDetunes[16] =
{
    0, EDGE_DETUNE_1, EDGE_DETUNE_2, EDGE_DETUNE_3, EDGE_DETUNE_4, EDGE_DETUNE_5, EDGE_DETUNE_6,
    EDGE_DETUNE_7, EDGE_DETUNE_8, EDGE_DETUNE_9, EDGE_DETUNE_10, EDGE_DETUNE_11, EDGE_DETUNE_12,
    EDGE_DETUNE_13, EDGE_DETUNE_14, EDGE_DETUNE_15
};

typedef struct
{
    int32_t low;
    int32_t band;
    int32_t y;                      // band / q, the output
} EdgeBand;

typedef struct
{
    uint32_t noise;
    int voicing;
    EdgeBand edge;
    EdgeBand breath;
    int32_t edgeCoef;               // each band's 2 sin(pi f / 50,000) in 1/65,536; 0: silent
    int32_t breathCoef;
    int32_t edgeLevel;              // each one's attenuation past the envelope's, 1/64 dB
    int32_t breathLevel;
    int32_t rise;                   // the attack's rate while the speech's rise lasts; 0: none
    int32_t breathAtt;              // the breath's decay since the key on, 1/64 dB with 12 fraction bits
    int32_t breathRate;
} EdgeState;

// x times k/65,536, rounded.
static int64_t
edgeMul(int64_t x, int64_t k)
{
    return( ((x * k) + 32768) >> 16 );
}

// x held to a state word's range.
static int32_t
edgeSat(int64_t x)
{
    if( x > EDGE_LIMIT )
    {
        return( EDGE_LIMIT );
    }
    if( x < -EDGE_LIMIT )
    {
        return( -EDGE_LIMIT );
    }
    return( (int32_t)x );
}

// log2(x), x positive, in 1/65,536: the fraction by 16 squarings, truncated.
static int64_t
edgeLog2(int64_t x)
{
uint64_t y;
int64_t frac;
int m;
int i;

    m = 0;
    while( (x >> (m + 1)) != 0 )
    {
        m++;
    }
    y = ((m <= 30) ? ((uint64_t)x << (30 - m)) : ((uint64_t)x >> (m - 30)));
    frac = 0;
    for( i = 0; i < 16; i++ )
    {
        y = ((y * y) >> 30);
        frac <<= 1;
        if( y >= (1ULL << 31) )
        {
            y >>= 1;
            frac |= 1;
        }
    }
    return( ((int64_t)m << 16) | frac );
}

// A band's coefficient, 2 sin(pi f / 50,000) in 1/65,536, at f = mult times the
// fundamental of step and period; 0 at or above 50,000/6 Hz, where it gives nothing.
// pi f / 50,000 is pi step / (period 2^18); the sine by its Taylor series to the 5th power.
static int32_t
edgeBandCoef(int64_t step, int64_t period, int64_t mult)
{
int64_t t;
int64_t t2;
int64_t t3;
int64_t t5;
int64_t s;

    if( (step <= 0) || (period <= 0) || ((6 * mult * step) >= (period << 18)) )
    {
        return( 0 );
    }

    t = ((EDGE_PI_Q30 * step * mult) / (period << 18));
    t2 = ((t * t) >> 30);
    t3 = ((t2 * t) >> 30);
    t5 = ((t3 * t2) >> 30);
    s = (t - (t3 / 6) + (t5 / 120));
    return( (int32_t)(((2 * s) + (1 << 13)) >> 14) );
}

// An attenuation in 1/64 dB: a + s log2(step / period) + extra (a and s in 1/65,536),
// never below 0.
static int32_t
edgeLevelOf(int64_t a, int64_t s, int64_t step, int64_t period, int64_t extra)
{
int64_t lg;
int64_t e;

    lg = (edgeLog2(step) - edgeLog2(period));
    e = (((a + edgeMul(s, lg)) + 32768) >> 16);
    e += extra;
    return( (int32_t)((e > 0) ? e : 0) );
}

// The céleste's detune k, half cents, from a voicing; 0: none.
static int
edgeCeleste(int voicing)
{
    return( (voicing & EDGE_CELESTE) >> EDGE_CELESTE_SHIFT );
}

// The second phase accumulator's step: k half cents sharp of step, rounded.
static int32_t
edgeCelesteStep(int64_t step, int k)
{
    return( (int32_t)(step + (((step * edgeDetunes[k & 017]) + (1LL << 23)) >> 24)) );
}

// Clears a converter's state at its go: the noise seeded from its number, 0-3.
static void
edgeInit(EdgeState *eP, int conv)
{
    memset(eP, 0, sizeof(*eP));
    eP->noise = (uint32_t)(((uint64_t)(conv + 1) * (uint64_t)EDGE_NOISE_SEED) & 0xFFFFFFFFULL);
    eP->breathAtt = EDGE_BREATH_DONE;
}

// True while the noise runs: the note has the edge or the speech.
static int
edgeOn(const EdgeState *eP)
{
    return( (eP->voicing & (EDGE_MASK | EDGE_RISE)) != 0 );
}

// A note loads with its voicing, step and period, its start and peak levels (1/8 dB), its
// own attack rate, and keyOn if it keys on. Returns the attack's rate while the speech's rise
// lasts, or 0 for none: the caller holds the attack to it, never faster than the note's own.
// The breath dies away e^-1 over the attack at the slower of the two rates, or over 10
// periods where there is no attack.
static int32_t
edgeLoad(EdgeState *eP, int voicing, int64_t step, int64_t period, int64_t start, int64_t peak, int64_t attack,
         int keyOn)
{
int64_t r;
int64_t a;
int k;

    eP->voicing = (voicing & EDGE_VOICING);
    k = (eP->voicing & EDGE_MASK);
    if( k != 0 )
    {
        eP->edgeCoef = edgeBandCoef(step, period, 1);
        eP->edgeLevel = edgeLevelOf(EDGE_A_Q16, EDGE_S_Q16, step, period, (-64 * (EDGE_DB_BASE + (2 * k))));
        if( eP->edgeCoef == 0 )
        {
            eP->edge.y = 0;         // a band above its range is silent at once
        }
    }

    if( (eP->voicing & EDGE_RISE) == 0 )
    {
        return( 0 );
    }

    eP->breathCoef = edgeBandCoef(step, period, 2);
    eP->breathLevel = edgeLevelOf(EDGE_BREATH_A_Q16, EDGE_BREATH_S_Q16, step, period, 0);
    if( eP->breathCoef == 0 )
    {
        eP->breath.y = 0;
    }
    if( keyOn )
    {
        eP->rise = 0;
        if( (start > peak) && (step > 0) && (period > 0) )
        {
            r = (((start - peak) * step) / (EDGE_RISE_DIV * period));
            eP->rise = (int32_t)((r > 0) ? r : 1);
        }
        eP->breathAtt = 0;
        a = (((eP->rise > 0) && (eP->rise < attack)) ? eP->rise : attack);
        if( (start > peak) && (a > 0) )
        {
            eP->breathRate = (int32_t)((EDGE_BREATH_TOTAL * a) / ((start - peak) << 15));
        }
        else
        {
            eP->breathRate = (int32_t)(((step > 0) && (period > 0)) ? ((step * EDGE_BREATH_K) / (period << 16)) : 0);
        }
    }
    return( eP->rise );
}

// One cycle, after the envelope's step: the breath dies away.
static void
edgePoll(EdgeState *eP)
{
    if( ((eP->voicing & EDGE_RISE) != 0) && (eP->breathAtt < EDGE_BREATH_DONE) )
    {
        eP->breathAtt += eP->breathRate;
        if( eP->breathAtt > EDGE_BREATH_DONE )
        {
            eP->breathAtt = EDGE_BREATH_DONE;
        }
    }
}

// One step of a band on input x.
static void
edgeBandStep(EdgeBand *bP, int64_t x, int64_t f, int64_t d)
{
int64_t high;

    bP->low = edgeSat(bP->low + edgeMul(bP->band, f));
    high = (x - bP->low - edgeMul(bP->band, d));
    bP->band = edgeSat(bP->band + edgeMul(high, f));
    bP->y = (int32_t)edgeMul(bP->band, d);
}

// Every fourth cycle from the go, after the sample clock: while the noise runs, its next
// sample through each band the note has.
static void
edgeTick(EdgeState *eP)
{
uint32_t x;
int64_t in;

    if( !edgeOn(eP) )
    {
        return;
    }

    x = eP->noise;
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    eP->noise = x;
    in = ((int64_t)(int32_t)x >> 8);
    if( ((eP->voicing & EDGE_MASK) != 0) && (eP->edgeCoef != 0) )
    {
        edgeBandStep(&eP->edge, in, eP->edgeCoef, EDGE_DAMP);
    }
    if( ((eP->voicing & EDGE_RISE) != 0) && (eP->breathCoef != 0) )
    {
        edgeBandStep(&eP->breath, in, eP->breathCoef, EDGE_BREATH_DAMP);
    }
}

#endif
