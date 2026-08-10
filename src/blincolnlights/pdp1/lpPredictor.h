// Definitions for the lightpen motion prediction software.
#ifndef LPPREDICTOR_H
#define LPPREDICTOR_H

#include <stdbool.h>

#define DEFAULT_POSITION_TAU 0.008      // 8ms position update interval t30dpy uses
#define DEFAULT_VELOCITY_TAU 0.015      // 15ms light velocity smoothing
#define DEFAULT_ACCELERATION_TAU 0.005  // 10ms fast acceleration ramping
#define DEFAULT_DECELERATION_TAU 0.080  // 80ms heavy deceleration damping to prevent overshoots

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
    
    double lastArrivalTime; // Local monotonic timestamp of the last update, seconds.microseconds
} LightpenCoordinateFilter, *LightpenCoordinateFilterP;

void motionFilterReset(LightpenCoordinateFilterP fP);
void motionFilterAdd(LightpenCoordinateFilterP fP, int newPos);
int motionFilterPredict(LightpenCoordinateFilterP fP);

#endif
