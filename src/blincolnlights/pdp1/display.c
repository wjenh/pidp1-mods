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
// 23-Apr-2026 wje switch to semaphores, better for this than mutexes
// 1-May-2026 wje tune buffer sizes for better Type 340 performance
// 2-May-2026 wje back to mutexes, more buffer tweaking
// 4-May-2026 wje add a timed run/wait semaphore and actually buffer commands
// 4-May-2026 wje and change clock to REALTIME for sem_timedwait()
// 7-May-2026 wje reduce buffer sizes, 256 seems to be fine
// 8-May-2026 wje minor cleanup in lightpen detection code
// 13-Jun-2026 wje (Claude) handle EAGAIN/EWOULDBLOCK in flushDisplay(),
//    Now EAGAIN/EWOULDBLOCK/EINTR leave the buffered commands in place for a later retry,
//    partial writes shift the remainder to the front of the buffer,
//    and addDpyCommand() retries (bounded by DPYFULLRETRIES)
//    instead of risking a dpyBuf[] overflow if the buffer stays full after a flush.
// 26-Jun-2026 wje (Claude) deep analysis of possible bottlenecks done, tuning changes made
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <semaphore.h>

#include "common.h"
#include "pdp1.h"

// No particular limit, but the type 340 with option 343 monitors can use up to 17, the impl restricts it to 9
#define MAXDISPLAYS 9

//#define DOLOGGING
#include "logger.h"
#define LOG_INIT 0
#define LOG_FD 0
#define LOG_CMD 0
#define LOG_WAIT 0
#define LOG_DISPLAY 0
#define LOG_DPYCMD 0
#define LOG_DPYWRITE 0
#define LOG_LP 0
#define LOG_BOUNDS 0
#define LOG_AGEDELAY 0
#define LOG_FULL 0

#define WORKERSLEEPTIME 1000       // max time the worker thread will sleep, usecs
#define PENBUFSIZE  64             // read up to this many lightpen update commands at once
#define CMDBUFSIZE  4096           // buffer up to this many display commands (sized to the 4096-word display memory)
#define DPYBUFSIZE  4096           // buffer up to this many outgoing dpy commands
#define DPYFULLRETRIES 100         // addDpyCommand: max retries while dpy buf is full before dropping a command
#undef NEVER
#define NEVER ~((uint64_t)0)       // a long time from now
#define null 0
#define isOpen(ctlP) ((ctlP)->fd >= 0)

static int curRadius2 = 6*6;       // setLightpenRadius2() will override this
static bool displayInitialized;
static sem_t runLock;
static uint64_t dpyDropCount;      // count of dpy commands dropped because a client could not keep up

typedef struct {
    int fd;                        // file descriptor to use
    int curX, curY;                // last x,y coordinates set, 0-1023
    int intensity;                 // last intensity set
    uint64_t now;                  // really the time the current worker thead cycle started
    uint64_t lastTime;             // used by addDpyCommand();
    uint64_t ageTime;              // used by addDpyCommand() and ageDisplay();
    bool penDown;                  // lightpen is on the screen
    bool lpData;                   // set when lp data comes in, cleared when read
    int lpX, lpY;                  // last x,y lightpen coordinates received
    int lpRadius2;                 // used by the lightpen check
    pthread_mutex_t controlLock;   // for interlocking with the worker thread
    pthread_mutex_t dataLock;      // for locking get/setDisplayData

    int numCommands;
    uint32_t commandBuf[CMDBUFSIZE];
    int numDpyCommands;
    uint32_t dpyBuf[DPYBUFSIZE];
} DisplayControl, *DisplayControlP;

static DisplayControlP displays[MAXDISPLAYS];

// External calls to manage various things.
int getDisplayFD(int screenNo);
bool setDisplayFD(int screenNo, int fd);
bool getDisplayData(int screenNo, int *xP, int *yP, int *intensityP);
bool setDisplayData(int screenNo, int x, int y, int intensity);
bool lockDisplayData(int screenNo);
bool unlockDisplayData(int screenNo);

// Called to set and get the lightpen radius squared used for hit detection.
void setLightpenRadius2(int screenNo, int radius2);
int getLightpenRadius2(int screenNo);

// The outside draWing interfaces.
bool display(int screenNo, int x, int y, int intensity);

// The outside inteface for checking the lightpen.
// It will return true if there was a lightpen hit at the given coordinates, else false.
bool checkLightpen(int screenNo,  int x, int y);

// Internal functions.
static void initializeDisplaySubsystem(void);
static bool isDisplayConfigured(int screenNo);

static DisplayControlP initializeDisplay(int screenNo);
static DisplayControlP getDisplayControlP(int screenNo);
static bool lightpenReader(DisplayControlP ctlP);
static int cvtDpyTo1024(int dpy);
static uint64_t currentTime(void);
static void initializeDisplayControl(DisplayControlP ctlP);
static bool lockDisplayDataByCtlP(DisplayControlP ctlP);
static bool unlockDisplayDataByCtlP(DisplayControlP ctlP);

// This is the processing thread
static void *worker(void *);
static void lockControl(DisplayControlP ctlP);
static void unlockControl(DisplayControlP ctlP);
static void flushDisplay(DisplayControlP ctlP);
static void addCommand(DisplayControlP ctlP, int x, int y, int intensity, bool defer);
static void putDpyCommand(DisplayControlP ctlP, unsigned int x, unsigned int y, unsigned int intensity);
static void addDpyCommand(DisplayControlP ctlP, uint32_t cmd);
static void ageDisplay(DisplayControlP ctlP);
static void waitForRun(void);

extern int decflg(int flg);

// Set the fd to use, true if screen is valid, false if out of range.
// Automatically allocate the display if it isn't already.
// An fd of -1 just closes the current fd if any.
bool
setDisplayFD(int screenNo, int fd)
{
int fdFlags;
int nodelay;
DisplayControlP ctlP;

    if( !(ctlP = initializeDisplay(screenNo)) )
    {
        return(false);
    }

    lockControl(ctlP);
    if( ctlP->fd >= 0 )
    {
        close(ctlP->fd);
    }

    // Lightpen needs nonblocking
    fdFlags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fdFlags | O_NONBLOCK));

    // Disable Nagle's algorithm. The display stream is latency-sensitive and is
    // frequently flushed in small batches; TCP_QUICKACK is also set on the read side in lightpenReader().
    nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ctlP->fd = fd;
    ctlP->now = currentTime();
    ctlP->ageTime = 50 *1000;   // initial age time
    ctlP->lastTime = ctlP->now;
    unlockControl(ctlP);

    logger(LOG_FD,"screen %d fd now %d\n", screenNo, fd);
    return(true);
}

// Get the current fd, -1 if none or out of range.
int
getDisplayFD(int screenNo)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return( -1 );
    }

    return(ctlP->fd);
}

bool
lockDisplayData(int screenNo)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return(false);
    }

    return( lockDisplayDataByCtlP(ctlP) );
}

bool
unlockDisplayData(int screenNo)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return(false);
    }

    return( unlockDisplayDataByCtlP(ctlP) );
}

static bool
lockDisplayDataByCtlP(DisplayControlP ctlP)
{
    pthread_mutex_lock(&(ctlP->dataLock));
    return(true);
}

static bool
unlockDisplayDataByCtlP(DisplayControlP ctlP)
{
    pthread_mutex_unlock(&(ctlP->dataLock));
    return(true);
}

// Set the lightpen radius squared for the screen.
// Currently, all screens are updated with the same radius.
// Not sure it's correct.
void
setLightpenRadius2(int screenNo, int radius2)
{
int i;
DisplayControlP ctlP;

    curRadius2 = radius2;       // will be used when a new screen is initialzed

    // All screens share the same radius setting
    for( i = 0; i < MAXDISPLAYS; ++i )
    {
        if( (ctlP = getDisplayControlP(i)) && isOpen(ctlP) )
        {
            ctlP->lpRadius2 = radius2;
        }
    }
}

// Get the current lightpen radius squared setting for the screen.
// Return 0 if none.
int
getLightpenRadius2(int screenNo)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return(0);
    }

    return( ctlP->lpRadius2 );
}

// Get the last coordinates and intensity that were set by setDisplayData.
// If a pointer is 0, it is not set.
// No mapping of coordinates is done, the meaning is up to the caller.
// If no data is available, return false, else true.
bool
getDisplayData(int screenNo, int *xP, int *yP, int *intensityP)
{
DisplayControlP ctlP;

    ctlP = getDisplayControlP(screenNo);

    if( !ctlP )
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
// Caller manages locking.
bool
setDisplayData(int screenNo,  int x, int y, int intensity)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return(false);
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

// Display a point.
// The primary external method.
// Returns true if display is open, else false.
// The point is put into the output buffer to be processed when the worker thread wakes up.
// It will also be awakened if the buffer is full.
bool
display(int screenNo, int x, int y, int intensity)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) || !isOpen(ctlP) )
    {
        return(false);
    }

    logger(LOG_DISPLAY,"display(), screen %d x %d y %d intensity %d\n", screenNo, x, y, intensity);
    addCommand(ctlP, x & 01777, y & 01777, intensity & 07, false);
    return(true);
}

// See if there is a light pen hit in the radius that is set for the screen.
// The coordinates to check are passed.
// The coordinates are expected to be in 0-1024 range!
// If so, return true, else false.
bool
checkLightpen(int screenNo, int x, int y)
{
int delx, dely;
bool gotHit;
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) || !isOpen(ctlP) )
    {
        return(false);
    }

    lockDisplayDataByCtlP(ctlP);
    if( !ctlP->penDown || !ctlP->lpData )
    {
        unlockDisplayDataByCtlP(ctlP);
        return(false);
    }

    logger(LOG_LP, "LP checking x %d against lp x %d, y %d against lp y %d, r2 %d\n",
            x, ctlP->lpX, y, ctlP->lpY, ctlP->lpRadius2);

    gotHit = false;
    // Use the distance equation for a circle to simulate an actual circular aperture
    delx = ctlP->lpX - x;               // Find squared magnitudes of hit offset
    dely = ctlP->lpY - y;
    if( ((delx*delx) + (dely*dely)) < ctlP->lpRadius2 )
    {
        logger(LOG_LP, "LP hit\n");
        gotHit = true;
        ctlP->lpData = false;
    }

    unlockDisplayDataByCtlP(ctlP);
    return(gotHit);
}

// End of external functions, these are all internal.

// Called initially to set up everything.
static void
initializeDisplaySubsystem()
{
pthread_t thread;
pthread_attr_t attr;
struct sched_param schedParam;
bool haveAttr;
int created;

    if( displayInitialized )
    {
        return;             // already done
    }

    displayInitialized = true;
    logger(LOG_INIT,"Display subsystem being initialized\n");

    sem_init(&runLock, 0, 0);

    // The worker thread is the only thread that moves bytes onto the display
    // socket, so the smoothest output comes from it being the least likely of the
    // emulator's threads to be starved. The real-time-priority path below is
    // OPTIONAL and self-disabling: it tries to create the worker at a low SCHED_RR
    // priority, but that requires process privilege (CAP_SYS_NICE / RLIMIT_RTPRIO).
    // When that privilege is absent, pthread_create() fails with EPERM and we transparently retry with
    // default (SCHED_OTHER) scheduling, so a worker is always created.
    haveAttr = false;
    if( pthread_attr_init(&attr) == 0 )
    {
        if( (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0) &&
            (pthread_attr_setschedpolicy(&attr, SCHED_RR) == 0) )
        {
            schedParam.sched_priority = (sched_get_priority_min(SCHED_RR) + 1);
            if( pthread_attr_setschedparam(&attr, &schedParam) == 0 )
            {
                haveAttr = true;
            }
        }
    }

    created = pthread_create(&thread, (haveAttr)?(&attr):(NULL), worker, null);
    if( created && haveAttr )
    {
        // Most likely EPERM: no privilege for real-time scheduling. Fall back to a
        // default-scheduled worker rather than running without one at all.
        logger(LOG_INIT,"Display worker RT create failed (err %d), retrying with default scheduling\n", created);
        created = pthread_create(&thread, NULL, worker, null);
    }

    if( haveAttr )
    {
        pthread_attr_destroy(&attr);
    }

    if( created )
    {
        logger(LOG_INIT,"Display worker thread create failed.\n");
        displayInitialized = false;
        return;
    }

    logger(LOG_INIT,"Initialization done.\n");
}

// Create a control entry for the screen number passed.
// If it is already created, do nothing.
// Also initialize the display subsystem if needed.
// Returns the control pointer or null if the screen number is invalid.
static DisplayControlP
initializeDisplay(int screenNo)
{
    // displays[] has MAXDISPLAYS entries (valid indices 0..MAXDISPLAYS-1), so the
    // upper bound must be >= MAXDISPLAYS, not > MAXDISPLAYS (which let index
    // MAXDISPLAYS through and read/write one past the array).
    if( (screenNo < 0) || (screenNo >= MAXDISPLAYS) )
    {
        return(false);
    }

    initializeDisplaySubsystem();

    if( !displays[screenNo] )
    {
        displays[screenNo] = calloc(1, sizeof(DisplayControl));
        initializeDisplayControl(displays[screenNo]);
    }

    return( displays[screenNo] );
}

// If a display has been opened at least once, return true else false.
// This means the display control structure has been allcated, not that the fd is necessarily open.
static bool
isDisplayConfigured(int screenNo)
{
    return( getDisplayControlP(screenNo) != null );
}

// Return the control ptr for the screen or null if not configured.
// Also initializes the display subsystem if needed.
static DisplayControlP
getDisplayControlP(int screenNo)
{
    // See initializeDisplay(): index must be < MAXDISPLAYS to stay in bounds.
    if( (screenNo < 0) || (screenNo >= MAXDISPLAYS) )
    {
        return(0);
    }

    initializeDisplaySubsystem();
    return( displays[screenNo] );
}

// Initialize a display, setting all of its fields to the default values.
static void
initializeDisplayControl(DisplayControlP ctlP)
{
    ctlP->fd = -1;
    ctlP->numCommands = 0;
    ctlP->numDpyCommands = 0;
    ctlP->now = currentTime();
    ctlP->lastTime = ctlP->now;
    ctlP->ageTime = 50 * 1000;  // 50 milliseconds
    ctlP->curX = ctlP->curY = ctlP->intensity = 0;
    ctlP->lpX = ctlP->lpY = 0;
    ctlP->penDown = false;
    ctlP->lpData = false;
    ctlP->lpRadius2 = curRadius2;      // latest setting from config
    pthread_mutex_init(&(ctlP->controlLock), 0);
    pthread_mutex_init(&(ctlP->dataLock), 0);
}

static void
lockControl(DisplayControlP ctlP)
{
    pthread_mutex_lock(&(ctlP->controlLock));
}

static void
unlockControl(DisplayControlP ctlP)
{
    pthread_mutex_unlock(&(ctlP->controlLock));
}

// This is where all the work is done.
// Commands are u32_t types with the form:
// 0xIXXXYYY
// where X and Y are 0-1023 and I is 0-7.
static void *
worker(void *argP)
{
int i, j;
int x, y;
int intensity;
DisplayControlP ctlP;
uint32_t cmd;

    while( true )
    {
        waitForRun();

        for( i = 0; i < MAXDISPLAYS; ++i )
        {
            if( !(ctlP = getDisplayControlP(i)) || !isOpen(ctlP) )
            {
                continue;       // not open
            }

            // We have to lock so that the out-of-thread side doesn't mess up our cmd count
            lockControl(ctlP);
            ctlP->now = currentTime();

            if( ctlP->numCommands )
            {
                logger(LOG_CMD,"Worker awake for screen %d, %d commands\n", i, ctlP->numCommands);
            }

            // Check for work
            for( j = 0; j < ctlP->numCommands; ++j )
            {
                cmd = ctlP->commandBuf[j];
                intensity = (cmd >> 24) & 07;
                x = (cmd >> 12) & 01777;
                y = cmd & 01777;
                putDpyCommand(ctlP, x, y, intensity);
            }

            ctlP->numCommands = 0;
            unlockControl(ctlP);

            ageDisplay(ctlP);
            flushDisplay(ctlP);

            // we read lp data even if not enabled, display could be sending it
            lightpenReader(ctlP);          // check for any pending input
        }
    }

    return(0);
}

// Puts a command into the worker command buffer.
// X and y will be the usual Type 30 -511,511 10 bit coordinates.
// If defer is true, no worker wakeup is issued unless the buffer is full.
static void
addCommand(DisplayControlP ctlP, int x, int y, int intensity, bool defer)
{
int cmd;
bool wasEmpty;
bool hitThreshold;

    lockControl(ctlP);
    while( ctlP->numCommands >= CMDBUFSIZE )
    {
        // Buffer full: signal the worker and yield the CPU to it rather than
        // sleeping a fixed interval.
        unlockControl(ctlP);
        logger(LOG_FULL,"addCommand cmd buffer full\n");
        sem_post(&runLock);
        sched_yield();
        lockControl(ctlP);
    }

    cmd = ((intensity << 24) | (x << 12) | y);
    wasEmpty = (ctlP->numCommands == 0);
    ctlP->commandBuf[ctlP->numCommands++] = cmd;
    hitThreshold = (ctlP->numCommands >= (CMDBUFSIZE / 4));
    unlockControl(ctlP);

    // Only wake the worker when the buffer goes from empty to
    // non-empty, or when a batch has accumulated.
    if( !defer && (wasEmpty || hitThreshold) )
    {
        sem_post(&runLock);
    }
}

// Puts a dpy-style command into the dpy command buffer.
// If the buffer is full, try to flush it to make room.
// The fd is non-blocking so a flush can fail with EAGAIN
// without freeing any space.
// in that case retry a bounded number oftimes with a short delay,
// giving the client a chance to drain its socket.
// If the buffer is still full after DPYFULLRETRIES attempts, drop this command rather than
// overflowing dpyBuf[] or blocking the worker thread indefinitely.
static void
addDpyCommand(DisplayControlP ctlP, uint32_t cmd)
{
int retries;

    retries = 0;
    while( ctlP->numDpyCommands >= DPYBUFSIZE )
    {
        logger(LOG_FULL,"addDpyCommand dpy cmd buffer full\n");
        flushDisplay(ctlP);

        if( ctlP->fd < 0 )
        {
            return;     // connection was closed due to a real error
        }

        if( ctlP->numDpyCommands < DPYBUFSIZE )
        {
            break;      // flush made room
        }

        if( ++retries >= DPYFULLRETRIES )
        {
            // Client can't keep up even after repeated retries.
            // Drop this command rather than overflow the buffer or
            // stall the worker thread (and other displays) forever.
            // Count drops so a chronically slow/stalled client is diagnosable.
            ++dpyDropCount;
            logger(LOG_FULL,"addDpyCommand giving up, dropping command (total drops %lu)\n",
                (unsigned long)dpyDropCount);
            return;
        }

        // This runs on the worker thread and is waiting for the non-blocking
        // socket's send buffer to drain, so a short sleep is fine here.
        usleep(30);         // delay a bit so display doesn't overrun
    }

    ctlP->dpyBuf[ctlP->numDpyCommands++] = cmd;
}

// Write any buffered dpy commands to the display fd.
// The fd is non-blocking so write() can legitimately return -1/EAGAIN or EWOULDBLOCK when the socket's
// send buffer is full.
// In that case, leave the buffered commands in place and let the caller retry later.
// A partial write is also possible on a non-blocking socket; the unsent
// commands are shifted to the front of the buffer for the next attempt.
// Any other write() error indicates the connection is gone, so the fd is
// closed and the buffer discarded.
static void
flushDisplay(DisplayControlP ctlP)
{
int size;
int resp;
int sentCommands;

    if( ctlP->fd < 0 )
    {
        return;     // nothing to do
    }

    size = ctlP->numDpyCommands * sizeof(ctlP->dpyBuf[0]);
    if( size <= 0 )
    {
        return;     // nothing to write
    }

    resp = write(ctlP->fd, ctlP->dpyBuf, size);

    if( resp < 0 )
    {
        if( (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR) )
        {
            // Socket send buffer full or interrupted - not fatal, retry later.
            logger(LOG_DPYWRITE,"flushDisplay: write would block (errno %d), retrying later\n", errno);
            return;
        }

        logger(LOG_FD,"fd %d closed, write error (errno %d)\n", ctlP->fd, errno);
        close(ctlP->fd);
        ctlP->fd = -1;
        ctlP->numDpyCommands = 0;
        return;
    }

    logger(LOG_DPYWRITE,"flushdisplay wrote %d bytes\n", resp);

    if( resp == size )
    {
        ctlP->numDpyCommands = 0;
        return;
    }

    // Partial write: shift the unsent commands down to the front of the
    // buffer so the next flush attempt picks up where this one left off.
    sentCommands = resp / (int)sizeof(ctlP->dpyBuf[0]);
    memmove(ctlP->dpyBuf, &ctlP->dpyBuf[sentCommands], (size_t)(size - resp));
    ctlP->numDpyCommands -= sentCommands;
}

static void
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
        ctlP->lastTime = ctlP->now;
        flushDisplay(ctlP);

        // increase interval during fade out
        // to reduce number of age commands
        if( ctlP->ageTime < (1000 * 1000))
        {
             ctlP->ageTime +=  ctlP->ageTime / 6;
        }
    }
}

// Format and add a dpy format command, will be sent to the display.
static void
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
    if( delayTime >= 511 )  // would overflow and/or be the same as the extended delay marker
    {
        delayTime = 510;
    }
    ctlP->lastTime = ctlP->now;

    if( (x > 01777) || (y > 01777) || (intensity > 7) )
    {
        logger(LOG_BOUNDS, "Boundary violation x %d y%d intensity %d\n", x, y, intensity);
    }

    cmd = (x & 0x3FF) | ((y & 0x3FF) << 10) | (delayTime << 23);
    cmd |= (intensity & 7) << 20;
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
// Each display has its own buffer and status.
// Return true if any read, else false.
static bool
lightpenReader(DisplayControlP ctlP)
{
int i;
int count;
int sockFlag = 1;
int lastX, lastY;
bool gotPosition;
bool penDown;

uint32_t penBuf[PENBUFSIZE];
uint32_t cmd;

    if( !isOpen(ctlP) )
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
                    // lightpen coordinates come in as dpy coordinates, -511,511 10 bit 1's complement,
                    // just to confuse things.
                    lastX = cvtDpyTo1024((cmd >> 10) & 0x3FF);
                    lastY = cvtDpyTo1024(cmd & 0x3FF);
                    logger(LOG_LP, "LP received x %d, y %d\n", lastX, lastY);
                }
            }
        }
    }

    // This is the only data modified in the display thread that is externally used.
    lockDisplayDataByCtlP(ctlP);
    if( gotPosition )
    {
        ctlP->lpX = lastX;
        ctlP-> lpY = lastY;
        ctlP->lpData = true;
    }

    ctlP->penDown = penDown;
    unlockDisplayDataByCtlP(ctlP);

    // Turn on fast ack to minimize delays.
    // This might or might not improve lightpen performance.
    setsockopt(ctlP->fd, IPPROTO_TCP, TCP_QUICKACK, &sockFlag, sizeof(sockFlag));
    return( gotPosition );
}

// Do a timed wait.
// We don't want the worker thread to block forever, needs to run occasionally.
static void
waitForRun()
{
struct timespec tm;
#if LOG_WAIT
struct timespec logTm;
uint64_t startTime;
uint64_t endTime;

    clock_gettime(CLOCK_MONOTONIC, &logTm);
    startTime = (logTm.tv_nsec + ((uint64_t)logTm.tv_sec * 1000 * 1000 * 1000));
#endif

#if defined(__GLIBC__) && ((__GLIBC__ > 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ >= 30)))
    // Wait against CLOCK_MONOTONIC so an NTP step or slew of the realtime clock
    //  can't distort the worker's wakeup cadence.
    clock_gettime(CLOCK_MONOTONIC, &tm);
    tm.tv_nsec += (WORKERSLEEPTIME * 1000);   // WORKERSLEEPTIME is in usecs
    if( tm.tv_nsec > 999999999 )
    {
        tm.tv_sec++;
        tm.tv_nsec -= 1000000000;
    }

    sem_clockwait(&runLock, CLOCK_MONOTONIC, &tm);
#else
    // Fallback for older glibc: POSIX sem_timedwait() is defined only against
    // CLOCK_REALTIME, so the absolute timeout must be built from it.
    clock_gettime(CLOCK_REALTIME, &tm);
    tm.tv_nsec += (WORKERSLEEPTIME * 1000);   // WORKERSLEEPTIME is in usecs
    if( tm.tv_nsec > 999999999 )
    {
        tm.tv_sec++;
        tm.tv_nsec -= 1000000000;
    }

    sem_timedwait(&runLock, &tm);
#endif

#if LOG_WAIT
    clock_gettime(CLOCK_MONOTONIC, &logTm);
    endTime = (logTm.tv_nsec + ((uint64_t)logTm.tv_sec * 1000 * 1000 * 1000));
    logger(LOG_WAIT, "woke up after %d usec\n", (endTime - startTime) / 1000);
#endif
}

// Get current system time in ns.
static uint64_t
currentTime()
{
struct timespec tm;
uint64_t time;

    clock_gettime(CLOCK_MONOTONIC, &tm);
    time = tm.tv_nsec;
    time += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    return( time );
}

// Convert a 10 bit 1's complement dpy coordinate to a 2's complement 0-1023 value.
static int
cvtDpyTo1024(int dpy)
{
    if( dpy & 01000 )
    {
        dpy++;
    }

    return( (dpy + 01000) & 01777);
}
