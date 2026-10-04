/*
 * This is an implementation of the PDP-1 Type 33 Character Generator for the Type 30 display.
 * IOT 26 also alises to this.
 *
 * DEC's Type 33 manual (1964, pages 3-19 to 3-22, Table 3-7) times a character as 35 dot
 * positions, gpl's 17 then gpr's 18, one timing cycle each: 2 usec, plus 3 usec to intensify
 * when the bit is 1. After the 35th, 4 to 7 more cycles (character sizes 1 to 4) move the
 * matrix to the next character. So 78-84 usecs with no dot lit and 183-189 usecs with all 35
 * lit, plus the program's own time between the two IOTs. This code times all of it, as
 * deadlines on the device time base (iotPollAt(), iotHandler.h): each half charges its own
 * positions, each lit dot is drawn when its position comes due, and gpr adds the increment
 * cycles before it completes.
 *
 * Note that we store dpy coords in -511,+511 style, only used by this and the Type 33 symgen.
 *
 * 21-Jun-2026 wje cleanup, no functional change
 * 4-Jul-2026 wje more cleanup, minor fixes, no significant functional change
 * 4-Oct-2026 Claude power clear abandons a character and clears the light pen status.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>

#include "display.h"
#include "iotHandler.h"
#include "configuration.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_CMD 0
#define LOG_POLL 0
#define LOG_IOT 0
#define LOG_DRAW 0
#define LOG_BITS 0
#define LOG_BOUNDS 0
#define LOG_CONFIG 0

#define GPLBIT 02000
#define GCFBIT 00100
#define GLFBIT 02000

// The spacing between dots is controlled by the glf iot.
// The actual spacing is 2 + (size value in iot) pixels.

#define DOTSPACE    2       // smallest dot spacing, min size is this
#define SUBOFFSET   2       // base number of dot spacings to offset for a subscript
#define SEPSPACING  4       // base number of pixel for autospacing between chars

#define DARKNS      USTONS(2)   // a dot position whose bit is 0: one timing cycle
#define LITNS       USTONS(5)   // a bit of 1 adds 3 usecs to intensify
#define INCREMENTNS USTONS(2)   // each increment cycle after the 35th position
#define INCREMENTS  4           // increment cycles at size 1, one more for each size up

static bool needCompletion;
static bool draw;
static bool autoSpace;
static bool charDone;           // a complete gpl, gpr cycle completed
static bool lightpenEnabled;
static int subscript;
static int dotSpacing;          // spacing between pixels inside a char
static int sepSpacing;          // spacing in pixels between chars when autospacing is on
static int charSize;            // 0-3, total dot spacing is DOTSPACE + charsize
static int intensity;
static int xctr, yctr;
static int xpos, ystart;
static int shiftregister;
static int bitCtr;
static uint64_t dueNs;          // device time at which the next dot position, or the end, is due
static PDP1P lastPdp1P;         // from the last call that had it, for the power clear; NULL if
                                // none, and then no light pen status can have been set

static void configure(void);
static int flagToBits(int);

extern int cvtDpyTo1024(int);

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
int x, y;
bool noWait;

    if( !pulse )
    {
        return(1);                  // only on one edge
    }

    iotCondLog(LOG_IOT, "In iot 27 as %o\n", dev);

    lastPdp1P = pdp1P;
    noWait = false;
    needCompletion = completion;

    switch( dev )
    {
    case 026:            // glf, gsp
        if( MB(pdp1P) & GLFBIT )
        {
            noWait = true;

            charSize = IO(pdp1P) & 03;
            dotSpacing = DOTSPACE + charSize;
            sepSpacing = SEPSPACING + charSize;
            autoSpace = IO(pdp1P) & 04;
            subscript = 0;
            intensity = 0;      // manual says sets to normal
            x = y = 0;
            iotCondLog(LOG_CMD, "Glf, dotspace %d, sepspace %d, auto %d, intensity %d, x %04o y %04o\n",
                dotSpacing, sepSpacing, autoSpace, intensity, x, y);
        }
        else                                // gsp
        {
            // Move right one character width, plus one inter-character separation if
            // autospacing is enabled. This completes immediately (no dots are drawn), so
            // flag it as such. This lets the noWait/needCompletion check below fire
            // IOCOMPLETE() right away if the caller asked for one (e.g. "gsp C"), instead
            // of silently dropping the completion pulse forever.
            noWait = true;

            lockDisplayData(0);
            getDisplayData(0, &x, &y, &intensity);
            x += (5 * dotSpacing) + ((autoSpace)?sepSpacing:0);
            setDisplayData(0, x, -1, -1);
            unlockDisplayData(0);
            iotCondLog(LOG_CMD, "Gsp, x now %04o\n", x);
        }
        break;

    case 027:           // gpl, gpr, gcf
        if( MB(pdp1P) & GPLBIT )            // draw the left part of a character
        {
            bitCtr = 17;                    // only 17 bits in left side
            shiftregister = IO(pdp1P);
            subscript = (shiftregister & 01)?-dotSpacing * SUBOFFSET:0;
            getDisplayData(0, &x, &y, &intensity);
            xpos = x;
            ystart = y;
            xctr = yctr = 0;
            draw = true;
            charDone = false;
            dueNs = iotTime(IOT_TIME_DEVICE);
            iotPollAt(IOT_TIME_DEVICE, dueNs);
            iotCondLog(LOG_CMD, "Gpl, io %06o x %04o y %04o sr %06o\n",
                IO(pdp1P), x, y, shiftregister);
        }
        else if( MB(pdp1P) & GCFBIT )       // clears light pen flag, cks bit 0400000
        {
            CKS(pdp1P) &= ~0400000;
            noWait = true;
        }
        else                                // gpr
        {
            // xctr and yctr were left by gpl in the right state for gpr
            bitCtr = 18;                    // full 18 bits in right side
            shiftregister = IO(pdp1P);
            iotCondLog(LOG_CMD, "Gpr, io %06o sr %06o\n", IO(pdp1P), shiftregister);
            draw = true;
            dueNs = iotTime(IOT_TIME_DEVICE);
            iotPollAt(IOT_TIME_DEVICE, dueNs);
        }
        break;
    }

    if( noWait && needCompletion )
    {
        needCompletion = false;
        IOCOMPLETE(pdp1P);                  // no completion pulse if noWait
    }

    return(1);
}

void
iotStart()
{
    iotLog("IOT 27 started\n");
    configure();
}

void
iotStop()
{
    iotCloseLog();
}

// Called once when the power switch goes off, before iotStop(). The core has disarmed the
// deadline, so a character being drawn is abandoned, and its completion with it. The light pen
// status, CKS 0400000, is cleared. The format set by glf (size, auto-space) is kept: the Type 33
// manual (1964) has no clear input for the format buffer, whose flip-flops change only by a
// jam transfer from glf (pp. 3-16, 3-18), and none of its interface signals is a power clear
// (Table 2-1). Its intensity and subscript are cleared by the next gpl, as now.
// No return value.
void
iotPowerClear(void)
{
    draw = false;
    charDone = false;
    needCompletion = false;
    bitCtr = 0;

    if( lastPdp1P )
    {
        CKS(lastPdp1P) &= ~0400000;
    }
}

// Actually put out our dots, each when its position comes due, then complete when the
// half, and for gpr the increment cycles, are over.
void
iotDeadline(PDP1P pdp1P)
{
int bit;
int x, y;
uint64_t now;

    lastPdp1P = pdp1P;
    now = iotTime(IOT_TIME_DEVICE);

    if( draw )
    {
        // Several positions can come due in one main-loop pass: take them all.
        while( bitCtr && (dueNs <= now) )
        {
            bit = shiftregister & 0400000;
            bitCtr--;
            shiftregister <<= 1;

            if( bit )
            {
                iotCondLog(LOG_DRAW, "Poll, sr %06o, drawing xctr %d yctr %d, xpos %d ypos %d\n",
                    shiftregister & 0777777, xctr, yctr, xpos, ystart + (yctr * dotSpacing) + subscript);

                y = ystart + (yctr * dotSpacing) + subscript;
                if(  (xpos < 0) || (y < 0) || (xpos > 01777) || (y > 01777) )
                {
                    iotCondLog(LOG_BOUNDS, "Boundary, x %d y %d\n", xpos, y);
                }
                display( 0, cvtDpyTo1024(xpos), cvtDpyTo1024(y), type30Intensity(intensity));
                dueNs += LITNS;
            }
            else
            {
                dueNs += DARKNS;
            }

            if( ++yctr > 6)
            {
                yctr = 0;
                ++xctr;
                xpos += dotSpacing;
            }
        }

        if( bitCtr )
        {
            iotPollAt(IOT_TIME_DEVICE, dueNs);     // the next position
            return;
        }

        draw = false;                       // this half's positions are all taken

        if( xctr > 4 )          // completed a full character
        {
            charDone = true;

            if( autoSpace )
            {
                xpos += sepSpacing;
            }

            // We update the global position
            lockDisplayData(0);
            setDisplayData(0, xpos, -1, -1);
            unlockDisplayData(0);

            // The increment cycles move the matrix on to the next character.
            dueNs += (INCREMENTS + charSize) * INCREMENTNS;
            iotCondLog(LOG_BITS, "Increment cycles %d\n", INCREMENTS + charSize);
        }

        // The last position, and for gpr the increment cycles, may still be running.
        if( dueNs > now )
        {
            iotPollAt(IOT_TIME_DEVICE, dueNs);
            iotCondLog(LOG_POLL, "Complete at %llu, now %llu\n",
                (unsigned long long)dueNs, (unsigned long long)now);
            return;
        }
    }

    if( !draw )
    {
        if( charDone )
        {
            // We already put the dots out, just move the current location
            iotCondLog(LOG_DRAW, "display invisible xpos %d, ystart %d\n", xpos, ystart);
            if(  (xpos < 0) || (ystart < 0) || (xpos > 01777) || (ystart > 01777) )
            {
                iotCondLog(LOG_BOUNDS, "Done, but Boundary, x %d y %d\n", xpos, ystart);
            }

            lockDisplayData(0);
            setDisplayData(0, xpos, ystart, -1);
            unlockDisplayData(0);
            if( lightpenEnabled && checkLightpen(0, cvtDpyTo1024(xpos), cvtDpyTo1024(ystart)) )
            {
                CKS(pdp1P) |= 0400000;               // cleared by next dpy
                PFLAGS(pdp1P) |= flagToBits(3);
            }
        }

        if( needCompletion )
        {
            IOCOMPLETE(pdp1P);
        }

        getDisplayData(0, &x, &y, 0);
        iotCondLog(LOG_DRAW, "Character display complete, x %04o y %04o\n", x, y);
    }
}

// Convert a flag number to the bits needes for program flags
int
flagToBits(int bits)
{
    switch(bits & 7)
    {
    case 1:
        return(040);
    case 2:
        return(020);
    case 3:
        return(010);
    case 4:
        return(004);
    case 5:
        return(002);
    case 6:
        return(001);
    case 7:
        return(077);
    }

    return(0);
}

// Get our configurations settings, can be called more than once.
void
configure()
{
ConfigurationSettingP settingP;

    iotCondLog(LOG_CONFIG, "IOT 27 checking configuration\n");
    lightpenEnabled = false;

    if( (settingP = findConfigurationSetting(getConfiguration(), "lightpen")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 lightpen is enabled\n");
        lightpenEnabled = settingP->onOff;
    }
}
