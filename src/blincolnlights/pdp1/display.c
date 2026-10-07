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
// The aging has since gone from the emulator entirely: the display clients fade the points on their own clocks.
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
// 8-Aug-2026 wje add lightpen exponential moving average prediction logic
// 14-Sep-2026 wje (Claude) a lightpen hit no longer uses up the pen position.
//    checkLightpen() used to clear lpData on every hit, and only a new position from the client set it
//    again. The client sends a position only when the mouse moves, so a pen held still got one hit and
//    then none. A real pen sees phosphor, not motion: now every point drawn inside the aperture while
//    the pen is down is a hit. lpData is cleared when the pen is lifted instead.
//    With motion prediction on, a pen that stops is fed its last position again every
//    PENHOLDRESAMPLE, so the prediction settles on where it stopped instead of extrapolating
//    from the last velocity it saw.
// 14-Sep-2026 wje (Claude) predictLightpen() no longer dereferences a null control pointer when the
//    screen is not configured, and it reads the filters under dataLock. lightpenReader() resets the
//    filters on pen up under dataLock too; it did that unlocked while checkLightpen() predicted
//    from them. initializeDisplay() publishes a new screen's control entry with a release store
//    (getDisplayControlP() loads it with acquire), since the worker thread is already running.
// 22-Sep-2026 wje (Claude) fix the ctlP->fd race between the worker thread and setDisplayFD()
//    (found by ThreadSanitizer). ctlP->fd is now
//    only ever installed by setDisplayFD() (atomic exchange, so it still updates synchronously) and
//    only ever closed by the worker thread: its own fd, in dropClient(), on a fatal write error or when
//    a read finds the client gone (atomic compare-exchange against the fd it was using, so a reconnect that already
//    replaced it is never double-closed); or an fd a reconnect displaced, handed off via ctlP->closeFd and closed once at
//    the top of the worker's next pass, never mid read/write. The worker snapshots ctlP->fd once per
//    screen per pass and passes it explicitly to putDpyCommand()/addDpyCommand()/
//    flushDisplay()/lightpenReader(), instead of each of them re-reading ctlP->fd, so a reconnect or
//    a write failure mid-pass can no longer make flushDisplay() write a batch meant for one client to
//    another client's socket. isOpen() and getDisplayFD() read ctlP->fd with an atomic load.
//    A ThreadSanitizer stress run with two threads reconnecting the same screen concurrently (the
//    original bug report used one) found the same unlocked-vs-locked pattern on the worker's
//    per-connection state: setDisplayFD() used to reset it under controlLock while the worker read
//    and wrote it without one. setDisplayFD() no longer touches it at all; the worker resets it
//    itself when ctlP->connGen (bumped by setDisplayFD() on every install) shows a new connection,
//    since fd numbers themselves get reused by the OS and can't be used to detect that. Two other,
//    unrelated pre-existing races the same stress
//    runs turned up -- displayInitialized, and initializeDisplay()'s non-atomic final read of
//    displays[screenNo] -- are not fixed by this change; they are not about fd ownership.
// 23-Sep-2026 Claude - while pidp1timing is on, the worker accounts for its own host time (waits, lock,
//    writes, lightpen reads, CPU) and for display()'s lock waits, one line a second to TIMING_FILE_DPY.
// 24-Sep-2026 Claude - the worker holds controlLock only to copy the pending commands out, not while it
//    converts and writes them, so a slow display client can no longer hold up display()'s callers,
//    among them the emulator thread (Type 30, Type 33). Once a client's full socket has cost a
//    command in a pass, the rest of that pass's commands are dropped without the retry wait.
// 25-Sep-2026 Claude - the worker is woken per batch instead of per point, runs with 1 ns of timer slack, and
//    stamps each point with its producer's time; a producer waits a bounded time on a full buffer, then drops,
//    and a stalled client's commands are dropped without retrying until it takes data again.
// 26-Sep-2026 Claude - the accounting is switched by displaytiming instead of pidp1timing, so the cycle
//    report no longer brings a file that grows a line a second.
// 28-Sep-2026 Claude - while nothing is drawing and the pen is up, the worker waits IDLESLEEPTIME instead of
//    WORKERSLEEPTIME, and the first point added during that wait wakes it.
// 28-Sep-2026 Claude - first-time setup of the subsystem and of each screen is serialized by setupLock, and
//    displayInitialized is published only once the worker exists, so concurrent first callers start one worker
//    and build one control entry per screen.
// 7-Oct-2026 wje/Claude - a partial write that ends inside a word no longer misaligns the client stream.
//    flushDisplay() keeps how much was actually sent and starts the next write after that
//    instead of sending those bytes again.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
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
#include "configuration.h"
#include "lpPredictor.h"

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
#define LOG_FULL 0

#define WORKERSLEEPTIME 250        // max time the worker thread will sleep while it has work, usecs
// The worker's wait while it has nothing to do, in usecs: no point came in its last pass, nothing is
// waiting to be written, and the pen is up on every screen. Its 250 us backstop cost 3,900 wakeups
// a second of a Pi 4 core with nothing to do. Only the pen waits on it: a pen put down on an idle
// screen is read up to this much later.
#define IDLESLEEPTIME 10000
#define PENBUFSIZE  64             // read up to this many lightpen update commands at once
#define CMDBUFSIZE  4096           // buffer up to this many display commands (sized to the 4096-word display memory)
// A producer wakes the worker with a point added this long after the worker last emptied the
// buffer, in ns, or once a quarter of the buffer is used. Waking it for every point cost a pass, a
// write() and a lightpen read per one to three points. A point reaches the client up to this much
// later, half of it on average, and a lightpen position is read up to this much later too.
#define POSTINTERVAL 250000
// How long a producer yields to the worker on a full commandBuf before it drops the point, in ns.
// A 340 held longer than its PACE_MAX_LAG slips against the CPU, which a program can see; a
// dropped point on a refreshed display blinks for one frame.
#define CMDFULLWAIT 3000000
#define DPYBUFSIZE  4096           // buffer up to this many outgoing dpy commands
#define DPYFULLRETRIES 100         // addDpyCommand: max retries while dpy buf is full before dropping a command
// A pen that is down but has sent nothing for this long, in ns, has stopped moving, and the motion
// prediction filters are given its last position again (see lightpenReader()).
// It is the predictor's own extrapolation limit, so a pen that is merely moving slowly is not
// mistaken for a stopped one.
#define PENHOLDRESAMPLE ((uint64_t)(DELTA_TIME_CLAMP * 1000.0 * 1000.0 * 1000.0))
#define TIMING_FILE_DPY "/tmp/pidp1-dpytiming.txt"
#define TIMING_WINDOW_NS 1000000000ULL      // one report line per this much wall time
#undef NEVER
#define NEVER ~((uint64_t)0)       // a long time from now
#define null 0
// ctlP->fd is written by more than one thread (setDisplayFD() and the worker, see the 22-Sep-2026
// header note), so every read outside the worker's own snapshot goes through an atomic load.
#define isOpen(ctlP) (__atomic_load_n(&((ctlP)->fd), __ATOMIC_ACQUIRE) >= 0)

bool useMotionPrediction = false;  // changed via config file motionPrediction setting

static int curRadius2 = 6*6;       // setLightpenRadius2() will override this
static bool displayInitialized;    // release-stored once the worker exists, see initializeDisplaySubsystem()
// Held only for first-time setup, of the subsystem and of each screen's control entry, and never while another
// lock is taken.
static pthread_mutex_t setupLock = PTHREAD_MUTEX_INITIALIZER;
static sem_t runLock;
static uint64_t dpyDropCount;      // count of dpy commands dropped because a client could not keep up

// Host-time accounting, gathered only while displaytiming is on. It is read once, when the subsystem
// starts, so a SIGHUP reload does not change it. The worker's counters are touched by the worker alone;
// the producer counters are added to by whichever thread calls display(), so they are atomic.
typedef struct {
    uint64_t passes;        // worker wakeups
    uint64_t timeouts;      // wakeups that were the timed wait running out, not a post
    uint64_t idleWaits;     // waits that were the IDLESLEEPTIME one
    uint64_t waitNs;        // wall ns in waitForRun()
    uint64_t lockWaitNs;    // ns the worker waited for controlLock
    uint64_t lockHoldNs;    // ns the worker held controlLock copying a batch out
    uint64_t points;        // commands taken from commandBuf
    uint64_t writes;        // write() calls
    uint64_t writeBytes;    // bytes write() accepted
    uint64_t writeNs;       // ns in write()
    uint64_t writeBlocked;  // writes refused with EAGAIN, EWOULDBLOCK or EINTR
    uint64_t writePartial;  // writes that took less than they were offered
    uint64_t fullSleeps;    // usleep()s waiting for a full dpyBuf to drain
    uint64_t penNs;         // ns in lightpenReader(): a read, a lock and a setsockopt every pass
    } DpyTimeAcc;

static bool dpyTimingOn;
static DpyTimeAcc dpyAcc;
static uint64_t dpyWinWall;         // currentTime() when the window opened, 0 while none is open
static uint64_t dpyWinCpu;          // worker CPU ns when the window opened
static uint64_t dpyWinDrops;        // dpyDropCount when the window opened
static uint64_t prodCalls;          // atomic: addCommand() calls
static uint64_t prodLockWaitNs;     // atomic: ns addCommand() waited for controlLock
static uint64_t prodPosts;          // atomic: worker wakeups posted by addCommand()
static uint64_t prodFullYields;     // atomic: yields because commandBuf was full
static uint64_t cmdDropCount;       // atomic: points producers dropped on a full commandBuf, always counted
static uint64_t dpyWinCmdDrops;     // cmdDropCount when the window opened

// Set by the worker while it takes the IDLESLEEPTIME wait, so a producer knows to wake it for the
// first point instead of leaving the point for the timeout. The worker sets it before it checks
// the command buffers under controlLock, and a producer reads it under controlLock after adding a
// point, so either the worker sees the point or the producer sees the flag. See enterIdle().
static bool workerIdle;

typedef struct {
    int fd;                        // file descriptor to use; see the 22-Sep-2026 header note for
                                    // who may write and close it
    int closeFd;                   // an fd setDisplayFD() displaced, for the worker to close at the
                                    // top of its next pass; -1 if none pending. Under controlLock.
    int connGen;                   // bumped by setDisplayFD() on every install, under controlLock.
                                    // Lets the worker notice a new connection even if the OS reused
                                    // the same fd number, without setDisplayFD() itself touching
                                    // dpyStalled (which the worker owns unlocked below).
    int curX, curY;                // last x,y coordinates set, 0-1023
    int intensity;                 // last intensity set
    uint64_t now;                  // the time of the worker's pass
    bool penDown;                  // lightpen is on the screen
    bool lpData;                   // set when a pen position comes in, cleared when the pen is lifted
    int lpX, lpY;                  // last x,y lightpen coordinates received
    int rawX, rawY;                // last x,y the client sent, never replaced by a prediction
    uint64_t lpSampleTime;         // when the prediction filters were last given a position, ns
    int lpRadius2;                 // used by the lightpen check
    pthread_mutex_t controlLock;   // for interlocking with the worker thread
    pthread_mutex_t dataLock;      // for locking get/setDisplayData

    int numCommands;
    uint32_t commandBuf[CMDBUFSIZE];
    uint64_t lastDrain;            // currentTime() when the worker last emptied commandBuf. Under controlLock.
    bool posted;                   // a producer has woken the worker since lastDrain. Under controlLock.
    bool cmdStalled;               // a producer dropped a point on a full commandBuf since lastDrain;
                                    // the rest are dropped without the wait. Under controlLock.
    int numDpyCommands;
    uint32_t dpyBuf[DPYBUFSIZE];
    int dpySentBytes;              // bytes of dpyBuf[0] a partial write already sent, 0-3; the next
                                    // write starts after them. Worker only.
    bool dpyStalled;               // a command was dropped because the client's socket stayed full;
                                    // commands are dropped without retrying until a write takes
                                    // data again. Worker only.
    LightpenCoordinateFilter xFilter;     // the two prediction filters
    LightpenCoordinateFilter yFilter;
} DisplayControl, *DisplayControlP;

static DisplayControlP displays[MAXDISPLAYS];
// The worker thread's own record of the last connGen it saw installed per screen, so it can
// reset its per-connection state exactly once per new connection. Touched only by the worker thread
// (see worker()), so it needs no lock and no atomics of its own.
static int lastSeenGen[MAXDISPLAYS];

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
// The outside interface for getting the predicted lightpen coordinates.
// It will set the coordiates and return true if there is a valid prediction, else false.
bool predictLightpen(int screenNo,  int *xP, int *yP);

// Internal functions.
static void initializeDisplaySubsystem(void);

static DisplayControlP initializeDisplay(int screenNo);
static DisplayControlP getDisplayControlP(int screenNo);
static bool lightpenReader(DisplayControlP ctlP, int fd);
static int cvtDpyTo1024(int dpy);
static uint64_t currentTime(void);
static void initializeDisplayControl(DisplayControlP ctlP);
static bool lockDisplayDataByCtlP(DisplayControlP ctlP);
static bool unlockDisplayDataByCtlP(DisplayControlP ctlP);

// This is the processing thread
static void *worker(void *);
static void lockControl(DisplayControlP ctlP);
static void unlockControl(DisplayControlP ctlP);
static bool flushDisplay(DisplayControlP ctlP, int fd);
static void dropClient(DisplayControlP ctlP, int fd);
static void addCommand(DisplayControlP ctlP, int x, int y, int intensity, bool defer);
static void putDpyCommand(DisplayControlP ctlP, int fd, unsigned int x, unsigned int y, unsigned int intensity);
static void addDpyCommand(DisplayControlP ctlP, int fd, uint32_t cmd);
static bool enterIdle(void);
static bool waitForRun(int waitUs);
static uint64_t dpyStamp(void);
static void dpyTimingCheck(void);

extern int decflg(int flg);

// Set the fd to use, true if screen is valid, false if out of range.
// Automatically allocate the display if it isn't already.
// An fd of -1 just closes the current fd if any.
//
// ctlP->fd is installed here synchronously (an atomic exchange, so getDisplayFD() right after this
// call returns still sees it), but the fd this call displaces is never closed here -- the worker
// thread may be mid read or mid write on it. It is handed to the worker instead (ctlP->closeFd),
// which closes it at the top of its next pass. See the file header, 22-Sep-2026.
bool
setDisplayFD(int screenNo, int fd)
{
int fdFlags;
int nodelay;
int oldFd;
DisplayControlP ctlP;

    if( !(ctlP = initializeDisplay(screenNo)) )
    {
        return(false);
    }

    // Lightpen needs nonblocking
    fdFlags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fdFlags | O_NONBLOCK));

    // Disable Nagle's algorithm. The display stream is latency-sensitive and is
    // frequently flushed in small batches; TCP_QUICKACK is also set on the read side in lightpenReader().
    nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    lockControl(ctlP);

    // Don't overwrite a handoff the worker hasn't picked up yet (a reconnect faster than the
    // worker's own pass rate), or that fd would leak. This only ever waits on the thread that
    // accepted the new connection, never on the emulator or worker thread.
    while( ctlP->closeFd >= 0 )
    {
        unlockControl(ctlP);
        sem_post(&runLock);     // make sure the worker isn't sleeping through this
        usleep(200);
        lockControl(ctlP);
    }

    oldFd = __atomic_exchange_n(&(ctlP->fd), fd, __ATOMIC_ACQ_REL);
    if( oldFd >= 0 )
    {
        ctlP->closeFd = oldFd;
    }
    ctlP->connGen++;

    // The worker's per-connection state (dpyBuf, dpyStalled) is NOT reset here (a second, prior stress
    // test found this race too): the worker reads and writes it without a lock, same as ctlP->fd
    // used to be. The worker resets it itself, once it notices the fd it just snapshotted is a
    // new one -- see worker().
    unlockControl(ctlP);

    // Wake the worker promptly rather than have the new connection, or the old fd above, wait
    // out a full sleep.
    sem_post(&runLock);

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

    return( __atomic_load_n(&(ctlP->fd), __ATOMIC_ACQUIRE) );
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

    (void)screenNo;             // kept in the signature to mirror getLightpenRadius2()'s
                                 // per-screen API, but every screen shares one radius (see below)
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
// The point is put into the output buffer to be processed when the worker thread wakes up, which is
// once a batch is ready (see addCommand()), not for every point.
bool
display(int screenNo, int x, int y, int intensity)
{
DisplayControlP ctlP;

    if( !(ctlP = getDisplayControlP(screenNo)) || !isOpen(ctlP) )
    {
        return(false);
    }

    logger(LOG_DISPLAY,"display(), screen %d x %d y %d intensity %d\n", screenNo, x, y, intensity);
    addCommand(ctlP, x & 01777, y & 01777, intensity & 07, true);
    return(true);
}

// See if there is a light pen hit in the radius that is set for the screen.
// The coordinates to check are passed.
// The coordinates are expected to be in 0-1024 range!
// If so, return true, else false.
// A hit does not use up the pen position: a pen held still hits every point drawn inside its
// aperture, every time it is drawn, as a real pen does. Limiting how often that is seen is
// the display's business, e.g. the Type 340 pauses and turns the pen off until it is resumed.
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
    // Use the distance equation for a circle to simulate an actual circular aperture.

    if( useMotionPrediction )
    {
        ctlP->lpX = motionFilterPredict(&(ctlP->xFilter));
        ctlP->lpY = motionFilterPredict(&(ctlP->yFilter));
    }

    delx = ctlP->lpX - x;               // Find squared magnitudes of hit offset
    dely = ctlP->lpY - y;

    if( ((delx*delx) + (dely*dely)) < ctlP->lpRadius2 )
    {
        logger(LOG_LP, "LP hit\n");
        gotHit = true;
    }

    unlockDisplayDataByCtlP(ctlP);
    return(gotHit);
}

// The outside interface for getting the predicted lightpen coordinates.
// It will set the coordiates and return true if there is a valid prediction, else false.
// The predicted location is updated automatically when a lightpen event comes in from the display.
// If the screen is not configured, the coordinates passed in are left as they are.
bool
predictLightpen(int screenNo,  int *xP, int *yP)
{
DisplayControlP ctlP;
bool predicted;

    if( !(ctlP = getDisplayControlP(screenNo)) )
    {
        return(false);
    }

    // The filters and the pen state are changed by the display thread, so read them under its lock.
    lockDisplayDataByCtlP(ctlP);
    if( !isOpen(ctlP) || !ctlP->penDown )
    {
        // Return whatever coords we last had, could be invalid.
        *xP = ctlP->lpX;
        *yP = ctlP->lpY;
        predicted = false;
    }
    else
    {
        *xP = motionFilterPredict(&(ctlP->xFilter));
        *yP = motionFilterPredict(&(ctlP->yFilter));
        predicted = true;
    }
    unlockDisplayDataByCtlP(ctlP);

    return(predicted);
}

// End of external functions, these are all internal.

// Called initially to set up everything. Safe to call from any thread, any number of times.
static void
initializeDisplaySubsystem()
{
pthread_t thread;
pthread_attr_t attr;
struct sched_param schedParam;
bool haveAttr;
int created;
ConfigurationSettingP settingP;
static bool runLockReady;       // under setupLock; a retry after a failed create must not re-init runLock

    // Every entry point comes through here, display() once per point, so the done case is one acquire
    // load and no lock. It pairs with the release store at the end: a caller that sees true also sees
    // runLock and the settings initialized.
    if( __atomic_load_n(&displayInitialized, __ATOMIC_ACQUIRE) )
    {
        return;             // already done
    }

    pthread_mutex_lock(&setupLock);
    if( __atomic_load_n(&displayInitialized, __ATOMIC_RELAXED) )
    {
        pthread_mutex_unlock(&setupLock);   // another first caller finished it while this one waited
        return;
    }

    logger(LOG_INIT,"Display subsystem being initialized\n");

    // Once only, even if the worker's create fails below and a later call retries: a screen built
    // after a failure already posts to runLock.
    if( !runLockReady )
    {
        // The first display connection comes after main.c's configure(), so the setting is loaded by now.
        settingP = findConfigurationSetting(getConfiguration(), "displaytiming");
        dpyTimingOn = (settingP && settingP->onOff);

        sem_init(&runLock, 0, 0);
        runLockReady = true;
    }

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
        // displayInitialized stays false, so the next call tries again.
        logger(LOG_INIT,"Display worker thread create failed.\n");
        pthread_mutex_unlock(&setupLock);
        return;
    }

    __atomic_store_n(&displayInitialized, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&setupLock);
    logger(LOG_INIT,"Initialization done.\n");
}

// Create a control entry for the screen number passed.
// If it is already created, do nothing.
// Also initialize the display subsystem if needed.
// Returns the control pointer, or null if the screen number is invalid or the entry cannot be allocated.
static DisplayControlP
initializeDisplay(int screenNo)
{
DisplayControlP ctlP;

    // displays[] has MAXDISPLAYS entries (valid indices 0..MAXDISPLAYS-1), so the
    // upper bound must be >= MAXDISPLAYS, not > MAXDISPLAYS (which let index
    // MAXDISPLAYS through and read/write one past the array).
    if( (screenNo < 0) || (screenNo >= MAXDISPLAYS) )
    {
        return(0);
    }

    initializeDisplaySubsystem();

    if( (ctlP = __atomic_load_n(&(displays[screenNo]), __ATOMIC_ACQUIRE)) )
    {
        return(ctlP);
    }

    // Two threads can connect the same new screen at once (the display listener and the console's
    // d command). The check, the build and the publish are one step under setupLock, so a screen gets
    // one entry: a second one would be orphaned, and the connection put into it never serviced or closed.
    pthread_mutex_lock(&setupLock);
    if( !(ctlP = __atomic_load_n(&(displays[screenNo]), __ATOMIC_RELAXED)) )
    {
        if( (ctlP = calloc(1, sizeof(DisplayControl))) )
        {
            initializeDisplayControl(ctlP);
            motionFilterReset(&(ctlP->xFilter));
            motionFilterReset(&(ctlP->yFilter));

            // The worker thread is already running and finds the entry through getDisplayControlP().
            // Publish it only once it is complete, with a release store paired with that acquire load,
            // so another core cannot see the pointer before the fields and the mutexes behind it.
            __atomic_store_n(&(displays[screenNo]), ctlP, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&setupLock);

    return(ctlP);
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
    return( __atomic_load_n(&(displays[screenNo]), __ATOMIC_ACQUIRE) );    // see initializeDisplay()
}

// Initialize a display, setting all of its fields to the default values.
static void
initializeDisplayControl(DisplayControlP ctlP)
{
    ctlP->fd = -1;
    ctlP->closeFd = -1;
    ctlP->connGen = 0;
    ctlP->numCommands = 0;
    ctlP->numDpyCommands = 0;
    ctlP->dpySentBytes = 0;
    ctlP->dpyStalled = false;
    ctlP->lastDrain = 0;
    ctlP->posted = false;
    ctlP->cmdStalled = false;
    ctlP->now = currentTime();
    ctlP->curX = ctlP->curY = ctlP->intensity = 0;
    ctlP->lpX = ctlP->lpY = 0;
    ctlP->rawX = ctlP->rawY = 0;
    ctlP->lpSampleTime = ctlP->now;
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
int fd;
int closeFd;
int gen;
int numDrained;
DisplayControlP ctlP;
uint32_t cmd;
uint64_t t0;
uint64_t t1;
bool timedOut;
bool idle;          // the last pass found nothing to do, see IDLESLEEPTIME
bool busy;
static uint32_t drained[CMDBUFSIZE];    // one screen's commands, copied out of commandBuf under the lock

    (void)argP;         // unused; pthread_create() is always called with null here

    // The timed wait is one of the worker's regular wake-ups now that producers post per batch, and
    // Linux's default 50 us of timer slack would be added to every one. The other threads inherit
    // 1 ns from throttleConfigure(), but this one has been measured at the default, so set it here.
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

    idle = false;
    while( true )
    {
        dpyTimingCheck();
        t0 = dpyStamp();
        idle = (idle && enterIdle());
        timedOut = waitForRun((idle) ? IDLESLEEPTIME : WORKERSLEEPTIME);
        __atomic_store_n(&workerIdle, false, __ATOMIC_RELAXED);
        if( t0 )
        {
            dpyAcc.waitNs += (currentTime() - t0);
            ++dpyAcc.passes;
            if( timedOut )
            {
                ++dpyAcc.timeouts;
            }

            if( idle )
            {
                ++dpyAcc.idleWaits;
            }
        }

        busy = false;
        for( i = 0; i < MAXDISPLAYS; ++i )
        {
            if( !(ctlP = getDisplayControlP(i)) )
            {
                continue;       // not allocated
            }

            // Snapshot the fd once for this screen's whole pass, and pick up any fd a reconnect
            // displaced since our last pass. Every function below is given this one value instead
            // of re-reading ctlP->fd, so a reconnect mid-pass is picked up on the NEXT pass rather
            // than switching the fd out from under a read or write in progress. See the file
            // header, 22-Sep-2026.
            // The pending commands are copied out under the lock too, and the buffer emptied.
            // Converting and writing them happens after the lock is let go, so a producer (the
            // emulator thread itself for the Type 30 and Type 33) never waits on a socket write.
            t0 = dpyStamp();
            lockControl(ctlP);
            t1 = 0;
            if( t0 )
            {
                t1 = currentTime();
                dpyAcc.lockWaitNs += (t1 - t0);
                dpyAcc.points += ctlP->numCommands;
            }

            fd = __atomic_load_n(&(ctlP->fd), __ATOMIC_ACQUIRE);
            closeFd = ctlP->closeFd;
            ctlP->closeFd = -1;
            gen = ctlP->connGen;
            numDrained = ctlP->numCommands;
            memcpy(drained, ctlP->commandBuf, (size_t)numDrained * sizeof(drained[0]));
            ctlP->numCommands = 0;
            ctlP->lastDrain = currentTime();    // read under the lock, so no producer stamp is later
            ctlP->posted = false;
            ctlP->cmdStalled = false;
            unlockControl(ctlP);
            if( t1 )
            {
                dpyAcc.lockHoldNs += (currentTime() - t1);
            }

            if( (fd >= 0) && (gen != lastSeenGen[i]) )
            {
                // A new connection since we last looked at this screen (connGen, not the fd
                // number itself, since the OS can and does reuse fd numbers across a close and
                // a later accept). Reset its state here, on the thread that reads and writes it
                // unlocked below, instead of setDisplayFD() writing it under controlLock against
                // that unlocked use.
                // The new client starts with an empty buffer, as dropClient() leaves one: what the
                // old client had not taken was drawn for it, and would reach the new one before
                // current output. Its stream starts on a word, whatever part of one the old got.
                ctlP->dpyStalled = false;
                ctlP->numDpyCommands = 0;
                ctlP->dpySentBytes = 0;
            }
            lastSeenGen[i] = gen;

            if( numDrained )
            {
                busy = true;
                logger(LOG_CMD,"Worker awake for screen %d, %d commands\n", i, numDrained);
            }

            if( closeFd >= 0 )
            {
                // The fd a reconnect displaced. Only ever closed here, never mid read/write on it.
                close(closeFd);
            }

            // Check for work
            for( j = 0; j < numDrained; ++j )
            {
                if( fd < 0 )
                {
                    break;      // fd died partway through this batch; the rest has nowhere to go
                }

                cmd = drained[j];
                intensity = (cmd >> 24) & 07;
                x = (cmd >> 12) & 01777;
                y = cmd & 01777;
                putDpyCommand(ctlP, fd, x, y, intensity);

                if( __atomic_load_n(&(ctlP->fd), __ATOMIC_ACQUIRE) != fd )
                {
                    fd = -1;    // putDpyCommand() found it dead, or a reconnect displaced it
                }
            }

            ctlP->now = currentTime();          // the lightpen goes by the pass's own time

            // Re-check: the drain loop above may have found this fd dead, or a reconnect may have
            // displaced it while we were not holding the lock. Either way, don't keep using a
            // stale fd number for the rest of this pass; the next pass will pick up whatever is
            // current.
            if( (fd < 0) || (__atomic_load_n(&(ctlP->fd), __ATOMIC_ACQUIRE) != fd) )
            {
                continue;
            }

            if( !flushDisplay(ctlP, fd) )
            {
                // The write found the client gone and fd is closed. Its number may already belong to
                // another socket, so the lightpen read below must not use it.
                continue;
            }

            // we read lp data even if not enabled, display could be sending it
            t0 = dpyStamp();
            lightpenReader(ctlP, fd);          // check for any pending input
            if( t0 )
            {
                dpyAcc.penNs += (currentTime() - t0);
            }

            // Commands a full client socket left behind (a stalled client included) need the flush
            // retried soon, and a pen that is down needs reading at the short interval.
            if( (ctlP->numDpyCommands > 0) || ctlP->penDown )
            {
                busy = true;
            }
        }

        idle = !busy;
    }

    return(0);
}

// Called by the worker before it waits, once a pass found nothing to do. Sets workerIdle, then
// checks every screen's command buffer, since a producer that added a point before the flag was
// set did not wake the worker for it.
// Returns true if every buffer is empty, so the IDLESLEEPTIME wait may be taken; false, with the
// flag cleared again, if a point is waiting.
static bool
enterIdle(void)
{
int i;
int pending;
DisplayControlP ctlP;

    __atomic_store_n(&workerIdle, true, __ATOMIC_RELAXED);
    for( i = 0; i < MAXDISPLAYS; ++i )
    {
        if( !(ctlP = getDisplayControlP(i)) )
        {
            continue;
        }

        lockControl(ctlP);
        pending = ctlP->numCommands;
        unlockControl(ctlP);
        if( pending )
        {
            __atomic_store_n(&workerIdle, false, __ATOMIC_RELAXED);
            return(false);
        }
    }

    return(true);
}

// Puts a command into the worker command buffer.
// X and y will be the usual Type 30 -511,511 10 bit coordinates.
// If defer is false, the worker is woken when the buffer goes from empty to non-empty. If it is
// true, the worker is woken only once a batch is ready: a quarter of the buffer used, or a point
// added POSTINTERVAL or more after the worker last emptied it. Either way the worker is woken for a
// point added during its IDLESLEEPTIME wait, at most one wakeup is posted between two drains, and the
// worker's timed wait picks up whatever is left when points stop.
// On a full buffer, the producer yields to the worker for up to CMDFULLWAIT and then drops the
// point; once one is dropped, the rest are dropped at once until the worker next drains.
static void
addCommand(DisplayControlP ctlP, int x, int y, int intensity, bool defer)
{
int cmd;
bool wasEmpty;
bool hitThreshold;
bool post;
uint64_t t0;
uint64_t now;
uint64_t fullSince;

    t0 = dpyStamp();
    lockControl(ctlP);
    if( t0 )
    {
        __atomic_fetch_add(&prodLockWaitNs, (currentTime() - t0), __ATOMIC_RELAXED);
        __atomic_fetch_add(&prodCalls, 1, __ATOMIC_RELAXED);
    }

    fullSince = 0;
    while( ctlP->numCommands >= CMDBUFSIZE )
    {
        now = currentTime();
        if( !fullSince )
        {
            fullSince = now;
        }

        if( ctlP->cmdStalled || ((now - fullSince) >= CMDFULLWAIT) )
        {
            ctlP->cmdStalled = true;
            unlockControl(ctlP);
            __atomic_fetch_add(&cmdDropCount, 1, __ATOMIC_RELAXED);
            logger(LOG_FULL,"addCommand cmd buffer still full, point dropped\n");
            return;
        }

        // Buffer full: signal the worker and yield the CPU to it rather than
        // sleeping a fixed interval.
        unlockControl(ctlP);
        if( dpyTimingOn )
        {
            __atomic_fetch_add(&prodFullYields, 1, __ATOMIC_RELAXED);
        }

        logger(LOG_FULL,"addCommand cmd buffer full\n");
        sem_post(&runLock);
        sched_yield();
        lockControl(ctlP);
    }

    cmd = ((intensity << 24) | (x << 12) | y);
    wasEmpty = (ctlP->numCommands == 0);
    ctlP->commandBuf[ctlP->numCommands++] = cmd;
    hitThreshold = (ctlP->numCommands >= (CMDBUFSIZE / 4));
    if( defer )
    {
        now = currentTime();        // read under the lock, so it is never earlier than lastDrain
        post = (hitThreshold || ((now - ctlP->lastDrain) >= POSTINTERVAL));
    }
    else
    {
        post = (wasEmpty || hitThreshold);
    }

    // Read under the lock, after the point is in: see workerIdle.
    if( __atomic_load_n(&workerIdle, __ATOMIC_RELAXED) )
    {
        post = true;
    }

    // A second post before the worker drains would only queue a pass with nothing to do.
    post = (post && !ctlP->posted);
    if( post )
    {
        ctlP->posted = true;
    }
    unlockControl(ctlP);

    if( post )
    {
        if( dpyTimingOn )
        {
            __atomic_fetch_add(&prodPosts, 1, __ATOMIC_RELAXED);
        }

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
// After one drop, the client is stalled until a write takes data again (flushDisplay() clears
// it): a command that finds the buffer full is dropped at once, and the flush at the end of each
// pass is what finds out whether the client is reading again. So a stopped client costs one retry
// period, not one a pass: with passes every POSTINTERVAL, a retry period in each would keep the
// worker from emptying commandBuf, and the producers behind it would wait.
// fd is this pass's snapshot of ctlP->fd (see worker()); it is not re-read from ctlP here.
static void
addDpyCommand(DisplayControlP ctlP, int fd, uint32_t cmd)
{
int retries;

    retries = 0;
    while( ctlP->numDpyCommands >= DPYBUFSIZE )
    {
        logger(LOG_FULL,"addDpyCommand dpy cmd buffer full\n");

        if( ctlP->dpyStalled )
        {
            ++dpyDropCount;
            return;
        }

        if( !flushDisplay(ctlP, fd) )
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
            ctlP->dpyStalled = true;
            logger(LOG_FULL,"addDpyCommand giving up, dropping command (total drops %lu)\n",
                (unsigned long)dpyDropCount);
            return;
        }

        // This runs on the worker thread and is waiting for the non-blocking
        // socket's send buffer to drain, so a short sleep is fine here.
        if( dpyTimingOn )
        {
            ++dpyAcc.fullSleeps;
        }

        usleep(30);         // delay a bit so display doesn't overrun
    }

    ctlP->dpyBuf[ctlP->numDpyCommands++] = cmd;
}

// The client on fd is gone, found by a failed write (flushDisplay()) or a read (lightpenReader()).
// Its pen is lifted, so a later client on this screen does not inherit a pen that is down, and what
// was buffered for it is dropped. Apart from setDisplayFD()'s handoff, this is the only place that
// ends a display fd's life. ctlP->fd is cleared with a compare-exchange against fd: if setDisplayFD()
// has already swapped in a newer connection, that call took ownership of closing fd (ctlP->closeFd),
// so the compare fails and fd is left alone, or it would be closed twice.
// Worker thread only, holding no lock.
static void
dropClient(DisplayControlP ctlP, int fd)
{
int expected;

    lockDisplayDataByCtlP(ctlP);
    ctlP->penDown = false;
    ctlP->lpData = false;
    motionFilterReset(&(ctlP->xFilter));
    motionFilterReset(&(ctlP->yFilter));
    unlockDisplayDataByCtlP(ctlP);

    expected = fd;
    if( __atomic_compare_exchange_n(&(ctlP->fd), &expected, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) )
    {
        close(fd);
    }

    ctlP->numDpyCommands = 0;
    ctlP->dpySentBytes = 0;
    ctlP->dpyStalled = false;
}

// Write any buffered dpy commands to fd, this pass's snapshot of ctlP->fd (see worker()); it is
// not re-read from ctlP here, so a reconnect swapping ctlP->fd in mid-call can never make this
// write, or dropClient()'s close, land on the wrong client's socket.
// The fd is non-blocking so write() can legitimately return -1/EAGAIN or EWOULDBLOCK when the socket's
// send buffer is full.
// In that case, leave the buffered commands in place and let the caller retry later.
// A partial write is also possible on a non-blocking socket, and it can end inside a word. The words
// it finished are moved out of the buffer, and dpySentBytes keeps how much of the next word went, so
// the next attempt starts at the first byte not sent. The client reads 4-byte words with nothing to
// resync by, so a byte sent twice, or one not sent, would garble every word after it.
// Any other write() error means fd is gone, and dropClient() ends it.
// Returns false if fd just died (the caller must stop using it), true otherwise.
static bool
flushDisplay(DisplayControlP ctlP, int fd)
{
int size;
int resp;
int sentBytes;
int sentCommands;
uint64_t t0;

    if( fd < 0 )
    {
        return(true);       // nothing to do
    }

    size = (ctlP->numDpyCommands * (int)sizeof(ctlP->dpyBuf[0])) - ctlP->dpySentBytes;
    if( size <= 0 )
    {
        return(true);       // nothing to write
    }

    t0 = dpyStamp();
    resp = write(fd, ((char *)ctlP->dpyBuf) + ctlP->dpySentBytes, size);

    // clock_gettime() leaves errno alone on success, so the tests below still see write()'s.
    if( t0 )
    {
        dpyAcc.writeNs += (currentTime() - t0);
        ++dpyAcc.writes;
        if( resp > 0 )
        {
            dpyAcc.writeBytes += resp;
            if( resp < size )
            {
                ++dpyAcc.writePartial;
            }
        }
    }

    if( resp < 0 )
    {
        if( (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR) )
        {
            if( dpyTimingOn )
            {
                ++dpyAcc.writeBlocked;
            }

            // Socket send buffer full or interrupted - not fatal, retry later.
            logger(LOG_DPYWRITE,"flushDisplay: write would block (errno %d), retrying later\n", errno);
            return(true);
        }

        logger(LOG_FD,"fd %d closed, write error (errno %d)\n", fd, errno);
        dropClient(ctlP, fd);
        return(false);
    }

    logger(LOG_DPYWRITE,"flushdisplay wrote %d bytes\n", resp);
    ctlP->dpyStalled = false;       // the client is reading again, see addDpyCommand()

    if( resp == size )
    {
        ctlP->numDpyCommands = 0;
        ctlP->dpySentBytes = 0;
        return(true);
    }

    // Partial write: move the words it finished out, and keep the bytes it sent of the one after.
    // A word partly sent stays in dpyBuf[0], so addDpyCommand()'s count of what is buffered still holds.
    sentBytes = ctlP->dpySentBytes + resp;
    sentCommands = sentBytes / (int)sizeof(ctlP->dpyBuf[0]);
    memmove(ctlP->dpyBuf, &ctlP->dpyBuf[sentCommands],
        (size_t)(ctlP->numDpyCommands - sentCommands) * sizeof(ctlP->dpyBuf[0]));
    ctlP->numDpyCommands -= sentCommands;
    ctlP->dpySentBytes = sentBytes % (int)sizeof(ctlP->dpyBuf[0]);
    return(true);
}

// Format and add a dpy format command, will be sent to the display.
// Bits 23-31 are the protocol's delay field, sent as 0: each client fades the points on its own
// clock. 511 there would mark an escape pair (then a delay word), which is never sent.
static void
putDpyCommand(DisplayControlP ctlP, int fd, unsigned int x, unsigned int y, unsigned int intensity)
{
int cmd;

    if( fd < 0 )
    {
        return;     // not open, don't bother
    }

    if( (x > 01777) || (y > 01777) || (intensity > 7) )
    {
        logger(LOG_BOUNDS, "Boundary violation x %d y%d intensity %d\n", x, y, intensity);
    }

    cmd = (x & 0x3FF) | ((y & 0x3FF) << 10);
    cmd |= (intensity & 7) << 20;
    logger(LOG_DPYCMD,"adding dpy command 0x%08x, x %d y %d\n", cmd, x, y);
    addDpyCommand(ctlP, fd, cmd);
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
// The position stays valid until the pen moves or is lifted; a hit does not use it up.
// The client sends nothing while the mouse is still, so a pen held still is simply one
// whose last position is still current.
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
lightpenReader(DisplayControlP ctlP, int fd)
{
int i;
int count;
int sockFlag = 1;
int lastX, lastY;
bool gotPosition;
bool penDown;
bool penLifted;
bool readAny;

uint32_t penBuf[PENBUFSIZE];
uint32_t cmd;

    if( fd < 0 )
    {
        lockDisplayDataByCtlP(ctlP);
        ctlP->lpX = ctlP->lpY = 0;
        ctlP->penDown = false;
        ctlP->lpData = false;
        unlockDisplayDataByCtlP(ctlP);
        return(false);                          // nothing open yet
    }

    gotPosition = false;
    penLifted = false;
    readAny = false;
    lastX = 0;                                  // audit M10: hygiene init, gotPosition guards real use
    lastY = 0;
    penDown = ctlP->penDown;                    // audit M10: preserve current state across a no-data pass

    // Read all pending commands.
    // Only the mouse move last will be significant, but we do need to check pen up / pen down for all.
    while( (count = read(fd, penBuf, sizeof(penBuf))) > 0 )
    {
        readAny = true;
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
                    penLifted = true;           // the filters are reset below, under the lock
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

    // A read of 0 is the client closing its end, and an error other than "nothing to read yet" is a
    // failed connection: either way the client is gone. An idle screen writes nothing, so a failed
    // write would not notice until the program drew again.
    if( (count == 0) || ((count < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) )
    {
        logger(LOG_FD,"fd %d closed, the client left\n", fd);
        dropClient(ctlP, fd);
        return(false);
    }

    // This is the only data modified in the display thread that is externally used.
    lockDisplayDataByCtlP(ctlP);
    if( penLifted )
    {
        // Any position read after the last pen up is still set in gotPosition/lastX/lastY, and is
        // added below, after the reset, the same order the commands came in.
        motionFilterReset(&(ctlP->xFilter));
        motionFilterReset(&(ctlP->yFilter));
    }

    if( gotPosition )
    {
        ctlP->lpX = lastX;
        ctlP-> lpY = lastY;
        ctlP->rawX = lastX;
        ctlP->rawY = lastY;
        ctlP->lpData = true;
        motionFilterAdd(&(ctlP->xFilter), lastX);
        motionFilterAdd(&(ctlP->yFilter), lastY);
        ctlP->lpSampleTime = ctlP->now;
    }
    else if( penDown && useMotionPrediction && ((ctlP->now - ctlP->lpSampleTime) >= PENHOLDRESAMPLE) )
    {
        // The pen is down and has sent nothing for PENHOLDRESAMPLE: it is being held still.
        // Without new samples the filters would go on extrapolating from the last velocity they saw,
        // and the prediction would stay off to one side of where the pen stopped.
        // Giving them the last position again lets the velocity decay and the prediction settle on it.
        motionFilterAdd(&(ctlP->xFilter), ctlP->rawX);
        motionFilterAdd(&(ctlP->yFilter), ctlP->rawY);
        ctlP->lpSampleTime = ctlP->now;
    }

    if( !penDown )
    {
        ctlP->lpData = false;       // a lifted pen has no position
    }

    ctlP->penDown = penDown;
    unlockDisplayDataByCtlP(ctlP);

    // Turn on fast ack to minimize delays.
    // This might or might not improve lightpen performance.
    // Only after the client has sent something: a pass with nothing read has nothing to acknowledge.
    if( readAny )
    {
        setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &sockFlag, sizeof(sockFlag));
    }

    return( gotPosition );
}

// Wait up to waitUs microseconds for a post.
// We don't want the worker thread to block forever, needs to run occasionally.
// Returns true if the wait ended without a post (the timeout, or a signal), false if posted.
static bool
waitForRun(int waitUs)
{
struct timespec tm;
int status;
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
    tm.tv_nsec += ((long)waitUs * 1000);
    if( tm.tv_nsec > 999999999 )
    {
        tm.tv_sec++;
        tm.tv_nsec -= 1000000000;
    }

    status = sem_clockwait(&runLock, CLOCK_MONOTONIC, &tm);
#else
    // Fallback for older glibc: POSIX sem_timedwait() is defined only against
    // CLOCK_REALTIME, so the absolute timeout must be built from it.
    clock_gettime(CLOCK_REALTIME, &tm);
    tm.tv_nsec += ((long)waitUs * 1000);
    if( tm.tv_nsec > 999999999 )
    {
        tm.tv_sec++;
        tm.tv_nsec -= 1000000000;
    }

    status = sem_timedwait(&runLock, &tm);
#endif

#if LOG_WAIT
    clock_gettime(CLOCK_MONOTONIC, &logTm);
    endTime = (logTm.tv_nsec + ((uint64_t)logTm.tv_sec * 1000 * 1000 * 1000));
    logger(LOG_WAIT, "woke up after %d usec\n", (endTime - startTime) / 1000);
#endif

    return( status != 0 );
}

// The current time, or 0 when displaytiming is off, so a caller pays for a clock read only while measuring.
// A nonzero return is the start stamp for an accumulator; 0 means skip it.
static uint64_t
dpyStamp(void)
{
    if( dpyTimingOn )
    {
        return( currentTime() );
    }

    return(0);
}

// Open the first accounting window, or close the current one once it is TIMING_WINDOW_NS old by
// appending its totals to TIMING_FILE_DPY as one line of key=value pairs and starting the next.
// Worker thread only. Wall time is the window's whole span; CPU time is the worker's own.
static void
dpyTimingCheck(void)
{
uint64_t now;
uint64_t cpu;
uint64_t pCalls;
uint64_t pLockWait;
uint64_t pPosts;
uint64_t pYields;
uint64_t cmdDrops;
struct timespec tm;
FILE *fP;

    if( !dpyTimingOn )
    {
        return;
    }

    now = currentTime();
    if( dpyWinWall && ((now - dpyWinWall) < TIMING_WINDOW_NS) )
    {
        return;
    }

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tm);
    cpu = (((uint64_t)tm.tv_sec * 1000 * 1000 * 1000) + (uint64_t)tm.tv_nsec);
    pCalls = __atomic_exchange_n(&prodCalls, 0, __ATOMIC_RELAXED);
    pLockWait = __atomic_exchange_n(&prodLockWaitNs, 0, __ATOMIC_RELAXED);
    pPosts = __atomic_exchange_n(&prodPosts, 0, __ATOMIC_RELAXED);
    pYields = __atomic_exchange_n(&prodFullYields, 0, __ATOMIC_RELAXED);
    cmdDrops = __atomic_load_n(&cmdDropCount, __ATOMIC_RELAXED);

    // The first call only opens the window; what the producers counted before it is discarded above.
    if( dpyWinWall && (fP = fopen(TIMING_FILE_DPY, "a")) )
    {
        fprintf(fP, "dpy tid=%ld wall=%llu cpu=%llu passes=%llu timeouts=%llu wait=%llu lockwait=%llu "
            "lockhold=%llu points=%llu writes=%llu wbytes=%llu write_ns=%llu wblocked=%llu wpartial=%llu "
            "fullsleeps=%llu drops=%llu pen_ns=%llu prod_n=%llu prod_lockwait=%llu prod_posts=%llu "
            "prod_yields=%llu prod_drops=%llu idlewaits=%llu\n",
            (long)syscall(SYS_gettid),
            (unsigned long long)(now - dpyWinWall), (unsigned long long)(cpu - dpyWinCpu),
            (unsigned long long)dpyAcc.passes, (unsigned long long)dpyAcc.timeouts,
            (unsigned long long)dpyAcc.waitNs, (unsigned long long)dpyAcc.lockWaitNs,
            (unsigned long long)dpyAcc.lockHoldNs, (unsigned long long)dpyAcc.points,
            (unsigned long long)dpyAcc.writes, (unsigned long long)dpyAcc.writeBytes,
            (unsigned long long)dpyAcc.writeNs, (unsigned long long)dpyAcc.writeBlocked,
            (unsigned long long)dpyAcc.writePartial, (unsigned long long)dpyAcc.fullSleeps,
            (unsigned long long)(dpyDropCount - dpyWinDrops), (unsigned long long)dpyAcc.penNs,
            (unsigned long long)pCalls, (unsigned long long)pLockWait, (unsigned long long)pPosts,
            (unsigned long long)pYields, (unsigned long long)(cmdDrops - dpyWinCmdDrops),
            (unsigned long long)dpyAcc.idleWaits);
        fclose(fP);
    }

    memset(&dpyAcc, 0, sizeof(dpyAcc));
    dpyWinWall = currentTime();
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tm);
    dpyWinCpu = (((uint64_t)tm.tv_sec * 1000 * 1000 * 1000) + (uint64_t)tm.tv_nsec);
    dpyWinDrops = dpyDropCount;
    dpyWinCmdDrops = cmdDrops;
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
