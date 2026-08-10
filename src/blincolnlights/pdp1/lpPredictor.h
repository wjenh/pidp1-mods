// Definitions for the lightpen motion prediction software.
#ifndef LPPREDICTOR_H
#define LPPREDICTOR_H

#include <stdbool.h>

#define DEFAULT_POSITION_TAU 0.008      // 8ms position update interval t30dpy uses
#define DEFAULT_VELOCITY_TAU 0.015      // 15ms light velocity smoothing
#define DEFAULT_ACCELERATION_TAU 0.005  // 5ms fast acceleration ramping
#define DEFAULT_DECELERATION_TAU 0.080  // 80ms heavy deceleration damping to prevent overshoots
#define DEFAULT_REVERSAL_TAU 0.002      // ~2ms near-instant snap tau used only when a genuine

#define DELTA_TIME_CLAMP 0.025          // If a requested prediction time is too far from the last one, clamp tp this.
#define FINE_MAGNITUDE_LIMIT 50.0       // If a movement is less than this, use a finer scale adjustment.
#define REVERSAL_THRESHOLD 5.0          // Minimum |filtered acceleration| (px/sec^2) before an opposing-sign
                                        // instantaneous sample is treated as a genuine reversal rather than
                                        // noise dithering around zero.

                                         // direction reversal is detected (see motionFilterAdd())

// Per-axis motion prediction filter state.
// Two of these (x and y) are needed for every display that is using motion prediction.
typedef struct
{
    bool isInitialized;
    int lastPredicted;
    double position;
    double velocity;
    double acceleration;

    double tauPosition;
    double tauVelocity;
    double tauAcceleration;
    double tauDeceleration;
    double tauReversal;     // fast-snap tau applied on a detected acceleration sign reversal

    double lastArrivalTime; // Local monotonic timestamp of the last update, seconds.microseconds
} LightpenCoordinateFilter, *LightpenCoordinateFilterP;

void motionFilterReset(LightpenCoordinateFilterP fP);
void motionFilterAdd(LightpenCoordinateFilterP fP, int newPos);
int motionFilterPredict(LightpenCoordinateFilterP fP);

#endif
