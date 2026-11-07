// The operator's command console (console.c). cli() and handlecmd() are declared in pdp1.h.
#ifndef CONSOLE_H
#define CONSOLE_H

#include "pdp1.h"

// What the run, cont and halt commands ask of the main loop, which presses the matching panel key.
#define RUN_REQ_NONE 0
#define RUN_REQ_START 1         // the Start key, at the address switches
#define RUN_REQ_CONTINUE 2      // the Continue key
#define RUN_REQ_STOP 3          // the Stop key

void consoleStart(PDP1 *pdp);
int consoleTakeRunRequest(void);

#endif
