/*
 * This was written originally by Angelo Papenhoff, aap.
 * It has been modified by Bill Ezell, wje, pdp1@quackers.net to:
 * Add new features.
 * Make it readable. :)
 * It uses the One True Formatting Style, keep it.
 * The formatting is based on research done at Stanford many years ago that determined the major causes
 * of coding errors, the formatting reduced that. It works.
 *
 * wje 05-Jan-26 break from original repo, now independent.
 * wje 10-Feb-26 style cleanup, remove conditionals for light pen, origin shift, lai, lia
 * wje 18-Feb-26 massive cleanup, add shm support
 * wje 22-Feb-26 massive cleanup was too massive, revert much of it
 * wje 28-Mar-26 add timing logging
 * wje 6-Apr-26 small mod to not clear AD1_STEP until the end of a cycle, use config setting for mem file
 * wje 7-Apr-26 reload configuration on sigint
 * wje 8-Apr-26 make timing configurable instead of compile time
 * wje 11-Apr-26 the light pen really doesn't need a listener thread, just use nonblocking reads
 * wje 1-May-26 set radius^2 from config for the Type 340 display
 * wje 16-Jun-26 many changes so the CHM simple test program displays like the real PDP-1
 * wje 3-Jul-26 code cleanup, add a few safety checks, no functional change
 * wje 14-Jul-2026 wje some more cleanup
 * wje 8-Aug-2026 wje add motion prediction setting
 * wje 15-Aug-2026 wje if stopped, readin did not do an iot start causing failures
 * wje/claude 11-Sep-2026 extend the pidp1timing report
 * Claude 20-Sep-2026 remove the shared-memory segment, ad1 and fastload now talk to the emulator
 *    through the network server in ad1server.c.
 * Claude 24-Sep-2026 the reader and punch ports' sockets are non-blocking and are handed to the
 *    emulator thread (mountReader()/mountPunch()) instead of replacing r_fd/p_fd here.
 * Claude 27-Sep-2026 the startup tapes are mounted through papertape.c, and stdin commands run on
 *    the console thread (console.c).
 * Claude 29-Sep-2026 display.c no longer sends display aging; the display clients keep their own clocks.
 * Claude 29-Sep-2026 display.c keeps the client's stream on word boundaries across a partial write.
 * Claude 30-Sep-2026 display.c closes a display client that has left, and lifts its pen, without waiting for a write to fail.
 * Claude 30-Sep-2026 display.c lifts the pen of a display client dropped after a failed write too, and no longer
 *    reads the lightpen from that client's fd once it is closed.
 * Claude 03-Oct-2026 a channel steal before an instruction's final cycle restarts the instruction, and none is
 *    taken in a sequence break's entry or after cycle 0 of a once-deferred jump (pdp1.c, F17 6-18, 6-19).
 *    The panel tally keeps the cycles of an instruction a break cancels, and all three break cycles.
 * Claude 04-Oct-2026 the power switch's off edge gives the IOT plugins a power clear (dynamicIots.c).
 * Claude 04-Oct-2026 a power cycle puts sbs16 back as the configuration has it.
 * Claude 05-Oct-2026 the command port now serves each connection on its own thread, ports that cannot listen
 *    are retried, the typewriter's relay marks its input ready with no pollthread,
 *    and pdp1 creates the panel segment if no panel has done so.
 * Claude 05-Oct-2026 only one pdp1 runs at a time.
 *    A second start exits before it touches the panel, ports or coremem.
 * wje/Claude 07-Oct-2026 a new display or typewriter client starts with current output, any leftover data
 *    before the connection opened is first discarded.
 * Claude 07-Oct-2026 the default reader and punch tapes are posted before any port listens, so a front end's
 *    tape socket is no longer replaced by them.
 * Claude 07-Oct-2026 only the emulator thread drives the audio device; the console's audio requests are posted
 *    to it (audio.c, console.c).
 * Claude 08-Oct-2026 the sound comes back with the power if audio is on.
*/

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <math.h>
#include <locale.h>
#include <time.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "common.h"
#include "pdp1.h"
#include "args.h"
#include "panel_pidp1.h"    // wje - add, only a typedef was here, not the thing being typedef-ed!

#include "configuration.h"
#define NOTIOTH
#include "dynamicIots.h"
#include "highSpeedChannels.h"
#include "ad1server.h"
#include "papertape.h"
#include "console.h"

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_WATCH 0
#define LOG_BREAK 0
#define LOG_APERTURE 0

// If present, will set the startup state of audio, etc.
// See the distributed one for all settings.
#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"
#define TIMING_FILE "/tmp/pidp1-timing.txt"

// Extended timing histograms.
// Each is an array of bucket edges in ns:
// Bucket 0 counts values below the first edge.
// Bbucket i counts [edge[i-1], edge[i]).
// The final bucket counts everything at or above the last edge.
// A histogram has (number of edges + 1) buckets.
// Cycle and gap times use 1-2-5 steps, which puts 5us, one machine cycle, exactly on an edge.
#define TIMING_CYCLE_EDGES 17
// Throttle sleeps are nominally usleep(1000), fine steps just above 1ms show timer slack and
// wakeup latency, coarse ones above that show the preempted tail.
#define TIMING_SLEEP_EDGES 10
// Lag is how far simtime trails the wall clock at the start of a cycle.
#define TIMING_LAG_EDGES 10
#define TIMING_LABEL_SIZE 32        // room for one formatted bucket label, e.g. "1.02ms"
#define TIMING_STATUS_FILE "/proc/thread-self/status"   // this thread's context switch counts

#define NIL 0
#define Edge(sw) (pdp->sw && !prev_##sw)

void configure(void);
void reconfigure(void);
void sigReconfigure(int);

extern void updateswitches(PDP1 *pdp, Panel *panel);
extern void updatelights(PDP1 *pdp, Panel *panel);
extern void lightsoff(Panel *panel);
extern void lightson(Panel *panel);
extern Panel *getpanel(void);
extern void setLightpenRadius2(int screenNo, int radius2);

ConfigurationP getConfiguration(void);     // so other stuff can use our configuration, like IOTs

extern ConfigurationP loadConfigFile(char *filenameP);
extern void HSCreset(void);
extern bool processHSCchannels(bool mayTake);
extern bool hscBreakAllowed(void);
extern void hscBreak(PDP1 *pdp);
extern bool setDisplayFD(int screen, int fd);

static bool checkBreakpoints(PDP1 *pdp1P);
static bool checkWatches(PDP1 *pdp1P);
static u64 configThrottleDuration(char *nameP, u64 unitNs, u64 defaultNs);
static float configCutoff(char *nameP, float fallback);

static void timingRunStart(void);
static void timingNoteCycle(PDP1 *pdp, u64 startNs, u64 deltaNs);
static void timingNoteSleep(u64 wakeNs);
static void timingReport(FILE *fP);
static void timingReset(void);
static int timingBucket(u64 value, const u64 *edgesP, int numEdges);
static void timingLabel(u64 ns, char *bufP, size_t bufSize);
static void timingPrintHist(FILE *fP, const char *titleP, const long *histP, const u64 *edgesP, int numEdges);
static u64 timingThreadCpuNs(void);
static void timingCtxSwitches(long *voluntaryP, long *involuntaryP);

PDP1P pdp1P;      // Here because dynamic IOT code needs it

extern bool audioEnabled;
extern bool lailiaEnabled;
extern bool core1DEnabled;
extern bool all1DEnabled;
extern bool useMotionPrediction;

static bool timingEnabled;

static volatile sig_atomic_t reconfigRequested;     // SIGHUP synchronization, thread-safe

// All for audio
extern void setFilterCutoff(int, float);
extern void setMixerGain(float);
extern float getMixerGain(void);
extern void setAudioTuning(float);
extern float getAudioTuning(void);
extern void setSampleRate(int);
extern int getSampleRate(void);
extern int getOverflowData(int *);

ConfigurationP configurationP;  // from the config file

// Used to track how long a cycle actually takes if timing is enabled
static long precycleTime;
static long cycleDeltaTime;
static long slowestTime;
static long fastestTime = LONG_MAX;
static long overflowCount;
static long totalTime;
static long totalCycles;

// Extended timing (11-Sep-2026, see the file header).
// All of it is gathered only while pidp1timing is on and the machine is running, written by
// timingReport() after the one-line summary above, and cleared by timingReset().
static const u64 cycleEdges[TIMING_CYCLE_EDGES] =
{
    250, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
    1000000, 2000000, 5000000, 10000000, 20000000, 50000000
};
static const u64 sleepEdges[TIMING_SLEEP_EDGES] =
{
    1000000, 1020000, 1050000, 1100000, 1200000, 1500000, 2000000, 5000000, 10000000, 20000000
};
static const u64 lagEdges[TIMING_LAG_EDGES] =
{
    500000, 1000000, 1500000, 2000000, 3000000, 5000000, 10000000, 20000000, 50000000, 100000000
};
static long cycleHist[TIMING_CYCLE_EDGES + 1];      // every timed cycle
static long firstCycleHist[TIMING_CYCLE_EDGES + 1]; // only the first cycle after a throttle sleep
static long gapHist[TIMING_CYCLE_EDGES + 1];        // untimed loop work between two cycles, no sleep between
static long sleepHist[TIMING_SLEEP_EDGES + 1];      // wall length of each throttle sleep
static long lagHist[TIMING_LAG_EDGES + 1];          // simtime lag at the start of every timed cycle
static u64 lastCycleEnd;        // gettime() at the end of the previous timed cycle, 0 = none yet this run
static bool sleptSinceCycle;    // a throttle sleep came after the previous timed cycle
static u64 gapTotal;            // sum of the gapHist samples, ns
static long gapCount;           // number of gapHist samples
static long stealCycles;        // timed cycles that were high-speed-channel steals, not cycle()
static long burstCount;         // completed bursts, runs of cycles between two throttle sleeps
static long burstCycles;        // cycles so far in the current burst
static long burstCyclesMin;
static long burstCyclesMax;
static long burstCyclesTotal;
static u64 burstStart;          // wall time the current burst began (the last wakeup), 0 = none yet
static u64 burstWallTotal;      // wall ns of all completed bursts
static u64 burstWallMax;
static long sleepCount;         // throttle sleeps seen while running
static u64 sleepTotal;          // wall ns of all of them
static u64 sleepMin;
static u64 sleepMax;
static long long lagMax;        // largest lag seen, ns (signed, simtime can be ahead of the wall clock)
static u64 runStartWall;        // gettime() at the first timed cycle of this run
static u64 runStartCpu;         // emulator thread CPU time then, ns
static long runStartVoluntary;  // this thread's voluntary context switches then, -1 if unknown
static long runStartInvoluntary;// and involuntary ones (preemptions), -1 if unknown

// The main emulator loop.
// Runs forever or until exit() or the SIGTERM handler ends the process.
// Each iteration applies any pending remote debugger overrides of the front-panel
// switches and services start/stop/continue/examine/deposit/readin switch edges via
// spec()/cycle()/start_readin()/readin1()/readin2(), runs one machine cycle  or steals one for an active
// high-speed-channel DMA transfer when pdp->run is set, services the panel lights in all states,
// then services file-descriptor-backed I/O via handleio() and dynamicIotProcessorDoIOPoll(), and
// the console's cli(), which applies a staged load and passes stdin lines to the console thread.
// This function never returns normally.
void
emu(PDP1 *pdp, Panel *panel)
{
bool prev_start_sw;
bool prev_stop_sw;
bool prev_continue_sw;
bool prev_examine_sw;
bool prev_deposit_sw;
bool prev_readin_sw;
bool prev_power_sw;

FILE *tmpfP;    // used for timing
u64 realtimeBefore;     // pdp->realtime before throttle(); it changes only if throttle() slept
u64 passStartSim;       // simtime at the top of a powered pass
u64 passNs;             // the pass's simtime up to throttle(), its mul/div time, not the lag cap
bool ran;               // the pass ran the machine, a stolen cycle included

    pdp->panel = panel;
    pwrclr(pdp);
    updateswitches(pdp, panel);

    inittime();
    pdp->simtime = gettime();
    timingReset();          // gives the extended timing minimums/maximums their starting values

    for(;;)
    {
        if( reconfigRequested )
        {
            reconfigRequested = 0;
            reconfigure();
        }

        prev_start_sw = pdp->start_sw;
        prev_stop_sw = pdp->stop_sw;
        prev_continue_sw = pdp->continue_sw;
        prev_examine_sw = pdp->examine_sw;
        prev_deposit_sw = pdp->deposit_sw;
        prev_readin_sw = pdp->readin_sw;
        prev_power_sw = pdp->power_sw;
        updateswitches(pdp, panel);

        // Serve the network debugger's requests here, between cycles and just before the flag
        // handling below, so a start, stop or step it asks for takes effect in this same pass.
        if( ad1WorkPending() )
        {
            ad1Service(pdp);
        }

        // Override with any AD1 operations

        // This one stays on until cleared by ad1 or a real switch, be sure it's first
        if( AD1_SINGLE(pdp1P) )
        {
            if(Edge(start_sw) || Edge(continue_sw) )
            {
                AD1_CLEAR_SINGLE(pdp1P);
            }
            else
            {
                pdp->single_inst_sw = 1;
            }
        }

        // A start, stop or continue that arrives in the pass right after the previous one was
        // consumed would otherwise find the previous value of its switch still set and see no
        // edge, so each one forces its own edge, as the stop already did.
        if( AD1_START(pdp1P) )
        {
            prev_start_sw = 0;
            pdp->start_sw = 1;
            pdp->ta = pdp->ad1StartAddr;
            pdp->eta = pdp->ad1ExtendedAddr;
            AD1_CLEAR_START(pdp1P);
        }

        if( AD1_STOP(pdp1P) )
        {
            prev_stop_sw = 0;
            pdp->stop_sw = 1;
            AD1_CLEAR_STOP(pdp1P);
        }

        if( AD1_CONTINUE(pdp1P) )
        {
            prev_continue_sw = 0;
            pdp->continue_sw = 1;
        }

        if( pdp->power_sw )
        {
            passStartSim = pdp->simtime;
            ran = false;

            // The power-off branch stopped the sound; it comes back if audio is on.
            // A device never opened stays closed, as at start-up.
            if( Edge(power_sw) && audioEnabled && isAudioInitialized() )
            {
                continueaudio();
            }

            if(Edge(start_sw) || Edge(continue_sw) || Edge(examine_sw) || Edge(deposit_sw))
            {
                // We don't check for a bp hit until spec() runs, it sets the pc
                spec(pdp1P);
                HSCreset();
                dynamicIotProcessorStart();
                logger(LOG_BREAK, "Spec-cycle PC %06o\n", pdp->epc | pdp->pc);
                if( checkBreakpoints(pdp) || checkWatches(pdp) )
                {
                    pdp->run_enable = 0;
                    ad1NoteHit(pdp);
                }

                // A start, etc.
                cycle(pdp1P);
                AD1_CLEAR_CONTINUE(pdp1P);
            }

            if( Edge(stop_sw) )
            {
                pdp->run_enable = 0;
                // Technically, the high speed channels aren't reset until a start, etc. above occur, but
                // high speed processing needs to be stopped anyway, so do it now also.
                HSCreset();
                dynamicIotProcessorStop();
            }

            if( Edge(readin_sw) )
            {
                HSCreset();         // Stop any transfer, makes no sense to run if reloading memoery
                dynamicIotProcessorStart(); // might be in stop state, run switch won't be toggled
                start_readin(pdp);
            }

            if( pdp->rim_cycle )
            {
                readin1(pdp);
            }

            if( pdp->rim_return && (--pdp->rim_return == 0) && pdp->rim )
            {
                // restart after reader is done
                if( (IR == 0) && !pdp->stop_sw )
                {
                    readin2(pdp);
                }
                else if(IR_DIO)
                {
                    cycle(pdp);
                    pdp->rim_cycle = 1;
                }
            }

            if(pdp->run)
            {
                ran = true;

                if( audioEnabled )                   // handle new audio stream
                {
                    svc_audio(pdp);
                }

                dynamicIotProcessorStart();          // let dyn IOTs know we transitioned to run

                logger(LOG_BREAK, "Pre-cycle PC %06o\n", pdp->epc | pdp->pc);
                if( checkBreakpoints(pdp) || checkWatches(pdp) )
                {
                    pdp->run_enable = 0;
                    ad1NoteHit(pdp);
                }

                if( timingEnabled )
                {
                    // First timed cycle of a run, snapshot thread CPU time and context switches.
                    // Done before precycleTime is taken so its /proc read is not charged to the cycle.
                    if( totalCycles == 0 )
                    {
                        timingRunStart();
                    }

                    precycleTime = gettime();
                }

                // A dma transfer can be in STEAL mode, in which case it effectively halts the processor
                // and transfers all of its requested words at 5us/word.
                // We fake this by just not cycling.
                if( processHSCchannels(hscBreakAllowed()) )   // need to steal a cycle
                {
                    hscBreak(pdp);                  // mid-instruction, restarts the instruction
                    updatelights(pdp, panel);
                    updatelights_pwm(panel, 1);     // tally one stolen cycle for new panel driver
                    if( timingEnabled )
                    {
                        ++stealCycles;
                    }
                }
                else
                {
                    cycle(pdp);
                    AD1_CLEAR_CONTINUE(pdp1P);      // if we were continuing, clear so ad1 knows we're done
                }

                if( timingEnabled )
                {
                    cycleDeltaTime = gettime() - precycleTime;
                    if( cycleDeltaTime > 5000 )
                    {
                        ++overflowCount;
                    }

                    if( cycleDeltaTime > slowestTime )
                    {
                        slowestTime = cycleDeltaTime;
                    }
                    if( cycleDeltaTime < fastestTime )
                    {
                        fastestTime = cycleDeltaTime;
                    }

                    totalTime += cycleDeltaTime;
                    ++totalCycles;

                    timingNoteCycle(pdp, (u64)precycleTime, (u64)cycleDeltaTime);
                }

                logger(LOG_BREAK, "Post-cycle PC %06o\n", pdp->epc | pdp->pc);
            }
            else
            {
                if( timingEnabled && totalCycles )
                {
                    if( (tmpfP = fopen(TIMING_FILE, "a")) )
                    {
                        setlocale(LC_NUMERIC,"en_US.utf-8");
                        fprintf(tmpfP,
                            "Fastest time %'ldns; slowest %'ldns; avg %'ldns; cycles %'ld; overflows %'ld:%4.02f%%\n",
                            fastestTime, slowestTime, totalTime/totalCycles, totalCycles,
                            overflowCount, ((float)overflowCount/(float)totalCycles) * 100.0);
                        timingReport(tmpfP);                // extended data, after the old line
                        fclose(tmpfP);
                    }
                    slowestTime = 0;
                    fastestTime = LONG_MAX;
                    totalTime = 0;
                    overflowCount = 0;
                    totalCycles = 0;
                    timingReset();
                }

                updatelights(pdp, panel);
                updatelights_pwm(panel, 1);     // tally one halted cycle for new panel driver
            }

            // throttle() moves simtime only through its lag cap, so measuring up to here leaves
            // out the span the cap forgives, the plugins' deadline time bases do not count it.
            passNs = pdp->simtime - passStartSim;

            realtimeBefore = pdp->realtime;
            throttle(pdp);

            // throttle() only refreshes pdp->realtime after a sleep, so a change means it slept.
            // Only tracked inside a timed run (totalCycles is cleared when the run's report is written).
            if( timingEnabled && totalCycles && (pdp->realtime != realtimeBefore) )
            {
                timingNoteSleep(pdp->realtime);
            }

            handleio(pdp);
            // 19-Jun-2026 wje: independent real-time poll hook for reader/punch/typewriter-style
            // dynamic IOTs (iotIOPoll), called at the same site as handleio() so plugin-owned
            // devices keep their cadence regardless of run state.
            // See dynamicIots.h/.c.
            dynamicIotProcessorDoIOPoll(pdp);
            pdp->simtime += 5000;
            dynamicIotProcessorAdvance(pdp, (passNs + 5000), ran);
        }
        else
        {
            stopaudio();

            // This branch runs on every pass until the power switch is onr.
            // IOTs get powerclear once at the off edge and before the stop notification/
            // A device that has not been stopped can still finish with its files.
            // Any 16-channel special SBS enable a program has done via an IOT will be
            // reset to the config file setting.
            if( prev_power_sw )
            {
                dynamicIotProcessorPowerClear();
                pdp->sbs16 = configurationP->sbs16Enabled;
            }

            dynamicIotProcessorStop();
            pwrclr(pdp);

            /* magic key combo used for shutdown */
            if( pdp->start_sw && pdp->readin_sw )
            {
                lightson(panel);
                sleep(1);
                exit(100);
            }

            lightsoff(panel);
            pdp->simtime = gettime();
        }

        cli(pdp);
    }
}

// Extended pidp1timing support.
// The throttle() code in pdp1.c runs the CPU in bursts, flat out until simtime leads the
// wall clock by throttleburst, 100us by default, then a wait chunked to at most throttlequantum,
// 1000us by default.
// These functions record what the one-line summary cannot show, how cycle times are distributed,
// whether slow cycles are the first after a sleep, how much untimed
// loop work sits between cycles, how long the sleeps really are, how far simtime trails the wall
// clock, and how much CPU and how many preemptions the emulator thread took.
// All run on the emulator thread only.

// Snapshot the start of a timed run: wall time, the emulator thread's CPU time and its context
// switch counts, so timingReport() can give this run's deltas.
static void
timingRunStart(void)
{
    runStartWall = gettime();
    runStartCpu = timingThreadCpuNs();
    timingCtxSwitches(&runStartVoluntary, &runStartInvoluntary);
}

// Record one timed cycle that started at wall time startNs and took deltaNs of C time.
// Also records the untimed gap since the previous cycle but only when no throttle sleep came
// between since that gap would just be the sleep, and the simtime lag at the cycle's start.
static void
timingNoteCycle(PDP1 *pdp, u64 startNs, u64 deltaNs)
{
long long lag;

    ++(cycleHist[timingBucket(deltaNs, cycleEdges, TIMING_CYCLE_EDGES)]);

    if( sleptSinceCycle )
    {
        // First cycle of a burst, kept separately to test whether slow cycles follow sleeps
        // rather than landing anywhere.
        ++(firstCycleHist[timingBucket(deltaNs, cycleEdges, TIMING_CYCLE_EDGES)]);
    }
    else if( lastCycleEnd )
    {
        // Everything the main loop does between cycles, switch and ad1 handling, breakpoint
        // checks, handleio(), IO polls, cli(), plus any preemption that lands here.
        ++(gapHist[timingBucket((startNs - lastCycleEnd), cycleEdges, TIMING_CYCLE_EDGES)]);
        gapTotal += (startNs - lastCycleEnd);
        ++gapCount;
    }

    sleptSinceCycle = false;
    lastCycleEnd = (startNs + deltaNs);
    ++burstCycles;

    // Positive lag, simtime is behind the wall clock, the throttle's normal state right after a
    // sleep, which it then runs flat out to close.
    // Negative lag, simtime is ahead.
    lag = ((long long)startNs - (long long)pdp->simtime);
    if( lag > lagMax )
    {
        lagMax = lag;
    }

    ++(lagHist[(lag < 0)?0:timingBucket((u64)lag, lagEdges, TIMING_LAG_EDGES)]);
}

// Record a throttle sleep that ended (woke) at wall time wakeNs.
// The sleep began when the previous timed cycle ended, the loop does nothing measurable between
// the end of a cycle and throttle().
// A sleep also ends a burst, measured from the previous/ wakeup to the end of the burst's last cycle.
// Cycles before a run's first sleep are not a complete burst and are not counted as one.
static void
timingNoteSleep(u64 wakeNs)
{
u64 sleptNs;
u64 burstWallNs;

    if( !lastCycleEnd )
    {
        return;     // no timed cycle yet this run, so no known start for the sleep
    }

    sleptNs = (wakeNs - lastCycleEnd);
    ++sleepCount;
    sleepTotal += sleptNs;
    if( sleptNs < sleepMin )
    {
        sleepMin = sleptNs;
    }

    if( sleptNs > sleepMax )
    {
        sleepMax = sleptNs;
    }

    ++(sleepHist[timingBucket(sleptNs, sleepEdges, TIMING_SLEEP_EDGES)]);

    if( burstStart )
    {
        burstWallNs = (lastCycleEnd - burstStart);
        ++burstCount;
        burstCyclesTotal += burstCycles;
        burstWallTotal += burstWallNs;
        if( burstCycles < burstCyclesMin )
        {
            burstCyclesMin = burstCycles;
        }

        if( burstCycles > burstCyclesMax )
        {
            burstCyclesMax = burstCycles;
        }

        if( burstWallNs > burstWallMax )
        {
            burstWallMax = burstWallNs;
        }
    }

    burstStart = wakeNs;
    burstCycles = 0;
    sleptSinceCycle = true;
}

// Append the extended timing data for the run that just ended to fP, an open writable stream.
// Called on the emulator thread.
static void
timingReport(FILE *fP)
{
u64 wallNs;
u64 cpuNs;
long voluntary;
long involuntary;
int policy;
const char *policyP;
struct sched_param param;
time_t clock;
struct tm localTm;
char stamp[64];

    wallNs = (gettime() - runStartWall);
    cpuNs = (timingThreadCpuNs() - runStartCpu);
    timingCtxSwitches(&voluntary, &involuntary);

    // The emulator thread's scheduling class, matters because the panel driver's threads run
    // SCHED_FIFO and the Type 30 display worker runs SCHED_RR when they have the privilege.
    policy = sched_getscheduler(0);
    if( policy == SCHED_FIFO )
    {
        policyP = "SCHED_FIFO";
    }
    else if( policy == SCHED_RR )
    {
        policyP = "SCHED_RR";
    }
    else if( policy == SCHED_OTHER )
    {
        policyP = "SCHED_OTHER";
    }
    else
    {
        policyP = "other";
    }

    param.sched_priority = 0;
    (void)sched_getparam(0, &param);

    clock = time(NULL);
    stamp[0] = '\0';
    if( localtime_r(&clock, &localTm) )
    {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &localTm);
    }

    fprintf(fP, "  run ended %s; wall %.3fs; %ld CPUs online; emulator thread %s priority %d\n",
        stamp, ((double)wallNs / 1e9), sysconf(_SC_NPROCESSORS_ONLN), policyP, param.sched_priority);

    // CPU time covers ALL of the thread's loop work, timed or not, so CPU per cycle is the
    // real host cost of one simulated 5us cycle.
    // Wall percent is the thread's share of a core.
    fprintf(fP, "  emulator thread CPU %.3fs (%.1f%% of wall), %lluns per cycle\n",
        ((double)cpuNs / 1e9),
        ((wallNs) ? (((double)cpuNs * 100.0) / (double)wallNs):0.0),
        (unsigned long long)((totalCycles) ? (cpuNs / (u64)totalCycles):0));

    if( (voluntary >= 0) && (involuntary >= 0) && (runStartVoluntary >= 0) && (runStartInvoluntary >= 0) )
    {
        fprintf(fP, "  emulator thread context switches: %ld involuntary (preempted), %ld voluntary (slept or blocked)\n",
            (involuntary - runStartInvoluntary), (voluntary - runStartVoluntary));
    }
    else
    {
        fprintf(fP, "  emulator thread context switches: not available (%s unreadable)\n", TIMING_STATUS_FILE);
    }

    fprintf(fP, "  hsc steal cycles %ld\n", stealCycles);
    timingPrintHist(fP, "cycle", cycleHist, cycleEdges, TIMING_CYCLE_EDGES);
    timingPrintHist(fP, "first cycle after a sleep", firstCycleHist, cycleEdges, TIMING_CYCLE_EDGES);

    fprintf(fP, "  gap (untimed loop work between cycles, no sleep between): avg %lluns\n",
        (unsigned long long)((gapCount) ? (gapTotal / (u64)gapCount) : 0));
    timingPrintHist(fP, "gap", gapHist, cycleEdges, TIMING_CYCLE_EDGES);

    if( burstCount )
    {
        fprintf(fP, "  bursts %ld: cycles avg %ld min %ld max %ld; wall avg %lluus max %lluus\n",
            burstCount, (burstCyclesTotal / burstCount), burstCyclesMin, burstCyclesMax,
            (unsigned long long)((burstWallTotal / (u64)burstCount) / 1000),
            (unsigned long long)(burstWallMax / 1000));
    }
    else
    {
        fprintf(fP, "  bursts 0\n");
    }

    if( sleepCount )
    {
        fprintf(fP, "  throttle sleeps %ld: avg %lluus min %lluus max %lluus\n",
            sleepCount,
            (unsigned long long)((sleepTotal / (u64)sleepCount) / 1000),
            (unsigned long long)(sleepMin / 1000),
            (unsigned long long)(sleepMax / 1000));
    }
    else
    {
        fprintf(fP, "  throttle sleeps 0\n");
    }

    timingPrintHist(fP, "sleep", sleepHist, sleepEdges, TIMING_SLEEP_EDGES);

    fprintf(fP, "  lag (wall clock minus simtime at cycle start): max %lldus\n",
        ((lagMax == LLONG_MIN) ? 0LL : (lagMax / 1000)));
    timingPrintHist(fP, "lag", lagHist, lagEdges, TIMING_LAG_EDGES);

    // How often the throttle's/ lag cap forgave simulated time instead of running an
    // unpaced burst to catch up.
    // A nonzero count here is the only sign that this happened, since a
    // firing does not show up as a burst or a slow cycle.
    fprintf(fP, "  throttle lag-cap firings %ld\n", throttleCapFirings);
}

// Clear all extended timing data and give minimums and maximums their starting values.
// Also clears the per-device data in dynamicIots.c in case its report was never written.
static void
timingReset(void)
{
    memset(cycleHist, 0, sizeof(cycleHist));
    memset(firstCycleHist, 0, sizeof(firstCycleHist));
    memset(gapHist, 0, sizeof(gapHist));
    memset(sleepHist, 0, sizeof(sleepHist));
    memset(lagHist, 0, sizeof(lagHist));
    lastCycleEnd = 0;
    sleptSinceCycle = false;
    gapTotal = 0;
    gapCount = 0;
    stealCycles = 0;
    burstCount = 0;
    burstCycles = 0;
    burstCyclesMin = LONG_MAX;
    burstCyclesMax = 0;
    burstCyclesTotal = 0;
    burstStart = 0;
    burstWallTotal = 0;
    burstWallMax = 0;
    sleepCount = 0;
    sleepTotal = 0;
    sleepMin = UINT64_MAX;
    sleepMax = 0;
    lagMax = LLONG_MIN;
    runStartWall = 0;
    runStartCpu = 0;
    runStartVoluntary = -1;
    runStartInvoluntary = -1;
    throttleCapFirings = 0;
}

// Find the histogram bucket for value given numEdges ascending bucket edges.
// Returns 0 for a value below the first edge, i for a value in
// [edge[i-1], edge[i]), and numEdges for a value at or above the last edge.
static int
timingBucket(u64 value, const u64 *edgesP, int numEdges)
{
int i;

    for( i = 0; i < numEdges; ++i )
    {
        if( value < edgesP[i] )
        {
            return(i);
        }
    }

    return(numEdges);
}

// Format a duration of ns nanoseconds into bufP as a short label in the largest whole unit
// (ns, us or ms), with two decimals only when it is not a whole number of that unit.
static void
timingLabel(u64 ns, char *bufP, size_t bufSize)
{
    if( ns < 1000 )
    {
        snprintf(bufP, bufSize, "%lluns", (unsigned long long)ns);
    }
    else if( ns < 1000000 )
    {
        if( (ns % 1000) == 0 )
        {
            snprintf(bufP, bufSize, "%lluus", (unsigned long long)(ns / 1000));
        }
        else
        {
            snprintf(bufP, bufSize, "%.2fus", ((double)ns / 1e3));
        }
    }
    else
    {
        if( (ns % 1000000) == 0 )
        {
            snprintf(bufP, bufSize, "%llums", (unsigned long long)(ns / 1000000));
        }
        else
        {
            snprintf(bufP, bufSize, "%.2fms", ((double)ns / 1e6));
        }
    }
}

// Write one histogram to fP on a single line: its title and sample total, then every non-empty
// bucket as "<edge:count(percent)" for the lowest bucket or "edge+:count(percent)" for the rest,
// where edge is the bucket's lower bound.
// HistP has numEdges + 1 buckets.
static void
timingPrintHist(FILE *fP, const char *titleP, const long *histP, const u64 *edgesP, int numEdges)
{
int i;
long total;
char label[TIMING_LABEL_SIZE];

    total = 0;
    for( i = 0; i <= numEdges; ++i )
    {
        total += histP[i];
    }

    fprintf(fP, "  %s histogram (%ld):", titleP, total);
    if( total == 0 )
    {
        fprintf(fP, " empty\n");
        return;
    }

    for( i = 0; i <= numEdges; ++i )
    {
        if( histP[i] == 0 )
        {
            continue;
        }

        if( i == 0 )
        {
            timingLabel(edgesP[0], label, sizeof(label));
            fprintf(fP, " <%s:%ld", label, histP[i]);
        }
        else
        {
            timingLabel(edgesP[i - 1], label, sizeof(label));
            fprintf(fP, " %s+:%ld", label, histP[i]);
        }

        fprintf(fP, "(%.3f%%)", (((double)histP[i] * 100.0) / (double)total));
    }

    fprintf(fP, "\n");
}

// Returns the calling thread's CPU time (user plus system) in ns, or 0 if it can't be read.
static u64
timingThreadCpuNs(void)
{
struct timespec tm;

    if( clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tm) != 0 )
    {
        return(0);
    }

    return( (((u64)tm.tv_sec) * 1000000000ULL) + (u64)tm.tv_nsec );
}

// Read the calling thread's voluntary and involuntary context switch counts from
// /proc/thread-self/status into *voluntaryP and *involuntaryP.
// Either is set to -1 if the file or its line can't be read.
// Involuntary switches are preemptions, another thread took
// the core while this one could still run.
static void
timingCtxSwitches(long *voluntaryP, long *involuntaryP)
{
FILE *statusP;
char line[256];
long value;

    *voluntaryP = -1;
    *involuntaryP = -1;
    if( !(statusP = fopen(TIMING_STATUS_FILE, "r")) )
    {
        return;
    }

    while( fgets(line, sizeof(line), statusP) )
    {
        if( sscanf(line, "voluntary_ctxt_switches: %ld", &value) == 1 )
        {
            *voluntaryP = value;
        }
        else if( sscanf(line, "nonvoluntary_ctxt_switches: %ld", &value) == 1 )
        {
            *involuntaryP = value;
        }
    }

    fclose(statusP);
}

// Network command-port connection handler.
// Reads lines from fd, a connected socket, passes each to handlecmd() for processing,
// and writes the response back followed by a newline.
// Loops until the peer closes the connection then closes fd.
void
handlenetcmd(int fd, void *arg)
{
PDP1 *pdp = (PDP1*)arg;
char *r;
char line[1024];
int n;

    while( (n = read(fd, line, (sizeof(line) - 1))), n > 0 )
    {
        line[n] = 0;
        r = handlecmd(pdp, line);
        n = strlen(r);

        // handlecmd() returns a pointer to this thread's 1024-byte response buffer.
        // Only append our newline terminator when doing so cannot walk past the end
        // of that buffer, otherwise send/ the response as-is/
        if( (n + 2) <= 1024 )
        {
            r[n] = '\n';
            r[n + 1] = '\0';
            if( write(fd, r, (n + 1)) < 0 )
            {
                break;      // peer gone or socket error; stop feeding it more commands
            }
        }
        else
        {
            if( write(fd, r, n) < 0 )
            {
                break;
            }
        }
    }

    close(fd);
}

// Attaches a newly-connected display client's socket fd to display screen screenNo (0-3) and
// puts it in nonblocking mode.
void
connectdpy(int screenNo, int fd)
{
    nodelay(fd);
    setDisplayFD(screenNo, fd);
}

// Called when a connection request comes in on the screen-0 display port
// Hooks fd up as screen 0's display client via connectdpy().
void
handledpy(int fd, void *arg)
{
    (void)arg;      // signature matched to the pollfd accept-callback type; not needed here
    connectdpy(0, fd);
}

// Same as handledpy() above, but for display screen 1.
void
handledpy2(int fd, void *arg)
{
    (void)arg;
    connectdpy(1, fd);
}

// Same as handledpy() above, but for display screen 2.
void
handledpy3(int fd, void *arg)
{
    (void)arg;
    connectdpy(2, fd);
}

// Same as handledpy() above, but for display screen 3.
void
handledpy4(int fd, void *arg)
{
    (void)arg;
    connectdpy(3, fd);
}

// Called when a connection request comes in on the paper-tape-reader network port.
// Connects the newly-connected fd in the reader in place of whatever it had.
// The fd is made/ non-blockin, so a client that pauses or stops reading the echoes
// cannot hold the emulator thread, and it is handed over with mountReader() in pdp1.c.
// at its next pass.
void
handleptr(int fd, void *arg)
{
    (void)arg;      // signature matched to the pollfd accept-callback type; not needed here
    nodelay(fd);
    fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
    mountReader(fd);
}

// Same as handleptr() above, but for the paper-tape punch (p_fd) instead of the reader.
void
handleptp(int fd, void *arg)
{
    (void)arg;
    nodelay(fd);
    fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
    mountPunch(fd);
}

// Thread entry point that listens on all the network command/display/reader/punch ports
//  and dispatches accepted connections to the matching handler.
// Runs for the life of the process, serveN() only returns if its poll() fails.
// Returns nil, the thread's exit value is unused.
void*
netthread(void *arg)
{
    // The command port's handler keeps its connection, so it runs threaded.
    // The others just hand the fd over.
    struct PortHandler ports[] =
    {
        { 1040, handlenetcmd, 1 },
        // 1041 is typewriter
        { 1042, handleptr, 0 },
        { 1043, handleptp, 0 },
        { 3400, handledpy, 0 },
        { 3401, handledpy2, 0 },
        { 3402, handledpy3, 0 },
        { 3403, handledpy4, 0 },
    };
    serveN(ports, nelem(ports), arg);
    return( nil );
}

// Read a memory file produced by dumpmem() into working memory.
// Reworked by wje to keep the address and data on one line, much more readable.
void
readmem(const char *filenameP, Word *mem, Word size)
{
int val;
Word addr;
Word word;
char *strP;
FILE *fileP;
char buf[100];

    if( (fileP = fopen(filenameP, "r")) == NIL )
    {
        return;
    }

    // Handle both old and new file formats.
    // New is addr: word, old has each on a separate line.
    addr = 0;
    while( (strP = fgets(buf, 100, fileP)) )
    {
        val = strtol(strP, &strP, 8);
        if( strP && (*strP == ':') )
        {
            addr = val;         // address part
            if( addr >= size )
            {
                fprintf(stderr, "Address out of range: %o\n", addr);
                break;
            }
        }

        if( !strP || (*strP == '\n') )
        {
            // must be old format memory
            if( addr < size)
            {
                mem[addr] = val;
            }
        }
        else
        {
            // We'll assume the line is OK if we got the firt part
            word = strtol(strP+1, &strP, 8);
            if( addr < size)
            {
                mem[addr] = word;
            }
        }
    }

    fclose(fileP);
}

// Dump working memory to the memory image file.
// Reworked by wje to keep the address and data on one line, much more readable.
void
dumpmem(const char *fileNameP, Word *mem, Word size)
{
FILE *fP;
Word i;

    if( !(fP = fopen(fileNameP, "w")) )
    {
        return;
    }

    for(i = 0; i < size; i++)
    {
        if( mem[i] != 0 )
        {
            // Just put it on one line, jeez
            // Support for the old format removed by wje 7-Oct-2026.
            fprintf(fP, "%06o: %06o\n", i, mem[i]);
        }
    }

    fclose(fP);
}

// a bit ugly...
static Panel *panel;
static Word *memp;
static int memsz;

// Registered via atexit(), saves working memory to the coremem image file and turns the panel
// lights off.
void
exitcleanup(void)
{
    dumpmem("coremem", memp, memsz);
    lightsoff(panel);
}

// Signal handler registered for SIGTERM, exits cleanly (status 0), which triggers the
// atexit()-registered exitcleanup() above.
void
sighandler(int sig)
{
    (void)sig;
    exit(0);
}

#define LOCKFILE "/tmp/pdp1.lock"

// Only one pdp1 may run, a second one would drive the same panel lamps, take the first one's
// ports when it exits, and overwrite coremem at its exit.
// The lock is an flock() on a file in /tmp, the namespace the panel segment already shares.
// The fd stays open for the life of the process.
// The kernel drops the lock at any exit, SIGKILL included, so nothing stale is left.
// Returns false when another pdp1 holds the lock.
// A lock that can't be had for any other reason is reported and pdp1 runs
// as it did before there was a lock.
static bool
lockInstance(void)
{
int fd;
mode_t mask;

    // In a sticky /tmp, fs.protected_regular refuses an O_CREAT open of a file another user made,
    // even read-only, so an existing file is opened without O_CREAT and only a new one is made.
    // Read-only is enough for flock(), whoever owns the file.
    for( ;; )
    {
        fd = open(LOCKFILE, (O_RDONLY | O_CLOEXEC));
        if( (fd >= 0) || (errno != ENOENT) )
        {
            break;
        }

        mask = umask(0);
        fd = open(LOCKFILE, (O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC), 0666);
        umask(mask);
        if( (fd >= 0) || (errno != EEXIST) )
        {
            break;
        }
    }

    if( fd < 0 )
    {
        fprintf(stderr, "can't open %s (%s), not checking for another pdp1\n", LOCKFILE, strerror(errno));
        return( true );
    }

    if( flock(fd, (LOCK_EX | LOCK_NB)) < 0 )
    {
        if( errno == EWOULDBLOCK )
        {
            return( false );
        }
        fprintf(stderr, "can't lock %s (%s), not checking for another pdp1\n", LOCKFILE, strerror(errno));
        close(fd);
    }

    return( true );
}

// Startup, takes the one-pdp1 lock, finds the operator panel, installs signal
// handlers and the exitcleanup() atexit hook, loads configuration, loads the saved core memory
// image, posts the default reader and punch tapes, starts the polling/network/display threads
// and the debugger server, opens the typewriter fds, then calls emu() which runs forever.
// Returns 1 if another pdp1 is running or no operator panel could be found,
// otherwise returns 0, but only in the unreachable case where emu() ever
// returns, which it doesn't.
int
main(int argc, char *argv[])
{
PDP1 pdp1;
PDP1 *pdp = &pdp1;
pthread_t th;
const char *tape = "tapes/dpys5.rim";
int fd[2];

    // First of all, before the panel, the exit hook that writes coremem, or any port.
    if( !lockInstance() )
    {
        fprintf(stderr, "another pdp1 is already running\n");
        return 1;
    }

    pdp1P = pdp;
    panel = getpanel();

    if(panel == nil)
    {
        fprintf(stderr, "can't find operator panel\n");
        return 1;
    }

    atexit(exitcleanup);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, sigReconfigure);
    signal(SIGTERM, sighandler);

    configure();

    memset(pdp, 0, sizeof(*pdp));
    memp = pdp->core;
    memsz = MAXMEM;
    readmem("coremem", memp, memsz);

    // And set a few things we can't set until there is memory allocated
    pdp->muldiv_sw = configurationP->muldivEnabled;
    pdp->sbs16 = configurationP->sbs16Enabled;

    // Posted like any other mount and adopted at the first pass; opened non-blocking, so a FIFO
    // here can't hang the start.
    // Posted before any port listens or the console runs, a front end's or operator's mount
    // can only come later and replaces these, as at any other time.
    pdp->r_fd = -1;
    pdp->p_fd = -1;
    mountReaderPath(tape);
    mountPunchPath("punch.out");

    pthread_create(&th, NULL, netthread, pdp);
    ad1ServerStart(pdp, configurationP);
    consoleStart(pdp);

    pdp->typ_fd.id = -1;
    socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
    pdp->typ_fd.fd = fd[0];
    waitfd(&pdp->typ_fd);
    typtelnetInput(&pdp->typ_fd);
    typtelnet(1041, fd[1]);

    // Pre-load the tyi IOT 4 so its iotIOPoll is registered before the first
    // character arrives.
    dynamicIotOwnsDevice(4);

    emu(pdp, panel);
    return( 0 );   // can't happen
}

// Scan the breakpoint table to see if the passed address matches an enabled entry.
// If so, check the count and if reached, signal a breakpoint.
// Return true if a brekpoint was hit, else false.
static bool
checkBreakpoints(PDP1 *pdp1P)
{
int i;
uint32_t addr;      // matches BreakpointP's address field (ad1intf.h); addr is always
                     // non-negative anyway (masked to 17 bits below)
BreakpointP brkP;

    if( !AD1_BREAKPOINTS_ENABLED(pdp1P) )
    {
        return(false);
    }

    addr =  (pdp1P->epc | pdp1P->pc) & 0177777;
    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        if( (brkP->isSet) && (brkP->isEnabled) && (brkP->address == addr) )
        {
            brkP->curCount++;
            logger(LOG_BREAK, "breakpoint %d seen curcount %d\n", i+1, brkP->curCount);
            if( brkP->curCount >= brkP->count )    // a count of 0 or 1 are wquivalent
            {
                brkP->curCount = 0;     // for next time
                AD1_SET_BREAKPOINT_HIT(pdp1P);
                pdp1P->ad1brkNo = i;
                logger(LOG_BREAK, "breakpoint %d hit\n", i+1);
                return(true);
            }
        }
    }

    return(false);
}

// Scan the watch table to see if the passed address and its data match an enabled entry.
// If so, signal a watch and return true, else false.
static bool
checkWatches(PDP1 *pdp1P)
{
int i;
int curVal;
bool hit;
WatchP watchP;

    if( !AD1_WATCHES_ENABLED(pdp1P) )
    {
        return(false);
    }

    hit = false;
    watchP = pdp1P->ad1Watches;

    for( i = 0; i < AD1_NUM_WATCHES; ++i, ++watchP )
    {
        if( watchP->isSet && watchP->isEnabled )
        {
            curVal = pdp1P->core[watchP->address] & 0777777;
            if( curVal != watchP->lastVal )     // it was changed
            {
                if( watchP->onAny )
                {
                    hit = true;
                }
                else if( watchP->value == curVal )
                {
                    hit = true;
                }
            }

            if( watchP->onAny )
            {
                watchP->value = curVal;     // just so ad1 can report it
            }

            watchP->lastVal = curVal;
        }

        if( hit )
        {
            AD1_SET_WATCH_HIT(pdp1P);
            pdp1P->ad1watchNo = i;
            logger(LOG_WATCH, "watch %d hit\n", i+1);
            return(true);
        }
    }

    return(false);
}

// Accessor so other modules (including dynamic IOT plugins) can get at the loaded
// configuration without needing their own extern of configurationP.
// Returns the current ConfigurationP as set up by configure(), never NULL after startup.
ConfigurationP
getConfiguration()
{
    return( configurationP );
}

// Read a throttle* extra as a duration, in unitNs per
// config unit, or defaultNs if the key is absent, negative or not a plain number.
static u64
configThrottleDuration(char *nameP, u64 unitNs, u64 defaultNs)
{
ConfigurationSettingP settingP;

    settingP = findConfigurationSetting(configurationP, nameP);
    if( !settingP )
    {
        return( defaultNs );
    }

    if( settingP->strvalueP || (settingP->ivalue < 0) )
    {
        fprintf(stderr, "pidp1: %s must be a non-negative number; using %llu\n",
            nameP, (unsigned long long)(defaultNs / unitNs));
        return( defaultNs );
    }

    return( (u64)settingP->ivalue * unitNs );
}

// Read an audio cutoff extra, in Hz, or fallback if the key is absent or not a positive number.
// A fallback of 0 leaves the voice at the CHM interface's cutoff.
static float
configCutoff(char *nameP, float fallback)
{
ConfigurationSettingP settingP;
float hz;

    settingP = findConfigurationSetting(configurationP, nameP);
    if( !settingP )
    {
        return( fallback );
    }

    hz = (isnan(settingP->fvalue) ? (float)settingP->ivalue : settingP->fvalue);
    if( settingP->strvalueP || (hz <= 0.0f) )
    {
        fprintf(stderr, "pidp1: %s must be a positive number of Hz; ignored\n", nameP);
        return( fallback );
    }

    return( hz );
}

// Read the config file, set our various settings
void
configure()
{
int i;
ConfigurationSettingP configSettingP;

    configurationP = loadConfigFile(CONFIG_FILE);
    audioEnabled = configurationP->audioEnabled;
    lailiaEnabled = configurationP->lailiaEnabled;
    core1DEnabled = configurationP->core1DEnabled;
    all1DEnabled = configurationP->all1DEnabled;

    // This will only be used if called from sigint.
    // pdp1P won't be set yet in the prmary call from main()
    if( pdp1P )
    {
        pdp1P->muldiv_sw = configurationP->muldivEnabled;
        pdp1P->sbs16 = configurationP->sbs16Enabled;
    }

    setMixerGain(configurationP->gain);
    setAudioTuning(configurationP->tuning);
    setSampleRate(configurationP->sampleRate);

    // The filters are set in Hz, cutoff for all four voices, cutoff1-cutoff4 for one.
    // A voice with neither gets the CHM interface's cutoff.
    for( i = 1; i <= 4; ++i )
    {
        char nameBuf[16];

        snprintf(nameBuf, sizeof(nameBuf), "cutoff%d", i);
        setFilterCutoff(i, configCutoff(nameBuf, configCutoff("cutoff", 0.0f)));
    }
    for( i = 0; i < 5; ++i )
    {
        static char *alphaNames[5] = { "alpha", "alpha1", "alpha2", "alpha3", "alpha4" };

        if( findConfigurationSetting(configurationP, alphaNames[i]) )
        {
            fprintf(stderr, "pidp1: %s is no longer used; the filters are set by cutoff, in Hz "
                "(see pidp1.config.example)\n", alphaNames[i]);
        }
    }

    // Extra stuff that is local
    if( (configSettingP = findConfigurationSetting(configurationP, "pidp1timing")) )
    {
        timingEnabled = configSettingP->onOff;
    }

    // Control of the builtin exponential moving average filters for the lightpen
    if( (configSettingP = findConfigurationSetting(configurationP, "motionPrediction")) )
    {
        useMotionPrediction = configSettingP->onOff;
    }

    // Type 340 has its own aperture control
    if( (configSettingP = findConfigurationSetting(configurationP, "aperture")) )
    {
        i = configSettingP->ivalue;
        setLightpenRadius2(0, i * i);
        logger(LOG_APERTURE, "aperture %d\n",i);
    }

    // throttlequantum/throttlespin/throttlemaxlag 
    // parsed here, alongside the other config extras, then pushed into pdp1.c's throttle(),
    // which owns the pacing state and the timer-slack reduction that goes with it.
    // throttleConfigure() reads throttleburst itself, so its declaration in pdp1.h is unchanged.
    throttleConfigure(
        configThrottleDuration("throttlequantum", 1000, 1000000),
        configThrottleDuration("throttlespin", 1000, 0),
        configThrottleDuration("throttlemaxlag", 1000000, 20000000));
}

void
sigReconfigure(int sig)
{
    (void)sig;
    reconfigRequested = 1;
}

void
reconfigure(void)
{
    reloadConfigFile(CONFIG_FILE);
    configure();
    dynamicIotProcessorUpdate();
}
