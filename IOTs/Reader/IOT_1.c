/**
 * This IOT is part of the pair (1, 2) that implement the paper tape reader (rpa/rpb).
 * Device 1 is rpa (alphanumeric); processing is handled by IOT 2 (rpb's handler), which
 * tells the two devices apart by the device number dynamicIotProcessor() passes through.
 * Device 1 also carries the Type 550 Microtape (sub-devices 2-7, 720201-720701); IOT 2's
 * handler routes those to the tape as well (see IOT_2.c and IOTs/Microtape/microtape.c).
 *
 * 19-Jun-2026 wje initial version.
 * 11-Sep-2026 Claude -- comment: the Microtape shares device 1.
*/
#include "iotHandler.h"

// Tells dynamicIots.c's initializeEntry() that device 1 (rpa) has no handler of its own and
// should instead be serviced by whatever handler is loaded for the device number returned here.
// Returns 2 (octal), the device number of rpb's handler (IOT_2.c), which tells the two devices
// apart at runtime by the original device number dynamicIotProcessor() still passes through.
int
iotAlias()
{
    return( 02 );
}
