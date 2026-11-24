/*
 * Device 46 is the IBM 1301 disk's 131D channel, with SID, SWD and SDF.
 * Processing is handled by IOT 10 (IOT_10.c); this plugin only points the emulator there.
 */
#include "iotHandler.h"

// Returns the device that handles device 46's IOTs, 010.
int
iotAlias()
{
    return( 010 );
}
