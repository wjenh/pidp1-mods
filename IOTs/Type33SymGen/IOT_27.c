/*
 * This is an implementation of the PDP-1 Type 33 Character Generator for the Type 30 display.
 * IOT 26 also alises to this.
 *
 * According to the DEC documentation, it takes approximately 300 usecs per character to render.
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

//#define DOLOGGING
#include "iotLogger.h"

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

void drawDot(PDP1P pdp1P, int x, int y);

extern void flushdpy(DispConP dispP);
extern void dpycmd(PDP1P pdp1P, int screen, int cmd);
void display(PDP1P pdp1P, int screenNo, int x, int y, int intensity);
bool checkLightpen(PDP1P pdp1P, int x, int y);

static bool needCompletion;
static bool draw;
static bool autoSpace;
static bool charDone;           // a complete gpl, gpr cycle completed
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

int
iotHandler(PDP1P pdp1P, int dev, int pulse, int completion)
{
bool noWait;
uint64_t utmp;

    if( !pulse )
    {
        return(1);                  // only on one edge
    }

    iotLog("In iot 27 as %o\n", dev);

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
            iotLog("Glf, dotspace %d, sepspace %d, auto %d, intensity %d, x %04o y %04o\n",
                dotSpacing, sepSpacing, autoSpace, intensity, pdp1P->curDispX, pdp1P->curDispY);
        }
        else                                // gsp
        {
            // move right one character width plus one inter-character spacing if autospacing on
            pdp1P->curDispX += (5 * dotSpacing) + (autoSpace)?sepSpacing:0; 
            iotLog("Gsp, curDispX now %04o\n", pdp1P->curDispX);
        }
        break;

    case 027:           // gpl, gpr, gcf
        if( pdp1P->mb & GPLBIT )            // draw the left part of a character
        {
            onBits = 0;
            bitCtr = 17;                    // only 17 bits in left side
            shiftregister = pdp1P->io;
            subscript = (shiftregister & 01)?-dotSpacing * SUBOFFSET:0;
            xpos = pdp1P->curDispX;
            ystart = pdp1P->curDispY;
            intensity = pdp1P->curDispIntensity;        // pick up from last sdb or dpy
            xctr = yctr = 0;
            draw = true;
            charDone = false;
            enablePolling(DOTDELAY);
            iotLog("Gpl, io %06o curDispX %04o curDispY %04o sr %06o\n",
                pdp1P->io, pdp1P->curDispX, pdp1P->curDispY, shiftregister);
        }
        else if( pdp1P->mb & GCFBIT )       // clears light pen flag, cks bit 0400000
        {
            pdp1P->cksflags &= ~0400000;
            noWait = true;
        }
        else                                // gpr
        {
            onBits = 0;
            bitCtr = 18;                    // full 18 bits in right side
            shiftregister = pdp1P->io;
            iotLog("Gpr, io %06o sr %06o\n", pdp1P->io, shiftregister);
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
}

void
iotStop()
{
    iotCloseLog();
}

void
drawDot(PDP1P pdp1P, int x, int y)
{
int cmd, delayTime;
DispConP dispP;

    /*
    // Sym gen is only on display 0.
    dispP = &(pdp1P->dpy[0]);

    if( dispP->fd < 0 )  // no connection open
    {
        return;
    }

    if(x & 01000)           // 1's cmpl PDP-1 wraparound
    {
        x++;
    }
    if(y & 01000)
    {
        y++;
    }

    x = (x+01000) & 01777;  // why?
    y = (y+01000) & 01777;

    delayTime = (pdp1P->simtime - pdp1P->dpy[0].last)/1000;
    cmd = x | (y<<10) | (delayTime<<23);
    if( intensity == 4 )    // sdb wasn't used, dpy-i 400 was, override
    {
        intensity = 0;
    }

    cmd |= ((intensity + 4) & 7) << 20;

    dispP->last = pdp1P->simtime;
    dpycmd(pdp1P, 0, cmd);

    if( (dispP->fd >= 0) && (pdp1P->dpy[1].fd >= 0) )    // check for dual screens
    {
        if( !!(intensity & 4) != 1 )
        {
            return;
        }

        // unclear what's happening here exactly
        // spacewar 4.4 uses only intensity 0/4
        delayTime = (pdp1P->simtime - pdp1P->dpy[1].last)/1000;
        cmd = x | (y<<10) | (delayTime<<23);

        cmd |= (((intensity & 3) + 4) & 7) << 20;
        dpycmd(pdp1P, 1, cmd);
    }
    */

    display(pdp1P, 0, x, y, intensity);
}

// Actually put out our dots
void
iotPoll(PDP1P pdp1P)
{
int bit;
int totalTime;

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
                iotLog("Poll, sr %06o, drawing xctr %d yctr %d, xpos %d ypos %d\n",
                    shiftregister & 0777777, xctr, yctr, xpos, ystart + (yctr * dotSpacing) + subscript);

                drawDot(pdp1P, xpos, ystart + (yctr * dotSpacing) + subscript);
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
            pdp1P->curDispX = xpos;

            // Wait our remaining delay time, 2 usec for each bit that was off, we already waited
            // 5usec per on bit plus the instruction cycle itself.
            // We will always get called one more time with draw off to complete the operation.
            totalTime = ((35 - onBits) * 2) / 5;    // converted to cycles
            iotLog("%d on, %d off\n", onBits, 35 - onBits);
            if( totalTime > 0 )
            {
                draw = false;
                enablePolling(totalTime);
                iotLog("Delay %d cycles\n", totalTime);
                return;
            }
            else
            {
                iotLog("No delay\n");
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
            // We already put the dots out, just tell display() to update with no visible dot
            display(pdp1P, 0, xpos, ystart, 4);
            checkLightpen(pdp1P, xpos, ystart);
        }

        if( needCompletion )
        {
            IOCOMPLETE(pdp1P);
        }

        iotLog("Character display complete, curDispX %04o curDispY %04o\n", pdp1P->curDispX, pdp1P->curDispY);
        enablePolling(0);           // no need to poll now
    }
}
