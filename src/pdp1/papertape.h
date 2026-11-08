// Paper tape reader and punch files (papertape.c): mounting a tape by path or fd from any
// thread, and handleio(), which adopts it on the emulator thread.
#ifndef PAPERTAPE_H
#define PAPERTAPE_H

#include "pdp1.h"

// mountReaderPath() and mountPunchPath() results.
#define TAPE_MOUNTED 0          // mounted, or unmounted as asked
#define TAPE_WAITING 1          // a reader FIFO with no writer yet: mounted when one opens it
#define TAPE_FAILED (-1)        // couldn't be opened: the device has no tape

void mountReader(int fd);
void mountPunch(int fd);
int mountReaderPath(const char *pathP);
int mountPunchPath(const char *pathP);

#endif
