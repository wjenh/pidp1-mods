/*
 * This is an implementation of a BCD interface to the classic Chrono-Log series 2100 time-of-day clock.
 * It's a virtual certianty one existed on some PDP-1.
 *
 * The Chrono-Log returned the time in BCD, binary coded decimal, typical for the era.
 * If there was a real one connected, the BCD would have been compressed to fit.
 * It's here in tribute to the hackers of the time.
 * Besides, the adventure game needs it. :)
 * It's quite simple, no interrupts, nothing fancy.
 * IOT 70 returns the hours, minutes, and seconds in AC, day, month, year in IO.
 * Classic bcd-to-binary bit compression:
 * 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
 * | HH 0-23     | MM 0-59      | SS 0-59    |
 * | MM 1-12  | DD 1-31     | YYY 0-511      |
 *
 * This is HH - 5 bits, MM - 6 bits, SS - 6 bits,
 * and MM - 4 bits, DD, 5 bits, YYY, 9 bits.
 *
 * The year is 3 digits, 026 for 2026. I'm optimistic.
 *
 * No waits, no completion, 
 *
 * 24-Aug-2026 wje initial implementation
 */

#include <time.h>

#include "iotHandler.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_IOT 0

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
int i;
time_t raw;
struct tm *tmP;

    if( !pulse )
    {
        return(1);                  // only on one edge
    }

    iotCondLog(LOG_IOT, "In iot 70\n");

    if( completion )
    {
        IOCOMPLETE(pdp1P);          // Not supported, just clear it
    }

    time(&raw);
    tmP = localtime(&raw);

    i = (tmP->tm_hour & 0x1F) << 13;  // mask just for completeness
    i |= (tmP->tm_min & 0x3F) << 6;
    i |= tmP->tm_sec & 0x3F;
    AC(pdp1P) = i;

    i = ((tmP->tm_mon  + 1) & 0xF) << 14;  // mask just for completeness
    i |= (tmP->tm_mday & 0x3F) << 9;
    i |= tmP->tm_year - 100;          // relative to 2000
    IO(pdp1P) = i;

    return(1);
}
