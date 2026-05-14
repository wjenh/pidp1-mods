/*
 * This is an implementation of the PDP-1 Type 340 display.
 * It is a complex beast!
 * It is a computer in its own right, this is the emulator for it.
 *
 * Note that unlike the Type 30 display and the Type 33 symbol generator, the coordinate system is
 * always positve; the lower left corner is 0,0, upper right 1023,1023.
 * Coordinates have to be mapped to the Type 30 standard -511,511 10 bit ones complement form for the
 * display communication.
 *
 * 20-Apr-2026 wje initial implementation
 * 23-Apr-2026 wje switch to a semaphore for synchronization, more efficient
 * 25-Apr-2026 wje switch from nanosleep() to a spin-wait, the resheculing by Linux is just too unpredictable.
 *    It now uses the spin-wait for all of the short delays, nanosleep() for the 35us dot delay, and lets
 *    rescheduling happen otherwise by the semaphore wait at the end of a display cycle.
 * 26-Apr-2026 wje add more timing, interrupt always on lp hit or edge violation
 * 29-Apr-2026 wje add dual charset control via the parameter instruction
 * 1-May-2026 wje tweak timings to match DEC logic document
 * 4-May-2026 wje finally got vector delay to give a stable display, in conjunction with display driver changes
 * 5-May-2026 wje set stop on an increment edge violation
 * 6-May-2026 wje just a note here, a start or stop of the pdp-1 stops the 340 and reverts to param mode
 * 8-May-2026 wje fix some code formatting, adjust vcontinue delay time, slight refactor to simplify resets
 * 11-May-2026 wje allow character to complete the current word before a lightpen pause is handled
 * 13-May-2026 wje handle case of both deltas 0 in vectors
 * 14-May-2026 wje general cleanup, remove unused code, refactor vcontinue to make it more clear
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>

#include "common.h"
#include "pdp1.h"
#include "highSpeedChannels.h"
#include "display.h"
#include "configuration.h"
#include "type340emu.h"

//#define DOLOGGING
#include "iotLogger.h"
#define LOG_INIT 0
#define LOG_START 0
#define LOG_RUN 0
#define LOG_CMD 0
#define LOG_WAIT 0
#define LOG_CONFIG 0
#define LOG_ERR 0
#define LOG_PARAM 0
#define LOG_SLAVE 0
#define LOG_DRAW 0
#define LOG_POINT 0
#define LOG_INCREMENT 0
#define LOG_VECTOR 0
#define LOG_BRM 0
#define LOG_SUBROUTINE 0
#define LOG_JUMP 0
#define LOG_SAVE 0
#define LOG_DEPOSIT 0
#define LOG_CHARACTER 0
#define LOG_ESCAPE 0
#define LOG_LP 0
#define LOG_FLAGS 0
#define LOG_BOUNDS 0
#define LOG_TIMING 0

// This defines the time each command takes to initialize, 1.5 usecs. This might not be accurate.
#define SETUP_TIME 1500

// Special characters for character mode
#define CH_LF     0001   // Line feed
#define CH_CR     0002   // Carriage return
#define CH_UC     0003   // Shift in
#define CH_LC     0004   // Shift out
#define CH_ESC    0005   // Escape from character mode
#define CH_NSPC   0006   // Non spaced
#define CH_D      0007   // Descender
#define CH_BS     0010   // Backspace
#define CH_SUB    0011   // Subscript
#define CH_SUP    0012   // Superscript

#define HSC_CHAN 3         // lowest priority, drum uses 1
#define SPIN_LIMIT 20000   // max ns we will spin for, otherwise use nanosleep()

// The interrupt channel in the documentation is 0.
#define BRKCHAN 0
#define NUMSLAVEGROUPS 2
#define NUMSLAVES (NUMSLAVEGROUPS * 4)

#define CHARWIDTH 6     // char width for scale 0, pixels
#define CHARHEIGHT 11   // char width for scale 0, pixels

// Convert a command word mode field to one of the mode enum values
#define MODE(x) (((x) >> 13) & 07)
// Convert one of the mode enum values to the value to used in a command word.
#define PUTMODE(x) (((x) << 13) & 07)

// Used to terminate vector and increment modes
#define ESCAPE(x) ((x) & 0400000)

// Used by vector and increment, but not point
#define INTENSIFY(x) ((x) & 0200000)

// Per-mode flags.
// Extract various parameter settings.

#define PARAM_LP_CHANGE(x) ((x) & 010000)
#define PARAM_LP_ENABLE(x) ((x) & 04000)

#define PARAM_STOP(x) ((x) & 02000)
#define PARAM_STOP_INTERRUPT(x) ((x) & 01000)

#define PARAM_SCALE_CHANGE(x) ((x) & 0100)
#define PARAM_SCALE(x) (((x) >> 4) & 03)

#define PARAM_INTENSITY_CHANGE(x) ((x) & 010)
#define PARAM_INTENSITY(x) ((x) & 07)

// These are nonstandard, but added because we can switch between one and two charsets.
// These allow overriding of the original config setting.
#define PARAM_CHARSET_CHANGE(x) ((x) & 0400)
#define PARAM_CHARSETS(x) ((x) & 0200)

#define SLAVE_GROUP(x) (((x) >> 16) & 03);
#define SLAVE_GET_FLAGS(x, slave) (((x) >> (3 * (3 - slave))) & 07)

#define POINT_VERTICAL(x) ((x) & 0200000)
#define POINT_LP_CHANGE(x) PARAM_LP_CHANGE(x)
#define POINT_LP_ENABLE(x) PARAM_LP_ENABLE(x)
#define POINT_INTENSIFY(x) ((x) & 02000)
#define POINT_ADDRESS(x) ((x) & 01777)

#define VECTOR_DX(x) ((x) & 0377)
#define VECTOR_DY(x) (((x) >> 8) & 0377)
#define VECTOR_SIGN(x) ((x) & 0200)

#define SUBROUTINE_OP(x) (((x) >> 16) & 03)
#define PUT_SUBROUTINE_OP(x) (((x) << 16) & 03)
#define SUBROUTINE_ADDR(x) ((x) & 017777)

// flags that can be set for skips, etc.
#define FLAG_VEDGE 01
#define FLAG_HEDGE 02
#define FLAG_STOP 04
#define FLAG_LP 010

// Bit masks for the increment nibble.
#define MOVEX 0x8
#define LEFT 0x4
#define MOVEY 0x2
#define DOWN 0x1

// These must have values that match the MODE values above!
typedef enum {
    PARAMETER = 0,
    POINT = 1,
    SLAVE = 2,
    CHARACTER = 3,
    VECTOR = 4,
    VCONTINUE = 5,
    INCREMENT = 6,
    SUBROUTINE = 7
    } Modes;

// Used in the polling loop by modes to control their flow.
typedef enum {
    INITIALIZE,
    RUNNING,
    STOPPED
    } States;

// Subroutine sub operations.
// Must have the values given here!
typedef enum {
    DEPOSITSAVEREGISTER = 1,
    JUMP = 2,
    JUMPANDSAVE = 3
    } SubOps;

// Status returns from brmNext(), doIncrement(). doCharacter()
typedef enum {
    BRMRUNNING,
    EDGEVIOLATION,
    COMPLETED,
    PAUSE,
    ESCAPE
    } Status;

// This holds state for the pseudo-binary-rate-multiplier.
typedef struct {
    int startX;
    int startY;
    int deltaX;     // relative to startX, accumulated delta
    int deltaY;     // relative to startY, accumulated delta
    int dotSpacing; // pixel spacing in pixels, not the raw value from the instruction
    int curStep;    // how many points we have consumed
    int nPoints;    // and how many we do
    int xRate;      // increment x every xRate points
    int yRate;      // increment y every yRate points
    bool plusX;     // the x axis endpoint is greater than the initial point
    bool plusY;     // the y axis endpoint is greater than the initial point
    bool draw;      // if true, the vector is drawn, else just positions
    } BRMState, *BRMStateP;

// For slave monitors
typedef struct {
    bool displayEnabled;
    bool lpEnabled;
    } Slave, *SlaveP;

static HSCChannelP chanP;   // how we get data
static sem_t waitSemaphore; // how we wait for commands

// Primary state
static Modes curMode = PARAMETER;
static Modes curState = STOPPED;
static BRMState brmState;
static int pendingDelay;
static int curAddress;      // the address set by the dla command, where we will fetch data from
static int curX, curY;      // current display coordinates
static int lpX, lpY;        // last lp hit display coordinates
static int curScale;
static int curIntensity;
static int shiftState;     // will be 0 for upper shift or 64 for lower shift, first set or second set
static int flags;           // one of the FLAG x values

static int saveRegister;    // used with the SAVE subroutine subcommand
static bool saveActive;
static bool interruptEnabled = false;
static bool lpEnabled = false;
static bool slavesEnabled;  // set if we saw a SLAVE command

static bool threadRunning = false;  // emulator thread is set up
static bool isPaused = false;       // got a PAUSE command
static bool needBreak = false;      // edge violation occurred and an interrupt is needed
static bool origCharsets = false;   // initial twoCharsets from config or default
static bool twoCharsets = false;
static Slave slaves[NUMSLAVES];    // could be up to 16 slaves, but the core display support is 8

#if LOG_TIMING
static uint64_t startTime;
static int minX, minY;
static int maxX, maxY;
static int totalPoints;
#endif

// Communication with the IOT
static EmuControl emuControl;
static EmuControlP ctlP = &emuControl;

static void *emulator(void *argP);
static void reset340(void);
static Word getWord(PDP1P pdp1P);
static bool drawAndCheck(bool tryLightpen, int x, int y, int intensity);
static bool checkBounds(int x, int y);
static bool brmInitialize(BRMStateP stateP, int initialX, int initialY,
    int dX, int dY, int step, bool draw);
static void brmContinue(BRMStateP stateP, int x, int y);
static Status brmNext(BRMStateP stateP, int *xP, int *yP);
static Status doIncrement(int dotSpacing, int bits);
Status doCharacter(int dotSpacing, unsigned char ch);
static void emuOrFlags(int newFlags);   // only used internally
static void emuClearFlag(int flagbit);  // only used internally
static int nanowait(int ns);
static int nanodelay(int ns);
static int nanopause(int ns);
static uint64_t getNow(void);
static void configure(void);

// Interface to the low-level display subsystem
extern bool display(int screenNo, int x, int y, int intensity);
extern bool checkLightpen(int screenNo, int x, int y);

// And to the pidp-1 emulator
extern void initiateBreak(int brkno);

void
emuInitialize(PDP1P pdp1P)
{
pthread_t thread;
pthread_attr_t tattr;
struct sched_param param;

    if( threadRunning )
    {
        return;         // already done
    }

    threadRunning = true;
    iotCondLog(LOG_INIT,"initialize called\n");

    ctlP->pdp1P = pdp1P;
    configure();

    chanP = HSCallocateChannel(HSC_CHAN);
    ctlP->commandSent = false;
    ctlP->responseSent = false;
    ctlP->response = EMU_RESPONSE_DONE;

    sem_init(&waitSemaphore, 0, 0);

    pthread_attr_init (&tattr);
    pthread_attr_getschedparam (&tattr, &param);
    param.sched_priority = -10;
    pthread_attr_setschedparam (&tattr, &param);

    if( pthread_create(&thread, &tattr, emulator, ctlP) )
    {
        iotCondLog(LOG_INIT,"thread create failed\n");
        threadRunning = false;
    }
}

// Return a pointer to the control structure.
EmuControlP
getEmuControlP()
{
    return( ctlP );
}

// Called by external code to wake up if sleeping
void
emuWakeup(EmuControlP ctlP)
{
int val;

    //if( sem_trywait(&waitSemaphore) )
    sem_getvalue(&waitSemaphore, &val);
    if( val <= 0 )      // worker is sleeping
    {
        sem_post(&waitSemaphore);
    }
}

// Return the current execution address.
// Note that this is a snapshot, the emulator could be running.
int
emuGetAddress()
{
    return( curAddress );
}

// Return the current flag settings;
int
emuGetFlags()
{
    iotCondLog(LOG_FLAGS,"Returning flags %o\n", flags);
    return( flags );
}

// Clear the current flags.
void
emuClearFlags()
{
    iotCondLog(LOG_FLAGS,"Clear flags\n");
    flags = 0;
}

// Clear a flag.
void
emuClearFlag(int flagbit)
{
    iotCondLog(LOG_FLAGS,"Clear flag %0\n", flagbit);
    flags &= ~flagbit;
}

// Ors the passed flag bits into the current flags
void
emuOrFlags(int newFlags)
{
    flags |= newFlags;
    iotCondLog(LOG_FLAGS,"Set flags to %o\n", flags);
}

// Return the current x and y coordinates.
// Note that unless the emulator is stopped or paused,  this is a snapshot.
// If paused, it was because of a lightpen hit, so return those coords instead of the curren x and y.
void
emuGetXY(int *xP, int *yP)
{
    if( isPaused )
    {
        *xP = lpX;
        *yP = lpY;
    }
    else
    {
        *xP = curX;
        *yP = curY;
    }
}

// Return the initialization state.
// Note that this is a snapshot, the emulator could be running.
bool
emuIsInitialized()
{
    return( threadRunning );
}

// Return the pause state.
// Note that this is a snapshot, the emulator could be running.
bool
emuIsPaused()
{
    return( threadRunning && isPaused );
}

// Return the run state.
// Note that this is a snapshot, the emulator could stop..
bool
emuIsRunning()
{
    return( threadRunning && (curState == RUNNING) );
}

// Thiis is where the work is done.
// It's a somewhat complex state machine with 2 determining factors, the current mode and state.
// It runs until it reaches a stop condition or is explicitly stopped.
void *
emulator(void *dummy)
{
int newMode;
int command;
int word;
int i, tmp;
int x, y;
int lastdX, lastdY;
bool sawExit;
bool sawEscape;
Status status;

    sawEscape = sawExit = false;

    // If the cmmand isn't NONE, ctlP will be locked.
    while( !sawExit )
    {
        if( isPaused || (curState != RUNNING) )
        {
            iotCondLog(LOG_WAIT, "Waiting\n");
            sem_wait(&waitSemaphore);
            iotCondLog(LOG_WAIT, "Woke up\n");
        }

        if( (command = get340Command(ctlP)) == EMU_CMD_EXIT )
        {
            break;      // shut down, kill thread
        }

        switch( command )
        {
        case EMU_CMD_NONE:
            // Nothing to do, wait for a wakeup
            curState = STOPPED;
            continue;

        case EMU_CMD_RUN:
#if LOG_TIMING
            startTime = getNow();
            totalPoints = 0;
            maxX = maxY = 0;
            minX = minY = 9999;
#endif
            reset340();
            curAddress = ctlP->address;
            curState = INITIALIZE;      // reset340() sets it to STOPPED;
            iotCondLog(LOG_RUN, "Received start at addr %o\n", curAddress);
            break;

        case EMU_CMD_RESUME:
            // If we were paused, the state remains the same as it was,
            // otherwise ignore.
            // The manual says this also clears the lp enable flag and the edge violation flags.
            if( isPaused )
            {
                emuClearFlags();
                lpEnabled = isPaused = false;
                iotCondLog(LOG_RUN, "Received resume while paused\n");
            }
            break;

        case EMU_CMD_PAUSE:
            isPaused = true;
            iotCondLog(LOG_RUN, "Received pause\n");
            break;

        case EMU_CMD_STOP:
            reset340();
            iotCondLog(LOG_RUN, "Received stop\n");
            break;
        }

        iotCondLog(LOG_CMD,"Got command %d\n", command);

        while( !isPaused && (curState != STOPPED) )
        {
            // We could be running in a continuous loop via JUMP, so check for any commands
            if( (command = get340Command(ctlP)) != EMU_CMD_NONE )
            {
                iotCondLog(LOG_CMD,"Got command %d while running\n", command);

                switch( command )
                {
                case EMU_CMD_EXIT:
                    sawExit = true;
                    curState = STOPPED;
                    continue;

                case EMU_CMD_STOP:
                    // A stop resets back to stopped and param mode
                    reset340();     // not sure this is strictly correct, but does no harm
                    continue;
                }
            }

            pendingDelay = 0;       // we accumuoate the total delay time for one cycle, wait at the end

            // Every command takes time to initialize
            if( curState == INITIALIZE )
            {
                pendingDelay = SETUP_TIME;
            }

            switch( curMode )
            {
            case PARAMETER:
                word = getWord(ctlP->pdp1P);
                curMode = MODE(word);
                iotCondLog(LOG_PARAM, "param word 0%o next mode 0%o\n", word, curMode);
                if( PARAM_STOP(word) )
                {
                    iotCondLog(LOG_PARAM, "param stop\n");
                    curState = STOPPED;
                    emuOrFlags(FLAG_STOP);
                    if( PARAM_STOP_INTERRUPT(word) )
                    {
                        initiateBreak(BRKCHAN);
                    }
                }
                else
                {
                    curState = INITIALIZE;
                    emuClearFlags();
                    twoCharsets = origCharsets; // we revert on each param instruction

                    if( PARAM_SCALE_CHANGE(word) )
                    {
                        // We keep the scale in pixels
                        curScale = 1 << PARAM_SCALE(word);
                    }

                    if( PARAM_INTENSITY_CHANGE(word) )
                    {
                        // The 340 supported 8 intensity levels, 0 off to 7 brightest.
                        // However, the Type 30 display only supported a limited set:
                        curIntensity = PARAM_INTENSITY(word);
                    }

                    if( PARAM_CHARSET_CHANGE(word) )
                    {
                        // Added functionality to enable/disable dual charsets on the fly.
                        // Not standard, but these bits weren't used.
                        twoCharsets = PARAM_CHARSETS(word)?true:false;
                    }

                    if( PARAM_LP_CHANGE(word) )
                    {
                        lpEnabled = PARAM_LP_ENABLE(word);
                        iotCondLog(LOG_LP, "lp enabled set to %d in parameter\n", lpEnabled);
                    }

                    iotCondLog(LOG_PARAM, "scale %d intensity %d lpon %d\n", curScale, curIntensity, lpEnabled);
                }
                break;

            case SLAVE:                        // 16 slave Type 343 terminals? Really? We limit to 8.
                word = getWord(ctlP->pdp1P);
                curMode = MODE(word);
                i = SLAVE_GROUP(word);
                if( i < NUMSLAVEGROUPS )
                {
                    i += i * 4;     // 4 monitors per group
                    for( x = 0; x < 4; ++x )
                    {
                        tmp = SLAVE_GET_FLAGS(word, x);
                        slaves[i + x].displayEnabled = (tmp & 1);
                        if( tmp & 4 )
                        {
                            slaves[i + x].lpEnabled = (tmp & 2);
                        }
                        iotCondLog(LOG_SLAVE,"Group %d slave %d flags %o\n", i, x+1, tmp);
                    }

                    slavesEnabled = true;
                }
                break;

            case POINT:
                // Point gets one word, an x or y coordinate
                curState = RUNNING;
                word = getWord(ctlP->pdp1P);
                newMode = MODE(word);
                iotCondLog(LOG_POINT, "point word 0%o next mode 0%o\n", word, newMode);

                if( POINT_VERTICAL(word) )
                {
                    curY = POINT_ADDRESS(word);
                    if( POINT_INTENSIFY(word) )
                    {
                        pendingDelay += 35000;           // 35 us positioning and draw delay
                    }
                    iotCondLog(LOG_POINT, "vertical 0%d\n", curY);
                }
                else
                {
                    curX = POINT_ADDRESS(word);
                    pendingDelay += 35000;               // 35 us delay always
                    iotCondLog(LOG_POINT, "horizontal 0%d\n", curX);
                }

                if( POINT_LP_CHANGE(word) )
                {
                    lpEnabled = POINT_LP_ENABLE(word);
                    iotCondLog(LOG_LP, "lp enabled set to %d in point\n", lpEnabled);
                }

                // Do we actually draw anything?
                if( POINT_INTENSIFY(word) )
                {
                    if( drawAndCheck(true, curX, curY, curIntensity) )
                    {
                        // FLAG_LP will have been set already
                        isPaused = true;
                    }
                }

                // Switching to something else
                if( newMode != curMode )
                {
                    curMode = newMode;
                    curState = INITIALIZE;
                }
                break;

            case VECTOR:
            case VCONTINUE:
                // Vcontinue goes until an edge violation
                // Linux scheduling really interferes with long vectors, specifically vcontinue.
                // No good solution yet, can only get a stable display by ignoring timing for vcontine.
                // Otherwise, defer the delay until the entire vector completes.
                if( curState == INITIALIZE )
                {
                    word = getWord(ctlP->pdp1P);
                    if( (curMode == VECTOR) && ESCAPE(word) )
                    {
                        sawEscape = true;
                        iotCondLog(LOG_VECTOR, "vector escape\n");
                    }

                    // Get the deltas, sign extend them
                    x = VECTOR_DX(word);
                    if( VECTOR_SIGN(x) )
                    {
                        x = -(x & 0177);
                    }

                    y = VECTOR_DY(word);
                    if( VECTOR_SIGN(y) )
                    {
                        y = -(y & 0177);
                    }

                    lastdX = x;     // needed for vec continue
                    lastdY = y;

                    iotCondLog(LOG_VECTOR, "vector%s initialize curx %d cury %d dx %d dy %d\n",
                        (curMode == VCONTINUE)?" continue":"",curX, curY, x, y);
                    // If we can't initialise, ignore it and continue
                    if( brmInitialize(&brmState, curX, curY, x, y, curScale, INTENSIFY(word)) )
                    {
                        curState = RUNNING;
                    }
                    else
                    {
                        iotCondLog(LOG_VECTOR, "vector%s brmInitialize failed\n", 
                            (curMode == VCONTINUE)?" continue":"");
                        emuOrFlags(FLAG_STOP);
                        curState = STOPPED;
                    }
                }
                else
                {
                    if( (status = brmNext(&brmState, &curX, &curY)) != BRMRUNNING )
                    {
                        // Do our delay at the end of each vector.
                        // The worst-case vector, corner-to-corner in vcontinue mode takes 2 milliseconds.
                        // The delay will happen when it completes, even though it uses repeated short vectors.
                        if( status == COMPLETED )
                        {
                            if( curMode == VCONTINUE )
                            {
                                // We stay in run mode, updating the start and end points
                                brmContinue(&brmState, curX, curY);
                            }
                            else
                            {
                                curState = INITIALIZE;     // back to fetching another vector
                            }
                            break;
                        }
                        else
                        {
                            // VCONTINUE will have caused an edge violation, go back to param mode.
                            // It does NOT cause a break.
                            if( curMode == VCONTINUE )
                            {
                                curMode = PARAMETER;
                                curState = INITIALIZE;
                            }
                            else
                            {
                                curState = STOPPED;
                                iotCondLog(LOG_BOUNDS, "vector edge violation x, y %d %d\n", curX, curY);
                                needBreak = true;
                            }
                        }

                        iotCondLog(LOG_VECTOR,"vector done, status %d, curX %d curY %d\n", status, curX, curY);
                    }
                    else if( brmState.draw )
                    {
                        pendingDelay += INTENSIFY(word)?1500:1000;

                        if( drawAndCheck(true, curX, curY, curIntensity) )
                        {
                            isPaused = true;
                        }
                    }
                }
                break;

            case INCREMENT:
                curState == RUNNING;            // not necessary, but do it for consistency
                word = getWord(ctlP->pdp1P);

                if( ESCAPE(word) )
                {
                    sawEscape = true;
                    iotCondLog(LOG_INCREMENT, "increment escape\n");
                }

                // The word contains 4, 4 bit fields, each moves up, down, left, or right, or diagonally
                // one dotSpacing location. The high bit determines visible/invisible.
                for( tmp = 12; tmp >= 0; tmp -= 4)
                {
                    if( (status = doIncrement(curScale, (word >> tmp) & 0xF)) != COMPLETED )
                    {
                        // Edge violation
                        curState = STOPPED;
                        needBreak = true;
                        break;
                    }
                    else
                    {
                        // Actually want to draw it
                        if( INTENSIFY(word) )
                        {
                            if( drawAndCheck(true, curX, curY, curIntensity) )
                            {
                                // Will take effect on the next word
                                isPaused = true;
                            }

                            pendingDelay += 1500;            // manual says 1.5 usec
                        }
                        else
                        {
                            pendingDelay += 1000;            // but only 1 usec if not showing a dot
                        }
                    }
                }

                // Don't lose the edge violation stop
                if( curState != STOPPED )
                {
                    curState = INITIALIZE;
                }
                break;

            case CHARACTER:
                if( curState == INITIALIZE )
                {
                    shiftState = 0;
                    curState = RUNNING;
                }

                word = getWord(ctlP->pdp1P);
                iotCondLog(LOG_CHARACTER,"Char got word %6o\n", word);
                for( i = 0; i < 3; ++i )
                {
                    tmp = (word & 0770000) >> 12;
                    word <<= 6;

                    if( (status = doCharacter(curScale, tmp)) != COMPLETED )
                    {
                        if( status == ESCAPE )
                        {
                            curMode = PARAMETER;
                            curState = INITIALIZE;
                            break;
                        }
                        else if( status == PAUSE )
                        {
                            // lp hit, but finish the current loop
                            isPaused = true;
                        }
                        else
                        {
                            // Edge violation
                            curState = STOPPED;
                            needBreak = true;
                            break;
                        }
                    }
                }
                break;

            case SUBROUTINE:
                word = getWord(ctlP->pdp1P);
                curMode = MODE(word);
                curState = INITIALIZE;
                iotCondLog(LOG_SUBROUTINE,"Subroutine op %d, mode %d\n", SUBROUTINE_OP(word), curMode);

                switch( SUBROUTINE_OP(word) )
                {
                case JUMP:
                    curAddress = SUBROUTINE_ADDR(word);    // 13 bit address, strange. So, bank 0 or 1 only.
                    iotCondLog(LOG_JUMP,"JUMP %d\n", curAddress);
                    break;

                case JUMPANDSAVE:                          // copy the next address to the save register, then jump
                    saveRegister = curAddress;             // already incremented by getWord()
                    saveActive = true;
                    curAddress = SUBROUTINE_ADDR(word);
                    iotCondLog(LOG_SAVE,"SAVE word %0o save addr %o, JUMP %o, mode %d\n",
                        word, saveRegister, curAddress, curMode);
                    break;

                case DEPOSITSAVEREGISTER:                   // a strange one
                    if( !saveActive )
                    {
                        // Do what? Assume use the last saved address, could bomb terribly if it's invalid
                        iotCondLog(LOG_SUBROUTINE,"Deposit and save, but save register is not active\n");
                    }

                    tmp = SUBROUTINE_ADDR(word);            // put a jump to saveReg and param mode in the address
                    ctlP->pdp1P->core[tmp] = PUT_SUBROUTINE_OP(JUMP) | PUTMODE(PARAMETER) | saveRegister;
                    iotCondLog(LOG_DEPOSIT,"DEPOSIT %o into %d\n", ctlP->pdp1P->core[tmp], tmp);
                    break;

                default:
                    iotCondLog(LOG_SUBROUTINE, "Invalid subroutine subop 0\n");
                    break;                          // ignre it
                }
                break;
            }

            // Delay for the accumulated time from the last operation
            pendingDelay = nanowait(pendingDelay);

            // The instruction completes, then escape is processed
            if( (curState != RUNNING) && sawEscape )
            {
                sawEscape = false;
                curMode = PARAMETER;        // This leaves the current mode, may return to a saved address
                curState = INITIALIZE;

                if( saveActive )
                {
                    iotCondLog(LOG_ESCAPE,"escape to %d\n", curAddress);
                    curAddress = saveRegister;
                    saveActive = false;
                }
                else
                {
                    iotCondLog(LOG_ESCAPE,"escape\n");
                }
            }
        }

#if LOG_TIMING
        if( startTime )
        {
            uint64_t delta = getNow() - startTime;
            iotCondLog(LOG_TIMING, "%d points in %d usec, min x,y %d,%d max x,y %d,%d\n",
                totalPoints, delta/1000, minX, minY, maxX, maxY);
            startTime = 0;
        }
#endif

        // An hp hit or edge violation always interrupts
        if( isPaused || needBreak )
        {
            initiateBreak(BRKCHAN);
            needBreak = false;
        }
        else
        {
            iotCondLog(LOG_CMD,"Stopped, responding done\n");
            ctlP->response = EMU_RESPONSE_DONE;
            emuOrFlags(FLAG_STOP);
            emuResponseSet(ctlP);
        }
    }

    iotCondLog(LOG_RUN, "exit seen, terminating thread\n");
    threadRunning = false;
    return(0);
}

// reset the current state to stopped, clear flags, etc.
void
reset340()
{
    curState = STOPPED;
    curMode = PARAMETER;
    isPaused = false;
    flags = 0;
    slavesEnabled = false;
    lpEnabled = false;
    memset(slaves, 0, sizeof(slaves));
}

// A binary rate multiplier implementation, needed for vectors.
// In order to minimize rounding errors, the integer values used during the processing are multiplied
// by a scale factor, essentially fixed-point pseudo-floating-point.
#define BRMSCALEFACTOR 20.0

// Initialize a brm, expects the origin in initialX and initialY, the realtive lenghts in dX and dY,
// with a step increment in step.
// Note that the coordinate system is always positve; the lower left corner is 0,0, upper right 1023,1023.
// If the data is invalid, such as no dX and dY or no step, return false, else true;
//
// Note that the dotSpacing is in pixels, not the raw 2 bit selector from the instruction.

bool
brmInitialize(BRMStateP stateP, int initialX, int initialY, int dX, int dY, int dotSpacing, bool draw)
{
int xSpan, ySpan;
float side, fx, fy;

    if( dotSpacing == 0 )
    {
        iotCondLog(LOG_BRM, "brm init dotSpacing 0\n");
        return(false);      // nothing to do
    }

    if( (dX == 0) && (dY == 0) )
    {
        iotCondLog(LOG_BRM, "brm init both deltas 0\n");
        return(false);      // nothing to do
    }

    stateP->startX =  initialX;
    stateP->startY =  initialY;
    stateP->deltaX = 0;
    stateP->deltaY = 0;
    stateP->plusX = (dX >= 0);
    stateP->plusY = (dY >= 0);

    // Just how far, not the direction
    xSpan = abs(dX);
    ySpan = abs(dY);

    // Figure out the rate multiplier
    // The largest delta is the primary control, one point will be generated for
    // every call for that one.
    // The smaller sets the rate multiplier for it.
    stateP->nPoints = 0;

    // Check for degenerate cases
    if( (xSpan == 0) || (ySpan == 0) )
    {
        if( dX == 0 )
        {
            stateP->nPoints = ySpan;
            stateP->xRate = 0;
            stateP->yRate = 1;
        }
        else
        {
            stateP->nPoints = xSpan;
            stateP->xRate = 1;
            stateP->yRate = 0;
        }
    }
    else
    {
        // Have to do it the hard way
        fx = (float)xSpan;
        fy = (float)ySpan;
        side = hypot(fx, fy);
        stateP->nPoints = (int)(side * BRMSCALEFACTOR);
        stateP->xRate = (int)((side / fx) * BRMSCALEFACTOR);
        stateP->yRate = (int)((side / fy) * BRMSCALEFACTOR);
    }

    stateP->dotSpacing = dotSpacing;
    stateP->draw = draw;
    stateP->curStep = 0;
    iotCondLog(LOG_BRM, "brmInit initial x %d xrate %d initial y %d yrate %d dotSpacing %d points %d\n",
        stateP->startX, stateP->xRate, stateP->startY, stateP->yRate, dotSpacing, stateP->nPoints);
    return(true);
}

// Reinitiialize a brm keeping the same x, y rates.
// This is used by vcontinue to do the next segment in its vector.
void
brmContinue(BRMStateP stateP, int nextX, int nextY)
{
    stateP->startX =  nextX;
    stateP->startY =  nextY;
    stateP->deltaX = 0;         // current delta offsets from starting point
    stateP->deltaY = 0;
    stateP->curStep = 0;
    iotCondLog(LOG_BRM, "brmContinue new x %d new y %d\n", stateP->startX, stateP->startY);
}

// Get the next point from the passed brm state.
// Return RUNNING if there was a point,COMPLETED if done or VIOLATION for an edge violation.
// If there was an edge violation, the offending axis coordinate will be set to its opposite edge.
Status
brmNext(BRMStateP stateP, int *xP, int *yP)
{
bool didOne;
Status brmStatus;

    iotCondLog(LOG_BRM, "brmNext deltaX %d deltaY %d\n", stateP->deltaX, stateP->deltaY);

    didOne = false;

    while( !didOne )
    {
        if( ++(stateP->curStep) > stateP->nPoints )
        {
            iotCondLog(LOG_BRM, "brmNext finished with deltaX %d deltayY %d\n", stateP->deltaX, stateP->deltaY);
            return(COMPLETED);
        }

        if( stateP->xRate > 0 )
        {
            if( (stateP->xRate == 1) || !(stateP->curStep % stateP->xRate) )
            {
                if( stateP->plusX )
                {
                    stateP->deltaX++;
                    iotCondLog(LOG_BRM, "brmNext increment x at step %d\n", stateP->curStep);
                }
                else
                {
                    stateP->deltaX--;
                    iotCondLog(LOG_BRM, "brmNext decrement x at step %d\n", stateP->curStep);
                }

                *xP = stateP->startX + (stateP->deltaX * stateP->dotSpacing);
                didOne = true;
            }
        }

        if( stateP->yRate > 0 )
        {
            if( (stateP->yRate == 1) || !(stateP->curStep % stateP->yRate) )
            {
                if( stateP->plusY )
                {
                    stateP->deltaY++;
                    iotCondLog(LOG_BRM, "brmNext increment y at step %d\n", stateP->curStep);
                }
                else
                {
                    stateP->deltaY--;
                    iotCondLog(LOG_BRM, "brmNext decrement y at step %d\n", stateP->curStep);
                }

                *yP = stateP->startY + (stateP->deltaY * stateP->dotSpacing);
                didOne = true;
            }
        }
    }

    // Check for edge violations
    if( checkBounds(*xP, *yP) )
    {
        if( flags & FLAG_HEDGE )
        {
            *xP = (*xP < 0)?1023:0;
        }

        if( flags & FLAG_VEDGE )
        {
            *yP = (*yP < 0)?1023:0;
        }

        brmStatus = EDGEVIOLATION;
        iotCondLog(LOG_BRM, "brmNext returning edge violation\n");
    }
    else
    {
        brmStatus = BRMRUNNING;
    }

    iotCondLog(LOG_BRM, "brmNext returning %d\n", brmStatus);
    return( brmStatus );
}

// Handle one 4 bit increment operation
// The bits are MOVEX, DOWN, MOVEY, LEFT
// Returns an edge violation or completion.
Status
doIncrement(int dotSpacing, int bits)
{
    iotCondLog(LOG_INCREMENT, "increment %b initial x, y %d %d dotSpacing %d\n", bits, curX, curY, dotSpacing);

    if( bits & MOVEX )
    {
        if( bits & LEFT )
        {
            curX -= dotSpacing;
        }
        else
        {
            curX += dotSpacing;
        }
    }

    if( bits & MOVEY )
    {
        if( bits & DOWN )
        {
            curY -= dotSpacing;
        }
        else
        {
            curY += dotSpacing;
        }
    }

    iotCondLog(LOG_INCREMENT, "final x, y %d %d\n", curX, curY);
    if( checkBounds(curX, curY) )
    {
        iotCondLog(LOG_BOUNDS, "increment edge violation x, y %d %d\n", curX, curY);
        return( EDGEVIOLATION );
    }

    return( COMPLETED );
}

/* 
 * Character set for the Type 342 character generator.
 * Shamelessly stolen from Phil Budne and Lars Brinkhoff from the simh version.
 * A character is represented by 5 bytes plus one flag byte that make up a 5x7 matrix.
 * Each byte contains a vertical stripe of the matrix.
 * The highest bit is top, lowest bit is unused.
 * THe first byte is leftmost column.
 */
static unsigned char charSet[128][6] = {
    { 0070, 0124, 0154, 0124, 0070, 0 },   // 00 blob
    { 0176, 0220, 0220, 0220, 0176, 0 },   // 01 A */
    { 0376, 0222, 0222, 0222, 0154, 0 },   // 02 B
    { 0174, 0202, 0202, 0202, 0104, 0 },   // 03 C
    { 0376, 0202, 0202, 0202, 0174, 0 },   // 04 D
    { 0376, 0222, 0222, 0222, 0222, 0 },   // 05 E
    { 0376, 0220, 0220, 0220, 0220, 0 },   // 06 F
    { 0174, 0202, 0222, 0222, 0134, 0 },   // 07 G
    { 0376, 0020, 0020, 0020, 0376, 0 },   // 10 H
    { 0000, 0202, 0376, 0202, 0000, 0 },   // 11 I
    { 0004, 0002, 0002, 0002, 0374, 0 },   // 12 J
    { 0376, 0020, 0050, 0104, 0202, 0 },   // 13 K
    { 0376, 0002, 0002, 0002, 0002, 0 },   // 14 L
    { 0376, 0100, 0040, 0100, 0376, 0 },   // 15 M
    { 0376, 0100, 0040, 0020, 0376, 0 },   // 16 N
    { 0174, 0202, 0202, 0202, 0174, 0 },   // 17 O
    { 0376, 0220, 0220, 0220, 0140, 0 },   // 20 P
    { 0174, 0202, 0212, 0206, 0176, 0 },   // 21 Q
    { 0376, 0220, 0230, 0224, 0142, 0 },   // 22 R
    { 0144, 0222, 0222, 0222, 0114, 0 },   // 23 S
    { 0200, 0200, 0376, 0200, 0200, 0 },   // 24 T
    { 0374, 0002, 0002, 0002, 0374, 0 },   // 25 U
    { 0370, 0004, 0002, 0004, 0370, 0 },   // 26 V
    { 0376, 0004, 0010, 0004, 0376, 0 },   // 27 W
    { 0202, 0104, 0070, 0104, 0202, 0 },   // 30 X
    { 0200, 0100, 0076, 0100, 0200, 0 },   // 31 Y
    { 0226, 0232, 0222, 0262, 0322, 0 },   // 32 Z
    { 0000, 0000, 0000, 0000, 0000, CH_LF },   // 33 LF
    { 0000, 0000, 0000, 0000, 0000, CH_CR },   // 34 CR
    { 0000, 0000, 0000, 0000, 0000, CH_UC },   // 35 HORIZ
    { 0000, 0000, 0000, 0000, 0000, CH_LC },   // 36 VERT
    { 0000, 0000, 0000, 0000, 0000, CH_ESC },  // 37 ESC
    { 0000, 0000, 0000, 0000, 0000, 0 },   // 40 space
    { 0000, 0000, 0372, 0000, 0000, 0 },   // 41 !
    { 0000, 0340, 0000, 0340, 0000, 0 },   // 42 "
    { 0050, 0376, 0050, 0376, 0050, 0 },   // 43 #
    { 0144, 0222, 0376, 0222, 0114, 0 },   // 44 $
    { 0306, 0310, 0220, 0246, 0306, 0 },   // 45 %
    { 0154, 0222, 0156, 0004, 0012, 0 },   // 46 &
    { 0000, 0000, 0300, 0340, 0000, 0 },   // 47 '
    { 0000, 0070, 0104, 0202, 0000, 0 },   // 50 ( Source: AI film 104
    { 0000, 0202, 0104, 0070, 0000, 0 },   // 51 ) Source: AI film 104
    { 0104, 0050, 0174, 0050, 0104, 0 },   // 52 * Source: AI film
    { 0020, 0020, 0174, 0020, 0020, 0 },   // 53 +
    { 0000, 0032, 0034, 0000, 0000, 0 },   // 54 , Source: AI film 104
    { 0020, 0020, 0020, 0020, 0020, 0 },   // 55 -
    { 0000, 0006, 0006, 0000, 0000, 0 },   // 56 .
    { 0004, 0010, 0020, 0040, 0100, 0 },   // 57 /
    { 0174, 0212, 0222, 0242, 0174, 0 },   // 60 0
    { 0000, 0102, 0376, 0002, 0000, 0 },   // 61 1
    { 0116, 0222, 0222, 0222, 0142, 0 },   // 62 2
    { 0104, 0202, 0222, 0222, 0154, 0 },   // 63 3
    { 0020, 0060, 0120, 0376, 0020, 0 },   // 64 4
    { 0344, 0222, 0222, 0222, 0214, 0 },   // 65 5
    { 0174, 0222, 0222, 0222, 0114, 0 },   // 66 6
    { 0306, 0210, 0220, 0240, 0300, 0 },   // 67 7
    { 0154, 0222, 0222, 0222, 0154, 0 },   // 70 8
    { 0144, 0222, 0222, 0222, 0174, 0 },   // 71 9
    { 0000, 0066, 0066, 0000, 0000, 0 },   // 72 :
    { 0000, 0332, 0334, 0000, 0000, 0 },   // 73 ; Source: consistent with ,
    { 0020, 0050, 0104, 0202, 0000, 0 },   // 74 <
    { 0050, 0050, 0050, 0050, 0050, 0 },   // 75 =
    { 0000, 0202, 0104, 0050, 0020, 0 },   // 76 >
    { 0100, 0200, 0236, 0220, 0140, 0 },   // 77 ?
// Lars Brinkhoff: I added new shapes from AI lab film footage, and
// from the Knight TV font.
    { 0070, 0124, 0154, 0124, 0070, 0 },   // 00 blob
    { 0034, 0042, 0042, 0074, 0002, 0 },   // 01 a Source: AI film 75
    { 0376, 0042, 0042, 0042, 0034, 0 },   // 02 b Source: AI film 75
    { 0034, 0042, 0042, 0042, 0024, 0 },   // 03 c
    { 0034, 0042, 0042, 0042, 0376, 0 },   // 04 d Source: AI film 75
    { 0034, 0052, 0052, 0052, 0030, 0 },   // 05 e Source: AI film 75
    { 0020, 0176, 0220, 0200, 0100, 0 },   // 06 f Source: Knight TV
    { 0160, 0212, 0212, 0212, 0174, CH_D },// 07 g Source: AI film 75
    { 0376, 0040, 0040, 0040, 0036, 0 },   // 10 h Source: AI film 75
    { 0000, 0042, 0276, 0002, 0000, 0 },   // 11 i Source: AI film 75
    { 0000, 0004, 0042, 0274, 0000, 0 },   // 12 j
    { 0376, 0010, 0030, 0044, 0002, 0 },   // 13 k Source: AI film 75
    { 0000, 0202, 0376, 0002, 0000, 0 },   // 14 l Source: AI film 75
    { 0076, 0040, 0036, 0040, 0036, 0 },   // 15 m
    { 0076, 0020, 0040, 0040, 0036, 0 },   // 16 n Source: AI film 75
    { 0034, 0042, 0042, 0042, 0034, 0 },   // 17 o Source: AI film 75
    { 0376, 0210, 0210, 0210, 0160, CH_D },// 20 p Source: Knight TV
    { 0160, 0210, 0210, 0210, 0376, CH_D },// 21 q Source: Knight TV
    { 0076, 0020, 0040, 0040, 0020, 0 },   // 22 r Source: AI film 75
    { 0022, 0052, 0052, 0052, 0044, 0 },   // 23 s
    { 0040, 0374, 0042, 0002, 0004, 0 },   // 24 t Source: AI film 75
    { 0074, 0002, 0002, 0004, 0076, 0 },   // 25 u Source: AI film 75
    { 0070, 0004, 0002, 0004, 0070, 0 },   // 26 v Source: Knight TV
    { 0074, 0002, 0034, 0002, 0074, 0 },   // 27 w Source: AI film 75
    { 0042, 0024, 0010, 0024, 0042, 0 },   // 30 x
    { 0360, 0012, 0012, 0012, 0374, CH_D },// 31 y Source: AI film 75
    { 0042, 0056, 0052, 0072, 0042, 0 },   // 32 z Source: Knight TV
    { 0000, 0000, 0000, 0000, 0000, CH_LF },   // 33 LF
    { 0000, 0000, 0000, 0000, 0000, CH_CR },   // 34 CR
    { 0000, 0000, 0000, 0000, 0000, CH_UC },   // 35 HORIZ
    { 0000, 0000, 0000, 0000, 0000, CH_LC },   // 36 VERT
    { 0000, 0000, 0000, 0000, 0000, CH_ESC },  // 37 ESC
    { 0000, 0000, 0000, 0000, 0000, 0 },   // 40 space
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 41 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 42 ???
    { 0100, 0200, 0100, 0040, 0100, 0 },   // 43 ~
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 44 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 45 ???
    { 0040, 0100, 0376, 0100, 0040, 0 },   // 46 up arrow
    { 0020, 0020, 0124, 0070, 0020, 0 },   // 47 left arrow
    { 0010, 0004, 0376, 0004, 0010, 0 },   // 50 down arrow
    { 0020, 0070, 0124, 0020, 0020, 0 },   // 51 right arrow
    { 0100, 0040, 0020, 0010, 0004, 0 },   // 52 backslash
    { 0000, 0376, 0202, 0202, 0000, 0 },   // 53 [
    { 0000, 0202, 0202, 0376, 0000, 0 },   // 54 ]
    { 0000, 0020, 0154, 0202, 0000, 0 },   // 55 {
    { 0000, 0202, 0154, 0020, 0000, 0 },   // 56 }
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 57 ???
    { 0002, 0002, 0002, 0002, 0002, 0 },   // 60 _
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 61 ???
    { 0000, 0000, 0376, 0000, 0000, 0 },   // 62 |
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 63 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 64 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 65 ???
    { 0000, 0200, 0100, 0040, 0000, 0 },  // 66 `
    { 0040, 0100, 0200, 0100, 0040, 0 },  // 67 ^
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 70 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 71 block?
    { 0000, 0000, 0000, 0000, 0000, CH_BS },  // 72 backspace
    { 0376, 0376, 0376, 0376, 0376, CH_SUB }, // 73 subscript
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 74 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 75 ???
    { 0376, 0376, 0376, 0376, 0376, 0 },   // 76 ???
    { 0376, 0376, 0376, 0376, 0376, CH_SUP } // 77 superscript
};

/*
 * Draw one character.
 * The 340 could have one or two character sets.
 * If only one was installed, then shifted characters were written vertically, top-to-bottom.
 * Cur x and y are updated on return.
 * Return COMPLETED if ok, PAUSE if there is an LP hit,  else another status.
 */
Status
doCharacter(int dotSpacing, unsigned char ch)
{
int x, y;
int xTmp, yTmp;
int flags;
int curChar;
bool sawHit;

    if( ch > 63 )
    {
        iotCondLog(LOG_CHARACTER,"Invalid character %o\n", ch);
        return(COMPLETED);      // invalid char, just stop
    }

    sawHit = false;
    curChar = ch | ((twoCharsets)?shiftState:0);
    iotCondLog(LOG_CHARACTER,"Mapped character %o\n", ch);

    // Each char has 5 data bytes plus one flag byte.
    flags = charSet[curChar][5];

    iotCondLog(LOG_CHARACTER,"curChar %o flags %o\n", curChar, flags);
    switch( flags )
    {
    case CH_LF:
        // Down one line dotSpacing
        curY -= CHARHEIGHT * dotSpacing;

        if( checkBounds(curX, curY) )
        {
            iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", curX, curY);
            return(EDGEVIOLATION);
        }
        return(COMPLETED);

    case CH_CR:
        curX = 0;
        return(COMPLETED);

    case CH_UC:
        // SHIFT IN (horizontal chars)
        // Uppercase characters and some special symbols.
        shiftState = 0;
        return(COMPLETED);

    case CH_LC:
        // SHIFT OUT (vertical chars)
        // unless two character sets are enabled, then lowercase characters and some more symbols.
        shiftState = 0100;
        return(COMPLETED);

    case CH_ESC:
        iotCondLog(LOG_CHARACTER,"escape\n");
        return(ESCAPE);          // all done

    case CH_NSPC:
        if( twoCharsets || (shiftState == 0) )
        {
            if( curX >= (CHARWIDTH * dotSpacing) )
            {
                curX -= CHARWIDTH * dotSpacing;     // non dotSpacing character
            }
        }
        else if( !twoCharsets && (shiftState != 0) )
        {
            if( curY <= 1023 - (CHARHEIGHT * dotSpacing) )
            {
                curY += CHARHEIGHT * dotSpacing;     // non dotSpacing character
            }
        }
        break;

    case CH_D:
        curY -= 2 * dotSpacing;                     // descender
        break;

    case CH_SUB:
        curY -= (CHARHEIGHT * dotSpacing) / 2;      // subscript

        if( checkBounds(curX, curY) )
        {
            iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", curX, curY);
            return(EDGEVIOLATION);
        }
        return(COMPLETED);

    case CH_SUP:
        curY += (CHARHEIGHT * dotSpacing) / 2;   // superscript

        if( checkBounds(curX, curY) )
        {
            iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", curX, curY);
            return(EDGEVIOLATION);
        }
        return(COMPLETED);

    default:
        break;
    }

    // Finally!
    // The Type 342 character generator didn't scan the display across every point in the 5x7 matrix.
    // Instead, it moved from each 'on' bit position directly to the next, only doing an intensify
    // if in visible mode.
    for( x = 0; x < 5; x++ )
    {
        // columns 0 to 4, left to right
        for( y = 0; y < 7; y++ )
        {
            // Rows 0 to 6, bottom to top.
            // The Type 342 character generator didn't scan the display across every point in the 5x7 matrix.
            // Instead, it moved from each 'on' bit position directly to the next.
            // So, the delay time is 1.5 usecs per drawm bit.
            if( charSet[curChar][x] & (2 << y) )
            {
                // Bit on, draw it
                xTmp = curX + (x * dotSpacing);
                yTmp = curY + (y * dotSpacing);
                if( checkBounds(xTmp, yTmp) )
                {
                    iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", xTmp, yTmp);
                    return(EDGEVIOLATION);
                }

                pendingDelay += 1500;
                if( drawAndCheck(true, xTmp, yTmp, type340Intensity(curIntensity)) )
                {
                    sawHit = true;
                }
            }
        }
    }

    if( twoCharsets || (shiftState == 0) )
    {
        curX += CHARWIDTH * dotSpacing;
    }
    else if( !twoCharsets && (shiftState != 0) )
    {
        curY -= CHARHEIGHT * dotSpacing;
    }

    if( flags == CH_BS )
    {
        if( twoCharsets || (shiftState == 0) )
        {
            curX -= CHARWIDTH * dotSpacing;  // backspace
        }
        else if( !twoCharsets && (shiftState != 0) )
        {
            curY += CHARHEIGHT * dotSpacing;  // backspace
        }

        if( checkBounds(curX, curY) )
        {
            iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", curX, curY);
            return(EDGEVIOLATION);
        }
    }
    else if( flags == CH_D )
    {
        curY += 2 * dotSpacing;     // undo descender
    }

    if( checkBounds(curX, curY) )
    {
        iotCondLog(LOG_BOUNDS, "character edge violation x, y %d %d\n", curX, curY);
        return(EDGEVIOLATION);
    }

    return((sawHit)?PAUSE:COMPLETED);
}

// Draw a point and check for a lightpen hit if it is enabled.
// Display 0 is always the primary display.
// Set the LP flag if a hit occurred.
// Even if the lp is not enabled, check for it so the lp queue is emptied.
// Return true if an lp hit occurred, else false.
bool
drawAndCheck(bool tryLightpen, int x, int y, int intensity)
{
int i;
bool gotLpHit;

    if( (x < 0) || (x > 1023) || (y < 0) || (y > 1023) )
    {
        iotCondLog(LOG_BOUNDS, "drawAndCheck edge violation x, y %d %d\n", x, y);
    }

    gotLpHit = false;

    for( i = 0; i < (slavesEnabled?NUMSLAVES+1:1); ++i )
    {
        if( (i == 0) || slaves[i-1].displayEnabled )
        {
            iotCondLog(LOG_DRAW, "draw display %d x,y %d,%d intensity %d\n", i, x, y, intensity);
            display(i, x, y, type340Intensity(intensity));

#if LOG_TIMING
            ++totalPoints;
            if( x > maxX )
            {
                maxX = x;
            }
            if( x < minX )
            {
                minX = x;
            }
            if( y > maxY )
            {
                maxY = y;
            }
            if( y < minY )
            {
                minY = y;
            }
#endif
            if( tryLightpen && !gotLpHit )
            {
                if( ((i == 0) && lpEnabled) || slaves[i-1].lpEnabled )
                {
                    if( (gotLpHit = checkLightpen(i, x, y)) )
                    {
                        lpX = x;
                        lpY = y;
                        emuOrFlags(FLAG_LP);
                        iotCondLog(LOG_LP,"lp hit screen %d at x %d y %d\n", i, x, y);
                    }
                }
            }
        }
    }

    return(gotLpHit);
}

// Get the next data word from memory using our global curAddr wrapping curAddr if needed.
Word
getWord(PDP1P pdp1P)
{
Word addr;
Word val;
HSCRequest request;
Word buffer[8];     // we only use 1, but just to be sure

    addr = curAddress++;
    curAddress %= MAXMEM;

    // We can use cycle-stealing, but the rescheduling interference can be annoying especially
    // for small transfers like the 340 does.
    // THREADED mode fakes the cycle stealing without having to synchronize with the emulator.
    // However, if you want to be more 'pure', regular hsc mode is useable.
    //request.mode = HSC_MODE_FROMMEM;
    request.mode = HSC_MODE_FROMMEM | HSC_MODE_THREADED;
    request.count = 1;
    request.memBank = (addr >> 12) & 017;
    request.memAddr = addr & 07777;
    request.fromBufferP = buffer;
    HSCexecute(chanP, &request);

    // it's possible to get an abort from the hsc, but that comes from a stop, will be handled by the IOT
    HSCwait(chanP);

    val = buffer[0];
    return(val);
}

// look for an edge violation.
// If there is one, set the appropriate flags, reset the offending axis and return true,
// else return false.
bool
checkBounds(int x, int y)
{
bool xViolation, yViolation;

    xViolation = yViolation = false;

    // Docs say edge is reset to the opposite side
    if( x < 0 )
    {
        curX = 1023;
        xViolation = true;
    }
    else if( x > 1023 )
    {
        curX = 0;
        xViolation = true;
    }

    if( y < 0 )
    {
        curY = 1023;
        yViolation = true;
    }
    else if( y > 1023 )
    {
        curY = 0;
        yViolation = true;
    }

    if( xViolation )
    {
        emuOrFlags(FLAG_STOP | FLAG_HEDGE);
    }

    if( yViolation )
    {
        emuOrFlags(FLAG_STOP | FLAG_VEDGE);
    }

    return( xViolation || yViolation );
}

// Get our configurations settings, can be called more than once.
void
configure()
{
ConfigurationSettingP settingP;

    iotCondLog(LOG_CONFIG, "340 emulator checking configuration\n");

    if( (settingP = findConfigurationSetting(getConfiguration(), "two340charsets")) )
    {
        origCharsets = twoCharsets = settingP->onOff;
        iotCondLog(LOG_CONFIG, "340 emulator dual charsets %s\n", (twoCharsets)?"enabled":"disabled");
    }
}

uint64_t
getNow()
{
uint64_t now;
struct timespec tm;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    now = tm.tv_nsec;
    now += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    return(now);
}

// do a nanodelay() if <= SPIN_LIMIT, else a nanopause().
// Always return 0.
int
nanowait(int ns)
{
    if( ns <= 0 )
    {
        return(0);
    }
    else if( ns <= SPIN_LIMIT )
    {
        nanodelay(ns);
    }
    else
    {
        nanopause(ns);
    }

    return(0);
}

// Wait ns nanoseconds in a spinloop so we don't reschedule while drawing.
// Always return 0.
int
nanodelay(int ns)
{
struct timespec tm;
uint64_t startTime;
uint64_t now;

    clock_gettime( CLOCK_MONOTONIC, &tm );
    startTime = tm.tv_nsec;
    startTime += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;

    for( now = startTime; (now - startTime) < ns; )
    {
        clock_gettime( CLOCK_MONOTONIC, &tm );
        now = tm.tv_nsec;
        now += (uint64_t)tm.tv_sec * 1000 * 1000 * 1000;
    }

    return(0);
}

// Wait ns nanoseconds, allow rescheduling.
// This usually means significantly longer than what is requested.
// Always return 0.
int
nanopause(int ns)
{
struct timespec tm;
    
    tm.tv_sec = 0;
    tm.tv_nsec = ns;
    nanosleep(&tm, 0);

    return(0);
}

// See if there is a pending command.
// Called from the 340 emulator side.
// If so, return it.
// Otherwise,return NONE.
int
get340Command(EmuControlP ctlP)
{
int command;

    if( !(ctlP->commandSent) )
    {
        command = EMU_CMD_NONE;
    }
    else
    {
        command = ctlP->command;
        ctlP->responseSent = ctlP->commandSent = false;
    }

    return(command);
}

// See if there is a pending response.
// Called from the IOT side.
// If so, return it.
// Otherwise, return NONE.
int
get340Response(EmuControlP ctlP)
{
int resp;

    if( ctlP->responseSent )
    {
        resp = ctlP->response;
        ctlP->responseSent = false;
    }
    else
    {
        resp = EMU_RESPONSE_NONE;
    }

    return(resp);
}
