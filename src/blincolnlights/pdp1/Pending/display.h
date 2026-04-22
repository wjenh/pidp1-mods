#ifndef DISPLAY_H
#define DISPLAY_H
// Various definitions for use with the asynchromous display subsystem

// The Type 30 display had some strange intensity mappings.
#define type30Intensity(i) (((i) + 4) & 07)

// The Type 340 display had 7 levels, p7sim implements them.
// p7sim also tries to mimic beam-spreading at high intensities but totally overdoes it, oh well.
#define type340Intensity(i) ((i) & 07)

// External calls to manage various things.
bool setDisplayFD(int screen, int fd);
int getDisplayFD(int screen);
bool getDisplayData(int screen, int *xP, int *yP, int *intensityP);
bool setDisplayData(int screen, int x, int y, int intensity);
bool lockDisplayData(int screen);
bool unlockDisplayData(int screen);

// Called to set the lightpen radius squared used for hit detection.
void setLightpenRadius2(int screen, int radius2);
// and to get it
int getLightpenRadius2(int screen);

// The outside interface.
void display(int screenNo, int x, int y, int intensity);

// The outside inteface for checking the lightpen.
// It will return true if there was a lightpen hit at the given coordinates, else false.
bool checkLightpen(PDP1 *pdp1P, int screenNo,  int x, int y);
#endif
