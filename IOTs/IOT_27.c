#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "pdp1.h"
#include "iotHandler.h"

//#define DOLOGGING
#include "Logger/iotLogger.h"

/*
 * This is an implementation of the PDP-1 Type 33 Character Generator for the Type 30 display.
 * IOT 26 also alises to this.
 *
 * According to the DEC documentation, it takes approximately 300 usecs per character to render.
 *
 */

#define GPLBIT 02000
#define GCFBIT 00100
#define GLFBIT 02000

// The spacing between dots is controlled by the glf iot.
// The actual spacing is 2 + (size value in iot) pixels.

#define SUBOFFSET   2       // number of dot spacings to offset for a subscript
#define SEPNUM      2       // number of dot spacings for autospacing between chars

#define INTENSITY   3       // dpy intensity, 4 is off, 0 is normal, 3 is brightest
#define DELAY       2       // 5 usec cycles between dot updates
#define DPYDELAY    2       // delay in usecs passed to the dpy code

#define USECS(s) (s * 1000)  // convert us to ns

void drawDot(PDP1 *pdp1P, int x, int y, int visible);

static bool needCompletion;
static bool draw;
static bool autoSpace;
static int subscript;
static int dotSpacing;
static int xctr, yctr;
static int xpos, ystart;
static uint64_t shiftregister;

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
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

            dotSpacing = 2 + (pdp1P->io & 03);
            autoSpace = pdp1P->io & 04;
            subscript = 0;
            iotLog("Glf, dotspace %d, auto %d, x %d y %d\n", dotSpacing, autoSpace, pdp1P->dbx, pdp1P->dby);
        }
        else                                // gsp
        {
            pdp1P->dbx += (5 * dotSpacing) + (dotSpacing * SEPNUM); 
            iotLog("Gsp, dbx %d\n", pdp1P->dbx);
        }
        break;

    case 027:           // gpl, cpr, gcf
        if( pdp1P->mb & GPLBIT )            // draw the left part of a character
        {
            noWait = true;
            subscript = (pdp1P->io & 01)?-dotSpacing * SUBOFFSET:0;
            utmp = (uint64_t)(pdp1P->io);
            shiftregister = (utmp & 0777776) << 18;
            xpos = pdp1P->dbx;
            ystart = pdp1P->dby;
            iotLog("Gpl, io %06o dbx %d dby %d sr %012lo\n", pdp1P->io, pdp1P->dbx, pdp1P->dby, shiftregister);
        }
        else if( pdp1P->mb & GCFBIT )       // clears light pen flag, which we don't have, does nothing
        {
            noWait = true;
        }
        else                                // gpr
        {
            utmp = (uint64_t)(pdp1P->io);
            shiftregister |= utmp << 1;
            iotLog("Gpr, io %06o sr %012lo\n", pdp1P->io, shiftregister);
            xctr = yctr = 0;
            draw = true;
            enablePolling(DELAY);           // go too fast, display can't keep up
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
drawDot(PDP1 *pdp1P, int x, int y, int visible)
{
    pdp1P->dcp = 0;
    pdp1P->dbx = x;
    pdp1P->dby = y;
    pdp1P->dint = visible?INTENSITY:4;
    pdp1P->dpy_defl_time = pdp1P->simtime + USECS(DPYDELAY);
    pdp1P->dpy_time = pdp1P->dpy_defl_time + USECS(DPYDELAY);
}

// Actually put out our dots
void
iotPoll(PDP1 *pdp1P)
{
    if( draw )
    {
        // we draw one dot per poll
        while( !(shiftregister & 0400000000000) )
        {
            shiftregister = (shiftregister << 1) & 0777777777777;

            if( ++yctr > 6)
            {
                yctr = 0;
                if( ++xctr > 4 )            // all finished
                {
                    if( autoSpace )
                    {
                        xpos += dotSpacing * SEPNUM;
                    }

                    draw = false;
                    break;
                }
                else
                {
                    xpos += dotSpacing;
                }
            }
        }

        if( draw )      // if true, sr bit was a 1, draw a dot
        {
            iotLog("Poll, sr %012lo, drawing xctr %d yctr %d, xpos %d ypos %d\n",
                shiftregister, xctr, yctr, xpos, ystart + (yctr * dotSpacing) + subscript);

            drawDot(pdp1P, xpos, ystart + (yctr * dotSpacing) + subscript, 1);
            shiftregister = (shiftregister << 1) & 0777777777777;

            if( ++yctr > 6 )
            {
                yctr = 0;
                xpos += dotSpacing;

                if( ++xctr > 4 )
                {
                    draw = false;
                
                    if( autoSpace )
                    {
                        xpos += dotSpacing * SEPNUM;
                    }

                    return;             // we will be called one more time with draw off, let last op complete
                }
            }
        }
    }

    if( !draw )
    {
        pdp1P->dbx = xpos;
        pdp1P->dby = ystart;

        if( needCompletion )
        {
            IOCOMPLETE(pdp1P);
        }

        iotLog("Character display complete, dbx %d dby %d\n", pdp1P->dbx, pdp1P->dby);
        enablePolling(0);           // no need to poll now
    }
}
