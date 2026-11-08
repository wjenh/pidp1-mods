/*
 * Low pass filter for the generated music for the pidp-1.
 *
 * The music interface puts each program flag through a series resistor into a capacitor, a
 * first-order RC low-pass. This models it in continuous time on simtime: between input changes the
 * output decays exactly toward the input, y = x + (y - x) * exp(-dt/tau), and a change is applied at
 * the simtime it happened. The output can then be read at any instant, so the sample rate does not
 * move the cutoff, and an edge is not moved to the next sample.
 *
 * 26-Sep-2026 wje (Claude) - replaced the per-sample IIR, whose alpha tied the cutoff to the sample
 *    rate and whose point sampling moved each edge by up to a sample.
*/
#include <math.h>
#include "lowpass.h"

// Starts the filter settled at level, as of simtime t.
void
rcInit(RCFilterP fP, double level, uint64_t t)
{
    fP->y = level;
    fP->x = level;
    fP->t = t;
    fP->cacheDt[0] = fP->cacheDt[1] = 0;
    fP->cacheNext = 0;
}

// Sets the -3 dB point, 1 / (2 pi R C).
void
rcSetCutoff(RCFilterP fP, double hz)
{
    fP->invTauNs = ((2.0 * M_PI * hz) / 1e9);
    fP->cacheDt[0] = fP->cacheDt[1] = 0;      // the factors belong to the old tau
}

// Brings the output to simtime t with the input unchanged. A t at or before the last one leaves it.
void
rcAdvance(RCFilterP fP, uint64_t t)
{
uint64_t dt;
double k;

    if( t <= fP->t )
    {
        return;
    }

    dt = (t - fP->t);
    if( dt == fP->cacheDt[0] )
    {
        k = fP->cacheK[0];
    }
    else if( dt == fP->cacheDt[1] )
    {
        k = fP->cacheK[1];
    }
    else
    {
        k = exp(-((double)dt * fP->invTauNs));
        fP->cacheDt[fP->cacheNext] = dt;
        fP->cacheK[fP->cacheNext] = k;
        fP->cacheNext ^= 1;
    }

    fP->y = (fP->x + ((fP->y - fP->x) * k));
    fP->t = t;
}

// The input changes to level at simtime t.
void
rcSetInput(RCFilterP fP, uint64_t t, double level)
{
    rcAdvance(fP, t);
    fP->x = level;
}
