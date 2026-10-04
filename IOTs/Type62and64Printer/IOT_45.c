/*
 * This is an implementation of line printers for the pdp-1, the Type 62 abd Type 64 printers.
 *
 * 21-Jun-2026 wje minor revision for some timing changes
 * 3-Jul-2026 wje minor cleanup, also avoid extraneous IO completions
 *
 * The output file is opened and written by a writer thread, never the emulator thread, through a
 * non-blocking fd, so the path a program names can be a FIFO or a device, and a slow card or disk
 * holds nothing (5 s writes were measured on a Pi 4 with a busy SD card). The printed text waits
 * in a ring for the writer. With no FIFO reader yet, or one that isn't keeping up, the printer
 * stays busy (its completion is held back) until the output is taken, as a printer out of paper
 * would. A regular file does not hold the completion: the writer takes the text however long the
 * disk takes. A close or a new file name (lpf, lpm, a Type 64 reset) is a mark in the stream,
 * applied by the writer once the text before it is written.
 *
 * 27-Sep-2026 Claude the output file is a non-blocking fd; a FIFO no longer hangs the emulator.
 * 27-Sep-2026 Claude the file is opened and written on a writer thread.
 * 28-Sep-2026 Claude the print and spacing delays are simtime deadlines, not counts of executed cycles.
 * 04-Oct-2026 Claude power clear empties the print buffer and resets the shift, the mode and the wait.
*/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "configuration.h"
#include "iotHandler.h"
#include "flexlib.h"

//#define DOLOGGING
#include "iotLogger.h"

#define LOG45 0
#define LOG45ASCII 0
#define LOG45FLEX 0
#define LOG45FILE 0
#define LOG45PRINT 0
#define LOG45CONFIG 0
#define LOG45FF 0

#define DEFAULTFILE "/tmp/pdp1lpt.txt"
#define BUFSIZE 120     // 120 column printer

// Some versions of the Type 62 could do 600 lpm.
#define TYPE62LINEDELAY 16  // milliseconds per
#define TYPE62PRINTDELAY 84  // milliseconds per

#define TYPE64LINEDELAY 32 // milliseconds per
#define TYPE64PRINTDELAY 168 // milliseconds per

// Buffer clear/reset (lpc).
// No documented value could be confirmed, so this is derived from the printer's rated 300 lines/minute.
#define TYPE64CLEARDELAY 5 // milliseconds

#define ERROR 0777776   // -1 in 1's cmpl 12 bit

#define PENDMAX 16384           // printed text waiting for the writer
#define RETRYDELAY TYPE62LINEDELAY  // ms between tries while the output is not taken
#define MAXMARKS 8              // closes and file name changes waiting in the stream

// What the writer last found: outState
#define OPEN_OK 0
#define OPEN_WAIT 1             // a FIFO with no reader yet, or one not keeping up: not ready
#define OPEN_FAIL 2             // the file could not be opened

// Define the actions that can be done, bits that are or'd
#define PRINT   0x1     // print the current buffer, reset buffer counter to 0
#define SPACE   0x2     // do line spacing
#define ADD     0x4     // add chars to buffer
#define OVER    0x10    // set buffer counter to 0
#define RESET   0x40    // reset everything
#define LPM     0x400   // lpm, same for both 62 and 64
#define LPF     0x1000  // lpf, same for both 62 and 64

// These are the defaults, entries 1-6 overridden by any config file setting
static int spacing64[] = {
    0,          // overstrike
    1,
    2,
    3,
    6,
    11,
    22,
    -1          // marker for formfeed
    };

static int spacing62[] = {
    1,
    2,
    3,
    4,
    11,
    22,
    33,
    -1          // marker for formfeed
    };

static int curShift = LCS;          // used for the flex shift char processing
static int bufLoc;                  // location to place next character in buffer
static int lineNo;                  // number of lines done
static int linesPerPage = 66;       // override in config
static char buffer[BUFSIZE + 1];    // the print buffer

static char *filenameP = DEFAULTFILE;

// Shared with the writer thread, under outLock. The ring holds bytes appended to written:
// written counts bytes the writer wrote, or dropped.
typedef struct
{
    uint64_t pos;                   // close the file once the text before pos is written
    char *nameP;                    // then use this name, if not NULL (the writer frees it)
} OutMark;

static pthread_mutex_t outLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workCond = PTHREAD_COND_INITIALIZER;     // text, a mark, or an open wanted
static pthread_cond_t roomCond = PTHREAD_COND_INITIALIZER;     // the writer took something
static pthread_t writerThread;
static bool writerStarted;
static bool writerStop;             // the process is exiting
static bool writerDone;             // the writer has ended: output is dropped
static char ring[PENDMAX];
static uint64_t appended;
static uint64_t written;
static OutMark marks[MAXMARKS];
static int nMarks;
static bool openWanted;             // an IOT ran: open the file if it isn't open
static bool outOpen;                // the writer has the file open
static int outState = OPEN_OK;

// The writer thread's own.
static int outFd = -1;
static char *outNameP;              // NULL for DEFAULTFILE

static bool configDone;             // config loaded
static bool type64;                 // emulating a type 64, else a type 62
static bool asciiMode;
static bool noFF;

static bool inWait;                 // completion delay in effect
static bool wantCompletion;         // did the instruction that armed the current delay actually request IOCOMPLETE?

static void configure(void);
static void startWriter(void);
static void emitOutput(const char *sP, int len);
static void closeOutput(const char *newNameP);
static void wantOpen(void);
static bool outputFailed(void);
static bool outputBlocked(void);
static void *writer(void *argP);
static void waitRetry(void);
static void stopWriter(void) __attribute__((destructor));

extern int flexToAscii(char fc, int *shiftP);
extern char *getFileName(PDP1P pdp1P, unsigned int addr, char *bufP, size_t bufLen);

// Starts the writer thread, once. If it cannot be started the printer's output is dropped, as
// for a file that cannot be opened. No return value.
static void
startWriter(void)
{
    if( writerStarted )
    {
        return;
    }

    writerStarted = true;
    if( pthread_create(&writerThread, NULL, writer, NULL) != 0 )
    {
        writerStarted = false;
        writerDone = true;
        outState = OPEN_FAIL;
    }
}

// Adds len bytes of sP to the text waiting for the writer. A full ring waits for the writer if
// it is writing the file, however slowly, so nothing is lost; if the output is not being taken
// (a FIFO with no reader, a file that cannot be opened) what does not fit is dropped and
// logged, since a program that waits for the printer's completion never gets that far.
static void
emitOutput(const char *sP, int len)
{
int room;
int at;
int n;

    pthread_mutex_lock(&outLock);
    while( len > 0 )
    {
        room = (PENDMAX - (int)(appended - written));
        if( room == 0 )
        {
            if( writerDone || (outState != OPEN_OK) )
            {
                iotCondLog(LOG45FILE, "Output not taken, %d bytes dropped\n", len);
                break;
            }

            pthread_cond_wait(&roomCond, &outLock);
            continue;
        }

        at = (int)(appended % PENDMAX);
        n = len;
        if( n > room )
        {
            n = room;
        }
        if( n > (PENDMAX - at) )
        {
            n = (PENDMAX - at);
        }

        memcpy(ring + at, sP, n);
        appended += n;
        sP += n;
        len -= n;
    }

    pthread_cond_signal(&workCond);
    pthread_mutex_unlock(&outLock);
}

// Closes the output file once the text printed so far is written, and from then on uses
// newNameP, if not NULL. Text a FIFO reader has not taken by then is dropped. Waits only if
// MAXMARKS closes are already waiting. No return value.
static void
closeOutput(const char *newNameP)
{
    pthread_mutex_lock(&outLock);
    while( (nMarks >= MAXMARKS) && !writerDone )
    {
        pthread_cond_wait(&roomCond, &outLock);
    }

    if( !writerDone )
    {
        marks[nMarks].pos = appended;
        marks[nMarks].nameP = (newNameP ? strdup(newNameP) : NULL);
        ++nMarks;
        pthread_cond_signal(&workCond);
    }

    pthread_mutex_unlock(&outLock);
}

// Every IOT asks the writer to open the file if it isn't open, as every IOT used to open it;
// so a file that could not be opened is tried again. No return value.
static void
wantOpen(void)
{
    pthread_mutex_lock(&outLock);
    if( !outOpen )
    {
        openWanted = true;
        pthread_cond_signal(&workCond);
    }
    pthread_mutex_unlock(&outLock);
}

// Returns true if the writer's last try at opening the file failed.
static bool
outputFailed(void)
{
bool failed;

    pthread_mutex_lock(&outLock);
    failed = (outState == OPEN_FAIL);
    pthread_mutex_unlock(&outLock);
    return(failed);
}

// Returns true if printed text is waiting and is not being taken: the file is not open yet, or
// it is a FIFO with no reader, or one not keeping up. The printer is not ready until it is.
static bool
outputBlocked(void)
{
bool blocked;

    pthread_mutex_lock(&outLock);
    blocked = (appended > written) && !writerDone && (!outOpen || (outState == OPEN_WAIT));
    pthread_mutex_unlock(&outLock);
    return(blocked);
}

// The writer thread: opens the file, writes the text in order and applies the marks, all
// with outLock released, so the emulator thread never waits on the file. A FIFO with no reader
// (ENXIO) or a full one (EAGAIN) is tried again every RETRYDELAY. A file that cannot be opened
// drops the text waiting for it. Any other write error drops the text and closes the file, so
// the next text opens it again. At exit it writes what it can and ends.
static void *
writer(void *argP)
{
uint64_t limit;
int at;
int n;
int fd;
int err;
ssize_t put;
char *nameP;

    (void)argP;
    pthread_mutex_lock(&outLock);
    for( ;; )
    {
        limit = (nMarks ? marks[0].pos : appended);

        if( writerStop && (outState != OPEN_OK) )
        {
            written = appended;             // exiting, and the output is not being taken
            limit = appended;
        }

        // A close drops what the output is not taking; then it is applied.
        if( nMarks && (written < marks[0].pos) && (outState != OPEN_OK) )
        {
            iotCondLog(LOG45FILE, "Closed with %d bytes unwritten\n", (int)(marks[0].pos - written));
            written = marks[0].pos;
            pthread_cond_broadcast(&roomCond);
        }

        if( nMarks && (written >= marks[0].pos) )
        {
            fd = outFd;
            outFd = -1;
            outOpen = false;
            outState = OPEN_OK;
            if( marks[0].nameP )
            {
                free(outNameP);
                outNameP = marks[0].nameP;
            }

            --nMarks;
            memmove(&marks[0], &marks[1], (nMarks * sizeof(OutMark)));
            pthread_cond_broadcast(&roomCond);

            if( fd >= 0 )
            {
                pthread_mutex_unlock(&outLock);
                close(fd);
                pthread_mutex_lock(&outLock);
            }
            continue;
        }

        if( (outFd < 0) && ((written < limit) || openWanted) && !writerStop )
        {
            openWanted = false;
            nameP = (outNameP ? outNameP : DEFAULTFILE);
            pthread_mutex_unlock(&outLock);
            fd = open(nameP, (O_WRONLY | O_APPEND | O_CREAT | O_NONBLOCK), 0666);
            err = errno;
            pthread_mutex_lock(&outLock);

            if( fd >= 0 )
            {
                outFd = fd;
                outOpen = true;
                outState = OPEN_OK;
            }
            else if( err == ENXIO )
            {
                outState = OPEN_WAIT;
                if( written < limit )
                {
                    waitRetry();
                }
            }
            else
            {
                iotCondLog(LOG45FILE, "Open file '%s' failed\n", nameP);
                outState = OPEN_FAIL;
                written = limit;
                pthread_cond_broadcast(&roomCond);
            }
            continue;
        }

        if( (outFd >= 0) && (written < limit) )
        {
            at = (int)(written % PENDMAX);
            n = (int)(limit - written);
            if( n > (PENDMAX - at) )
            {
                n = (PENDMAX - at);
            }

            // The bytes from written to limit are not touched by emitOutput() until taken.
            pthread_mutex_unlock(&outLock);
            put = write(outFd, ring + at, n);
            err = errno;
            pthread_mutex_lock(&outLock);

            if( put > 0 )
            {
                written += (uint64_t)put;
                outState = OPEN_OK;
                pthread_cond_broadcast(&roomCond);
            }
            else if( (put < 0) && (err == EINTR) )
            {
                ;
            }
            else if( (put < 0) && ((err == EAGAIN) || (err == EWOULDBLOCK)) )
            {
                outState = OPEN_WAIT;
                waitRetry();
            }
            else
            {
                iotCondLog(LOG45FILE, "Write failed, errno %d, %d bytes dropped\n", err, (int)(limit - written));
                written = limit;
                fd = outFd;
                outFd = -1;
                outOpen = false;
                outState = OPEN_OK;
                pthread_cond_broadcast(&roomCond);
                pthread_mutex_unlock(&outLock);
                close(fd);
                pthread_mutex_lock(&outLock);
            }
            continue;
        }

        if( writerStop )
        {
            break;                          // everything is written or dropped
        }

        pthread_cond_wait(&workCond, &outLock);
    }

    writerDone = true;
    outState = OPEN_FAIL;
    pthread_cond_broadcast(&roomCond);
    pthread_mutex_unlock(&outLock);

    if( outFd >= 0 )
    {
        close(outFd);
        outFd = -1;
    }

    return(NULL);
}

// Waits RETRYDELAY, or less if something new comes. Called with outLock held. No return value.
static void
waitRetry(void)
{
struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (RETRYDELAY * 1000000L);
    if( until.tv_nsec >= 1000000000L )
    {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
    }

    pthread_cond_timedwait(&workCond, &outLock, &until);
}

// At exit: the writer writes what it can and ends. No return value.
static void
stopWriter(void)
{
    if( !writerStarted )
    {
        return;
    }

    pthread_mutex_lock(&outLock);
    writerStop = true;
    pthread_cond_signal(&workCond);
    pthread_mutex_unlock(&outLock);
    pthread_join(writerThread, NULL);
}

int
iotHandler(PDP1 *pdp1P, int dev, int pulse, int completion)
{
int i, j;
int word, addr;
int actions;
int spaceval;
uint64_t delaytime;
int fchar, achar;
bool fail;
bool noWait;

    if( pulse )
    {
        return(1);
    }

    iotCondLog(LOG45, "In lpt iot mb %o dev %o cmpl %d\n", MB(pdp1P), dev, completion);
    inWait = noWait = fail = false;

    if( !configDone )
    {
        configure();
        startWriter();
        configDone = true;
    }

    // Figure out the type 62 vs 64 diffs.
    // 62:
    // 0045, print no advance
    // 1045, add chars
    // 2x45, space
    // 64:
    // 0045, add chars
    // 1x45, print and space
    // 2045, reset
    actions = 0;
    delaytime = 0;  // if 0, no delay, else ns on the device time base

    if( (MB(pdp1P) & 03700) == 0 )             // 0045
    {
        actions = (type64)?ADD:OVER;
    }
    else if( (MB(pdp1P) & 03000) == 01000 )    // 1x45
    {
        if( type64 )
        {
            spaceval = (MB(pdp1P) >> 6) & 07;
            if( spaceval )
            {
                actions = PRINT|SPACE;
            }
            else
            {
                // x=0 is overstrike.
                // Like the Type 62 prl, defer the actual write to the
                // output file until a real line advance happens rather than printing and
                // clearing the pre-overstrike buffer immediately.
                actions = OVER;
            }
        }
        else
        {
            actions = ADD;
        }
    }
    else if( (MB(pdp1P) & 03000) == 02000 )    // 2x45
    {
        if( type64 )
        {
            actions = RESET;
        }
        else
        {
            actions = PRINT|SPACE;
            spaceval = (MB(pdp1P) >> 6) & 07;
        }
    }
    else if( (MB(pdp1P) & 03700) == 03000 )    // 3045
    {
        actions = LPF;
    }
    else if( (MB(pdp1P) & 03700) == 03100 )    // 3145
    {
        actions = LPM;
    }

    // Now do the processing.
    if( actions & RESET )
    {
        lineNo = 1;
        bufLoc = 0;
        memset(buffer, 0, sizeof(buffer));
        curShift = LCS;
        asciiMode = false;                      // also resets to the default flexo mode
        delaytime = 0;

        iotCondLog(LOG45FILE, "Closing file\n");
        closeOutput(NULL);

        delaytime = MSTONS(TYPE64CLEARDELAY);
        inWait = true;
    }

    // The writer opens the file. A FIFO with no reader yet is not a failure: the output waits
    // and the printer stays busy. A file the writer could not open fails this IOT, and this
    // IOT's request has it tried again.
    wantOpen();
    if( outputFailed() )
    {
        fail = true;                          // sorry
        iotCondLog(LOG45FILE, "Open file '%s' failed\n", filenameP);
    }

    if( !fail && (actions & ADD) )                     // put chars in buffer
    {
        word = IO(pdp1P);

        if( asciiMode )
        {
            for( i = 0; i < 2; ++i )
            {
                if( bufLoc >= BUFSIZE )
                {
                    break;                          // no more room
                }

                achar = (word & 0377000) >> 9;
                word <<= 9;
                iotCondLog(LOG45ASCII, "Ascii mode achar 0x%x\n", achar);
                if( !achar )
                {
                    continue;
                }

                buffer[bufLoc++] = achar;
            }
        }
        else
        {
            for( i = 0; i < 3; ++i )
            {
                if( bufLoc >= BUFSIZE )
                {
                    break;                          // no more room
                }

                fchar = (word & 0770000) >> 12;
                word <<= 6;
                if( (achar = flexToAscii(fchar, &curShift)) == NONE )
                {
                    continue;
                }

                iotCondLog(LOG45FLEX, "Flex mode achar 0x%x\n", achar);
                buffer[bufLoc++] = achar;
            }
        }

        noWait = true;                         // immediate
    }

    // This needs to be separate because the 62 and 64 handle overstrikes differently.
    // This is the Type 62 prl path (bare OVER) and the Type 64 pas-with-x=0 path (also bare
    // OVER, see above).
    // Neither actually prints anything here, but the real printer still ran
    // its print cycle (just without advancing the paper), so this needs the same completion
    // delay as an actual print.
    if( actions & OVER )
    {
        bufLoc = 0;
        delaytime += (type64)?MSTONS(TYPE64PRINTDELAY):MSTONS(TYPE62PRINTDELAY);
        inWait = true;
    }

    // do before SPACE
    if( !fail && (actions & PRINT) )
    {
        // buffer will always be null terminated, just print it if not empty
        if( *buffer )
        {
            emitOutput(buffer, strlen(buffer));
            iotCondLog(LOG45PRINT, "Printed '%s'\n", buffer);
        }

        bufLoc = 0;
        curShift = LCS;
        memset(buffer, 0, sizeof(buffer));
    }

    if( !fail && (actions & SPACE) )
    {
        // what to do for spacing
        i = (type64)?spacing64[(MB(pdp1P) >> 6) & 07]:spacing62[(MB(pdp1P) >> 6) & 07];
        iotCondLog(LOG45FF, "SPACE, spacing %d\n", i);

        if( i == -1 )      // form feed
        {
            // The delay time is not certain, assume same as one line advance time per remaining lines
            j = linesPerPage - lineNo;
            iotCondLog(LOG45FF, "FF, lpp %d, lineNo %d\n", linesPerPage, lineNo);

            if( noFF )
            {
                iotCondLog(LOG45FF, "FF with noFF\n");
                // We go one more to termiate the current line
                for( i = 0; i <= j; ++i )
                {
                    emitOutput("\n", 1);
                }
            }
            else
            {
                iotCondLog(LOG45FF, "FF using formfeed\n");
                emitOutput("\n\f", 2);
            }

            lineNo = 1;
            // j is negative only for an lptLines below 1; MSTONS() of a negative count would wrap.
            if( j > 0 )
            {
                delaytime += (type64)?MSTONS(TYPE64LINEDELAY * j):MSTONS(TYPE62LINEDELAY * j);
            }
            iotCondLog(LOG45FF, "FF delay time %llu ns\n", (unsigned long long)delaytime);
        }
        else
        {
            while( i-- > 0 )
            {
                ++lineNo;
                if( lineNo >= linesPerPage )
                {
                    lineNo = 1;
                }

                emitOutput("\n", 1);
                delaytime += (type64)?MSTONS(TYPE64LINEDELAY):MSTONS(TYPE62LINEDELAY);
            }
        }

        // reset the buffer and shift state
        bufLoc = 0;
        curShift = LCS;
        memset(buffer, 0, sizeof(buffer));

        // iotDeadline() holds the completion while the output is not being taken.
        inWait = true;
    }

    // Do even if there was a fail
    if( actions == LPF )
    {
        if( IO(pdp1P) == 0 )                // just reset the file
        {
            closeOutput(DEFAULTFILE);
            if( filenameP != DEFAULTFILE )
            {
                free(filenameP);
            }

            filenameP = DEFAULTFILE;
            iotCondLog(LOG45FILE, "File reset to '%s'\n",filenameP);
        }
        else
        {
            // first unpack the file name, we'll use the lp buffer
            addr = IO(pdp1P) & (MAXMEM - 1);
            iotCondLog(LOG45FILE, "Open  file, io %06o, addr %06o\n", IO(pdp1P), addr);
            if( !getFileName(pdp1P, addr, buffer, sizeof(buffer)) )
            {
                closeOutput(NULL);
                fail = true;
            }
            else
            {
                closeOutput(buffer);
                if( filenameP != DEFAULTFILE )
                {
                    free(filenameP);
                }

                filenameP = (char *)malloc(strlen(buffer) + 1);
                strcpy(filenameP, buffer);
                iotCondLog(LOG45FILE, "Open  file, filename '%s'\n", filenameP);

                // and reset
                bufLoc = 0;
                memset(buffer, 0, sizeof(buffer));
                curShift = LCS;
                iotPollCancel();                    // just in case
                if( inWait )
                {
                    // If there is a pending completion request, post it.
                    inWait = false;
                    if( wantCompletion )
                    {
                        wantCompletion = false;
                        IOCOMPLETE(pdp1P);
                    }
                }
            }
        }

        noWait = true;                         // immediate
    }

    // Do even if there was a fail
    if( actions  == LPM )                      // change character mode, close file
    {
        asciiMode = IO(pdp1P) & 1;
        iotCondLog(LOG45FILE, "File mode %d\n", asciiMode);

        // This does almost what the Type 64 reset command does, the Type 62 doesn't have a reset,
        // and that's how we close the output file.
        // It does not change ascii mode though, the above bit does that.
        if( IO(pdp1P) & 2 )
        {
            closeOutput(NULL);
            iotCondLog(LOG45FILE, "File closed\n");

            lineNo = 1;
            bufLoc = 0;
            memset(buffer, 0, sizeof(buffer));
            curShift = LCS;
        }

        noWait = true;
    }

    if( !fail && delaytime && !noWait )
    {
        wantCompletion = completion;
        iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + delaytime));
    }

    if( noWait && completion )
    {
        IOCOMPLETE(pdp1P);                  // this IOT never waits
    }

    if( fail )
    {
        // The writer has no file open after a failed open, and lpf closed it already.
        iotCondLog(LOG45, "Fail\n");
        IO(pdp1P) = ERROR;
    }
    else
    {
        IO(pdp1P) = 0;
    }

    return(1);
}

// Our 'interrupt' handler: the print or spacing delay is over. If the output is not being taken
// (a FIFO with no reader, or a file not open yet), the printer stays busy and this looks again
// after RETRYDELAY. The disk's own speed never holds the completion. The delays are simtime
// deadlines on the device time base, so cycles the drum or the 340 steal count, and a halt does
// not stop them.
void
iotDeadline(PDP1 *pdp1P)
{
    if( outputBlocked() )
    {
        iotPollAt(IOT_TIME_DEVICE, (iotTime(IOT_TIME_DEVICE) + MSTONS(RETRYDELAY)));
        return;
    }

    inWait = false;

    // Post complete if one is pending.
    if( wantCompletion )
    {
        wantCompletion = false;
        IOCOMPLETE(pdp1P);
    }
}

// Called once when the power switch goes off. Power clear resets the printer's control: the
// characters loaded and not printed are dropped, the shift goes back to lower case, the mode to
// flexo, and a print or spacing under way is forgotten (the core has disarmed its deadline), its
// completion with it. The paper does not move, so the line count stays. The output file and its
// name are the host's, and are kept.
// No return value.
void
iotPowerClear(void)
{
    bufLoc = 0;
    memset(buffer, 0, sizeof(buffer));
    curShift = LCS;
    asciiMode = false;
    inWait = false;
    wantCompletion = false;
}

void
configure()
{
int i, ival;
char *cP;
ConfigurationSettingP settingP;

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptType64")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lptType64 %d\n", settingP->onOff);
        type64 = settingP->onOff;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptLineSpacing")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lptLineSpacing %s\n", settingP->strvalueP);
        // pick up no more than 8 values
        for( cP = settingP->strvalueP, i = 0; cP && *cP && (i < 8); ++i)
        {
            if( (ival = atoi(cP)) )
            {
                iotCondLog(LOG45CONFIG, "In lpt, spacing %d is %d\n", i, ival);
                spacing62[i] = spacing64[i] = ival;
            }

            if( (cP = strchr(cP, ',')) )
            {
                ++cP;
            }
        }
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptLines")) )
    {
        iotCondLog(LOG45CONFIG, "In lpt, lines per page %d\n", settingP->ivalue);
        linesPerPage = settingP->ivalue;
    }

    if( (settingP = findConfigurationSetting(getConfiguration(), "lptNoFF")) )
    {
        noFF = settingP->onOff;
        iotCondLog(LOG45CONFIG, "In lpt, noFF %d\n", noFF);
    }
}
