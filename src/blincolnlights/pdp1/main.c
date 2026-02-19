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

#define DOLOGGING
#include "logger.h"
// Set desired log type to 1 to enable output assuming logging is defined.
#define LOG_SHM 1

// If present, will set the startup state of audio, lightpen support, etc.
// See the distributed one for all settings.
#define CONFIG_FILE "/opt/pidp1-mods/pidp1.config"
#define SHM_NAME "/pidp1"

#define Edge(sw) (pdp1P->sw && !prev_##sw)

int lightpenListener(PDP1P pdp1P);
void updateswitches(PDP1P pdp1P, Panel *panel);
void updatelights(PDP1P pdp1P, Panel *panel);
void lightsoff(Panel *panel);
void lightson(Panel *panel);
void loadConfigFile(PDP1P pdp1P, char *filenameP);
Panel *getpanel(void);

// Completely reworked to malloc if needed for shared mem support
PDP1 pdp1;
PDP1P pdp1P;
char *argv0;

static Panel *panel;
static Word *memp;
static int memsz;
static bool useShm;

extern int penAperture;
extern int penRadius2;
extern bool lightpenEnabled;
extern bool sdbEnabled;
extern bool dpyShiftEnabled;
extern bool audioEnabled;

void
emu(PDP1P pdp1P, Panel *panel)
{
bool prev_start_sw;
bool prev_stop_sw;
bool prev_continue_sw;
bool prev_examine_sw;
bool prev_deposit_sw;
bool prev_readin_sw;

    pdp1P->panel = panel;
    pwrclr(pdp1P);
    updateswitches(pdp1P, panel);

    inittime();
    pdp1P->simtime = gettime();
    pdp1P->dpy[0].last = pdp1P->simtime;
    pdp1P->dpy[1].last = pdp1P->simtime;
    pdp1P->dpy[0].ncmds = 0;
    pdp1P->dpy[1].ncmds = 0;

    for(;;)
    {
        prev_start_sw = pdp1P->start_sw;
        prev_stop_sw = pdp1P->stop_sw;
        prev_continue_sw = pdp1P->continue_sw;
        prev_examine_sw = pdp1P->examine_sw;
        prev_deposit_sw = pdp1P->deposit_sw;
        prev_readin_sw = pdp1P->readin_sw;
        updateswitches(pdp1P, panel);

        if(pdp1P->power_sw)
        {
            if(Edge(start_sw) || Edge(continue_sw) || Edge(examine_sw) || Edge(deposit_sw))
            {
                spec(pdp1P);
                cycle(pdp1P);
            }

            if(Edge(stop_sw))
            {
                pdp1P->run_enable = 0;
            }

            if(Edge(readin_sw))
            {
                start_readin(pdp1P);
            }

            if(pdp1P->rim_cycle)
            {
                readin1(pdp1P);
            }

            if(pdp1P->rim_return && (--pdp1P->rim_return == 0) && pdp1P->rim)
            {
                // restart after reader is done
                if((IR == 0) && !pdp1P->stop_sw)
                {
                    readin2(pdp1P);
                }
                else if(IR_DIO)
                {
                    cycle(pdp1P);
                    pdp1P->rim_cycle = 1;
                }
            }

            if(pdp1P->run)
            {
                if(audioEnabled)                           // wje - handle new audio stream
                {
                    svc_audio(pdp1P);
                }

                dynamicIotProcessorStart();          // wje - let dyn IOTs know we transitioned to run

                // A dma transfer can be in STEAL mode, in which case it effectively halts the processor
                // and transfers all of its requested words at 5us/word. We fake this by just not cycling.
                while(processHSChannels(pdp1P))        // wje - handle dma and see if we need to give up cycles
                {
                    updatelights(pdp1P, panel);
                    pdp1P->simtime += 5000;
                    throttle(pdp1P);
                }

                cycle(pdp1P);
            }
            else
            {
                dynamicIotProcessorStop();           // wje - let dyn IOTs know we transitioned to stop
                updatelights(pdp1P, panel);
            }

            throttle(pdp1P);
            handleio(pdp1P);
            pdp1P->simtime += 5000;
        }
        else
        {
            stopaudio();
            pwrclr(pdp1P);

            /* magic key combo used for shutdown */
            if(pdp1P->start_sw && pdp1P->readin_sw)
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
            pdp1P->simtime = gettime();
        }

        agedisplay(pdp1P, 0);
        agedisplay(pdp1P, 1);
        cli(pdp1P);
    }
}

void
handlenetcmd(int fd, void *arg)
{
PDP1P pdp1P = (PDP1P)arg;
char *r;
char line[1024];
int n;

    while((n = read(fd, line, sizeof(line))), n > 0)
    {
        line[n] = 0;
        r = handlecmd(pdp1P, line);
        n = strlen(r);
        r[n] = '\n';
        r[n + 1] = '\0';
        write(fd, r, strlen(r));
    }

    close(fd);
}

void
connectdpy(PDP1P pdp1P, DispCon *d, int fd)
{
    if(d->fd >= 0)
    {
        close(fd);
    }
    else
    {
        d->fd = fd;
        d->last = pdp1P->simtime;
        d->agetime = 50 * 1000;
        nodelay(d->fd);
    }
}

// Called when a connection request comes in
void
handledpy(int fd, void *arg)
{
pthread_t lp_thread;
PDP1P pdp1P = (PDP1P)arg;

    connectdpy(pdp1P, &pdp1P->dpy[0], fd);

    if(lightpenEnabled)
    {
        pthread_create(&lp_thread, NULL, lightpenListener, pdp1P);
    }
}

void
handledpy2(int fd, void *arg)
{
PDP1P pdp1P = (PDP1P)arg;

    connectdpy(pdp1P, &pdp1P->dpy[1], fd);
}

void
handleptr(int fd, void *arg)
{
PDP1P pdp1P = (PDP1P)arg;

    close(pdp1P->r_fd);
    pdp1P->r_fd = fd;
    nodelay(pdp1P->r_fd);
}

void
handleptp(int fd, void *arg)
{
PDP1P pdp1P = (PDP1P)arg;

    close(pdp1P->p_fd);
    pdp1P->p_fd = fd;
    nodelay(pdp1P->p_fd);
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
    return(nil);
}

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

    if((f = fopen(file, "r")), f == nil)
    {
        return;
    }

    a = 0;

    while(s = fgets(buf, 100, f))
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

    if((f = fopen("coremem", "w")), f == nil)
    {
        return;
    }

    a = 0;

    for(i = 0; i < size; i++)
    {
        if(mem[i] != 0)
        {
            a = i;
            fprintf(f, "%06o:\n", a);
            fprintf(f, "%06o\n", mem[a++]);
        }
    }

    fclose(f);
}

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
loadConfigFile(PDP1P pdp1P, char *filenameP)
{
int i;
bool onOff;
FILE *fP;
char line[256];
char option[64];
char answer[64];

    if(!(fP = fopen(filenameP, "r")))
    {
        return;
    }

    while(fgets(line, sizeof(line), fP))
    {
        if((line[0] == '#') || (line[0] == '\n'))
        {
            continue;
        }

        if((i = sscanf(line, "%[a-z0-9] = %[a-z0-9.]", option, answer)) != 2)
        {
            fprintf(stderr, "Invalid config file line %d, %s", i, line);
            continue;
        }

        onOff = !strcmp(answer, "y") || !strcmp(answer, "yes") || !strcmp(answer, "on");

        if(!strcmp(option, "audio"))
        {
            audioEnabled = onOff;
        }
        else if(!strcmp(option, "samplerate"))
        {
            setSampleRate(atoi(answer));
        }
        else if(!strcmp(option, "alpha"))
        {
            setFilterAlpha(atof(answer));
        }
        else if(!strcmp(option, "alpha1"))
        {
            setFilter1Alpha(atof(answer));
        }
        else if(!strcmp(option, "alpha2"))
        {
            setFilter2Alpha(atof(answer));
        }
        else if(!strcmp(option, "alpha3"))
        {
            setFilter3Alpha(atof(answer));
        }
        else if(!strcmp(option, "alpha4"))
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
        else if(!strcmp(option, "lightpen"))
        {
            lightpenEnabled = onOff;
        }
        else if(!strcmp(option, "aperture"))
        {
            penAperture = atoi(answer);
            penRadius2 = (penAperture / 2) * (penAperture / 2);
        }
        else if(!strcmp(option, "dpyshift"))
        {
            dpyShiftEnabled = onOff;
        }
        else if(!strcmp(option, "sdb"))
        {
            sdbEnabled = onOff;
        }
        else if(!strcmp(option, "sbs16"))
        {
            pdp1P->sbs16 = onOff;
        }
        else if(!strcmp(option, "muldiv"))
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
pthread_t th;
const char *host;
const char *tape = "tapes/dpys5.rim";
int port;
int fd[2];
int shmFd;

    argv0 = argv[0];

    // Assume local, not shared
    pdp1P = &pdp1;

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

    pdp1P->muldiv_sw = 1;
    loadConfigFile(pdp1P, CONFIG_FILE);

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
            pdp1P = mmap(NIL, sizeof(PDP1), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
            if( pdp1P == NIL )
            {
                logger(LOG_SHM, "mmap failed, using local memoryry\n");
                useShm = false;
                pdp1P = &pdp1;
            }
            else
            {
                memcpy(pdp1P, &pdp1, sizeof(PDP1));
            }

            close(shmFd);
            logger(LOG_SHM, "shared memory in use\n");
        }
    }

    memset(pdp1P, 0, sizeof(*pdp1P));
    memp = pdp1P->core;
    memsz = MAXMEM;
    readmem("coremem", memp, memsz);

    pdp1P->dpy[0].fd = -1;
    pdp1P->dpy[1].fd = -1;

    pthread_create(&th, NULL, netthread, pdp1P);
    pdp1P->r_fd = open(tape, O_RDONLY);
    pdp1P->p_fd = open("punch.out", O_CREAT | O_WRONLY | O_TRUNC, 0644);

    pdp1P->typ_fd.id = -1;
    socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
    pdp1P->typ_fd.fd = fd[0];
    waitfd(&pdp1P->typ_fd);
    typtelnet(1041, fd[1]);

    startpolling();
    emu(pdp1P, panel);
    return(0);     // can't happen
}
