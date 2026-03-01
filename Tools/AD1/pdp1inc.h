#ifndef PDP1INC_H
#define PDP1INC_H

// pdp1.h needs these defined
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;

typedef struct FD
{
    int fd;
    int ready;
    int id;
} FD;

// We need this to pick up the definition of the PDP1 data structure.
#define USEAM1
#include "/opt/pidp1-mods/src/blincolnlights/pdp1/pdp1.h"
#endif
