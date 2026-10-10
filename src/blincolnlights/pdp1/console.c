/*
 * The operator's command console: the commands typed on the emulator's stdin and sent to the
 * command port (1040, main.c's handlenetcmd()), parsed and carried out by handlecmd().
 *
 * Threads. Nothing a command does runs on the emulator thread, so a command that blocks (a
 * connect to a display host, a FIFO, a slow file) cannot stop the CPU, the panel or the audio.
 * - cli() runs on the emulator thread every pass. Every CLIPASSES passes it reads a waiting
 *   stdin line and hands it to the console thread. While the console thread is busy the line
 *   stays in stdin.
 * - The console thread (consoleStart()) runs handlecmd() and prints the response.
 * - The command port calls handlecmd() on its network thread.
 * handlecmd() serializes itself, and each thread gets its own response buffer.
 *
 * The l command changes core, which the emulator thread owns. The command thread reads the RIM
 * tape into a staged image, and cli() applies it between two cycles, after stopping a running
 * machine the way ad1's load does (AD1_SET_STOP, then wait for run to drop).
 *
 * 27-Sep-2026 Claude moved out of pdp1.c; commands run off the emulator thread, and l stops the
 * machine before it changes core.
 * 10-Oct-2026 Claude handlecmd() parses the caller's line, it no longer gives split() a padded
 * copy since split() stops at the end of its string.
 * Its audio and display calls come from audio.h and display.h, not externs of its own.
*/

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "common.h"
#include "pdp1.h"
#include "audio.h"
#include "display.h"
#include "papertape.h"

#include "logger.h"
#define LOG_RIM 0

#define CLIPASSES 10000         // passes between stdin checks
#define CMDLINEMAX 1024         // longest command line, and the response buffer's size
#define RIMWORDS 010000         // a RIM tape's dio addresses are 12 bits
#define LOADSTOPNS 1000000000ULL    // how long l waits for a running machine to stop
#define LOADPOLLUS 1000         // how often the command thread checks on a staged load

// The staged load's states. POSTED is set by the command thread, DONE or NOSTOP by the emulator
// thread, and IDLE again by the command thread once it has read the result.
#define LOAD_IDLE 0
#define LOAD_POSTED 1
#define LOAD_DONE 2
#define LOAD_NOSTOP 3

typedef struct
{
    Word word[RIMWORDS];
    bool present[RIMWORDS];
} RimImage;

// Held for the whole of a command, so the command port and the console never interleave.
static pthread_mutex_t cmdLock = PTHREAD_MUTEX_INITIALIZER;

// The console thread's hand-off: consoleLine is written by cli() only while consoleBusy is 0,
// and read by the console thread only while it is 1.
static pthread_mutex_t consoleMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t consoleCond = PTHREAD_COND_INITIALIZER;
static char consoleLine[CMDLINEMAX];
static bool consolePosted;
static int consoleBusy;
static bool consoleStarted;
static bool stdinClosed;

// The staged l load. rimImage is written by the command thread before it posts, and read by
// the emulator thread after it sees LOAD_POSTED.
static RimImage rimImage;
static int loadState = LOAD_IDLE;
static bool loadWasRunning;

// Emulator thread only.
static bool loadStopIssued;
static u64 loadDeadline;

static int getwrd(int fd);
static int readRimImage(int fd, RimImage *imageP, int *badWordP);
static int stagedLoad(void);
static void serviceLoad(PDP1 *pdp);
static void *consoleThread(void *argP);

// Read one 18-bit word from a RIM-format tape: each word is encoded as three 6-bit characters,
// each with its high bit (0200) set as a frame marker; unframed bytes (leader, blank tape) before
// each character are skipped. Returns the word, or -1 at the end of the tape or on an error.
static int
getwrd(int fd)
{
u8 c;
int w, n;

    w = 0;
    n = 3;

    while( n-- )
    {
        do
        {
            if( read(fd, &c, 1) <= 0 )
            {
                return( -1 );
            }
        }
        while( (c & 0200) == 0 );

        w = ((w << 6) | (c & 077));
    }

    return( w );
}

// Read a RIM-format tape into *imageP: pairs of a dio word (0320000 | address) and its data
// word, ended by a jmp word (0600000 | start address). Returns the number of words staged. If
// the tape does not end with a jmp, *badWordP is the word that ended it (-1 at the end of the
// tape); otherwise it is 0. What was staged before a bad word is kept, as a partial load.
static int
readRimImage(int fd, RimImage *imageP, int *badWordP)
{
int inst;
int wd;
int count;

    memset(imageP, 0, sizeof(*imageP));
    *badWordP = 0;
    count = 0;

    for( ;; )
    {
        inst = getwrd(fd);

        if( (inst >= 0) && ((inst & 0760000) == 0320000) )
        {
            if( (wd = getwrd(fd)) < 0 )
            {
                *badWordP = -1;
                return( count );
            }

            imageP->word[inst & 07777] = (Word)wd;
            imageP->present[inst & 07777] = true;
            ++count;
        }
        else if( (inst >= 0) && ((inst & 0760000) == 0600000) )
        {
            logger(LOG_RIM, "start: %04o\n", inst & 07777);
            return( count );
        }
        else
        {
            logger(LOG_RIM, "rim botch: %06o\n", inst);
            *badWordP = inst;
            return( count );
        }
    }
}

// Hand the staged rimImage to the emulator thread and wait for it to be applied. Command thread
// only, with cmdLock held. Returns LOAD_DONE, or LOAD_NOSTOP if a running machine did not stop
// within LOADSTOPNS, in which case core is unchanged.
static int
stagedLoad(void)
{
int state;

    __atomic_store_n(&loadState, LOAD_POSTED, __ATOMIC_RELEASE);
    while( (state = __atomic_load_n(&loadState, __ATOMIC_ACQUIRE)) == LOAD_POSTED )
    {
        usleep(LOADPOLLUS);
    }

    __atomic_store_n(&loadState, LOAD_IDLE, __ATOMIC_RELAXED);
    return( state );
}

// Apply a staged load between two cycles. A running machine is stopped first, and the load
// waits for it over the passes that takes. Emulator thread only.
static void
serviceLoad(PDP1 *pdp)
{
int i;

    if( !loadStopIssued )
    {
        loadStopIssued = true;
        loadWasRunning = (pdp->run != 0);
        loadDeadline = (gettime() + LOADSTOPNS);
        if( pdp->run )
        {
            AD1_SET_STOP(pdp);
        }
    }

    if( pdp->run || AD1_STOP(pdp) )
    {
        if( gettime() > loadDeadline )
        {
            loadStopIssued = false;
            __atomic_store_n(&loadState, LOAD_NOSTOP, __ATOMIC_RELEASE);
        }

        return;
    }

    // Core is cleared first, as a RIM load always has, so nothing of the last program is left.
    for( i = 0; i < MAXMEM; ++i )
    {
        pdp->core[i] = 0;
    }

    for( i = 0; i < RIMWORDS; ++i )
    {
        if( rimImage.present[i] )
        {
            pdp->core[i] = rimImage.word[i];
        }
    }

    loadStopIssued = false;
    __atomic_store_n(&loadState, LOAD_DONE, __ATOMIC_RELEASE);
}

// Runs on the emulator thread every pass: applies a staged load, and every CLIPASSES passes
// hands a waiting stdin line to the console thread. A stdin that is not a terminal and has
// reached its end is not polled again.
void
cli(PDP1 *pdp)
{
static int timer;
int n;

    if( __atomic_load_n(&loadState, __ATOMIC_RELAXED) == LOAD_POSTED )
    {
        serviceLoad(pdp);
    }

    if( ++timer < CLIPASSES )
    {
        return;
    }

    timer = 0;
    if( !consoleStarted || stdinClosed || __atomic_load_n(&consoleBusy, __ATOMIC_ACQUIRE) || !hasinput(0) )
    {
        return;
    }

    n = read(0, consoleLine, (sizeof(consoleLine) - 1));
    if( n <= 0 )
    {
        if( (n == 0) && !isatty(0) )
        {
            stdinClosed = true;
        }

        return;
    }

    consoleLine[n] = '\0';
    __atomic_store_n(&consoleBusy, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&consoleMutex);
    consolePosted = true;
    pthread_cond_signal(&consoleCond);
    pthread_mutex_unlock(&consoleMutex);
}

// The console thread: runs each stdin line cli() hands it and prints the response.
static void *
consoleThread(void *argP)
{
PDP1 *pdp;
char line[CMDLINEMAX];
char *respP;

    pdp = (PDP1 *)argP;
    for( ;; )
    {
        pthread_mutex_lock(&consoleMutex);
        while( !consolePosted )
        {
            pthread_cond_wait(&consoleCond, &consoleMutex);
        }

        consolePosted = false;
        strcpy(line, consoleLine);
        pthread_mutex_unlock(&consoleMutex);

        respP = handlecmd(pdp, line);
        printf("%s\n", respP);
        fflush(stdout);
        __atomic_store_n(&consoleBusy, 0, __ATOMIC_RELEASE);
    }

    return( nil );
}

// Start the console thread. Until it runs, cli() leaves stdin alone. Called once, from main().
void
consoleStart(PDP1 *pdp)
{
pthread_t th;

    if( pthread_create(&th, nil, consoleThread, pdp) == 0 )
    {
        consoleStarted = true;
    }
}

// Parse and execute one console command line. Supported commands:
//   r [file]         mount/unmount the paper-tape reader
//   p [file]         mount/unmount the paper-tape punch
//   l [file]         stop the machine and load a RIM-format tape into core
//   d [host] [port]  connect to an external display program
//   muldiv [on/off]  toggle the type-10 multiply/divide option
//   audio ...        configure/query the audio output subsystem
//   ?/help           list commands
// Any thread but the emulator's. Writes into line: its first CR and its first LF end it.
// Returns the calling thread's own response buffer.
char *
handlecmd(PDP1 *pdp, char *line)
{
int n;
int fd;
int state;
int count;
int badWord;
float alpha;
char *p;
char **args;
int overflows[8];

static char *hostP;
static int port = 3400;
static char *rimfile = nil;
static __thread char resp[CMDLINEMAX];

    pthread_mutex_lock(&cmdLock);

    if( (p = strchr(line, '\r')) )
    {
        *p = '\0';
    }

    if( (p = strchr(line, '\n')) )
    {
        *p = '\0';
    }

    args = split(line, &n);

    strcpy(resp, "ok");

    if( n > 0 )
    {
        // reader
        if( strcmp(args[0], "r") == 0 )
        {
            state = mountReaderPath(args[1]);
            if( state == TAPE_FAILED )
            {
                snprintf(resp, sizeof(resp), "couldn't open %s", args[1]);
            }
            else if( state == TAPE_WAITING )
            {
                snprintf(resp, sizeof(resp), "ok, %s is a FIFO: mounted when a writer opens it", args[1]);
            }
        }
        // punch
        else if( strcmp(args[0], "p") == 0 )
        {
            if( mountPunchPath(args[1]) == TAPE_FAILED )
            {
                snprintf(resp, sizeof(resp), "couldn't open %s", args[1]);
            }
        }
        // load
        else if( strcmp(args[0], "l") == 0 )
        {
            if( args[1] )
            {
                free(rimfile);
                rimfile = strdup(args[1]);
            }

            if( !rimfile )
            {
                snprintf(resp, sizeof(resp), "no filename");
            }
            else if( (fd = open(rimfile, O_RDONLY)) < 0 )
            {
                snprintf(resp, sizeof(resp), "couldn't open %s", rimfile);
            }
            else
            {
                count = readRimImage(fd, &rimImage, &badWord);
                close(fd);

                if( stagedLoad() == LOAD_NOSTOP )
                {
                    snprintf(resp, sizeof(resp), "the machine did not stop, nothing loaded");
                }
                else
                {
                    p = resp;
                    p += snprintf(p, sizeof(resp), "ok%s", (loadWasRunning ? ", machine stopped first" : ""));
                    if( badWord == -1 )
                    {
                        snprintf(p, (sizeof(resp) - (p - resp)), ", tape ended after %d words", count);
                    }
                    else if( badWord )
                    {
                        snprintf(p, (sizeof(resp) - (p - resp)), ", rim botch %06o after %d words", badWord, count);
                    }
                }
            }
        }
        // display
        else if( strcmp(args[0], "d") == 0 )
        {
            if( args[1] )
            {
                free(hostP);
                hostP = strdup(args[1]);

                if( args[2] )
                {
                    port = atoi(args[2]);
                }
            }

            setDisplayFD(0, dial((hostP ? hostP : "localhost"), port));
            fd = getDisplayFD(0);
            if( fd < 0 )
            {
                strcpy(resp, "can't open display");
            }
            else
            {
                nodelay(fd);
            }
        }
        else if( (strcmp(args[0], "?") == 0) || (strcmp(args[0], "help") == 0) )
        {
            p = resp;
            p += sprintf(p, "r                     unmount tape from reader\n");
            p += sprintf(p, "r filename            mount tape in reader\n");
            p += sprintf(p, "p                     unmount tape from punch\n");
            p += sprintf(p, "p filename            mount tape in punch\n");
            p += sprintf(p, "l filename            stop the machine, load memory from RIM-file\n");
            p += sprintf(p, "d [host] [port]       connect to display program\n");
            p += sprintf(p, "muldiv [on/off]       set/toggle type 10 mul-div option\n");
            p += sprintf(p, "audio [on/off]        set/toggle audio output");
        }
        else if( strcmp(args[0], "muldiv") == 0 )
        {
            if( args[1] )
            {
                if( (strcmp(args[1], "on") == 0) || (strcmp(args[1], "1") == 0) )
                {
                    pdp->muldiv_sw = 1;
                }
                else if( (strcmp(args[1], "off") == 0) || (strcmp(args[1], "0") == 0) )
                {
                    pdp->muldiv_sw = 0;
                }

                resp[0] = '\0';
            }
            else
            {
                pdp->muldiv_sw = !pdp->muldiv_sw;
            }

            snprintf(resp, sizeof(resp), "mul-div now %s", (pdp->muldiv_sw ? "on" : "off"));
        }
        else if( strcmp(args[0], "audio") == 0 )
        {
            resp[0] = '\0';

            if( args[1] )
            {
                if( (strcmp(args[1], "on") == 0) || (strcmp(args[1], "1") == 0) )
                {
                    audioEnabled = 1;
                }
                else if( (strcmp(args[1], "off") == 0) || (strcmp(args[1], "0") == 0) )
                {
                    audioEnabled = 0;
                }
                else if( strcmp(args[1], "query") == 0 )
                {
                    snprintf(resp, sizeof(resp),
            "Audio %s, cutoff1 %.1f Hz, cutoff2 %.1f Hz, cutoff3 %.1f Hz, cutoff4 %.1f Hz, gain %f, tuning %f sample rate %d",
                        (audioEnabled ? "on" : "off"),
                        getFilterCutoff(1),
                        getFilterCutoff(2),
                        getFilterCutoff(3),
                        getFilterCutoff(4),
                        getMixerGain(),
                        getAudioTuning(),
                        getSampleRate());
                }
                else if( strcmp(args[1], "overflow") == 0 )
                {
                    n = getOverflowData(overflows);
                    snprintf(resp, sizeof(resp), "Overflows %d, high %d, low %d, samples %d",
                        n, overflows[0], overflows[1], overflows[2]);
                }
                // The filters are set in Hz. cutoff is all four voices, cutoff1-cutoff4 one; 0
                // puts back the CHM interface's own cutoff. An alpha is the old per-sample value,
                // taken as the cutoff it gives at the current sample rate.
                else if( ((strncmp(args[1], "cutoff", 6) == 0) || (strncmp(args[1], "alpha", 5) == 0)) && args[2] )
                {
                    bool isAlpha = (args[1][0] == 'a');
                    char *suffixP = (args[1] + (isAlpha ? 5 : 6));
                    int voiceNum = ((*suffixP == '\0') ? 0 : atoi(suffixP));

                    alpha = (float)atof(args[2]);
                    if( (voiceNum < 0) || (voiceNum > 4) || ((voiceNum == 0) && (*suffixP != '\0')) )
                    {
                        snprintf(resp, sizeof(resp), "No such voice: %s", args[1]);
                    }
                    else
                    {
                        setFilterCutoff(voiceNum, (isAlpha ? alphaToCutoff(alpha) : alpha));
                        if( voiceNum == 0 )
                        {
                            snprintf(resp, sizeof(resp), "Cutoff for all channels now %.1f, %.1f, %.1f, %.1f Hz",
                                getFilterCutoff(1), getFilterCutoff(2), getFilterCutoff(3), getFilterCutoff(4));
                        }
                        else
                        {
                            snprintf(resp, sizeof(resp), "Cutoff channel %d now %.1f Hz", voiceNum, getFilterCutoff(voiceNum));
                        }
                    }
                }
                else if( (strcmp(args[1], "gain") == 0) && args[2] )
                {
                    alpha = atof(args[2]);
                    setMixerGain(alpha);
                    snprintf(resp, sizeof(resp), "Mixer gain now %f", alpha);
                }
                else if( (strcmp(args[1], "tuning") == 0) && args[2] )
                {
                    alpha = atof(args[2]);
                    setAudioTuning(alpha);
                    snprintf(resp, sizeof(resp), "Tuning now %f", alpha);
                }
                else if( (strcmp(args[1], "rate") == 0) && args[2] )
                {
                    n = atoi(args[2]);
                    setSampleRate(n);
                    snprintf(resp, sizeof(resp), "Sample rate now %d", n);
                }
            }
            else
            {
                audioEnabled = !audioEnabled;
            }

            if( audioEnabled )
            {
                if( isAudioInitialized() )
                {
                    continueaudio();
                }
                else
                {
                    initaudio();
                    startaudio();
                }
            }
            else
            {
                // stopaudio() is the emulator thread's; this thread asks.
                postaudio(false);
            }

            if( !resp[0] )
            {
                snprintf(resp, sizeof(resp), "Audio is %s, use query to see more details.", (audioEnabled ? "on" : "off"));
            }
        }
    }

    free(args[0]);
    free(args);
    pthread_mutex_unlock(&cmdLock);

    return( resp );
}
