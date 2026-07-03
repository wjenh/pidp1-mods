#include <stdio.h>
#include "iotHandler.h"

/*
 * This works with demo57.mac to demonsrate an IOT handler.
*/

static FILE *fP;

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int chan;

    if( !fP )
    {
        if( !(fP = fopen("/tmp/iot", "a")) )
        {
            return(0);      // this will fail us, emulator will see nothing is implemented.
        }
    }

    if( pulse )     // we are in clock cycle TP10
    {
        fprintf(fP,"IOT 57 called, pulse 1.\n");
        fprintf(fP,"IOT 57 request %d b1 %d b2 %d b3 %d b4 %d.\n",
            (MB(pdp1P) >> 6) & 077, pdp1P->b1, pdp1P->b2, pdp1P->b3, pdp1P->b4);
    }
    else            // we are in clock cycle TP7
    {
        fprintf(fP,"IOT 57 called, pulse 0 mb %o.\n", MB(pdp1P));
        chan = (MB(pdp1P) >> 6) & 077;      // we expect the 'control' bits to have a channel number
        initiateBreak(chan);
        fprintf(fP,"IOT 57 break chan %d done, sbs16 is %d, b1 is %d.\n",
            chan,pdp1P->sbs16,pdp1P->b1);
    }

    fflush(fP);
    return(1);
}

void
iotStart()
{
    if( !fP )
    {
        fP = fopen("/tmp/iot", "a");
    }

    if( fP )
    {
        fprintf(fP,"iotStart()\n");
    }
}

void
iotStop()
{
    if( fP )
    {
        fprintf(fP,"iotStop()\n");
        fclose(fP);
        fP = 0;
    }
}
