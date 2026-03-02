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
*/
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "common.h"
#include "pdp1.h"
#include "args.h"
#include "panel_pidp1.h"    // wje - add, only a typedef was here, not the thing being typedefed!

#define NOTIOTH
#include "dynamicIots.h"
#include "highSpeedChannels.h"

//#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_SHM 0

// If present, will set the startup state of audio, lightpen support, etc.
// See the distributed one for all settings.
#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"
#define SHM_NAME "/pidp1"

#define NIL 0
#define Edge(sw) (pdp->sw && !prev_##sw)

void *lightpenListener(void *pdp);
void updateswitches(PDP1 *pdp, Panel *panel);
void updatelights(PDP1 *pdp, Panel *panel);
void lightsoff(Panel *panel);
void lightson(Panel *panel);
void loadConfigFile(PDP1 *pdp1P, char *filenameP);
Panel *getpanel(void);

PDP1P pdp1P;      // Here because dynamic IOT code needs it

extern int penAperture;
extern int penRadius2;
extern bool lightpenEnabled;
extern bool sdbEnabled;
extern bool dpyShiftEnabled;
extern bool audioEnabled;

static bool useShm;

void
emu(PDP1 *pdp, Panel *panel)
{
bool prev_start_sw;
bool prev_stop_sw;
bool prev_continue_sw;
bool prev_examine_sw;
bool prev_deposit_sw;
bool prev_readin_sw;

    pdp->panel = panel;
    pwrclr(pdp);
    updateswitches(pdp, panel);

    inittime();
    pdp->simtime = gettime();
    pdp->dpy[0].last = pdp->simtime;
    pdp->dpy[1].last = pdp->simtime;
    pdp->dpy[0].ncmds = 0;
    pdp->dpy[1].ncmds = 0;

    for(;;)
    {
        prev_start_sw = pdp->start_sw;
        prev_stop_sw = pdp->stop_sw;
        prev_continue_sw = pdp->continue_sw;
        prev_examine_sw = pdp->examine_sw;
        prev_deposit_sw = pdp->deposit_sw;
        prev_readin_sw = pdp->readin_sw;
        updateswitches(pdp, panel);

        if( pdp->power_sw )
        {
            if(Edge(start_sw) || Edge(continue_sw) ||
            AD1_START(pdp1P) || AD1_STEP(pdp1P) || AD1_CONTINUE(pdp1P) ||
                 Edge(examine_sw) || Edge(deposit_sw))
            {
                spec(pdp1P);
                cycle(pdp1P);
                AD1_CLEAR_START(pdp1P);
                AD1_CLEAR_STEP(pdp1P);
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
                if( audioEnabled )                         // wje - handle new audio stream
                {
                    svc_audio(pdp);
                }

                dynamicIotProcessorStart();          // wje - let dyn IOTs know we transitioned to run

                // A dma transfer can be in STEAL mode, in which case it effectively halts the processor
                // and transfers all of its requested words at 5us/word. We fake this by just not cycling.
                while(processHSChannels(pdp))        // wje - handle dma and see if we need to give up cycles
                {
                    updatelights(pdp, panel);
                    pdp->simtime += 5000;
                    throttle(pdp);
                }

                cycle(pdp);
            }
            else
            {
                dynamicIotProcessorStop();           // wje - let dyn IOTs know we transitioned to stop
                updatelights(pdp, panel);
            }

            throttle(pdp);
            handleio(pdp);
            pdp->simtime += 5000;
        }
        else
        {
            stopaudio();
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

        agedisplay(pdp, 0);
        agedisplay(pdp, 1);
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
connectdpy(PDP1 *pdp, DispCon *d, int fd)
{
    if(d->fd >= 0)
    {
        close(fd);
    }
    else
    {
        d->fd = fd;
        d->last = pdp->simtime;
        d->agetime = 50 * 1000;
        nodelay(d->fd);
    }
}

// Called when a connection request comes in
void
handledpy(int fd, void *arg)
{
pthread_t lp_thread;

    PDP1 *pdp = (PDP1*)arg;
    connectdpy(pdp, &pdp->dpy[0], fd);
    if( lightpenEnabled )
    {
        pthread_create(&lp_thread, NULL, lightpenListener, pdp);
    }
}

void
handledpy2(int fd, void *arg)
{
    PDP1 *pdp = (PDP1*)arg;
    connectdpy(pdp, &pdp->dpy[1], fd);
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

void
readmem(const char *file, Word *mem, Word size)
{
FILE *f;
char buf[100], *s;
Word a;
Word w;

    if( (f = fopen(file, "r")), f == nil)
    {
        return;
    }

    a = 0;

    while( s = fgets(buf, 100, f) )
    {
        while(*s)
        {
            if(*s == ';')
            {
                break;
            }
            else if('0' <= *s && *s <= '7')
            {
                w = strtol(s, &s, 8);

                if(*s == ':')
                {
                    a = w;
                    s++;
                }
                else if(a < size)
                {
                    mem[a++] = w;
                }
                else
                {
                    fprintf(stderr, "Address out of range: %o\n", a++);
                }
            }
            else
            {
                s++;
            }
        }
    }

    fclose(f);
}

void
dumpmem(const char *file, Word *mem, Word size)
{
    FILE *f;
    Word i, a;

    if( (f = fopen("coremem", "w")), f == nil )
    {
        return;
    }

    a = 0;

    for(i = 0; i < size; i++)
    {
        if( mem[i] != 0 )
        {
            a = i;
            fprintf(f, "%06o:\n", a);
            fprintf(f, "%06o\n", mem[a++]);
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


// The config file has a simple format.
// Lines starting with '#' are comments and are ignored.
// Empty lines are ignored.
// Otherwise, a line of the form 'xxx=yyy' is expected.
// Embedded spaces are ignored.
// The meaning of 'yyy' depends upon the option.
// For an option that is on or off, 'y', 'yes', or 'on' means enable, anything else means disable.
// Invalid lines and options are reported on stderr.
void
loadConfigFile(PDP1 *pdp1P, char *filenameP)
{
int i;
bool onOff;
FILE *fP;
char line[256];
char option[64];
char answer[64];

    if( !(fP = fopen(filenameP, "r")) )
    {
        return;
    }

    while( fgets(line, sizeof(line), fP) )
    {
        if( (line[0] == '#') || (line[0] == '\n') )
        {
            continue;
        }

        if( (i = sscanf(line, "%[a-z0-9] = %[a-z0-9.]", option, answer)) != 2 )
        {
            fprintf(stderr, "Invalid config file line %d, %s", i, line);
            continue;
        }

        onOff = !strcmp(answer,"y") || !strcmp(answer,"yes") || !strcmp(answer,"on");

        if( !strcmp(option,"audio") )
        {
            audioEnabled = onOff;
        }
        else if( !strcmp(option,"samplerate") )
        {
            setSampleRate(atoi(answer));
        }
        else if( !strcmp(option,"alpha") )
        {
            setFilterAlpha(atof(answer));
        }
        else if( !strcmp(option,"alpha1") )
        {
            setFilter1Alpha(atof(answer));
        }
        else if( !strcmp(option,"alpha2") )
        {
            setFilter2Alpha(atof(answer));
        }
        else if( !strcmp(option,"alpha3") )
        {
            setFilter3Alpha(atof(answer));
        }
        else if( !strcmp(option,"alpha4") )
        {
            setFilter4Alpha(atof(answer));
        }
        else if(strcmp(option, "gain") == 0)
        {
            setMixerGain(atof(answer));
        }
        else if(strcmp(option, "tuning") == 0)
        {
            setAudioTuning(atof(answer));
        }
        else if( !strcmp(option,"lightpen") )
        {
            lightpenEnabled = onOff;
        }
        else if( !strcmp(option,"aperture") )
        {
            penAperture = atoi(answer);
            penRadius2 = (penAperture/2) * (penAperture/2);
        }
        else if( !strcmp(option,"dpyshift") )
        {
            dpyShiftEnabled = onOff;
        }
        else if( !strcmp(option,"sdb") )
        {
            sdbEnabled = onOff;
        }
        else if( !strcmp(option,"sbs16") )
        {
            pdp1P->sbs16 = onOff;
        }
        else if( !strcmp(option,"muldiv") )
        {
            pdp1P->muldiv_sw = onOff;
        }
        else if(!strcmp(option, "shared"))
        {
            // Put the PDP1 struct in shared memory for use with other tools
            useShm = true;
        }
    }

    fclose(fP);
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
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    loadConfigFile(pdp, CONFIG_FILE);

    // Now check for shared mem use
    if( useShm )
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

    pdp->muldiv_sw = 1;
    startpolling();

    pdp->dpy[0].fd = -1;
    pdp->dpy[1].fd = -1;

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
