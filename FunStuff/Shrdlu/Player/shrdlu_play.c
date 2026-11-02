// shrdlu_play.c -- a scripted telnet client for SHRDLU.
// It connects to the PDP-1/ program's telnet port or to the native C program,
// types a script's/ sentences one at a time and waits for each to finish,
// just as a person at a telnet terminal would.
//
// Stdout is the session record.
// The server echoes what is typed so the script's lines appear as the server prints them,
// followed by its replies, the hans moves as useal, and the * prompt prints.
// The telnet commands and NULs are taken out and a CR LF is mapped to a newline.
// The player's messages (step times, the total time, errors) go to stderr.
//
// A script is one step a line.
// A blank line and a line starting with # are skipped.
// A line starting with > answers a question the previous line caused to be asked.
// A line ending in ... is typed up to the dots and a space.
// The player then types a filler word, Return and Ctrl-U.
// This is needed for one sentence in the Winograd demo.
// A line starting with / is a SHRDLU / command and is sent as it is.
//
// Exit status 0 means every step ended, 1 a timeout or a closed connection, and 2 a
// script or usage error.
//
// 2-Nov-2026 Claude/wje - initial version.

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ANSWERS_MAX 8
#define LINE_CHARS 1024
#define SLICE_MS 100
#define STOP_CHAR_MS 20
#define STOP_PAUSE_MS 500
#define STOP_RETURN_MS 800
#define CHUNK_CHARS 4096

#define TELNET_IAC 255
#define TELNET_SE 240
#define TELNET_SB 250
#define TELNET_WILL 251
#define TELNET_DONT 254

#define CHAR_CTRL_U 025

#define STOP_FILLER "xyz"
#define STOP_TEXT "PLEASE TYPE RETURN AND CONTINUE THE SENTENCE.\n"
#define QUESTION_TEXT "DO YOU MEAN:\n"
#define REFUSED_TEXT "PLEASE TYPE ONE OF THE NUMBERS"

#define EXIT_RUN 1
#define EXIT_USAGE 2

typedef struct Step
{
    char *textP;                // what is typed; for a stop, up to the dots
    int stop;                   // the step ends at the program's stop, not at a *
    int lineNo;                 // the line of the script, for messages
    int nAnswers;
    char *answerP[ANSWERS_MAX];
} Step;

static const char *hostP = "localhost";
static const char *portP = "2042";
static double timeoutSec = 300.0;
static double gapSec;
static int charMs;
static int defaultAnswer = -1;
static int firstStep = 1;
static int stepLimit;
static int stepByStep;
static int quietRun;
static FILE *logFp;

static int sockFd = -1;
static int telnetState;
static int curStep;

// What the current step has shown so far, as text. It is the server's bytes after
// the telnet layer and the CRs are taken out. Searched for the closing *.
static char *bufP;
static size_t bufLen;
static size_t bufCap;

static Step *stepP;
static int nSteps;
static int stepCap;

static void
usage(void)
{
    fprintf(stderr,
        "usage: shrdlu_play [options] SCRIPT\n"
        "  -h HOST     the host (localhost)\n"
        "  -p PORT     the telnet port (2042)\n"
        "  -t SECONDS  the most to wait for one step to end (300)\n"
        "  -c MS       pause between typed characters (0)\n"
        "  -g SECONDS  pause after each step (0)\n"
        "  -a N        the answer to a question the script gives none for\n"
        "  -f N        start at the Nth step\n"
        "  -n M        stop after M steps\n"
        "  -s          wait for Return on stdin before each step\n"
        "  -l FILE     also write the session to FILE\n"
        "  -q          print nothing on stderr but errors\n");
    exit(EXIT_USAGE);
}

static double
nowSec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec + ((double)ts.tv_nsec / 1e9));
}

// Reports a failure on stderr, naming the step that was running, and ends the
// run with the given exit status. The session printed so far is flushed first.
static void
failRun(int status, const char *fmtP, ...)
{
    va_list ap;

    fflush(stdout);
    if(logFp)
    {
        fflush(logFp);
    }
    fprintf(stderr, "shrdlu_play: ");
    if(curStep > 0)
    {
        fprintf(stderr, "step %d: ", curStep);
    }
    va_start(ap, fmtP);
    vfprintf(stderr, fmtP, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(status);
}

// The session.

// Puts a byte of the session in the step's buffer, on stdout and in the log.
// A NUL is padding and a CR is half of a line end, so neither is shown.
static void
emit(unsigned char c)
{
    if((c == 0) || (c == '\r'))
    {
        return;
    }
    if((bufLen + 2) > bufCap)
    {
        bufCap = ((bufCap == 0) ? 4096 : (bufCap * 2));
        bufP = realloc(bufP, bufCap);
        if(bufP == NULL)
        {
            failRun(EXIT_RUN, "out of memory");
        }
    }
    bufP[bufLen++] = (char)c;
    bufP[bufLen] = 0;
    fputc(c, stdout);
    if(logFp)
    {
        fputc(c, logFp);
    }
}

// One byte from the server through the telnet layer: IAC commands and option
// negotiations are dropped, IAC IAC is a literal 255, a subnegotiation ends at
// IAC SE. The server is never answered, as the DCS2 side needs no reply.
static void
feed(unsigned char c)
{
    switch(telnetState)
    {
    case 0:
        if(c == TELNET_IAC)
        {
            telnetState = 1;
        }
        else
        {
            emit(c);
        }
        break;
    case 1:
        if(c == TELNET_IAC)
        {
            emit(c);
            telnetState = 0;
        }
        else if((c >= TELNET_WILL) && (c <= TELNET_DONT))
        {
            telnetState = 2;
        }
        else if(c == TELNET_SB)
        {
            telnetState = 3;
        }
        else
        {
            telnetState = 0;
        }
        break;
    case 2:
        telnetState = 0;
        break;
    case 3:
        if(c == TELNET_IAC)
        {
            telnetState = 4;
        }
        break;
    default:
        telnetState = ((c == TELNET_SE) ? 0 : 3);
        break;
    }
}

// Waits up to ms for the server and shows what comes. Returns 1 if bytes came,
// 0 if none did, -1 if the server closed the connection.
static int
pump(int ms)
{
    struct pollfd pfd;
    unsigned char chunk[CHUNK_CHARS];
    ssize_t n;
    int r;
    int k;

    pfd.fd = sockFd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, ms);
    if(r < 0)
    {
        if(errno == EINTR)
        {
            return 0;
        }
        failRun(EXIT_RUN, "poll: %s", strerror(errno));
    }
    if(r == 0)
    {
        return 0;
    }
    n = read(sockFd, chunk, sizeof(chunk));
    if(n == 0)
    {
        return -1;
    }
    if(n < 0)
    {
        if((errno == EINTR) || (errno == EAGAIN))
        {
            return 0;
        }
        failRun(EXIT_RUN, "read: %s", strerror(errno));
    }
    for(k = 0; k < n; k++)
    {
        feed(chunk[k]);
    }
    fflush(stdout);
    if(logFp)
    {
        fflush(logFp);
    }
    return 1;
}

// Shows whatever comes until the server has been silent for ms.
static void
quiet(int ms)
{
    int r;

    do
    {
        r = pump(ms);
        if(r < 0)
        {
            failRun(EXIT_RUN, "the connection was closed");
        }
    } while(r > 0);
}

// Lets sec seconds go by, showing whatever the server says meanwhile.
static void
pauseFor(double sec)
{
    double until;
    double left;
    int ms;

    until = (nowSec() + sec);
    for(;;)
    {
        left = (until - nowSec());
        if(left <= 0)
        {
            return;
        }
        ms = ((left * 1000) < SLICE_MS) ? (int)(left * 1000) + 1 : SLICE_MS;
        if(pump(ms) < 0)
        {
            failRun(EXIT_RUN, "the connection was closed");
        }
    }
}

static void
sendAll(const char *p, size_t len)
{
    ssize_t n;

    while(len > 0)
    {
        n = write(sockFd, p, len);
        if(n < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            failRun(EXIT_RUN, "write: %s", strerror(errno));
        }
        p += n;
        len -= (size_t)n;
    }
}

// Types text a character at a time, a full ms apart however soon the echo comes,
// showing what the server says in between; with ms 0 it goes out whole.
static void
typeText(const char *p, int ms)
{
    size_t k;
    size_t len;

    len = strlen(p);
    if(ms <= 0)
    {
        sendAll(p, len);
        return;
    }
    for(k = 0; k < len; k++)
    {
        sendAll(p + k, 1);
        pauseFor((double)ms / 1000.0);
    }
}

// A line holding only *, the end of a reply. Nothing is before it but a newline.
static int
starSeen(void)
{
    if(bufLen == 0)
    {
        return 0;
    }
    if(strncmp(bufP, "*\n", 2) == 0)
    {
        return 1;
    }
    return (strstr(bufP, "\n*\n") != NULL);
}

// The offset just past a question back whose choices have all come, found at or
// after from, or 0 if there is none. The last choice is the numbered line that
// ends in a question mark.
static size_t
questionEnd(size_t from)
{
    char *qP;
    char *lineP;
    char *nlP;

    qP = strstr(bufP + from, QUESTION_TEXT);
    if(qP == NULL)
    {
        return 0;
    }
    lineP = qP + strlen(QUESTION_TEXT);
    while((nlP = strchr(lineP, '\n')) != NULL)
    {
        if((isdigit((unsigned char)*lineP)) && (nlP > lineP) && (nlP[-1] == '?'))
        {
            return (size_t)((nlP + 1) - bufP);
        }
        lineP = nlP + 1;
    }
    return 0;
}

// The steps.

// Waits for the step to end in its *, answering a question back when one
// comes. A step that ends in the stop is not waited for here.
static void
awaitStar(const Step *stP)
{
    double deadline;
    size_t qFrom;
    int used;
    int r;
    char answer[32];
    const char *aP;

    qFrom = 0;
    used = 0;
    deadline = (nowSec() + timeoutSec);
    for(;;)
    {
        if(strstr(bufP, STOP_TEXT) != NULL)
        {
            failRun(EXIT_USAGE, "the program stopped at a word it does not know (line %d of the script)",
                stP->lineNo);
        }
        if((used > 0) && (strstr(bufP + qFrom, REFUSED_TEXT) != NULL))
        {
            failRun(EXIT_USAGE, "the answer \"%s\" was refused", stP->answerP[used - 1]);
        }
        if(starSeen())
        {
            if(used < stP->nAnswers)
            {
                failRun(EXIT_USAGE, "the script gives an answer after line %d and the program asked no question",
                    stP->lineNo);
            }
            return;
        }
        if(questionEnd(qFrom) > 0)
        {
            if(used < stP->nAnswers)
            {
                aP = stP->answerP[used];
            }
            else if(defaultAnswer >= 0)
            {
                snprintf(answer, sizeof(answer), "%d", defaultAnswer);
                aP = answer;
            }
            else
            {
                failRun(EXIT_USAGE, "the program asks a question and the script has no answer (line %d; -a gives one)",
                    stP->lineNo);
            }
            qFrom = bufLen;
            used++;
            typeText(aP, charMs);
            sendAll("\r", 1);
            deadline = (nowSec() + timeoutSec);
            continue;
        }
        r = pump(SLICE_MS);
        if(r < 0)
        {
            failRun(EXIT_RUN, "the connection was closed before the reply ended");
        }
        if(nowSec() > deadline)
        {
            failRun(EXIT_RUN, "no closing * in %.0f s", timeoutSec);
        }
    }
}

// Types a line up to its dots and a space, waits for the program to stop at the
// word, then forgets the rest of the sentence with a filler word, Return and Ctrl-U.
// The program says nothing more, so there is no * to wait for.
static void
runStop(const Step *stP)
{
    double deadline;
    char ctrl;
    int ms;
    int r;

    ms = ((charMs > STOP_CHAR_MS) ? charMs : STOP_CHAR_MS);
    typeText(stP->textP, ms);
    typeText(" ", ms);
    deadline = (nowSec() + timeoutSec);
    while(strstr(bufP, STOP_TEXT) == NULL)
    {
        if(starSeen())
        {
            failRun(EXIT_USAGE, "line %d of the script: the sentence ended, no stop at the unknown word",
                stP->lineNo);
        }
        r = pump(SLICE_MS);
        if(r < 0)
        {
            failRun(EXIT_RUN, "the connection was closed before the stop");
        }
        if(nowSec() > deadline)
        {
            failRun(EXIT_RUN, "no stop in %.0f s (a word the program knows?)", timeoutSec);
        }
    }
    quiet(STOP_PAUSE_MS);
    typeText(STOP_FILLER, ms);
    quiet(STOP_PAUSE_MS);
    sendAll("\r", 1);
    quiet(STOP_RETURN_MS);
    ctrl = CHAR_CTRL_U;
    sendAll(&ctrl, 1);
    quiet(STOP_PAUSE_MS);
}

// One step, with the line typed and the reply waited for. Returns the step's seconds.
static double
runStep(const Step *stP)
{
    double start;
    char reply[8];
    char probe[LINE_CHARS + 2];

    if(stepByStep)
    {
        fprintf(stderr, "step %d of %d: press Return ", curStep, nSteps);
        if(fgets(reply, sizeof(reply), stdin) == NULL)
        {
            stepByStep = 0;
        }
    }
    bufLen = 0;
    if(bufP)
    {
        bufP[0] = 0;
    }
    start = nowSec();
    if(stP->stop)
    {
        runStop(stP);
    }
    else
    {
        typeText(stP->textP, charMs);
        sendAll("\r", 1);
        awaitStar(stP);
        // The last character is left out of the look: a closing quote typed after
        // the sentence's period is read as part of it and not echoed.
        strncpy(probe, stP->textP, (sizeof(probe) - 1));
        probe[sizeof(probe) - 1] = 0;
        if(strlen(probe) > 1)
        {
            probe[strlen(probe) - 1] = 0;
        }
        if(strstr(bufP, probe) == NULL)
        {
            fprintf(stderr, "shrdlu_play: step %d: the server did not echo the line\n", curStep);
        }
    }
    return (nowSec() - start);
}

// The script and the setup.

static char *
trim(char *p)
{
    size_t len;

    while(isspace((unsigned char)*p))
    {
        p++;
    }
    len = strlen(p);
    while((len > 0) && (isspace((unsigned char)p[len - 1])))
    {
        p[--len] = 0;
    }
    return p;
}

static Step *
newStep(void)
{
    if(nSteps == stepCap)
    {
        stepCap = ((stepCap == 0) ? 64 : (stepCap * 2));
        stepP = realloc(stepP, ((size_t)stepCap * sizeof(Step)));
        if(stepP == NULL)
        {
            failRun(EXIT_USAGE, "out of memory");
        }
    }
    memset(&stepP[nSteps], 0, sizeof(Step));
    return &stepP[nSteps++];
}

// Reads the script into steps. Ends the run with a message on a malformed line.
static void
loadScript(const char *pathP)
{
    FILE *fp;
    char raw[LINE_CHARS + 2];
    char *tP;
    size_t len;
    int lineNo;
    Step *stP;

    fp = fopen(pathP, "r");
    if(fp == NULL)
    {
        failRun(EXIT_USAGE, "%s: %s", pathP, strerror(errno));
    }
    lineNo = 0;
    while(fgets(raw, sizeof(raw), fp) != NULL)
    {
        lineNo++;
        len = strlen(raw);
        if((len > LINE_CHARS) || ((raw[len - 1] != '\n') && (!feof(fp))))
        {
            failRun(EXIT_USAGE, "%s line %d is too long", pathP, lineNo);
        }
        tP = trim(raw);
        if((*tP == 0) || (*tP == '#'))
        {
            continue;
        }
        if(*tP == '>')
        {
            tP = trim(tP + 1);
            if((nSteps == 0) || (*tP == 0) || (stepP[nSteps - 1].stop) ||
                (stepP[nSteps - 1].nAnswers == ANSWERS_MAX))
            {
                failRun(EXIT_USAGE, "%s line %d: an answer must follow a step that can ask", pathP, lineNo);
            }
            stP = &stepP[nSteps - 1];
            stP->answerP[stP->nAnswers++] = strdup(tP);
            continue;
        }
        stP = newStep();
        stP->lineNo = lineNo;
        len = strlen(tP);
        if((len >= 3) && (strcmp(tP + len - 3, "...") == 0))
        {
            tP[len - 3] = 0;
            tP = trim(tP);
            if(*tP == 0)
            {
                failRun(EXIT_USAGE, "%s line %d: nothing before the dots", pathP, lineNo);
            }
            stP->stop = 1;
        }
        stP->textP = strdup(tP);
    }
    fclose(fp);
    if(nSteps == 0)
    {
        failRun(EXIT_USAGE, "%s holds no steps", pathP);
    }
}

static void
connectTo(void)
{
    struct addrinfo hints;
    struct addrinfo *listP;
    struct addrinfo *aP;
    int rc;
    int on;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(hostP, portP, &hints, &listP);
    if(rc != 0)
    {
        failRun(EXIT_RUN, "%s: %s", hostP, gai_strerror(rc));
    }
    for(aP = listP; aP != NULL; aP = aP->ai_next)
    {
        sockFd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);
        if(sockFd < 0)
        {
            continue;
        }
        if(connect(sockFd, aP->ai_addr, aP->ai_addrlen) == 0)
        {
            break;
        }
        close(sockFd);
        sockFd = -1;
    }
    freeaddrinfo(listP);
    if(sockFd < 0)
    {
        failRun(EXIT_RUN, "cannot connect to %s port %s", hostP, portP);
    }
    // Characters go out as they are written, or a typed-look run would clump.
    on = 1;
    setsockopt(sockFd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

// The greeting, made of the telnet negotiation, the banner and its * line.
static void
awaitGreeting(void)
{
    double deadline;
    int r;

    deadline = (nowSec() + timeoutSec);
    while(!starSeen())
    {
        r = pump(SLICE_MS);
        if(r < 0)
        {
            failRun(EXIT_RUN, "the connection was closed before the banner");
        }
        if(nowSec() > deadline)
        {
            failRun(EXIT_RUN, "no banner in %.0f s", timeoutSec);
        }
    }
}

int
main(int argc, char **argv)
{
    int opt;
    int k;
    int last;
    int longest;
    double t;
    double total;
    double longestSec;
    char *endP;

    while((opt = getopt(argc, argv, "h:p:t:c:g:a:f:n:sl:q")) != -1)
    {
        switch(opt)
        {
        case 'h':
            hostP = optarg;
            break;
        case 'p':
            portP = optarg;
            break;
        case 't':
            timeoutSec = strtod(optarg, &endP);
            if((*endP != 0) || (timeoutSec <= 0))
            {
                usage();
            }
            break;
        case 'c':
            charMs = atoi(optarg);
            break;
        case 'g':
            gapSec = strtod(optarg, &endP);
            if((*endP != 0) || (gapSec < 0))
            {
                usage();
            }
            break;
        case 'a':
            defaultAnswer = atoi(optarg);
            break;
        case 'f':
            firstStep = atoi(optarg);
            break;
        case 'n':
            stepLimit = atoi(optarg);
            break;
        case 's':
            stepByStep = 1;
            break;
        case 'l':
            logFp = fopen(optarg, "w");
            if(logFp == NULL)
            {
                failRun(EXIT_USAGE, "%s: %s", optarg, strerror(errno));
            }
            break;
        case 'q':
            quietRun = 1;
            break;
        default:
            usage();
            break;
        }
    }
    if((argc - optind) != 1)
    {
        usage();
    }
    signal(SIGPIPE, SIG_IGN);
    loadScript(argv[optind]);
    if((firstStep < 1) || (firstStep > nSteps))
    {
        failRun(EXIT_USAGE, "-f %d: the script has %d steps", firstStep, nSteps);
    }
    last = nSteps;
    if((stepLimit > 0) && ((firstStep - 1 + stepLimit) < last))
    {
        last = (firstStep - 1 + stepLimit);
    }
    connectTo();
    awaitGreeting();

    total = 0;
    longest = 0;
    longestSec = 0;
    for(k = firstStep; k <= last; k++)
    {
        curStep = k;
        t = runStep(&stepP[k - 1]);
        total += t;
        if(t > longestSec)
        {
            longestSec = t;
            longest = k;
        }
        if(!quietRun)
        {
            fprintf(stderr, "[step %d of %d: %.1f s]\n", k, nSteps, t);
        }
        if(gapSec > 0)
        {
            pauseFor(gapSec);
        }
    }
    curStep = 0;
    if(!quietRun)
    {
        fprintf(stderr, "[%d steps in %.1f s; the longest, step %d, %.1f s]\n",
            ((last - firstStep) + 1), total, longest, longestSec);
    }
    close(sockFd);
    if(logFp)
    {
        fclose(logFp);
    }
    return 0;
}
