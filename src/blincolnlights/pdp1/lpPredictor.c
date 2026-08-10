/*
 * This implements a floating-point time-normalized cascaded velocity/acceleration exponential moving
 * average filter chain.
 * The first filter is the velocity filter, the second is dual slope and handles acceleration
 * and deceleration.
 * It compensates for the lack of actual lightpen instantaneous hit detection on the original display and
 * allows much better tracking of a moving point.
 * It expects to be called on each lightpen coordinate update with an elapsed-time delta in seconds from
 * the previous sample.
 * The alphas below are per-second time constants.
 * Unlike the assembler version that uses fixed-point integers, we don't have to worry about overflow
 * or wrapping.
 *
 * 1-Aug-26 wje initial version
 * 10-Aug-26 wje (Claude) add reversal detection override.
 *    The acceleration cascade now forces a fast-snap alpha whenever the instantaneous
 *    acceleration sign flips against the currently filtered acceleration, instead of
 *    coasting through a direction change on the slower asymmetric taus.
 *
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include "lpPredictor.h"

//#define DOLOGGING
#include "logger.h"
#define LOG_STATS 0

static double getNow(void);
static int constrainCoordinate(int);

// Initialize a filter to a known state, should be called before a filter is used
// and after any untracked lightpen movement, e.g. pen-up movement.
void
motionFilterReset(LightpenCoordinateFilterP filterP)
{
    filterP->tauPosition = DEFAULT_POSITION_TAU;
    filterP->tauVelocity = DEFAULT_VELOCITY_TAU;
    filterP->tauAcceleration = DEFAULT_ACCELERATION_TAU;
    filterP->tauDeceleration = DEFAULT_DECELERATION_TAU;
    filterP->tauReversal = DEFAULT_REVERSAL_TAU;
    filterP->position = 0.0;
    filterP->velocity = 0.0;
    filterP->acceleration = 0.0;
    filterP->lastArrivalTime = 0.0;
    filterP->lastPredicted = 0;
    filterP->isInitialized = false;
}

// Call this when a mosue coordinate update is received from the display.
// It will update the time-dependent filter alphas appropriately.
// These will then be used by the prediction algorithm.
void
motionFilterAdd(LightpenCoordinateFilterP filterP, int newPos)
{
double now;
double deltaTime;
double positionAlpha;
double velocityAlpha;
double accelerationAlpha;
double prevPosition;
double prevVelocity, instantVelocity;
double instantAcceleration;
bool isReversal;

    newPos = constrainCoordinate(newPos);
    now = getNow();

    // First time since a reset.
    if( !filterP->isInitialized )
    {
        filterP->lastPredicted = filterP->position = newPos;
        filterP->isInitialized = true;
        filterP->lastArrivalTime = now;
        return;
    }

    // Everything is based on time deltas from the last update.
    deltaTime = now - filterP->lastArrivalTime;
    filterP->lastArrivalTime = now;
    
    // Safety guardrail for OS packet clamping/clumping.
    if( deltaTime < 0.001 )
    {
        deltaTime = 0.001; 
    }

    // Kinematic cascade with asymmetric alphas
    positionAlpha = 1.0 - exp(-deltaTime / filterP->tauPosition);
    velocityAlpha = 1.0 - exp(-deltaTime / filterP->tauVelocity);

    prevPosition = filterP->position;
    filterP->position = (positionAlpha * newPos) + ((1.0 - positionAlpha) * filterP->position);

    instantVelocity = (filterP->position - prevPosition) / deltaTime;
    prevVelocity = filterP->velocity;
    filterP->velocity = (velocityAlpha * instantVelocity) + ((1.0 - velocityAlpha) * filterP->velocity);

    instantAcceleration = (filterP->velocity - prevVelocity) / deltaTime;

    // If this sample's instantaneous acceleration points the opposite way from what the filter currently
    // believesand the filtered value is large enough that this isn't just dithering near zero,the pen has
    // changed direction.
    // The asymmetric accel/decel taus below are tuned for smooth ramping
    // and would coast through a real reversal too slowly, so force a much faster,
    // near-instant alpha to let the acceleration term snap onto the new direction immediately.
    isReversal = ((instantAcceleration > 0.0) && (filterP->acceleration < -REVERSAL_THRESHOLD)) ||
        ((instantAcceleration < 0.0) && (filterP->acceleration > REVERSAL_THRESHOLD));

    if( isReversal )
    {
        accelerationAlpha = 1.0 - exp(-deltaTime / filterP->tauReversal);
    }
    else if( instantAcceleration >= 0.0 )
    {
        // Asymmetric sign-based alpha assignment
        accelerationAlpha = 1.0 - exp(-deltaTime / filterP->tauAcceleration);
    }
    else
    {
        accelerationAlpha = 1.0 - exp(-deltaTime / filterP->tauDeceleration);
    }

    filterP->acceleration = (accelerationAlpha * instantAcceleration) +
        ((1.0 - accelerationAlpha) * filterP->acceleration);
    filterP->lastArrivalTime = now;

    // Log statistics for tuning.
    logger(LOG_STATS, "lpPredictor now %0.6f dt %0.6f coord %d, pos %0.1f, vel %0.1f acc %0.1f error %d\n",
        now, deltaTime, newPos, filterP->position, filterP->velocity, filterP->acceleration,
        newPos - filterP->lastPredicted);
}

// Get the predicted coordinate based on the previous history.
// Result will be constrained to 0..1023, 0 returned if the filter is not initialized.
int
motionFilterPredict(LightpenCoordinateFilter *filterP)
{
double deltaTime;
double velocityMagnitude;
double accelerationModifier;
double predictedPos;

    if( !filterP->isInitialized )
    {
        return(0);
    }

    // Calculate exactly how many seconds/microseconds have ticked past since the last mouse update
    deltaTime = getNow() - filterP->lastArrivalTime;
    if( deltaTime < 0.0 )         // should never happen, but be sure
    {
        deltaTime = 0.0;
    }

    // T30dpy updates mouse coordinates roughly every 8 msecs.
    // If the time delta exceeds the clamp threshold, limit it to that to prevent runaway extrapolation.
    if( deltaTime > DELTA_TIME_CLAMP )
    {
        deltaTime = DELTA_TIME_CLAMP;
    }

    // Velocity braking to protect pixel-coincidence accuracy when not moving.
    velocityMagnitude = fabs(filterP->velocity);
    accelerationModifier = 1.0;
    
    if( velocityMagnitude < 8.0 )
    {
        // Round and constrain the position.
        filterP->lastPredicted = constrainCoordinate((int)lrint(filterP->position));
        return( filterP->lastPredicted );   // Snaps to pixel center when hand stops
    }
    else if( velocityMagnitude < FINE_MAGNITUDE_LIMIT )
    {
        accelerationModifier = 0.1;         // Dampen the curve when approaching fine adjustments
    }

    // Taylor-series expansion to pinpoint the exact coordinate at this specific time.
    predictedPos = filterP->position + (filterP->velocity * deltaTime) +
        (0.5f * filterP->acceleration * deltaTime * deltaTime * accelerationModifier);

    // Round and constrain the position.
    filterP->lastPredicted = constrainCoordinate((int)lrint(predictedPos));
    return( filterP->lastPredicted );
}

// Return the current time in seconds with fractional microseconds.
// This uses a monotonic clock.
static double
getNow()
{
uint64_t usecs;
double secs;
struct timespec tm;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    usecs = tm.tv_nsec / 1000L;
    secs = (double)tm.tv_sec + ((double)usecs / 1000000.0); 
    return(secs);
}

// Constrain a coordinate value to 0..1023.
static int
constrainCoordinate(int coord)
{
    if( coord < 0 )
    {
        coord = 0;
    }
    else if( coord > 1023 )
    {
        coord = 1023;
    }

    return( coord );
}
