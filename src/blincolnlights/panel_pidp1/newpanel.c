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
 * Here, the emulator tallies the on state once per 5us emulated cycle by incrementing panelP->pwmcount[][].
 * This driver periodically reads and resets those counts and uses them directly as the PWM
 * phase-count threshold for lightRow() after applying a global brightness scaling factor (dimmingFactor).
 *
 * wje 14-Jun-26 - new implementation based on panel_pidp1.c:
 *    replace lightthread's NSAMPLES sampling/decay filter with
 *    a periodic read+reset of panelP->pwmcount[][]; add brightness scaling.
 * wje 14-Jun-26 - change PWM phase count from 31 to NLEVELS (16)
 *    and lengthen each phase (initDelays()) so a full row scan takes ~1ms
 *    instead of ~340us, and increase PWM_PERIOD_US from 155 to 1000us.
 *    Both changes make the driver more tolerant of ordinary (non-realtime) scheduling jitter.
 * wje 15-Jun-26 - general code cleanup, add command line args
 * wje 15-Jun-26 - reduce NLEVELS from 16 to 8. This eliminates an obscure cause of the panel light
 *    flicker present in p7sim unless running with real time threads. This is not necessary now.
 * wje 15-Jun-26 - add per-light brightness filter as a simple asymmetric first-order IIR
 *    low-pass filter with a fast alpha (FILTER_ALPHA_RISE) for brightening and a slow alpha
 *    (FILTER_ALPHA_FALL) for dimming.
 *    Unlike the p7sim NSAMPLES/exponential filter this operates on the already-computed intensity values,
 *    not raw samples, and significantly reduces cpu use.
 * wje 16-Jun-26 - reload config on sighup
 */
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <signal.h>
#include "common.h"
#include "configuration.h"
#include "pinctrl/gpiolib.h"
#include "panel_pidp1.h"

#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"

// The sleep time in panelthread after updating the light and switch states in shared memory.
#define UPDATEDELAY 100

// Number of PWM brightness levels, also the phases used by lightRow)), 0-(NLEVELS-1).
//
// This setting directly affects light flicker caused by the linux scheduler.
// The original panel_pidp1 used a 31-phase scheme, the shortest phase delay and the full 10-row scan were
// both well within typical non-realtime scheduling jitter, so jitter was a large fraction of every
// phase and showed up as visible flicker, hence the need to set realtime priority on it.
//
// Each phase's delay is computed at startup by initDelays() so that the
// sum of all phase delays for a row totals TARGET_ROW_SCAN_US * 1000 regardless of NLEVELS, we use ns
// in that computation.
// This is a configurable parameter via the config file.
//
#define MINLEVELS 8
#define MAXLEVELS 64    // totally over the top
#define NLEVELS 8

// Target total time (in ns) for one lightRow() phase loop (i.e. one row's
// worth of phase delays, before any per-syscall scheduling overhead).
// initDelays() picks a scaling so phase_delays[] sums to approximately this.
// At 8 phases per row and 10 rows/pass, the nominal zero overhead full-pass time 1ms.
#define TARGET_ROW_SCAN_US 1000

// The target period between pwmthread updates, in microseconds.
//
// Pwmcount[][] is a u16 and at the pdp1's nominal 5us/cycle 1000us corresponds to ~200 cycles,
// giving pwmthread a much longer integration window.
// This both reduces pwmthread's wakeup rate (lower CPU load) and combined with the longer lightRow()
// phase delays above, makes the whole pipeline far less sensitive to non-realtime scheduling jitter.
// Pwmthread measures the actual number of elapsed cycles via panelP->cyclecount and scales
// counts accordingly rather than assuming a fixed cycle count.
// This sets the approximate update loop cycle time, the update sleeps this long after each cycle.
// This is a configurable parameter via the config file.
#define PWM_PERIOD_US 1000

#define PWM_BASE 1.3f

// Asymmetric IIR filter coefficients applied to each light's intensity in
// pwmthread, modeling an incandescent lamp's filament.
// A real one turns on faster than it cools down so an asymmetric filter is used.
// Higher alpha = faster response, a shorter time constant.
// FILTER_ALPHA_RISE ~0.45 gives turn-on a time constant of ~2ms.
// FILTER_ALPHA_FALL ~0.04 gives turn-off a time constant of ~25ms,
// With a first-order filter the visible afterglow is then roughly 50-100ms, matching the real
// characteristics.
#define FILTER_ALPHA_RISE 0.45f
#define FILTER_ALPHA_FALL 0.04f

#define FLIMIT(f) (((f) < 0.0)?0.0:(((f) > 1.0)?1.0:(f)))

typedef Panel *PanelP;      // Panel is in the panel_pidp1.h include file, it's what's in shared memory.

// Per-light brightness state used as a phase-count threshold by lightRow()
// for the 10 rows x 18 columns of front-panel lights, plus a pointer to the shared
// memory Panel struct used to exchange switch/light data with the emulator.
struct PanelLights
{
    u8 lights[10][18];
    float lightsF[10][18];     // filter state for lights[][], see FILTER_ALPHA_RISE/FALL
    PanelP panelP;
};

typedef struct PanelLights PanelLights, *PanelLightsP;

volatile int doexit;

bool setPriority = true;
bool doTiming = false;
u64 pwmDelta, pwmLoopCount;     // Used for timing data

// Global brightness scaling ("dimmer") factor, applied to every light's
// PWM count before it's used by lightRow(). 1.0 = full brightness.
float dimmingFactor = 1.0f;

// Alpha values
float onAlpha = FILTER_ALPHA_RISE;
float offAlpha = FILTER_ALPHA_FALL;

// Loop timing and phase delay factors, these are configurable in the config file,.
int numLevels = NLEVELS; 
int pwmCycleTime = PWM_PERIOD_US;
int scanTime = TARGET_ROW_SCAN_US;

// Table of precomputed phase delays
u32 phase_delays[MAXLEVELS];

// GPIO pin numbers for the 4 ADDR select lines. These select which of the 16 possible
// "rows" (light groups on output, switch registers on input) is currently active on
// the COLUMNS bus.
int ADDR[] = {4, 17, 27, 22};

// GPIO pin numbers for the 18 COLUMN lines, one per bit of the selected row.
// On output these drive light segments; on input they read switch positions.
int COLUMNS[] = {26, 19, 13, 6, 5, 11, 9, 10, 18, 23, 24, 25, 8, 7, 12, 16, 20, 21 };

extern ConfigurationP loadConfigFile(char *filenameP);

int getPin(int p);
u32 readRow(int row);
void loadConfig();
void usage();
void inRow(void);
void outRow(void);
void setPin(int p, int val);
void setRow(int l);
void setAddr(int a);
void initDelays(void);
void lightRow(int row, u8 *l);
void setLights(PanelLightsP lightsP);
void readSwitches(PanelP panelP);
void *pwmthread(void *arg);
void *panelthread(void *arg);
void sighandler(int sig);
void sigReconfigure(int sig);
int initGPIO(void);

// Create/attach the shared Panel segment, initialize GPIO, then run the panel
// thread which never returns under normal operation.
int
main(int argc, char *argv[])
{
int opt;
PanelP panelP;

    loadConfig();           // get any config parameters

    while( (opt = getopt(argc, argv, "b:tn")) != -1 )
    {
        switch( opt )
        {
        case 'b':
            dimmingFactor = atof(optarg);
            dimmingFactor = FLIMIT(dimmingFactor);
            break;

        case 't':
            doTiming = true;
            break;

        case 'n':
            setPriority = false; // no realtime thread priority set
            break;

        default:
            usage();
            break;
        }
    }

    if( optind < argc )
    {
        usage();
    }

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
int
getPin(int p)
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

// Precompute the per-phase delay in ns for each of the numLevels PWM phases
// used by lightRow(): phase_delays[i] = base^i * scale, with base = 1.3 and
// scale chosen so a full row scan (sum of all phase delays) takes ~1ms.
// Later phases get progressively longer delays, which,combined with the
// phase-count threshold in lightRow, approximates a perceptually-linear brightness ramp.
void
initDelays(void)
{
int i;
float base = PWM_BASE;
float scale;
float sum = 0.0f;

    // Sum of base^i for i = 0..numLevels-1, so scale can be picked to make
    // the total phase_delays[] sum equal scanTime regardless of numLevels.
    for(i = 0; i < numLevels; i++)
    {
        sum += pow(base, i);
    }

    // scanTime is in usecs, we want ns for this
    scale = (float)(scanTime * 1000) / sum;

    for(int i = 0; i < numLevels; i++)
    {
        phase_delays[i] = pow(base, i) * scale;
    }
}

// Light one row using the per-light brightness values.
// For each of numLevels phases, a column's light is held on while
// phase < l[i] and turned off once phase reaches l[i] so higher brightness values
// keep the light lit for more phases, giving a PWM-like intensity effect.
// After all phases, the row is blanked and ADDR is parked at 8 (idle/switch row) before returning.
void
lightRow(int row, u8 *l)
{
    setRow(~0);
    setAddr(row);
    usleep(20); // the gpio state chages need time to take effect

    for(int phase = 0; phase < numLevels; phase++)
    {
        for(int i = 0; i < nelem(COLUMNS); i++)
        {
            setPin(COLUMNS[i], !(phase < l[i]));
        }

        nsleep(phase_delays[phase]);
    }

    setRow(~0);
    setAddr(8);
    usleep(30);
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
        if( getPin(COLUMNS[i]) )
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
// p->lights[][] is updated concurrently by pwmthread roughly every pwmCycleTime,
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

// Background thread: every ~pwmCycleTime, read and reset panelP->pwmcount[row][col]
// for each of the 10x18 light bits, scale to the 0-(numLevels-1) range based on the actual number
// of emulated cycles elapsed, apply dimmingFactor, clamp to 0-(numLevels-1),
// and store into p->lights[row][col] for lightRow() to use as its phase-count threshold.
// This replaces panel_pidp1.c's lightthread NSAMPLES sampling + exponentialdecay filter.
void *
pwmthread(void *arg)
{
int rt = 0;
int i, j;
int intensity;
u16 count;
u64 lastCycleCount;
u64 currentCycleCount;
u64 expectedCycles;
u64 currentTime, dbgLastTime;       // used for timing data
PanelLightsP lightsP;
PanelP panelP;
struct sched_param sp;

    lightsP = (PanelLightsP)arg;
    panelP = lightsP->panelP;

    if( setPriority )
    {
        // Run this thread real-time too, scheduling jitter here shows up as
        // brightness jitter (and flicker) on the panel.
        // Priority is below panelthread's (99) so the light-row scan still wins
        // if both are runnable at once.
        sp.sched_priority = 98;
        rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    }

    printf("pwmthread realtime: %s\n", rt ? "yes" : "no");

    lastCycleCount = panelP->cyclecount;
    if( doTiming )
    {
        dbgLastTime = gettime();
    }

    for(;;)
    {
        usleep(pwmCycleTime);

        // Self-calibration: rather than assuming this loop iterates exactly
        // every (pwmCycleTime / 5us) cycles or inferring the cycle count from wall-clock time,
        // which assumes pdp1 runs at exactly 5us/cycle and is sensitive to its scheduling jitter, read
        // the emulator's own cyclecount and use the delta since our last reading as the true number of cycles
        // that occurred.
        // This keeps the scaling correct regardless of pdp1's actual cycle rate.
        currentCycleCount = panelP->cyclecount;
        expectedCycles = currentCycleCount - lastCycleCount;

        // pdp1's main loop runs in bursts, pacing itself to 5us/cycle
        // using usleep(1000), so cyclecount advances in chunks of ~200 every ~1ms rather than smoothly.
        // If we proceeded with expectedCycles==0 clamped to 1 and count==0, every light would
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
        // expectedCycles would exceed that, clamped it to match -- otherwise
        // a light that's on every cycle (e.g. the PWR light) would read
        // count==65535 but expectedCycles > 65535, making
        // count/expectedCycles < 1 and dimming an "always on" light during
        // long scheduling delays.
        // At ~5us/cycle this is a ~327ms delay, so in practice this clamped is just a safety net.
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

                intensity = (int)(((float)count * (float)(numLevels - 1) / (float)expectedCycles) * dimmingFactor);
                if( intensity < 0 )
                {
                    intensity = 0;
                }
                if( intensity > (numLevels - 1) )
                {
                    intensity = (numLevels - 1);
                }

                // Smooth across iterations with an asymmetric IIR filter. A
                // single ~200us sample is noisy enough to snap straight
                // between 0 and numLevels-1 from one window to the next, which
                // strobes visibly against lightRow()'s scan period. Using a
                // faster alpha when brightening and a slower alpha when
                // dimming also models an incandescent lamp's filament,
                // which heats up faster than it cools, so lamps fade out
                // rather than snapping off.
                float *fP = &lightsP->lightsF[i][j];
                float alpha = ((float)intensity > *fP) ? onAlpha : offAlpha;

                *fP += alpha * ((float)intensity - *fP);
                lightsP->lights[i][j] = (u8)(*fP + 0.5f);
            }
        }

        if( doTiming )
        {
            currentTime = gettime();
            pwmDelta += (currentTime - dbgLastTime) / 1000;  // just usecs
            dbgLastTime = currentTime;
            ++pwmLoopCount;
        }
    }
}

// Main panel thread, runs at SCHED_FIFO real-time priority. Spawns pwmthread()
// to periodically turn panelP->pwmcount[][] into light brightness values, then
// loops driving the light rows (setLights) and reading one switch register
// per iteration (readSwitches) until doexit is set by sighandler(), at which
// point GPIO is parked and the process exits.
// Histogram buckets for per-iteration main loop times, used by the -t option
// to characterize the distribution of panelthread loop times (not just the
// average), to help distinguish "every pass is fairly consistently slower
// than ideal" from "most passes are fine but occasional ones spike".
// loopHistBounds[i] is the upper bound (in usec) of bucket i; the last
// bucket catches everything >= loopHistBounds[NHISTBUCKETS-2].
#define NHISTBUCKETS 6
u64 loopHistBounds[NHISTBUCKETS - 1] = { 12000, 20000, 40000, 100000, 250000 };
u64 loopHist[NHISTBUCKETS];

void*
panelthread(void *arg)
{
int rt = 0;
u64 startTime, lastTime, now, delta, elapsed, loopCount;    // timing data
u64 loopUs, loopMin, loopMax;
pthread_t th;
PanelLights panel;
struct sched_param params;

    memset(&panel, 0, sizeof(panel));
    panel.panelP = (PanelP)arg;
    pthread_create(&th, nil, pwmthread, &panel);

    if( setPriority )
    {
        params.sched_priority = 99;
        rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &params) == 0;
    }

    printf("realtime thread: %s\n", rt ? "yes" : "no");

    initDelays();

    if( doTiming )
    {
        startTime = lastTime = gettime();
        delta = loopCount = 0;
        loopMin = ~0ULL;
        loopMax = 0;
        memset(loopHist, 0, sizeof(loopHist));
    }

    while( !doexit )
    {
        setLights(&panel);
        readSwitches(panel.panelP);
        usleep(UPDATEDELAY);

        if( doTiming )
        {
            now = gettime();
            loopUs = (now - lastTime) / 1000;   // just usecs
            delta += loopUs;
            lastTime = now;
            ++loopCount;

            if( loopUs < loopMin )
            {
                loopMin = loopUs;
            }

            if( loopUs > loopMax )
            {
                loopMax = loopUs;
            }

            int bucket = NHISTBUCKETS - 1;
            for(int b = 0; b < NHISTBUCKETS - 1; b++)
            {
                if( loopUs < loopHistBounds[b] )
                {
                    bucket = b;
                    break;
                }
            }

            loopHist[bucket]++;
        }
    }

    if( doTiming )
    {
        elapsed = (gettime() - startTime) / 1000;   // just usecs
        delta /= loopCount; // now per-loop avg
        printf("Avg main loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
            delta, loopCount, elapsed, ((float)(loopCount * delta) / (float)elapsed) * 100.0);
        printf("Main loop time min %lu usec, max %lu usec.\n", loopMin, loopMax);

        printf("Main loop time histogram (usec):\n");
        for(int b = 0; b < NHISTBUCKETS; b++)
        {
            u64 lo = (b == 0) ? 0 : loopHistBounds[b - 1];

            if( b < NHISTBUCKETS - 1 )
            {
                printf("  [%6lu - %6lu): %lu (%.1f%%)\n",
                    lo, loopHistBounds[b], loopHist[b], 100.0 * loopHist[b] / loopCount);
            }
            else
            {
                printf("  [%6lu -    inf): %lu (%.1f%%)\n",
                    lo, loopHist[b], 100.0 * loopHist[b] / loopCount);
            }
        }

        pwmDelta /= pwmLoopCount; // now per-loop avg
        printf("Avg pwm update loop time %lu usec over %lu cycles (target %d usec).\n",
            pwmDelta, pwmLoopCount, pwmCycleTime);
    }

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

// SIGHUP triggers this to reload the configuration and recompute all the phase delays.
void
sigReconfigure(int sig)
{
    reloadConfigFile(CONFIG_FILE); // need to force a reload
    loadConfig();
    initDelays();
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
    signal(SIGHUP, sigReconfigure);
    return(0);
}

// Get our settings from the common config file, /opt/pidp1-mods/pidp1.config
void
loadConfig()
{
ConfigurationP confP;
ConfigurationSettingP settingP;

    if( !(confP = loadConfigFile(CONFIG_FILE)) )
    {
        return;         // no config file found
    }

    if( (settingP = findConfigurationSetting(confP, "panelbrightness")) )
    {
        dimmingFactor = settingP->fvalue;
        dimmingFactor = FLIMIT(dimmingFactor);
    }

    if( (settingP = findConfigurationSetting(confP, "panelonalpha")) )
    {
        onAlpha = settingP->fvalue;
        onAlpha = FLIMIT(onAlpha);
    }

    if( (settingP = findConfigurationSetting(confP, "paneloffalpha")) )
    {
        offAlpha = settingP->fvalue;
        offAlpha = FLIMIT(offAlpha);
    }

    if( (settingP = findConfigurationSetting(confP, "panelrealtime")) )
    {
        setPriority = settingP->onOff;
    }

    if( (settingP = findConfigurationSetting(confP, "panellevels")) )
    {
        numLevels = settingP->ivalue;
        if( numLevels > MAXLEVELS )
        {
            numLevels = MAXLEVELS;
        }
        if( numLevels < MINLEVELS )
        {
            numLevels = MINLEVELS;
        }
    }

    if( (settingP = findConfigurationSetting(confP, "panelcycletime")) )
    {
        pwmCycleTime = settingP->ivalue;
    }

    if( (settingP = findConfigurationSetting(confP, "panelscantime")) )
    {
        scanTime = settingP->ivalue;
    }
}

void
usage()
{
    printf("Usage: newpanel [-b brightness] [-t] [-n]\n");
    printf("-b brightness, set max brigtness, 0.0 to 1.0\n");
    printf("-t, enable timing statistics, printed on exit\n");
    printf("-n, don't use realtime threads\n");
    exit(1);
}
