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
 * Claude 07-Oct-2026 the default reader and punch tapes are posted before any port listens so a front end's
 *    tape socket is no longer replaced by them.
 * Claude 07-Oct-2026 only the emulator thread drives the audio device; the console's audio requests are posted
 *    to it (audio.c, console.c).
 * Claude 08-Oct-2026 the sound comes back with the power if audio is on.
 * Claude 10-Oct-2026 the audio, display and panel calls come from audio.h, display.h and panel1.h, not
 *    externs of its own.
 * wje 2-Nov-2026 motion prediction control removed, it is now always enabled.
 * Claude 06-Nov-2026 the command port's run, cont and halt press the Start, Continue and Stop keys, and
 *    run ? reports the machine's state (console.c).
 * Claude 07-Nov-2026 the channel and PDP-1D calls and flags come from hscCore.h and pdp1core.h, not
 *    externs of its own.
 * Claude 08-Nov-2026 the network ports, the core image file and the pidp1timing report move to
 *    netPorts.c, coreImage.c and timingReport.c.
*/

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
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
#include "hscCore.h"
#include "pdp1core.h"
#include "ad1server.h"
#include "papertape.h"
#include "console.h"
#include "audio.h"
#include "display.h"
#include "panel1.h"
#include "netPorts.h"
#include "coreImage.h"
#include "timingReport.h"

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_WATCH 0
#define LOG_BREAK 0
#define LOG_APERTURE 0

// If present, will set the startup state of audio, etc.
// See the distributed one for all settings.
#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"

#define Edge(sw) (pdp->sw && !prev_##sw)

void configure(void);
void reconfigure(void);
void sigReconfigure(int);

ConfigurationP getConfiguration(void);     // so other stuff can use our configuration, like IOTs

static bool checkBreakpoints(PDP1 *pdp1P);
static bool checkWatches(PDP1 *pdp1P);
static u64 configThrottleDuration(char *nameP, u64 unitNs, u64 defaultNs);
static float configCutoff(char *nameP, float fallback);

PDP1P pdp1P;      // Here because dynamic IOT code needs it

static volatile sig_atomic_t reconfigRequested;     // SIGHUP synchronization, thread-safe

ConfigurationP configurationP;  // from the config file

// The main emulator loop.
// Runs forever or until exit() or the SIGTERM handler ends the process.
// Each iteration applies any pending remote debugger overrides of the front-panel
// switches and services start/stop/continue/examine/deposit/readin switch edges via
// spec()/cycle()/start_readin()/readin1()/readin2(), runs one machine cycle or steals one for an active
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
int keyRequest;         // a RUN_REQ_ value from the command port

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
        // handling below so a start, stop or step it asks for takes effect in this same pass.
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
        // edge so each one forces its own edge, as the stop already did.
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

        // The command port's run, cont and halt press the panel's keys, each forcing its edge as
        // ad1's do. A start keeps the ta updateswitches() read, so it starts at the address
        // switches as the Start key does. With the power off a key does nothing, and a forced
        // start there could meet the shutdown combination below, so a late request is dropped.
        keyRequest = consoleTakeRunRequest();
        if( pdp->power_sw )
        {
            if( keyRequest == RUN_REQ_START )
            {
                prev_start_sw = 0;
                pdp->start_sw = 1;
            }
            else if( keyRequest == RUN_REQ_CONTINUE )
            {
                prev_continue_sw = 0;
                pdp->continue_sw = 1;
            }
            else if( keyRequest == RUN_REQ_STOP )
            {
                prev_stop_sw = 0;
                pdp->stop_sw = 1;
            }
        }

        if( pdp->power_sw )
        {
            passStartSim = pdp->simtime;
            ran = false;

            // The power-off branch stopped the sound; it comes back if audio is on. A device never
            // opened stays closed, as at start-up, where the config's audio only allows the sound.
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
                // high speed processing needs to be stopped anyway, do it now.
                HSCreset();
                dynamicIotProcessorStop();
            }

            if( Edge(readin_sw) )
            {
                HSCreset();         // Stop any transfer, makes no sense to run if reloading memory
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
                    timingCycleBegin();
                }

                // A dma transfer can be in STEAL mode, in which case it effectively halts the processor
                // and transfers all of its requested words at 5us/word. We fake this by just not cycling.
                if( processHSCchannels(hscBreakAllowed()) )   // need to steal a cycle
                {
                    hscBreak(pdp);                  // mid-instruction, restarts the instruction
                    updatelights(pdp, panel);
                    updatelights_pwm(panel, 1);     // tally one stolen cycle for new panel driver
                    if( timingEnabled )
                    {
                        timingNoteSteal();
                    }
                }
                else
                {
                    cycle(pdp);
                    AD1_CLEAR_CONTINUE(pdp1P);      // if we were continuing, clear so ad1 knows we're done
                }

                if( timingEnabled )
                {
                    timingCycleEnd(pdp);
                }

                logger(LOG_BREAK, "Post-cycle PC %06o\n", pdp->epc | pdp->pc);
            }
            else
            {
                if( timingEnabled && timingRunActive() )
                {
                    timingRunEnd();
                }

                updatelights(pdp, panel);
                updatelights_pwm(panel, 1);     // tally one halted cycle for new panel driver
            }

            // throttle() moves simtime only through its lag cap so measuring up to here leaves
            // out the span the cap forgives, the plugins' deadline time bases do not count it.
            passNs = pdp->simtime - passStartSim;

            realtimeBefore = pdp->realtime;
            throttle(pdp);

            // throttle() only refreshes pdp->realtime after a sleep so a change means it slept.
            // Only tracked inside a timed run (totalCycles is cleared when the run's report is written).
            if( timingEnabled && timingRunActive() && (pdp->realtime != realtimeBefore) )
            {
                timingNoteSleep(pdp->realtime);
            }

            handleio(pdp);
            // 19-Jun-2026 wje: independent real-time poll hook for reader/punch/typewriter-style
            // dynamic IOTs (iotIOPoll), called at the same site as handleio() so plugin-owned
            // devices keep their cadence regardless of run state. See dynamicIots.h/.c.
            dynamicIotProcessorDoIOPoll(pdp);
            pdp->simtime += 5000;
            dynamicIotProcessorAdvance(pdp, (passNs + 5000), ran);
        }
        else
        {
            stopaudio();

            // This branch runs on every pass until the power switch is on.
            // IOTs get powerclear once at the off edge and before the stop notification.
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
// The kernel drops the lock at any exit, SIGKILL included so nothing stale is left.
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

    // Posted like any other mount and adopted at the first pass; opened non-blocking so a FIFO
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
uint32_t addr;       // matches BreakpointP's address field (ad1intf.h); addr is always
                     // non-negative anyway, masked to 17 bits below.
BreakpointP brkP;

    if( !AD1_BREAKPOINTS_ENABLED(pdp1P) )
    {
        return(false);
    }

    addr =  (pdp1P->epc | pdp1P->pc) & 0177777;
    brkP = pdp1P->ad1Breakpoints;

    for( i = 0; i < AD1_NUM_BREAKPOINTS; ++i, ++brkP )
    {
        // A pass with the condition false is not counted and the scan goes on so two
        // breakpoints at one address with different conditions each fire separately.
        if( (brkP->isSet) && (brkP->isEnabled) && (brkP->address == addr) &&
            ad1BreakConditionHolds(pdp1P, i) )
        {
            brkP->curCount++;
            logger(LOG_BREAK, "breakpoint %d seen curcount %d\n", i+1, brkP->curCount);
            if( brkP->curCount >= brkP->count )    // a count of 0 or 1 are equivalent
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
            // lastVal follows the word whatever the condition so a change made while the
            // condition is false (an overlay being loaded) is never reported later.
            if( (curVal != watchP->lastVal) && ad1WatchConditionHolds(pdp1P, i) )     // it was changed
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
    // pdp1P won't be set yet in the primary call from main()
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
