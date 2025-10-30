/**
 * This is a utility to allow debugging.
 * It will initialize itself on first use creating a log file '/tmp/dbg.log'.
 * It should be closed, but it flushes its output so that's not strictly necessary.
 * Use: logger("same format as printf", .....);
 */

#include <stdarg.h>
#include <stdio.h>

#define DOLOGGING
#include "logger.h"

static FILE *fP;

void
_logger(char *fmt, ...)
{
    if( !fP )
    {
        fP = fopen("/tmp/dbg.log", "a");
        fprintf(fP,"Logging started\n");
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(fP, fmt, args);
    fflush(fP);
}

void
_closeLog()
{
    if( fP )
    {
        fclose(fP);
        fP = 0;
    }
}
