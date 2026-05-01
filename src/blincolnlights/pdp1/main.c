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
*/

#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "common.h"
#include "pdp1.h"
#include "args.h"
#include "panel_pidp1.h"    // wje - add, only a typedef was here, not the thing being typedefed!

#include "configuration.h"
#define NOTIOTH
#include "dynamicIots.h"
#include "highSpeedChannels.h"

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_SHM 0
#define LOG_WATCH 0
#define LOG_BREAK 0
#define LOG_APERTURE 0

// If present, will set the startup state of audio, etc.
// See the distributed one for all settings.
#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"
#define SHM_NAME "/pidp1"
#define TIMING_FILE "/tmp/pidp1-timing.txt"

#define NIL 0
#define Edge(sw) (pdp->sw && !prev_##sw)

void configure(void);
void reconfigure(int);

extern void updateswitches(PDP1 *pdp, Panel *panel);
extern void updatelights(PDP1 *pdp, Panel *panel);
extern void lightsoff(Panel *panel);
extern void lightson(Panel *panel);
extern Panel *getpanel(void);
extern void setLightpenRadius2(int screenNo, int radius2);

ConfigurationP getConfiguration(void);     // so other stuff can use our configuration, like IOTs

extern ConfigurationP loadConfigFile(char *filenameP);
extern bool processHSCchannels(void);
extern bool setDisplayFD(int screen, int fd);

static bool checkBreakpoints(PDP1 *pdp1P);
static bool checkWatches(PDP1 *pdp1P);

PDP1P pdp1P;      // Here because dynamic IOT code needs it

extern bool audioEnabled;
extern bool lailiaEnabled;
extern bool core1DEnabled;
extern bool all1DEnabled;

static bool timingEnabled;
static bool useShm;
static bool newMemFile;

// All for audio
extern void setFilterAlpha(float);
extern void setFilter1Alpha(float);
extern void setFilter2Alpha(float);
extern void setFilter3Alpha(float);
extern void setFilter4Alpha(float);
extern float getFilterAlpha(void);
extern float getFilter1Alpha(void);
extern float getFilter2Alpha(void);
extern float getFilter3Alpha(void);
extern float getFilter4Alpha(void);
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

void
emu(PDP1 *pdp, Panel *panel)
{
bool prev_start_sw;
bool prev_stop_sw;
bool prev_continue_sw;
bool prev_examine_sw;
bool prev_deposit_sw;
bool prev_readin_sw;

FILE *tmpfP;    // used for timing

    pdp->panel = panel;
    pwrclr(pdp);
    updateswitches(pdp, panel);

    inittime();
    pdp->simtime = gettime();

    for(;;)
    {
        prev_start_sw = pdp->start_sw;
        prev_stop_sw = pdp->stop_sw;
        prev_continue_sw = pdp->continue_sw;
        prev_examine_sw = pdp->examine_sw;
        prev_deposit_sw = pdp->deposit_sw;
        prev_readin_sw = pdp->readin_sw;
        updateswitches(pdp, panel);
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

        if( AD1_START(pdp1P) )
        {
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
            pdp->continue_sw = 1;
        }

        if( pdp->power_sw )
        {
            if(Edge(start_sw) || Edge(continue_sw) || Edge(examine_sw) || Edge(deposit_sw))
            {
                // We don't check for a bp hit until spec() runs, it sets the pc
                spec(pdp1P);
                logger(LOG_BREAK, "Spec-cycle PC %06o\n", pdp->epc | pdp->pc);
                if( checkBreakpoints(pdp) || checkWatches(pdp) )
                {
                    pdp->run_enable = 0;
                }
                cycle(pdp1P);
                AD1_CLEAR_CONTINUE(pdp1P);
            }

            if( Edge(stop_sw) )
            {
                pdp->run_enable = 0;
            }

            if( Edge(readin_sw) )
            {
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
                if( audioEnabled )                   // handle new audio stream
                {
                    svc_audio(pdp);
                }

                dynamicIotProcessorStart();          // let dyn IOTs know we transitioned to run

                logger(LOG_BREAK, "Pre-cycle PC %06o\n", pdp->epc | pdp->pc);
                if( checkBreakpoints(pdp) || checkWatches(pdp) )
                {
                    pdp->run_enable = 0;
                }

                if( timingEnabled )
                {
                    precycleTime = gettime();
                }

                // A dma transfer can be in STEAL mode, in which case it effectively halts the processor
                // and transfers all of its requested words at 5us/word. We fake this by just not cycling.
                if( processHSCchannels() )          // need to steal a cycle
                {
                    updatelights(pdp, panel);
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
                        fclose(tmpfP);
                    }
                    slowestTime = 0;
                    fastestTime = LONG_MAX;
                    totalTime = 0;
                    overflowCount = 0;
                    totalCycles = 0;
                }

                updatelights(pdp, panel);
            }

            throttle(pdp);
            handleio(pdp);
            pdp->simtime += 5000;
        }
        else
        {
            stopaudio();
            dynamicIotProcessorStop();
            pwrclr(pdp);

            /* magic key combo used for shutdown */
            if( pdp->start_sw && pdp->readin_sw )
            {
                lightson(panel);
                sleep(1);
                if( useShm )
                {
                    shm_unlink(SHM_NAME);
                }
                exit(100);
            }

            lightsoff(panel);
            pdp->simtime = gettime();
        }

        cli(pdp);
    }
}

void
handlenetcmd(int fd, void *arg)
{
PDP1 *pdp = (PDP1*)arg;
char *r;
char line[1024];
int n;

    while( (n = read(fd, line, sizeof(line))), n > 0 )
    {
        line[n] = 0;
        r = handlecmd(pdp, line);
        n = strlen(r);
        r[n] = '\n';
        r[n + 1] = '\0';
        write(fd, r, strlen(r));
    }

    close(fd);
}

void
connectdpy(int screenNo, int fd)
{
    nodelay(fd);
    setDisplayFD(screenNo, fd);
}

// Called when a connection request comes in
void
handledpy(int fd, void *arg)
{
    connectdpy(0, fd);
}

void
handledpy2(int fd, void *arg)
{
    connectdpy(1, fd);
}

void
handledpy3(int fd, void *arg)
{
    connectdpy(2, fd);
}

void
handledpy4(int fd, void *arg)
{
    connectdpy(3, fd);
}

void
handleptr(int fd, void *arg)
{
    PDP1 *pdp = (PDP1*)arg;
    close(pdp->r_fd);
    pdp->r_fd = fd;
    nodelay(pdp->r_fd);
}

void
handleptp(int fd, void *arg)
{
    PDP1 *pdp = (PDP1*)arg;
    close(pdp->p_fd);
    pdp->p_fd = fd;
    nodelay(pdp->p_fd);
}

void*
netthread(void *arg)
{
    struct PortHandler ports[] =
    {
        { 1040, handlenetcmd },
        // 1041 is typewriter
        { 1042, handleptr },
        { 1043, handleptp },
        { 3400, handledpy },
        { 3401, handledpy2 },
        { 3402, handledpy3 },
        { 3403, handledpy4 },
    };
    serveN(ports, nelem(ports), arg);
    return( nil );
}

char *argv0;
void
usage(void)
{
    fprintf(stderr, "usage: %s [-h host] [-p port]\n", argv0);
    exit(1);
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
dumpmem(const char *file, Word *mem, Word size)
{
FILE *f;
Word i;

    if( (f = fopen("coremem", "w")), f == nil )
    {
        return;
    }

    for(i = 0; i < size; i++)
    {
        if( mem[i] != 0 )
        {
            if( newMemFile )
            {
                // Just put it on one line, jeez
                fprintf(f, "%06o: %06o\n", i, mem[i]);
            }
            else
            {
                // Why 2 lines? Silly.
                fprintf(f, "%06o:\n", i);
                fprintf(f, "%06o\n", mem[i]);
            }
        }
    }

    fclose(f);
}

// a bit ugly...
static Panel *panel;
static Word *memp;
static int memsz;

void
exitcleanup(void)
{
    dumpmem("coremem", memp, memsz);
    lightsoff(panel);
    if( useShm )
    {
        shm_unlink(SHM_NAME);
    }
}

void
sighandler(int sig)
{
    exit(0);
}

int
main(int argc, char *argv[])
{
PDP1 pdp1;
PDP1 *pdp = &pdp1;
pthread_t th;
const char *host;
const char *tape = "tapes/dpys5.rim";
int port;
int fd[2];
int shmFd;

    host = "localhost";
    port = 3400;
    ARGBEGIN
    {
    case 'h':
        host = EARGF(usage());
        break;

    case 'p':
        port = atoi(EARGF(usage()));
        break;

    default:
        usage();
        break;
    } ARGEND;

    pdp1P = pdp;
    panel = getpanel();

    if(panel == nil)
    {
        fprintf(stderr, "can't find operator panel\n");
        return 1;
    }

    atexit(exitcleanup);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, reconfigure);
    signal(SIGTERM, sighandler);

    configure();

    // Now check for shared mem use
    if( configurationP->useShm )
    {
        shmFd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0666);
        if( shmFd < 0 )
        {
            logger(LOG_SHM, "shm_open failed, using local memory\n");
            useShm = false;
        }
        else
        {
            ftruncate(shmFd, sizeof(PDP1));
            pdp = mmap(NIL, sizeof(PDP1), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
            pdp1P = pdp;
            if( pdp == NIL )
            {
                logger(LOG_SHM, "mmap failed, using local memoryry\n");
                useShm = false;
                pdp = &pdp1;
            }
            else
            {
                memcpy(pdp, &pdp1, sizeof(PDP1));
            }

            close(shmFd);
            logger(LOG_SHM, "shared memory in use\n");
        }
    }

    memset(pdp, 0, sizeof(*pdp));
    memp = pdp->core;
    memsz = MAXMEM;
    readmem("coremem", memp, memsz);

    // And set a few things we can't set until there is memory allocated
    pdp->muldiv_sw = configurationP->muldivEnabled;
    pdp->sbs16 = configurationP->sbs16Enabled;

    startpolling();

    pthread_create(&th, NULL, netthread, pdp);

    pdp->r_fd = open(tape, O_RDONLY);
    pdp->p_fd = open("punch.out", O_CREAT | O_WRONLY | O_TRUNC, 0644);

    pdp->typ_fd.id = -1;
    socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
    pdp->typ_fd.fd = fd[0];
    waitfd(&pdp->typ_fd);
    typtelnet(1041, fd[1]);

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
int addr;
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

    for( i = 0; i < AD1_NUM_WATCHES; ++i )
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

        return(false);
    }
}

ConfigurationP
getConfiguration()     // so other stuff can use our configuration, like IOTs
{
    return( configurationP );
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
    useShm = configurationP->useShm;
    newMemFile = configurationP->newMemFile;

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
    setFilterAlpha(configurationP->alpha);
    setFilter1Alpha(configurationP->alpha1);
    setFilter2Alpha(configurationP->alpha2);
    setFilter3Alpha(configurationP->alpha3);
    setFilter4Alpha(configurationP->alpha4);

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
}

// Called on SIGHUP to reload config file
void
reconfigure(int sig)
{
    reloadConfigFile(CONFIG_FILE);
    configure();
}
