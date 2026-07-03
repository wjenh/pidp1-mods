/**
 * This IOT is part of the pair (5, 6) that implement the paper tape punch (ppa/ppb).
 * Device 5 is ppa (alphanumeric); processing is handled by IOT 6 (ppb's handler), which
 * tells the two devices apart by the device number dynamicIotProcessor() passes through.
 *
 * 19-Jun-2026 wje initial version.
*/
#include "iotHandler.h"

// Tells dynamicIots.c's initializeEntry() that device 5 (ppa) has no handler of its own and
// should instead be serviced by whatever handler is loaded for the device number returned here.
// Returns 6 (octal), the device number of ppb's handler (IOT_6.c), which tells the two devices
// apart at runtime by the original device number dynamicIotProcessor() still passes through.
int
iotAlias()
{
    return( 06 );
}
