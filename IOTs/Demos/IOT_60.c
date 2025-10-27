#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

/*
 * This IOT enables or disables sbs16 mode.
 */

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
    pdp1P->sbs16 = (pdp1P->mb >> 6) & 01;      // we expect the 'control' bits to have on/off
    return(1);
}
