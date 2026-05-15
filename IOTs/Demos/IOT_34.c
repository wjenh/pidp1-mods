// An IOT to return a 36 bit random number in IO and AC.

#include <sys/random.h>
#include <stdint.h>

#include "iotHandler.h"

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
uint64_t tmp;

    if( pulse )
    {
        return(1);
    }

    getrandom(&tmp, sizeof(tmp), GRND_NONBLOCK);
    AC(pdp1P) = tmp & 0x3FFFF;
    IO(pdp1P) = (tmp >> 18) & 0x3FFFF;
    return(1);
}
