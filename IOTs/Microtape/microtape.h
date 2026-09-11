/*
 * microtape.h -- the Type 550 Microtape as the paper tape reader IOT sees it.
 */
#ifndef MICROTAPE_H
#define MICROTAPE_H

#include <stdbool.h>

#include "pdp1.h"
#include "control550.h"

#define MT_SUB_MOUNT        2               // 720201, mount a tape image

// Results of the mount IOT, left in IO.
#define MT_MOUNT_OK         0               // mounted (or unmounted) as asked
#define MT_MOUNT_LOCKED     1               // mounted, but write-locked: the file is read-only
#define MT_MOUNT_FAILED     0777776         // failed; -1 in ones' complement, like lpf's error

bool mtOwnsIot(Word inst);
int mtIotHandler(PDP1 *pdp1P, int pulse, int completion);
void mtIOPoll(PDP1 *pdp1P);
void mtStart(void);
void mtStop(void);
void mtUpdate(void);

// For the host tests only: the control and its drives.
Mt550P mtControl(void);

#endif
