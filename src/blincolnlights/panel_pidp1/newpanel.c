/*
 * New hardware panel driver for the pidp-1 -- rework of panel_pidp1.c.
 *
 * Like panel_pidp1.c, this uses data set by the emulator in a shared memory
 * segment to update the panel lights and to read the switches. The
 * difference is in how lamp brightness is derived:
 *
 * panel_pidp1.c's lampthread() repeatedly sampled the raw lights0-lights9
 * bits (NSAMPLES=1000 times, 3us apart) and ran the result through an
 * asymmetric exponential decay filter to estimate each lamp's duty cycle.
 * That sampling loop was the dominant CPU consumer of the panel driver.
 * Additionally, the exponential attack and decay was pointless, the human eye can't
 * really detect the short interval. It instead integrates over a significantly longer period,
 * Bloch's law.
 *
 * Here, the emulator tallies the on stat, once per 5us emulated cycle by incrementing panel->pwmcount[][].
 * This driver just periodically reads and resets those counts and uses them directly as the 0-31 PWM
 * phase-count threshold for lightRow() after applying a global brightness scaling factor (lamp_dim).
 * There is no per-lamp floating point decay filter now.
 *
 * wje 14-Jun-26 - new implementation based on panel_pidp1.c:
 *    replace lampthread's NSAMPLES sampling/decay filter with
 *    a periodic read+reset of panel->pwmcount[][]; add global
 *    lamp_dim brightness scaling factor
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

// Global brightness scaling ("dimmer") factor, applied to every lamp's 0-31
// PWM count before it's used by lightRow(). 1.0 = full brightness (same as
// the original driver's maximum). Users have reported the panel is too
// bright at full brightness; lower values (e.g. 0.5) dim the whole panel.
// This is just a plain initialized global for now -- the intent is for it
// to eventually be set from a configuration file at startup.
float lamp_dim = 1.0f;

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

#ifdef TIMINGS
u64 pwmDelta, pwmLoopCount;
#endif

// The period between pwmthread updates, in microseconds. Chosen to match 31
// emulated cycles (31 * 5us = 155us), so that panel->pwmcount[][] values
// (incremented at most once per cycle, in panel1.c's updatelights()) land
// directly in the 0-31 range expected by lightRow(), requiring no scaling
// beyond the lamp_dim factor below.
#define PWM_PERIOD_US 155

// Background thread: every PWM_PERIOD_US, read and reset
// panel->pwmcount[row][col] for each of the 10x18 lamp bits, scale by
// lamp_dim, clamp to 0-31, and store into p->lamps[row][col] for lightRow()
// to use as its phase-count threshold. This replaces panel_pidp1.c's
// lampthread (NSAMPLES sampling + exponential decay filter).
void *
pwmthread(void *arg)
{
#ifdef TIMINGS
    u64 lastTime, currentTime;
#endif

PanelLamps *p = (PanelLamps*)arg;
Panel *panel = p->p;

#ifdef TIMINGS
    lastTime = gettime();
#endif

    for(;;)
    {
        usleep(PWM_PERIOD_US);

        for(int i = 0; i < 10; i++)
        {
            for(int j = 0; j < 18; j++)
            {
                // Read this lamp's "on" tally for the last period and reset
                // it for the next one. Not synchronized against pdp1's
                // concurrent increments (see panel_pidp1.h) -- worst case
                // an increment is occasionally lost or counted in the next
                // period, which is not visible at this update rate.
                u8 count = panel->pwmcount[i][j];
                panel->pwmcount[i][j] = 0;

                int in = (int)((float)count * lamp_dim);
                if(in < 0)
                {
                    in = 0;
                }
                if(in > 31)
                {
                    in = 31;
                }
                p->lamps[i][j] = in;
            }
        }

#ifdef TIMINGS
        currentTime = gettime();
        pwmDelta += (currentTime - lastTime) / 1000;  // just usecs
        lastTime = currentTime;
        ++pwmLoopCount;
#endif
    }
}

volatile int doexit;

// Main panel thread, run at SCHED_FIFO real-time priority. Spawns pwmthread()
// to periodically turn panel->pwmcount[][] into lamp brightness values, then
// loops driving the lamp rows (setLights) and reading one switch register
// per iteration (readSwitches) until doexit is set by sighandler(), at which
// point GPIO is parked and the process exits.
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
    pthread_create(&th, nil, pwmthread, &panel);

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

    pwmDelta /= pwmLoopCount; // now per-loop avg
    printf("Avg pwm update loop time %lu usec over %lu cycles (target %d usec).\n",
        pwmDelta, pwmLoopCount, PWM_PERIOD_US);
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
