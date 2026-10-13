/*
 * IOT 72: wavetable mode's commands for the four D/A converters. The device is IOT 71
 * (IOT_71.c), which handles both device numbers; this plugin only points the emulator there.
 */

#include "iotHandler.h"

// Returns the device that handles IOT 72's commands, 071.
int
iotAlias()
{
    return( 071 );
}
