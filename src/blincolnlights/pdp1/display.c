// All the global code for managing the Type 30 display is here, moved frm pdp1.c.
// It can't be encapsulated in IOT 7 because other code needs it, such as the symbol generator.
//
// The original code was not really correct, a real pdp-1 did not manage the display in its own logic,
// such as display fadeout. That was an intrinsic property of the display itself and had nothing to do with
// the pdp-1. Of course, the original displays with P7 phosphors aren't what we use now, but the simulation
// of it should not have been in the main emulation code. In fact, it should be in the display programs
// themselves, but that's a problem for another time.
// The compromise is here, done by moving everything related to aging, etc. into a worker thread that is
// independent of the emulator itself.
//
// 12-Apr-2026 wje initial version

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include "common.h"
#include "pdp1.h"

//#define DOLOGGING
#include "logger.h"
#define LOG_INIT 0
#define LOG_FD 0
#define LOG_CMD 0
#define LOG_DPYCMD 0
#define LOG_DPYWRITE 0
#define LOG_LP 0
#define LOG_BOUNDS 0
#define LOG_AGEDELAY 0

#define WORKERSLEEPTIME 10          // how often worker thread runs, microseconds
#define PENBUFSIZE  64              // read up to this many lightpen update commands at once
#define CMDBUFSIZE  64              // buffer this many display commands
#define DPYBUFSIZE  256             // buffer this many outgoing dpy commands, can be many more than CMDBUFSIZE
#define NEVER ~((uint64_t)0)        // a long time from now

typedef struct {
    int fd;             // file descriptor to use
    int curX, curY;     // last x,y coordinates set, 0-1023
    int intensity;      // last intensity set
    uint64_t now;       // really the time the current worker thead cycle started
    uint64_t lastTime;  // used by addDpyCommand();
    uint64_t ageTime;   // used by addDpyCommand() and ageDisplay();
    bool penDown;       // lightpen is on the screen
    int lpX, lpY;       // last x,y lightpen coordinates received
    int lpRadius2;      // used by the lightpen check
    bool terminate;     // if set to true, worker will terminate
    pthread_mutex_t dpyMutex;    // for interlocking with the worker thread
    pthread_mutex_t ctlMutex;    // for locking get/setDisplayData

    int numCommands;
    uint32_t commandBuf[CMDBUFSIZE];

    int numDpyCommands;
    uint32_t dpyBuf[CMDBUFSIZE];
} DisplayControl, *DisplayControlP;

// Support 2 displays
static DisplayControl display0Control;
static DisplayControl display1Control;
static DisplayControlP control0P;
static DisplayControlP control1P;

static bool displayInitialized;

// Handle iniital setup of the worker thread, mutex, etc.
void initializeDisplaySubsystem(void);

// External calls to manage various things.
bool setDisplayFD(int screen, int fd);
int getDisplayFD(int screen);
bool getDisplayData(int screen, int *xP, int *yP, int *intensityP);
bool setDisplayData(int screen, int x, int y, int intensity);
bool lockDisplayData(int screen);
bool unlockDisplayData(int screen);

// Called to set the lightpen radius squared used for hit detection.
void setLightpenRadius2(int screen, int radius2);
// and to get it
int getLightpenRadius2(int screen);

// The outside interface.
void display(int screenNo, int x, int y, int intensity);

// The outside inteface for checking the lightpen.
// It will return true if there was a lightpen hit at the given coordinates, else false.
bool checkLightpen(PDP1 *pdp1P, int screenNo,  int x, int y);

static int cvtDpyTo1024(int dpy);
static bool lightpenReader(DisplayControlP ctlP);
static uint64_t currentTime(void);
static void initializeDisplayControl(DisplayControlP ctlP);
static void *worker(void *);
static void lockDisplay(DisplayControlP ctlP);
static void unlockDisplay(DisplayControlP ctlP);
static void flushDisplay(DisplayControlP ctlP);
static void addCommand(DisplayControlP ctlP, int x, int y, int intensity);
static void putDpyCommand(DisplayControlP ctlP, unsigned int x, unsigned int y, unsigned int intensity);
static void addDpyCommand(DisplayControlP ctlP, uint32_t cmd);

extern int decflg(int flg);

void
initializeDisplaySubsystem()
{
pthread_t thread;

    if( displayInitialized )
    {
        return;             // already done
    }

    displayInitialized = true;
    logger(LOG_INIT,"Display subsystem being initialized\n");

    control0P = &display0Control;
    initializeDisplayControl(control0P);
    control1P = &display1Control;
    initializeDisplayControl(control1P);

    if( pthread_create(&thread, NULL, worker, control0P) )
    {
        logger(LOG_INIT,"Display worker thread create failed.\n");
        displayInitialized = false;
        return;
    }

    logger(LOG_INIT,"Initialization done.\n");
}

void
initializeDisplayControl(DisplayControlP ctlP)
{
pthread_mutexattr_t attr;

    ctlP->fd = -1;
    ctlP->numCommands = 0;
    ctlP->numDpyCommands = 0;
    ctlP->now = currentTime();
    ctlP->lastTime = ctlP->now;
    ctlP->ageTime = 50 * 1000;  // 50 microseconds
    ctlP->terminate = false;
    ctlP->curX = ctlP->curY = ctlP->intensity = 0;
    ctlP->lpX = ctlP->lpY = 0;
    ctlP->penDown = false;
    ctlP->lpRadius2 = 6*6;      // pretty small, but is overridden later
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&(ctlP->dpyMutex), &attr);
    pthread_mutex_init(&(ctlP->ctlMutex), &attr);
}

// Set the fd to use, true if screen is valid, false if out of range.
// An fd of -1 just closes the current fd if any.
bool
setDisplayFD(int screen, int fd)
{
DisplayControlP ctlP;

    if( (screen < 0) || (screen > 1) )
    {
        return(false);
    }

    ctlP = (screen == 0)?control0P:control1P;

    lockDisplay(ctlP);
    if( ctlP->fd >= 0 )
    {
        close(ctlP->fd);
    }

    ctlP->fd = fd;
    ctlP->now = currentTime();
    ctlP->ageTime = 50 *1000;   // initial age time
    ctlP->lastTime = ctlP->now;
    unlockDisplay(ctlP);

    logger(LOG_FD,"screen %d fd now %d\n", screen, fd);
    return(true);
}

// Get the current fd, -1 if none or out of range.
int
getDisplayFD(int screen)
{
    if( screen == 0 )
    {
        return(control0P->fd);
    }
    else if( screen == 1 )
    {
        return(control1P->fd);
    }
    else
    {
        return(-1);
    }
}

bool
lockDisplayData(int screen)
{
DisplayControlP ctlP;

    if( (screen < 0) || (screen > 1) )
    {
        return(false);
    }

    ctlP = (screen == 0)?control0P:control1P;
    pthread_mutex_lock(&(ctlP->ctlMutex));
    return(true);
}

bool
unlockDisplayData(int screen)
{
DisplayControlP ctlP;

    if( (screen < 0) || (screen > 1) )
    {
        return(false);
    }

    ctlP = (screen == 0)?control0P:control1P;
    pthread_mutex_unlock(&(ctlP->ctlMutex));
}

void
setLightpenRadius2(int screenNo, int radius2)
{
    // All screens share the same radius setting?
    lockDisplayData(control0P);
    control0P->lpRadius2 = radius2;
    unlockDisplayData(control0P);
    lockDisplayData(control1P);
    control1P->lpRadius2 = radius2;
    unlockDisplayData(control1P);
}

int
getLightpenRadius2(int screenNo)
{
    // Only supported for screen 0
    if( screenNo == 0 )
    {
        return(control0P->lpRadius2);
    }
    else
    {
        return(0);
    }
}

// Get the last coordinates and intensity that were set by setDisplayData.
// If a pointer is 0, it is not set.
// No mapping of coordinates is done, the meaning is up to the caller.
// If no data is available, return false, else true.
bool
getDisplayData(int screen, int *xP, int *yP, int *intensityP)
{
DisplayControlP ctlP;

    if( (screen < 0) || (screen > 1) )
    {
        return(false);
    }

    ctlP = (screen > 0)?control1P:control0P;

    if( ctlP->fd < 0 )
    {
        return(false);      // display not open
    }

    if( xP )
    {
        *xP = ctlP->curX;
    }

    if( yP )
    {
        *yP = ctlP->curY;
    }

    if( intensityP )
    {
        *intensityP = ctlP->intensity;
    }

    return(true);
}

// Set the coordinates and intensity that will be returned by getDisplayData();
// If a value is -1, it is not set.
// Coordinates will be constrained to 10 bits.
// If the screen is invalid, return false else true.
bool
setDisplayData(int screen,  int x, int y, int intensity)
{
DisplayControlP ctlP;

    if( (screen < 0) || (screen > 1) )
    {
        return(false);
    }

    ctlP = (screen > 0)?control1P:control0P;

    if( ctlP->fd < 0 )
    {
        return(false);      // display not open
    }

    if( x >= 0 )
    {
        ctlP->curX = x & 01777;
    }

    if( y >= 0 )
    {
        ctlP->curY = y & 01777;
    }

    if( intensity >= 0 )
    {
        ctlP->intensity = intensity;
    }

    return(true);
}

void
display(int screenNo, int x, int y, int intensity)
{
DisplayControlP ctlP;

    if( (screenNo < 0) || (screenNo > 1) )
    {
        // Invalid screen
        return;
    }

    ctlP = (screenNo)?control1P:control0P;
    if( !displayInitialized || (ctlP->fd < 0) )
    {
        return;         // display isn't open
    }

    logger(LOG_CMD,"display(), screen %d x %d y %d intensity %d\n", screenNo, x, y, intensity);

    addCommand(ctlP, x & 01777, y & 01777, intensity & 07);
}

void
lockDisplay(DisplayControlP ctlP)
{
    pthread_mutex_lock(&(ctlP->dpyMutex));
}

void
unlockDisplay(DisplayControlP ctlP)
{
    pthread_mutex_unlock(&(ctlP->dpyMutex));
}

// This is where all the work is done.
// Commands are u32_t types with the form:
// 0xIXXXYYY
// where X and Y are 0-1023 and I is 0-7.
void *
worker(void *argP)
{
int i;
int screen;
int x, y;
int intensity;
uint32_t cmd;

    // screen 0 is the primary

    while( !control0P->terminate )
    {
        control0P->now = currentTime();
        // Check for work
        lockDisplay(control0P);
        for( i = 0; i < control0P->numCommands; ++i )
        {
            cmd = control0P->commandBuf[i];
            intensity = (cmd >> 24) & 07;
            x = (cmd >> 12) & 01777;
            y = cmd & 01777;
            logger(LOG_CMD,"Worker got screen 0 x %d, y %d, intensity %d\n", x, y, intensity);
            putDpyCommand(control0P, x, y, intensity);
        }

        control0P->numCommands = 0;
        unlockDisplay(control0P);

        if( control0P->numDpyCommands > DPYBUFSIZE )
        {
            logger(LOG_BOUNDS,"worker numDpyCommands too big: %d\n", control0P->numDpyCommands);
        }

        ageDisplay(control0P);
        flushDisplay(control0P);

        if( control1P->fd >= 0 )
        {
            lockDisplay(control1P);

            for( i = 0; i < control1P->numCommands; ++i )
            {
                cmd = control1P->commandBuf[i];
                intensity = (cmd >> 24) & 07;
                x = (cmd >> 12) & 01777;
                y = cmd & 01777;
                logger(LOG_CMD,"Worker got screen 1 x %d, y %d, intensity %d\n", x, y, intensity);
                putDpyCommand(control1P, x, y, intensity);
            }

            control1P->numCommands = 0;
            unlockDisplay(control1P);

            ageDisplay(control1P);
            flushDisplay(control1P);
        }

        // we read lp data even if not enabled, display could be sending it
        lightpenReader(control0P);          // check for any pending input
        lightpenReader(control1P);
        usleep(WORKERSLEEPTIME);
    }

    return(0);
}

// Puts a command into the worker command buffer.
// X and y will be the usual Type 30 -511,511 10 bit coordinates.
void
addCommand(DisplayControlP ctlP, int x, int y, int intensity)
{
int cmd;

    while( ctlP->numCommands >= CMDBUFSIZE )
    {
        usleep(5);          // wait for the buffer to drain
    }

    cmd = (intensity << 24) | (x << 12) | y;
    lockDisplay(ctlP);
    ctlP->commandBuf[ctlP->numCommands++] = cmd;
    unlockDisplay(ctlP);
}

// Puts a dpy-style command into the dpy command buffer
void
addDpyCommand(DisplayControlP ctlP, uint32_t cmd)
{
    if( ctlP->numDpyCommands > DPYBUFSIZE )
    {
        logger(LOG_BOUNDS,"addDpyCommand numDpy too big: %d\n", ctlP->numDpyCommands);
    }

    if( ctlP->numDpyCommands >= DPYBUFSIZE )
    {
        flushDisplay(ctlP);
    }

    ctlP->dpyBuf[ctlP->numDpyCommands++] = cmd;
}

void
flushDisplay(DisplayControlP ctlP)
{
int size;
int resp;

    if( ctlP->fd < 0 )
    {
        return;     // nothing to do
    }

    if( ctlP->numDpyCommands > DPYBUFSIZE )
    {
        logger(LOG_BOUNDS,"flushDisplay numDpyCommands too big: %d\n", ctlP->numDpyCommands);
        ctlP->numDpyCommands = 0;
        return;
    }

    size = ctlP->numDpyCommands * sizeof(ctlP->dpyBuf[0]);
    if( size <= 0 )
    {
        return;     // nothing to write
    }

    resp = write(ctlP->fd, ctlP->dpyBuf, size);
    logger(LOG_DPYWRITE,"flushdisplay wrote %d bytes\n", resp);
    ctlP->numDpyCommands = 0;

    // Write failed, stop writing
    if( resp < size )
    {
        logger(LOG_FD,"fd %d closed\n", ctlP->fd);
        close(ctlP->fd);
        ctlP->fd = -1;
    }
}

void
ageDisplay(DisplayControlP ctlP)
{
uint64_t delayTime;

    if( ctlP->fd < 0 )
    {
        // nothing to do
        return;
    }

    delayTime = (ctlP->now -  ctlP->lastTime) / 1000;

    // Agetime is in microseconds
    if( delayTime >= ctlP->ageTime )
    {
        logger(LOG_AGEDELAY, "deltay time %d, age time %d\n", delayTime, ctlP->ageTime);
        addDpyCommand(ctlP, 511 << 23);
        addDpyCommand(ctlP, delayTime);
        flushDisplay(ctlP);

        lockDisplay(ctlP);
        ctlP->lastTime = ctlP->now;

        // increase interval during fade out
        // to reduce number of age commands
        if( ctlP->ageTime < (1000 * 1000))
        {
             ctlP->ageTime +=  ctlP->ageTime / 6;
        }
        unlockDisplay(ctlP);
    }
}

// Format and add a dpy format command, will be sent to the display.
void
putDpyCommand(DisplayControlP ctlP, unsigned int x, unsigned int y, unsigned int intensity)
{
int delayTime;
int cmd;

    if( ctlP->fd < 0 )
    {
        return;     // not open, don't bother
    }

    // Agetime is in microseconds
    ctlP->ageTime = 510;
    ageDisplay(ctlP);
    // reset age interval for regular aging
    ctlP->ageTime = 50 * 1000;  // 50 msecs
    delayTime = (ctlP->now - ctlP->lastTime) / 1000;
    ctlP->lastTime = ctlP->now;

    if( (x > 01777) || (y > 01777) || (intensity > 7) )
    {
        logger(LOG_BOUNDS, "Boundary violation x %d y%d intensity %d\n", x, y, intensity);
    }

    // The real hardware used intensity 4 for a brightness that was only
    // visible to the lightpen.
    // Simulate that by just not drawing a point.
    if( intensity == 4 )
    {
        return;
    }

    // Put coords in the format the target display device expects, 0-1023
    x = cvtDpyTo1024(x);
    y = cvtDpyTo1024(y);

    cmd = x | (y << 10) | (delayTime << 23);
    cmd |= ((intensity + 4) & 7) << 20;
    logger(LOG_DPYCMD,"adding dpy command 0x%08x, x %d y %d\n", cmd, x, y);
    addDpyCommand(ctlP, cmd);
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
lightpenReader(DisplayControlP ctlP)
{
int i;
int count;
int sockFlag = 1;
uint32_t penBuf[PENBUFSIZE];
uint32_t cmd;
bool gotPosition;

static int lastX, lastY;
static bool penDown;

    if( ctlP->fd < 0 )
    {
        ctlP->lpX = ctlP->lpY = 0;
        ctlP->penDown = false;
        return(false);                          // nothing open yet
    }

    gotPosition = false;

    // Read all pending commands.
    // Only the mouse move last will be significant, but we do need to check pen up / pen down for all.
    while( (count = read(ctlP->fd, penBuf, sizeof(penBuf))) > 0 )
    {
        count /= sizeof(uint32_t);              // convert to index
        for( i = 0; i < count; ++i )
        {
            cmd = penBuf[i];                    // this is shared between displays, ok because access is serialized
            if( (cmd & CMDBITS) == LPCMD )      // light pen, just to be sure
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
                    gotPosition = true;
                    // lightpen coordinates come in as dpy coordinates, -511,511 10 bit 1's complement
                    lastX = (cmd >> 10) & 0x3FF;
                    lastY = cmd & 0x3FF;
                    logger(LOG_LP, "LP received x %d, y %d\n", lastX, lastY);
                }
            }
        }
    }

    lockDisplayData(ctlP);
    if( gotPosition )
    {
        ctlP->lpX = lastX;
        ctlP-> lpY = lastY;
    }

    ctlP->penDown = penDown;
    unlockDisplayData(ctlP);

    // Turn on fast ack to minimize delays.
    // This might or might not improve lightpen performance.
    setsockopt(ctlP->fd, IPPROTO_TCP, TCP_QUICKACK, &sockFlag, sizeof(sockFlag));
    return( gotPosition );
}

// See if there is a light pen hit in the given radius.
// The square of radius is passed.
// If so, return true, else false.
bool
checkLightpen(PDP1P pdp1P, int screenNo, int x, int y)
{
DisplayControlP ctlP;

int  dpyx, dpyy;
int  lpx, lpy;
int delx, dely;

    if( (screenNo < 0) || (screenNo > 1) )
    {
        return(false);
    }

    ctlP = (screenNo == 0)?control0P:control1P;
    if( !ctlP->penDown )
    {
        return(false);
    }

    // Just for sanity, convert 10 0-1023
    dpyx = cvtDpyTo1024(x);
    dpyy = cvtDpyTo1024(y);
    lpx = cvtDpyTo1024(ctlP->lpX);
    lpy = cvtDpyTo1024(ctlP->lpY);

    // Use the distance equation for a circle to simulate an actual circular aperture
    delx = lpx - dpyx;               // Find squared magnitudes of hit offset
    dely = lpy - dpyy;
    if( ((delx*delx) + (dely*dely)) < ctlP->lpRadius2 )
    {
        pdp1P->cksflags |= 0400000;               // cleared by next dpy
        pdp1P->pf |= decflg(3);
        return(true);
    }

    return(false);
}

// Convert a 10 bit 1's complement dpy coordinate to a 2's complement 0-1023 value.
int
cvtDpyTo1024(int dpy)
{
    if( dpy & 01000 )
    {
        dpy++;
    }

    return( (dpy + 01000) & 01777);
}

// Get current system time in ns.
uint64_t
currentTime()
{
struct timespec tm;
uint64_t time;

    clock_gettime(CLOCK_MONOTONIC, &tm);
    time = tm.tv_nsec;
    time += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    return( time );
}
