// SHRDLU, Terry Winograd's blocks-world dialogue of 1968-70, as a native Linux program.
//
// Why? Just as a simple demo to show what a modern cpu that you can buy for what, in todays's dollars,
// a single roll of paper tape for the tape punch would have cost.
//
// It operates the same way the PDP-1 assembly progam does, you can drive it with shrdlu_player also.
//
// The dialogue is via a telnet connection, on port 2042 by default.
// The block-world view is the point stream of the PiDP-1's Type 340 display, on port 3400 by
// default, for t30dpy.
//
// The Lisp of the original runs as C.
// The files code.c and image.c are generated from it.
//
// 2-Nov-2026 Claude/wje - initial version

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rt.h"
#include "scene.h"
#include "term.h"
#include "top.h"

#define TELNET_PORT 2042
#define DISPLAY_PORT 3400
#define STACK_BYTES ((size_t)1 << 30)
#define GUARD_BYTES ((size_t)1 << 20)
#define SIGNAL_STACK_BYTES ((size_t)1 << 16)

static int telnetPort = TELNET_PORT;
static int displayPort = DISPLAY_PORT;
static int fastHand;
static unsigned seed = 1;

static char *stackLowP;

// A fault in the stack's guard is a sentence recursing for good.
// It stops here.
// Any other fault is left to be the fault it is.
static void
faultHandler(int sig, siginfo_t *infoP, void *contextP)
{
    char *atP;

    (void)contextP;
    atP = (char *)infoP->si_addr;
    if((atP >= stackLowP) && (atP < (stackLowP + GUARD_BYTES)))
    {
        rtAbort("THE STACK IS FULL");
    }
    signal(sig, SIG_DFL);
}

// The thread the program runs on.
static void *
lispThread(void *argP)
{
    stack_t alt;
    struct sigaction action;

    (void)argP;
    alt.ss_sp = malloc(SIGNAL_STACK_BYTES);
    alt.ss_size = SIGNAL_STACK_BYTES;
    alt.ss_flags = 0;
    sigaltstack(&alt, NULL);
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = faultHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    rtInit(__builtin_frame_address(0));
    topInit(seed);
    termServe(telnetPort);
    return NULL;
}

static void
usage(const char *nameP)
{
    fprintf(stderr, "usage: %s [-p telnet-port] [-d display-port] [-f] [-s seed]\n", nameP);
    fprintf(stderr, "  -p port  the dialogue's telnet port, %d if not given\n", TELNET_PORT);
    fprintf(stderr, "  -d port  the display's port, %d if not given\n", DISPLAY_PORT);
    fprintf(stderr, "  -f       the hand jumps where it would walk\n");
    fprintf(stderr, "  -s seed  the random numbers' first state, 1 if not given\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    int opt;
    pthread_t thread;
    pthread_attr_t attr;
    void *stackP;

    while((opt = getopt(argc, argv, "p:d:fs:")) != -1)
    {
        switch(opt)
        {
        case 'p':
            telnetPort = atoi(optarg);
            break;
        case 'd':
            displayPort = atoi(optarg);
            break;
        case 'f':
            fastHand = 1;
            break;
        case 's':
            seed = (unsigned)strtoul(optarg, NULL, 0);
            break;
        default:
            usage(argv[0]);
        }
    }
    if(optind != argc)
    {
        usage(argv[0]);
    }
    signal(SIGPIPE, SIG_IGN);
    sceneInit(displayPort, fastHand);
    stackP = mmap(NULL, STACK_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if(stackP == MAP_FAILED)
    {
        perror("stack");
        return 1;
    }
    stackLowP = stackP;
    mprotect(stackP, GUARD_BYTES, PROT_NONE);
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, (char *)stackP + GUARD_BYTES, STACK_BYTES - GUARD_BYTES);
    pthread_create(&thread, &attr, lispThread, NULL);
    pthread_join(thread, NULL);
    return 0;
}
