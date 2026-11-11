// The pidp1timing report: how long the emulator thread's cycles, the loop work between them, the
// throttle's sleeps and simtime's lag were over a run, appended to TIMING_FILE when the machine
// stops. emu() (main.c) makes every call, on the emulator thread, and only while timingEnabled is
// set (configure(), from the config's pidp1timing), apart from its first timingReset().

#include <limits.h>
#include <locale.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

#include "common.h"
#include "pdp1.h"
#include "timingReport.h"

#define TIMING_FILE "/tmp/pidp1-timing.txt"

// Extended timing histograms.
// Each is an array of bucket edges in ns:
// Bucket 0 counts values below the first edge.
// Bucket i counts [edge[i-1], edge[i]).
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

static void timingRunStart(void);
static void timingNoteCycle(PDP1 *pdp, u64 startNs, u64 deltaNs);
static void timingReport(FILE *fP);
static int timingBucket(u64 value, const u64 *edgesP, int numEdges);
static void timingLabel(u64 ns, char *bufP, size_t bufSize);
static void timingPrintHist(FILE *fP, const char *titleP, const long *histP, const u64 *edgesP, int numEdges);
static u64 timingThreadCpuNs(void);
static void timingCtxSwitches(long *voluntaryP, long *involuntaryP);

bool timingEnabled;

// Used to track how long a cycle actually takes if timing is enabled
static long precycleTime;
static long cycleDeltaTime;
static long slowestTime;
static long fastestTime = LONG_MAX;
static long overflowCount;
static long totalTime;
static long totalCycles;

// Extended timing.
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

// Start timing one cycle; the first timed cycle of a run also takes the run's start snapshot.
void
timingCycleBegin(void)
{
    // First timed cycle of a run, snapshot thread CPU time and context switches.
    // Done before precycleTime is taken so its /proc read is not charged to the cycle.
    if( totalCycles == 0 )
    {
        timingRunStart();
    }

    precycleTime = gettime();
}

// Count a timed cycle that was a high-speed channel's steal, not cycle().
void
timingNoteSteal(void)
{
    ++stealCycles;
}

// Record the cycle timingCycleBegin() started: the one-line summary's figures and the extended data.
void
timingCycleEnd(PDP1 *pdp)
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

// Returns true while a timed run has cycles not yet reported.
bool
timingRunActive(void)
{
    return( totalCycles != 0 );
}

// Append the run that just ended to TIMING_FILE, the one-line summary then the extended data, and
// clear everything for the next run.
void
timingRunEnd(void)
{
FILE *tmpfP;

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

// Extended pidp1timing support.
// The throttle() code in pdp1.c runs the CPU in bursts, flat out until simtime leads the
// wall clock by throttleburst, 10us by default, then a wait chunked to at most throttlequantum,
// 1000us by default.
// These functions record what the one-line summary cannot show, how cycle times are distributed,
// whether slow cycles are the first after a sleep, how much untimed
// loop work sits between cycles, how long the sleeps really are, how far simtime trails the wall
// clock, and how much CPU and how many preemptions the emulator thread took.
// All run on the emulator thread only.

// Snapshot the start of a timed run: wall time, the emulator thread's CPU time and its context
// switch counts so timingReport() can give this run's deltas.
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
// A sleep also ends a burst, measured from the previous wakeup to the end of the burst's last cycle.
// Cycles before a run's first sleep are not a complete burst and are not counted as one.
void
timingNoteSleep(u64 wakeNs)
{
u64 sleptNs;
u64 burstWallNs;

    if( !lastCycleEnd )
    {
        return;     // no timed cycle yet this run, no known start for the sleep
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

    // CPU time covers ALL of the thread's loop work, timed or not so CPU per cycle is the
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

    // How often the throttle's lag cap forgave simulated time instead of running an
    // unpaced burst to catch up.
    // A nonzero count here is the only sign that this happened, since a
    // firing does not show up as a burst or a slow cycle.
    fprintf(fP, "  throttle lag-cap firings %ld\n", throttleCapFirings);
}

// Clear all extended timing data and give minimums and maximums their starting values.
// Also clears pdp1.c's throttleCapFirings.
void
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
// where edge is the bucket's lower bound. histP has numEdges + 1 buckets.
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
