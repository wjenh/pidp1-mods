#include "iotHandler.h"

// An example of polling
int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
    enablePolling(20000);  // poll every 20K cycles, 0.1 secs
    return(1);
}

void iotPoll(PDP1 *pdp1P)
{
    if( AC(pdp1P) & 0400000 )
        AC(pdp1P) = 1;          // bit wraparound
    else
        (AC(pdp1P)) <<= 1;
}
