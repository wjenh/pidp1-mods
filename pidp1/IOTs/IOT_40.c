#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
    enablePolling(1);
    return(1);
}

static int cycles = 200000;    // about 1 sec

void iotPoll(PDP1 *pdp1P)
{
    if( !--cycles )
    {
        pdp1P->pf = 1;      // turn on program flag 6
    }
}
