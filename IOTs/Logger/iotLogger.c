/**
 * This is a utility to allow debugging of dynamic IOTs.
 * It will initialize itself on first use creating a log file '/tmp/iot.dbg'.
 * It should be closed in an iotStop(), but it flushes its output so that's not strictly necessary.
 * Use: iotLog("same format as printf", .....);
 */

#include <stdarg.h>
#include <stdio.h>

#define DOLOGGING
#include "iotLogger.h"

static FILE *fP;

void
_iotLog(char *fmt, ...)
{
    if( !fP )
    {
        fP = fopen("/tmp/iot.dbg", "a");
        fprintf(fP,"Logging started\n");
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(fP, fmt, args);
    fflush(fP);
}

void
_iotCloseLog()
{
    if( fP )
    {
        fclose(fP);
        fP = 0;
    }
}
