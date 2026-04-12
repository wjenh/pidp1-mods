// All the global code for managing the Type 30 display is here, moved frm pdp1.c.
// It can't be encapsulated in IOT 7 because other code needs it, such as the symbol generator.
// 12-Apr-2026 wje initial version

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "common.h"
#include "pdp1.h"

//#define DOLOGGING
#include "logger.h"
#define LOG_UPDATE 0
#define LOG_LP 0

#define PENBUFSIZE  64              // read up to this many lightpen update commands at once

void display(PDP1 *pdp, int screenNo, int x, int y, int intensity);
void flushdpy(DispConP dispP);
void dpycmd(PDP1P pdp1P, int screenNo, u32 cmd);
bool checkLightpen(PDP1 *pdp1P, int x, int y);

static bool lightpenReader(PDP1P pdp1P, int *xP, int *yP, bool *penDownP);

extern int decflg(int flg);

void
dpycmd(PDP1P pdp1P, int screenNo, u32 cmd)
{
DispConP dispP;

    dispP = &pdp1P->dpy[screenNo];
    dispP->cmdbuf[dispP->ncmds++] = cmd;

    if(dispP->ncmds == nelem(dispP->cmdbuf))
    {
        flushdpy(dispP);
    }
}

void
flushdpy(DispConP dispP)
{
int size;
int n;

    size = dispP->ncmds * sizeof(dispP->cmdbuf[0]);
    n = write(dispP->fd, dispP->cmdbuf, size);
    dispP->ncmds = 0;

    if(n < size)
    {
        close(dispP->fd);
        dispP->fd = -1;
    }
}

void
agedisplay(PDP1P pdp1P, int screenNo)
{
u64 delayTime;
DispCon *dspP;

    dspP = &(pdp1P->dpy[screenNo]);

    if( dspP->fd < 0)
    {
        return;
    }

    delayTime = (pdp1P->simtime -  dspP->last) / 1000;

    // Agetime is in microseconds
    if( delayTime >= dspP->agetime )
    {
        dpycmd(pdp1P, screenNo, 511 << 23);
        dpycmd(pdp1P, screenNo, delayTime);
        dspP->last = pdp1P->simtime;
        flushdpy(dspP);

        // increase interval during fade out
        // to reduce number of age commands
        if( dspP->agetime < (1000 * 1000))
        {
             dspP->agetime +=  dspP->agetime / 6;
        }
    }
}

void
display(PDP1P pdp1P, int screenNo, int x, int y, int intensity)
{
int delayTime;
int cmd;
DispConP dpyP;

    if( (screenNo < 0) || (screenNo > 1) )
    {
        return;     // only 2 screens
    }

    dpyP = &(pdp1P->dpy[screenNo]);

    if( dpyP->fd < 0 )
    {
        return;     // not open, don't bother
    }

    // Agetime is in microseconds
    dpyP->agetime = 510;
    agedisplay(pdp1P, screenNo);
    // reset age interval for every point shown
    dpyP->agetime = 50 * 1000;  // 50 msecs
    delayTime = (pdp1P->simtime - dpyP->last) / 1000;
    dpyP->last = pdp1P->simtime;

    // The real hardware used intensity 4 for a brightness that was only
    // visible to the lightpen.
    // Simulate that by just not drawing a point.
    if( (screenNo == 0) && (intensity == 4) )
    {
        return;
    }

    if( x & 01000 )
    {
        x++;
    }

    if( y & 01000 )
    {
        y++;
    }

    x = (x + 01000) & 01777;
    y = (y + 01000) & 01777;
    cmd = x | (y << 10) | (delayTime << 23);

    // The real hardware used intensity 4 for a brightness that was only
    // visible to the lightpen.
    // Simulate that by just not drawing a point.
    if( intensity != 4 )
    {
        cmd |= ((intensity + 4) & 7) << 20;
        dpycmd(pdp1P, screenNo, cmd);
        logger(LOG_UPDATE, "dpycmd issued, %o\n", cmd);
    }
    else
    {
        logger(LOG_UPDATE, "intensity 4, no dpycmd issued\n");
    }
}

// Convert a 10 bit dpy coordinate to a 2's complement signed int
int
cvtDpyToSigned(int dpy)
{
    // For coordinates, the 1's complement -0 value, 01777, is legitimate, map negative numbers
    // to the range -1 to -512.
    if(dpy & 01000)                 // high bit set means neg 1's cmpl number
    {
        return(-(~dpy & 0777) - 1);
    }
    else
    {
        return(dpy);
    }
}

// The client sends x,y coordinates whenever the mouse is moved with mouse button 1 down.
// When the button is releasde, a 'pen lifted' notice is sent.
// The physical light pen aperture is simulated by having any match within the dot location
// plus the aperture setting.
//
// The algorithm is:
// If a pen-lifted event is seen, set penDwon to false, no hit cheking will be done.
// If a coordnate event is see, set penDown to true and save the most recent x, y update.
// Whenever a dpy completion occurs, check the status and if the pen is down, see if the
// dpy coordinates match the current position within the aperture boundaries and if so,
// set the appropriate flags.
//
// A command from the client is a 32 bit word:
// FFpccccc where:
// p = 0x1 if the pen is up, 0x0 if down
// c is the packed x, y 10 bit 1's complement coordinates (x << 10) | y

#define CMDBITS 0xFF000000
#define LPCMD   0xFF000000
#define PENBITS 0x00F00000
#define LPUP    0x00100000

// This is the update reader for the lightpen.
// See if there is data from the client, update lp status.
// Return true if any read, else false.
bool
lightpenReader(PDP1P pdp1P, int *xP, int *yP, bool *penDownP)
{
int i;
int count;
int sockFlag = 1;
uint32_t cmdBuf[PENBUFSIZE];
uint32_t cmd;
bool gotPosition;
DispCon *dpyP;

static int lastX, lastY;
static bool penDown;

    dpyP = &(pdp1P->dpy[0]);

    if( dpyP->fd < 0 )
    {
        lastX = lastY = 0;
        return(false);                          // nothing open yet
    }

    gotPosition = false;

    // Read all pending commands.
    // Only the mouse move last will be significant, but we do need to check pen up / pen down for all.
    while( (count = read(dpyP->fd, cmdBuf, sizeof(cmdBuf))) > 0 )
    {
        count /= sizeof(uint32_t);              // convert to index
        for( i = 0; i < count; ++i )
        {
            cmd = cmdBuf[i];
            if( (cmd & CMDBITS) == LPCMD )  // light pen, just to be sure
            {
                if( (cmd & PENBITS) == LPUP )   // pen up, done
                {
                    penDown = false;
                    gotPosition = false;
                    logger(LOG_LP, "Pen up\n");
                }
                else
                {
                    penDown = true;
                    // We convert to a signed 2's complement integer
                    lastX = cvtDpyToSigned((cmd >> 10) & 0x3FF);
                    lastY = cvtDpyToSigned(cmd & 0x3FF);
                    gotPosition = true;
                    logger(LOG_LP, "LP received x %d, y %d\n", lastX, lastY);
                }
            }
        }
    }

    *xP = lastX;
    *yP = lastY;
    *penDownP = penDown;

    // Turn on fast ack to minimize delays.
    // This might or might not improve lightpen performance.
    setsockopt(dpyP->fd, IPPROTO_TCP, TCP_QUICKACK, &sockFlag, sizeof(sockFlag));
    return( gotPosition );
}

// See if there is a light pen hit in the given radius.
// The square of radius is passed.
// If so, return true, else false.
bool
checkLightpen(PDP1 *pdp1P, int x, int y)
{
int i, sawOne, dpyx, dpyy;
int delx, dely;
int lpX, lpY;
bool penDown;
DispCon *dpyP;

    lightpenReader(pdp1P, &lpX, &lpY, &penDown);          // check for any pending input

    if( !penDown )
    {
        return(false);
    }

    dpyP = &(pdp1P->dpy[0]);
    dpyx = cvtDpyToSigned(x);
    dpyy = cvtDpyToSigned(y);

    // Both coordinate pairs have been converted from 10 bit 1's complement to full signed 2's complement.
    // We have to take edge wrapping into account, do nothing if it wrapped.
    // Just compare bits outside the range, neg will have the bit set, pos won't
    if( !((dpyx ^ lpX) & 0x200) && !((dpyy ^ lpY) & 0x200) )
    {
        // Use the distance equation for a circle to simulate an actual circular aperture
        delx = lpX - dpyx;               // Find squared magnitudes of hit offset
        dely = lpY - dpyy;
        if( ((delx*delx) + (dely*dely)) < dpyP->lpRadius2 )
        {
            pdp1P->cksflags |= 0400000;               // cleared by next dpy
            pdp1P->pf |= decflg(3);
            return(true);
        }
    }

    return(false);
}
