#include "common.h"
#include "pdp1.h"
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
    if( pdp1P->ac & 0400000 )
        pdp1P->ac = 1;          // bit wraparound
    else
        (pdp1P->ac) <<= 1;
}
