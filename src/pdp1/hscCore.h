#ifndef HSCCORE_H
#define HSCCORE_H
// What highSpeedChannels.c gives the emulator's main loop and not the plugins. Kept out of
// highSpeedChannels.h, which plugins include, so a change here rebuilds only the emulator.
// highSpeedChannels.c includes it too, so the compiler checks the declaration against its
// definition.

#include <stdbool.h>

// One pass of the channel scan. Returns true if a channel took this pass's memory cycle.
bool processHSCchannels(bool mayTake);
#endif
