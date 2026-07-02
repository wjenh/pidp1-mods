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
 * wje 26-Jun-26 - improve performance a bit, fix some rare potential failure points
 * wje 02-Jul-26 - double-buffer PanelLights.lights[][] (lightsReadyIdx) so setLights()'s
 *    per-pass snapshot can never tear against pwmthread's concurrent writes; a torn read
 *    could show up as a brief, spuriously-bright flash on one pass.
 *    LightRow()'s phase sleeps now target absolute per-row deadlines
 *    instead of chained relative nsleep() calls, so a late wakeup in one phase no longer pushes
 *    every later phase/row of the pass out with it.
 *    Clamping added to keep the pwm count from exceeding the dimming factor.
 *    The gpio interaction was optimized to work better with the pi 5, no effect on the pi 4.
 *    Extensive testing shows the use of real time thread use is not expensive, keeping the capability.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/mman.h>
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
// This both reduces pwmthread's wakeup rate and combined with the phase delays above
// makes the pipeline less sensitive to non-realtime scheduling jitter.
// Pwmthread measures the actual number of elapsed cycles via panelP->cyclecount and scales
// counts accordingly rather than assuming a fixed cycle count.
// This sets the approximate update loop cycle time, the update sleeps this long after each cycle.
// This is a configurable parameter via the config file.
#define PWM_PERIOD_US 1000

// Minimum sane values for the config-supplied loop times, so a bad or zero config value
// can't produce zero phase delays (instant scan, full-bright flicker) or a usleep(0) busy spin.
#define MIN_PWM_PERIOD_US 100
#define MIN_SCAN_US 100

// Real-time (SCHED_FIFO) priorities for the two panel threads.
// Panelthread is one above pwmthread so it wins if both are runnable at once.
#define PANEL_RT_PRIO 80
#define PWM_RT_PRIO 79

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

#define NPWMHISTBUCKETS 6   // used for timing
#define NPHASEHISTBUCKETS 6

#define FLIMIT(f) (((f) < 0.0)?0.0:(((f) > 1.0)?1.0:(f)))
#define ELEMENTS(x) ((int)(sizeof(x)/sizeof(x[0])))

typedef Panel *PanelP;      // Panel is in the panel_pidp1.h include file, it's what's in shared memory.

// Per-light brightness state used as a phase-count threshold by lightRow()
// for the 10 rows x 18 columns of front-panel lights, plus a pointer to the shared
// memory Panel struct used to exchange switch/light data with the emulator.
//
// lights[][] is double-buffered so that pwmthread (writer) and setLights()/panelthread (reader)
// never touch the same buffer at once.
// Pwmthread computes a full pass into the pending buffer then publishes it by storing its
// index into lightsReadyIdx with a release fence.
// SetLights() loads lightsReadyIdx with an acquire fence and copies from that buffer.i
struct PanelLights
{
    u8 lights[2][10][18];
    atomic_int lightsReadyIdx; // which of lights[0]/lights[1] is complete and safe to read
    float lightsF[10][18];     // filter state for lights[][], see FILTER_ALPHA_RISE/FALL
    PanelP panelP;
};

typedef struct PanelLights PanelLights, *PanelLightsP;

volatile int doexit;
volatile sig_atomic_t reconfigRequested;    // set by the SIGHUP handler, serviced in panelthread's loop

bool setPriority = false;
bool doTiming = false;
u64 startTime;
u64 pwmDelta, pwmLoopCount;     // Used for timing data
u64 loopDelta, loopCount, loopMin, loopMax;

// Timing data for just the 10x18 read/scale/filter compute loop inside pwmthread.
u64 pwmCompUs, pwmCompMin, pwmCompMax;
u64 pwmHistBounds[NPWMHISTBUCKETS - 1] = { 100, 300, 1000, 5000, 20000 }; // usec
u64 pwmHist[NPWMHISTBUCKETS];
u64 phaseStallCount, phaseStallNsSum, phaseStallMinNs, phaseStallMaxNs;
u64 phaseHistBoundsUs[NPHASEHISTBUCKETS - 1] = { 20, 100, 500, 2000, 10000 }; // usec
u64 phaseHist[NPHASEHISTBUCKETS];
int worstPhaseStallRow = -1, worstPhaseStallPhase = -1;   // -1 means "no stall recorded yet"
u64 worstPhaseStallNs;
u8 worstPhaseStallCols[18];    // l[] snapshot at the worst stall seen so far

// "Transitioning" variant of the above.
// The worst-by-raw-lateness sample isn't necessarily the most diagnostic one, a stall can
// land on a phase where no column's l[i] equals that phase, in which case nothing was
// actually held on past its intended time, the row/pass was merely delayed. This variant
// only counts/tracks samples where at least one column DOES have l[i]==phase, i.e. a real
// OFF-transition was pending and that column (or columns, since several often share the
// same brightness level) was actually held on ns nanoseconds longer than intended for one pass.
// This is the metric that most directly demonstrates a visible-brightness-impact
// stall rather than just a delayed-but-harmless one.
u64 transitionStallCount, transitionStallNsSum;
int worstTransitionStallRow = -1, worstTransitionStallPhase = -1;
u64 worstTransitionStallNs;
u8 worstTransitionStallCols[18];
int worstTransitionStallNCols;   // how many of the 18 columns were transitioning at that phase

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
static void tsAddNs(struct timespec *ts, u64 ns);
static u64 tsDiffNs(struct timespec *a, struct timespec *b);
static void recordPhaseStall(int row, int phase, u64 ns, u8 *l);
void lightRow(int row, u8 *l);
void setLights(PanelLightsP lightsP);
void readSwitches(PanelP panelP);
void *pwmthread(void *arg);
void *panelthread(void *arg);
void sighandler(int sig);
void sigReconfigure(int sig);
void reportTiming();
int initGPIO(void);

// Create/attach the shared Panel segment, initialize GPIO, then run the panel
// thread which never returns under normal operation.
int
main(int argc, char *argv[])
{
int opt;
PanelP panelP;

    loadConfig();           // get any config parameters

    while( (opt = getopt(argc, argv, "b:tr")) != -1 )
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

        case 'r':
            setPriority = true;         // use realtime thread priority, permission must have been set on newpanel
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
int i;

    for(i = 0; i < ELEMENTS(COLUMNS); i++)
    {
        gpio_set_fsel(COLUMNS[i], GPIO_FSEL_INPUT);
    }
}

// Switch all 18 COLUMN GPIO pins to outputs, for driving lights.
void
outRow(void)
{
int i;

    for(i = 0; i < ELEMENTS(COLUMNS); i++)
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
    for(int i = 0; i < ELEMENTS(COLUMNS); i++)
    {
        setPin(COLUMNS[i], (l >> i) & 1);
    }
}

// Drive the 4 ADDR select pins from the low 4 bits of a, selecting which
// row of lights/switches is connected to the COLUMNS bus.
void
setAddr(int a)
{
    for(int i = 0; i < ELEMENTS(ADDR); i++)
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

// Add ns nanoseconds to *ts, normalizing tv_nsec back into [0, 1e9).
static void
tsAddNs(struct timespec *ts, u64 ns)
{
    ts->tv_nsec += (long)(ns % 1000000000ULL);
    ts->tv_sec  += (time_t)(ns / 1000000000ULL);

    if( ts->tv_nsec >= 1000000000L )
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

// Return (a - b) in nanoseconds. Used to measure how late a clock_nanosleep()
// wakeup was relative to the absolute deadline it targeted (a = actual wakeup
// time, b = intended deadline). Defensively returns 0 if a is not actually
// later than b (a wakeup can never be early against CLOCK_MONOTONIC, but
// timer-granularity rounding could in principle land a==b, and returning 0
// rather than wrapping to a huge unsigned value keeps this safe regardless).
static u64
tsDiffNs(struct timespec *a, struct timespec *b)
{
long sec, nsec;

    sec = a->tv_sec - b->tv_sec;
    nsec = a->tv_nsec - b->tv_nsec;

    if( nsec < 0 )
    {
        nsec += 1000000000L;
        sec--;
    }

    if( sec < 0 )
    {
        return(0);
    }

    return(((u64)sec * 1000000000ULL) + (u64)nsec);
}

// Record one lightRow() phase-wakeup lateness sample (see the block comment
// above phaseStallCount/phaseHist near the top of this file). Only called
// when doTiming is set. row/phase identify which row and which PWM phase
// this sample is for; ns is the lateness in nanoseconds computed by the
// caller via tsDiffNs(); l is the row's current brightness-threshold array,
// snapshotted into worstPhaseStallCols[] whenever this sample becomes the
// new worst one seen, so the affected column(s) (those with l[i]==phase)
// can be identified after the fact.
//
// Also checks whether any column actually had l[i]==phase, i.e. whether
// an OFF-transition was actually pending at the exact phase this stall
// delayed and if so separately tracks that as a "transitioning" stall.
// A stall that lands on a phase with nothing
// pending only delays the row/pass; a "transitioning" stall actually holds
// a lit column on ns nanoseconds longer than intended, which is the case
// that can produce a visible brightness spike.
//
static void
recordPhaseStall(int row, int phase, u64 ns, u8 *l)
{
int bucket;
u64 us;
int i;
int nTransitioning;

    phaseStallCount++;
    phaseStallNsSum += ns;

    if( ns < phaseStallMinNs )
    {
        phaseStallMinNs = ns;
    }

    if( ns > phaseStallMaxNs )
    {
        phaseStallMaxNs = ns;
    }

    if( ns > worstPhaseStallNs )
    {
        worstPhaseStallNs = ns;
        worstPhaseStallRow = row;
        worstPhaseStallPhase = phase;

        for(i = 0; i < ELEMENTS(COLUMNS); i++)
        {
            worstPhaseStallCols[i] = l[i];
        }
    }

    us = ns / 1000;
    bucket = NPHASEHISTBUCKETS - 1;

    for(int b = 0; b < NPHASEHISTBUCKETS - 1; b++)
    {
        if( us < phaseHistBoundsUs[b] )
        {
            bucket = b;
            break;
        }
    }

    phaseHist[bucket]++;

    // How many of this row's 18 columns were actually scheduled to turn
    // off exactly at "phase"? Those, and only those, were held on ns
    // nanoseconds past their intended off-time by this specific stall.
    nTransitioning = 0;

    for(i = 0; i < ELEMENTS(COLUMNS); i++)
    {
        if( l[i] == phase )
        {
            nTransitioning++;
        }
    }

    if( nTransitioning > 0 )
    {
        transitionStallCount++;
        transitionStallNsSum += ns;

        if( ns > worstTransitionStallNs )
        {
            worstTransitionStallNs = ns;
            worstTransitionStallRow = row;
            worstTransitionStallPhase = phase;
            worstTransitionStallNCols = nTransitioning;

            for(i = 0; i < ELEMENTS(COLUMNS); i++)
            {
                worstTransitionStallCols[i] = l[i];
            }
        }
    }
}

// Light one row using the per-light brightness values.
// For each of numLevels phases, a column's light is held on while
// phase < l[i] and turned off once phase reaches l[i] so higher brightness values
// keep the light lit for more phases, giving a PWM-like intensity effect.
// After all phases, the row is blanked and ADDR is parked at 8 (idle/switch row) before returning.
//
// Each phase's wakeup is scheduled against an absolute deadline computed from a single timestamp taken at the
// start of the row, and each column's on/off pin state only changes once per row update.
// It starts on if l[i] > 0, then turns off at phase == l[i].
// Minimizing updates is especially important on the Pi 5 to reduce overheadm, every GPIO write is a
// round trip to a separate controller over the pci bus to reduce overhead.
void
lightRow(int row, u8 *l)
{
u64 lateNs;
struct timespec deadline;
struct timespec now;

    setRow(~0);
    setAddr(row);
    usleep(20); // the gpio state chages need time to take effect

    // Establish phase-0 state once: on iff l[i] > 0.
    for(int i = 0; i < ELEMENTS(COLUMNS); i++)
    {
        setPin(COLUMNS[i], !(0 < l[i]));
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);

    for(int phase = 0; phase < numLevels; phase++)
    {
        if( phase > 0 )
        {
            if( doTiming )
            {
                clock_gettime(CLOCK_MONOTONIC, &now);
                lateNs = tsDiffNs(&now, &deadline);
                recordPhaseStall(row, phase, lateNs, l);
            }

            // Only columns transitioning off exactly at this phase need touching;
            // everything else already holds the state it needs from a prior pass.
            for(int i = 0; i < ELEMENTS(COLUMNS); i++)
            {
                if( l[i] == phase )
                {
                    setPin(COLUMNS[i], 1); // off
                }
            }
        }

        tsAddNs(&deadline, phase_delays[phase]);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nil);
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

    for(int i = 0; i < ELEMENTS(COLUMNS); i++)
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
// Snapshot p->lights[][] once per pass so every row is driven from a single,
// internally-consistent set of values.
// This avoids random display intensity jitter.
//
// The snapshot is taken from whichever of the two lights[][] buffers lightsReadyIdx
// (loaded with an acquire fence) currently points to; pwmthread never writes that buffer
// until it has a newer one fully ready, so this memcpy can't observe a half-written mix
// of an old and new pass.
void
setLights(PanelLightsP lightsP)
{
    u8 lights[10][18];
    int readyIdx = atomic_load_explicit(&lightsP->lightsReadyIdx, memory_order_acquire);

    memcpy(lights, lightsP->lights[readyIdx], sizeof(lights));

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
// for each of the 10x18 light bits, scale to the 0-numLevels range based on the actual number
// of emulated cycles elapsed, apply dimmingFactor, clamp to 0-numLevels,
// and store into p->lights[row][col] for lightRow() to use as its phase-count threshold.
// This replaces the old panel_pidp1.c's lightthread NSAMPLES sampling + exponentialdecay filter.
void *
pwmthread(void *arg)
{
int i, j;
int intensity;
u16 count;
u64 lastCycleCount;
u64 currentCycleCount;
u64 expectedCycles;
u64 currentTime, dbgLastTime;       // used for timing data
u64 compStart, compEnd, compUs;     // used for compute-loop timing data
PanelLightsP lightsP;
PanelP panelP;
struct sched_param sp;

    lightsP = (PanelLightsP)arg;
    panelP = lightsP->panelP;

    if( setPriority )
    {
        // Run this thread real-time too, scheduling jitter here shows up as
        // brightness jitter (and flicker) on the panel.
        // Priority is one below panelthread's so the light-row scan still wins
        // if both are runnable at once.
        sp.sched_priority = PWM_RT_PRIO;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    lastCycleCount = panelP->cyclecount;
    if( doTiming )
    {
        dbgLastTime = gettime();
        pwmCompMin = ~0ULL;
        pwmCompMax = 0;
        memset(pwmHist, 0, sizeof(pwmHist));
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

        // The pdp1's main loop runs in bursts, pacing itself to 5us/cycle
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

        // Compute this pass into the buffer NOT currently marked ready, so setLights()'s
        // concurrent reads of the ready buffer are never disturbed. Published below via
        // lightsReadyIdx once the whole 10x18 pass is written.
        int writeIdx = 1 - atomic_load_explicit(&lightsP->lightsReadyIdx, memory_order_relaxed);

        // Panel->pwmcount[][] is a u16, saturating at 65535
        // If this thread is ever delayed long enough that
        // expectedCycles would exceed that, clamp it to match.
        // Otherwise a light that's on every cycle (e.g. the PWR light) would read
        // count==65535 but expectedCycles > 65535, making count/expectedCycles < 1 and
        // dimming an "always on" light during long scheduling delays.
        // At ~5us/cycle this is a ~327ms delay, so in practice this clamp is just a safety net.
        if(expectedCycles > 65535)
        {
            expectedCycles = 65535;
        }

        if( doTiming )
        {
            compStart = gettime();
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

                // Clamp the count, scheduling delays could result in an invalid value.
                if( count > expectedCycles )
                {
                    count = (u16)expectedCycles; // expectedCycles is already clamped to <=65535 above
                }

                // Scale the duty fraction (count/expectedCycles, 0..1) to a phase threshold of
                // 0..numLevels. Scaling to numLevels lets an always-on light reach full brightness.
                intensity = (int)(((float)count * (float)numLevels / (float)expectedCycles) * dimmingFactor);
                if( intensity < 0 )
                {
                    intensity = 0;
                }
                if( intensity > numLevels )
                {
                    intensity = numLevels;
                }

                // Smooth across iterations with an asymmetric IIR filter.
                // A single ~200us sample is noisy enough to snap straight
                // between 0 and numLevels-1 from one window to the next, which
                // strobes visibly against lightRow()'s scan period.
                // Using a faster alpha when brightening and a slower alpha when
                // dimming also models an incandescent lamp's filament
                // which heats up faster than it cools, so lamps fade out rather than snapping off.
                float *fP = &lightsP->lightsF[i][j];
                float alpha = ((float)intensity > *fP) ? onAlpha : offAlpha;

                *fP += alpha * ((float)intensity - *fP);
                lightsP->lights[writeIdx][i][j] = (u8)(*fP + 0.5f);
            }
        }

        // Publish this pass: readers loading lightsReadyIdx with acquire semantics are now
        // guaranteed to see the fully-written buffer above, not a partial one.
        atomic_store_explicit(&lightsP->lightsReadyIdx, writeIdx, memory_order_release);

        if( doTiming )
        {
            compEnd = gettime();
            compUs = (compEnd - compStart) / 1000;  // just usecs
            pwmCompUs += compUs;

            if( compUs < pwmCompMin )
            {
                pwmCompMin = compUs;
            }
            if( compUs > pwmCompMax )
            {
                pwmCompMax = compUs;
            }

            int bucket = NPWMHISTBUCKETS - 1;
            for(int b = 0; b < NPWMHISTBUCKETS - 1; b++)
            {
                if( compUs < pwmHistBounds[b] )
                {
                    bucket = b;
                    break;
                }
            }
            pwmHist[bucket]++;

            currentTime = gettime();
            pwmDelta += (currentTime - dbgLastTime) / 1000;  // just usecs
            dbgLastTime = currentTime;
            ++pwmLoopCount;
        }
    }
}

// Main panel thread, can run with normal SCHED_FIFO real-time priority.
// Spawns pwmthread() to periodically turn panelP->pwmcount[][] into light brightness values, then
// loops driving the light rows (setLights) and reading one switch register
// per iteration (readSwitches) until doexit is set by sighandler(), at which
// point GPIO is parked and the process exits.
// Histogram buckets are used for per-iteration main loop times if the -t option is on
// to characterize the distribution of panelthread loop times.
#define NHISTBUCKETS 6
u64 loopHistBounds[NHISTBUCKETS - 1] = { 12000, 20000, 40000, 100000, 250000 };
u64 loopHist[NHISTBUCKETS];

void*
panelthread(void *arg)
{
int rt = 0;
u64 lastTime, now;              // timing data
u64 loopUs;
pthread_t th;
PanelLights panel;
struct sched_param params;

    memset(&panel, 0, sizeof(panel));
    panel.panelP = (PanelP)arg;

    // Create the brightness thread BEFORE any mlockall() below. If MCL_FUTURE were already in
    // effect, locking the new thread's stack could fail making pthread_create() fail.
    if( pthread_create(&th, nil, pwmthread, &panel) != 0 )
    {
        fprintf(stderr, "Error: could not create pwmthread; panel lights will not update.\n");
    }

    if( setPriority )
    {
        params.sched_priority = PANEL_RT_PRIO;
        rt = pthread_setschedparam(pthread_self(), SCHED_FIFO, &params) == 0;
    }

    printf("realtime thread: %s\n", rt ? "yes" : "no");

    // Lock pages into RAM only when real-time scheduling is actually in effect.
    // The page-fault determinism mlockall() buys is only meaningful for an RT thread, so there is no
    // reason to use it otherwise.
    // Non-fatal on failure.
    if( rt )
    {
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }

    initDelays();

    if( doTiming )
    {
        startTime = lastTime = gettime();
        loopDelta = loopCount = 0;
        loopMin = ~0ULL;
        loopMax = 0;
        memset(loopHist, 0, sizeof(loopHist));

        phaseStallCount = phaseStallNsSum = 0;
        phaseStallMinNs = ~0ULL;
        phaseStallMaxNs = 0;
        memset(phaseHist, 0, sizeof(phaseHist));
        worstPhaseStallNs = 0;
        worstPhaseStallRow = -1;
        worstPhaseStallPhase = -1;

        transitionStallCount = transitionStallNsSum = 0;
        worstTransitionStallNs = 0;
        worstTransitionStallRow = -1;
        worstTransitionStallPhase = -1;
        worstTransitionStallNCols = 0;
    }

    while( !doexit )
    {
        // Service a pending SIGHUP reconfig here, between scans, instead of in the signal
        // handler, the processing is not safe when done from the interrupt handler.
        if( reconfigRequested )
        {
            reconfigRequested = 0;
            reloadConfigFile(CONFIG_FILE);
            loadConfig();
            initDelays();
        }

        setLights(&panel);
        readSwitches(panel.panelP);
        usleep(UPDATEDELAY);

        if( doTiming )
        {
            now = gettime();
            loopUs = (now - lastTime) / 1000;   // just usecs
            loopDelta += loopUs;
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

    // If collecting timing data, print it.
    if( doTiming  )
    {
        reportTiming();
    }

    setAddr(8);
    inRow();
    exit(0);
}

// Print our massively-detailed timing data.
void
reportTiming()
{
u64 elapsed;

    elapsed = (gettime() - startTime) / 1000;   // just usecs
    printf(
        "Avg main loop time %lu usec over %lu cycles, elapsed time %lu usec, %.2f percent of elapsed time.\n",
        loopDelta, loopCount, elapsed, ((float)(loopCount * loopDelta) / (float)elapsed) * 100.0);
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

    // Timing for just the 10x18 read/scale/filter compute loop (see pwmCompUs comment) --
    // this is the section vulnerable to the stale-expectedCycles overshoot; a max here
    // anywhere near pwmCycleTime means pwmthread itself got descheduled mid-pass.
    printf("Pwm compute-loop time avg %lu usec, min %lu usec, max %lu usec, over %lu cycles.\n",
        pwmCompUs / pwmLoopCount, pwmCompMin, pwmCompMax, pwmLoopCount);

    printf("Pwm compute-loop time histogram (usec):\n");
    for(int b = 0; b < NPWMHISTBUCKETS; b++)
    {
        u64 lo = (b == 0) ? 0 : pwmHistBounds[b - 1];

        if( b < NPWMHISTBUCKETS - 1 )
        {
            printf("  [%6lu - %6lu): %lu (%.1f%%)\n",
                lo, pwmHistBounds[b], pwmHist[b], 100.0 * pwmHist[b] / pwmLoopCount);
        }
        else
        {
            printf("  [%6lu -    inf): %lu (%.1f%%)\n",
                lo, pwmHist[b], 100.0 * pwmHist[b] / pwmLoopCount);
        }
    }

    if( phaseStallCount )
    {
        printf("Lightrow phase-stall lateness avg %lu usec, min %lu usec, max %lu usec, over %lu samples.\n",
            (phaseStallNsSum / phaseStallCount) / 1000, phaseStallMinNs / 1000, phaseStallMaxNs / 1000,
            phaseStallCount);

        printf("Lightrow phase-stall lateness histogram (usec):\n");
        for(int b = 0; b < NPHASEHISTBUCKETS; b++)
        {
            u64 lo = (b == 0) ? 0 : phaseHistBoundsUs[b - 1];

            if( b < NPHASEHISTBUCKETS - 1 )
            {
                printf("  [%6lu - %6lu): %lu (%.1f%%)\n",
                    lo, phaseHistBoundsUs[b], phaseHist[b], 100.0 * phaseHist[b] / phaseStallCount);
            }
            else
            {
                printf("  [%6lu -    inf): %lu (%.1f%%)\n",
                    lo, phaseHist[b], 100.0 * phaseHist[b] / phaseStallCount);
            }
        }

        if( worstPhaseStallRow >= 0 )
        {
            printf("Worst lightrow phase stall (any phase): row %d phase %d, %lu usec late, "
                "row's l[] at that time (18 columns):",
                worstPhaseStallRow, worstPhaseStallPhase, worstPhaseStallNs / 1000);

            for(int i = 0; i < ELEMENTS(COLUMNS); i++)
            {
                printf(" %u", worstPhaseStallCols[i]);
            }

            printf("\n");
        }

        // These are the subset of the above where a column was actually held on late,
        // not just a harmlessly-delayed idle phase.
        if( transitionStallCount )
        {
            printf("Of those, %lu (%.1f%%) coincided with an actual pending OFF-transition "
                "(i.e. held a lit column on late), avg %lu usec late.\n",
                transitionStallCount, 100.0 * transitionStallCount / phaseStallCount,
                (transitionStallNsSum / transitionStallCount) / 1000);

            printf("Worst TRANSITIONING lightrow phase stall: row %d phase %d, %lu usec late, "
                "%d column(s) held on late, row's l[] at that time (18 columns):",
                worstTransitionStallRow, worstTransitionStallPhase, worstTransitionStallNs / 1000,
                worstTransitionStallNCols);

            for(int i = 0; i < ELEMENTS(COLUMNS); i++)
            {
                printf(" %u", worstTransitionStallCols[i]);
            }

            printf("\n");
        }
        else
        {
            printf("Of those, none coincided with an actual pending OFF-transition -- "
                "every stall this run landed on an already-idle phase.\n");
        }
    }
    else
    {
        printf("Lightrow phase-stall: no samples recorded (numLevels <= 1?).\n");
    }
}

// SIGINT/SIGTERM handler: requests a clean shutdown of panelthread()'s main loop.
void
sighandler(int sig)
{
    doexit = 1;
}

// SIGHUP triggers this to reload the configuration and recompute all the phase delays.
// If timing data is being accumlated, print a snapshot.
void
sigReconfigure(int sig)
{
    reconfigRequested = 1;

    if( doTiming )
    {
        reportTiming();
    }
}

// Initialize the GPIO subsystem: map GPIO registers, configure the 4 ADDR pins as
// outputs and the 18 COLUMN pins with pull-ups, set the bus to input/idle, and
// install signal handlers for clean shutdown.
// Returns 0 on success, 1 on failure.
int
initGPIO(void)
{
int i;
int ngpio;

    ngpio = gpiolib_init();

    if(ngpio <= 0)
    {
        return(1);
    }

    if(gpiolib_mmap())
    {
        return(1);
    }

    for(i = 0; i < ELEMENTS(ADDR); i++)
    {
        gpio_set_fsel(ADDR[i], GPIO_FSEL_OUTPUT);
    }

    for(i = 0; i < ELEMENTS(COLUMNS); i++)
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
        if( pwmCycleTime < MIN_PWM_PERIOD_US )
        {
            pwmCycleTime = MIN_PWM_PERIOD_US;
        }
    }

    if( (settingP = findConfigurationSetting(confP, "panelscantime")) )
    {
        scanTime = settingP->ivalue;
        if( scanTime < MIN_SCAN_US )
        {
            scanTime = MIN_SCAN_US;
        }
    }
}

void
usage()
{
    printf("Usage: newpanel [-b brightness] [-t] [-r]\n");
    printf("-b brightness, set max brigtness, 0.0 to 1.0\n");
    printf("-t, enable timing statistics, printed on exit\n");
    printf("-r, use realtime threads\n");
    exit(1);
}
