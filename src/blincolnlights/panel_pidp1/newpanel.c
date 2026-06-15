/*
 * New hardware panel driver for the pidp-1 -- rework of panel_pidp1.c.
 *
 * Like panel_pidp1.c, this uses data set by the emulator in a shared memory
 * segment to update the panel lights and to read the switches. The
 * difference is in how light brightness is derived:
 *
 * panel_pidp1.c's lightthread() repeatedly sampled the raw lights0-lights9
 * bits (NSAMPLES=1000 times, 3us apart) and ran the result through an
 * asymmetric exponential decay filter to estimate each light's duty cycle.
 * That sampling loop was the dominant CPU consumer of the panel driver.
 * Additionally, the exponential attack and decay was pointless, the human eye can't
 * really detect the short interval. It instead integrates over a significantly longer period,
 * Bloch's law.
 *
 * Here, the emulator tallies the on state once per 5us emulated cycle by incrementing panel->pwmcount[][].
 * This driver periodically reads and resets those counts and uses them directly as the PWM
 * phase-count threshold for lightRow() after applying a global brightness scaling factor (dimmingFactor).
 * There is no per-light floating point decay filter now.
 *
 * wje 14-Jun-26 - new implementation based on panel_pidp1.c:
 *    replace lightthread's NSAMPLES sampling/decay filter with
 *    a periodic read+reset of panel->pwmcount[][]; add global
 *    dimmingFactor brightness scaling factor
 * wje 14-Jun-26 -  reduce PWM phase count from 31 to NLEVELS (16)
 *    and lengthen each phase (init_delays()) so a full row scan takes ~1ms
 *    instead of ~340us, and increase PWM_PERIOD_US from 155 to 1000us. Both
 *    changes make the driver more tolerant of ordinary (non-realtime) scheduling
 *    jitter, with the goal of dropping the SCHED_FIFO/cap_sys_nice
 *    requirement in install.sh.
 */
#include "common.h"
#include "configuration.h"
#include "pinctrl/gpiolib.h"
#include "panel_pidp1.h"
#include <math.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

// Define this to get the loop timing for the 2 main loops
//#define TIMINGS

#define CONFIG_FILE "/opy/pidp1-mods/pidp1.config"

// Number of PWM brightness levels, also the phases used by lightRow)), 0-(NLEVELS-1).
//
// This setting directly affects light flicker caused by the linux scheduler.
// The original panel_pidp1 used a 31-phase scheme, the shortest phase delay and the full 10-row scan were
// both well within typical non-realtime scheduling jitter, so jitter was a large fraction of every
// phase and showed up as visible flicker, hence the need to set realtime priority on it.
// With 16 phases scaled so a full row scan takes ~1ms, a full 10 row pass ~10mx each phase is on the order of
// microseconds to hundreds of microseconds -- large enough that normal
// scheduler jitter is a small fraction of each phase and isn't visible.
// The refresh rate is approximately 100 Hz.
#define NLEVELS 16

// The target period between pwmthread updates, in microseconds.
//
// Pwmcount[][] is a u16 and at the pdp1's nominal 5us/cycle 1000us corresponds to ~200 cycles,
// giving pwmthread a much longer integration window.
// This both reduces pwmthread's wakeup rate (lower CPU load) and combined with the longer lightRow()
// phase delays above, makes the whole pipeline far less sensitive to non-realtime scheduling jitter.
// Pwmthread measures the actual number of elapsed cycles via panel->cyclecount and scales
// counts accordingly (see below) rather than assuming a fixed cycle count.
#define PWM_PERIOD_US 1000

#define PWM_BASE 1.3f
#define PWM_SCALE 4600.0f

typedef Panel *PanelP;      // Panel is in the panel_pidp1.h include file, it's what's in shared memory.

// Per-light brightness state used as a phase-count threshold by lightRow()
// for the 10 rows x 18 columns of front-panel lights, plus a pointer to the shared
// memory Panel struct used to exchange switch/light data with the emulator.
struct PanelLights
{
    u8 lights[10][18];
    PanelP p;
};

typedef struct PanelLights PanelLights, *PanelLightsP;

bool setPriority = true;

// Global brightness scaling ("dimmer") factor, applied to every light's
// PWM count before it's used by lightRow(). 1.0 = full brightness.
float dimmingFactor = 1.0f;

// Table of precomputed phase delays
u32 phase_delays[NLEVELS];

// GPIO pin numbers for the 4 ADDR select lines. These select which of the 16 possible
// "rows" (light groups on output, switch registers on input) is currently active on
// the COLUMNS bus.
int ADDR[] = {4, 17, 27, 22};

// GPIO pin numbers for the 18 COLUMN lines, one per bit of the selected row.
// On output these drive light segments; on input they read switch positions.
int COLUMNS[] = {26, 19, 13, 6, 5, 11, 9, 10, 18, 23, 24, 25, 8, 7, 12, 16, 20, 21 };

void loadConfig();
extern ConfigurationP loadConfigFile(char *filenameP);

// Switch all 18 COLUMN GPIO pins to inputs, for reading switch positions.
void
inRow(void)
{
    for(int i = 0; i < nelem(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_INPUT);
    }
}

// Switch all 18 COLUMN GPIO pins to outputs, for driving lights.
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
// row of lights/switches is connected to the COLUMNS bus.
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

// Precompute the per-phase delay in ns for each of the NLEVELS PWM phases
// used by lightRow(): phase_delays[i] = base^i * scale, with base = 1.3 and
// scale chosen so a full row scan (sum of all phase delays) takes ~1ms.
// Later phases get progressively longer delays, which,combined with the
// phase-count threshold in lightRow, approximates a perceptually-linear brightness ramp.
void
init_delays(void)
{
float base = PWM_BASE;
float scale = PWM_SCALE;

    for(int i = 0; i < NLEVELS; i++)
    {
        phase_delays[i] = pow(base, i) * scale;
    }
}

// Light one row using the per-light brightness values.
// For each of NLEVELS phases, a column's light is held on while
// phase < l[i] and turned off once phase reaches l[i] so higher brightness values
// keep the light lit for more phases, giving a PWM-like intensity effect.
// After all phases, the row is blanked and ADDR is parked at 8 (idle/switch // row) before returning.
void
lightRow(int row, u8 *l)
{
    setRow(~0);
    setAddr(row);
    usleep(100);

    for(int phase = 0; phase < NLEVELS; phase++)
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

// Read one row of 18 switches and return them packed into the low 18 bits of the result, one bit per column.
// Switches read active-low; a high pin level means the switch is "off", so that bit is cleared in sw
// which starts as as all-1s).
// ADDR is parked at 8 (idle row) before returning.
u32
readRow(int row)
{
    setAddr(row);
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

// Drive all 10 front-panel light rows from the current brightness values in p->lights[].
// Rows 0-6 are the main control panel; rows 12-14 are the I/O panel, addresses 7-11 are unused.
// COLUMNS is switched to output first via outRow().
//
// p->lights[][] is updated concurrently by pwmthread roughly every/ PWM_PERIOD_US (~1ms),
// while a full pass here takes ~10ms (10 rows * ~1ms/row);
// Those two rates aren't commensurate, so reading p->lights[][] live would let
// a row's brightness values change mid-scan, with the phase of that change
// drifting across passes, visible as a slow "wave" of brightness moving down the rows.
// Snapshot p->lights[][] once per pass instead, so every row in
// this pass is driven from a single, internally-consistent set of values.
void
setLights(PanelLightsP lightsP)
{
    u8 lights[10][18];

    memcpy(lights, lightsP->lights, sizeof(lights));

    outRow();
    lightRow(0, lights[0]);
    lightRow(1, lights[1]);
    lightRow(2, lights[2]);
    lightRow(3, lights[3]);
    lightRow(4, lights[4]);
    lightRow(5, lights[5]);
    lightRow(6, lights[6]);

    // IO panel
    lightRow(12, lights[7]);
    lightRow(13, lights[8]);
    lightRow(14, lights[9]);
}

// Read one of the 4 switch registers (rows 8-11) per call, cycling round-robin via
// the static 'cycle' counter, and store the result into the corresponding sw0-sw3
// field of the shared Panel struct. COLUMNS is switched to input first via inRow().
// One register is read per call so that the cost of reading switches is spread across
// multiple panelthread iterations.
void
readSwitches(PanelP panelP)
{
    static u32 cycle = 0;

    inRow();
    int i = (cycle++) % 4;
    (&panelP->sw0)[i] = readRow(8 + i);
}

#ifdef TIMINGS
u64 pwmDelta, pwmLoopCount;
#endif

// Background thread: every ~PWM_PERIOD_US, read and reset panel->pwmcount[row][col]
// for each of the 10x18 light bits, scale to the 0-(NLEVELS-1) range based on the actual number
// of emulated cycles elapsed (via // panel->cyclecount), apply dimmingFactor, clight to 0-(NLEVELS-1),
// and store into // p->lights[row][col] for lightRow() to use as its phase-count threshold.
// This replaces panel_pidp1.c's lightthread NSAMPLES sampling + exponentialdecay filter.
void *
pwmthread(void *arg)
{
int rt;
int i, j;
int intensity;
u16 count;
u64 lastCycleCount;
u64 currentCycleCount;
u64 expectedCycles;
PanelLightsP lightsP;
PanelP panelP;
struct sched_param sp;

#ifdef TIMINGS
    u64 currentTime, dbgLastTime;
#endif

    lightsP = (PanelLightsP)arg;
    panelP = lightsP->p;

    // Run this thread real-time too, scheduling jitter here shows up as
    // brightness jitter (and flicker) on the panel.
    // Priority is below panelthread's (99) so the light-row scan still wins
    // if both are runnable at once.
    sp.sched_priority = 98;
    rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    printf("pwmthread realtime: %s\n", rt ? "yes" : "no");

    lastCycleCount = panelP->cyclecount;
#ifdef TIMINGS
    dbgLastTime = gettime();
#endif

    for(;;)
    {
        usleep(PWM_PERIOD_US);

        // Self-calibration: rather than assuming this loop iterates exactly
        // every 31 emulated cycles (PWM_PERIOD_US / 5us) or inferring the
        // cycle count from wall-clock time, which assumes pdp1 runs at
        // exactly 5us/cycle and is sensitive to its scheduling jitter, read
        // the emulator's own cyclecount and use the delta since our last
        // reading as the true number of cycles that occurred.
        // This keeps the scaling correct regardless of pdp1's actual cycle rate.
        currentCycleCount = panelP->cyclecount;
        expectedCycles = currentCycleCount - lastCycleCount;

        // pdp1's main loop runs in bursts, pacing itself to 5us/cycle
        // using usleep(1000), so cyclecount advances in chunks of ~200 every ~1ms rather than smoothly.
        // Many of our ~155us wakeups will land in a gap where cyclecount and
        // pwmcount[][] haven't advanced at all yet.
        // If we proceeded with expectedCycles==0 clighted to 1 and count==0, every light would
        // compute in=0 for this iteration, a brief blackout.
        // The beat between our period and pdp1's burst/sleep cycle then shows up as
        // a slow, drifting on/off flicker.
        // Instead, just skip this iteration entirely and wait for cyclecount to actually advance;
        // pwmcount[][] keeps accumulating untouched in the meantime.
        if( expectedCycles == 0 )
        {
            continue;
        }

        lastCycleCount = currentCycleCount;

        // Panel->pwmcount[][] is a u16, saturating at 65535
        //If this thread is ever delayed long enough that
        // expectedCycles would exceed that, clight it to match -- otherwise
        // a light that's on every cycle (e.g. the PWR light) would read
        // count==65535 but expectedCycles > 65535, making
        // count/expectedCycles < 1 and dimming an "always on" light during
        // long scheduling delays.
        // At ~5us/cycle this is a ~327ms delay, so in practice this clight is just a safety net.
        if(expectedCycles > 65535)
        {
            expectedCycles = 65535;
        }

        for(i = 0; i < 10; i++)
        {
            for(j = 0; j < 18; j++)
            {
                // Read this light's "on" tally for the last period and reset it for the next one.
                // Not synchronized against pdp1's concurrent increments.
                // Worst case, an increment is occasionally lost or counted in the next period,
                // which is not visible at this update rate.
                count = panelP->pwmcount[i][j];
                panelP->pwmcount[i][j] = 0;

                intensity = (int)(((float)count * (float)(NLEVELS - 1) / (float)expectedCycles) * dimmingFactor);
                if( intensity < 0 )
                {
                    intensity = 0;
                }
                if( intensity > (NLEVELS - 1) )
                {
                    intensity = (NLEVELS - 1);
                }

                // Smooth across iterations. A single ~200us sample is noisy
                // enough to snap straight between 0 and 31 from one window to
                // the next, which strobes visibly against lightRow()'s scan
                // period. A short moving average filter removes that without
                // bringing back the old NSAMPLES/exponential filter.
                lightsP->lights[i][j] = (u8)((((int)lightsP->lights[i][j] * 3) + intensity) / 4);
            }
        }

#ifdef TIMINGS
        currentTime = gettime();
        pwmDelta += (currentTime - dbgLastTime) / 1000;  // just usecs
        dbgLastTime = currentTime;
        ++pwmLoopCount;
#endif
    }
}

volatile int doexit;

// Main panel thread, runs at SCHED_FIFO real-time priority. Spawns pwmthread()
// to periodically turn panel->pwmcount[][] into light brightness values, then
// loops driving the light rows (setLights) and reading one switch register
// per iteration (readSwitches) until doexit is set by sighandler(), at which
// point GPIO is parked and the process exits.
void*
panelthread(void *arg)
{
int rt;
pthread_t th;
PanelLights panel;
struct sched_param params;

#ifdef TIMINGS
u64 startTime, lastTime, now, delta, elapsed, loopCount;
#endif

    memset(&panel, 0, sizeof(panel));
    panel.p = (PanelP)arg;
    pthread_create(&th, nil, pwmthread, &panel);

    params.sched_priority = 99;
    rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &params) == 0;
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
// install signal handlers for clean shutdown.
// Returns 0 on success, 1 on failure.
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
// thread which never returns under normal operation.
int
main(int argc, char *argv[])
{
PanelP panelP;

    loadConfig();           // get any config parameters

    panelP = createseg("/tmp/pdp1_panel", sizeof(Panel));

    if( panelP == nil )
    {
        return(1);
    }

    if(initGPIO())
    {
        return(1);
    }

    panelthread(panelP);

    return(0);      // can't happen
}

// Get our settings from the common config file, /opt/pidp1-mods/pidp1.config
void
loadConfig()
{
int i, ival;
char *cP;
ConfigurationP confP;
ConfigurationSettingP settingP;

    if( !(confP = loadConfigFile(CONFIG_FILE)) )
    {
        return;         // no config file found
    }

    if( (settingP = findConfigurationSetting(confP, "panelbrightness")) )
    {
        dimmingFactor = atof(settingP->strvalueP);

        if( dimmingFactor < 0.0 )
        {
            dimmingFactor = 0.0;
        }

        if( dimmingFactor > 1.0 )
        {
            dimmingFactor = 1.0;
        }
    }

    // If true, the default, set our thread priority to realtime fifo, else don't.
    if( (settingP = findConfigurationSetting(confP, "panelrealtime")) )
    {
        setPriority = settingP->onOff;
    }
}
