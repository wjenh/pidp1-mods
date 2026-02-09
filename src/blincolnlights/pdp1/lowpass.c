/*
 * Low pass filter for flitering the generated music for the pidp-1.
 * This implements a classic 1st-order IIR filter.
 *
 * It requires a FilterSpec reference with an alpha value set.
 * The alpha value determines the upper cutoff frequency with 0.0 being the lowest cutoff, 1.0 the highest.
 * A gain value can also be provided, the default is a gain of 1.0.
 * The result returned by the filter is multiplied by the gain.
*/
#include <stdio.h>
#include "lowpass.h"

// Set the required values for a filter
FilterSpecP
initializeFilter(FilterSpecP specP, float alpha, float gain, float initialValue)
{
    specP->alpha = boundValue(alpha);
    specP->gain = gain;
    specP->lastResult = 0.0;
    specP->initialValue = initialValue;
    specP->highCount = 0;
    specP->highValue = 0.0;

    specP->sawOne = false;
    specP->doGain = (gain != 0.0)?true:false;           // we can eliminate one floating multiply if false
    specP->deadElimination = false;

    return( specP );
}

/*
 * Set a limit that when the current input value equals or exceeds hival for cutoff calls
 * it will be considered a zero value.
 * This is because the PDP-1 music player may leave an output channel at the high value
 * when no note is being played.
 */
void
setFilterDeadDetection(FilterSpecP specP, float hival, int cutoff)
{
    specP->deadElimination = true;
    specP->highValue = hival;
    specP->highLimit = cutoff;
    specP->highCount = 0;
}

// Simple filter, only uses one prior sample
float
lowPassFilter(FilterSpecP specP, float sample)
{
float result;

    if( specP->deadElimination )
    {
        if( sample >= specP->highValue )
        {
            if( ++(specP->highCount) >= specP->highLimit)
            {
                sample = 0.0;
            }
        }
        else
        {
            specP->highCount = 0;
        }
    } 

    if( specP->sawOne != true )         // prime the pump
    {
        specP->sawOne = true;
        result = specP->initialValue;
    }
    else
    {
        // Calculate the current filtered output
        result = (specP->alpha * sample) + ((1.0 - specP->alpha) * specP->lastResult);
    }
    
    // Update the previous filtered value for the next iteration
    specP->lastResult = result;

    return( (specP->doGain)?(specP->gain * result):result );
}
