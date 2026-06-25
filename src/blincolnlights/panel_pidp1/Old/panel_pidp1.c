/*
 * This is the hardware panel driver for the pidp-1.
 * It uses data set by the emulator in a shared memory segment to update the panel lights
 * and to read the switches.
 *
 * Original version: Angelo Papenhoff (aap)
 * Modified by: Bill Ezell (wje) to reduce load and improve light behavior
 *
 * wje 26-Jan-26 - initial work, reformat (sorry), add timing measurement, tweak some delays and counts
 * wje 2-Jun-26 - increase usleep in switch read from 10 usecs back to 20 usecs
 * wje 12-Jun-26 - documentation pass and defensive parenthesization, no logic changes,
 *                 in preparation for a possible future refactor
 *
*/
#include "common.h"
#include "pinctrl/gpiolib.h"
#include "panel_pidp1.h"
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

// define this to get the loop timing for the 2 main loops
//#define TIMINGS

// GPIO pin numbers for the 4 ADDR select lines. These select which of the 16 possible
// "rows" (lamp groups on output, switch registers on input) is currently active on
// the COLUMNS bus.
int ADDR[] = {4, 17, 27, 22};

// GPIO pin numbers for the 18 COLUMN lines, one per bit of the selected row.
// On output these drive lamp segments; on input they read switch positions.
int COLUMNS[] = {26, 19, 13, 6, 5, 11, 9, 10, 18, 23, 24, 25, 8, 7, 12, 16, 20, 21 };

// Per-lamp brightness state (0-31, used as a phase-count threshold by lightRow())
// for the 10 rows x 18 columns of front-panel lamps, plus a pointer to the shared
// memory Panel struct used to exchange switch/lamp data with the emulator.
typedef struct PanelLamps PanelLamps;
struct PanelLamps
{
    u8 lamps[10][18];
    Panel *p;
};

// Switch all 18 COLUMN GPIO pins to inputs, for reading switch positions.
void
inRow(void)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_INPUT);
    }
}

// Switch all 18 COLUMN GPIO pins to outputs, for driving lamps.
void
outRow(void)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_OUTPUT);
    }
}

// Drive a single GPIO pin high or low.
void setPin(int p, int val)
{
    gpio_set_drive(p, val ? DRIVE_HIGH : DRIVE_LOW);
}

// Read the current level of a single GPIO pin.
int getPin(int p)
{
    return(gpio_get_level(p));
}

// Drive the 18 COLUMN pins from the low 18 bits of l, one bit per column,
// bit 0 -> COLUMNS[0], bit 1 -> COLUMNS[1], etc.
void
setRow(int l)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        setPin(COLUMNS[i], (l >> i) & 1);
    }
}

// Drive the 4 ADDR select pins from the low 4 bits of a, selecting which
// row of lamps/switches is connected to the COLUMNS bus.
void
setAddr(int a)
{
    for(int i = 0; i < nelem(ADDR); i++)
    {
        setPin(ADDR[i], (a >> i) & 1);
    }
}

// pwmtable[32][31]: precomputed phase-on patterns for each of the 32 possible
// brightness levels (currently unused by lightRow(), which instead uses a simple
// phase-count threshold via phase_delays[]; kept here for reference/future use).
#include "pwmtab.inc"

// Busy-wait for at least dt nanoseconds. Used where nanosleep()'s scheduling
// jitter would be too coarse. resolution ~50-150ns
void
xsleep(u64 dt)
{
    u64 t1 = gettime();
    u64 t2 = gettime();

    while(t1 + dt > t2)
    {
        t2 = gettime();
    }
}

// calculate exponential delays for every phase
// this could be done a lot better...
// Precompute the per-phase delay (in ns) for each of the 31 PWM phases used by
// lightRow(): phase_delays[i] = base^i * 30, with base = 1.3. Later phases get
// progressively longer delays, which (combined with the phase-count threshold in
// lightRow) approximates a perceptually-linear brightness ramp.
u32 phase_delays[31];
void
init_delays(void)
{
    float base = 1.3f;

    for(int i = 0; i < 31; i++)
    {
        phase_delays[i] = pow(base, i) * 30;
    }
}

// Light one row (address a) of 18 lamps using the per-lamp brightness values in l[]
// (0-31, one per column). For each of 31 phases, a column's lamp is held on while
// phase < l[i] and turned off once phase reaches l[i] -- so higher brightness values
// keep the lamp lit for more (and longer-delayed) phases, giving a PWM-like dimming
// effect. After all phases, the row is blanked and ADDR is parked at 8 (idle/switch
// row) before returning.
void
lightRow(int a, u8 *l)
{
    setRow(~0);
    setAddr(a);
    usleep(100);

    for(int phase = 0; phase < 31; phase++)
    {
        for(int i = 0; i < nelem(COLUMNS); i++)
        {
            setPin(COLUMNS[i], !(phase < l[i]));
        }

// The following used to be nsleep(1) followed by xsleep(3000), with the comment 'allowing syscalls takes too long'.
// But, nsleep() calls nanosleep(), which allows a reschedule. So, the comment made no sense.
// Replaced with just nsleep().
        nsleep(phase_delays[phase]);
    }

    setRow(~0);
    setAddr(8);
    usleep(100);
}

// Read one row (address a) of 18 switches and return them packed into the low 18
// bits of the result, one bit per column. Switches read active-low: a high pin level
// means the switch is "off", so that bit is cleared in sw (sw starts as all-1s).
// ADDR is parked at 8 (idle row) before returning.
u32
readRow(int a)
{
    setAddr(a);
    usleep(20);
    int sw = 0777777;

    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        if(getPin(COLUMNS[i]))
        {
            sw &= ~(1 << i);
        }
    }

    setAddr(8);
    usleep(100);
    return(sw);
}

// Drive all 10 front-panel lamp rows from the current brightness values in p->lamps[].
// Rows 0-6 are the main control panel; rows 12-14 are the I/O panel (addresses 7-11
// are unused/skipped). COLUMNS is switched to output first via outRow().
void
setLights(PanelLamps *p)
{
    outRow();
    lightRow(0, p->lamps[0]);
    lightRow(1, p->lamps[1]);
    lightRow(2, p->lamps[2]);
    lightRow(3, p->lamps[3]);
    lightRow(4, p->lamps[4]);
    lightRow(5, p->lamps[5]);
    lightRow(6, p->lamps[6]);

    // IO panel
    lightRow(12, p->lamps[7]);
    lightRow(13, p->lamps[8]);
    lightRow(14, p->lamps[9]);
}

// Read one of the 4 switch registers (rows 8-11) per call, cycling round-robin via
// the static 'cycle' counter, and store the result into the corresponding sw0-sw3
// field of the shared Panel struct (via pointer arithmetic on &p->sw0). COLUMNS is
// switched to input first via inRow(). One register is read per call so that the
// cost of reading switches is spread across multiple panelthread iterations.
void
readSwitches(Panel *p)
{
    static u32 cycle = 0;

    inRow();
    int i = (cycle++) % 4;
    (&p->sw0)[i] = readRow(8 + i);
}

// Accumulate, into on[0..17], a 1 for each bit of 'bits' that is set -- used by
// lampthread() to tally how many of NSAMPLES samples had each lamp bit on.
void
countRow(u32 *on, u32 bits)
{
    for(int i = 0; i < 18; i++)
    {
        on[i] += (bits >> i) & 1;
    }
}

// Decay-filter tuning constants used by lampthread(). 'fall' is the per-second decay
// factor applied when a lamp's measured "on" fraction is decreasing (dimming);
// 'rise' is applied when it is increasing (brightening) -- rise is much smaller than
// fall so lamps brighten quickly but dim more gradually. 'power' is a gamma applied
// to the filtered intensity before mapping to a 0-31.5 brightness value.
//float fall = 0.995f;
float fall = 0.990f;
//float rise = 0.012f;
//float rise = 0.040f;
float rise = 0.030f;
float power = 1.0f;

// Linear-interpolate x from the range [x1,x2] to the range [y1,y2], then clamp the
// result to [y1,y2] (handles both y1<y2 and y1>y2 orderings).
float map(float x, float x1, float x2, float y1, float y2)
{
    float t = (((x - x1) / (x2 - x1)) * (y2 - y1)) + y1;
    return(t < y1 ? y1 : (t > y2 ? y2 : t));
}

#ifdef TIMINGS
u64 decayDelta, decayLoopCount;
#endif

// The following 2 values greatly affect processor loading.
// The original did a non-rescheduling delay which really chewed up processing time.
// Switching to a usleep lets a reschedule occur which works fine and dramatically reduces cpu load.
//
// Setting NSAMPLES to 1K and SAMPLEDELAY to 3 usecs works well.

// The number of times the light array from shared mem is scanned per cycle, originally 10K
#define NSAMPLES 1000

// The delay between each light array scan, usecs
#define SAMPLEDELAY 3

// Background thread: repeatedly samples the shared-memory lamp bits NSAMPLES times
// (with a short delay between samples) to estimate the duty cycle (fraction of time
// on) of each of the 10x18 lamp bits, then runs that duty-cycle estimate through an
// asymmetric exponential decay filter (separate rise/fall rates) and a gamma curve
// to produce the smoothed 0-31.5 brightness values consumed by lightRow() via
// p->lamps[][]. This sampling/decay loop is the dominant CPU consumer of the panel
// driver; NSAMPLES and SAMPLEDELAY (below) are the primary tuning knobs.
void *
lampthread(void *arg)
{
#ifdef TIMINGS
    u64 lastTime, currentTime;
#endif

PanelLamps *p = (PanelLamps*)arg;
Panel cur;
u32 on[10][18];
float intensity[10][18];
u64 now, prev;
float dt;

    memset(intensity, 0, sizeof(intensity));
#ifdef TIMINGS
    lastTime = now = gettime();
#else
    now = gettime();
#endif

    for(;;)
    {
        memset(on, 0, sizeof(on));

        // Sample the shared lamp-bit registers NSAMPLES times, with a SAMPLEDELAY
        // (3us) sleep between samples so the OS can reschedule other threads. Each
        // sample tallies, per lamp bit, whether it was on (countRow accumulates
        // into on[row][col]).
        for(int i = 0; i < NSAMPLES; i++)
        {
            cur = *p->p;
            countRow(on[0], cur.lights0);
            countRow(on[1], cur.lights1);
            countRow(on[2], cur.lights2);
            countRow(on[3], cur.lights3);
            countRow(on[4], cur.lights4);
            countRow(on[5], cur.lights5);
            countRow(on[6], cur.lights6);
            countRow(on[7], cur.lights7);
            countRow(on[8], cur.lights8);
            countRow(on[9], cur.lights9);
            usleep(3);
        }

        // dt = elapsed wall-clock time (seconds) since the previous decay update,
        // used to scale the exponential decay/rise rates below.
        prev = now;
        now = gettime();
        dt = (now - prev) / (1000.0f * 1000.0f);

        for(int i = 0; i < 10; i++)
        {
            for(int j = 0; j < 18; j++)
            {
                // targ = measured duty cycle (0.0-1.0) for this lamp bit over the
                // NSAMPLES samples just taken.
                float targ = (float)on[i][j] / NSAMPLES;

                if(targ >= intensity[i][j])
                {
                    // Lamp is brightening: move intensity toward targ using the
                    // (faster) rise rate, scaled by elapsed time dt.
                    float t = powf(1.0f - rise, dt);
                    intensity[i][j] = (intensity[i][j] * t) + (targ * (1 - t));
                }
                else
                {
                    // Lamp is dimming: move intensity toward targ using the
                    // (slower) fall rate, scaled by elapsed time dt.
                    float t = powf(fall, dt);
                    intensity[i][j] = (intensity[i][j] * t) + (targ * (1 - t));
                }

                // Apply gamma ('power'), then map the 0.1-1.0 intensity range to a
                // 0-31.5 brightness value for lightRow()'s phase-count threshold.
                float l = intensity[i][j];
                int in = map(powf(l, power), 0.1f, 1.0f, 0.0f, 31.5f);
                p->lamps[i][j] = in;
            }
        }

#ifdef TIMINGS
        currentTime = gettime();
        decayDelta += (currentTime - lastTime) / 1000;  // just usecs
        lastTime = currentTime;
        ++decayLoopCount;
#endif
    }
}

volatile int doexit;

// Main panel thread, run at SCHED_FIFO real-time priority. Spawns lampthread() to
// compute lamp brightness in the background, then loops driving the lamp rows
// (setLights) and reading one switch register per iteration (readSwitches) until
// doexit is set by sighandler(), at which point GPIO is parked and the process
// exits.
void*
panelthread(void *arg)
{
#ifdef TIMINGS
    u64 startTime, lastTime, now, delta, elapsed, loopCount;
#endif

pthread_t th;
PanelLamps panel;
struct sched_param sp;

    memset(&panel, 0, sizeof(panel));
    panel.p = (Panel*)arg;
    pthread_create(&th, nil, lampthread, &panel);

    sp.sched_priority = 99;  // not high, just above the minimum of 1
    int rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    printf("realtime thread: %s\n", rt ? "yes" : "no");

    init_delays();

#ifdef TIMINGS
    startTime = lastTime = gettime();
    delta = loopCount = 0;
#endif

    while(!doexit)
    {
        setLights(&panel);
        readSwitches(panel.p);

#ifdef TIMINGS
        now = gettime();
        delta += (now - lastTime) / 1000;   // just usecs
        lastTime = now;
        ++loopCount;
#endif
    }

#ifdef TIMINGS
    elapsed = (gettime() - startTime) / 1000;   // just usecs
    delta /= loopCount; // now per-loop avg
    printf("Avg main loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
        delta, loopCount, elapsed, ((float)(loopCount * delta) / (float)elapsed) * 100.0);

    decayDelta /= decayLoopCount; // now per-loop avg
    printf("Avg decay loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
        decayDelta, decayLoopCount, elapsed,
        ((float)(decayDelta * decayLoopCount) / (float)elapsed) * 100.0);
#endif

    setAddr(8);
    inRow();
    exit(0);
}

// SIGINT/SIGTERM handler: requests a clean shutdown of panelthread()'s main loop.
void
sighandler(int sig)
{
    doexit = 1;
}

// Initialize the GPIO subsystem: map GPIO registers, configure the 4 ADDR pins as
// outputs and the 18 COLUMN pins with pull-ups, set the bus to input/idle, and
// install signal handlers for clean shutdown. Returns 0 on success, 1 on failure.
int
initGPIO(void)
{
    int ngpio = gpiolib_init();

    if(ngpio <= 0)
    {
        return(1);
    }

    if(gpiolib_mmap())
    {
        return(1);
    }

    for(int i = 0; i < nelem(ADDR); i++)
    {
        gpio_set_fsel(ADDR[i], GPIO_FSEL_OUTPUT);
    }

    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_pull(COLUMNS[i], PULL_UP);
    }

    inRow();
    setAddr(8);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    return(0);
}

// Create/attach the shared Panel segment, initialize GPIO, then run the panel
// thread (which never returns under normal operation).
int
main(int argc, char *argv[])
{
    Panel *p;

    p = createseg("/tmp/pdp1_panel", sizeof(Panel));

    if(p == nil)
    {
        return(1);
    }

    if(initGPIO())
    {
        return(1);
    }

    panelthread(p);

    return(0);      // can't happen
}
