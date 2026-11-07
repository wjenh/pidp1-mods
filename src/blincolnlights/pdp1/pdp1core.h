#ifndef PDP1CORE_H
#define PDP1CORE_H
// What pdp1.c gives the rest of the emulator and not the plugins. Kept out of pdp1.h, which every
// IOT plugin includes, so a change here rebuilds only the emulator. pdp1.c includes it too, so the
// compiler checks each declaration against its definition.

#include <stdbool.h>
#include "pdp1.h"

// The PDP-1D instruction sets enabled, copied from the configuration by main.c.
// pdp1.h's IR_INCORR reads all1DEnabled.
extern bool lailiaEnabled;
extern bool core1DEnabled;
extern bool all1DEnabled;

// A high-speed channel's break: whether one may take the next memory cycle, and taking it.
bool hscBreakAllowed(void);
void hscBreak(PDP1 *pdp);
#endif
