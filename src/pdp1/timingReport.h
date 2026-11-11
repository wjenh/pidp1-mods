#ifndef TIMINGREPORT_H
#define TIMINGREPORT_H

#include <stdbool.h>
#include "common.h"
#include "pdp1.h"

// The config's pidp1timing, set by configure().
// The calls below except for timingReset() are made only while the emulator program is running.
extern bool timingEnabled;

void timingReset(void);
void timingCycleBegin(void);
void timingNoteSteal(void);
void timingCycleEnd(PDP1 *pdp);
bool timingRunActive(void);
void timingRunEnd(void);
void timingNoteSleep(u64 wakeNs);

#endif
