// An IOT to return a 36 bit random number in IO and AC.

#include <sys/random.h>
#include <stdint.h>
#include <time.h>

#include "iotHandler.h"

// Used when getrandom() has nothing to give: with GRND_NONBLOCK it fails with EAGAIN until the
// kernel's pool is ready, early in a boot. Blocking instead would stall the emulator thread.
// An xorshift generator seeded from the clock is random enough for a demo.
static uint64_t
fallbackRandom(void)
{
static uint64_t state;
struct timespec ts;

    if( state == 0 )
    {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        state = ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec) | 1;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return(state);
}

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
uint64_t tmp;

    if( pulse )
    {
        return(1);
    }

    if( getrandom(&tmp, sizeof(tmp), GRND_NONBLOCK) != sizeof(tmp) )
    {
        tmp = fallbackRandom();
    }
    AC(pdp1P) = tmp & 0x3FFFF;
    IO(pdp1P) = (tmp >> 18) & 0x3FFFF;
    return(1);
}
