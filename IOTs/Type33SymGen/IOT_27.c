/*
 * This is an implementation of the PDP-1 Type 33 Character Generator for the Type 30 display.
 * IOT 26 also alises to this.
 *
 * According to the DEC documentation, it takes approximately 300 usecs per character to render.
 *
 * Note that we store dpy coords in -511,+511 style, only used by this and the Type 33 symgen.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "pdp1.h"
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

#define DOTDELAY    1       // 5 usec cycles between dot updates
#define DPYDELAY    5000    // setup delay in NANOSECS passed to the dpy code

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
static int onBits;              // track the number of on bits we saw, for timing computation

static void configure(void);
static int flagToBits(int);

extern int cvtDpyTo1024(int);

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
int x, y, intensity;
bool noWait;
uint64_t utmp;

    if( !pulse )
    {
        return(1);                  // only on one edge
    }

    iotCondLog(LOG_IOT, "In iot 27 as %o\n", dev);

    noWait = false;
    needCompletion = completion;

    switch( dev )
    {
    case 026:            // glf, gsp
        if( pdp1P->mb & GLFBIT )
        {
            noWait = true;

            charSize = pdp1P->io & 03;
            dotSpacing = DOTSPACE + charSize;
            sepSpacing = SEPSPACING + charSize;
            autoSpace = pdp1P->io & 04;
            subscript = 0;
            intensity = 0;      // manual says sets to normal
            iotCondLog(LOG_CMD, "Glf, dotspace %d, sepspace %d, auto %d, intensity %d, x %04o y %04o\n",
                dotSpacing, sepSpacing, autoSpace, intensity, x, y);
        }
        else                                // gsp
        {
            // move right one character width plus one inter-character spacing if autospacing on
            lockDisplayData(0);
            getDisplayData(0, &x, &y, &intensity);
            x += (5 * dotSpacing) + (autoSpace)?sepSpacing:0; 
            setDisplayData(0, x, -1, -1);
            unlockDisplayData(0);
            iotCondLog(LOG_CMD, "Gsp, x now %04o\n", x);
        }
        break;

    case 027:           // gpl, gpr, gcf
        if( pdp1P->mb & GPLBIT )            // draw the left part of a character
        {
            onBits = 0;
            bitCtr = 17;                    // only 17 bits in left side
            shiftregister = pdp1P->io;
            subscript = (shiftregister & 01)?-dotSpacing * SUBOFFSET:0;
            getDisplayData(0, &x, &y, &intensity);
            xpos = x;
            ystart = y;
            xctr = yctr = 0;
            draw = true;
            charDone = false;
            enablePolling(DOTDELAY);
            iotCondLog(LOG_CMD, "Gpl, io %06o x %04o y %04o sr %06o\n",
                pdp1P->io, x, y, shiftregister);
        }
        else if( pdp1P->mb & GCFBIT )       // clears light pen flag, cks bit 0400000
        {
            pdp1P->cksflags &= ~0400000;
            noWait = true;
        }
        else                                // gpr
        {
            // xctr and yctr were left by gpl in the right state for gpr
            onBits = 0;
            bitCtr = 18;                    // full 18 bits in right side
            shiftregister = pdp1P->io;
            iotCondLog(LOG_CMD, "Gpr, io %06o sr %06o\n", pdp1P->io, shiftregister);
            draw = true;
            enablePolling(DOTDELAY);
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

// Actually put out our dots
void
iotPoll(PDP1P pdp1P)
{
int bit;
int totalTime;
int x, y;

    if( draw )
    {
        // we draw one dot per poll
        bit = 0;

        while( bitCtr )
        {
            bit = shiftregister & 0400000;
            bitCtr--;
            shiftregister <<= 1;

            if( bit )
            {
                onBits++;
                iotCondLog(LOG_DRAW, "Poll, sr %06o, drawing xctr %d yctr %d, xpos %d ypos %d\n",
                    shiftregister & 0777777, xctr, yctr, xpos, ystart + (yctr * dotSpacing) + subscript);

                y = ystart + (yctr * dotSpacing) + subscript;
                if(  (xpos < 0) || (y < 0) || (xpos > 01777) || (y > 01777) )
                {
                    iotCondLog(LOG_BOUNDS, "Boundary, x %d y %d\n", xpos, y);
                }
                display( 0, cvtDpyTo1024(xpos), cvtDpyTo1024(y), type30Intensity(intensity));
            }

            if( ++yctr > 6)
            {
                yctr = 0;
                ++xctr;
                xpos += dotSpacing;
            }

            if( bit )
            {
                return;         // wait for the next dot time
            }
        }

        if( xctr > 4 )          // completed a full character
        {
            draw = false;
            charDone = true;
        
            if( autoSpace )
            {
                xpos += sepSpacing;
            }

            // We update the global position
            lockDisplayData(0);
            setDisplayData(0, xpos, -1, -1);
            unlockDisplayData(0);

            // Wait our remaining delay time, 2 usec for each bit that was off, we already waited
            // 5usec per on bit plus the instruction cycle itself.
            // We will always get called one more time with draw off to complete the operation.
            totalTime = ((35 - onBits) * 2) / 5;    // converted to cycles
            iotCondLog(LOG_BITS, "%d on, %d off\n", onBits, 35 - onBits);
            if( totalTime > 0 )
            {
                draw = false;
                enablePolling(totalTime);
                iotCondLog(LOG_POLL, "Delay %d cycles\n", totalTime);
                return;
            }
            else
            {
                iotLog(LOG_POLL, "No delay\n");
            }
        }

        if( bitCtr <= 0 )
        {
            draw = false;                   // end of left side, gdl ends
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
            if( lightpenEnabled && checkLightpen(pdp1P, 0, cvtDpyTo1024(xpos), cvtDpyTo1024(ystart)) )
            {
                pdp1P->cksflags |= 0400000;               // cleared by next dpy
                pdp1P->pf |= flagToBits(3);
            }
        }

        if( needCompletion )
        {
            IOCOMPLETE(pdp1P);
        }

        getDisplayData(0, &x, &y, 0);
        iotCondLog(LOG_DRAW, "Character display complete, x %04o y %04o\n", x, y);
        enablePolling(0);           // no need to poll now
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
ConfigurationP confP;
ConfigurationSettingP settingP;

    iotCondLog(LOG_CONFIG, "IOT 27 checking configuration\n");
    lightpenEnabled = false;

    if( (settingP = findConfigurationSetting(getConfiguration(), "lightpen")) )
    {
        iotCondLog(LOG_CONFIG, "IOT 7 lightpen is enabled\n");
        lightpenEnabled = settingP->onOff;
    }
}
